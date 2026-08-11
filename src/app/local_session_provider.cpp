#include "local_session_provider.hpp"

#include "appellate/packs/runtime_pack.hpp"
#include "appellate/storage/asset_store.hpp"

#include <QByteArray>
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace appellate::ui {
namespace {

constexpr auto workflow_session_domain = "appellate-workbench-local-workflow-session-v1";
constexpr auto oral_session_domain = "appellate-workbench-local-oral-session-v1";
constexpr auto legal_state_domain = "appellate-workbench-workflow-legal-state-v2";
constexpr auto workflow_engine_revision = "engine.workflow.local.v1";
constexpr auto oral_engine_revision = "engine.oral.local.v1";

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

[[nodiscard]] auto validateDirectoryTarget(const QString& path, QStringView description)
    -> std::expected<void, QString> {
    auto candidate = path;
    while (true) {
        const QFileInfo info(candidate);
        if (info.isSymbolicLink()) {
            return std::unexpected(
                QStringLiteral("%1 must not contain symbolic links").arg(description));
        }
        if (info.exists()) {
            if (!info.isDir()) {
                return std::unexpected(
                    QStringLiteral("%1 parent is not a directory").arg(description));
            }
            if (QDir::cleanPath(info.canonicalFilePath()) != candidate) {
                return std::unexpected(
                    QStringLiteral("%1 must have a canonical local path").arg(description));
            }
            return {};
        }
        const auto parent = QDir::cleanPath(info.absoluteDir().absolutePath());
        if (parent == candidate) {
            return std::unexpected(
                QStringLiteral("%1 has no usable directory ancestor").arg(description));
        }
        candidate = parent;
    }
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
    if (const auto database_parent = validateDirectoryTarget(
            QDir::cleanPath(database_info.absoluteDir().absolutePath()), u"Local session database");
        !database_parent) {
        return std::unexpected(database_parent.error());
    }
    if (const auto asset_parent = validateDirectoryTarget(
            asset_info.exists() ? paths.asset_root
                                : QDir::cleanPath(asset_info.absoluteDir().absolutePath()),
            u"Local session asset store");
        !asset_parent) {
        return std::unexpected(asset_parent.error());
    }
    if (!QDir{}.mkpath(database_info.absolutePath())) {
        return std::unexpected(
            QStringLiteral("Local session database directory cannot be created"));
    }
    if (const auto created_database_parent = validateDirectoryTarget(
            QDir::cleanPath(database_info.absoluteDir().absolutePath()), u"Local session database");
        !created_database_parent) {
        return std::unexpected(created_database_parent.error());
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
    const auto sessions = QDir(root).filePath(QStringLiteral("sessions"));
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
    if (!QDir{}.mkpath(prepared->asset_root)) {
        return std::unexpected(QStringLiteral("Local session asset directory cannot be created"));
    }
    if (const auto created_asset_root =
            validateDirectoryTarget(prepared->asset_root, u"Local session asset store");
        !created_asset_root) {
        return std::unexpected(created_asset_root.error());
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

const LocalSessionPaths& LocalSessionProvider::paths() const noexcept { return paths_; }

} // namespace appellate::ui
