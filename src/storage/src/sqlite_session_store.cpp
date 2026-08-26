#include "appellate/storage/asset_store.hpp"
#include "appellate/storage/detail/private_state.hpp"
#include "appellate/storage/session_store.hpp"
#include "strict_json_scan.hpp"

#include <QDateTime>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QUuid>
#include <QVariant>

#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <optional>
#include <ranges>
#include <vector>
#include <utility>

#if defined(Q_OS_UNIX)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <io.h>
#endif

namespace appellate::storage {
namespace {

constexpr auto current_schema_version = 3;
constexpr auto application_id = 1'095'784'258;
constexpr qsizetype maximum_asset_purpose_length = 128;
constexpr std::size_t maximum_asset_references_per_batch = 4096;
constexpr std::size_t maximum_events_per_batch = 4096;
constexpr std::size_t maximum_docket_changes_per_batch = 4096;
constexpr std::size_t maximum_session_pins = 128;
constexpr qsizetype maximum_json_bytes = 1024 * 1024;
constexpr qsizetype backup_buffer_bytes = 64 * 1024;

[[nodiscard]] auto fail(StoreErrorCode code, QString message) -> std::unexpected<StoreError> {
    return std::unexpected(StoreError{code, std::move(message)});
}

[[nodiscard]] auto queryFailure(StoreErrorCode code, const QSqlQuery& query, const QString& action)
    -> std::unexpected<StoreError> {
    return fail(code, QStringLiteral("%1: %2").arg(action, query.lastError().text()));
}

[[nodiscard]] bool validText(const QString& value) {
    return !value.isEmpty() && value.size() <= 512 && !value.contains(QChar::Null);
}

[[nodiscard]] bool validCanonicalUtc(const QString& value) {
    if (value.size() != 20 || !value.endsWith(u'Z')) {
        return false;
    }
    const auto parsed = QDateTime::fromString(value, Qt::ISODate);
    return parsed.isValid() && parsed.offsetFromUtc() == 0 &&
           parsed.toUTC().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'")) == value;
}

[[nodiscard]] bool validJsonObject(const QByteArray& value) {
    if (value.isEmpty() || value.size() > maximum_json_bytes) {
        return false;
    }
    if (const auto strict = detail::scanStrictJson(QByteArrayView(value)); !strict) {
        return false;
    }
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(value, &parse_error);
    return parse_error.error == QJsonParseError::NoError && document.isObject();
}

[[nodiscard]] bool validDigest(const QString& value) {
    if (value.size() != 64) {
        return false;
    }
    for (const auto character : value) {
        if (!(character >= u'0' && character <= u'9') &&
            !(character >= u'a' && character <= u'f')) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool isSqliteReservedSidecarPath(const QString& path) {
    const auto leaf = QFileInfo(path).fileName();
    return leaf.endsWith(QStringLiteral("-wal"), Qt::CaseInsensitive) ||
           leaf.endsWith(QStringLiteral("-shm"), Qt::CaseInsensitive) ||
           leaf.endsWith(QStringLiteral("-journal"), Qt::CaseInsensitive);
}

[[nodiscard]] bool validAssetPurpose(const QString& value) {
    if (value.isEmpty() || value.size() > maximum_asset_purpose_length) {
        return false;
    }
    return std::ranges::all_of(value, [](QChar character) {
        return (character >= u'a' && character <= u'z') ||
               (character >= u'0' && character <= u'9') || character == u'.' || character == u'_' ||
               character == u'-' || character == u':';
    });
}

[[nodiscard]] auto authorityContractName(SessionAuthorityContract contract)
    -> std::optional<QString> {
    switch (contract) {
    case SessionAuthorityContract::LegacyV1:
        return QStringLiteral("legacy-v1");
    case SessionAuthorityContract::CanonicalV2:
        return QStringLiteral("canonical-v2");
    }
    return std::nullopt;
}

[[nodiscard]] auto parseAuthorityContract(const QString& value)
    -> std::optional<SessionAuthorityContract> {
    if (value == QStringLiteral("legacy-v1")) {
        return SessionAuthorityContract::LegacyV1;
    }
    if (value == QStringLiteral("canonical-v2")) {
        return SessionAuthorityContract::CanonicalV2;
    }
    return std::nullopt;
}

[[nodiscard]] auto execStatement(QSqlDatabase& database, const QString& sql, StoreErrorCode code,
                                 const QString& action) -> std::expected<void, StoreError> {
    QSqlQuery query(database);
    if (!query.exec(sql)) {
        return queryFailure(code, query, action);
    }
    return {};
}

[[nodiscard]] bool syncFile(QFileDevice& file) {
    if (!file.flush()) {
        return false;
    }
    const auto handle = file.handle();
    if (handle < 0) {
        return false;
    }
#if defined(Q_OS_UNIX)
    int result{};
    do {
        result = ::fsync(static_cast<int>(handle));
    } while (result != 0 && errno == EINTR);
    return result == 0;
#elif defined(Q_OS_WIN)
    return ::_commit(static_cast<int>(handle)) == 0;
#else
    return true;
#endif
}

[[nodiscard]] bool syncDirectory(const QString& directory) {
#if defined(Q_OS_UNIX)
    auto flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const auto encoded = QFile::encodeName(directory);
    const auto descriptor = ::open(encoded.constData(), flags);
    if (descriptor < 0) {
        return false;
    }
    int result{};
    do {
        result = ::fsync(descriptor);
    } while (result != 0 && errno == EINTR);
    const auto saved_errno = errno;
    static_cast<void>(::close(descriptor));
    errno = saved_errno;
    return result == 0;
#else
    Q_UNUSED(directory);
    return true;
#endif
}

[[nodiscard]] auto migrationSql(int version) -> std::expected<QByteArray, StoreError>;

void appendSchemaFrame(QByteArray& output, const QVariant& value) {
    if (value.isNull()) {
        output.append("N", 1);
        return;
    }
    const auto bytes = value.toString().toUtf8();
    output.append("V", 1);
    const auto size = static_cast<quint64>(bytes.size());
    for (int shift = 56; shift >= 0; shift -= 8) {
        output.append(static_cast<char>((size >> static_cast<unsigned>(shift)) & 0xffU));
    }
    output.append(bytes);
}

[[nodiscard]] QString quotedIdentifier(QString value) {
    value.replace(u'"', QStringLiteral("\"\""));
    return u'"' + value + u'"';
}

[[nodiscard]] auto appendQueryRows(QByteArray& output, QSqlQuery& query, QStringView action)
    -> std::expected<void, StoreError> {
    if (!query.isActive()) {
        return queryFailure(StoreErrorCode::MigrationFailed, query, action.toString());
    }
    while (query.next()) {
        output.append("R", 1);
        for (int column = 0; column < query.record().count(); ++column) {
            appendSchemaFrame(output, query.value(column));
        }
    }
    return {};
}

[[nodiscard]] auto schemaFingerprint(QSqlDatabase& database)
    -> std::expected<QByteArray, StoreError> {
    QByteArray output("appellate-session-schema-fingerprint-v2", 39);
    QSqlQuery objects(database);
    if (!objects.exec(QStringLiteral(
            "SELECT type, name, tbl_name, COALESCE(sql, '') FROM sqlite_schema "
            "WHERE name NOT LIKE 'sqlite_%' OR name LIKE 'sqlite_autoindex_%' "
            "ORDER BY type, name, tbl_name"))) {
        return queryFailure(StoreErrorCode::MigrationFailed, objects,
                            QStringLiteral("read schema objects"));
    }
    if (const auto appended = appendQueryRows(output, objects, u"read schema objects");
        !appended) {
        return std::unexpected(appended.error());
    }

    QSqlQuery table_flags(database);
    if (!table_flags.exec(QStringLiteral(
            "SELECT schema, name, type, ncol, wr, strict FROM pragma_table_list "
            "WHERE schema='main' AND name NOT LIKE 'sqlite_%' ORDER BY name"))) {
        return queryFailure(StoreErrorCode::MigrationFailed, table_flags,
                            QStringLiteral("read schema table flags"));
    }
    if (const auto appended =
            appendQueryRows(output, table_flags, u"read schema table flags");
        !appended) {
        return std::unexpected(appended.error());
    }

    QSqlQuery tables(database);
    if (!tables.exec(QStringLiteral(
            "SELECT name FROM sqlite_schema WHERE type='table' AND name NOT LIKE 'sqlite_%' "
            "ORDER BY name"))) {
        return queryFailure(StoreErrorCode::MigrationFailed, tables,
                            QStringLiteral("enumerate schema tables"));
    }
    QStringList table_names;
    while (tables.next()) {
        table_names.push_back(tables.value(0).toString());
    }
    for (const auto& table : table_names) {
        appendSchemaFrame(output, table);
        const auto quoted_table = quotedIdentifier(table);
        QSqlQuery columns(database);
        if (!columns.exec(QStringLiteral("PRAGMA table_xinfo(%1)").arg(quoted_table))) {
            return queryFailure(StoreErrorCode::MigrationFailed, columns,
                                QStringLiteral("read table columns"));
        }
        if (const auto appended = appendQueryRows(output, columns, u"read table columns");
            !appended) {
            return std::unexpected(appended.error());
        }

        QSqlQuery foreign_keys(database);
        if (!foreign_keys.exec(
                QStringLiteral("PRAGMA foreign_key_list(%1)").arg(quoted_table))) {
            return queryFailure(StoreErrorCode::MigrationFailed, foreign_keys,
                                QStringLiteral("read table foreign keys"));
        }
        if (const auto appended =
                appendQueryRows(output, foreign_keys, u"read table foreign keys");
            !appended) {
            return std::unexpected(appended.error());
        }

        QSqlQuery indexes(database);
        if (!indexes.exec(QStringLiteral("PRAGMA index_list(%1)").arg(quoted_table))) {
            return queryFailure(StoreErrorCode::MigrationFailed, indexes,
                                QStringLiteral("read table indexes"));
        }
        QStringList index_names;
        while (indexes.next()) {
            output.append("R", 1);
            for (int column = 0; column < indexes.record().count(); ++column) {
                appendSchemaFrame(output, indexes.value(column));
            }
            index_names.push_back(indexes.value(1).toString());
        }
        std::ranges::sort(index_names);
        for (const auto& index : index_names) {
            appendSchemaFrame(output, index);
            QSqlQuery index_columns(database);
            if (!index_columns.exec(
                    QStringLiteral("PRAGMA index_xinfo(%1)").arg(quotedIdentifier(index)))) {
                return queryFailure(StoreErrorCode::MigrationFailed, index_columns,
                                    QStringLiteral("read index columns"));
            }
            if (const auto appended =
                    appendQueryRows(output, index_columns, u"read index columns");
                !appended) {
                return std::unexpected(appended.error());
            }
        }
    }
    return output;
}

[[nodiscard]] auto pragmaInteger(QSqlDatabase& database, QStringView pragma)
    -> std::expected<int, StoreError> {
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA %1").arg(pragma)) || !query.next()) {
        return queryFailure(StoreErrorCode::MigrationFailed, query,
                            QStringLiteral("read PRAGMA %1").arg(pragma));
    }
    return query.value(0).toInt();
}

[[nodiscard]] auto validateApplicationSchema(QSqlDatabase& database,
                                             bool allow_older_schema)
    -> std::expected<void, StoreError> {
    const auto stored_application_id = pragmaInteger(database, u"application_id");
    const auto stored_user_version = pragmaInteger(database, u"user_version");
    if (!stored_application_id || !stored_user_version) {
        return std::unexpected((!stored_application_id ? stored_application_id.error()
                                                       : stored_user_version.error()));
    }

    QSqlQuery object_count(database);
    if (!object_count.exec(QStringLiteral(
            "SELECT COUNT(*) FROM sqlite_schema WHERE type IN ('table','index','view','trigger') "
            "AND (name NOT LIKE 'sqlite_%' OR name LIKE 'sqlite_autoindex_%')")) ||
        !object_count.next()) {
        return queryFailure(StoreErrorCode::MigrationFailed, object_count,
                            QStringLiteral("count schema objects"));
    }
    if (object_count.value(0).toInt() == 0) {
        return fail(StoreErrorCode::MigrationFailed,
                    QStringLiteral("A pre-existing schema-empty database is not an Appellate database"));
    }

    QSqlQuery versions(database);
    if (!versions.exec(QStringLiteral(
            "SELECT version FROM schema_migrations ORDER BY version"))) {
        return queryFailure(StoreErrorCode::MigrationFailed, versions,
                            QStringLiteral("read migration ledger"));
    }
    std::vector<int> migration_versions;
    while (versions.next()) {
        migration_versions.push_back(versions.value(0).toInt());
    }
    if (migration_versions.empty() ||
        migration_versions.size() > static_cast<std::size_t>(current_schema_version)) {
        return fail(StoreErrorCode::MigrationFailed,
                    QStringLiteral("Migration ledger is empty or newer than this application"));
    }
    for (std::size_t index = 0; index < migration_versions.size(); ++index) {
        if (migration_versions[index] != static_cast<int>(index) + 1) {
            return fail(StoreErrorCode::MigrationFailed,
                        QStringLiteral("Migration ledger is not consecutive from version 1"));
        }
    }
    const auto version = migration_versions.back();
    if ((!allow_older_schema && version != current_schema_version) ||
        version > current_schema_version) {
        return fail(StoreErrorCode::MigrationFailed,
                    QStringLiteral("Database schema %1 is not supported").arg(version));
    }
    if (version < 3) {
        if (*stored_application_id != 0 || *stored_user_version != 0) {
            return fail(StoreErrorCode::MigrationFailed,
                        QStringLiteral("Legacy database identity PRAGMAs are unexpected"));
        }
    } else if (*stored_application_id != application_id || *stored_user_version != version) {
        return fail(StoreErrorCode::MigrationFailed,
                    QStringLiteral("Database application identity does not match its ledger"));
    }

    const auto reference_connection =
        QStringLiteral("appellate-schema-reference-%1")
            .arg(QUuid::createUuid().toString(QUuid::Id128));
    std::expected<QByteArray, StoreError> reference_fingerprint;
    {
        auto reference = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), reference_connection);
        reference.setDatabaseName(QStringLiteral(":memory:"));
        if (!reference.open()) {
            reference_fingerprint = fail(StoreErrorCode::MigrationFailed,
                                         reference.lastError().text());
        } else {
            for (int migration_version = 1; migration_version <= version; ++migration_version) {
                const auto migration = migrationSql(migration_version);
                if (!migration) {
                    reference_fingerprint = std::unexpected(migration.error());
                    break;
                }
                const auto statements =
                    QString::fromUtf8(*migration).split(u';', Qt::SkipEmptyParts);
                for (const auto& statement : statements) {
                    if (statement.trimmed().isEmpty()) {
                        continue;
                    }
                    QSqlQuery apply(reference);
                    if (!apply.exec(statement)) {
                        reference_fingerprint = queryFailure(
                            StoreErrorCode::MigrationFailed, apply,
                            QStringLiteral("construct reference schema"));
                        break;
                    }
                }
                if (!reference_fingerprint.has_value()) {
                    break;
                }
                QSqlQuery record(reference);
                record.prepare(QStringLiteral(
                    "INSERT INTO schema_migrations(version, applied_at_utc) VALUES(?, ?)"));
                record.addBindValue(migration_version);
                record.addBindValue(QStringLiteral("2026-08-11T00:00:00Z"));
                if (!record.exec()) {
                    reference_fingerprint = queryFailure(
                        StoreErrorCode::MigrationFailed, record,
                        QStringLiteral("construct reference migration ledger"));
                    break;
                }
            }
            if (reference_fingerprint.has_value()) {
                reference_fingerprint = schemaFingerprint(reference);
            }
            reference.close();
        }
        reference = QSqlDatabase{};
    }
    QSqlDatabase::removeDatabase(reference_connection);
    if (!reference_fingerprint) {
        return std::unexpected(reference_fingerprint.error());
    }
    const auto candidate_fingerprint = schemaFingerprint(database);
    if (!candidate_fingerprint) {
        return std::unexpected(candidate_fingerprint.error());
    }
    if (*candidate_fingerprint != *reference_fingerprint) {
        return fail(StoreErrorCode::MigrationFailed,
                    QStringLiteral(
                        "Database schema objects, columns, indexes, or foreign keys differ from "
                        "the exact Appellate schema (candidate %1, expected %2)")
                        .arg(QString::fromLatin1(QCryptographicHash::hash(
                                                    *candidate_fingerprint,
                                                    QCryptographicHash::Sha256)
                                                    .toHex()),
                             QString::fromLatin1(QCryptographicHash::hash(
                                                    *reference_fingerprint,
                                                    QCryptographicHash::Sha256)
                                                    .toHex())));
    }

    if (version == 3) {
        QSqlQuery identity(database);
        if (!identity.exec(QStringLiteral(
                "SELECT singleton, identity FROM store_identity ORDER BY singleton")) ||
            !identity.next()) {
            return queryFailure(StoreErrorCode::MigrationFailed, identity,
                                QStringLiteral("read database asset-store identity"));
        }
        static const QRegularExpression identity_pattern(
            QStringLiteral("^[0-9a-f]{32}$"));
        if (identity.value(0).toInt() != 1 ||
            !identity_pattern.match(identity.value(1).toString()).hasMatch() ||
            identity.next()) {
            return fail(StoreErrorCode::MigrationFailed,
                        QStringLiteral(
                            "Schema-3 database must contain exactly one valid store identity"));
        }
    }

    QSqlQuery foreign_key_check(database);
    if (!foreign_key_check.exec(QStringLiteral("PRAGMA foreign_key_check"))) {
        return queryFailure(StoreErrorCode::MigrationFailed, foreign_key_check,
                            QStringLiteral("check database foreign keys"));
    }
    if (foreign_key_check.next()) {
        return fail(StoreErrorCode::MigrationFailed,
                    QStringLiteral("Database contains a foreign-key violation"));
    }
    return {};
}

[[nodiscard]] auto verifyDatabase(const QString& path, StoreErrorCode failure_code,
                                  bool allow_older_schema = false)
    -> std::expected<void, StoreError> {
    const auto connection =
        QStringLiteral("appellate-verify-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    std::expected<void, StoreError> result;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        database.setDatabaseName(path);
        if (!database.open()) {
            result = fail(failure_code, database.lastError().text());
        } else {
            QSqlQuery integrity(database);
            if (!integrity.exec(QStringLiteral("PRAGMA integrity_check")) || !integrity.next() ||
                integrity.value(0).toString() != QStringLiteral("ok")) {
                result = queryFailure(failure_code, integrity,
                                      QStringLiteral("verify database integrity"));
            } else {
                const auto schema = validateApplicationSchema(database, allow_older_schema);
                if (!schema) {
                    result = fail(failure_code, schema.error().message);
                }
            }
            database.close();
        }
        database = QSqlDatabase{};
    }
    QSqlDatabase::removeDatabase(connection);
    return result;
}

[[nodiscard]] auto databaseContainsAssetReferences(const QString& path,
                                                    StoreErrorCode failure_code)
    -> std::expected<bool, StoreError> {
    const auto connection =
        QStringLiteral("appellate-asset-reference-check-%1")
            .arg(QUuid::createUuid().toString(QUuid::Id128));
    std::expected<bool, StoreError> result =
        fail(failure_code, QStringLiteral("Cannot inspect database asset references"));
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        database.setDatabaseName(path);
        if (!database.open()) {
            result = fail(failure_code, database.lastError().text());
        } else {
            QSqlQuery query(database);
            if (!query.exec(QStringLiteral("SELECT 1 FROM asset_references LIMIT 1"))) {
                result = queryFailure(failure_code, query,
                                      QStringLiteral("inspect database asset references"));
            } else {
                result = query.next();
            }
            database.close();
        }
        database = QSqlDatabase{};
    }
    QSqlDatabase::removeDatabase(connection);
    return result;
}

[[nodiscard]] auto databaseSchemaVersion(const QString& path, StoreErrorCode failure_code)
    -> std::expected<int, StoreError> {
    const auto connection =
        QStringLiteral("appellate-schema-version-check-%1")
            .arg(QUuid::createUuid().toString(QUuid::Id128));
    std::expected<int, StoreError> result =
        fail(failure_code, QStringLiteral("Cannot inspect database schema version"));
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        database.setDatabaseName(path);
        if (!database.open()) {
            result = fail(failure_code, database.lastError().text());
        } else {
            QSqlQuery query(database);
            if (!query.exec(QStringLiteral(
                    "SELECT COALESCE(MAX(version),0) FROM schema_migrations")) ||
                !query.next()) {
                result = queryFailure(failure_code, query,
                                      QStringLiteral("inspect database schema version"));
            } else {
                result = query.value(0).toInt();
            }
            database.close();
        }
        database = QSqlDatabase{};
    }
    QSqlDatabase::removeDatabase(connection);
    return result;
}

[[nodiscard]] auto rotateRestoredDatabaseIdentity(const QString& path)
    -> std::expected<QString, StoreError> {
    const auto connection =
        QStringLiteral("appellate-restore-identity-%1")
            .arg(QUuid::createUuid().toString(QUuid::Id128));
    std::expected<QString, StoreError> result =
        fail(StoreErrorCode::RestoreFailed,
             QStringLiteral("Cannot rotate the restored database identity"));
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(path);
        if (!database.open()) {
            result = fail(StoreErrorCode::RestoreFailed, database.lastError().text());
        } else {
            QSqlQuery journal(database);
            if (!journal.exec(QStringLiteral("PRAGMA journal_mode=DELETE")) || !journal.next() ||
                journal.value(0).toString().compare(QStringLiteral("delete"),
                                                    Qt::CaseInsensitive) != 0) {
                result = queryFailure(StoreErrorCode::RestoreFailed, journal,
                                      QStringLiteral("make restored database standalone"));
            } else {
                journal.finish();
                QSqlQuery begin(database);
                if (!begin.exec(QStringLiteral("BEGIN IMMEDIATE"))) {
                    result = queryFailure(StoreErrorCode::RestoreFailed, begin,
                                          QStringLiteral("reserve restored database"));
                } else {
                    begin.finish();
                    QSqlQuery references(database);
                    if (!references.exec(QStringLiteral(
                            "SELECT 1 FROM asset_references LIMIT 1"))) {
                        result = queryFailure(StoreErrorCode::RestoreFailed, references,
                                              QStringLiteral("inspect restored asset references"));
                    } else if (references.next()) {
                        result = fail(
                            StoreErrorCode::RestoreFailed,
                            QStringLiteral(
                                "Database-only restore cannot contain asset references"));
                    } else {
                        references.finish();
                        const auto identity =
                            QUuid::createUuid().toString(QUuid::Id128).toLower();
                        QSqlQuery update(database);
                        update.prepare(QStringLiteral(
                            "UPDATE store_identity SET identity=? WHERE singleton=1"));
                        update.addBindValue(identity);
                        if (!update.exec() || update.numRowsAffected() != 1) {
                            result = queryFailure(StoreErrorCode::RestoreFailed, update,
                                                  QStringLiteral(
                                                      "rotate restored asset-store identity"));
                        } else {
                            update.finish();
                            QSqlQuery commit(database);
                            if (!commit.exec(QStringLiteral("COMMIT"))) {
                                result = queryFailure(StoreErrorCode::RestoreFailed, commit,
                                                      QStringLiteral(
                                                          "commit restored database identity"));
                            } else {
                                result = identity;
                            }
                        }
                    }
                    if (!result.has_value()) {
                        QSqlQuery rollback(database);
                        static_cast<void>(rollback.exec(QStringLiteral("ROLLBACK")));
                    }
                }
            }
            database.close();
        }
        database = QSqlDatabase{};
    }
    QSqlDatabase::removeDatabase(connection);
    if (!result) {
        return result;
    }
    for (const auto& suffix : {QStringLiteral("-wal"), QStringLiteral("-shm"),
                               QStringLiteral("-journal")}) {
        if (QFileInfo::exists(path + suffix)) {
            return fail(StoreErrorCode::RestoreFailed,
                        QStringLiteral("Restored database retained a SQLite sidecar"));
        }
    }
    return result;
}

#if defined(Q_OS_UNIX)
struct CooperativeDatabaseLock final {
    int descriptor{-1};

    ~CooperativeDatabaseLock() {
        if (descriptor >= 0) {
            static_cast<void>(::flock(descriptor, LOCK_UN));
            static_cast<void>(::close(descriptor));
        }
    }
};

QMutex cooperative_lock_registry_mutex;
QHash<QString, std::weak_ptr<CooperativeDatabaseLock>> cooperative_lock_registry;

[[nodiscard]] bool sameOwnedRegularFileBinding(int descriptor, int parent_descriptor,
                                               const QByteArray& name);
[[nodiscard]] bool quarantineFreshRegularFile(int descriptor, int parent_descriptor,
                                              const QByteArray& name);
#endif

struct DatabasePreflight final {
    QString original_path;
    QString sqlite_path;
    QString private_snapshot_path;
    std::unique_ptr<QTemporaryDir> private_snapshot_directory;
    bool created_by_this_open{};
#if defined(Q_OS_UNIX)
    struct Identity final {
        dev_t device{};
        ino_t inode{};
        nlink_t link_count{};
        off_t size{};
        timespec modified{};
        timespec changed{};

        friend bool operator==(const Identity& left, const Identity& right) {
            return left.device == right.device && left.inode == right.inode &&
                   left.link_count == right.link_count &&
                   left.size == right.size &&
                   left.modified.tv_sec == right.modified.tv_sec &&
                   left.modified.tv_nsec == right.modified.tv_nsec &&
                   left.changed.tv_sec == right.changed.tv_sec &&
                   left.changed.tv_nsec == right.changed.tv_nsec;
        }
    };

    struct FreshFile final {
        QByteArray name;
        int descriptor{-1};

        FreshFile(QByteArray value, int retained_descriptor)
            : name(std::move(value)), descriptor(retained_descriptor) {}
        FreshFile(const FreshFile&) = delete;
        FreshFile& operator=(const FreshFile&) = delete;
        FreshFile(FreshFile&& other) noexcept
            : name(std::move(other.name)), descriptor(std::exchange(other.descriptor, -1)) {}
        FreshFile& operator=(FreshFile&&) = delete;
        ~FreshFile() {
            if (descriptor >= 0) {
                static_cast<void>(::close(descriptor));
            }
        }
    };

    int parent_descriptor{-1};
    int database_anchor_descriptor{-1};
    std::shared_ptr<CooperativeDatabaseLock> cooperative_lock;
    QByteArray database_name;
    Identity parent_identity{};
    std::optional<Identity> database_identity;
    std::optional<Identity> wal_identity;
    std::optional<Identity> shm_identity;
    std::optional<Identity> journal_identity;
    std::vector<FreshFile> fresh_sidecars;
    bool cleanup_fresh_files{true};
#endif

    DatabasePreflight() = default;
    DatabasePreflight(const DatabasePreflight&) = delete;
    DatabasePreflight& operator=(const DatabasePreflight&) = delete;
    DatabasePreflight(DatabasePreflight&& other) noexcept
        : original_path(std::move(other.original_path)), sqlite_path(std::move(other.sqlite_path)),
          private_snapshot_path(std::move(other.private_snapshot_path)),
          private_snapshot_directory(std::move(other.private_snapshot_directory)),
          created_by_this_open(other.created_by_this_open)
#if defined(Q_OS_UNIX)
          ,
          parent_descriptor(std::exchange(other.parent_descriptor, -1)),
          database_anchor_descriptor(std::exchange(other.database_anchor_descriptor, -1)),
          cooperative_lock(std::move(other.cooperative_lock)),
          database_name(std::move(other.database_name)), parent_identity(other.parent_identity),
          database_identity(other.database_identity), wal_identity(other.wal_identity),
          shm_identity(other.shm_identity), journal_identity(other.journal_identity),
          fresh_sidecars(std::move(other.fresh_sidecars)),
          cleanup_fresh_files(std::exchange(other.cleanup_fresh_files, false))
#endif
    {}
    DatabasePreflight& operator=(DatabasePreflight&&) = delete;
    ~DatabasePreflight() {
#if defined(Q_OS_UNIX)
        if (cleanup_fresh_files && parent_descriptor >= 0) {
            for (auto sidecar = fresh_sidecars.rbegin(); sidecar != fresh_sidecars.rend();
                 ++sidecar) {
                static_cast<void>(quarantineFreshRegularFile(sidecar->descriptor, parent_descriptor,
                                                             sidecar->name));
            }
            if (created_by_this_open) {
                static_cast<void>(quarantineFreshRegularFile(database_anchor_descriptor,
                                                             parent_descriptor, database_name));
            }
            static_cast<void>(::fsync(parent_descriptor));
        }
        if (parent_descriptor >= 0) {
            static_cast<void>(::close(parent_descriptor));
        }
        if (database_anchor_descriptor >= 0) {
            static_cast<void>(::close(database_anchor_descriptor));
        }
#endif
    }

    void acceptFreshFiles() {
#if defined(Q_OS_UNIX)
        cleanup_fresh_files = false;
        fresh_sidecars.clear();
#endif
    }
};

struct SessionStoreLifetimeLease final {
    explicit SessionStoreLifetimeLease(DatabasePreflight&& validated_preflight)
        : preflight(std::move(validated_preflight)) {}

    DatabasePreflight preflight;
    QMutex fork_mutex;
};

#if defined(Q_OS_UNIX)
[[nodiscard]] DatabasePreflight::Identity identityFromStat(const struct stat& status) {
    return DatabasePreflight::Identity{status.st_dev, status.st_ino, status.st_nlink,
                                       status.st_size,
#if defined(Q_OS_DARWIN)
                                       status.st_mtimespec, status.st_ctimespec
#else
                                       status.st_mtim, status.st_ctim
#endif
    };
}

[[nodiscard]] QString systemError(QStringView action) {
    return QStringLiteral("%1: %2").arg(action, QString::fromLocal8Bit(std::strerror(errno)));
}

[[nodiscard]] auto openAbsoluteDirectoryNoFollow(const QString& absolute_path)
    -> std::expected<int, StoreError> {
    const auto descriptor = detail::openPrivateStateController(absolute_path);
    return descriptor ? std::expected<int, StoreError>{*descriptor}
                      : fail(StoreErrorCode::OpenFailed, descriptor.error());
}

[[nodiscard]] bool sameOwnedRegularFileBinding(int descriptor, int parent_descriptor,
                                               const QByteArray& name) {
    struct stat held{};
    struct stat named{};
    return descriptor >= 0 && ::fstat(descriptor, &held) == 0 &&
           ::fstatat(parent_descriptor, name.constData(), &named, AT_SYMLINK_NOFOLLOW) == 0 &&
           S_ISREG(held.st_mode) && S_ISREG(named.st_mode) && held.st_dev == named.st_dev &&
           held.st_ino == named.st_ino && held.st_uid == ::geteuid() &&
           named.st_uid == ::geteuid() && held.st_nlink == 1 && named.st_nlink == 1;
}

[[nodiscard]] int renameNoReplace(int parent_descriptor, const QByteArray& source_name,
                                  const QByteArray& destination_name) {
#if defined(Q_OS_LINUX) && defined(SYS_renameat2)
    int result{};
    do {
        result =
            static_cast<int>(::syscall(SYS_renameat2, parent_descriptor, source_name.constData(),
                                       parent_descriptor, destination_name.constData(), 1U));
    } while (result != 0 && errno == EINTR);
    return result;
#else
    Q_UNUSED(parent_descriptor);
    Q_UNUSED(source_name);
    Q_UNUSED(destination_name);
    errno = ENOTSUP;
    return -1;
#endif
}

[[nodiscard]] bool quarantineFreshRegularFile(int descriptor, int parent_descriptor,
                                              const QByteArray& name) {
    if (!sameOwnedRegularFileBinding(descriptor, parent_descriptor, name)) {
        return false;
    }
    const auto quarantine_name = QByteArrayLiteral(".appellate-quarantine-") +
                                 QUuid::createUuid().toByteArray(QUuid::Id128) +
                                 QByteArrayLiteral(".tmp");
    // Never unlink or roll back this detach: either the retained fresh inode or a same-UID raced
    // replacement remains preserved under the reserved tombstone name.
    if (renameNoReplace(parent_descriptor, name, quarantine_name) == 0) {
        int synced{};
        do {
            synced = ::fsync(parent_descriptor);
        } while (synced != 0 && errno == EINTR);
        return synced == 0;
    }
    return false;
}

constexpr QByteArrayView database_initialization_payload("appellate-session-initializing-v1\n");

[[nodiscard]] QByteArray databaseInitializationMarkerName(const QByteArray& database_name) {
    return QByteArrayLiteral(".") + database_name + QByteArrayLiteral(".appellate-initializing");
}

[[nodiscard]] auto validateDatabaseInitializationMarker(int descriptor, int parent_descriptor,
                                                        const QByteArray& marker_name)
    -> std::expected<void, StoreError> {
    if (const auto private_file =
            detail::validatePrivateStateFileBinding(descriptor, parent_descriptor, marker_name);
        !private_file) {
        return fail(StoreErrorCode::OpenFailed,
                    QStringLiteral("Database initialization marker is unsafe: %1")
                        .arg(private_file.error()));
    }
    struct stat status{};
    if (::fstat(descriptor, &status) != 0 ||
        status.st_size != database_initialization_payload.size()) {
        return fail(StoreErrorCode::OpenFailed,
                    QStringLiteral("Database initialization marker is malformed"));
    }
    QByteArray marker(database_initialization_payload.size(), Qt::Uninitialized);
    qsizetype offset{};
    while (offset < marker.size()) {
        ssize_t count{};
        do {
            count = ::pread(descriptor, marker.data() + offset,
                            static_cast<std::size_t>(marker.size() - offset), offset);
        } while (count < 0 && errno == EINTR);
        if (count <= 0) {
            return fail(StoreErrorCode::OpenFailed,
                        QStringLiteral("Database initialization marker is incomplete"));
        }
        offset += count;
    }
    if (marker != database_initialization_payload) {
        return fail(StoreErrorCode::OpenFailed,
                    QStringLiteral("Database initialization marker is malformed"));
    }
    return {};
}

[[nodiscard]] auto openDatabaseInitializationMarker(int parent_descriptor,
                                                    const QByteArray& database_name)
    -> std::expected<std::optional<int>, StoreError> {
    const auto marker_name = databaseInitializationMarkerName(database_name);
    auto flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const auto descriptor = ::openat(parent_descriptor, marker_name.constData(), flags);
    if (descriptor < 0 && errno == ENOENT) {
        return std::optional<int>{};
    }
    if (descriptor < 0) {
        return fail(StoreErrorCode::OpenFailed,
                    systemError(u"Open database initialization marker"));
    }
    if (const auto validated =
            validateDatabaseInitializationMarker(descriptor, parent_descriptor, marker_name);
        !validated) {
        static_cast<void>(::close(descriptor));
        return std::unexpected(validated.error());
    }
    return std::optional<int>{descriptor};
}

[[nodiscard]] auto publishDatabaseInitializationMarker(int parent_descriptor,
                                                       const QByteArray& database_name)
    -> std::expected<void, StoreError> {
    const auto existing = openDatabaseInitializationMarker(parent_descriptor, database_name);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (existing->has_value()) {
        static_cast<void>(::close(**existing));
        return {};
    }
#if !defined(O_TMPFILE) || !defined(AT_EMPTY_PATH)
    return fail(StoreErrorCode::OpenFailed,
                QStringLiteral("Atomic database initialization markers are unavailable"));
#else
    auto flags = O_TMPFILE | O_RDWR;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const auto descriptor = ::openat(parent_descriptor, ".", flags, 0600);
    if (descriptor < 0) {
        return fail(StoreErrorCode::OpenFailed,
                    systemError(u"Create database initialization marker"));
    }
    if (const auto normalized = detail::normalizeNewPrivateStateFile(descriptor, 0); !normalized) {
        static_cast<void>(::close(descriptor));
        return fail(StoreErrorCode::OpenFailed, normalized.error());
    }
    qsizetype offset{};
    while (offset < database_initialization_payload.size()) {
        ssize_t count{};
        do {
            count = ::pwrite(
                descriptor, database_initialization_payload.data() + offset,
                static_cast<std::size_t>(database_initialization_payload.size() - offset), offset);
        } while (count < 0 && errno == EINTR);
        if (count <= 0) {
            const auto message = systemError(u"Write database initialization marker");
            static_cast<void>(::close(descriptor));
            return fail(StoreErrorCode::OpenFailed, message);
        }
        offset += count;
    }
    if (::fsync(descriptor) != 0) {
        const auto message = systemError(u"Flush database initialization marker");
        static_cast<void>(::close(descriptor));
        return fail(StoreErrorCode::OpenFailed, message);
    }
    const auto marker_name = databaseInitializationMarkerName(database_name);
    if (::linkat(descriptor, "", parent_descriptor, marker_name.constData(), AT_EMPTY_PATH) != 0) {
        const auto link_errno = errno;
        static_cast<void>(::close(descriptor));
        if (link_errno == EEXIST) {
            const auto raced = openDatabaseInitializationMarker(parent_descriptor, database_name);
            if (!raced || !raced->has_value()) {
                return raced ? fail(StoreErrorCode::OpenFailed,
                                    QStringLiteral("Database initialization marker race is unsafe"))
                             : std::unexpected(raced.error());
            }
            static_cast<void>(::close(**raced));
            return {};
        }
        errno = link_errno;
        return fail(StoreErrorCode::OpenFailed,
                    systemError(u"Publish database initialization marker"));
    }
    const auto validated =
        validateDatabaseInitializationMarker(descriptor, parent_descriptor, marker_name);
    static_cast<void>(::close(descriptor));
    if (!validated) {
        return std::unexpected(validated.error());
    }
    if (::fsync(parent_descriptor) != 0) {
        return fail(StoreErrorCode::OpenFailed,
                    systemError(u"Flush published database initialization marker"));
    }
    return {};
#endif
}

[[nodiscard]] auto clearDatabaseInitializationMarker(int parent_descriptor,
                                                     const QByteArray& database_name)
    -> std::expected<void, StoreError> {
    const auto marker = openDatabaseInitializationMarker(parent_descriptor, database_name);
    if (!marker) {
        return std::unexpected(marker.error());
    }
    if (!marker->has_value()) {
        return {};
    }
    const auto marker_name = databaseInitializationMarkerName(database_name);
    const auto quarantined = quarantineFreshRegularFile(**marker, parent_descriptor, marker_name);
    static_cast<void>(::close(**marker));
    if (!quarantined) {
        return fail(StoreErrorCode::OpenFailed,
                    QStringLiteral("Cannot retire database initialization marker"));
    }
    return {};
}

[[nodiscard]] auto recoverInterruptedDatabaseInitialization(DatabasePreflight& preflight)
    -> std::expected<bool, StoreError> {
    if (!preflight.cooperative_lock || preflight.cooperative_lock->descriptor < 0) {
        return false;
    }
    const auto marker =
        openDatabaseInitializationMarker(preflight.parent_descriptor, preflight.database_name);
    if (!marker) {
        return std::unexpected(marker.error());
    }
    if (!marker->has_value()) {
        return false;
    }
    static_cast<void>(::close(**marker));
    for (const auto& name : {
             preflight.database_name + QByteArrayLiteral("-journal"),
             preflight.database_name + QByteArrayLiteral("-wal"),
             preflight.database_name + QByteArrayLiteral("-shm"),
             preflight.database_name,
         }) {
        auto flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        const auto descriptor = ::openat(preflight.parent_descriptor, name.constData(), flags);
        if (descriptor < 0 && errno == ENOENT) {
            continue;
        }
        if (descriptor < 0 ||
            !sameOwnedRegularFileBinding(descriptor, preflight.parent_descriptor, name)) {
            if (descriptor >= 0) {
                static_cast<void>(::close(descriptor));
            }
            return fail(StoreErrorCode::OpenFailed,
                        QStringLiteral("Interrupted database initialization residue is unsafe"));
        }
        const auto quarantined =
            quarantineFreshRegularFile(descriptor, preflight.parent_descriptor, name);
        static_cast<void>(::close(descriptor));
        if (!quarantined) {
            return fail(StoreErrorCode::OpenFailed,
                        QStringLiteral("Cannot quarantine interrupted database initialization"));
        }
    }
    if (::fsync(preflight.parent_descriptor) != 0) {
        return fail(StoreErrorCode::OpenFailed,
                    systemError(u"Flush interrupted database initialization recovery"));
    }
    if (const auto cleared =
            clearDatabaseInitializationMarker(preflight.parent_descriptor, preflight.database_name);
        !cleared) {
        return std::unexpected(cleared.error());
    }
    return true;
}

[[nodiscard]] auto identityAt(int parent_descriptor, const QByteArray& name)
    -> std::expected<std::optional<DatabasePreflight::Identity>, StoreError> {
    auto flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const auto descriptor = ::openat(parent_descriptor, name.constData(), flags);
    if (descriptor < 0) {
        if (errno == ENOENT) {
            return std::optional<DatabasePreflight::Identity>{};
        }
        return fail(StoreErrorCode::OpenFailed, systemError(u"Inspect database file"));
    }
    if (const auto private_file =
            detail::validatePrivateStateFileBinding(descriptor, parent_descriptor, name);
        !private_file) {
        static_cast<void>(::close(descriptor));
        return fail(StoreErrorCode::OpenFailed,
                    QStringLiteral("Database or sidecar permissions are unsafe: %1")
                        .arg(private_file.error()));
    }
    struct stat status{};
    if (::fstat(descriptor, &status) != 0) {
        const auto message = systemError(u"Inspect private database file");
        static_cast<void>(::close(descriptor));
        return fail(StoreErrorCode::OpenFailed, message);
    }
    static_cast<void>(::close(descriptor));
    return std::optional<DatabasePreflight::Identity>{identityFromStat(status)};
}

[[nodiscard]] auto openAnchoredFile(int parent_descriptor, const QByteArray& name)
    -> std::expected<int, StoreError> {
    auto flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const auto descriptor = ::openat(parent_descriptor, name.constData(), flags);
    if (descriptor < 0) {
        return fail(StoreErrorCode::OpenFailed, systemError(u"Open database snapshot source"));
    }
    if (const auto private_file =
            detail::validatePrivateStateFileBinding(descriptor, parent_descriptor, name);
        !private_file) {
        static_cast<void>(::close(descriptor));
        return fail(StoreErrorCode::OpenFailed, private_file.error());
    }
    return descriptor;
}

[[nodiscard]] auto acquireDatabaseOpenLock(int parent_descriptor,
                                           const QByteArray& database_name,
                                           const QString& database_path)
    -> std::expected<std::shared_ptr<CooperativeDatabaseLock>, StoreError> {
    QMutexLocker registry_guard(&cooperative_lock_registry_mutex);
    if (const auto existing = cooperative_lock_registry.value(database_path).lock()) {
        Q_UNUSED(existing);
        return fail(StoreErrorCode::StateInUse,
                    QStringLiteral("Session state is already open in this process"));
    }
    const auto lock_name = QByteArray(".") + database_name + QByteArray(".appellate-open.lock");
    auto flags = O_RDWR | O_NOFOLLOW | O_NONBLOCK;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    auto descriptor = ::openat(parent_descriptor, lock_name.constData(), flags);
    if (descriptor < 0 && errno == ENOENT) {
#if defined(Q_OS_LINUX) && defined(O_TMPFILE) && defined(AT_EMPTY_PATH)
        auto temporary_flags = O_TMPFILE | O_RDWR | O_NONBLOCK;
#ifdef O_CLOEXEC
        temporary_flags |= O_CLOEXEC;
#endif
        descriptor = ::openat(parent_descriptor, ".", temporary_flags, 0600);
        if (descriptor < 0) {
            return fail(StoreErrorCode::OpenFailed,
                        systemError(u"Create staged database preflight lock"));
        }
        if (const auto private_file = detail::normalizeNewPrivateStateFile(descriptor, 0);
            !private_file) {
            static_cast<void>(::close(descriptor));
            return fail(StoreErrorCode::OpenFailed, private_file.error());
        }
        int synced{};
        do {
            synced = ::fsync(descriptor);
        } while (synced != 0 && errno == EINTR);
        if (synced != 0) {
            const auto message = systemError(u"Flush staged database preflight lock");
            static_cast<void>(::close(descriptor));
            return fail(StoreErrorCode::OpenFailed, message);
        }
        int linked{};
        do {
            linked =
                ::linkat(descriptor, "", parent_descriptor, lock_name.constData(), AT_EMPTY_PATH);
        } while (linked != 0 && errno == EINTR);
        if (linked == 0) {
            do {
                synced = ::fsync(parent_descriptor);
            } while (synced != 0 && errno == EINTR);
            if (synced != 0) {
                const auto message = systemError(u"Flush published database preflight lock");
                static_cast<void>(::close(descriptor));
                return fail(StoreErrorCode::OpenFailed, message);
            }
        } else {
            const auto link_errno = errno;
            static_cast<void>(::close(descriptor));
            if (link_errno != EEXIST) {
                errno = link_errno;
                return fail(StoreErrorCode::OpenFailed,
                            systemError(u"Publish database preflight lock"));
            }
            descriptor = ::openat(parent_descriptor, lock_name.constData(), flags);
        }
#else
        return fail(StoreErrorCode::OpenFailed,
                    QStringLiteral("Atomic database preflight locks are unavailable"));
#endif
    }
    if (descriptor < 0) {
        return fail(StoreErrorCode::OpenFailed,
                    systemError(u"Open database preflight lock"));
    }
    const auto private_file = detail::validatePrivateStateFileDescriptor(descriptor, 1);
    if (!private_file ||
        !detail::validatePrivateStateFileBinding(descriptor, parent_descriptor, lock_name)) {
        static_cast<void>(::close(descriptor));
        return fail(StoreErrorCode::OpenFailed,
                    private_file ? QStringLiteral("Database preflight lock binding is unsafe")
                                 : private_file.error());
    }
    int flock_result{};
    do {
        flock_result = ::flock(descriptor, LOCK_EX | LOCK_NB);
    } while (flock_result != 0 && errno == EINTR);
    if (flock_result != 0) {
        const auto lock_errno = errno;
        // Once published, even a lock created by this attempt may have been opened or acquired by
        // another process. No flock-failure branch may detach that process's coordination name.
        static_cast<void>(::close(descriptor));
        if (lock_errno == EACCES || lock_errno == EAGAIN) {
            return fail(StoreErrorCode::StateInUse,
                        QStringLiteral(
                            "Session state is already open by another Appellate process"));
        }
        errno = lock_errno;
        return fail(StoreErrorCode::OpenFailed,
                    systemError(u"Acquire database preflight lock"));
    }
    if (const auto private_lock =
            detail::validatePrivateStateFileBinding(descriptor, parent_descriptor, lock_name);
        !private_lock) {
        static_cast<void>(::flock(descriptor, LOCK_UN));
        static_cast<void>(::close(descriptor));
        return fail(StoreErrorCode::OpenFailed,
                    QStringLiteral("Database preflight lock changed while acquiring it: %1")
                        .arg(private_lock.error()));
    }
    struct stat locked_status {};
    struct stat named_status {};
    if (::fstat(descriptor, &locked_status) != 0 ||
        ::fstatat(parent_descriptor, lock_name.constData(), &named_status,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        locked_status.st_dev != named_status.st_dev ||
        locked_status.st_ino != named_status.st_ino || !S_ISREG(named_status.st_mode) ||
        locked_status.st_nlink != 1 || named_status.st_nlink != 1) {
        static_cast<void>(::flock(descriptor, LOCK_UN));
        static_cast<void>(::close(descriptor));
        return fail(StoreErrorCode::OpenFailed,
                    QStringLiteral("Database preflight lock identity changed"));
    }
    auto held = std::make_shared<CooperativeDatabaseLock>();
    held->descriptor = descriptor;
    cooperative_lock_registry.insert(database_path, held);
    return held;
}

[[nodiscard]] auto copyDescriptor(int descriptor, const QString& destination)
    -> std::expected<QByteArray, StoreError> {
    QFile output(destination);
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        return fail(StoreErrorCode::OpenFailed,
                    QStringLiteral("Cannot create private database snapshot"));
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::array<char, backup_buffer_bytes> buffer{};
    off_t offset{};
    while (true) {
        ssize_t count{};
        do {
            count = ::pread(descriptor, buffer.data(), buffer.size(), offset);
        } while (count < 0 && errno == EINTR);
        if (count < 0) {
            return fail(StoreErrorCode::OpenFailed, systemError(u"Read database snapshot"));
        }
        if (count == 0) {
            break;
        }
        if (output.write(buffer.data(), count) != count) {
            return fail(StoreErrorCode::OpenFailed,
                        QStringLiteral("Cannot write private database snapshot"));
        }
        hash.addData(QByteArrayView(buffer.data(), count));
        offset += count;
    }
    output.close();
    return hash.result();
}

[[nodiscard]] auto hashDescriptor(int descriptor) -> std::expected<QByteArray, StoreError> {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::array<char, backup_buffer_bytes> buffer{};
    off_t offset{};
    while (true) {
        ssize_t count{};
        do {
            count = ::pread(descriptor, buffer.data(), buffer.size(), offset);
        } while (count < 0 && errno == EINTR);
        if (count < 0) {
            return fail(StoreErrorCode::OpenFailed,
                        systemError(u"Re-read database snapshot source"));
        }
        if (count == 0) {
            break;
        }
        hash.addData(QByteArrayView(buffer.data(), count));
        offset += count;
    }
    return hash.result();
}

[[nodiscard]] auto descriptorIdentity(int descriptor)
    -> std::expected<DatabasePreflight::Identity, StoreError> {
    struct stat status {};
    if (::fstat(descriptor, &status) != 0) {
        return fail(StoreErrorCode::OpenFailed,
                    systemError(u"Revalidate database snapshot descriptor"));
    }
    return identityFromStat(status);
}

[[nodiscard]] auto reserveFreshSidecar(DatabasePreflight& preflight, QByteArrayView suffix,
                                       std::optional<DatabasePreflight::Identity>& identity)
    -> std::expected<void, StoreError> {
    if (identity.has_value()) {
        return {};
    }
    const auto name = preflight.database_name + suffix;
    auto flags = O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW | O_NONBLOCK;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const auto descriptor = ::openat(preflight.parent_descriptor, name.constData(), flags, 0600);
    if (descriptor < 0) {
        return fail(StoreErrorCode::OpenFailed, systemError(u"Reserve private SQLite sidecar"));
    }
    preflight.fresh_sidecars.emplace_back(name, descriptor);
    if (const auto private_file = detail::normalizeNewPrivateStateFile(descriptor, 1);
        !private_file) {
        return fail(StoreErrorCode::OpenFailed, private_file.error());
    }
    if (const auto bound =
            detail::validatePrivateStateFileBinding(descriptor, preflight.parent_descriptor, name);
        !bound) {
        return fail(StoreErrorCode::OpenFailed, bound.error());
    }
    const auto reserved_identity = descriptorIdentity(descriptor);
    if (!reserved_identity) {
        return std::unexpected(reserved_identity.error());
    }
    identity = *reserved_identity;
    return {};
}

[[nodiscard]] auto reserveFreshSidecars(DatabasePreflight& preflight)
    -> std::expected<void, StoreError> {
    for (auto reservation : {
             std::pair{QByteArrayView("-journal"), &preflight.journal_identity},
             std::pair{QByteArrayView("-wal"), &preflight.wal_identity},
             std::pair{QByteArrayView("-shm"), &preflight.shm_identity},
         }) {
        if (const auto reserved =
                reserveFreshSidecar(preflight, reservation.first, *reservation.second);
            !reserved) {
            return reserved;
        }
    }
    return {};
}

[[nodiscard]] auto revalidatePreflight(const DatabasePreflight& preflight)
    -> std::expected<void, StoreError> {
    if (const auto private_parent =
            detail::validatePrivateStateControllerDescriptor(preflight.parent_descriptor);
        !private_parent) {
        return fail(StoreErrorCode::OpenFailed, private_parent.error());
    }
    const auto current_parent = descriptorIdentity(preflight.parent_descriptor);
    if (!current_parent || current_parent->device != preflight.parent_identity.device ||
        current_parent->inode != preflight.parent_identity.inode) {
        return fail(StoreErrorCode::OpenFailed,
                    QStringLiteral("Database parent changed during private preflight"));
    }
    const auto current_database = identityAt(preflight.parent_descriptor, preflight.database_name);
    const auto current_wal =
        identityAt(preflight.parent_descriptor, preflight.database_name + "-wal");
    const auto current_shm =
        identityAt(preflight.parent_descriptor, preflight.database_name + "-shm");
    const auto current_journal =
        identityAt(preflight.parent_descriptor, preflight.database_name + "-journal");
    if (!current_database || !current_wal || !current_shm || !current_journal) {
        return std::unexpected((!current_database
                                    ? current_database.error()
                                    : !current_wal
                                          ? current_wal.error()
                                          : !current_shm ? current_shm.error()
                                                         : current_journal.error()));
    }
    if (*current_database != preflight.database_identity ||
        *current_wal != preflight.wal_identity || *current_shm != preflight.shm_identity ||
        *current_journal != preflight.journal_identity) {
        return fail(StoreErrorCode::OpenFailed,
                    QStringLiteral(
                        "Database, WAL, SHM, or rollback journal changed during preflight"));
    }
    return {};
}

[[nodiscard]] auto revalidateOpenedDatabaseIdentity(const DatabasePreflight& preflight)
    -> std::expected<void, StoreError> {
    if (const auto private_parent =
            detail::validatePrivateStateControllerDescriptor(preflight.parent_descriptor);
        !private_parent) {
        return fail(StoreErrorCode::OpenFailed, private_parent.error());
    }
    const auto current_parent = descriptorIdentity(preflight.parent_descriptor);
    if (!current_parent || current_parent->device != preflight.parent_identity.device ||
        current_parent->inode != preflight.parent_identity.inode) {
        return fail(StoreErrorCode::OpenFailed,
                    QStringLiteral("Anchored database parent identity changed during open"));
    }
    const auto current = identityAt(preflight.parent_descriptor, preflight.database_name);
    if (!current || !current->has_value()) {
        return fail(StoreErrorCode::OpenFailed,
                    QStringLiteral("Opened database path no longer names a regular file"));
    }
    if (preflight.database_identity.has_value() &&
        ((*current)->device != preflight.database_identity->device ||
         (*current)->inode != preflight.database_identity->inode)) {
        return fail(StoreErrorCode::OpenFailed,
                    QStringLiteral("Opened database identity changed after private preflight"));
    }
    return {};
}

[[nodiscard]] auto validateFreshSidecarsAfterOpen(const DatabasePreflight& preflight)
    -> std::expected<void, StoreError> {
    for (const auto& sidecar : preflight.fresh_sidecars) {
        const auto current = identityAt(preflight.parent_descriptor, sidecar.name);
        if (!current) {
            return std::unexpected(current.error());
        }
        if (current->has_value() &&
            !detail::validatePrivateStateFileBinding(sidecar.descriptor,
                                                     preflight.parent_descriptor, sidecar.name)) {
            return fail(StoreErrorCode::OpenFailed,
                        QStringLiteral("SQLite replaced a sidecar reserved by this open attempt"));
        }
    }
    return {};
}

[[nodiscard]] auto revalidateLifetimeLease(const DatabasePreflight& preflight)
    -> std::expected<void, StoreError> {
    if (!preflight.cooperative_lock || preflight.parent_descriptor < 0 ||
        preflight.database_anchor_descriptor < 0 || preflight.cooperative_lock->descriptor < 0 ||
        !preflight.database_identity.has_value()) {
        return fail(StoreErrorCode::OpenFailed,
                    QStringLiteral("SessionStore lifetime lease is incomplete"));
    }
    if (const auto private_parent =
            detail::validatePrivateStateControllerDescriptor(preflight.parent_descriptor);
        !private_parent) {
        return fail(StoreErrorCode::OpenFailed, private_parent.error());
    }
    const auto current_parent = descriptorIdentity(preflight.parent_descriptor);
    if (!current_parent || current_parent->device != preflight.parent_identity.device ||
        current_parent->inode != preflight.parent_identity.inode) {
        return fail(StoreErrorCode::OpenFailed,
                    QStringLiteral("Anchored database parent changed during owner lifetime"));
    }
    const auto lock_name =
        QByteArray(".") + preflight.database_name + QByteArray(".appellate-open.lock");
    if (const auto private_lock = detail::validatePrivateStateFileBinding(
            preflight.cooperative_lock->descriptor, preflight.parent_descriptor, lock_name);
        !private_lock) {
        return fail(StoreErrorCode::OpenFailed,
                    QStringLiteral("Database lifetime lock changed: %1").arg(private_lock.error()));
    }
    struct stat anchored_main {};
    if (::fstat(preflight.database_anchor_descriptor, &anchored_main) != 0 ||
        !S_ISREG(anchored_main.st_mode) || anchored_main.st_nlink != 1 ||
        anchored_main.st_dev != preflight.database_identity->device ||
        anchored_main.st_ino != preflight.database_identity->inode) {
        return fail(StoreErrorCode::OpenFailed,
                    QStringLiteral("Retained database inode changed during owner lifetime"));
    }
    const auto named_main = identityAt(preflight.parent_descriptor, preflight.database_name);
    const auto wal = identityAt(preflight.parent_descriptor, preflight.database_name + "-wal");
    const auto shm = identityAt(preflight.parent_descriptor, preflight.database_name + "-shm");
    const auto journal =
        identityAt(preflight.parent_descriptor, preflight.database_name + "-journal");
    if (!named_main || !wal || !shm || !journal) {
        return std::unexpected(!named_main
                                   ? named_main.error()
                                   : !wal ? wal.error()
                                          : !shm ? shm.error() : journal.error());
    }
    if (!named_main->has_value() || (*named_main)->device != anchored_main.st_dev ||
        (*named_main)->inode != anchored_main.st_ino) {
        return fail(StoreErrorCode::OpenFailed,
                    QStringLiteral("Database pathname no longer names the retained inode"));
    }
    // Sidecar existence, size, and inode may change under our own SQLite connections. identityAt
    // still proves that every sidecar currently present is a single-link no-follow regular file.
    return {};
}
#endif

[[nodiscard]] auto validatePrivateDatabase(const QString& path, bool full_integrity)
    -> std::expected<void, StoreError> {
    const auto connection =
        QStringLiteral("appellate-preflight-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    std::expected<void, StoreError> result;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(path);
        if (!database.open()) {
            result = fail(StoreErrorCode::MigrationFailed, database.lastError().text());
        } else {
            if (full_integrity) {
                QSqlQuery integrity(database);
                if (!integrity.exec(QStringLiteral("PRAGMA integrity_check")) ||
                    !integrity.next() ||
                    integrity.value(0).toString() != QStringLiteral("ok")) {
                    result = queryFailure(StoreErrorCode::MigrationFailed, integrity,
                                          QStringLiteral("verify private database integrity"));
                }
            }
            if (result.has_value()) {
                result = validateApplicationSchema(database, true);
            }
            database.close();
        }
        database = QSqlDatabase{};
    }
    QSqlDatabase::removeDatabase(connection);
    return result;
}

[[nodiscard]] auto preflightExistingDatabase(const QString& path,
                                              bool full_integrity = true,
                                              bool require_standalone = false,
                                              bool acquire_cooperative_lock = true,
                                              std::shared_ptr<void> already_held_lock = {})
    -> std::expected<DatabasePreflight, StoreError> {
    if (path.isEmpty() || path.contains(QChar::Null)) {
        return fail(StoreErrorCode::InvalidArgument,
                    QStringLiteral("Database path is empty or contains NUL"));
    }
    DatabasePreflight preflight;
    preflight.original_path = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    const QFileInfo database_info(preflight.original_path);
#if defined(Q_OS_UNIX)
    const auto parent_path = QDir::cleanPath(database_info.absolutePath());
    const QFileInfo parent_info(parent_path);
    if (!parent_info.exists() || !parent_info.isDir() || parent_info.isSymbolicLink() ||
        QDir::cleanPath(parent_info.canonicalFilePath()) != parent_path) {
        return fail(StoreErrorCode::OpenFailed,
                    QStringLiteral("Database parent must be an existing canonical directory"));
    }
    const auto anchored_parent = openAbsoluteDirectoryNoFollow(parent_path);
    if (!anchored_parent) {
        return std::unexpected(anchored_parent.error());
    }
    preflight.parent_descriptor = *anchored_parent;
    const auto parent_identity = descriptorIdentity(preflight.parent_descriptor);
    if (!parent_identity) {
        return std::unexpected(parent_identity.error());
    }
    preflight.parent_identity = *parent_identity;
    preflight.cooperative_lock =
        std::static_pointer_cast<CooperativeDatabaseLock>(std::move(already_held_lock));
    preflight.database_name = QFile::encodeName(database_info.fileName());
    if (preflight.database_name.isEmpty() || preflight.database_name.contains('\0')) {
        return fail(StoreErrorCode::InvalidArgument, QStringLiteral("Database filename is invalid"));
    }
    const auto existing_lock_name =
        QByteArray(".") + preflight.database_name + QByteArray(".appellate-open.lock");
    const auto existing_lock_identity = identityAt(preflight.parent_descriptor, existing_lock_name);
    if (!existing_lock_identity) {
        return std::unexpected(existing_lock_identity.error());
    }
    const auto existing_lock = existing_lock_identity->has_value();
    const auto initialization_marker =
        openDatabaseInitializationMarker(preflight.parent_descriptor, preflight.database_name);
    if (!initialization_marker) {
        return std::unexpected(initialization_marker.error());
    }
    const auto interrupted_initialization = initialization_marker->has_value();
    if (interrupted_initialization) {
        static_cast<void>(::close(**initialization_marker));
    }
    if (interrupted_initialization && !acquire_cooperative_lock) {
        return fail(
            StoreErrorCode::OpenFailed,
            QStringLiteral("Interrupted database initialization requires an exclusive owner"));
    }
    if (acquire_cooperative_lock && existing_lock && !preflight.cooperative_lock) {
        const auto cooperative_lock = acquireDatabaseOpenLock(
            preflight.parent_descriptor, preflight.database_name, preflight.original_path);
        if (!cooperative_lock) {
            return std::unexpected(cooperative_lock.error());
        }
        preflight.cooperative_lock = *cooperative_lock;
    }
    if (acquire_cooperative_lock && interrupted_initialization && !preflight.cooperative_lock) {
        const auto cooperative_lock = acquireDatabaseOpenLock(
            preflight.parent_descriptor, preflight.database_name, preflight.original_path);
        if (!cooperative_lock) {
            return std::unexpected(cooperative_lock.error());
        }
        preflight.cooperative_lock = *cooperative_lock;
    }
    if (acquire_cooperative_lock && preflight.cooperative_lock) {
        const auto recovered = recoverInterruptedDatabaseInitialization(preflight);
        if (!recovered) {
            return std::unexpected(recovered.error());
        }
        if (*recovered) {
            return preflightExistingDatabase(path, full_integrity, require_standalone,
                                             acquire_cooperative_lock,
                                             std::move(preflight.cooperative_lock));
        }
    }
    const auto initial_database =
        identityAt(preflight.parent_descriptor, preflight.database_name);
    const auto initial_wal =
        identityAt(preflight.parent_descriptor, preflight.database_name + "-wal");
    const auto initial_shm =
        identityAt(preflight.parent_descriptor, preflight.database_name + "-shm");
    const auto initial_journal =
        identityAt(preflight.parent_descriptor, preflight.database_name + "-journal");
    if (!initial_database || !initial_wal || !initial_shm || !initial_journal) {
        return std::unexpected(!initial_database
                                   ? initial_database.error()
                                   : !initial_wal
                                         ? initial_wal.error()
                                         : !initial_shm ? initial_shm.error()
                                                        : initial_journal.error());
    }
    preflight.database_identity = *initial_database;
    preflight.wal_identity = *initial_wal;
    preflight.shm_identity = *initial_shm;
    preflight.journal_identity = *initial_journal;

    const auto transient_fresh_state =
        (preflight.database_identity.has_value() && preflight.database_identity->size == 0) ||
        ((!preflight.database_identity.has_value() || preflight.database_identity->size == 0) &&
         (preflight.wal_identity.has_value() || preflight.shm_identity.has_value() ||
          preflight.journal_identity.has_value()));
    if (acquire_cooperative_lock && !preflight.cooperative_lock && transient_fresh_state) {
        // A fresh winner may have created its lock just after our first lock inspection and then
        // reserved the zero-byte main/sidecars we observed. Recheck without creating anything;
        // if the lock appeared, either report its active owner or restart validation while holding
        // it. This keeps a concurrent first open bounded without weakening corrupt-file rejection.
        const auto raced_lock = identityAt(preflight.parent_descriptor, existing_lock_name);
        if (!raced_lock) {
            return std::unexpected(raced_lock.error());
        }
        if (raced_lock->has_value()) {
            const auto cooperative_lock = acquireDatabaseOpenLock(
                preflight.parent_descriptor, preflight.database_name, preflight.original_path);
            if (!cooperative_lock) {
                return std::unexpected(cooperative_lock.error());
            }
            return preflightExistingDatabase(path, full_integrity, require_standalone,
                                             acquire_cooperative_lock, *cooperative_lock);
        }
    }

    if (preflight.database_identity.has_value() && preflight.database_identity->size == 0) {
        return fail(StoreErrorCode::MigrationFailed,
                    QStringLiteral("A pre-existing zero-byte database is not an Appellate database"));
    }

    if ((!preflight.database_identity.has_value() ||
         preflight.database_identity->size == 0) &&
        (preflight.wal_identity.has_value() || preflight.shm_identity.has_value() ||
         preflight.journal_identity.has_value())) {
        return fail(StoreErrorCode::OpenFailed,
                    QStringLiteral("Database sidecars exist without a nonempty main database"));
    }

    if (require_standalone &&
        (preflight.wal_identity.has_value() || preflight.shm_identity.has_value() ||
         preflight.journal_identity.has_value())) {
        return fail(StoreErrorCode::OpenFailed,
                    QStringLiteral("Standalone database must not have WAL, SHM, or journal sidecars"));
    }

    if (preflight.database_identity.has_value() && preflight.database_identity->size > 0) {
        auto private_directory = std::make_unique<QTemporaryDir>();
        if (!private_directory->isValid()) {
            return fail(StoreErrorCode::OpenFailed,
                        QStringLiteral("Cannot create private database preflight directory"));
        }
        const auto private_database =
            QDir(private_directory->path()).filePath(database_info.fileName());
        const auto database_descriptor =
            openAnchoredFile(preflight.parent_descriptor, preflight.database_name);
        if (!database_descriptor) {
            return std::unexpected(database_descriptor.error());
        }
        const auto copied_database = copyDescriptor(*database_descriptor, private_database);
        if (!copied_database) {
            static_cast<void>(::close(*database_descriptor));
            return std::unexpected(copied_database.error());
        }

        std::optional<int> wal_descriptor;
        std::optional<QByteArray> copied_wal;
        if (preflight.wal_identity.has_value()) {
            const auto opened_wal =
                openAnchoredFile(preflight.parent_descriptor, preflight.database_name + "-wal");
            if (!opened_wal) {
                static_cast<void>(::close(*database_descriptor));
                return std::unexpected(opened_wal.error());
            }
            wal_descriptor = *opened_wal;
            const auto wal_copy = copyDescriptor(
                *wal_descriptor, private_database + QStringLiteral("-wal"));
            if (!wal_copy) {
                static_cast<void>(::close(*database_descriptor));
                static_cast<void>(::close(*wal_descriptor));
                return std::unexpected(wal_copy.error());
            }
            copied_wal = *wal_copy;
        }

        std::optional<int> journal_descriptor;
        std::optional<QByteArray> copied_journal;
        if (preflight.journal_identity.has_value()) {
            const auto opened_journal = openAnchoredFile(
                preflight.parent_descriptor, preflight.database_name + "-journal");
            if (!opened_journal) {
                static_cast<void>(::close(*database_descriptor));
                if (wal_descriptor.has_value()) {
                    static_cast<void>(::close(*wal_descriptor));
                }
                return std::unexpected(opened_journal.error());
            }
            journal_descriptor = *opened_journal;
            const auto journal_copy = copyDescriptor(
                *journal_descriptor, private_database + QStringLiteral("-journal"));
            if (!journal_copy) {
                static_cast<void>(::close(*database_descriptor));
                if (wal_descriptor.has_value()) {
                    static_cast<void>(::close(*wal_descriptor));
                }
                static_cast<void>(::close(*journal_descriptor));
                return std::unexpected(journal_copy.error());
            }
            copied_journal = *journal_copy;
        }

        const auto second_database_hash = hashDescriptor(*database_descriptor);
        const auto second_database_identity = descriptorIdentity(*database_descriptor);
        static_cast<void>(::close(*database_descriptor));
        if (!second_database_hash || !second_database_identity ||
            *copied_database != *second_database_hash ||
            *second_database_identity != *preflight.database_identity) {
            if (wal_descriptor.has_value()) {
                static_cast<void>(::close(*wal_descriptor));
            }
            if (journal_descriptor.has_value()) {
                static_cast<void>(::close(*journal_descriptor));
            }
            return fail(StoreErrorCode::OpenFailed,
                        QStringLiteral("Database changed while making private preflight copy"));
        }
        if (wal_descriptor.has_value()) {
            const auto second_wal_hash = hashDescriptor(*wal_descriptor);
            const auto second_wal_identity = descriptorIdentity(*wal_descriptor);
            static_cast<void>(::close(*wal_descriptor));
            if (!second_wal_hash || !second_wal_identity || !copied_wal.has_value() ||
                *copied_wal != *second_wal_hash ||
                *second_wal_identity != *preflight.wal_identity) {
                if (journal_descriptor.has_value()) {
                    static_cast<void>(::close(*journal_descriptor));
                }
                return fail(StoreErrorCode::OpenFailed,
                            QStringLiteral("WAL changed while making private preflight copy"));
            }
        }
        if (journal_descriptor.has_value()) {
            const auto second_journal_hash = hashDescriptor(*journal_descriptor);
            const auto second_journal_identity = descriptorIdentity(*journal_descriptor);
            static_cast<void>(::close(*journal_descriptor));
            if (!second_journal_hash || !second_journal_identity ||
                !copied_journal.has_value() || *copied_journal != *second_journal_hash ||
                *second_journal_identity != *preflight.journal_identity) {
                return fail(
                    StoreErrorCode::OpenFailed,
                    QStringLiteral("Rollback journal changed while making private preflight copy"));
            }
        }

        QFile header(private_database);
        if (!header.open(QIODevice::ReadOnly) || header.size() < 100 ||
            header.read(16) != QByteArray("SQLite format 3\0", 16)) {
            return fail(StoreErrorCode::MigrationFailed,
                        QStringLiteral("Existing database has no valid SQLite header"));
        }
        if (const auto validated = validatePrivateDatabase(private_database, full_integrity);
            !validated) {
            return std::unexpected(validated.error());
        }
        preflight.private_snapshot_path = private_database;
        preflight.private_snapshot_directory = std::move(private_directory);
    }

    // The lock is deliberately acquired only after a caller-controlled existing database has
    // passed private validation, so rejection never creates a lock artifact. Every successfully
    // opened SessionStore retains the shared in-process ownership object for its full lifetime;
    // the advisory lock coordinates only Appellate processes, not SQLite's own locking protocol.
    if (acquire_cooperative_lock && !preflight.cooperative_lock) {
        const auto cooperative_lock = acquireDatabaseOpenLock(
            preflight.parent_descriptor, preflight.database_name, preflight.original_path);
        if (!cooperative_lock) {
            return std::unexpected(cooperative_lock.error());
        }
        preflight.cooperative_lock = *cooperative_lock;

        // A cooperative owner can finish a first open between our initial no-lock snapshot and
        // lock acquisition. Refresh all validation under the retained lock rather than using
        // stale absence or identities. The recursive pass does not reacquire the lock.
        const auto current_database =
            identityAt(preflight.parent_descriptor, preflight.database_name);
        const auto current_wal =
            identityAt(preflight.parent_descriptor, preflight.database_name + "-wal");
        const auto current_shm =
            identityAt(preflight.parent_descriptor, preflight.database_name + "-shm");
        const auto current_journal =
            identityAt(preflight.parent_descriptor, preflight.database_name + "-journal");
        if (!current_database || !current_wal || !current_shm || !current_journal) {
            return std::unexpected(!current_database
                                       ? current_database.error()
                                       : !current_wal
                                             ? current_wal.error()
                                             : !current_shm ? current_shm.error()
                                                            : current_journal.error());
        }
        if (*current_database != preflight.database_identity ||
            *current_wal != preflight.wal_identity ||
            *current_shm != preflight.shm_identity ||
            *current_journal != preflight.journal_identity) {
            return preflightExistingDatabase(path, full_integrity, require_standalone,
                                             acquire_cooperative_lock,
                                             std::move(preflight.cooperative_lock));
        }
    }

    if (!preflight.database_identity.has_value()) {
        if (acquire_cooperative_lock) {
            if (!preflight.cooperative_lock) {
                return fail(StoreErrorCode::OpenFailed,
                            QStringLiteral("Fresh database initialization has no exclusive owner"));
            }
            if (const auto marker = publishDatabaseInitializationMarker(preflight.parent_descriptor,
                                                                        preflight.database_name);
                !marker) {
                return std::unexpected(marker.error());
            }
        }
        auto flags = O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        const auto descriptor =
            ::openat(preflight.parent_descriptor, preflight.database_name.constData(), flags, 0600);
        if (descriptor < 0) {
            return fail(StoreErrorCode::OpenFailed,
                        systemError(u"Atomically reserve new session database"));
        }
        preflight.database_anchor_descriptor = descriptor;
        preflight.created_by_this_open = true;
        if (const auto private_file = detail::normalizeNewPrivateStateFile(descriptor, 1);
            !private_file) {
            return fail(StoreErrorCode::OpenFailed, private_file.error());
        }
        if (const auto private_binding = detail::validatePrivateStateFileBinding(
                descriptor, preflight.parent_descriptor, preflight.database_name);
            !private_binding) {
            return fail(StoreErrorCode::OpenFailed, private_binding.error());
        }
        const auto created_identity = descriptorIdentity(descriptor);
        if (!created_identity) {
            return std::unexpected(created_identity.error());
        }
        preflight.database_identity = *created_identity;
    } else {
        const auto anchored = openAnchoredFile(preflight.parent_descriptor,
                                               preflight.database_name);
        if (!anchored) {
            return std::unexpected(anchored.error());
        }
        const auto anchored_identity = descriptorIdentity(*anchored);
        if (!anchored_identity || *anchored_identity != *preflight.database_identity) {
            static_cast<void>(::close(*anchored));
            return fail(StoreErrorCode::OpenFailed,
                        QStringLiteral("Database identity changed before writable open"));
        }
        preflight.database_anchor_descriptor = *anchored;
    }
    if (const auto stable = revalidatePreflight(preflight); !stable) {
        return std::unexpected(stable.error());
    }
    if (acquire_cooperative_lock) {
        if (const auto reserved = reserveFreshSidecars(preflight); !reserved) {
            return std::unexpected(reserved.error());
        }
        if (const auto stable = revalidatePreflight(preflight); !stable) {
            return std::unexpected(stable.error());
        }
    }
#if defined(Q_OS_LINUX)
    preflight.sqlite_path =
        QStringLiteral("/proc/self/fd/%1/%2")
            .arg(preflight.parent_descriptor)
            .arg(QString::fromLocal8Bit(preflight.database_name));
#else
    preflight.sqlite_path = preflight.original_path;
#endif
#else
    const QFileInfo information(preflight.original_path);
    if (information.exists() && (!information.isFile() || information.isSymbolicLink())) {
        return fail(StoreErrorCode::OpenFailed,
                    QStringLiteral("Database path must be a regular no-follow file"));
    }
    if (information.exists() && information.size() > 0) {
        QTemporaryDir private_directory;
        const auto private_database =
            QDir(private_directory.path()).filePath(information.fileName());
        if (!private_directory.isValid() ||
            !QFile::copy(preflight.original_path, private_database)) {
            return fail(StoreErrorCode::OpenFailed,
                        QStringLiteral("Cannot make private database preflight copy"));
        }
        if (const auto validated = validatePrivateDatabase(private_database, full_integrity);
            !validated) {
            return std::unexpected(validated.error());
        }
    }
    preflight.sqlite_path = preflight.original_path;
#endif
    return preflight;
}

[[nodiscard]] auto migrationSql(int version) -> std::expected<QByteArray, StoreError> {
    QString migration_name;
    switch (version) {
    case 1:
        migration_name = QStringLiteral("001_initial.sql");
        break;
    case 2:
        migration_name = QStringLiteral("002_session_authority_contract.sql");
        break;
    case 3:
        migration_name = QStringLiteral("003_application_identity.sql");
        break;
    default:
        return fail(StoreErrorCode::MigrationFailed,
                    QStringLiteral("Embedded migration %1 is unknown").arg(version));
    }
    QFile migration(QStringLiteral(":/appellate/storage/migrations/%1").arg(migration_name));
    if (!migration.open(QIODevice::ReadOnly)) {
        return fail(StoreErrorCode::MigrationFailed,
                    QStringLiteral("Embedded migration %1 is unavailable").arg(version));
    }
    constexpr qint64 maximum_migration_bytes = 1024 * 1024;
    if (migration.size() <= 0 || migration.size() > maximum_migration_bytes) {
        return fail(StoreErrorCode::MigrationFailed,
                    QStringLiteral("Embedded migration %1 has an invalid size").arg(version));
    }
    const auto bytes = migration.read(maximum_migration_bytes + 1);
    if (bytes.size() != migration.size()) {
        return fail(StoreErrorCode::MigrationFailed,
                    QStringLiteral("Cannot read embedded migration %1").arg(version));
    }
    return bytes;
}

[[nodiscard]] auto validateSessionCreate(const QString& session_id,
                                         const QString& engine_revision,
                                         const QString& created_at_utc,
                                         const std::vector<RevisionPin>& pins,
                                         SessionAuthorityContract authority_contract)
    -> std::expected<QString, StoreError> {
    const auto authority_contract_name = authorityContractName(authority_contract);
    if (!validText(session_id) || !validText(engine_revision) ||
        !validCanonicalUtc(created_at_utc) || pins.empty() ||
        pins.size() > maximum_session_pins || !authority_contract_name.has_value()) {
        return fail(
            StoreErrorCode::InvalidArgument,
            QStringLiteral("Session identity, engine revision, time, and pins are required"));
    }
    QSet<QString> pinned_pack_ids;
    for (const auto& pin : pins) {
        if (!validText(pin.pack_id) || !validText(pin.version) || !validDigest(pin.digest)) {
            return fail(StoreErrorCode::InvalidArgument, QStringLiteral("Invalid revision pin"));
        }
        if (pinned_pack_ids.contains(pin.pack_id)) {
            return fail(StoreErrorCode::InvalidArgument,
                        QStringLiteral("Duplicate pinned pack revision"));
        }
        pinned_pack_ids.insert(pin.pack_id);
    }
    return *authority_contract_name;
}

[[nodiscard]] auto validateCommit(const QString& session_id, qint64 expected_sequence,
                                  const CommitBatch& batch)
    -> std::expected<qint64, StoreError> {
    if (!validText(session_id) || expected_sequence < 0 || !validText(batch.command_id) ||
        !validJsonObject(batch.command_json) || !validCanonicalUtc(batch.recorded_at_utc) ||
        batch.events.empty() || batch.events.size() > maximum_events_per_batch ||
        batch.docket_changes.size() > maximum_docket_changes_per_batch) {
        return fail(StoreErrorCode::InvalidArgument, QStringLiteral("Invalid commit batch"));
    }
    const auto event_count = static_cast<qint64>(batch.events.size());
    if (expected_sequence > std::numeric_limits<qint64>::max() - event_count) {
        return fail(StoreErrorCode::InvalidArgument,
                    QStringLiteral("Commit batch sequence would overflow"));
    }
    for (const auto& event : batch.events) {
        if (!validText(event.event_type) || !validJsonObject(event.payload_json) ||
            !validText(event.authority_id)) {
            return fail(StoreErrorCode::InvalidArgument, QStringLiteral("Invalid event"));
        }
    }
    QSet<QString> docket_entry_ids;
    for (const auto& entry : batch.docket_changes) {
        if (!validText(entry.entry_id) || !validText(entry.title) || !validText(entry.status) ||
            entry.source_event_offset < 0 ||
            entry.source_event_offset >= static_cast<qsizetype>(batch.events.size()) ||
            docket_entry_ids.contains(entry.entry_id)) {
            return fail(StoreErrorCode::InvalidArgument, QStringLiteral("Invalid docket change"));
        }
        docket_entry_ids.insert(entry.entry_id);
    }
    if (batch.asset_references.size() > maximum_asset_references_per_batch) {
        return fail(StoreErrorCode::InvalidArgument, QStringLiteral("Too many asset references"));
    }
    QSet<QString> asset_reference_keys;
    for (const auto& reference : batch.asset_references) {
        if (!validDigest(reference.digest) || !validAssetPurpose(reference.purpose)) {
            return fail(StoreErrorCode::InvalidArgument, QStringLiteral("Invalid asset reference"));
        }
        const auto key = reference.digest + QChar::Null + reference.purpose;
        if (asset_reference_keys.contains(key)) {
            return fail(StoreErrorCode::InvalidArgument,
                        QStringLiteral("Duplicate asset reference in commit batch"));
        }
        asset_reference_keys.insert(key);
    }
    return event_count;
}

[[nodiscard]] auto insertSessionRows(QSqlDatabase& database, const QString& session_id,
                                     const QString& engine_revision,
                                     const QString& created_at_utc,
                                     const std::vector<RevisionPin>& pins,
                                     const QString& authority_contract_name)
    -> std::expected<void, StoreError> {
    QSqlQuery session(database);
    session.prepare(QStringLiteral(
        "INSERT INTO sessions(session_id, engine_revision, authority_contract, sequence, "
        "created_at_utc) VALUES(?, ?, ?, 0, ?)"));
    session.addBindValue(session_id);
    session.addBindValue(engine_revision);
    session.addBindValue(authority_contract_name);
    session.addBindValue(created_at_utc);
    if (!session.exec()) {
        return queryFailure(StoreErrorCode::AlreadyExists, session,
                            QStringLiteral("create session"));
    }

    QSqlQuery pin_query(database);
    pin_query.prepare(QStringLiteral(
        "INSERT INTO session_pins(session_id, pack_id, version, digest) VALUES(?, ?, ?, ?)"));
    for (const auto& pin : pins) {
        pin_query.bindValue(0, session_id);
        pin_query.bindValue(1, pin.pack_id);
        pin_query.bindValue(2, pin.version);
        pin_query.bindValue(3, pin.digest);
        if (!pin_query.exec()) {
            return queryFailure(StoreErrorCode::ConstraintViolation, pin_query,
                                QStringLiteral("pin pack revision"));
        }
    }
    return {};
}

[[nodiscard]] auto appendRows(QSqlDatabase& database, const QString& session_id,
                              qint64 expected_sequence, const CommitBatch& batch,
                              qint64 event_count) -> std::expected<qint64, StoreError> {
    QSqlQuery current(database);
    current.prepare(QStringLiteral("SELECT sequence FROM sessions WHERE session_id = ?"));
    current.addBindValue(session_id);
    if (!current.exec()) {
        return queryFailure(StoreErrorCode::QueryFailed, current,
                            QStringLiteral("read session sequence"));
    }
    if (!current.next()) {
        return fail(StoreErrorCode::NotFound, QStringLiteral("Session not found"));
    }
    if (current.value(0).toLongLong() != expected_sequence) {
        return fail(StoreErrorCode::StaleSequence, QStringLiteral("Stale session sequence"));
    }

    QSqlQuery command(database);
    command.prepare(QStringLiteral(
        "INSERT INTO command_log(session_id, command_id, expected_sequence, payload_json, "
        "recorded_at_utc) VALUES(?, ?, ?, ?, ?)"));
    command.addBindValue(session_id);
    command.addBindValue(batch.command_id);
    command.addBindValue(expected_sequence);
    command.addBindValue(batch.command_json);
    command.addBindValue(batch.recorded_at_utc);
    if (!command.exec()) {
        return queryFailure(StoreErrorCode::ConstraintViolation, command,
                            QStringLiteral("append command"));
    }

    QSqlQuery event(database);
    event.prepare(QStringLiteral(
        "INSERT INTO event_log(session_id, sequence, event_type, payload_json, authority_id) "
        "VALUES(?, ?, ?, ?, ?)"));
    for (std::size_t index = 0; index < batch.events.size(); ++index) {
        const auto sequence = expected_sequence + static_cast<qint64>(index) + 1;
        const auto& write = batch.events[index];
        event.bindValue(0, session_id);
        event.bindValue(1, sequence);
        event.bindValue(2, write.event_type);
        event.bindValue(3, write.payload_json);
        event.bindValue(4, write.authority_id);
        if (!event.exec()) {
            return queryFailure(StoreErrorCode::ConstraintViolation, event,
                                QStringLiteral("append event"));
        }
    }

    QSqlQuery docket(database);
    docket.prepare(QStringLiteral(
        "INSERT INTO docket_projection(session_id, entry_id, event_sequence, title, status) "
        "VALUES(?, ?, ?, ?, ?) ON CONFLICT(session_id, entry_id) DO UPDATE SET "
        "event_sequence=excluded.event_sequence, title=excluded.title, status=excluded.status"));
    for (const auto& write : batch.docket_changes) {
        docket.bindValue(0, session_id);
        docket.bindValue(1, write.entry_id);
        docket.bindValue(2, expected_sequence + static_cast<qint64>(write.source_event_offset) + 1);
        docket.bindValue(3, write.title);
        docket.bindValue(4, write.status);
        if (!docket.exec()) {
            return queryFailure(StoreErrorCode::ConstraintViolation, docket,
                                QStringLiteral("update docket projection"));
        }
    }

    QSqlQuery asset(database);
    asset.prepare(QStringLiteral(
        "INSERT INTO asset_references(session_id, digest, purpose) VALUES(?, ?, ?)"));
    for (const auto& reference : batch.asset_references) {
        asset.bindValue(0, session_id);
        asset.bindValue(1, reference.digest);
        asset.bindValue(2, reference.purpose);
        if (!asset.exec()) {
            return queryFailure(StoreErrorCode::ConstraintViolation, asset,
                                QStringLiteral("link session asset"));
        }
    }

    const auto new_sequence = expected_sequence + event_count;
    QSqlQuery update(database);
    update.prepare(
        QStringLiteral("UPDATE sessions SET sequence = ? WHERE session_id = ? AND sequence = ?"));
    update.addBindValue(new_sequence);
    update.addBindValue(session_id);
    update.addBindValue(expected_sequence);
    if (!update.exec() || update.numRowsAffected() != 1) {
        return queryFailure(StoreErrorCode::StaleSequence, update,
                            QStringLiteral("advance session sequence"));
    }
    return new_sequence;
}

} // namespace

SessionStore::SessionStore(QString connection_name)
    : connection_name_(std::move(connection_name)) {}

SessionStore::~SessionStore() { closeConnection(); }

void SessionStore::closeConnection() {
    const auto connection_name = std::exchange(connection_name_, {});
    if (database_.isValid()) {
        database_.close();
        database_ = QSqlDatabase{};
    }
    if (!connection_name.isEmpty()) {
        QSqlDatabase::removeDatabase(connection_name);
    }
    lifetime_lease_.reset();
    may_fork_ = false;
}

std::expected<std::unique_ptr<SessionStore>, StoreError>
SessionStore::open(const QString& database_path) {
    return open(database_path, {});
}

std::expected<std::unique_ptr<SessionStore>, StoreError>
SessionStore::open(const QString& database_path, const detail::SessionStoreOpenHooks& hooks) {
    if (database_path.isEmpty()) {
        return fail(StoreErrorCode::InvalidArgument, QStringLiteral("Database path is empty"));
    }
    auto preflight = preflightExistingDatabase(database_path);
    if (!preflight) {
        return std::unexpected(preflight.error());
    }
    if (hooks.after_private_preflight) {
        try {
            hooks.after_private_preflight(preflight->original_path);
        } catch (...) {
            return fail(StoreErrorCode::OpenFailed,
                        QStringLiteral("Session database open hook failed"));
        }
    }
    if (hooks.reject_after_private_preflight) {
        return fail(StoreErrorCode::OpenFailed,
                    QStringLiteral("Session database open was interrupted after preflight"));
    }

    auto store = std::unique_ptr<SessionStore>(new SessionStore(
        QStringLiteral("appellate-session-%1").arg(QUuid::createUuid().toString(QUuid::Id128))));
    store->database_path_ = preflight->original_path;
#if defined(Q_OS_UNIX)
    if (const auto identity = revalidatePreflight(*preflight); !identity) {
        return std::unexpected(identity.error());
    }
#endif
    store->database_ =
        QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), store->connection_name_);
    store->database_.setDatabaseName(preflight->sqlite_path);
    if (!store->database_.open()) {
        const auto message = store->database_.lastError().text();
        store->closeConnection();
        return fail(StoreErrorCode::OpenFailed, message);
    }
#if defined(Q_OS_UNIX)
    if (const auto identity = revalidateOpenedDatabaseIdentity(*preflight); !identity) {
        store->closeConnection();
        return std::unexpected(identity.error());
    }
#endif

    if (!preflight->created_by_this_open) {
        if (const auto live_schema = validateApplicationSchema(store->database_, true);
            !live_schema) {
            store->closeConnection();
            return std::unexpected(live_schema.error());
        }
    }

    if (auto configured = store->configure(); !configured) {
        return std::unexpected(configured.error());
    }
    if (auto migrated = store->migrate(); !migrated) {
        return std::unexpected(migrated.error());
    }
#if defined(Q_OS_UNIX)
    if (const auto validated = validateFreshSidecarsAfterOpen(*preflight); !validated) {
        store->closeConnection();
        return std::unexpected(validated.error());
    }
#endif
    if (auto schema = validateApplicationSchema(store->database_, false); !schema) {
        return std::unexpected(schema.error());
    }
#if defined(Q_OS_UNIX)
    if (const auto stable = revalidateLifetimeLease(*preflight); !stable) {
        return std::unexpected(stable.error());
    }
    if (preflight->created_by_this_open) {
        if (const auto retired = clearDatabaseInitializationMarker(preflight->parent_descriptor,
                                                                   preflight->database_name);
            !retired) {
            return std::unexpected(retired.error());
        }
    }
#endif
    preflight->private_snapshot_directory.reset();
    preflight->private_snapshot_path.clear();
    preflight->acceptFreshFiles();
    store->lifetime_lease_ =
        std::make_shared<SessionStoreLifetimeLease>(std::move(*preflight));
    store->may_fork_ = true;
    return store;
}

std::expected<std::unique_ptr<SessionStore>, StoreError>
SessionStore::forkConnection() const {
    if (!may_fork_ || !database_.isOpen() || !lifetime_lease_) {
        return fail(StoreErrorCode::InvalidArgument,
                    QStringLiteral("Only a live owner SessionStore can fork a connection"));
    }
    const auto lease =
        std::static_pointer_cast<SessionStoreLifetimeLease>(lifetime_lease_);
    QMutexLocker fork_guard(&lease->fork_mutex);
#if defined(Q_OS_UNIX)
    if (const auto stable = revalidateLifetimeLease(lease->preflight); !stable) {
        return std::unexpected(stable.error());
    }
#endif

    auto child = std::unique_ptr<SessionStore>(new SessionStore(
        QStringLiteral("appellate-session-child-%1")
            .arg(QUuid::createUuid().toString(QUuid::Id128))));
    child->database_path_ = database_path_;
    child->database_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                 child->connection_name_);
    child->database_.setDatabaseName(lease->preflight.sqlite_path);
    if (!child->database_.open()) {
        const auto message = child->database_.lastError().text();
        child->closeConnection();
        return fail(StoreErrorCode::OpenFailed, message);
    }
#if defined(Q_OS_UNIX)
    if (const auto stable = revalidateLifetimeLease(lease->preflight); !stable) {
        child->closeConnection();
        return std::unexpected(stable.error());
    }
#endif
    if (const auto schema = validateApplicationSchema(child->database_, false); !schema) {
        child->closeConnection();
        return std::unexpected(schema.error());
    }
    if (const auto configured = child->configure(); !configured) {
        child->closeConnection();
        return std::unexpected(configured.error());
    }
#if defined(Q_OS_UNIX)
    if (const auto stable = revalidateLifetimeLease(lease->preflight); !stable) {
        child->closeConnection();
        return std::unexpected(stable.error());
    }
#endif
    child->lifetime_lease_ = lease;
    child->may_fork_ = false;
    return child;
}

std::expected<void, StoreError> SessionStore::validateActiveLease() const {
    if (!database_.isOpen() || !lifetime_lease_) {
        return fail(StoreErrorCode::OpenFailed,
                    QStringLiteral("SessionStore has no active private-state lease"));
    }
#if defined(Q_OS_UNIX)
    const auto lease = std::static_pointer_cast<SessionStoreLifetimeLease>(lifetime_lease_);
    return revalidateLifetimeLease(lease->preflight);
#else
    return {};
#endif
}

std::expected<void, StoreError> SessionStore::configure() {
    constexpr std::pair<const char*, const char*> statements[]{
        {"PRAGMA foreign_keys = ON", "enable foreign keys"},
        {"PRAGMA journal_mode = WAL", "enable WAL mode"},
        {"PRAGMA synchronous = FULL", "enable full synchronization"},
        {"PRAGMA busy_timeout = 5000", "set busy timeout"},
    };
    for (const auto& [sql, action] : statements) {
        if (auto result = execStatement(database_, QLatin1StringView(sql),
                                        StoreErrorCode::OpenFailed, QLatin1StringView(action));
            !result) {
            return result;
        }
    }
    return {};
}

std::expected<void, StoreError> SessionStore::migrate() {
    if (auto begun = beginImmediate(); !begun) {
        return begun;
    }

    QSqlQuery version_query(database_);
    if (!version_query.exec(QStringLiteral(
            "SELECT 1 FROM sqlite_schema WHERE type='table' AND name='schema_migrations'"))) {
        rollback();
        return queryFailure(StoreErrorCode::MigrationFailed, version_query,
                            QStringLiteral("locate migration ledger"));
    }
    auto version = 0;
    if (version_query.next()) {
        if (!version_query.exec(
                QStringLiteral("SELECT COALESCE(MAX(version), 0) FROM schema_migrations")) ||
            !version_query.next()) {
            rollback();
            return queryFailure(StoreErrorCode::MigrationFailed, version_query,
                                QStringLiteral("read schema version"));
        }
        version = version_query.value(0).toInt();
    }
    if (version > current_schema_version) {
        rollback();
        return fail(StoreErrorCode::MigrationFailed,
                    QStringLiteral("Database schema %1 is newer than supported schema %2")
                        .arg(version)
                        .arg(current_schema_version));
    }

    for (auto next_version = version + 1; next_version <= current_schema_version; ++next_version) {
        const auto migration = migrationSql(next_version);
        if (!migration) {
            rollback();
            return std::unexpected(migration.error());
        }
        const auto statements = QString::fromUtf8(*migration).split(u';', Qt::SkipEmptyParts);
        for (const auto& statement : statements) {
            if (statement.trimmed().isEmpty()) {
                continue;
            }
            QSqlQuery migration_query(database_);
            if (!migration_query.exec(statement)) {
                rollback();
                return queryFailure(StoreErrorCode::MigrationFailed, migration_query,
                                    QStringLiteral("apply migration %1").arg(next_version));
            }
        }

        QSqlQuery record(database_);
        record.prepare(
            QStringLiteral("INSERT INTO schema_migrations(version, applied_at_utc) VALUES(?, ?)"));
        record.addBindValue(next_version);
        record.addBindValue(QStringLiteral("2026-08-11T00:00:00Z"));
        if (!record.exec()) {
            rollback();
            return queryFailure(StoreErrorCode::MigrationFailed, record,
                                QStringLiteral("record migration %1").arg(next_version));
        }
    }
    return commit();
}

std::expected<void, StoreError>
SessionStore::createSession(const QString& session_id, const QString& engine_revision,
                            const QString& created_at_utc, const std::vector<RevisionPin>& pins,
                            SessionAuthorityContract authority_contract) {
    const auto authority_contract_name = validateSessionCreate(
        session_id, engine_revision, created_at_utc, pins, authority_contract);
    if (!authority_contract_name) {
        return std::unexpected(authority_contract_name.error());
    }
    if (auto begun = beginImmediate(); !begun) {
        return begun;
    }
    if (const auto inserted = insertSessionRows(database_, session_id, engine_revision,
                                                created_at_utc, pins, *authority_contract_name);
        !inserted) {
        rollback();
        return std::unexpected(inserted.error());
    }
    return commit();
}

std::expected<qint64, StoreError> SessionStore::createSessionWithInitialBatch(
    const QString& session_id, const QString& engine_revision, const QString& created_at_utc,
    const std::vector<RevisionPin>& pins, SessionAuthorityContract authority_contract,
    const CommitBatch& initial_batch) {
    if (!initial_batch.asset_references.empty()) {
        return fail(StoreErrorCode::InvalidArgument,
                    QStringLiteral(
                        "Initial asset references require a paired CAS commit API"));
    }
    const auto authority_contract_name = validateSessionCreate(
        session_id, engine_revision, created_at_utc, pins, authority_contract);
    if (!authority_contract_name) {
        return std::unexpected(authority_contract_name.error());
    }
    const auto event_count = validateCommit(session_id, 0, initial_batch);
    if (!event_count) {
        return std::unexpected(event_count.error());
    }
    if (auto begun = beginImmediate(); !begun) {
        return std::unexpected(begun.error());
    }
    if (const auto inserted = insertSessionRows(database_, session_id, engine_revision,
                                                created_at_utc, pins, *authority_contract_name);
        !inserted) {
        rollback();
        return std::unexpected(inserted.error());
    }
    const auto appended = appendRows(database_, session_id, 0, initial_batch, *event_count);
    if (!appended) {
        rollback();
        return std::unexpected(appended.error());
    }
    if (const auto committed = commit(); !committed) {
        return std::unexpected(committed.error());
    }
    return *appended;
}

std::expected<qint64, StoreError> SessionStore::append(const QString& session_id,
                                                       qint64 expected_sequence,
                                                       const CommitBatch& batch) {
    if (!batch.asset_references.empty()) {
        return fail(StoreErrorCode::InvalidArgument,
                    QStringLiteral("Asset references require a paired CAS commit API"));
    }
    const auto event_count = validateCommit(session_id, expected_sequence, batch);
    if (!event_count) {
        return std::unexpected(event_count.error());
    }
    if (auto begun = beginImmediate(); !begun) {
        return std::unexpected(begun.error());
    }
    const auto appended = appendRows(database_, session_id, expected_sequence, batch, *event_count);
    if (!appended) {
        rollback();
        return std::unexpected(appended.error());
    }
    if (auto committed = commit(); !committed) {
        return std::unexpected(committed.error());
    }
    return *appended;
}

std::expected<QString, StoreError> SessionStore::assetStoreIdentity() const {
    if (const auto lease = validateActiveLease(); !lease) {
        return std::unexpected(lease.error());
    }
    QSqlQuery identity_query(database_);
    if (!identity_query.exec(QStringLiteral(
            "SELECT identity FROM store_identity WHERE singleton=1"))) {
        return queryFailure(StoreErrorCode::QueryFailed, identity_query,
                            QStringLiteral("read database asset-store identity"));
    }
    if (!identity_query.next()) {
        return fail(StoreErrorCode::ConstraintViolation,
                    QStringLiteral("Database has no authoritative asset-store identity"));
    }
    const auto database_identity = identity_query.value(0).toString();
    if (identity_query.next()) {
        return fail(StoreErrorCode::ConstraintViolation,
                    QStringLiteral("Database has multiple asset-store identities"));
    }
    return database_identity;
}

std::expected<void, StoreError> SessionStore::ensureAssetStoreIdentity(
    AssetStore& asset_store, const AssetStoreLock& lock,
    const QStringList& referenced_digests, bool require_exact_object_set) {
    const auto database_identity = assetStoreIdentity();
    if (!database_identity) {
        return std::unexpected(database_identity.error());
    }
    const auto asset_identity = asset_store.identity(lock);
    if (!asset_identity) {
        return fail(StoreErrorCode::QueryFailed, asset_identity.error().message);
    }
    if (asset_identity->has_value() && **asset_identity != *database_identity) {
        return fail(StoreErrorCode::ConstraintViolation,
                    QStringLiteral("Database and asset-store identities do not match"));
    }
    const auto object_digests = asset_store.objectDigests(lock);
    if (!object_digests) {
        return fail(StoreErrorCode::QueryFailed, object_digests.error().message);
    }
    auto expected_digests = referenced_digests;
    expected_digests.removeDuplicates();
    expected_digests.sort();

    // The database identity is authoritative. A missing CAS marker is created only after proving
    // the database's complete reference set exactly matches the CAS and every named object hashes
    // to its name. An existing marker is never adopted by a new or replacement database.
    if ((!asset_identity->has_value() || require_exact_object_set) &&
        *object_digests != expected_digests) {
        return fail(StoreErrorCode::ConstraintViolation,
                    QStringLiteral(
                        "Database asset references do not exactly match asset-store objects"));
    }
    for (const auto& digest : expected_digests) {
        if (const auto contents = asset_store.read(digest); !contents) {
            return fail(StoreErrorCode::ConstraintViolation,
                        QStringLiteral("Cannot bind a corrupt asset object %1: %2")
                            .arg(digest, contents.error().message));
        }
    }

    if (const auto written = asset_store.writeIdentity(*database_identity, lock); !written) {
        return fail(StoreErrorCode::QueryFailed, written.error().message);
    }
    return {};
}

std::expected<qint64, StoreError> SessionStore::appendWithStagedAsset(
    const QString& session_id, qint64 expected_sequence, const CommitBatch& batch,
    AssetStore& asset_store, StagedAsset& staged_asset) {
    const auto event_count = validateCommit(session_id, expected_sequence, batch);
    if (!event_count) {
        return std::unexpected(event_count.error());
    }
    if (staged_asset.finalized_ || !validDigest(staged_asset.sha256_) ||
        batch.asset_references.size() != 1 ||
        batch.asset_references.front().digest != staged_asset.sha256_) {
        return fail(StoreErrorCode::InvalidArgument,
                    QStringLiteral("The staged asset must match the batch's sole new reference"));
    }
    if (auto begun = beginImmediate(); !begun) {
        return std::unexpected(begun.error());
    }
    QSqlQuery head(database_);
    head.prepare(QStringLiteral("SELECT sequence FROM sessions WHERE session_id=?"));
    head.addBindValue(session_id);
    if (!head.exec()) {
        rollback();
        return queryFailure(StoreErrorCode::QueryFailed, head,
                            QStringLiteral("preflight paired append session"));
    }
    if (!head.next()) {
        rollback();
        return fail(StoreErrorCode::NotFound, QStringLiteral("Session not found"));
    }
    if (head.value(0).toLongLong() != expected_sequence) {
        rollback();
        return fail(StoreErrorCode::StaleSequence, QStringLiteral("Stale session sequence"));
    }
    QSqlQuery duplicate_command(database_);
    duplicate_command.prepare(QStringLiteral(
        "SELECT 1 FROM command_log WHERE session_id=? AND command_id=?"));
    duplicate_command.addBindValue(session_id);
    duplicate_command.addBindValue(batch.command_id);
    if (!duplicate_command.exec()) {
        rollback();
        return queryFailure(StoreErrorCode::QueryFailed, duplicate_command,
                            QStringLiteral("preflight paired append command"));
    }
    if (duplicate_command.next()) {
        rollback();
        return fail(StoreErrorCode::ConstraintViolation,
                    QStringLiteral("Command ID already exists in the session"));
    }
    QSqlQuery duplicate_asset(database_);
    duplicate_asset.prepare(QStringLiteral(
        "SELECT 1 FROM asset_references WHERE session_id=? AND digest=? AND purpose=?"));
    duplicate_asset.addBindValue(session_id);
    duplicate_asset.addBindValue(batch.asset_references.front().digest);
    duplicate_asset.addBindValue(batch.asset_references.front().purpose);
    if (!duplicate_asset.exec()) {
        rollback();
        return queryFailure(StoreErrorCode::QueryFailed, duplicate_asset,
                            QStringLiteral("preflight paired append asset reference"));
    }
    if (duplicate_asset.next()) {
        rollback();
        return fail(StoreErrorCode::ConstraintViolation,
                    QStringLiteral("Asset reference already exists in the session"));
    }
    QSqlQuery referenced_query(database_);
    if (!referenced_query.exec(QStringLiteral(
            "SELECT DISTINCT digest FROM asset_references ORDER BY digest"))) {
        rollback();
        return queryFailure(StoreErrorCode::QueryFailed, referenced_query,
                            QStringLiteral("read asset references before publication"));
    }
    QStringList existing_digests;
    while (referenced_query.next()) {
        existing_digests.push_back(referenced_query.value(0).toString());
    }
    const auto database_identity = assetStoreIdentity();
    if (!database_identity) {
        rollback();
        return std::unexpected(database_identity.error());
    }
    const auto published_lock = asset_store.hasPublishedLock();
    if (!published_lock) {
        rollback();
        return fail(published_lock.error().code == AssetStoreErrorCode::InvalidConfiguration
                        ? StoreErrorCode::ConstraintViolation
                        : StoreErrorCode::QueryFailed,
                    QStringLiteral("Cannot inspect asset store for database commit: %1")
                        .arg(published_lock.error().message));
    }
    if (!*published_lock) {
        if (const auto preflight =
                asset_store.preflightPair(*database_identity, existing_digests, true);
            !preflight) {
            rollback();
            return fail(preflight.error().code == AssetStoreErrorCode::InvalidConfiguration
                            ? StoreErrorCode::ConstraintViolation
                            : StoreErrorCode::QueryFailed,
                        QStringLiteral("Cannot preflight asset store for database commit: %1")
                            .arg(preflight.error().message));
        }
    }
    auto lock = asset_store.acquireLock();
    if (!lock) {
        rollback();
        return fail(StoreErrorCode::QueryFailed,
                    QStringLiteral("Cannot lock asset store for database commit: %1")
                        .arg(lock.error().message));
    }
    if (const auto paired =
            ensureAssetStoreIdentity(asset_store, *lock, existing_digests, true);
        !paired) {
        rollback();
        return std::unexpected(paired.error());
    }
    const auto appended = appendRows(database_, session_id, expected_sequence, batch, *event_count);
    if (!appended) {
        rollback();
        return std::unexpected(appended.error());
    }
    const auto finalized = asset_store.finalize(staged_asset, *lock);
    if (!finalized) {
        rollback();
        return fail(StoreErrorCode::QueryFailed,
                    QStringLiteral("Cannot publish staged asset for database commit: %1")
                        .arg(finalized.error().message));
    }
    const auto committed = commit();
    if (!committed) {
        rollback();
        QSqlQuery references(database_);
        references.prepare(QStringLiteral(
            "SELECT COUNT(*) FROM asset_references WHERE digest=?"));
        references.addBindValue(staged_asset.sha256_);
        const auto queried = references.exec() && references.next();
        const auto conclusively_unreferenced =
            queried && references.value(0).toLongLong() == 0;
        if (conclusively_unreferenced) {
            static_cast<void>(asset_store.removeNewlyFinalized(staged_asset, *lock));
        }
        return std::unexpected(committed.error());
    }
    return *appended;
}

std::expected<void, StoreError> SessionStore::recoverAssetStore(AssetStore& asset_store) {
    return recoverAssetStore(asset_store, {});
}

std::expected<void, StoreError>
SessionStore::recoverAssetStore(AssetStore& asset_store, const detail::AssetRecoveryHooks& hooks) {
    if (auto begun = beginImmediate(); !begun) {
        return begun;
    }
    QSqlQuery references(database_);
    if (!references.exec(QStringLiteral(
            "SELECT DISTINCT digest FROM asset_references ORDER BY digest"))) {
        rollback();
        return queryFailure(StoreErrorCode::QueryFailed, references,
                            QStringLiteral("read referenced assets for recovery"));
    }
    QStringList digests;
    while (references.next()) {
        digests.push_back(references.value(0).toString());
    }
    const auto database_identity = assetStoreIdentity();
    if (!database_identity) {
        rollback();
        return std::unexpected(database_identity.error());
    }
    const auto published_lock = asset_store.hasPublishedLock();
    if (!published_lock) {
        rollback();
        return fail(published_lock.error().code == AssetStoreErrorCode::InvalidConfiguration
                        ? StoreErrorCode::ConstraintViolation
                        : StoreErrorCode::QueryFailed,
                    QStringLiteral("Cannot inspect asset store for recovery: %1")
                        .arg(published_lock.error().message));
    }
    if (!*published_lock) {
        if (const auto preflight =
                asset_store.preflightPair(*database_identity, digests, false);
            !preflight) {
            rollback();
            return fail(preflight.error().code == AssetStoreErrorCode::InvalidConfiguration
                            ? StoreErrorCode::ConstraintViolation
                            : StoreErrorCode::QueryFailed,
                        QStringLiteral("Cannot preflight asset store for recovery: %1")
                            .arg(preflight.error().message));
        }
    }
    auto lock = asset_store.acquireLock();
    if (!lock) {
        rollback();
        return fail(StoreErrorCode::QueryFailed,
                    QStringLiteral("Cannot lock asset store for recovery: %1")
                        .arg(lock.error().message));
    }
    if (const auto recovered =
            asset_store.recoverPairedObjects(*database_identity, *lock, digests, &hooks);
        !recovered) {
        rollback();
        return fail(recovered.error().code == AssetStoreErrorCode::InvalidConfiguration
                        ? StoreErrorCode::ConstraintViolation
                        : StoreErrorCode::QueryFailed,
                    QStringLiteral("Cannot recover asset store: %1")
                        .arg(recovered.error().message));
    }
    return commit();
}

std::expected<SessionSnapshot, StoreError>
SessionStore::loadSession(const QString& session_id) const {
    if (const auto lease = validateActiveLease(); !lease) {
        return std::unexpected(lease.error());
    }
    QSqlQuery session(database_);
    session.prepare(QStringLiteral(
        "SELECT engine_revision, authority_contract, sequence, created_at_utc FROM sessions "
        "WHERE session_id = ?"));
    session.addBindValue(session_id);
    if (!session.exec()) {
        return queryFailure(StoreErrorCode::QueryFailed, session, QStringLiteral("load session"));
    }
    if (!session.next()) {
        return fail(StoreErrorCode::NotFound, QStringLiteral("Session not found"));
    }

    const auto authority_contract = parseAuthorityContract(session.value(1).toString());
    if (!authority_contract.has_value()) {
        return fail(StoreErrorCode::ConstraintViolation,
                    QStringLiteral("Stored session authority contract is invalid"));
    }

    SessionSnapshot snapshot{session_id,
                             session.value(0).toString(),
                             *authority_contract,
                             session.value(2).toLongLong(),
                             {},
                             {},
                             {},
                             {},
                             {},
                             session.value(3).toString()};

    QSqlQuery pins(database_);
    pins.prepare(QStringLiteral(
        "SELECT pack_id, version, digest FROM session_pins WHERE session_id = ? ORDER BY pack_id"));
    pins.addBindValue(session_id);
    if (!pins.exec()) {
        return queryFailure(StoreErrorCode::QueryFailed, pins, QStringLiteral("load pins"));
    }
    while (pins.next()) {
        snapshot.pins.push_back(RevisionPin{pins.value(0).toString(), pins.value(1).toString(),
                                            pins.value(2).toString()});
    }

    QSqlQuery commands(database_);
    commands.prepare(QStringLiteral(
        "SELECT command_id, expected_sequence, payload_json, recorded_at_utc FROM command_log "
        "WHERE session_id = ? ORDER BY expected_sequence, command_id"));
    commands.addBindValue(session_id);
    if (!commands.exec()) {
        return queryFailure(StoreErrorCode::QueryFailed, commands, QStringLiteral("load commands"));
    }
    while (commands.next()) {
        snapshot.commands.push_back(
            StoredCommand{commands.value(0).toString(), commands.value(1).toLongLong(),
                          commands.value(2).toByteArray(), commands.value(3).toString()});
    }

    QSqlQuery events(database_);
    events.prepare(
        QStringLiteral("SELECT sequence, event_type, payload_json, authority_id FROM event_log "
                       "WHERE session_id = ? ORDER BY sequence"));
    events.addBindValue(session_id);
    if (!events.exec()) {
        return queryFailure(StoreErrorCode::QueryFailed, events, QStringLiteral("load events"));
    }
    while (events.next()) {
        snapshot.events.push_back(
            StoredEvent{events.value(0).toLongLong(), events.value(1).toString(),
                        events.value(2).toByteArray(), events.value(3).toString()});
    }

    QSqlQuery docket(database_);
    docket.prepare(
        QStringLiteral("SELECT entry_id, event_sequence, title, status FROM docket_projection "
                       "WHERE session_id = ? ORDER BY event_sequence, entry_id"));
    docket.addBindValue(session_id);
    if (!docket.exec()) {
        return queryFailure(StoreErrorCode::QueryFailed, docket, QStringLiteral("load docket"));
    }
    while (docket.next()) {
        snapshot.docket.push_back(
            DocketEntry{docket.value(0).toString(), docket.value(1).toLongLong(),
                        docket.value(2).toString(), docket.value(3).toString()});
    }

    QSqlQuery assets(database_);
    assets.prepare(
        QStringLiteral("SELECT digest, purpose FROM asset_references WHERE session_id = ? "
                       "ORDER BY purpose, digest"));
    assets.addBindValue(session_id);
    if (!assets.exec()) {
        return queryFailure(StoreErrorCode::QueryFailed, assets,
                            QStringLiteral("load asset references"));
    }
    while (assets.next()) {
        snapshot.asset_references.push_back(
            AssetReference{assets.value(0).toString(), assets.value(1).toString()});
    }
    return snapshot;
}

std::expected<void, StoreError> SessionStore::backupTo(const QString& backup_path) const {
    if (const auto lease = validateActiveLease(); !lease) {
        return std::unexpected(lease.error());
    }
    if (backup_path.isEmpty()) {
        return fail(StoreErrorCode::InvalidArgument, QStringLiteral("Backup path is empty"));
    }
    const auto absolute_backup = QFileInfo(backup_path).absoluteFilePath();
    const auto database_path = database_path_;
    const QFileInfo destination(absolute_backup);
    if (isSqliteReservedSidecarPath(database_path) ||
        isSqliteReservedSidecarPath(absolute_backup) || absolute_backup == database_path ||
        destination.exists() || destination.isSymLink()) {
        return fail(StoreErrorCode::BackupFailed,
                    QStringLiteral(
                        "Backup source and destination must be main-database paths, and the "
                        "destination must be new"));
    }
    QSqlQuery referenced_assets(database_);
    if (!referenced_assets.exec(QStringLiteral(
            "SELECT 1 FROM asset_references LIMIT 1"))) {
        return queryFailure(StoreErrorCode::BackupFailed, referenced_assets,
                            QStringLiteral("inspect backup asset references"));
    }
    if (referenced_assets.next()) {
        return fail(StoreErrorCode::BackupFailed,
                    QStringLiteral(
                        "Database-only backup is unavailable for document-bearing sessions"));
    }
    referenced_assets.finish();
    const auto parent = destination.absolutePath();
    if (!QDir{}.mkpath(parent) || QFileInfo(parent).isSymLink()) {
        return fail(StoreErrorCode::BackupFailed,
                    QStringLiteral("Backup directory is unsafe or cannot be created"));
    }

    QTemporaryFile staged(QDir(parent).filePath(QStringLiteral(".backup-XXXXXX.sqlite")));
    staged.setAutoRemove(true);
    if (!staged.open()) {
        return fail(StoreErrorCode::BackupFailed,
                    QStringLiteral("Cannot reserve staged backup path"));
    }
    const auto staged_path = staged.fileName();
    staged.close();
    if (!staged.remove()) {
        return fail(StoreErrorCode::BackupFailed,
                    QStringLiteral("Cannot prepare staged backup path"));
    }

    QSqlQuery checkpoint(database_);
    if (!checkpoint.exec(QStringLiteral("PRAGMA wal_checkpoint(FULL)"))) {
        return queryFailure(StoreErrorCode::BackupFailed, checkpoint,
                            QStringLiteral("checkpoint session database"));
    }
    checkpoint.finish();
    QSqlQuery backup(database_);
    backup.prepare(QStringLiteral("VACUUM INTO ?"));
    backup.addBindValue(staged_path);
    if (!backup.exec()) {
        return queryFailure(StoreErrorCode::BackupFailed, backup,
                            QStringLiteral("create consistent database backup"));
    }
    backup.finish();
    if (const auto verified = verifyDatabase(staged_path, StoreErrorCode::BackupFailed);
        !verified) {
        return verified;
    }
    const auto staged_has_assets =
        databaseContainsAssetReferences(staged_path, StoreErrorCode::BackupFailed);
    if (!staged_has_assets) {
        return std::unexpected(staged_has_assets.error());
    }
    if (*staged_has_assets) {
        return fail(StoreErrorCode::BackupFailed,
                    QStringLiteral(
                        "Database snapshot acquired asset references during backup"));
    }
    if (!QFile::setPermissions(staged_path, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        return fail(StoreErrorCode::BackupFailed,
                    QStringLiteral("Cannot restrict backup permissions"));
    }
    QFile backup_file(staged_path);
    if (!backup_file.open(QIODevice::ReadOnly) || !syncFile(backup_file) ||
        !syncDirectory(parent)) {
        return fail(StoreErrorCode::BackupFailed,
                    QStringLiteral("Cannot durably flush database backup"));
    }
    backup_file.close();
    if (const auto lease = validateActiveLease(); !lease) {
        return std::unexpected(lease.error());
    }
    if (!QFile::rename(staged_path, absolute_backup)) {
        return fail(StoreErrorCode::BackupFailed,
                    QStringLiteral("Cannot atomically publish database backup"));
    }
    staged.setAutoRemove(false);
    if (!syncDirectory(parent)) {
        return fail(StoreErrorCode::BackupFailed,
                    QStringLiteral("Cannot durably flush backup directory"));
    }
    return {};
}

std::expected<void, StoreError> SessionStore::restoreBackup(const QString& backup_path,
                                                            const QString& destination_path) {
    if (backup_path.isEmpty() || destination_path.isEmpty()) {
        return fail(StoreErrorCode::InvalidArgument,
                    QStringLiteral("Backup and restore paths are required"));
    }
    const QFileInfo source_info(backup_path);
    const QFileInfo destination_info(destination_path);
    if (isSqliteReservedSidecarPath(source_info.absoluteFilePath()) ||
        isSqliteReservedSidecarPath(destination_info.absoluteFilePath()) ||
        !source_info.isFile() || source_info.isSymLink() || destination_info.exists() ||
        destination_info.isSymLink() ||
        source_info.absoluteFilePath() == destination_info.absoluteFilePath()) {
        return fail(StoreErrorCode::RestoreFailed,
                    QStringLiteral("Restore source must be a real backup and destination new"));
    }
    const auto source_preflight =
        preflightExistingDatabase(source_info.absoluteFilePath(), true, true, false);
    if (!source_preflight || source_preflight->private_snapshot_path.isEmpty()) {
        return fail(StoreErrorCode::RestoreFailed,
                    source_preflight
                        ? QStringLiteral("Restore source is not a nonempty standalone database")
                        : source_preflight.error().message);
    }

    const auto parent = destination_info.absolutePath();
    if (!QDir{}.mkpath(parent) || QFileInfo(parent).isSymLink()) {
        return fail(StoreErrorCode::RestoreFailed,
                    QStringLiteral("Restore directory is unsafe or cannot be created"));
    }
    // Copy only the stable, privately validated snapshot. The caller-controlled live path is
    // never opened through SQLite and is not reread after preflight.
    QFile source(source_preflight->private_snapshot_path);
    if (!source.open(QIODevice::ReadOnly)) {
        return fail(StoreErrorCode::RestoreFailed, QStringLiteral("Cannot read backup"));
    }
    QTemporaryFile staged(QDir(parent).filePath(QStringLiteral(".restore-XXXXXX.tmp")));
    staged.setAutoRemove(true);
    if (!staged.open()) {
        return fail(StoreErrorCode::RestoreFailed, QStringLiteral("Cannot create staged restore"));
    }
    std::array<char, backup_buffer_bytes> buffer{};
    while (true) {
        const auto read = source.read(buffer.data(), static_cast<qint64>(buffer.size()));
        if (read < 0) {
            return fail(StoreErrorCode::RestoreFailed,
                        QStringLiteral("Cannot read complete backup"));
        }
        if (read == 0) {
            break;
        }
        if (staged.write(buffer.data(), read) != read) {
            return fail(StoreErrorCode::RestoreFailed,
                        QStringLiteral("Cannot write staged restore"));
        }
    }
    if (!syncFile(staged)) {
        return fail(StoreErrorCode::RestoreFailed,
                    QStringLiteral("Cannot durably flush staged restore"));
    }
    const auto staged_path = staged.fileName();
    staged.close();
    if (const auto verified = verifyDatabase(staged_path, StoreErrorCode::RestoreFailed, true);
        !verified) {
        return verified;
    }
    const auto staged_has_assets =
        databaseContainsAssetReferences(staged_path, StoreErrorCode::RestoreFailed);
    if (!staged_has_assets) {
        return std::unexpected(staged_has_assets.error());
    }
    if (*staged_has_assets) {
        return fail(StoreErrorCode::RestoreFailed,
                    QStringLiteral(
                        "Database-only restore cannot contain asset references"));
    }
    const auto staged_schema =
        databaseSchemaVersion(staged_path, StoreErrorCode::RestoreFailed);
    if (!staged_schema) {
        return std::unexpected(staged_schema.error());
    }
    if (*staged_schema == current_schema_version) {
        if (const auto rotated = rotateRestoredDatabaseIdentity(staged_path); !rotated) {
            return std::unexpected(rotated.error());
        }
        if (const auto verified = verifyDatabase(staged_path, StoreErrorCode::RestoreFailed);
            !verified) {
            return verified;
        }
    } else if (*staged_schema < 1 || *staged_schema >= current_schema_version) {
        return fail(StoreErrorCode::RestoreFailed,
                    QStringLiteral("Restore source has an unsupported schema version"));
    }
    if (!QFile::setPermissions(staged_path, QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        !QFile::rename(staged_path, destination_info.absoluteFilePath())) {
        return fail(StoreErrorCode::RestoreFailed,
                    QStringLiteral("Cannot atomically commit restored database"));
    }
    staged.setAutoRemove(false);
    if (!syncDirectory(parent)) {
        return fail(StoreErrorCode::RestoreFailed,
                    QStringLiteral("Cannot durably flush restore directory"));
    }
    return {};
}

int SessionStore::schemaVersion() const {
    if (const auto lease = validateActiveLease(); !lease) {
        return -1;
    }
    QSqlQuery query(database_);
    if (!query.exec(QStringLiteral("SELECT COALESCE(MAX(version), 0) FROM schema_migrations")) ||
        !query.next()) {
        return -1;
    }
    return query.value(0).toInt();
}

std::expected<void, StoreError> SessionStore::beginImmediate() {
    if (lifetime_lease_) {
        if (const auto lease = validateActiveLease(); !lease) {
            return lease;
        }
    }
    return execStatement(database_, QStringLiteral("BEGIN IMMEDIATE"), StoreErrorCode::QueryFailed,
                         QStringLiteral("begin transaction"));
}

std::expected<void, StoreError> SessionStore::commit() {
    if (lifetime_lease_) {
        if (const auto lease = validateActiveLease(); !lease) {
            rollback();
            return lease;
        }
    }
    auto committed = execStatement(database_, QStringLiteral("COMMIT"), StoreErrorCode::QueryFailed,
                                   QStringLiteral("commit transaction"));
    if (!committed) {
        rollback();
    }
    return committed;
}

void SessionStore::rollback() {
    QSqlQuery query(database_);
    static_cast<void>(query.exec(QStringLiteral("ROLLBACK")));
}

} // namespace appellate::storage
