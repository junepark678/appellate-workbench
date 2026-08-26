#include "pack_catalog_reverse_admission_p.hpp"

#include <QByteArray>
#include <QCryptographicHash>
#include <QMetaType>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace appellate::packs::detail {
namespace {

constexpr std::uint64_t maximum_revisions = 20'000;
constexpr std::uint64_t maximum_dependencies_per_revision = 128;
constexpr std::uint64_t maximum_blobs_per_revision = 10'000;
constexpr std::uint64_t maximum_dependency_rows =
    maximum_revisions * maximum_dependencies_per_revision;
constexpr std::uint64_t maximum_blob_rows = maximum_revisions * maximum_blobs_per_revision;
constexpr std::uint64_t maximum_inventory_objects = 20'000;
constexpr qsizetype maximum_installed_at_characters = 512;
constexpr std::uint64_t maximum_blob_bytes = 512ULL * 1024ULL * 1024ULL;

struct RevisionRecord final {
    model::PackRevision revision;
    QString archive_digest;
};

struct PreflightResult final {
    std::uint64_t row_count{};
};

[[nodiscard]] auto failure(CatalogErrorCode code, QString message)
    -> std::unexpected<CatalogError> {
    return std::unexpected(CatalogError{code, std::move(message)});
}

[[nodiscard]] CatalogObservation observation(CatalogEvent event, CatalogSubject subject,
                                             CatalogOperation operation, const QString& component,
                                             std::size_t ordinal = 0,
                                             std::uint64_t byte_count = 0) {
    CatalogObservation result;
    result.event = event;
    result.subject = subject;
    result.operation = operation;
    result.component = component.toUtf8();
    result.ordinal = ordinal;
    result.byte_count = byte_count;
    return result;
}

[[nodiscard]] auto beginLifecycle(const CatalogHooks& hooks, const CatalogObservation& seen)
    -> std::expected<CatalogInjectedAction, CatalogError> {
    if (hooks.report != nullptr) {
        hooks.report->observations.push_back(seen);
    }
    const auto action = hooks.inject ? hooks.inject(seen) : CatalogInjectedAction::Continue;
    if (action == CatalogInjectedAction::FailBefore) {
        return failure(CatalogErrorCode::CannotOpen,
                       QStringLiteral("Injected reverse-admission failure"));
    }
    if (hooks.outcome) {
        const auto injected = hooks.outcome(seen);
        if (injected.has_value() && !injected->operation_succeeded) {
            return failure(CatalogErrorCode::CannotOpen,
                           QStringLiteral("Injected reverse-admission outcome"));
        }
    }
    return action;
}

[[nodiscard]] auto finishLifecycle(const CatalogHooks& hooks, const CatalogObservation& seen,
                                   CatalogInjectedAction action)
    -> std::expected<void, CatalogError> {
    if (hooks.barrier) {
        hooks.barrier(seen);
    }
    if (hooks.observe) {
        hooks.observe(seen);
    }
    if (action == CatalogInjectedAction::FailAfter) {
        return failure(CatalogErrorCode::CannotOpen,
                       QStringLiteral("Injected reverse-admission failure"));
    }
    return {};
}

template <typename Function>
[[nodiscard]] auto withLifecycle(const CatalogHooks& hooks, CatalogObservation seen,
                                 Function&& function) -> decltype(function()) {
    using Result = decltype(function());
    const auto action = beginLifecycle(hooks, seen);
    if (!action) {
        return Result(std::unexpected(action.error()));
    }
    auto result = function();
    const auto finished = finishLifecycle(hooks, seen, *action);
    if (!finished) {
        return Result(std::unexpected(finished.error()));
    }
    return result;
}

[[nodiscard]] auto queryFailure(const QSqlQuery& query, const QString& action)
    -> std::unexpected<CatalogError> {
    return failure(CatalogErrorCode::CannotOpen,
                   QStringLiteral("%1: %2").arg(action, query.lastError().text()));
}

[[nodiscard]] auto nonnegativeInteger(const QVariant& value, const QString& action)
    -> std::expected<std::uint64_t, CatalogError> {
    bool converted = false;
    const auto parsed = value.toLongLong(&converted);
    if (!converted || parsed < 0) {
        return failure(CatalogErrorCode::CannotOpen,
                       QStringLiteral("%1 returned an invalid count").arg(action));
    }
    return static_cast<std::uint64_t>(parsed);
}

[[nodiscard]] auto
runPreflight(QSqlDatabase& database, const QString& sql, std::uint64_t maximum_rows,
             const std::optional<std::uint64_t>& exact_rows, const QString& label,
             const CatalogHooks& hooks, CatalogOperation operation)
    -> std::expected<PreflightResult, CatalogError> {
    if (const auto plan =
            verifyCatalogQueryPlan(database, sql, label + QStringLiteral(":preflight"),
                                   CatalogSubject::PrivateDatabaseMain, hooks, operation);
        !plan) {
        return std::unexpected(plan.error());
    }
    QSqlQuery query(database);
    query.setForwardOnly(true);
    if (!query.exec(sql)) {
        return queryFailure(query, QStringLiteral("preflight %1").arg(label));
    }
    if (!query.next()) {
        return failure(CatalogErrorCode::CannotOpen,
                       QStringLiteral("Reverse-admission preflight returned no row"));
    }
    const auto count = nonnegativeInteger(query.value(0), label);
    const auto invalid = nonnegativeInteger(query.value(1), label);
    if (!count || !invalid || query.next()) {
        return failure(CatalogErrorCode::CannotOpen,
                       QStringLiteral("Reverse-admission preflight is malformed"));
    }
    if (*count > maximum_rows || (exact_rows.has_value() && *count != *exact_rows) ||
        *invalid != 0) {
        return failure(CatalogErrorCode::CorruptCatalog,
                       QStringLiteral("Reverse-admission preflight rejected %1").arg(label));
    }
    const auto seen =
        observation(CatalogEvent::SqlAllocationPreflight, CatalogSubject::PrivateDatabaseMain,
                    operation, label, static_cast<std::size_t>(*count));
    const auto admitted =
        withLifecycle(hooks, seen, []() -> std::expected<void, CatalogError> { return {}; });
    if (!admitted) {
        return std::unexpected(admitted.error());
    }
    return PreflightResult{*count};
}

[[nodiscard]] bool validDigest(const QString& value) {
    return value.size() == 64 && std::ranges::all_of(value, [](QChar character) {
               return (character >= u'0' && character <= u'9') ||
                      (character >= u'a' && character <= u'f');
           });
}

[[nodiscard]] bool validInstalledAt(const QString& value) {
    return !value.isEmpty() && value.size() <= maximum_installed_at_characters &&
           !value.contains(QChar::Null);
}

void accountRows(const CatalogHooks& hooks, std::uint64_t rows) {
    if (hooks.report != nullptr) {
        hooks.report->allocations.row_container_growth += static_cast<std::size_t>(rows);
    }
}

void accountTextConversions(const CatalogHooks& hooks, std::size_t conversions) {
    if (hooks.report != nullptr) {
        hooks.report->allocations.text_conversions += conversions;
    }
}

[[nodiscard]] auto revisionLess(const model::PackRevision& left, const model::PackRevision& right) {
    return std::tie(left.id.value, left.version, left.digest) <
           std::tie(right.id.value, right.version, right.digest);
}

void normalizeDependencies(std::vector<model::PackDependency>& dependencies) {
    std::ranges::sort(dependencies, [](const auto& left, const auto& right) {
        return revisionLess(left.revision, right.revision);
    });
}

void normalizeBlobs(std::vector<model::BlobDescriptor>& blobs) {
    std::ranges::sort(blobs, [](const auto& left, const auto& right) {
        return std::tie(left.path, left.media_type, left.byte_size, left.sha256) <
               std::tie(right.path, right.media_type, right.byte_size, right.sha256);
    });
}

void addUint64(QCryptographicHash& hash, std::uint64_t value) {
    std::array<char, 8> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto shift = static_cast<unsigned>((bytes.size() - index - 1U) * 8U);
        bytes.at(index) = static_cast<char>((value >> shift) & 0xffU);
    }
    hash.addData(QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())));
}

void addFrame(QCryptographicHash& hash, const std::string& value) {
    addUint64(hash, static_cast<std::uint64_t>(value.size()));
    hash.addData(QByteArrayView(value.data(), static_cast<qsizetype>(value.size())));
}

[[nodiscard]] QString blobSetDigest(const model::PackRevision& revision,
                                    const std::vector<model::BlobDescriptor>& sorted_blobs) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, "appellate-workbench-catalog-blob-set-v1");
    addFrame(hash, revision.id.value);
    addFrame(hash, revision.version);
    addFrame(hash, revision.digest);
    addUint64(hash, static_cast<std::uint64_t>(sorted_blobs.size()));
    for (const auto& blob : sorted_blobs) {
        addFrame(hash, blob.path);
        addFrame(hash, blob.media_type);
        addUint64(hash, blob.byte_size);
        addFrame(hash, blob.sha256);
    }
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] auto revisionPreflight(QSqlDatabase& database, const CatalogHooks& hooks,
                                     CatalogOperation operation)
    -> std::expected<PreflightResult, CatalogError> {
    return runPreflight(
        database,
        QStringLiteral("SELECT COUNT(*), COALESCE(SUM(CASE WHEN "
                       "typeof(pack_id) <> 'text' OR length(CAST(pack_id AS BLOB)) > 128 OR "
                       "typeof(version) <> 'text' OR length(CAST(version AS BLOB)) > 128 OR "
                       "typeof(digest) <> 'text' OR length(CAST(digest AS BLOB)) > 64 OR "
                       "typeof(archive_sha256) <> 'text' OR "
                       "length(CAST(archive_sha256 AS BLOB)) > 64 OR "
                       "typeof(installed_at_utc) <> 'text' OR "
                       "length(CAST(installed_at_utc AS BLOB)) > 2048 "
                       "THEN 1 ELSE 0 END), 0) FROM pack_revisions"),
        maximum_revisions, std::nullopt, QStringLiteral("pack revisions"), hooks, operation);
}

[[nodiscard]] auto dependencyPreflight(QSqlDatabase& database, const CatalogHooks& hooks,
                                       CatalogOperation operation)
    -> std::expected<PreflightResult, CatalogError> {
    return runPreflight(
        database,
        QStringLiteral("SELECT COUNT(*), COALESCE(SUM(CASE WHEN "
                       "typeof(pack_id) <> 'text' OR length(CAST(pack_id AS BLOB)) > 128 OR "
                       "typeof(version) <> 'text' OR length(CAST(version AS BLOB)) > 128 OR "
                       "typeof(dependency_pack_id) <> 'text' OR "
                       "length(CAST(dependency_pack_id AS BLOB)) > 128 OR "
                       "typeof(dependency_version) <> 'text' OR "
                       "length(CAST(dependency_version AS BLOB)) > 128 OR "
                       "typeof(dependency_digest) <> 'text' OR "
                       "length(CAST(dependency_digest AS BLOB)) > 64 "
                       "THEN 1 ELSE 0 END), 0) FROM pack_dependencies"),
        maximum_dependency_rows, std::nullopt, QStringLiteral("pack dependencies"), hooks,
        operation);
}

[[nodiscard]] auto blobPreflight(QSqlDatabase& database, const CatalogHooks& hooks,
                                 CatalogOperation operation)
    -> std::expected<PreflightResult, CatalogError> {
    return runPreflight(
        database,
        QStringLiteral("SELECT COUNT(*), COALESCE(SUM(CASE WHEN "
                       "typeof(pack_id) <> 'text' OR length(CAST(pack_id AS BLOB)) > 128 OR "
                       "typeof(version) <> 'text' OR length(CAST(version AS BLOB)) > 128 OR "
                       "typeof(path) <> 'text' OR length(CAST(path AS BLOB)) > 240 OR "
                       "typeof(media_type) <> 'text' OR length(CAST(media_type AS BLOB)) > 15 OR "
                       "typeof(byte_size) <> 'integer' OR "
                       "typeof(sha256) <> 'text' OR length(CAST(sha256 AS BLOB)) > 64 "
                       "THEN 1 ELSE 0 END), 0) FROM pack_blobs"),
        maximum_blob_rows, std::nullopt, QStringLiteral("pack blobs"), hooks, operation);
}

[[nodiscard]] auto blobSetPreflight(QSqlDatabase& database, std::uint64_t revision_count,
                                    const CatalogHooks& hooks, CatalogOperation operation)
    -> std::expected<PreflightResult, CatalogError> {
    return runPreflight(
        database,
        QStringLiteral("SELECT COUNT(*), COALESCE(SUM(CASE WHEN "
                       "typeof(pack_id) <> 'text' OR length(CAST(pack_id AS BLOB)) > 128 OR "
                       "typeof(version) <> 'text' OR length(CAST(version AS BLOB)) > 128 OR "
                       "typeof(blob_count) <> 'integer' OR "
                       "typeof(descriptor_sha256) <> 'text' OR "
                       "length(CAST(descriptor_sha256 AS BLOB)) > 64 "
                       "THEN 1 ELSE 0 END), 0) FROM pack_blob_sets"),
        maximum_revisions, revision_count, QStringLiteral("pack blob sets"), hooks, operation);
}

[[nodiscard]] auto readRevisions(QSqlDatabase& database, std::uint64_t expected_count,
                                 const CatalogHooks& hooks, CatalogOperation operation)
    -> std::expected<std::vector<RevisionRecord>, CatalogError> {
    const auto sql =
        QStringLiteral("SELECT pack_id, version, digest, archive_sha256, installed_at_utc "
                       "FROM pack_revisions ORDER BY pack_id, version");
    if (const auto plan = verifyCatalogQueryPlan(
            database, sql, QStringLiteral("pack revisions"), CatalogSubject::PrivateDatabaseMain,
            hooks, operation, 0, CatalogQueryPlanRequirement::IndexedAccess);
        !plan) {
        return std::unexpected(plan.error());
    }
    QSqlQuery query(database);
    query.setForwardOnly(true);
    if (!query.exec(sql)) {
        return queryFailure(query, QStringLiteral("read catalog revisions"));
    }
    std::vector<RevisionRecord> result;
    result.reserve(static_cast<std::size_t>(expected_count));
    accountRows(hooks, expected_count);
    while (query.next()) {
        if (result.size() >= static_cast<std::size_t>(expected_count)) {
            return failure(CatalogErrorCode::CorruptCatalog,
                           QStringLiteral("Catalog revision count changed after preflight"));
        }
        accountTextConversions(hooks, 5);
        const auto pack_id = query.value(0).toString();
        const auto version = query.value(1).toString();
        const auto digest = query.value(2).toString();
        const auto archive_digest = query.value(3).toString();
        const auto installed_at = query.value(4).toString();
        if (!validDigest(digest) || !validDigest(archive_digest) ||
            !validInstalledAt(installed_at)) {
            return failure(CatalogErrorCode::CorruptCatalog,
                           QStringLiteral("Catalog revision metadata is invalid"));
        }
        result.push_back(RevisionRecord{
            model::PackRevision{
                model::PackId{pack_id.toUtf8().toStdString()},
                version.toUtf8().toStdString(),
                digest.toLatin1().toStdString(),
            },
            archive_digest,
        });
    }
    if (result.size() != static_cast<std::size_t>(expected_count)) {
        return failure(CatalogErrorCode::CorruptCatalog,
                       QStringLiteral("Catalog revision count changed after preflight"));
    }
    return result;
}

[[nodiscard]] auto countForRevision(QSqlDatabase& database, const QString& sql,
                                    const model::PackRevision& revision, std::uint64_t maximum,
                                    const QString& label, const CatalogHooks& hooks,
                                    CatalogOperation operation)
    -> std::expected<std::uint64_t, CatalogError> {
    if (const auto plan = verifyCatalogQueryPlan(
            database, sql, label + QStringLiteral(":count"), CatalogSubject::PrivateDatabaseMain,
            hooks, operation, 2, CatalogQueryPlanRequirement::IndexedAccess);
        !plan) {
        return std::unexpected(plan.error());
    }
    QSqlQuery query(database);
    query.setForwardOnly(true);
    query.prepare(sql);
    query.addBindValue(QString::fromUtf8(revision.id.value));
    query.addBindValue(QString::fromUtf8(revision.version));
    if (!query.exec()) {
        return queryFailure(query, QStringLiteral("count %1").arg(label));
    }
    if (!query.next()) {
        return failure(CatalogErrorCode::CannotOpen,
                       QStringLiteral("Catalog row count returned no value"));
    }
    const auto count = nonnegativeInteger(query.value(0), label);
    if (!count || query.next()) {
        return failure(CatalogErrorCode::CannotOpen,
                       QStringLiteral("Catalog row count is malformed"));
    }
    if (*count > maximum) {
        return failure(CatalogErrorCode::CorruptCatalog,
                       QStringLiteral("Catalog per-revision row bound is exceeded"));
    }
    return count;
}

[[nodiscard]] auto readDependencies(QSqlDatabase& database, const model::PackRevision& revision,
                                    const CatalogHooks& hooks, CatalogOperation operation)
    -> std::expected<std::vector<model::PackDependency>, CatalogError> {
    const auto count = countForRevision(
        database,
        QStringLiteral("SELECT COUNT(*) FROM pack_dependencies WHERE pack_id = ? AND version = ?"),
        revision, maximum_dependencies_per_revision, QStringLiteral("pack dependencies"), hooks,
        operation);
    if (!count) {
        return std::unexpected(count.error());
    }
    const auto sql =
        QStringLiteral("SELECT dependency_pack_id, dependency_version, dependency_digest "
                       "FROM pack_dependencies WHERE pack_id = ? AND version = ? "
                       "ORDER BY dependency_pack_id, dependency_version");
    if (const auto plan = verifyCatalogQueryPlan(
            database, sql, QStringLiteral("pack dependencies"), CatalogSubject::PrivateDatabaseMain,
            hooks, operation, 2, CatalogQueryPlanRequirement::IndexedAccess);
        !plan) {
        return std::unexpected(plan.error());
    }
    QSqlQuery query(database);
    query.setForwardOnly(true);
    query.prepare(sql);
    query.addBindValue(QString::fromUtf8(revision.id.value));
    query.addBindValue(QString::fromUtf8(revision.version));
    if (!query.exec()) {
        return queryFailure(query, QStringLiteral("read catalog dependencies"));
    }
    std::vector<model::PackDependency> result;
    result.reserve(static_cast<std::size_t>(*count));
    accountRows(hooks, *count);
    while (query.next()) {
        if (result.size() >= static_cast<std::size_t>(*count)) {
            return failure(CatalogErrorCode::CorruptCatalog,
                           QStringLiteral("Dependency count changed after preflight"));
        }
        accountTextConversions(hooks, 3);
        const auto id = query.value(0).toString();
        const auto version = query.value(1).toString();
        const auto digest = query.value(2).toString();
        if (!validDigest(digest)) {
            return failure(CatalogErrorCode::CorruptCatalog,
                           QStringLiteral("Catalog dependency digest is invalid"));
        }
        result.push_back(model::PackDependency{
            model::PackRevision{model::PackId{id.toUtf8().toStdString()},
                                version.toUtf8().toStdString(), digest.toLatin1().toStdString()}});
    }
    if (result.size() != static_cast<std::size_t>(*count)) {
        return failure(CatalogErrorCode::CorruptCatalog,
                       QStringLiteral("Dependency count changed after preflight"));
    }
    return result;
}

[[nodiscard]] auto readBlobs(QSqlDatabase& database, const model::PackRevision& revision,
                             const CatalogHooks& hooks, CatalogOperation operation)
    -> std::expected<std::vector<model::BlobDescriptor>, CatalogError> {
    const auto count = countForRevision(
        database,
        QStringLiteral("SELECT COUNT(*) FROM pack_blobs WHERE pack_id = ? AND version = ?"),
        revision, maximum_blobs_per_revision, QStringLiteral("pack blobs"), hooks, operation);
    if (!count) {
        return std::unexpected(count.error());
    }
    const auto sql = QStringLiteral("SELECT path, media_type, byte_size, sha256 FROM pack_blobs "
                                    "WHERE pack_id = ? AND version = ? ORDER BY path");
    if (const auto plan = verifyCatalogQueryPlan(
            database, sql, QStringLiteral("pack blobs"), CatalogSubject::PrivateDatabaseMain, hooks,
            operation, 2, CatalogQueryPlanRequirement::IndexedAccess);
        !plan) {
        return std::unexpected(plan.error());
    }
    QSqlQuery query(database);
    query.setForwardOnly(true);
    query.prepare(sql);
    query.addBindValue(QString::fromUtf8(revision.id.value));
    query.addBindValue(QString::fromUtf8(revision.version));
    if (!query.exec()) {
        return queryFailure(query, QStringLiteral("read catalog blobs"));
    }
    std::vector<model::BlobDescriptor> result;
    result.reserve(static_cast<std::size_t>(*count));
    accountRows(hooks, *count);
    while (query.next()) {
        if (result.size() >= static_cast<std::size_t>(*count)) {
            return failure(CatalogErrorCode::CorruptCatalog,
                           QStringLiteral("Blob count changed after preflight"));
        }
        accountTextConversions(hooks, 3);
        const auto path = query.value(0).toString();
        const auto media_type = query.value(1).toString();
        bool size_ok = false;
        const auto byte_size = query.value(2).toLongLong(&size_ok);
        const auto digest = query.value(3).toString();
        if (!size_ok || byte_size <= 0 ||
            static_cast<std::uint64_t>(byte_size) > maximum_blob_bytes ||
            media_type != QStringLiteral("application/pdf") || !validDigest(digest)) {
            return failure(CatalogErrorCode::CorruptCatalog,
                           QStringLiteral("Catalog blob descriptor is invalid"));
        }
        result.push_back(model::BlobDescriptor{
            path.toUtf8().toStdString(), media_type.toUtf8().toStdString(),
            static_cast<std::uint64_t>(byte_size), digest.toLatin1().toStdString()});
    }
    if (result.size() != static_cast<std::size_t>(*count)) {
        return failure(CatalogErrorCode::CorruptCatalog,
                       QStringLiteral("Blob count changed after preflight"));
    }
    return result;
}

[[nodiscard]] auto validateBlobSet(QSqlDatabase& database, const model::PackRevision& revision,
                                   const std::vector<model::BlobDescriptor>& sorted_blobs,
                                   const CatalogHooks& hooks, CatalogOperation operation)
    -> std::expected<void, CatalogError> {
    const auto sql = QStringLiteral("SELECT blob_count, descriptor_sha256 FROM pack_blob_sets "
                                    "WHERE pack_id = ? AND version = ?");
    if (const auto plan = verifyCatalogQueryPlan(
            database, sql, QStringLiteral("pack blob set"), CatalogSubject::PrivateDatabaseMain,
            hooks, operation, 2, CatalogQueryPlanRequirement::IndexedAccess);
        !plan) {
        return std::unexpected(plan.error());
    }
    QSqlQuery query(database);
    query.setForwardOnly(true);
    query.prepare(sql);
    query.addBindValue(QString::fromUtf8(revision.id.value));
    query.addBindValue(QString::fromUtf8(revision.version));
    if (!query.exec()) {
        return queryFailure(query, QStringLiteral("read catalog blob set"));
    }
    bool count_ok = false;
    if (!query.next()) {
        return failure(CatalogErrorCode::CorruptCatalog,
                       QStringLiteral("Catalog revision has no blob set"));
    }
    const auto count = query.value(0).toLongLong(&count_ok);
    accountTextConversions(hooks, 1);
    const auto digest = query.value(1).toString();
    if (!count_ok || count < 0 ||
        static_cast<std::uint64_t>(count) != static_cast<std::uint64_t>(sorted_blobs.size()) ||
        !validDigest(digest) || digest != blobSetDigest(revision, sorted_blobs) || query.next()) {
        return failure(CatalogErrorCode::CorruptCatalog,
                       QStringLiteral("Catalog blob-set summary differs from its descriptors"));
    }
    return {};
}

[[nodiscard]] auto mapArchiveFailure(const Error& error) -> std::unexpected<CatalogError> {
    if (error.code == ErrorCode::UnsupportedCapability) {
        return failure(CatalogErrorCode::UnsupportedCapability, error.message);
    }
    if (error.code == ErrorCode::CannotRead) {
        return failure(CatalogErrorCode::CannotOpen, error.message);
    }
    return failure(CatalogErrorCode::CorruptCatalog, error.message);
}

[[nodiscard]] auto loadArchive(const QString& archive_digest,
                               const SecureCatalogArchiveLoader& loader, const CatalogHooks& hooks,
                               CatalogOperation operation, std::size_t ordinal)
    -> std::expected<LoadedPack, CatalogError> {
    const auto seen = observation(CatalogEvent::ArchiveImported, CatalogSubject::ArchiveObject,
                                  operation, archive_digest, ordinal);
    const auto action = beginLifecycle(hooks, seen);
    if (!action) {
        return std::unexpected(action.error());
    }
    auto loaded = loader(archive_digest);
    const auto finished = finishLifecycle(hooks, seen, *action);
    if (!finished) {
        return std::unexpected(finished.error());
    }
    if (!loaded) {
        return mapArchiveFailure(loaded.error());
    }
    return std::move(*loaded);
}

[[nodiscard]] auto reportArchiveValidated(const QString& archive_digest, const CatalogHooks& hooks,
                                          CatalogOperation operation, std::size_t ordinal)
    -> std::expected<void, CatalogError> {
    return withLifecycle(hooks,
                         observation(CatalogEvent::ArchiveValidated, CatalogSubject::ArchiveObject,
                                     operation, archive_digest, ordinal),
                         []() -> std::expected<void, CatalogError> { return {}; });
}

[[nodiscard]] auto consistentBlobGroups(QSqlDatabase& database, const CatalogHooks& hooks,
                                        CatalogOperation operation)
    -> std::expected<void, CatalogError> {
    const auto sql = QStringLiteral(
        "SELECT COUNT(*) FROM ("
        "SELECT sha256 FROM pack_blobs GROUP BY sha256 "
        "HAVING MIN(media_type) <> MAX(media_type) OR MIN(byte_size) <> MAX(byte_size))");
    if (const auto plan =
            verifyCatalogQueryPlan(database, sql, QStringLiteral("blob descriptor groups"),
                                   CatalogSubject::PrivateDatabaseMain, hooks, operation, 0,
                                   CatalogQueryPlanRequirement::IndexedAccess);
        !plan) {
        return std::unexpected(plan.error());
    }
    QSqlQuery query(database);
    query.setForwardOnly(true);
    if (!query.exec(sql)) {
        return queryFailure(query, QStringLiteral("group catalog blob descriptors"));
    }
    const auto inconsistent =
        query.next()
            ? nonnegativeInteger(query.value(0), QStringLiteral("blob descriptor groups"))
            : std::expected<std::uint64_t, CatalogError>{failure(
                  CatalogErrorCode::CannotOpen, QStringLiteral("Blob grouping returned no row"))};
    if (!inconsistent || query.next()) {
        return failure(CatalogErrorCode::CannotOpen,
                       QStringLiteral("Blob grouping result is malformed"));
    }
    if (*inconsistent != 0) {
        return failure(CatalogErrorCode::CorruptCatalog,
                       QStringLiteral("Shared blob descriptors are inconsistent"));
    }
    return {};
}

[[nodiscard]] auto referencedBlobSize(QSqlDatabase& database, const QString& digest,
                                      const CatalogHooks& hooks, CatalogOperation operation)
    -> std::expected<std::optional<std::uint64_t>, CatalogError> {
    const auto sql = QStringLiteral("SELECT byte_size FROM pack_blobs WHERE sha256 = ? LIMIT 1");
    if (const auto plan =
            verifyCatalogQueryPlan(database, sql, QStringLiteral("present blob reference"),
                                   CatalogSubject::PrivateDatabaseMain, hooks, operation, 1,
                                   CatalogQueryPlanRequirement::IndexedAccess);
        !plan) {
        return std::unexpected(plan.error());
    }
    QSqlQuery query(database);
    query.setForwardOnly(true);
    query.prepare(sql);
    query.addBindValue(digest);
    if (!query.exec()) {
        return queryFailure(query, QStringLiteral("resolve present blob object"));
    }
    if (!query.next()) {
        return std::optional<std::uint64_t>{};
    }
    bool converted = false;
    const auto size = query.value(0).toLongLong(&converted);
    if (!converted || size <= 0 || static_cast<std::uint64_t>(size) > maximum_blob_bytes ||
        query.next()) {
        return failure(CatalogErrorCode::CorruptCatalog,
                       QStringLiteral("Present blob reference is invalid"));
    }
    return std::optional<std::uint64_t>{static_cast<std::uint64_t>(size)};
}

[[nodiscard]] auto verifyPresentBlob(const QString& digest, std::uint64_t byte_size,
                                     const SecureCatalogBlobVerifier& verifier,
                                     const CatalogHooks& hooks, CatalogOperation operation,
                                     std::size_t ordinal) -> std::expected<void, CatalogError> {
    return withLifecycle(hooks,
                         observation(CatalogEvent::BlobValidated, CatalogSubject::BlobObject,
                                     operation, digest, ordinal, byte_size),
                         [&]() -> std::expected<void, CatalogError> {
                             const auto verified = verifier(digest, byte_size);
                             if (verified) {
                                 return {};
                             }
                             if (verified.error().code == CatalogErrorCode::CorruptCatalog ||
                                 verified.error().code == CatalogErrorCode::UnsupportedCapability) {
                                 return std::unexpected(verified.error());
                             }
                             return failure(CatalogErrorCode::CannotOpen, verified.error().message);
                         });
}

[[nodiscard]] auto validateInventory(const std::set<QString>& archives,
                                     const std::set<QString>& blobs,
                                     CatalogSchemaGeneration generation)
    -> std::expected<void, CatalogError> {
    if (archives.size() > maximum_inventory_objects || blobs.size() > maximum_inventory_objects ||
        archives.size() > static_cast<std::size_t>(maximum_inventory_objects) - blobs.size()) {
        return failure(CatalogErrorCode::CorruptCatalog,
                       QStringLiteral("Catalog object inventory exceeds its bound"));
    }
    if (!std::ranges::all_of(archives, validDigest) || !std::ranges::all_of(blobs, validDigest) ||
        (generation == CatalogSchemaGeneration::Version1 && !blobs.empty())) {
        return failure(CatalogErrorCode::CorruptCatalog,
                       QStringLiteral("Catalog object inventory is invalid"));
    }
    return {};
}

} // namespace

auto validateCatalogReverseAdmission(QSqlDatabase& private_database,
                                     CatalogSchemaGeneration generation,
                                     const std::set<QString>& present_archive_digests,
                                     const std::set<QString>& present_blob_digests,
                                     const SecureCatalogArchiveLoader& archive_loader,
                                     const SecureCatalogBlobVerifier& blob_verifier,
                                     const CatalogHooks& hooks, CatalogOperation operation)
    -> std::expected<void, CatalogError> {
    if (!private_database.isValid() || !private_database.isOpen() || !archive_loader ||
        (generation == CatalogSchemaGeneration::Current && !blob_verifier) ||
        (generation != CatalogSchemaGeneration::Version1 &&
         generation != CatalogSchemaGeneration::Current)) {
        return failure(CatalogErrorCode::CannotOpen,
                       QStringLiteral("Reverse-admission inputs are invalid"));
    }
    if (const auto inventory =
            validateInventory(present_archive_digests, present_blob_digests, generation);
        !inventory) {
        return inventory;
    }

    const auto revision_preflight = revisionPreflight(private_database, hooks, operation);
    if (!revision_preflight) {
        return std::unexpected(revision_preflight.error());
    }
    const auto dependency_preflight = dependencyPreflight(private_database, hooks, operation);
    if (!dependency_preflight) {
        return std::unexpected(dependency_preflight.error());
    }
    std::optional<PreflightResult> blob_preflight;
    std::optional<PreflightResult> blob_set_preflight;
    if (generation == CatalogSchemaGeneration::Current) {
        auto blobs = blobPreflight(private_database, hooks, operation);
        if (!blobs) {
            return std::unexpected(blobs.error());
        }
        blob_preflight.emplace(*blobs);
        auto blob_sets =
            blobSetPreflight(private_database, revision_preflight->row_count, hooks, operation);
        if (!blob_sets) {
            return std::unexpected(blob_sets.error());
        }
        blob_set_preflight.emplace(*blob_sets);
    }

    auto revisions =
        readRevisions(private_database, revision_preflight->row_count, hooks, operation);
    if (!revisions) {
        return std::unexpected(revisions.error());
    }
    std::set<QString> referenced_archives;
    for (const auto& revision : *revisions) {
        if (!referenced_archives.insert(revision.archive_digest).second) {
            return failure(CatalogErrorCode::CorruptCatalog,
                           QStringLiteral("Multiple revisions reference one archive object"));
        }
    }
    if (referenced_archives != present_archive_digests) {
        return failure(CatalogErrorCode::CorruptCatalog,
                       QStringLiteral("Archive namespace and database references differ"));
    }

    std::uint64_t consumed_dependencies{};
    std::uint64_t consumed_blobs{};
    std::uint64_t consumed_blob_sets{};
    for (std::size_t index = 0; index < revisions->size(); ++index) {
        const auto& record = revisions->at(index);
        auto loaded = loadArchive(record.archive_digest, archive_loader, hooks, operation, index);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        if (loaded->revision != record.revision ||
            loaded->dependencies.size() > maximum_dependencies_per_revision ||
            (generation == CatalogSchemaGeneration::Current &&
             loaded->blobs.size() > maximum_blobs_per_revision)) {
            return failure(CatalogErrorCode::CorruptCatalog,
                           QStringLiteral("Archive revision metadata differs from the catalog"));
        }

        auto dependencies = readDependencies(private_database, record.revision, hooks, operation);
        if (!dependencies) {
            return std::unexpected(dependencies.error());
        }
        consumed_dependencies += static_cast<std::uint64_t>(dependencies->size());
        auto loaded_dependencies = loaded->dependencies;
        normalizeDependencies(*dependencies);
        normalizeDependencies(loaded_dependencies);
        if (*dependencies != loaded_dependencies) {
            return failure(CatalogErrorCode::CorruptCatalog,
                           QStringLiteral("Archive dependencies differ from the catalog"));
        }

        if (generation == CatalogSchemaGeneration::Current) {
            auto blobs = readBlobs(private_database, record.revision, hooks, operation);
            if (!blobs) {
                return std::unexpected(blobs.error());
            }
            consumed_blobs += static_cast<std::uint64_t>(blobs->size());
            auto loaded_blobs = loaded->blobs;
            normalizeBlobs(*blobs);
            normalizeBlobs(loaded_blobs);
            if (*blobs != loaded_blobs) {
                return failure(CatalogErrorCode::CorruptCatalog,
                               QStringLiteral("Archive blob descriptors differ from the catalog"));
            }
            if (const auto blob_set =
                    validateBlobSet(private_database, record.revision, *blobs, hooks, operation);
                !blob_set) {
                return blob_set;
            }
            ++consumed_blob_sets;
        }
        if (const auto reported =
                reportArchiveValidated(record.archive_digest, hooks, operation, index);
            !reported) {
            return reported;
        }
    }

    if (consumed_dependencies != dependency_preflight->row_count ||
        (generation == CatalogSchemaGeneration::Current &&
         (consumed_blobs != blob_preflight->row_count ||
          consumed_blob_sets != blob_set_preflight->row_count))) {
        return failure(CatalogErrorCode::CorruptCatalog,
                       QStringLiteral("Catalog contains rows outside its revision set"));
    }

    if (generation == CatalogSchemaGeneration::Version1) {
        return {};
    }
    if (const auto groups = consistentBlobGroups(private_database, hooks, operation); !groups) {
        return groups;
    }
    std::size_t ordinal{};
    for (const auto& digest : present_blob_digests) {
        const auto size = referencedBlobSize(private_database, digest, hooks, operation);
        if (!size) {
            return std::unexpected(size.error());
        }
        if (!size->has_value()) {
            return failure(CatalogErrorCode::CorruptCatalog,
                           QStringLiteral("Present blob object is unreferenced"));
        }
        if (const auto verified =
                verifyPresentBlob(digest, **size, blob_verifier, hooks, operation, ordinal);
            !verified) {
            return verified;
        }
        ++ordinal;
    }
    return {};
}

} // namespace appellate::packs::detail
