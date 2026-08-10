#include "appellate/storage/session_store.hpp"

#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

#include <limits>
#include <ranges>
#include <utility>

namespace appellate::storage {
namespace {

constexpr auto current_schema_version = 1;
constexpr qsizetype maximum_asset_purpose_length = 128;
constexpr std::size_t maximum_asset_references_per_batch = 4096;

[[nodiscard]] auto fail(StoreErrorCode code, QString message) -> std::unexpected<StoreError> {
    return std::unexpected(StoreError{code, std::move(message)});
}

[[nodiscard]] auto queryFailure(StoreErrorCode code, const QSqlQuery& query, const QString& action)
    -> std::unexpected<StoreError> {
    return fail(code, QStringLiteral("%1: %2").arg(action, query.lastError().text()));
}

[[nodiscard]] bool validText(const QString& value) {
    return !value.isEmpty() && value.size() <= 512;
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

[[nodiscard]] auto execStatement(QSqlDatabase& database, const QString& sql, StoreErrorCode code,
                                 const QString& action) -> std::expected<void, StoreError> {
    QSqlQuery query(database);
    if (!query.exec(sql)) {
        return queryFailure(code, query, action);
    }
    return {};
}

constexpr auto migration_v1 = R"SQL(
CREATE TABLE IF NOT EXISTS schema_migrations (
    version INTEGER PRIMARY KEY,
    applied_at_utc TEXT NOT NULL
) STRICT;

CREATE TABLE IF NOT EXISTS installed_pack_revisions (
    pack_id TEXT NOT NULL,
    version TEXT NOT NULL,
    digest TEXT NOT NULL CHECK(length(digest) = 64),
    installed_at_utc TEXT NOT NULL,
    PRIMARY KEY (pack_id, version),
    UNIQUE (pack_id, version, digest)
) STRICT;

CREATE TABLE IF NOT EXISTS sessions (
    session_id TEXT PRIMARY KEY,
    engine_revision TEXT NOT NULL,
    sequence INTEGER NOT NULL DEFAULT 0 CHECK(sequence >= 0),
    created_at_utc TEXT NOT NULL
) STRICT;

CREATE TABLE IF NOT EXISTS session_pins (
    session_id TEXT NOT NULL REFERENCES sessions(session_id) ON DELETE CASCADE,
    pack_id TEXT NOT NULL,
    version TEXT NOT NULL,
    digest TEXT NOT NULL CHECK(length(digest) = 64),
    PRIMARY KEY (session_id, pack_id)
) STRICT;

CREATE TABLE IF NOT EXISTS command_log (
    session_id TEXT NOT NULL REFERENCES sessions(session_id) ON DELETE CASCADE,
    command_id TEXT NOT NULL,
    expected_sequence INTEGER NOT NULL CHECK(expected_sequence >= 0),
    payload_json BLOB NOT NULL,
    recorded_at_utc TEXT NOT NULL,
    PRIMARY KEY (session_id, command_id)
) STRICT;

CREATE TABLE IF NOT EXISTS event_log (
    session_id TEXT NOT NULL REFERENCES sessions(session_id) ON DELETE CASCADE,
    sequence INTEGER NOT NULL CHECK(sequence > 0),
    event_type TEXT NOT NULL,
    payload_json BLOB NOT NULL,
    authority_id TEXT NOT NULL,
    PRIMARY KEY (session_id, sequence)
) STRICT;

CREATE TABLE IF NOT EXISTS docket_projection (
    session_id TEXT NOT NULL REFERENCES sessions(session_id) ON DELETE CASCADE,
    entry_id TEXT NOT NULL,
    event_sequence INTEGER NOT NULL CHECK(event_sequence > 0),
    title TEXT NOT NULL,
    status TEXT NOT NULL,
    PRIMARY KEY (session_id, entry_id),
    FOREIGN KEY (session_id, event_sequence)
        REFERENCES event_log(session_id, sequence) ON DELETE CASCADE
) STRICT;

CREATE TABLE IF NOT EXISTS asset_references (
    session_id TEXT NOT NULL REFERENCES sessions(session_id) ON DELETE CASCADE,
    digest TEXT NOT NULL CHECK(length(digest) = 64),
    purpose TEXT NOT NULL,
    PRIMARY KEY (session_id, digest, purpose)
) STRICT;
)SQL";

} // namespace

SessionStore::SessionStore(QString connection_name)
    : connection_name_(std::move(connection_name)) {}

SessionStore::~SessionStore() { closeConnection(); }

void SessionStore::closeConnection() {
    if (connection_name_.isEmpty()) {
        return;
    }
    const auto connection_name = std::exchange(connection_name_, {});
    if (database_.isValid()) {
        database_.close();
        database_ = QSqlDatabase{};
    }
    QSqlDatabase::removeDatabase(connection_name);
}

std::expected<std::unique_ptr<SessionStore>, StoreError>
SessionStore::open(const QString& database_path) {
    if (database_path.isEmpty()) {
        return fail(StoreErrorCode::InvalidArgument, QStringLiteral("Database path is empty"));
    }

    auto store = std::unique_ptr<SessionStore>(new SessionStore(
        QStringLiteral("appellate-session-%1").arg(QUuid::createUuid().toString(QUuid::Id128))));
    store->database_ =
        QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), store->connection_name_);
    store->database_.setDatabaseName(database_path);
    if (!store->database_.open()) {
        const auto message = store->database_.lastError().text();
        store->closeConnection();
        return fail(StoreErrorCode::OpenFailed, message);
    }

    if (auto configured = store->configure(); !configured) {
        return std::unexpected(configured.error());
    }
    if (auto migrated = store->migrate(); !migrated) {
        return std::unexpected(migrated.error());
    }
    return store;
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
            "CREATE TABLE IF NOT EXISTS schema_migrations (version INTEGER PRIMARY KEY, "
            "applied_at_utc TEXT NOT NULL) STRICT"))) {
        rollback();
        return queryFailure(StoreErrorCode::MigrationFailed, version_query,
                            QStringLiteral("create migration ledger"));
    }

    if (!version_query.exec(
            QStringLiteral("SELECT COALESCE(MAX(version), 0) FROM schema_migrations")) ||
        !version_query.next()) {
        rollback();
        return queryFailure(StoreErrorCode::MigrationFailed, version_query,
                            QStringLiteral("read schema version"));
    }
    const auto version = version_query.value(0).toInt();
    if (version > current_schema_version) {
        rollback();
        return fail(StoreErrorCode::MigrationFailed,
                    QStringLiteral("Database schema %1 is newer than supported schema %2")
                        .arg(version)
                        .arg(current_schema_version));
    }

    if (version < 1) {
        const auto statements = QString::fromUtf8(migration_v1).split(u';', Qt::SkipEmptyParts);
        for (const auto& statement : statements) {
            if (statement.trimmed().isEmpty()) {
                continue;
            }
            QSqlQuery migration(database_);
            if (!migration.exec(statement)) {
                rollback();
                return queryFailure(StoreErrorCode::MigrationFailed, migration,
                                    QStringLiteral("apply migration 1"));
            }
        }

        QSqlQuery record(database_);
        record.prepare(
            QStringLiteral("INSERT INTO schema_migrations(version, applied_at_utc) VALUES(1, ?)"));
        record.addBindValue(QStringLiteral("2026-08-11T00:00:00Z"));
        if (!record.exec()) {
            rollback();
            return queryFailure(StoreErrorCode::MigrationFailed, record,
                                QStringLiteral("record migration 1"));
        }
    }
    return commit();
}

std::expected<void, StoreError> SessionStore::createSession(const QString& session_id,
                                                            const QString& engine_revision,
                                                            const QString& created_at_utc,
                                                            const std::vector<RevisionPin>& pins) {
    if (!validText(session_id) || !validText(engine_revision) || !validText(created_at_utc) ||
        pins.empty()) {
        return fail(
            StoreErrorCode::InvalidArgument,
            QStringLiteral("Session identity, engine revision, time, and pins are required"));
    }
    for (const auto& pin : pins) {
        if (!validText(pin.pack_id) || !validText(pin.version) || !validDigest(pin.digest)) {
            return fail(StoreErrorCode::InvalidArgument, QStringLiteral("Invalid revision pin"));
        }
    }

    if (auto begun = beginImmediate(); !begun) {
        return begun;
    }

    QSqlQuery session(database_);
    session.prepare(QStringLiteral(
        "INSERT INTO sessions(session_id, engine_revision, sequence, created_at_utc) "
        "VALUES(?, ?, 0, ?)"));
    session.addBindValue(session_id);
    session.addBindValue(engine_revision);
    session.addBindValue(created_at_utc);
    if (!session.exec()) {
        rollback();
        return queryFailure(StoreErrorCode::AlreadyExists, session,
                            QStringLiteral("create session"));
    }

    QSqlQuery pin_query(database_);
    pin_query.prepare(QStringLiteral(
        "INSERT INTO session_pins(session_id, pack_id, version, digest) VALUES(?, ?, ?, ?)"));
    for (const auto& pin : pins) {
        pin_query.bindValue(0, session_id);
        pin_query.bindValue(1, pin.pack_id);
        pin_query.bindValue(2, pin.version);
        pin_query.bindValue(3, pin.digest);
        if (!pin_query.exec()) {
            rollback();
            return queryFailure(StoreErrorCode::ConstraintViolation, pin_query,
                                QStringLiteral("pin pack revision"));
        }
    }
    return commit();
}

std::expected<qint64, StoreError> SessionStore::append(const QString& session_id,
                                                       qint64 expected_sequence,
                                                       const CommitBatch& batch) {
    if (!validText(session_id) || expected_sequence < 0 || !validText(batch.command_id) ||
        batch.command_json.isEmpty() || !validText(batch.recorded_at_utc) || batch.events.empty()) {
        return fail(StoreErrorCode::InvalidArgument, QStringLiteral("Invalid commit batch"));
    }
    if (batch.events.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return fail(StoreErrorCode::InvalidArgument, QStringLiteral("Too many events"));
    }
    for (const auto& event : batch.events) {
        if (!validText(event.event_type) || event.payload_json.isEmpty()) {
            return fail(StoreErrorCode::InvalidArgument, QStringLiteral("Invalid event"));
        }
    }
    for (const auto& entry : batch.docket_changes) {
        if (!validText(entry.entry_id) || !validText(entry.title) || !validText(entry.status) ||
            entry.source_event_offset < 0 ||
            entry.source_event_offset >= static_cast<qsizetype>(batch.events.size())) {
            return fail(StoreErrorCode::InvalidArgument, QStringLiteral("Invalid docket change"));
        }
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

    if (auto begun = beginImmediate(); !begun) {
        return std::unexpected(begun.error());
    }

    QSqlQuery current(database_);
    current.prepare(QStringLiteral("SELECT sequence FROM sessions WHERE session_id = ?"));
    current.addBindValue(session_id);
    if (!current.exec()) {
        rollback();
        return queryFailure(StoreErrorCode::QueryFailed, current,
                            QStringLiteral("read session sequence"));
    }
    if (!current.next()) {
        rollback();
        return fail(StoreErrorCode::NotFound, QStringLiteral("Session not found"));
    }
    if (current.value(0).toLongLong() != expected_sequence) {
        rollback();
        return fail(StoreErrorCode::StaleSequence, QStringLiteral("Stale session sequence"));
    }

    QSqlQuery command(database_);
    command.prepare(QStringLiteral(
        "INSERT INTO command_log(session_id, command_id, expected_sequence, payload_json, "
        "recorded_at_utc) VALUES(?, ?, ?, ?, ?)"));
    command.addBindValue(session_id);
    command.addBindValue(batch.command_id);
    command.addBindValue(expected_sequence);
    command.addBindValue(batch.command_json);
    command.addBindValue(batch.recorded_at_utc);
    if (!command.exec()) {
        rollback();
        return queryFailure(StoreErrorCode::ConstraintViolation, command,
                            QStringLiteral("append command"));
    }

    QSqlQuery event(database_);
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
            rollback();
            return queryFailure(StoreErrorCode::ConstraintViolation, event,
                                QStringLiteral("append event"));
        }
    }

    QSqlQuery docket(database_);
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
            rollback();
            return queryFailure(StoreErrorCode::ConstraintViolation, docket,
                                QStringLiteral("update docket projection"));
        }
    }

    QSqlQuery asset(database_);
    asset.prepare(QStringLiteral(
        "INSERT INTO asset_references(session_id, digest, purpose) VALUES(?, ?, ?)"));
    for (const auto& reference : batch.asset_references) {
        asset.bindValue(0, session_id);
        asset.bindValue(1, reference.digest);
        asset.bindValue(2, reference.purpose);
        if (!asset.exec()) {
            rollback();
            return queryFailure(StoreErrorCode::ConstraintViolation, asset,
                                QStringLiteral("link session asset"));
        }
    }

    const auto new_sequence = expected_sequence + static_cast<qint64>(batch.events.size());
    QSqlQuery update(database_);
    update.prepare(
        QStringLiteral("UPDATE sessions SET sequence = ? WHERE session_id = ? AND sequence = ?"));
    update.addBindValue(new_sequence);
    update.addBindValue(session_id);
    update.addBindValue(expected_sequence);
    if (!update.exec() || update.numRowsAffected() != 1) {
        rollback();
        return queryFailure(StoreErrorCode::StaleSequence, update,
                            QStringLiteral("advance session sequence"));
    }

    if (auto committed = commit(); !committed) {
        return std::unexpected(committed.error());
    }
    return new_sequence;
}

std::expected<SessionSnapshot, StoreError>
SessionStore::loadSession(const QString& session_id) const {
    QSqlQuery session(database_);
    session.prepare(
        QStringLiteral("SELECT engine_revision, sequence FROM sessions WHERE session_id = ?"));
    session.addBindValue(session_id);
    if (!session.exec()) {
        return queryFailure(StoreErrorCode::QueryFailed, session, QStringLiteral("load session"));
    }
    if (!session.next()) {
        return fail(StoreErrorCode::NotFound, QStringLiteral("Session not found"));
    }

    SessionSnapshot snapshot{
        session_id, session.value(0).toString(), session.value(1).toLongLong(), {}, {}, {}, {}};

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

int SessionStore::schemaVersion() const {
    QSqlQuery query(database_);
    if (!query.exec(QStringLiteral("SELECT COALESCE(MAX(version), 0) FROM schema_migrations")) ||
        !query.next()) {
        return -1;
    }
    return query.value(0).toInt();
}

std::expected<void, StoreError> SessionStore::beginImmediate() {
    return execStatement(database_, QStringLiteral("BEGIN IMMEDIATE"), StoreErrorCode::QueryFailed,
                         QStringLiteral("begin transaction"));
}

std::expected<void, StoreError> SessionStore::commit() {
    return execStatement(database_, QStringLiteral("COMMIT"), StoreErrorCode::QueryFailed,
                         QStringLiteral("commit transaction"));
}

void SessionStore::rollback() {
    QSqlQuery query(database_);
    static_cast<void>(query.exec(QStringLiteral("ROLLBACK")));
}

} // namespace appellate::storage
