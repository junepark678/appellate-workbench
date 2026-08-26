#include "local_session_provider.hpp"

#include "resolved_session_pins.hpp"

#include "appellate/packs/runtime_pack.hpp"
#include "appellate/storage/asset_store.hpp"
#include "appellate/storage/detail/private_state.hpp"
#include "appellate/storage/session_archive.hpp"

#include <QByteArray>
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(Q_OS_UNIX)
#include <unistd.h>
#endif

namespace appellate::ui {
namespace {

constexpr auto workflow_session_domain = "appellate-workbench-local-workflow-session-v1";
constexpr auto oral_session_domain = "appellate-workbench-local-oral-session-v1";
constexpr auto legal_state_domain = "appellate-workbench-workflow-legal-state-v2";
constexpr auto workflow_engine_revision = "engine.workflow.local.v1";
constexpr auto oral_engine_revision = "engine.oral.local.v1";

enum class ArchivedSessionFamily { Workflow, OralArgument };

struct ClosureCandidate final {
    const packs::ResolvedPack* resolved_pack{};
    std::vector<storage::RevisionPin> pins;
    std::optional<packs::RuntimePack> runtime;
};

struct ValidatedWorkflowArchiveSession final {
    const ClosureCandidate* closure{};
    const packs::RuntimeCase* runtime_case{};
    QString legal_state_digest;
};

[[nodiscard]] auto archiveProviderFail(SessionArchiveProviderErrorCode code, QString message)
    -> std::unexpected<SessionArchiveProviderError> {
    return std::unexpected(SessionArchiveProviderError{code, std::move(message), std::nullopt});
}

[[nodiscard]] auto archiveStorageFail(QStringView operation,
                                      const storage::SessionArchiveError& error)
    -> std::unexpected<SessionArchiveProviderError> {
    return std::unexpected(SessionArchiveProviderError{
        SessionArchiveProviderErrorCode::ArchiveFailure,
        QStringLiteral("%1: %2").arg(operation, error.message), error.code});
}

[[nodiscard]] auto archivedSessionFamily(const storage::SessionSnapshot& snapshot)
    -> std::expected<ArchivedSessionFamily, SessionArchiveProviderError> {
    if (snapshot.session_id.startsWith(QLatin1StringView("workflow.session."))) {
        if (snapshot.engine_revision != QLatin1StringView(workflow_engine_revision)) {
            return archiveProviderFail(
                SessionArchiveProviderErrorCode::UnsupportedSession,
                QStringLiteral("Workflow session %1 uses unsupported engine %2")
                    .arg(snapshot.session_id, snapshot.engine_revision));
        }
        return ArchivedSessionFamily::Workflow;
    }
    if (snapshot.session_id.startsWith(QLatin1StringView("oral.argument.session."))) {
        if (snapshot.engine_revision != QLatin1StringView(oral_engine_revision)) {
            return archiveProviderFail(
                SessionArchiveProviderErrorCode::UnsupportedSession,
                QStringLiteral("Oral-argument session %1 uses unsupported engine %2")
                    .arg(snapshot.session_id, snapshot.engine_revision));
        }
        return ArchivedSessionFamily::OralArgument;
    }
    return archiveProviderFail(
        SessionArchiveProviderErrorCode::UnsupportedSession,
        QStringLiteral("Archive session %1 is not a supported Workflow/Oral session")
            .arg(snapshot.session_id));
}

[[nodiscard]] auto validateReplayManifest(const storage::SessionArchiveReplayContents& contents)
    -> std::expected<void, SessionArchiveProviderError> {
    if (contents.manifest.sessions.size() != contents.sessions.size() ||
        contents.manifest.asset_digests.size() != static_cast<qsizetype>(contents.assets.size())) {
        return archiveProviderFail(
            SessionArchiveProviderErrorCode::InvalidArgument,
            QStringLiteral("Archive replay contents do not match their manifest"));
    }
    for (std::size_t index = 0; index < contents.sessions.size(); ++index) {
        const auto& declared = contents.manifest.sessions[index];
        const auto& snapshot = contents.sessions[index];
        if (declared.session_id != snapshot.session_id ||
            declared.engine_revision != snapshot.engine_revision ||
            declared.authority_contract != snapshot.authority_contract ||
            declared.sequence != snapshot.sequence ||
            declared.created_at_utc != snapshot.created_at_utc || declared.pins != snapshot.pins ||
            declared.command_count != snapshot.commands.size() ||
            declared.event_count != snapshot.events.size() ||
            declared.docket_count != snapshot.docket.size() ||
            declared.asset_reference_count != snapshot.asset_references.size()) {
            return archiveProviderFail(
                SessionArchiveProviderErrorCode::InvalidArgument,
                QStringLiteral("Archive replay session %1 differs from its manifest")
                    .arg(snapshot.session_id));
        }
    }
    qint64 total_asset_bytes = 0;
    for (qsizetype index = 0; index < contents.manifest.asset_digests.size(); ++index) {
        const auto& asset = contents.assets[static_cast<std::size_t>(index)];
        if (contents.manifest.asset_digests[index] != asset.digest ||
            asset.bytes.size() > std::numeric_limits<qint64>::max() - total_asset_bytes) {
            return archiveProviderFail(
                SessionArchiveProviderErrorCode::InvalidArgument,
                QStringLiteral("Archive replay assets differ from their manifest"));
        }
        total_asset_bytes += asset.bytes.size();
    }
    if (total_asset_bytes != contents.manifest.total_asset_bytes) {
        return archiveProviderFail(
            SessionArchiveProviderErrorCode::InvalidArgument,
            QStringLiteral("Archive replay asset size differs from its manifest"));
    }
    return {};
}

[[nodiscard]] auto matchingClosure(std::vector<ClosureCandidate>& candidates,
                                   const std::vector<storage::RevisionPin>& required_pins)
    -> std::expected<ClosureCandidate*, SessionArchiveProviderError> {
    const auto found = std::ranges::find(candidates, required_pins, &ClosureCandidate::pins);
    if (found == candidates.end()) {
        return archiveProviderFail(
            SessionArchiveProviderErrorCode::IncompatibleRevisionPins,
            QStringLiteral("An archived session's entire ordered revision-pin vector does not "
                           "equal any available resolved-pack closure"));
    }
    return &*found;
}

[[nodiscard]] auto runtimeFor(ClosureCandidate& candidate)
    -> std::expected<const packs::RuntimePack*, SessionArchiveProviderError> {
    if (!candidate.runtime.has_value()) {
        auto runtime = packs::loadRuntimePack(*candidate.resolved_pack);
        if (!runtime) {
            return archiveProviderFail(
                SessionArchiveProviderErrorCode::IncompatibleRevisionPins,
                QStringLiteral("An exact archived closure has no valid runtime projection: %1")
                    .arg(QString::fromStdString(runtime.error().message)));
        }
        candidate.runtime = std::move(*runtime);
    }
    return &*candidate.runtime;
}

void appendUint64(QByteArray& output, std::uint64_t value) {
    std::array<char, 8> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto shift = static_cast<unsigned>((bytes.size() - index - 1U) * 8U);
        bytes.at(index) = static_cast<char>((value >> shift) & 0xffU);
    }
    output.append(bytes.data(), static_cast<qsizetype>(bytes.size()));
}

void appendFrame(QByteArray& output, QByteArrayView value) {
    appendUint64(output, static_cast<std::uint64_t>(value.size()));
    output.append(value.data(), value.size());
}

void appendFrame(QByteArray& output, const QString& value) {
    const auto utf8 = value.toUtf8();
    appendFrame(output, QByteArrayView(utf8));
}

void appendFrame(QByteArray& output, std::string_view value) {
    appendUint64(output, value.size());
    output.append(value.data(), static_cast<qsizetype>(value.size()));
}

void appendFrame(QByteArray& output, const std::string& value) {
    appendFrame(output, std::string_view(value));
}

void appendClosure(QByteArray& output, const packs::ResolvedPack& resolved_pack) {
    const auto revisions = resolved_pack.revisionsByPackId();
    appendUint64(output, revisions.size());
    for (const auto& revision : revisions) {
        appendFrame(output, revision.id.value);
        appendFrame(output, revision.version);
        appendFrame(output, revision.digest);
    }
}

[[nodiscard]] QString digestWithPrefix(QStringView prefix, const QByteArray& framed) {
    return prefix.toString() +
           QString::fromLatin1(
               QCryptographicHash::hash(framed, QCryptographicHash::Sha256).toHex());
}

[[nodiscard]] QString workflowSessionId(const packs::ResolvedPack& resolved_pack,
                                        const packs::RuntimeCase& runtime_case) {
    QByteArray framed;
    appendFrame(framed, std::string_view(workflow_session_domain));
    appendClosure(framed, resolved_pack);
    appendFrame(framed, runtime_case.definition.id.value);
    appendFrame(framed, runtime_case.workflow.id.value);
    return digestWithPrefix(u"workflow.session.", framed);
}

[[nodiscard]] QString oralSessionId(const packs::ResolvedPack& resolved_pack,
                                    const model::CaseId& case_id,
                                    const packs::RuntimeArgumentConfigId& argument_configuration_id,
                                    const QString& legal_state_digest) {
    QByteArray framed;
    appendFrame(framed, std::string_view(oral_session_domain));
    appendClosure(framed, resolved_pack);
    appendFrame(framed, case_id.value);
    appendFrame(framed, argument_configuration_id.value);
    appendFrame(framed, legal_state_digest);
    return digestWithPrefix(u"oral.argument.session.", framed);
}

[[nodiscard]] auto runtimeCase(const packs::ResolvedPack& resolved_pack,
                               const model::CaseId& case_id)
    -> std::expected<packs::RuntimeCase, QString> {
    const auto runtime = packs::loadRuntimePack(resolved_pack);
    if (!runtime) {
        return std::unexpected(
            QStringLiteral("Exact installed closure has no valid runtime projection: %1")
                .arg(QString::fromStdString(runtime.error().message)));
    }
    const auto found = std::ranges::find_if(runtime->cases, [&](const auto& runtime_case) {
        return runtime_case.definition.id == case_id;
    });
    if (found == runtime->cases.end()) {
        return std::unexpected(QStringLiteral("Selected case is absent from the exact closure"));
    }
    return *found;
}

[[nodiscard]] auto workflowFail(app::WorkflowSessionErrorCode code, QString message)
    -> std::unexpected<app::WorkflowSessionError> {
    return std::unexpected(app::WorkflowSessionError{code, std::move(message)});
}

[[nodiscard]] auto oralFail(app::OralArgumentSessionErrorCode code, QString message)
    -> std::unexpected<app::OralArgumentSessionError> {
    return std::unexpected(app::OralArgumentSessionError{code, std::move(message)});
}

[[nodiscard]] app::OralArgumentSessionErrorCode oralCodeFor(app::WorkflowSessionErrorCode code) {
    switch (code) {
    case app::WorkflowSessionErrorCode::SessionStoreFailure:
        return app::OralArgumentSessionErrorCode::SessionStoreFailure;
    case app::WorkflowSessionErrorCode::CorruptSession:
        return app::OralArgumentSessionErrorCode::CorruptSession;
    case app::WorkflowSessionErrorCode::EngineFailure:
        return app::OralArgumentSessionErrorCode::EngineFailure;
    case app::WorkflowSessionErrorCode::CommandCodecFailure:
        return app::OralArgumentSessionErrorCode::CommandCodecFailure;
    case app::WorkflowSessionErrorCode::EventCodecFailure:
        return app::OralArgumentSessionErrorCode::EventCodecFailure;
    case app::WorkflowSessionErrorCode::InvalidConfiguration:
    case app::WorkflowSessionErrorCode::UnexpectedDocument:
    case app::WorkflowSessionErrorCode::AssetStoreFailure:
    case app::WorkflowSessionErrorCode::DocumentDigestMismatch:
        return app::OralArgumentSessionErrorCode::InvalidConfiguration;
    }
    return app::OralArgumentSessionErrorCode::InvalidConfiguration;
}

[[nodiscard]] bool containsNull(const QString& path) { return path.contains(QChar::Null); }

void closePrivateDirectory(int descriptor) {
#if defined(Q_OS_UNIX)
    if (descriptor >= 0) {
        static_cast<void>(::close(descriptor));
    }
#else
    Q_UNUSED(descriptor);
#endif
}

[[nodiscard]] auto ensurePrivateDirectory(const QString& path, const QString& boundary,
                                          QStringView description) -> std::expected<void, QString> {
    const auto opened = storage::detail::ensurePrivateStateDirectory(path, boundary);
    if (!opened) {
        return std::unexpected(QStringLiteral("%1 is unsafe: %2").arg(description, opened.error()));
    }
    closePrivateDirectory(*opened);
    return {};
}

[[nodiscard]] auto validateProspectivePrivateDirectory(const QString& path, QStringView description)
    -> std::expected<void, QString> {
    auto candidate = path;
    while (!QFileInfo::exists(candidate)) {
        const auto parent = QDir::cleanPath(QFileInfo(candidate).absoluteDir().absolutePath());
        if (parent == candidate) {
            return std::unexpected(
                QStringLiteral("%1 has no usable directory ancestor").arg(description));
        }
        candidate = parent;
    }
    const auto opened = candidate == path ? storage::detail::openPrivateStateDirectory(candidate)
                                          : storage::detail::openPrivateStateController(candidate);
    if (!opened) {
        return std::unexpected(QStringLiteral("%1 is unsafe: %2").arg(description, opened.error()));
    }
    closePrivateDirectory(*opened);
    return {};
}

[[nodiscard]] auto canonicalProspectivePath(const QString& path)
    -> std::expected<QString, QString> {
    auto candidate = path;
    QStringList missing_components;
    while (true) {
        const QFileInfo info(candidate);
        if (info.exists()) {
            const auto canonical = info.canonicalFilePath();
            if (canonical.isEmpty()) {
                return std::unexpected(
                    QStringLiteral("Local session path has no canonical existing ancestor"));
            }
            auto resolved = canonical;
            for (const auto& component : missing_components) {
                resolved = QDir(resolved).filePath(component);
            }
            return QDir::cleanPath(resolved);
        }
        missing_components.prepend(info.fileName());
        const auto parent = QDir::cleanPath(info.absoluteDir().absolutePath());
        if (parent == candidate) {
            return std::unexpected(
                QStringLiteral("Local session path has no existing filesystem ancestor"));
        }
        candidate = parent;
    }
}

[[nodiscard]] auto preparePaths(LocalSessionPaths paths)
    -> std::expected<LocalSessionPaths, QString> {
    if (paths.database_path.isEmpty() || paths.asset_root.isEmpty() ||
        containsNull(paths.database_path) || containsNull(paths.asset_root)) {
        return std::unexpected(
            QStringLiteral("Local session database and asset paths must be nonempty and NUL-free"));
    }
    if (!QDir::isAbsolutePath(paths.database_path) || !QDir::isAbsolutePath(paths.asset_root)) {
        return std::unexpected(QStringLiteral("Local session paths must be absolute"));
    }
    const auto clean_database_path = QDir::cleanPath(paths.database_path);
    const auto clean_asset_root = QDir::cleanPath(paths.asset_root);
    if (paths.database_path != clean_database_path || paths.asset_root != clean_asset_root) {
        return std::unexpected(QStringLiteral("Local session paths must be canonical and clean"));
    }
    paths.database_path = clean_database_path;
    paths.asset_root = clean_asset_root;
    if (paths.asset_root == paths.database_path ||
        paths.asset_root.startsWith(paths.database_path + u'/') ||
        paths.database_path.startsWith(paths.asset_root + u'/')) {
        return std::unexpected(
            QStringLiteral("Local session database and asset store must not contain each other"));
    }
    const auto canonical_database = canonicalProspectivePath(paths.database_path);
    const auto canonical_assets = canonicalProspectivePath(paths.asset_root);
    if (!canonical_database || !canonical_assets) {
        return std::unexpected(!canonical_database ? canonical_database.error()
                                                   : canonical_assets.error());
    }
    if (*canonical_database == *canonical_assets ||
        canonical_assets->startsWith(*canonical_database + u'/') ||
        canonical_database->startsWith(*canonical_assets + u'/')) {
        return std::unexpected(QStringLiteral(
            "Canonical local session database and asset paths must not contain each other"));
    }

    const QFileInfo database_info(paths.database_path);
    if (database_info.isSymbolicLink() ||
        (database_info.exists() &&
         (!database_info.isFile() ||
          QDir::cleanPath(database_info.canonicalFilePath()) != paths.database_path))) {
        return std::unexpected(
            QStringLiteral("Local session database path is not a canonical regular file"));
    }
    const QFileInfo asset_info(paths.asset_root);
    if (asset_info.isSymbolicLink() ||
        (asset_info.exists() &&
         (!asset_info.isDir() ||
          QDir::cleanPath(asset_info.canonicalFilePath()) != paths.asset_root))) {
        return std::unexpected(
            QStringLiteral("Local session asset path is not a canonical directory"));
    }
    const auto database_parent = QDir::cleanPath(database_info.absoluteDir().absolutePath());
    if (const auto validated =
            validateProspectivePrivateDirectory(paths.asset_root, u"Local session asset store");
        !validated) {
        return std::unexpected(validated.error());
    }
    if (const auto secured = ensurePrivateDirectory(database_parent, database_parent,
                                                    u"Local session database directory");
        !secured) {
        return std::unexpected(secured.error());
    }

    return paths;
}

} // namespace

QString workflowLegalStateDigest(const storage::SessionSnapshot& snapshot) {
    QByteArray framed;
    appendFrame(framed, std::string_view(legal_state_domain));
    appendFrame(framed, snapshot.session_id);
    appendFrame(framed, snapshot.engine_revision);
    appendUint64(framed, static_cast<std::uint64_t>(snapshot.authority_contract));
    appendUint64(framed, static_cast<std::uint64_t>(snapshot.sequence));
    appendFrame(framed, snapshot.created_at_utc);

    appendUint64(framed, snapshot.pins.size());
    for (const auto& pin : snapshot.pins) {
        appendFrame(framed, pin.pack_id);
        appendFrame(framed, pin.version);
        appendFrame(framed, pin.digest);
    }
    appendUint64(framed, snapshot.commands.size());
    for (const auto& command : snapshot.commands) {
        appendFrame(framed, command.command_id);
        appendUint64(framed, static_cast<std::uint64_t>(command.expected_sequence));
        appendFrame(framed, QByteArrayView(command.payload_json));
        appendFrame(framed, command.recorded_at_utc);
    }
    appendUint64(framed, snapshot.events.size());
    for (const auto& event : snapshot.events) {
        appendUint64(framed, static_cast<std::uint64_t>(event.sequence));
        appendFrame(framed, event.event_type);
        appendFrame(framed, QByteArrayView(event.payload_json));
        appendFrame(framed, event.authority_id);
    }
    appendUint64(framed, snapshot.docket.size());
    for (const auto& entry : snapshot.docket) {
        appendFrame(framed, entry.entry_id);
        appendUint64(framed, static_cast<std::uint64_t>(entry.event_sequence));
        appendFrame(framed, entry.title);
        appendFrame(framed, entry.status);
    }
    appendUint64(framed, snapshot.asset_references.size());
    for (const auto& asset : snapshot.asset_references) {
        appendFrame(framed, asset.digest);
        appendFrame(framed, asset.purpose);
    }
    return QString::fromLatin1(
        QCryptographicHash::hash(framed, QCryptographicHash::Sha256).toHex());
}

auto LocalSessionProvider::fromStandardPaths(UtcClock clock)
    -> std::expected<std::shared_ptr<LocalSessionProvider>, QString> {
    const auto root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (root.isEmpty() || containsNull(root) || !QDir::isAbsolutePath(root)) {
        return std::unexpected(
            QStringLiteral("Application-local data has no safe absolute filesystem path"));
    }
    auto private_boundary = root;
    const auto generic =
        QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
    const auto clean_root = QDir::cleanPath(root);
    if (!generic.isEmpty() && QDir::isAbsolutePath(generic) &&
        clean_root.startsWith(generic + u'/')) {
        const auto suffix = clean_root.sliced(generic.size() + 1);
        const auto first_component = suffix.section(u'/', 0, 0);
        if (!first_component.isEmpty()) {
            private_boundary = QDir(generic).filePath(first_component);
        }
    }
    if (const auto secured = ensurePrivateDirectory(clean_root, QDir::cleanPath(private_boundary),
                                                    u"Application-local data directory");
        !secured) {
        return std::unexpected(secured.error());
    }
    const auto sessions = QDir(clean_root).filePath(QStringLiteral("sessions"));
    return create(LocalSessionPaths{QDir(sessions).filePath(QStringLiteral("sessions.sqlite")),
                                    QDir(sessions).filePath(QStringLiteral("assets"))},
                  std::move(clock));
}

auto LocalSessionProvider::create(LocalSessionPaths paths, UtcClock clock)
    -> std::expected<std::shared_ptr<LocalSessionProvider>, QString> {
    auto prepared = preparePaths(std::move(paths));
    if (!prepared) {
        return std::unexpected(prepared.error());
    }
    if (!clock) {
        clock = [] { return QDateTime::currentDateTimeUtc(); };
    }
    auto owner_store = storage::SessionStore::open(prepared->database_path);
    if (!owner_store) {
        if (owner_store.error().code == storage::StoreErrorCode::StateInUse) {
            return std::unexpected(
                QStringLiteral("Local session state is in use by another application or provider"));
        }
        return std::unexpected(
            QStringLiteral("Local session owner cannot retain its validated database lease: %1")
                .arg(owner_store.error().message));
    }
    if (const auto secured = ensurePrivateDirectory(prepared->asset_root, prepared->asset_root,
                                                    u"Local session asset store");
        !secured) {
        return std::unexpected(secured.error());
    }
    storage::AssetStore asset_store(prepared->asset_root);
    if (const auto recovered = (*owner_store)->recoverAssetStore(asset_store); !recovered) {
        return std::unexpected(QStringLiteral("Local session assets cannot be recovered safely: %1")
                                   .arg(recovered.error().message));
    }
    return std::shared_ptr<LocalSessionProvider>(
        new LocalSessionProvider(std::move(*prepared), std::move(clock), std::move(*owner_store)));
}

LocalSessionProvider::LocalSessionProvider(LocalSessionPaths paths, UtcClock clock,
                                           std::unique_ptr<storage::SessionStore> owner_store)
    : paths_(std::move(paths)), clock_(std::move(clock)), owner_store_(std::move(owner_store)) {}

auto LocalSessionProvider::nowUtc() const -> std::expected<QString, QString> {
    const auto value = clock_();
    if (!value.isValid()) {
        return std::unexpected(QStringLiteral("Local session UTC clock returned an invalid time"));
    }
    return value.toUTC().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'"));
}

auto LocalSessionProvider::openWorkflow(const packs::ResolvedPack& resolved_pack,
                                        const model::CaseId& case_id)
    -> std::expected<std::unique_ptr<app::WorkflowSessionController>, app::WorkflowSessionError> {
    const auto selected = runtimeCase(resolved_pack, case_id);
    if (!selected) {
        return workflowFail(app::WorkflowSessionErrorCode::InvalidConfiguration, selected.error());
    }
    const auto session_id = workflowSessionId(resolved_pack, *selected);
    model::WorkflowState initial_state;
    initial_state.session_id = session_id.toStdString();
    initial_state.workflow_id = selected->workflow.id;
    initial_state.current_stage_id = selected->workflow.initial_stage_id;
    initial_state.next_event_sequence = 1;

    auto store = owner_store_->forkConnection();
    if (!store) {
        return workflowFail(app::WorkflowSessionErrorCode::SessionStoreFailure,
                            store.error().message);
    }
    const auto existing = (*store)->loadSession(session_id);
    if (existing) {
        return app::WorkflowSessionController::reopen(
            case_id, std::move(initial_state), storage::AssetStore(paths_.asset_root),
            std::move(*store), QString::fromLatin1(workflow_engine_revision), resolved_pack);
    }
    if (existing.error().code != storage::StoreErrorCode::NotFound) {
        return workflowFail(app::WorkflowSessionErrorCode::SessionStoreFailure,
                            existing.error().message);
    }
    const auto created_at = nowUtc();
    if (!created_at) {
        return workflowFail(app::WorkflowSessionErrorCode::InvalidConfiguration,
                            created_at.error());
    }
    return app::WorkflowSessionController::create(
        case_id, std::move(initial_state), storage::AssetStore(paths_.asset_root),
        std::move(*store), QString::fromLatin1(workflow_engine_revision), *created_at,
        resolved_pack);
}

auto LocalSessionProvider::open(const packs::ResolvedPack& resolved_pack,
                                const model::CaseId& case_id,
                                const packs::RuntimeArgumentConfigId& argument_configuration_id)
    -> std::expected<std::unique_ptr<app::OralArgumentSessionController>,
                     app::OralArgumentSessionError> {
    auto workflow = openWorkflow(resolved_pack, case_id);
    if (!workflow) {
        return oralFail(oralCodeFor(workflow.error().code),
                        QStringLiteral("Workflow legal state is unavailable: %1")
                            .arg(workflow.error().message));
    }
    const auto legal_state_digest = workflowLegalStateDigest((*workflow)->snapshot());
    const auto session_id =
        oralSessionId(resolved_pack, case_id, argument_configuration_id, legal_state_digest);
    (*workflow).reset();

    auto store = owner_store_->forkConnection();
    if (!store) {
        return oralFail(app::OralArgumentSessionErrorCode::SessionStoreFailure,
                        store.error().message);
    }
    const auto existing = (*store)->loadSession(session_id);
    if (existing) {
        return app::OralArgumentSessionController::reopen(
            session_id, case_id, argument_configuration_id, legal_state_digest.toStdString(),
            std::move(*store), QString::fromLatin1(oral_engine_revision), resolved_pack);
    }
    if (existing.error().code != storage::StoreErrorCode::NotFound) {
        return oralFail(app::OralArgumentSessionErrorCode::SessionStoreFailure,
                        existing.error().message);
    }
    const auto created_at = nowUtc();
    if (!created_at) {
        return oralFail(app::OralArgumentSessionErrorCode::InvalidConfiguration,
                        created_at.error());
    }
    return app::OralArgumentSessionController::create(
        session_id, case_id, argument_configuration_id, legal_state_digest.toStdString(),
        std::move(*store), QString::fromLatin1(oral_engine_revision), *created_at, resolved_pack);
}

auto LocalSessionProvider::exportAll() const
    -> std::expected<QByteArray, SessionArchiveProviderError> {
    storage::AssetStore asset_store(paths_.asset_root);
    auto archive = storage::SessionArchive::exportSessions(*owner_store_, asset_store);
    if (!archive) {
        return archiveStorageFail(u"Cannot export Workflow/Oral sessions", archive.error());
    }
    return *archive;
}

auto LocalSessionProvider::read(QByteArrayView archive) const
    -> std::expected<storage::SessionArchiveReplayContents, SessionArchiveProviderError> {
    auto contents = storage::SessionArchive::readForReplay(archive);
    if (!contents) {
        return archiveStorageFail(u"Cannot read Workflow/Oral session archive", contents.error());
    }
    return *contents;
}

auto LocalSessionProvider::validate(const storage::SessionArchiveReplayContents& contents,
                                    std::span<const packs::ResolvedPack* const> available_closures)
    const -> std::expected<ValidatedSessionArchive, SessionArchiveProviderError> {
    if (const auto coherent = validateReplayManifest(contents); !coherent) {
        return std::unexpected(coherent.error());
    }

    std::vector<ClosureCandidate> candidates;
    candidates.reserve(available_closures.size());
    for (const auto* closure : available_closures) {
        if (closure == nullptr) {
            return archiveProviderFail(
                SessionArchiveProviderErrorCode::InvalidArgument,
                QStringLiteral("Available resolved-pack closures must not contain null entries"));
        }
        auto pins = app::revisionPinsForSession(*closure);
        if (std::ranges::find(candidates, pins, &ClosureCandidate::pins) != candidates.end()) {
            continue;
        }
        candidates.push_back(ClosureCandidate{closure, std::move(pins), std::nullopt});
    }

    struct SessionBinding final {
        ArchivedSessionFamily family;
        ClosureCandidate* closure{};
    };
    std::vector<SessionBinding> bindings;
    bindings.reserve(contents.sessions.size());
    std::set<const ClosureCandidate*> used_closures;
    for (const auto& snapshot : contents.sessions) {
        const auto family = archivedSessionFamily(snapshot);
        if (!family) {
            return std::unexpected(family.error());
        }
        auto closure = matchingClosure(candidates, snapshot.pins);
        if (!closure) {
            return std::unexpected(closure.error());
        }
        bindings.push_back(SessionBinding{*family, *closure});
        used_closures.insert(*closure);
    }

    QTemporaryDir replay_root;
    if (!replay_root.isValid() ||
        !QFile::setPermissions(replay_root.path(), QFileDevice::ReadOwner |
                                                       QFileDevice::WriteOwner |
                                                       QFileDevice::ExeOwner)) {
        return archiveProviderFail(
            SessionArchiveProviderErrorCode::TemporaryAssetStoreFailure,
            QStringLiteral("Cannot create a private temporary replay asset store"));
    }
    storage::AssetStore replay_asset_store(
        QDir(replay_root.path()).filePath(QStringLiteral("assets")));
    for (const auto& asset : contents.assets) {
        const auto stored = replay_asset_store.put(QByteArrayView(asset.bytes));
        if (!stored) {
            return archiveProviderFail(SessionArchiveProviderErrorCode::TemporaryAssetStoreFailure,
                                       QStringLiteral("Cannot materialize replay asset %1: %2")
                                           .arg(asset.digest, stored.error().message));
        }
        if (stored->sha256 != asset.digest) {
            return archiveProviderFail(
                SessionArchiveProviderErrorCode::TemporaryAssetStoreFailure,
                QStringLiteral("Replay asset %1 differs from its declared digest")
                    .arg(asset.digest));
        }
    }

    std::vector<ValidatedWorkflowArchiveSession> validated_workflows;
    validated_workflows.reserve(contents.sessions.size());
    for (std::size_t index = 0; index < contents.sessions.size(); ++index) {
        if (bindings[index].family != ArchivedSessionFamily::Workflow) {
            continue;
        }
        const auto& snapshot = contents.sessions[index];
        auto runtime = runtimeFor(*bindings[index].closure);
        if (!runtime) {
            return std::unexpected(runtime.error());
        }
        const packs::RuntimeCase* matched_case = nullptr;
        for (const auto& runtime_case : (*runtime)->cases) {
            if (workflowSessionId(*bindings[index].closure->resolved_pack, runtime_case) !=
                snapshot.session_id) {
                continue;
            }
            if (matched_case != nullptr) {
                return archiveProviderFail(
                    SessionArchiveProviderErrorCode::ReplayFailure,
                    QStringLiteral("Workflow session %1 has an ambiguous runtime identity")
                        .arg(snapshot.session_id));
            }
            matched_case = &runtime_case;
        }
        if (matched_case == nullptr) {
            return archiveProviderFail(
                SessionArchiveProviderErrorCode::ReplayFailure,
                QStringLiteral("Workflow session %1 does not identify any case in its exact "
                               "resolved closure")
                    .arg(snapshot.session_id));
        }

        model::WorkflowState initial_state;
        initial_state.session_id = snapshot.session_id.toStdString();
        initial_state.workflow_id = matched_case->workflow.id;
        initial_state.current_stage_id = matched_case->workflow.initial_stage_id;
        initial_state.next_event_sequence = 1;
        const auto replayed = app::WorkflowSessionController::validateSnapshotForReplay(
            matched_case->definition.id, initial_state, replay_asset_store, snapshot,
            QString::fromLatin1(workflow_engine_revision), *bindings[index].closure->resolved_pack);
        if (!replayed) {
            return archiveProviderFail(
                SessionArchiveProviderErrorCode::ReplayFailure,
                QStringLiteral("Workflow session %1 failed exact semantic replay: %2")
                    .arg(snapshot.session_id, replayed.error().message));
        }
        validated_workflows.push_back(ValidatedWorkflowArchiveSession{
            bindings[index].closure, matched_case, workflowLegalStateDigest(snapshot)});
    }

    for (std::size_t index = 0; index < contents.sessions.size(); ++index) {
        if (bindings[index].family != ArchivedSessionFamily::OralArgument) {
            continue;
        }
        const auto& snapshot = contents.sessions[index];
        const ValidatedWorkflowArchiveSession* matched_workflow = nullptr;
        const packs::RuntimeArgumentConfiguration* matched_argument = nullptr;
        for (const auto& workflow : validated_workflows) {
            if (workflow.closure != bindings[index].closure) {
                continue;
            }
            for (const auto& argument : workflow.runtime_case->argument_configurations) {
                if (oralSessionId(*bindings[index].closure->resolved_pack,
                                  workflow.runtime_case->definition.id, argument.id,
                                  workflow.legal_state_digest) != snapshot.session_id) {
                    continue;
                }
                if (matched_argument != nullptr) {
                    return archiveProviderFail(
                        SessionArchiveProviderErrorCode::ReplayFailure,
                        QStringLiteral("Oral-argument session %1 has an ambiguous runtime identity")
                            .arg(snapshot.session_id));
                }
                matched_workflow = &workflow;
                matched_argument = &argument;
            }
        }
        if (matched_workflow == nullptr || matched_argument == nullptr) {
            return archiveProviderFail(
                SessionArchiveProviderErrorCode::ReplayFailure,
                QStringLiteral("Oral-argument session %1 is not bound to an exact argument "
                               "configuration and a validated workflow snapshot in this archive")
                    .arg(snapshot.session_id));
        }
        const auto replayed = app::OralArgumentSessionController::validateSnapshotForReplay(
            matched_workflow->runtime_case->definition.id, matched_argument->id,
            matched_workflow->legal_state_digest.toStdString(), snapshot,
            QString::fromLatin1(oral_engine_revision), *bindings[index].closure->resolved_pack);
        if (!replayed) {
            return archiveProviderFail(
                SessionArchiveProviderErrorCode::ReplayFailure,
                QStringLiteral("Oral-argument session %1 failed exact semantic replay: %2")
                    .arg(snapshot.session_id, replayed.error().message));
        }
    }

    std::map<std::pair<QString, QString>, storage::RevisionPin> available_pins;
    for (const auto* closure : used_closures) {
        for (const auto& pin : closure->pins) {
            const auto key = std::pair{pin.pack_id, pin.version};
            const auto [found, inserted] = available_pins.emplace(key, pin);
            if (!inserted && found->second.digest != pin.digest) {
                return archiveProviderFail(
                    SessionArchiveProviderErrorCode::IncompatibleRevisionPins,
                    QStringLiteral("Available exact closures disagree on revision %1 %2")
                        .arg(pin.pack_id, pin.version));
            }
        }
    }
    ValidatedSessionArchive validated{contents.manifest, {}};
    validated.available_revision_pins.reserve(available_pins.size());
    for (const auto& [key, pin] : available_pins) {
        static_cast<void>(key);
        validated.available_revision_pins.push_back(pin);
    }
    return validated;
}

auto LocalSessionProvider::import(QByteArrayView archive,
                                  std::span<const packs::ResolvedPack* const> available_closures)
    -> std::expected<storage::SessionArchiveManifest, SessionArchiveProviderError> {
    auto contents = read(archive);
    if (!contents) {
        return std::unexpected(contents.error());
    }
    auto validated = validate(*contents, available_closures);
    if (!validated) {
        return std::unexpected(validated.error());
    }

    storage::SessionArchiveImportOptions options;
    options.available_revision_pins = validated->available_revision_pins;
    storage::AssetStore asset_store(paths_.asset_root);
    const auto imported =
        storage::SessionArchive::importSessions(archive, *owner_store_, asset_store, options);
    if (!imported) {
        return archiveStorageFail(u"Cannot create imported Workflow/Oral sessions",
                                  imported.error());
    }
    return validated->manifest;
}

const LocalSessionPaths& LocalSessionProvider::paths() const noexcept { return paths_; }

} // namespace appellate::ui
