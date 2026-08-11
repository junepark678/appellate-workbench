#include "appellate/storage/event_codec.hpp"
#include "appellate/storage/asset_store.hpp"
#include "appellate/storage/session_store.hpp"

#include <QFile>
#include <QFileInfo>
#include <QDirIterator>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMap>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include <limits>

#if defined(Q_OS_UNIX)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using appellate::storage::AssetReference;
using appellate::storage::CommitBatch;
using appellate::storage::DocketWrite;
using appellate::storage::EventWrite;
using appellate::storage::RevisionPin;
using appellate::storage::SessionAuthorityContract;
using appellate::storage::SessionStore;
using appellate::storage::StoredCommand;
using appellate::storage::StoreErrorCode;

constexpr auto digest = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr auto second_digest = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
constexpr auto frozen_legacy_command_json =
    R"({"schema_version":1,"command_type":"submit_filing","marker":"frozen-command-v1"})";
constexpr auto frozen_legacy_event_json =
    R"({"schema_version":1,"event_type":"filing.accepted","marker":"frozen-event-v1"})";

class SessionStoreTest final : public QObject {
    Q_OBJECT

  private slots:
    void migratesFreshDatabase();
    void migratesLegacySessionAsLegacyAuthorityContract();
    void refusesNewerSchemaWithoutMutation();
    void refusesMalformedDatabaseWithoutMutation();
    void refusesPreexistingZeroByteDatabaseWithoutMutation();
    void refusesPreexistingSchemaEmptySqliteWithoutMutation();
    void refusesCorruptUnusedIndexPageWithoutMutation();
    void refusesUnrelatedSqliteWithoutMutation();
    void refusesIncompleteMigrationLedgerWithoutMutation();
    void refusesSchemaWithStrippedCheckConstraintsWithoutMutation();
    void refusesSchema3WithoutAuthoritativeIdentityWithoutMutation();
    void refusesHardLinkedDatabaseAndSidecarWithoutMutation();
    void refusesFutureSchemaInWalWithoutMutation();
    void refusesForeignSchemaInWalWithoutMutation();
    void recoversCoherentlyCopiedHotRollbackJournal();
    void persistsAndReopensPinnedSession();
    void persistsCanonicalAuthorityContract();
    void rejectsStaleSequenceWithoutPartialWrite();
    void rollsBackDuplicateCommand();
    void rejectsInvalidAndDuplicateAssetReferencesWithoutWrites();
    void rejectsMalformedOrUnboundedCommitDataWithoutWrites();
    void rejectsInvalidSessionMetadataWithoutWrites();
    void rollsBackDuplicateStoredAssetReference();
    void pairedAppendBindsFreshStoreAndHealsMissingMarker();
    void pairedAppendRejectsInvalidHeadsBeforeCasPublication();
    void backsUpAndRestoresConsistentSnapshot();
    void rejectsDocumentBearingBackupWithoutPublishingDestination();
    void rejectsCorruptRestoreWithoutCreatingDestination();
    void rejectsLegacyDocumentBearingRestoreWithoutMutation();
    void rejectsSqliteSidecarBackupAndRestorePathsWithoutMutation();
    void rejectsLiveWalRestoreSourceWithoutMutation();
    void persistsRecordAccessAcrossCloseAndReopen();
    void createsSessionAndInitialBatchAtomically();
    void recoversUnreferencedFinalAndStagingCrashStates();
    void corruptOrphanFailsRecoveryWithoutCleanup();
    void missingReferencedAssetFailsRecoveryBeforeCleanup();
    void authoritativeDatabaseIdentityHealsOnlyMissingCasMarker();
    void legacyDatabaseBindsOnlyToExactVerifiedCasObjects();
    void oldCasMarkerIsNeverAdoptedByReplacementDatabase();
    void pairValidationFailurePreservesEveryCasEntry();
    void corruptUnboundCasFailsWithoutPublishingLock();
    void recoveryValidationFailurePreservesEarlierCleanupCandidates();
    void rejectsHardLinkedCasIdentityWithoutChangingAlias();
    void rejectsPartialCasIdentityWithoutReplacingIt();
    void cooperativeLockBlocksAnotherAppellateProcess();
    void ownerForksShareLeaseAndOutliveOwnerSafely();
    void simultaneousFreshAndLegacyNoLockOpenHaveOneBoundedWinner();
    void cooperativeLockHelper();
    void cooperativeRaceHelper();
};

struct FileImage final {
    bool exists{};
    QByteArray bytes;

    friend bool operator==(const FileImage&, const FileImage&) = default;
};

[[nodiscard]] FileImage fileImage(const QString& path) {
    if (!QFileInfo::exists(path)) {
        return {};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {true, {}};
    }
    return {true, file.readAll()};
}

[[nodiscard]] QMap<QString, FileImage> databaseImages(const QString& path) {
    QMap<QString, FileImage> images;
    for (const auto& suffix : {QString{}, QStringLiteral("-wal"), QStringLiteral("-shm"),
                               QStringLiteral("-journal")}) {
        images.insert(suffix, fileImage(path + suffix));
    }
    const QFileInfo database(path);
    const auto lock_path = QDir(database.absolutePath())
                               .filePath(QStringLiteral(".%1.appellate-open.lock")
                                             .arg(database.fileName()));
    images.insert(QStringLiteral("open-lock"), fileImage(lock_path));
    return images;
}

[[nodiscard]] QMap<QString, FileImage> treeImages(const QString& root) {
    QMap<QString, FileImage> images;
    if (!QFileInfo::exists(root)) {
        return images;
    }
    QDirIterator iterator(root, QDir::AllEntries | QDir::NoDotAndDotDot,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const auto path = iterator.next();
        const QFileInfo info(path);
        const auto relative = QDir(root).relativeFilePath(path);
        if (info.isSymLink()) {
            images.insert(relative,
                          FileImage{true, QByteArrayLiteral("symlink:") +
                                              QFile::encodeName(info.symLinkTarget())});
        } else if (info.isDir()) {
            images.insert(relative + u'/', FileImage{true, QByteArrayLiteral("directory")});
        } else {
            images.insert(relative, fileImage(path));
        }
    }
    return images;
}

[[nodiscard]] QString databaseIdentity(const QString& path) {
    const auto connection =
        QStringLiteral("identity-check-%1")
            .arg(QUuid::createUuid().toString(QUuid::Id128));
    QString identity;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        database.setDatabaseName(path);
        if (database.open()) {
            QSqlQuery query(database);
            if (query.exec(QStringLiteral(
                    "SELECT identity FROM store_identity WHERE singleton=1")) &&
                query.next()) {
                identity = query.value(0).toString();
            }
            database.close();
        }
        database = QSqlDatabase{};
    }
    QSqlDatabase::removeDatabase(connection);
    return identity;
}

[[nodiscard]] bool createFutureDatabase(const QString& path) {
    const auto connection =
        QStringLiteral("future-schema-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    bool created = false;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(path);
        if (database.open()) {
            QSqlQuery query(database);
            created =
                query.exec(
                    QStringLiteral("CREATE TABLE schema_migrations (version INTEGER PRIMARY KEY, "
                                   "applied_at_utc TEXT NOT NULL) STRICT")) &&
                query.exec(QStringLiteral("INSERT INTO schema_migrations VALUES(3, 'future')")) &&
                query.exec(
                    QStringLiteral("CREATE TABLE future_sentinel (value TEXT NOT NULL) STRICT")) &&
                query.exec(QStringLiteral("INSERT INTO future_sentinel VALUES('untouched')"));
            database.close();
        }
        database = QSqlDatabase{};
    }
    QSqlDatabase::removeDatabase(connection);
    return created;
}

[[nodiscard]] bool createLegacyDatabase(const QString& path) {
    const auto connection =
        QStringLiteral("legacy-schema-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    bool created = false;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(path);
        QFile migration(QStringLiteral(":/appellate/storage/migrations/001_initial.sql"));
        if (database.open() && migration.open(QIODevice::ReadOnly)) {
            created = true;
            const auto statements =
                QString::fromUtf8(migration.readAll()).split(u';', Qt::SkipEmptyParts);
            for (const auto& statement : statements) {
                if (!statement.trimmed().isEmpty()) {
                    QSqlQuery query(database);
                    created = created && query.exec(statement);
                }
            }
            QSqlQuery seed(database);
            created =
                created &&
                seed.exec(QStringLiteral(
                    "INSERT INTO schema_migrations VALUES(1, '2026-08-11T00:00:00Z')")) &&
                seed.exec(QStringLiteral(
                    "INSERT INTO sessions(session_id, engine_revision, sequence, created_at_utc) "
                    "VALUES('legacy-session', 'engine-legacy', 1, "
                    "'2026-08-11T00:00:00Z')")) &&
                seed.exec(QStringLiteral(
                    "INSERT INTO session_pins(session_id, pack_id, version, digest) VALUES("
                    "'legacy-session', 'example.appellate.ca4', '0.1.0', "
                    "'0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef')"));
            QSqlQuery command(database);
            command.prepare(QStringLiteral(
                "INSERT INTO command_log(session_id, command_id, expected_sequence, payload_json, "
                "recorded_at_utc) VALUES('legacy-session', 'legacy-command', 0, ?, "
                "'2026-08-11T00:00:00Z')"));
            command.addBindValue(QByteArray{frozen_legacy_command_json});
            QSqlQuery event(database);
            event.prepare(QStringLiteral(
                "INSERT INTO event_log(session_id, sequence, event_type, payload_json, "
                "authority_id) VALUES('legacy-session', 1, 'filing.accepted', ?, 'frap.3')"));
            event.addBindValue(QByteArray{frozen_legacy_event_json});
            created = created && command.exec() && event.exec();
            database.close();
        }
        database = QSqlDatabase{};
    }
    QSqlDatabase::removeDatabase(connection);
    return created;
}

[[nodiscard]] bool upgradeLegacyDatabaseToV2(const QString& path) {
    const auto connection =
        QStringLiteral("legacy-v2-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    bool upgraded = false;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(path);
        QFile migration(QStringLiteral(":/appellate/storage/migrations/"
                                       "002_session_authority_contract.sql"));
        if (database.open() && migration.open(QIODevice::ReadOnly)) {
            upgraded = true;
            const auto statements =
                QString::fromUtf8(migration.readAll()).split(u';', Qt::SkipEmptyParts);
            for (const auto& statement : statements) {
                if (!statement.trimmed().isEmpty()) {
                    QSqlQuery query(database);
                    upgraded = upgraded && query.exec(statement);
                }
            }
            QSqlQuery ledger(database);
            upgraded = upgraded && ledger.exec(QStringLiteral(
                                      "INSERT INTO schema_migrations VALUES(2, "
                                      "'2026-08-11T00:00:00Z')"));
            database.close();
        }
        database = QSqlDatabase{};
    }
    QSqlDatabase::removeDatabase(connection);
    return upgraded;
}

[[nodiscard]] bool futureDatabaseIsUntouched(const QString& path) {
    const auto connection =
        QStringLiteral("future-check-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    bool untouched = false;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(path);
        if (database.open()) {
            QSqlQuery query(database);
            untouched = query.exec(QStringLiteral("SELECT value FROM future_sentinel")) &&
                        query.next() && query.value(0).toString() == QStringLiteral("untouched");
            database.close();
        }
        database = QSqlDatabase{};
    }
    QSqlDatabase::removeDatabase(connection);
    return untouched;
}

[[nodiscard]] auto pins() -> std::vector<RevisionPin> {
    return {{QStringLiteral("example.appellate.ca4"), QStringLiteral("0.1.0"),
             QString::fromLatin1(digest)}};
}

[[nodiscard]] CommitBatch acceptedFiling(const QString& command_id) {
    return CommitBatch{
        command_id,
        QByteArrayLiteral(R"({"type":"submit_filing"})"),
        QStringLiteral("2026-08-11T00:00:00Z"),
        {EventWrite{QStringLiteral("filing.accepted"),
                    QByteArrayLiteral(R"({"filing_id":"notice-1"})"), QStringLiteral("frap.3")}},
        {DocketWrite{QStringLiteral("entry-1"), 0, QStringLiteral("Notice of appeal"),
                     QStringLiteral("filed")}},
        {},
    };
}

[[nodiscard]] CommitBatch acceptedFilingWithAssets(const QString& command_id) {
    auto batch = acceptedFiling(command_id);
    batch.asset_references = {
        AssetReference{QString::fromLatin1(digest), QStringLiteral("record.exhibit")},
        AssetReference{QString::fromLatin1(second_digest), QStringLiteral("brief.primary")},
    };
    return batch;
}

[[nodiscard]] CommitBatch singleAssetFiling(const QString& command_id,
                                            const QString& asset_digest) {
    return CommitBatch{
        command_id,
        QByteArrayLiteral(R"({"type":"submit_filing"})"),
        QStringLiteral("2026-08-11T00:00:00Z"),
        {EventWrite{QStringLiteral("filing.accepted"),
                    QByteArrayLiteral(R"({"filing_id":"notice-single"})"),
                    QStringLiteral("frap.3")}},
        {DocketWrite{QStringLiteral("entry-single"), 0, QStringLiteral("Notice of appeal"),
                     QStringLiteral("filed")}},
        {AssetReference{asset_digest, QStringLiteral("filing-document")}},
    };
}

void SessionStoreTest::migratesFreshDatabase() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto path = temporary.filePath(QStringLiteral("sessions.sqlite"));
    auto store = SessionStore::open(path);
    if (!store.has_value()) {
        QFAIL(qPrintable(store.error().message));
    }
    QCOMPARE((*store)->schemaVersion(), 3);
    store->reset();

    const auto connection =
        QStringLiteral("identity-check-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        database.setDatabaseName(path);
        QVERIFY(database.open());
        QSqlQuery query(database);
        QVERIFY(query.exec(QStringLiteral("PRAGMA application_id")) && query.next());
        QCOMPARE(query.value(0).toInt(), 1'095'784'258);
        QVERIFY(query.exec(QStringLiteral("PRAGMA user_version")) && query.next());
        QCOMPARE(query.value(0).toInt(), 3);
        database.close();
        database = QSqlDatabase{};
    }
    QSqlDatabase::removeDatabase(connection);
}

void SessionStoreTest::migratesLegacySessionAsLegacyAuthorityContract() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto path = temporary.filePath(QStringLiteral("legacy.sqlite"));
    const auto restored_path = temporary.filePath(QStringLiteral("restored.sqlite"));
    QVERIFY(createLegacyDatabase(path));
    QVERIFY(SessionStore::restoreBackup(path, restored_path).has_value());

    {
        auto migrated = SessionStore::open(restored_path);
        if (!migrated.has_value()) {
            QFAIL(qPrintable(migrated.error().message));
        }
        QCOMPARE((*migrated)->schemaVersion(), 3);
    }

    auto store = SessionStore::open(restored_path);
    QVERIFY(store.has_value());
    QCOMPARE((*store)->schemaVersion(), 3);
    const auto snapshot = (*store)->loadSession(QStringLiteral("legacy-session"));
    QVERIFY(snapshot.has_value());
    QCOMPARE(snapshot->engine_revision, QStringLiteral("engine-legacy"));
    QCOMPARE(snapshot->authority_contract, SessionAuthorityContract::LegacyV1);
    QCOMPARE(snapshot->pins, pins());
    QCOMPARE(snapshot->commands.size(), std::size_t{1});
    QCOMPARE(snapshot->commands.front().payload_json, QByteArray{frozen_legacy_command_json});
    QCOMPARE(snapshot->events.size(), std::size_t{1});
    QCOMPARE(snapshot->events.front().payload_json, QByteArray{frozen_legacy_event_json});
}

void SessionStoreTest::refusesNewerSchemaWithoutMutation() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto path = temporary.filePath(QStringLiteral("future.sqlite"));
    QVERIFY(createFutureDatabase(path));
    QFile before_file(path);
    QVERIFY(before_file.open(QIODevice::ReadOnly));
    const auto before = before_file.readAll();
    before_file.close();

    const auto opened = SessionStore::open(path);
    QVERIFY(!opened.has_value());
    QCOMPARE(opened.error().code, StoreErrorCode::MigrationFailed);
    QVERIFY(futureDatabaseIsUntouched(path));
    QFile after_file(path);
    QVERIFY(after_file.open(QIODevice::ReadOnly));
    QCOMPARE(after_file.readAll(), before);
    QVERIFY(!QFileInfo::exists(path + QStringLiteral("-wal")));
    QVERIFY(!QFileInfo::exists(path + QStringLiteral("-shm")));
}

void SessionStoreTest::refusesMalformedDatabaseWithoutMutation() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto path = temporary.filePath(QStringLiteral("malformed.sqlite"));
    const QByteArray malformed("not a SQLite database\0with frozen bytes", 39);
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(malformed), malformed.size());
    file.close();

    const auto opened = SessionStore::open(path);
    QVERIFY(!opened.has_value());
    QCOMPARE(opened.error().code, StoreErrorCode::MigrationFailed);
    QFile after_file(path);
    QVERIFY(after_file.open(QIODevice::ReadOnly));
    QCOMPARE(after_file.readAll(), malformed);
    QVERIFY(!QFileInfo::exists(path + QStringLiteral("-wal")));
    QVERIFY(!QFileInfo::exists(path + QStringLiteral("-shm")));
}

void SessionStoreTest::refusesPreexistingZeroByteDatabaseWithoutMutation() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto path = temporary.filePath(QStringLiteral("zero.sqlite"));
    QFile zero(path);
    QVERIFY(zero.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    zero.close();
    const auto before = databaseImages(path);

    const auto opened = SessionStore::open(path);
    QVERIFY(!opened.has_value());
    QCOMPARE(opened.error().code, StoreErrorCode::MigrationFailed);
    QCOMPARE(databaseImages(path), before);
}

void SessionStoreTest::refusesPreexistingSchemaEmptySqliteWithoutMutation() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto path = temporary.filePath(QStringLiteral("schema-empty.sqlite"));
    const auto connection =
        QStringLiteral("schema-empty-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(path);
        QVERIFY(database.open());
        QSqlQuery query(database);
        QVERIFY(query.exec(QStringLiteral("VACUUM")));
        database.close();
        database = QSqlDatabase{};
    }
    QSqlDatabase::removeDatabase(connection);
    QVERIFY(QFileInfo(path).size() >= 4096);
    const auto before = databaseImages(path);

    const auto opened = SessionStore::open(path);
    QVERIFY(!opened.has_value());
    QCOMPARE(opened.error().code, StoreErrorCode::MigrationFailed);
    QCOMPARE(databaseImages(path), before);
}

void SessionStoreTest::refusesCorruptUnusedIndexPageWithoutMutation() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto path = temporary.filePath(QStringLiteral("corrupt-index.sqlite"));
    auto initialized = SessionStore::open(path);
    QVERIFY(initialized.has_value());
    initialized->reset();

    qint64 page_number{};
    qint64 page_size{};
    const auto connection =
        QStringLiteral("index-page-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(path);
        QVERIFY(database.open());
        QSqlQuery query(database);
        QVERIFY(query.exec(QStringLiteral("PRAGMA page_size")) && query.next());
        page_size = query.value(0).toLongLong();
        QVERIFY(page_size >= 512);
        QVERIFY(query.exec(QStringLiteral(
            "SELECT pageno FROM dbstat WHERE name='sqlite_autoindex_session_pins_1' "
            "ORDER BY pageno LIMIT 1")) &&
                query.next());
        page_number = query.value(0).toLongLong();
        QVERIFY(page_number > 1);
        database.close();
        database = QSqlDatabase{};
    }
    QSqlDatabase::removeDatabase(connection);

    QFile corrupt(path);
    QVERIFY(corrupt.open(QIODevice::ReadWrite));
    QVERIFY(corrupt.seek((page_number - 1) * page_size));
    const auto original_type = corrupt.read(1);
    QCOMPARE(original_type.size(), 1);
    QVERIFY(corrupt.seek((page_number - 1) * page_size));
    QCOMPARE(corrupt.write(QByteArray(1, static_cast<char>(0x7f))), qint64{1});
    corrupt.close();
    const auto before = databaseImages(path);

    const auto opened = SessionStore::open(path);
    QVERIFY(!opened.has_value());
    QCOMPARE(opened.error().code, StoreErrorCode::MigrationFailed);
    QCOMPARE(databaseImages(path), before);
}

void SessionStoreTest::refusesUnrelatedSqliteWithoutMutation() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto path = temporary.filePath(QStringLiteral("unrelated.sqlite"));
    const auto connection =
        QStringLiteral("unrelated-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(path);
        QVERIFY(database.open());
        QSqlQuery query(database);
        QVERIFY(query.exec(QStringLiteral(
            "CREATE TABLE unrelated_sentinel(value TEXT NOT NULL) STRICT")));
        QVERIFY(query.exec(QStringLiteral(
            "INSERT INTO unrelated_sentinel VALUES('must-remain')")));
        database.close();
        database = QSqlDatabase{};
    }
    QSqlDatabase::removeDatabase(connection);
    const auto before = databaseImages(path);

    const auto opened = SessionStore::open(path);
    QVERIFY(!opened.has_value());
    QCOMPARE(opened.error().code, StoreErrorCode::MigrationFailed);
    QCOMPARE(databaseImages(path), before);
}

void SessionStoreTest::refusesIncompleteMigrationLedgerWithoutMutation() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto path = temporary.filePath(QStringLiteral("incomplete.sqlite"));
    auto initialized = SessionStore::open(path);
    QVERIFY(initialized.has_value());
    initialized->reset();

    const auto connection =
        QStringLiteral("ledger-gap-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(path);
        QVERIFY(database.open());
        QSqlQuery query(database);
        QVERIFY(query.exec(QStringLiteral("DELETE FROM schema_migrations WHERE version=2")));
        database.close();
        database = QSqlDatabase{};
    }
    QSqlDatabase::removeDatabase(connection);
    const auto before = databaseImages(path);

    const auto opened = SessionStore::open(path);
    QVERIFY(!opened.has_value());
    QCOMPARE(opened.error().code, StoreErrorCode::MigrationFailed);
    QCOMPARE(databaseImages(path), before);
}

void SessionStoreTest::refusesSchemaWithStrippedCheckConstraintsWithoutMutation() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto path = temporary.filePath(QStringLiteral("stripped-checks.sqlite"));
    auto initialized = SessionStore::open(path);
    QVERIFY(initialized.has_value());
    initialized->reset();

    const auto connection =
        QStringLiteral("strip-checks-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(path);
        QVERIFY(database.open());
        QSqlQuery query(database);
        QVERIFY(query.exec(QStringLiteral("PRAGMA writable_schema=ON")));
        QVERIFY(query.exec(QStringLiteral(
            "UPDATE sqlite_schema SET sql=replace(sql, "
            "' CHECK(length(digest) = 64)', '') WHERE sql LIKE '%length(digest)%'")));
        QVERIFY(query.numRowsAffected() >= 3);
        QVERIFY(query.exec(QStringLiteral("PRAGMA schema_version")) && query.next());
        const auto schema_version = query.value(0).toInt();
        QVERIFY(query.exec(QStringLiteral("PRAGMA schema_version=%1").arg(schema_version + 1)));
        QVERIFY(query.exec(QStringLiteral("PRAGMA writable_schema=OFF")));
        database.close();
        database = QSqlDatabase{};
    }
    QSqlDatabase::removeDatabase(connection);
    const auto before = databaseImages(path);

    const auto opened = SessionStore::open(path);
    QVERIFY(!opened.has_value());
    QCOMPARE(opened.error().code, StoreErrorCode::MigrationFailed);
    QCOMPARE(databaseImages(path), before);
}

void SessionStoreTest::refusesSchema3WithoutAuthoritativeIdentityWithoutMutation() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto path = temporary.filePath(QStringLiteral("missing-identity.sqlite"));
    auto initialized = SessionStore::open(path);
    QVERIFY(initialized.has_value());
    initialized->reset();

    const auto connection =
        QStringLiteral("remove-identity-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(path);
        QVERIFY(database.open());
        QSqlQuery query(database);
        QVERIFY(query.exec(QStringLiteral("DELETE FROM store_identity")));
        database.close();
        database = QSqlDatabase{};
    }
    QSqlDatabase::removeDatabase(connection);
    const auto before = databaseImages(path);

    const auto opened = SessionStore::open(path);
    QVERIFY(!opened.has_value());
    QCOMPARE(opened.error().code, StoreErrorCode::MigrationFailed);
    QCOMPARE(databaseImages(path), before);
}

void SessionStoreTest::refusesHardLinkedDatabaseAndSidecarWithoutMutation() {
#if !defined(Q_OS_UNIX)
    QSKIP("Hard-link database boundary is Unix-only");
#else
    QTemporaryDir temporary;
    QTemporaryDir outside;
    QVERIFY(temporary.isValid());
    QVERIFY(outside.isValid());
    const auto path = temporary.filePath(QStringLiteral("hardlink.sqlite"));
    auto initialized = SessionStore::open(path);
    QVERIFY(initialized.has_value());
    initialized->reset();

    const auto main_alias = outside.filePath(QStringLiteral("main-alias"));
    QCOMPARE(::link(QFile::encodeName(path).constData(),
                    QFile::encodeName(main_alias).constData()),
             0);
    auto before = databaseImages(path);
    auto rejected = SessionStore::open(path);
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, StoreErrorCode::OpenFailed);
    QCOMPARE(databaseImages(path), before);
    QVERIFY(QFile::remove(main_alias));

    const auto wal_path = path + QStringLiteral("-wal");
    QFile wal(wal_path);
    QVERIFY(wal.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(wal.write("untrusted-wal"), qint64{13});
    wal.close();
    const auto wal_alias = outside.filePath(QStringLiteral("wal-alias"));
    QCOMPARE(::link(QFile::encodeName(wal_path).constData(),
                    QFile::encodeName(wal_alias).constData()),
             0);
    before = databaseImages(path);
    rejected = SessionStore::open(path);
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, StoreErrorCode::OpenFailed);
    QCOMPARE(databaseImages(path), before);
    QVERIFY(QFile::remove(wal_alias));
    QVERIFY(QFile::remove(wal_path));

    const QFileInfo database(path);
    const auto lock_path = QDir(database.absolutePath())
                               .filePath(QStringLiteral(".%1.appellate-open.lock")
                                             .arg(database.fileName()));
    const auto lock_alias = outside.filePath(QStringLiteral("lock-alias"));
    QCOMPARE(::link(QFile::encodeName(lock_path).constData(),
                    QFile::encodeName(lock_alias).constData()),
             0);
    before = databaseImages(path);
    rejected = SessionStore::open(path);
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, StoreErrorCode::OpenFailed);
    QCOMPARE(databaseImages(path), before);
    QVERIFY(QFile::remove(lock_alias));
#endif
}

void SessionStoreTest::refusesFutureSchemaInWalWithoutMutation() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto path = temporary.filePath(QStringLiteral("future-wal.sqlite"));
    auto initialized = SessionStore::open(path);
    QVERIFY(initialized.has_value());
    initialized->reset();

    const auto connection =
        QStringLiteral("future-wal-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
    database.setDatabaseName(path);
    QVERIFY(database.open());
    QSqlQuery query(database);
    QVERIFY(query.exec(QStringLiteral("PRAGMA journal_mode=WAL")) && query.next());
    QVERIFY(query.exec(QStringLiteral("PRAGMA wal_autocheckpoint=0")));
    QVERIFY(query.exec(QStringLiteral("BEGIN IMMEDIATE")));
    QVERIFY(query.exec(QStringLiteral(
        "INSERT INTO schema_migrations VALUES(4, '2026-08-12T00:00:00Z')")));
    QVERIFY(query.exec(QStringLiteral("PRAGMA user_version=4")));
    QVERIFY(query.exec(QStringLiteral("COMMIT")));
    QVERIFY(QFileInfo::exists(path + QStringLiteral("-wal")));
    QVERIFY(QFileInfo::exists(path + QStringLiteral("-shm")));
    const auto before = databaseImages(path);

    const auto opened = SessionStore::open(path);
    QVERIFY(!opened.has_value());
    QCOMPARE(opened.error().code, StoreErrorCode::MigrationFailed);
    QCOMPARE(databaseImages(path), before);

    database.close();
    database = QSqlDatabase{};
    QSqlDatabase::removeDatabase(connection);
}

void SessionStoreTest::refusesForeignSchemaInWalWithoutMutation() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto path = temporary.filePath(QStringLiteral("foreign-wal.sqlite"));
    auto initialized = SessionStore::open(path);
    QVERIFY(initialized.has_value());
    initialized->reset();

    const auto connection =
        QStringLiteral("foreign-wal-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
    database.setDatabaseName(path);
    QVERIFY(database.open());
    QSqlQuery query(database);
    QVERIFY(query.exec(QStringLiteral("PRAGMA journal_mode=WAL")) && query.next());
    QVERIFY(query.exec(QStringLiteral("PRAGMA wal_autocheckpoint=0")));
    QVERIFY(query.exec(QStringLiteral(
        "CREATE TABLE foreign_sentinel(value TEXT NOT NULL) STRICT")));
    QVERIFY(query.exec(QStringLiteral("INSERT INTO foreign_sentinel VALUES('untouched')")));
    const auto before = databaseImages(path);

    const auto opened = SessionStore::open(path);
    QVERIFY(!opened.has_value());
    QCOMPARE(opened.error().code, StoreErrorCode::MigrationFailed);
    QCOMPARE(databaseImages(path), before);

    database.close();
    database = QSqlDatabase{};
    QSqlDatabase::removeDatabase(connection);
}

void SessionStoreTest::recoversCoherentlyCopiedHotRollbackJournal() {
#if !defined(Q_OS_UNIX)
    QSKIP("Hot rollback-journal crash construction is Unix-only");
#else
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto path = temporary.filePath(QStringLiteral("hot-journal.sqlite"));
    {
        auto initialized = SessionStore::open(path);
        QVERIFY(initialized.has_value());
        QVERIFY((*initialized)
                    ->createSession(QStringLiteral("hot.journal.session"),
                                    QStringLiteral("engine.workflow.v1"),
                                    QStringLiteral("2026-08-12T00:00:00Z"), pins(),
                                    SessionAuthorityContract::CanonicalV2)
                    .has_value());
    }

    const auto child = ::fork();
    QVERIFY(child >= 0);
    if (child == 0) {
        const auto connection = QStringLiteral("hot-journal-child");
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(path);
        QSqlQuery query(database);
        const bool okay = database.open() &&
                          query.exec(QStringLiteral("PRAGMA journal_mode=DELETE")) &&
                          query.next() && query.exec(QStringLiteral("PRAGMA synchronous=FULL")) &&
                          query.exec(QStringLiteral("BEGIN IMMEDIATE")) &&
                          query.exec(QStringLiteral(
                              "UPDATE sessions SET sequence=77 WHERE "
                              "session_id='hot.journal.session'"));
        ::_exit(okay ? 0 : 2);
    }
    int status{};
    QCOMPARE(::waitpid(child, &status, 0), child);
    QVERIFY(WIFEXITED(status));
    QCOMPARE(WEXITSTATUS(status), 0);
    QVERIFY(QFileInfo::exists(path + QStringLiteral("-journal")));

    auto reopened = SessionStore::open(path);
    if (!reopened) {
        QFAIL(qPrintable(reopened.error().message));
    }
    const auto snapshot = (*reopened)->loadSession(QStringLiteral("hot.journal.session"));
    QVERIFY(snapshot.has_value());
    QCOMPARE(snapshot->sequence, qint64{0});
    QVERIFY(snapshot->commands.empty());
    QVERIFY(snapshot->events.empty());
#endif
}

void SessionStoreTest::persistsAndReopensPinnedSession() {
    QTemporaryDir temporary;
    const auto path = temporary.filePath(QStringLiteral("sessions.sqlite"));
    {
        auto store = SessionStore::open(path);
        QVERIFY(store.has_value());
        QVERIFY((*store)
                    ->createSession(QStringLiteral("session-1"), QStringLiteral("engine-1"),
                                    QStringLiteral("2026-08-11T00:00:00Z"), pins())
                    .has_value());
        const auto sequence = (*store)->append(QStringLiteral("session-1"), 0,
                                               acceptedFiling(QStringLiteral("command-1")));
        QVERIFY(sequence.has_value());
        QCOMPARE(*sequence, 1);
    }

    auto reopened = SessionStore::open(path);
    QVERIFY(reopened.has_value());
    const auto snapshot = (*reopened)->loadSession(QStringLiteral("session-1"));
    QVERIFY(snapshot.has_value());
    QCOMPARE(snapshot->engine_revision, QStringLiteral("engine-1"));
    QCOMPARE(snapshot->authority_contract, SessionAuthorityContract::LegacyV1);
    QCOMPARE(snapshot->sequence, 1);
    QCOMPARE(snapshot->pins, pins());
    const std::vector expected_commands{
        StoredCommand{QStringLiteral("command-1"), 0,
                      QByteArrayLiteral(R"({"type":"submit_filing"})"),
                      QStringLiteral("2026-08-11T00:00:00Z")},
    };
    QCOMPARE(snapshot->commands, expected_commands);
    QCOMPARE(snapshot->events.size(), std::size_t{1});
    QCOMPARE(snapshot->docket.size(), std::size_t{1});
    QCOMPARE(snapshot->docket.front().title, QStringLiteral("Notice of appeal"));
    QVERIFY(snapshot->asset_references.empty());
}

void SessionStoreTest::persistsCanonicalAuthorityContract() {
    QTemporaryDir temporary;
    auto store = SessionStore::open(temporary.filePath(QStringLiteral("sessions.sqlite")));
    QVERIFY(store.has_value());
    QVERIFY((*store)
                ->createSession(QStringLiteral("session-canonical"), QStringLiteral("engine-1"),
                                QStringLiteral("2026-08-11T00:00:00Z"), pins(),
                                SessionAuthorityContract::CanonicalV2)
                .has_value());

    const auto snapshot = (*store)->loadSession(QStringLiteral("session-canonical"));
    QVERIFY(snapshot.has_value());
    QCOMPARE(snapshot->authority_contract, SessionAuthorityContract::CanonicalV2);
}

void SessionStoreTest::rejectsStaleSequenceWithoutPartialWrite() {
    QTemporaryDir temporary;
    auto store = SessionStore::open(temporary.filePath(QStringLiteral("sessions.sqlite")));
    QVERIFY(store.has_value());
    QVERIFY((*store)
                ->createSession(QStringLiteral("session-1"), QStringLiteral("engine-1"),
                                QStringLiteral("2026-08-11T00:00:00Z"), pins())
                .has_value());
    QVERIFY(
        (*store)
            ->append(QStringLiteral("session-1"), 0, acceptedFiling(QStringLiteral("command-1")))
            .has_value());

    const auto stale = (*store)->append(QStringLiteral("session-1"), 0,
                                        acceptedFiling(QStringLiteral("command-2")));
    QVERIFY(!stale.has_value());
    QCOMPARE(stale.error().code, StoreErrorCode::StaleSequence);

    const auto snapshot = (*store)->loadSession(QStringLiteral("session-1"));
    QVERIFY(snapshot.has_value());
    QCOMPARE(snapshot->sequence, 1);
    QCOMPARE(snapshot->commands.size(), std::size_t{1});
    QCOMPARE(snapshot->events.size(), std::size_t{1});
}

void SessionStoreTest::rollsBackDuplicateCommand() {
    QTemporaryDir temporary;
    auto store = SessionStore::open(temporary.filePath(QStringLiteral("sessions.sqlite")));
    QVERIFY(store.has_value());
    QVERIFY((*store)
                ->createSession(QStringLiteral("session-1"), QStringLiteral("engine-1"),
                                QStringLiteral("2026-08-11T00:00:00Z"), pins())
                .has_value());
    QVERIFY(
        (*store)
            ->append(QStringLiteral("session-1"), 0, acceptedFiling(QStringLiteral("command-1")))
            .has_value());

    const auto duplicate = (*store)->append(QStringLiteral("session-1"), 1,
                                            acceptedFiling(QStringLiteral("command-1")));
    QVERIFY(!duplicate.has_value());
    QCOMPARE(duplicate.error().code, StoreErrorCode::ConstraintViolation);

    const auto snapshot = (*store)->loadSession(QStringLiteral("session-1"));
    QVERIFY(snapshot.has_value());
    QCOMPARE(snapshot->sequence, 1);
    QCOMPARE(snapshot->commands.size(), std::size_t{1});
    QCOMPARE(snapshot->events.size(), std::size_t{1});
    QCOMPARE(snapshot->docket.size(), std::size_t{1});
    QVERIFY(snapshot->asset_references.empty());
}

void SessionStoreTest::rejectsInvalidAndDuplicateAssetReferencesWithoutWrites() {
    QTemporaryDir temporary;
    auto store = SessionStore::open(temporary.filePath(QStringLiteral("sessions.sqlite")));
    QVERIFY(store.has_value());
    QVERIFY((*store)
                ->createSession(QStringLiteral("session-1"), QStringLiteral("engine-1"),
                                QStringLiteral("2026-08-11T00:00:00Z"), pins())
                .has_value());

    auto invalid_digest = acceptedFilingWithAssets(QStringLiteral("command-invalid-digest"));
    invalid_digest.asset_references.front().digest = QString(64, u'A');
    auto result = (*store)->append(QStringLiteral("session-1"), 0, invalid_digest);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, StoreErrorCode::InvalidArgument);

    auto invalid_purpose = acceptedFilingWithAssets(QStringLiteral("command-invalid-purpose"));
    invalid_purpose.asset_references.front().purpose = QStringLiteral("Record Exhibit");
    result = (*store)->append(QStringLiteral("session-1"), 0, invalid_purpose);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, StoreErrorCode::InvalidArgument);

    auto oversized_purpose = acceptedFilingWithAssets(QStringLiteral("command-oversized-purpose"));
    oversized_purpose.asset_references.front().purpose = QString(129, u'a');
    result = (*store)->append(QStringLiteral("session-1"), 0, oversized_purpose);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, StoreErrorCode::InvalidArgument);

    auto duplicate = acceptedFilingWithAssets(QStringLiteral("command-duplicate-reference"));
    duplicate.asset_references.push_back(duplicate.asset_references.front());
    result = (*store)->append(QStringLiteral("session-1"), 0, duplicate);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, StoreErrorCode::InvalidArgument);

    const auto snapshot = (*store)->loadSession(QStringLiteral("session-1"));
    QVERIFY(snapshot.has_value());
    QCOMPARE(snapshot->sequence, 0);
    QVERIFY(snapshot->commands.empty());
    QVERIFY(snapshot->events.empty());
    QVERIFY(snapshot->docket.empty());
    QVERIFY(snapshot->asset_references.empty());
}

void SessionStoreTest::rejectsMalformedOrUnboundedCommitDataWithoutWrites() {
    QTemporaryDir temporary;
    auto store = SessionStore::open(temporary.filePath(QStringLiteral("sessions.sqlite")));
    QVERIFY(store.has_value());
    QVERIFY((*store)
                ->createSession(QStringLiteral("session-1"), QStringLiteral("engine-1"),
                                QStringLiteral("2026-08-11T00:00:00Z"), pins())
                .has_value());

    auto batch = acceptedFiling(QStringLiteral("malformed-command"));
    batch.command_json = QByteArrayLiteral("not-json");
    auto result = (*store)->append(QStringLiteral("session-1"), 0, batch);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, StoreErrorCode::InvalidArgument);

    batch = acceptedFiling(QStringLiteral("oversized-command"));
    batch.command_json = QByteArray(1024 * 1024 + 1, 'x');
    result = (*store)->append(QStringLiteral("session-1"), 0, batch);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, StoreErrorCode::InvalidArgument);

    batch = acceptedFiling(QStringLiteral("malformed-event"));
    batch.events.front().payload_json = QByteArrayLiteral("[]");
    result = (*store)->append(QStringLiteral("session-1"), 0, batch);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, StoreErrorCode::InvalidArgument);

    batch = acceptedFiling(QStringLiteral("missing-authority"));
    batch.events.front().authority_id.clear();
    result = (*store)->append(QStringLiteral("session-1"), 0, batch);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, StoreErrorCode::InvalidArgument);

    batch = acceptedFiling(QStringLiteral("duplicate-docket"));
    batch.docket_changes.push_back(batch.docket_changes.front());
    result = (*store)->append(QStringLiteral("session-1"), 0, batch);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, StoreErrorCode::InvalidArgument);

    batch = acceptedFiling(QStringLiteral("overflow"));
    result =
        (*store)->append(QStringLiteral("session-1"), std::numeric_limits<qint64>::max(), batch);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, StoreErrorCode::InvalidArgument);

    const auto snapshot = (*store)->loadSession(QStringLiteral("session-1"));
    QVERIFY(snapshot.has_value());
    QCOMPARE(snapshot->sequence, qint64{0});
    QVERIFY(snapshot->commands.empty());
    QVERIFY(snapshot->events.empty());
    QVERIFY(snapshot->docket.empty());
}

void SessionStoreTest::rejectsInvalidSessionMetadataWithoutWrites() {
    QTemporaryDir temporary;
    auto store = SessionStore::open(temporary.filePath(QStringLiteral("sessions.sqlite")));
    QVERIFY(store.has_value());

    const auto invalid_time =
        (*store)->createSession(QStringLiteral("session-1"), QStringLiteral("engine-1"),
                                QStringLiteral("2026-08-11T00:00:00+00:00"), pins());
    QVERIFY(!invalid_time.has_value());
    QCOMPARE(invalid_time.error().code, StoreErrorCode::InvalidArgument);

    const auto empty_pins =
        (*store)->createSession(QStringLiteral("session-1"), QStringLiteral("engine-1"),
                                QStringLiteral("2026-08-11T00:00:00Z"), {});
    QVERIFY(!empty_pins.has_value());
    QCOMPARE(empty_pins.error().code, StoreErrorCode::InvalidArgument);

    auto duplicate_pins = pins();
    duplicate_pins.push_back(duplicate_pins.front());
    const auto duplicate =
        (*store)->createSession(QStringLiteral("session-1"), QStringLiteral("engine-1"),
                                QStringLiteral("2026-08-11T00:00:00Z"), duplicate_pins);
    QVERIFY(!duplicate.has_value());
    QCOMPARE(duplicate.error().code, StoreErrorCode::InvalidArgument);

    const auto invalid_contract = (*store)->createSession(
        QStringLiteral("session-1"), QStringLiteral("engine-1"),
        QStringLiteral("2026-08-11T00:00:00Z"), pins(), static_cast<SessionAuthorityContract>(99));
    QVERIFY(!invalid_contract.has_value());
    QCOMPARE(invalid_contract.error().code, StoreErrorCode::InvalidArgument);

    const auto missing = (*store)->loadSession(QStringLiteral("session-1"));
    QVERIFY(!missing.has_value());
    QCOMPARE(missing.error().code, StoreErrorCode::NotFound);
}

void SessionStoreTest::rollsBackDuplicateStoredAssetReference() {
    QTemporaryDir temporary;
    const auto database_path = temporary.filePath(QStringLiteral("sessions.sqlite"));
    const auto asset_root = temporary.filePath(QStringLiteral("assets"));
    appellate::storage::AssetStore assets(asset_root, 1024);
    auto store = SessionStore::open(database_path);
    QVERIFY(store.has_value());
    QVERIFY((*store)->recoverAssetStore(assets).has_value());
    QVERIFY((*store)
                ->createSession(QStringLiteral("session-1"), QStringLiteral("engine-1"),
                                QStringLiteral("2026-08-11T00:00:00Z"), pins())
                .has_value());
    const QByteArray first_bytes("first-paired-asset");
    auto first_staged = assets.stage(first_bytes);
    QVERIFY(first_staged.has_value());
    auto first_batch = singleAssetFiling(QStringLiteral("command-1"), first_staged->sha256());
    QVERIFY((*store)
                ->appendWithStagedAsset(QStringLiteral("session-1"), 0, first_batch, assets,
                                        *first_staged)
                .has_value());

    auto duplicate_staged = assets.stage(first_bytes);
    QVERIFY(duplicate_staged.has_value());
    auto duplicate =
        singleAssetFiling(QStringLiteral("command-2"), duplicate_staged->sha256());
    const auto rejected = (*store)->appendWithStagedAsset(
        QStringLiteral("session-1"), 1, duplicate, assets, *duplicate_staged);
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, StoreErrorCode::ConstraintViolation);

    auto snapshot = (*store)->loadSession(QStringLiteral("session-1"));
    QVERIFY(snapshot.has_value());
    QCOMPARE(snapshot->sequence, 1);
    QCOMPARE(snapshot->commands.size(), std::size_t{1});
    QCOMPARE(snapshot->events.size(), std::size_t{1});
    QCOMPARE(snapshot->asset_references.size(), std::size_t{1});

    const QByteArray second_bytes("second-paired-asset");
    auto second_staged = assets.stage(second_bytes);
    QVERIFY(second_staged.has_value());
    auto second_batch =
        singleAssetFiling(QStringLiteral("command-2"), second_staged->sha256());
    const auto retried = (*store)->appendWithStagedAsset(
        QStringLiteral("session-1"), 1, second_batch, assets, *second_staged);
    QVERIFY(retried.has_value());
    QCOMPARE(*retried, 2);

    snapshot = (*store)->loadSession(QStringLiteral("session-1"));
    QVERIFY(snapshot.has_value());
    QCOMPARE(snapshot->sequence, 2);
    QCOMPARE(snapshot->commands.size(), std::size_t{2});
    QCOMPARE(snapshot->events.size(), std::size_t{2});
    QCOMPARE(snapshot->asset_references.size(), std::size_t{2});
}

void SessionStoreTest::pairedAppendBindsFreshStoreAndHealsMissingMarker() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto database_path = temporary.filePath(QStringLiteral("paired-first.sqlite"));
    const auto asset_root = temporary.filePath(QStringLiteral("assets"));
    const auto marker_path =
        QDir(asset_root).filePath(QStringLiteral(".appellate-store-id"));
    appellate::storage::AssetStore assets(asset_root, 1024);
    auto store = SessionStore::open(database_path);
    QVERIFY(store.has_value());
    QVERIFY((*store)
                ->createSession(QStringLiteral("paired.session"),
                                QStringLiteral("engine.workflow.v1"),
                                QStringLiteral("2026-08-12T00:00:00Z"), pins(),
                                SessionAuthorityContract::CanonicalV2)
                .has_value());

    auto first = assets.stage(QByteArrayLiteral("first direct paired asset"));
    QVERIFY(first.has_value());
    auto first_batch =
        singleAssetFiling(QStringLiteral("paired.command.1"), first->sha256());
    QVERIFY((*store)
                ->appendWithStagedAsset(QStringLiteral("paired.session"), 0, first_batch, assets,
                                        *first)
                .has_value());
    QVERIFY(QFileInfo::exists(marker_path));
    QVERIFY(assets.read(first->sha256()).has_value());
    const auto authoritative_marker = fileImage(marker_path);

    QVERIFY(QFile::remove(marker_path));
    auto second = assets.stage(QByteArrayLiteral("second direct paired asset"));
    QVERIFY(second.has_value());
    auto second_batch =
        singleAssetFiling(QStringLiteral("paired.command.2"), second->sha256());
    QVERIFY((*store)
                ->appendWithStagedAsset(QStringLiteral("paired.session"), 1, second_batch, assets,
                                        *second)
                .has_value());
    QCOMPARE(fileImage(marker_path), authoritative_marker);
    QVERIFY(assets.read(first->sha256()).has_value());
    QVERIFY(assets.read(second->sha256()).has_value());
    const auto snapshot = (*store)->loadSession(QStringLiteral("paired.session"));
    QVERIFY(snapshot.has_value());
    QCOMPARE(snapshot->sequence, qint64{2});
    QCOMPARE(snapshot->asset_references.size(), std::size_t{2});
}

void SessionStoreTest::pairedAppendRejectsInvalidHeadsBeforeCasPublication() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    for (const auto& mode : {QStringLiteral("missing"), QStringLiteral("stale"),
                             QStringLiteral("duplicate")}) {
        const auto root = temporary.filePath(mode);
        QVERIFY(QDir{}.mkpath(root));
        const auto database_path = QDir(root).filePath(QStringLiteral("sessions.sqlite"));
        const auto asset_root = QDir(root).filePath(QStringLiteral("assets"));
        appellate::storage::AssetStore assets(asset_root, 1024);
        auto store = SessionStore::open(database_path);
        QVERIFY(store.has_value());
        if (mode != QStringLiteral("missing")) {
            QVERIFY((*store)
                        ->createSession(QStringLiteral("paired.invalid.session"),
                                        QStringLiteral("engine.workflow.v1"),
                                        QStringLiteral("2026-08-12T00:00:00Z"), pins(),
                                        SessionAuthorityContract::CanonicalV2)
                        .has_value());
        }
        if (mode == QStringLiteral("duplicate")) {
            QVERIFY((*store)
                        ->append(QStringLiteral("paired.invalid.session"), 0,
                                 acceptedFiling(QStringLiteral("paired.invalid.command")))
                        .has_value());
        }
        auto staged = assets.stage((mode + QStringLiteral(" staged bytes")).toUtf8());
        QVERIFY(staged.has_value());
        auto batch =
            singleAssetFiling(QStringLiteral("paired.invalid.command"), staged->sha256());
        const auto cas_before = treeImages(asset_root);
        const auto session_before = (*store)->loadSession(
            mode == QStringLiteral("missing") ? QStringLiteral("absent.session")
                                               : QStringLiteral("paired.invalid.session"));

        const auto rejected = (*store)->appendWithStagedAsset(
            mode == QStringLiteral("missing") ? QStringLiteral("absent.session")
                                               : QStringLiteral("paired.invalid.session"),
            mode == QStringLiteral("stale") ? 1 :
            mode == QStringLiteral("duplicate") ? 1 : 0,
            batch, assets, *staged);
        QVERIFY(!rejected.has_value());
        QCOMPARE(rejected.error().code,
                 mode == QStringLiteral("missing")
                     ? StoreErrorCode::NotFound
                     : mode == QStringLiteral("stale") ? StoreErrorCode::StaleSequence
                                                         : StoreErrorCode::ConstraintViolation);
        QCOMPARE(treeImages(asset_root), cas_before);
        QVERIFY(!QFileInfo::exists(
            QDir(asset_root).filePath(QStringLiteral(".appellate-store-id"))));
        QVERIFY(!QFileInfo::exists(
            QDir(asset_root).filePath(QStringLiteral(".cas.lock"))));
        const auto session_after = (*store)->loadSession(
            mode == QStringLiteral("missing") ? QStringLiteral("absent.session")
                                               : QStringLiteral("paired.invalid.session"));
        QCOMPARE(session_after.has_value(), session_before.has_value());
        if (session_before && session_after) {
            QCOMPARE(session_after->created_at_utc, session_before->created_at_utc);
            QCOMPARE(session_after->sequence, session_before->sequence);
            QCOMPARE(session_after->pins, session_before->pins);
            QCOMPARE(session_after->commands, session_before->commands);
            QCOMPARE(session_after->events, session_before->events);
            QCOMPARE(session_after->docket, session_before->docket);
            QCOMPARE(session_after->asset_references, session_before->asset_references);
        }
    }
}

void SessionStoreTest::backsUpAndRestoresConsistentSnapshot() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto live_path = temporary.filePath(QStringLiteral("live.sqlite"));
    const auto backup_path = temporary.filePath(QStringLiteral("backup.sqlite"));
    const auto restored_path = temporary.filePath(QStringLiteral("restored.sqlite"));
    const auto original_asset_root = temporary.filePath(QStringLiteral("original-assets"));
    const auto restored_asset_root = temporary.filePath(QStringLiteral("restored-assets"));
    appellate::storage::AssetStore original_assets(original_asset_root, 1024);
    {
        auto store = SessionStore::open(live_path);
        QVERIFY(store.has_value());
        QVERIFY((*store)->recoverAssetStore(original_assets).has_value());
        QVERIFY((*store)
                    ->createSession(QStringLiteral("session-1"), QStringLiteral("engine-1"),
                                    QStringLiteral("2026-08-11T00:00:00Z"), pins())
                    .has_value());
        QVERIFY((*store)
                    ->append(QStringLiteral("session-1"), 0,
                             acceptedFiling(QStringLiteral("command-1")))
                    .has_value());
        const auto backup = (*store)->backupTo(backup_path);
        if (!backup.has_value()) {
            QFAIL(qPrintable(backup.error().message));
        }
        const auto duplicate_backup = (*store)->backupTo(backup_path);
        QVERIFY(!duplicate_backup.has_value());
        QCOMPARE(duplicate_backup.error().code, StoreErrorCode::BackupFailed);
    }

    const auto source_identity = databaseIdentity(live_path);
    QVERIFY(!source_identity.isEmpty());
    QCOMPARE(databaseIdentity(backup_path), source_identity);
    const auto original_tree = treeImages(original_asset_root);

    const auto restored = SessionStore::restoreBackup(backup_path, restored_path);
    if (!restored.has_value()) {
        QFAIL(qPrintable(restored.error().message));
    }
    const auto overwrite = SessionStore::restoreBackup(backup_path, restored_path);
    QVERIFY(!overwrite.has_value());
    QCOMPARE(overwrite.error().code, StoreErrorCode::RestoreFailed);
    const auto restored_identity = databaseIdentity(restored_path);
    QVERIFY(!restored_identity.isEmpty());
    QVERIFY(restored_identity != source_identity);

    auto store = SessionStore::open(restored_path);
    QVERIFY(store.has_value());
    const auto original_pair_rejected = (*store)->recoverAssetStore(original_assets);
    QVERIFY(!original_pair_rejected.has_value());
    QCOMPARE(original_pair_rejected.error().code, StoreErrorCode::ConstraintViolation);
    QCOMPARE(treeImages(original_asset_root), original_tree);
    appellate::storage::AssetStore restored_assets(restored_asset_root, 1024);
    QVERIFY((*store)->recoverAssetStore(restored_assets).has_value());
    QCOMPARE(fileImage(QDir(restored_asset_root)
                           .filePath(QStringLiteral(".appellate-store-id")))
                 .bytes,
             restored_identity.toLatin1() + QByteArrayLiteral("\n"));
    const auto snapshot = (*store)->loadSession(QStringLiteral("session-1"));
    QVERIFY(snapshot.has_value());
    QCOMPARE(snapshot->sequence, qint64{1});
    QCOMPARE(snapshot->commands.size(), std::size_t{1});
    QCOMPARE(snapshot->events.size(), std::size_t{1});
    QCOMPARE(snapshot->docket.size(), std::size_t{1});
    QVERIFY(snapshot->asset_references.empty());
}

void SessionStoreTest::rejectsDocumentBearingBackupWithoutPublishingDestination() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto database_path = temporary.filePath(QStringLiteral("document.sqlite"));
    const auto backup_path = temporary.filePath(QStringLiteral("document-backup.sqlite"));
    const auto asset_root = temporary.filePath(QStringLiteral("assets"));
    appellate::storage::AssetStore assets(asset_root, 1024);
    auto store = SessionStore::open(database_path);
    QVERIFY(store.has_value());
    QVERIFY((*store)->recoverAssetStore(assets).has_value());
    QVERIFY((*store)
                ->createSession(QStringLiteral("document.session"),
                                QStringLiteral("engine.workflow.v1"),
                                QStringLiteral("2026-08-12T00:00:00Z"), pins(),
                                SessionAuthorityContract::CanonicalV2)
                .has_value());
    auto staged = assets.stage(QByteArrayLiteral("document-bearing-backup"));
    QVERIFY(staged.has_value());
    auto batch = singleAssetFiling(QStringLiteral("document.command"), staged->sha256());
    QVERIFY((*store)
                ->appendWithStagedAsset(QStringLiteral("document.session"), 0, batch, assets,
                                        *staged)
                .has_value());
    const auto before = (*store)->loadSession(QStringLiteral("document.session"));
    QVERIFY(before.has_value());
    const auto cas_before = treeImages(asset_root);

    const auto rejected = (*store)->backupTo(backup_path);
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, StoreErrorCode::BackupFailed);
    QVERIFY(!QFileInfo::exists(backup_path));
    QCOMPARE(treeImages(asset_root), cas_before);
    const auto after = (*store)->loadSession(QStringLiteral("document.session"));
    QVERIFY(after.has_value());
    QCOMPARE(after->created_at_utc, before->created_at_utc);
    QCOMPARE(after->sequence, before->sequence);
    QCOMPARE(after->pins, before->pins);
    QCOMPARE(after->commands, before->commands);
    QCOMPARE(after->events, before->events);
    QCOMPARE(after->docket, before->docket);
    QCOMPARE(after->asset_references, before->asset_references);
}

void SessionStoreTest::rejectsCorruptRestoreWithoutCreatingDestination() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto corrupt_path = temporary.filePath(QStringLiteral("corrupt.sqlite"));
    const auto destination_path = temporary.filePath(QStringLiteral("destination.sqlite"));
    QFile corrupt(corrupt_path);
    QVERIFY(corrupt.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(corrupt.write("not a database"), qint64{14});
    corrupt.close();

    const auto restored = SessionStore::restoreBackup(corrupt_path, destination_path);
    QVERIFY(!restored.has_value());
    QCOMPARE(restored.error().code, StoreErrorCode::RestoreFailed);
    QVERIFY(!QFileInfo::exists(destination_path));
}

void SessionStoreTest::rejectsLegacyDocumentBearingRestoreWithoutMutation() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    for (const auto version : {1, 2}) {
        const auto source_path =
            temporary.filePath(QStringLiteral("legacy-v%1.sqlite").arg(version));
        const auto destination_path =
            temporary.filePath(QStringLiteral("legacy-v%1-restored.sqlite").arg(version));
        QVERIFY(createLegacyDatabase(source_path));
        if (version == 2) {
            QVERIFY(upgradeLegacyDatabaseToV2(source_path));
        }
        const auto connection =
            QStringLiteral("legacy-document-restore-%1")
                .arg(QUuid::createUuid().toString(QUuid::Id128));
        {
            auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            database.setDatabaseName(source_path);
            QVERIFY(database.open());
            QSqlQuery insert(database);
            insert.prepare(QStringLiteral(
                "INSERT INTO asset_references(session_id,digest,purpose) VALUES(?,?,?)"));
            insert.addBindValue(QStringLiteral("legacy-session"));
            insert.addBindValue(QString::fromLatin1(digest));
            insert.addBindValue(QStringLiteral("legacy-document"));
            QVERIFY(insert.exec());
            database.close();
            database = QSqlDatabase{};
        }
        QSqlDatabase::removeDatabase(connection);
        const auto source_before = databaseImages(source_path);
        const auto directory_before =
            QDir(temporary.path())
                .entryList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name);

        const auto restored = SessionStore::restoreBackup(source_path, destination_path);
        QVERIFY(!restored.has_value());
        QCOMPARE(restored.error().code, StoreErrorCode::RestoreFailed);
        QVERIFY(!QFileInfo::exists(destination_path));
        QCOMPARE(databaseImages(source_path), source_before);
        QCOMPARE(QDir(temporary.path())
                     .entryList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name),
                 directory_before);
    }
}

void SessionStoreTest::rejectsSqliteSidecarBackupAndRestorePathsWithoutMutation() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto live_path = temporary.filePath(QStringLiteral("live.sqlite"));
    const auto backup_path = temporary.filePath(QStringLiteral("backup.sqlite"));
    auto store = SessionStore::open(live_path);
    QVERIFY(store.has_value());
    const auto live_before = databaseImages(live_path);
    const auto own_journal = live_path + QStringLiteral("-journal");
    const auto rejected_backup = (*store)->backupTo(own_journal);
    QVERIFY(!rejected_backup.has_value());
    QCOMPARE(rejected_backup.error().code, StoreErrorCode::BackupFailed);
    QVERIFY(!QFileInfo::exists(own_journal));
    QCOMPARE(databaseImages(live_path), live_before);
    QVERIFY((*store)->backupTo(backup_path).has_value());
    store->reset();

    const auto backup_before = databaseImages(backup_path);
    const auto source_sidecar_destination = backup_path + QStringLiteral("-journal");
    const auto rejected_source_sidecar =
        SessionStore::restoreBackup(backup_path, source_sidecar_destination);
    QVERIFY(!rejected_source_sidecar.has_value());
    QCOMPARE(rejected_source_sidecar.error().code, StoreErrorCode::RestoreFailed);
    QVERIFY(!QFileInfo::exists(source_sidecar_destination));
    QCOMPARE(databaseImages(backup_path), backup_before);

    const auto other_main = temporary.filePath(QStringLiteral("other.sqlite"));
    auto other = SessionStore::open(other_main);
    QVERIFY(other.has_value());
    other->reset();
    const auto other_before = databaseImages(other_main);
    const auto other_wal = other_main + QStringLiteral("-wal");
    const auto rejected_other_sidecar =
        SessionStore::restoreBackup(backup_path, other_wal);
    QVERIFY(!rejected_other_sidecar.has_value());
    QCOMPARE(rejected_other_sidecar.error().code, StoreErrorCode::RestoreFailed);
    QVERIFY(!QFileInfo::exists(other_wal));
    QCOMPARE(databaseImages(other_main), other_before);
    QCOMPARE(databaseImages(backup_path), backup_before);

    const auto reserved_source = temporary.filePath(QStringLiteral("reserved-source-wal"));
    QVERIFY(QFile::copy(backup_path, reserved_source));
    const auto reserved_before = fileImage(reserved_source);
    const auto reserved_destination =
        temporary.filePath(QStringLiteral("reserved-source-restored.sqlite"));
    const auto rejected_reserved_source =
        SessionStore::restoreBackup(reserved_source, reserved_destination);
    QVERIFY(!rejected_reserved_source.has_value());
    QCOMPARE(rejected_reserved_source.error().code, StoreErrorCode::RestoreFailed);
    QCOMPARE(fileImage(reserved_source), reserved_before);
    QVERIFY(!QFileInfo::exists(reserved_destination));
}

void SessionStoreTest::rejectsLiveWalRestoreSourceWithoutMutation() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto source_path = temporary.filePath(QStringLiteral("live-source.sqlite"));
    const auto destination_path = temporary.filePath(QStringLiteral("restored.sqlite"));
    auto initialized = SessionStore::open(source_path);
    QVERIFY(initialized.has_value());
    initialized->reset();

    const auto connection =
        QStringLiteral("restore-wal-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
    database.setDatabaseName(source_path);
    QVERIFY(database.open());
    QSqlQuery query(database);
    QVERIFY(query.exec(QStringLiteral("PRAGMA journal_mode=WAL")) && query.next());
    QVERIFY(query.exec(QStringLiteral("PRAGMA wal_autocheckpoint=0")));
    QVERIFY(query.exec(QStringLiteral(
        "INSERT INTO installed_pack_revisions VALUES("
        "'example.restore.pack', '1.0.0', "
        "'0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef', "
        "'2026-08-12T00:00:00Z')")));
    const auto before = databaseImages(source_path);
    QVERIFY(before.value(QStringLiteral("-wal")).exists);
    QVERIFY(before.value(QStringLiteral("-shm")).exists);

    const auto restored = SessionStore::restoreBackup(source_path, destination_path);
    QVERIFY(!restored.has_value());
    QCOMPARE(restored.error().code, StoreErrorCode::RestoreFailed);
    QVERIFY(!QFileInfo::exists(destination_path));
    QCOMPARE(databaseImages(source_path), before);

    database.close();
    database = QSqlDatabase{};
    QSqlDatabase::removeDatabase(connection);
}

void SessionStoreTest::persistsRecordAccessAcrossCloseAndReopen() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto path = temporary.filePath(QStringLiteral("record-access.sqlite"));
    const appellate::model::RecordAccessPolicy policy{
        "test.record.one",
        "test.record.access-policy",
        {{"test.document.psr", "test.authority.psr-access", "test.disclosure.psr", {}}}};

    {
        auto store = SessionStore::open(path);
        QVERIFY(store.has_value());
        QVERIFY((*store)
                    ->createSession(QStringLiteral("test.session.access"),
                                    QStringLiteral("engine.record-access.v1"),
                                    QStringLiteral("2026-08-11T00:00:00Z"), pins(),
                                    SessionAuthorityContract::CanonicalV2)
                    .has_value());
        auto snapshot = (*store)->loadSession(QStringLiteral("test.session.access"));
        QVERIFY(snapshot.has_value());
        const auto grant = appellate::storage::makeRecordAccessEvent(
            *snapshot, policy, "test.event.grant-psr", "test.document.psr",
            appellate::model::RecordAccessAction::Grant, "2026-08-11T00:01:00Z");
        QVERIFY(grant.has_value());
        const auto encoded = appellate::storage::encodeRecordAccessEvent(*grant);
        QVERIFY(encoded.has_value());
        const CommitBatch batch{
            QString::fromUtf8(grant->event_id),
            QByteArrayLiteral(R"({"command_type":"record_access_transition"})"),
            QStringLiteral("2026-08-11T00:01:00Z"),
            {EventWrite{appellate::storage::recordAccessEventType(grant->action), *encoded,
                        QString::fromUtf8(grant->authority_id)}},
            {},
            {}};
        QVERIFY((*store)->append(snapshot->session_id, 0, batch).has_value());
    }

    {
        auto store = SessionStore::open(path);
        QVERIFY(store.has_value());
        auto snapshot = (*store)->loadSession(QStringLiteral("test.session.access"));
        QVERIFY(snapshot.has_value());
        auto projection = appellate::storage::projectRecordAccess(*snapshot, policy);
        QVERIFY(projection.has_value());
        QCOMPARE(projection->authorized_document_ids,
                 std::vector<std::string>{"test.document.psr"});

        const auto revoke = appellate::storage::makeRecordAccessEvent(
            *snapshot, policy, "test.event.revoke-psr", "test.document.psr",
            appellate::model::RecordAccessAction::Revoke, "2026-08-11T00:02:00Z");
        QVERIFY(revoke.has_value());
        const auto encoded = appellate::storage::encodeRecordAccessEvent(*revoke);
        QVERIFY(encoded.has_value());
        const CommitBatch batch{
            QString::fromUtf8(revoke->event_id),
            QByteArrayLiteral(R"({"command_type":"record_access_transition"})"),
            QStringLiteral("2026-08-11T00:02:00Z"),
            {EventWrite{appellate::storage::recordAccessEventType(revoke->action), *encoded,
                        QString::fromUtf8(revoke->authority_id)}},
            {},
            {}};
        QVERIFY((*store)->append(snapshot->session_id, 1, batch).has_value());
    }

    auto reopened = SessionStore::open(path);
    QVERIFY(reopened.has_value());
    const auto snapshot = (*reopened)->loadSession(QStringLiteral("test.session.access"));
    QVERIFY(snapshot.has_value());
    const auto projection = appellate::storage::projectRecordAccess(*snapshot, policy);
    QVERIFY(projection.has_value());
    QVERIFY(projection->authorized_document_ids.empty());
    QCOMPARE(projection->through_sequence, std::uint64_t{2});
}

void SessionStoreTest::createsSessionAndInitialBatchAtomically() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    auto store = SessionStore::open(temporary.filePath(QStringLiteral("atomic.sqlite")));
    QVERIFY(store.has_value());

    auto invalid = acceptedFiling(QStringLiteral("opening-invalid"));
    invalid.events.clear();
    const auto rejected = (*store)->createSessionWithInitialBatch(
        QStringLiteral("oral.atomic.invalid"), QStringLiteral("engine.oral.v1"),
        QStringLiteral("2026-08-12T00:00:00Z"), pins(),
        SessionAuthorityContract::CanonicalV2, invalid);
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, StoreErrorCode::InvalidArgument);
    const auto absent = (*store)->loadSession(QStringLiteral("oral.atomic.invalid"));
    QVERIFY(!absent.has_value());
    QCOMPARE(absent.error().code, StoreErrorCode::NotFound);

    auto opening = acceptedFiling(QStringLiteral("oral.atomic.valid.opening"));
    const auto created = (*store)->createSessionWithInitialBatch(
        QStringLiteral("oral.atomic.valid"), QStringLiteral("engine.oral.v1"),
        QStringLiteral("2026-08-12T00:00:00Z"), pins(),
        SessionAuthorityContract::CanonicalV2, opening);
    QVERIFY(created.has_value());
    QCOMPARE(*created, qint64{1});
    const auto snapshot = (*store)->loadSession(QStringLiteral("oral.atomic.valid"));
    QVERIFY(snapshot.has_value());
    QCOMPARE(snapshot->sequence, qint64{1});
    QCOMPARE(snapshot->commands.size(), std::size_t{1});
    QCOMPARE(snapshot->events.size(), std::size_t{1});
    QCOMPARE(snapshot->created_at_utc, QStringLiteral("2026-08-12T00:00:00Z"));
}

void SessionStoreTest::recoversUnreferencedFinalAndStagingCrashStates() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto database_path = temporary.filePath(QStringLiteral("recovery.sqlite"));
    const auto asset_root = temporary.filePath(QStringLiteral("assets"));
    appellate::storage::AssetStore assets(asset_root, 1024);
    auto store = SessionStore::open(database_path);
    QVERIFY(store.has_value());
    QVERIFY((*store)->recoverAssetStore(assets).has_value());

    const auto orphan = assets.put(QByteArrayLiteral("finalized-before-database-commit"));
    QVERIFY(orphan.has_value());
    const auto staging_path =
        QDir(assets.objectsDirectory()).filePath(QStringLiteral(".asset-crash.tmp"));
    QFile staging(staging_path);
    QVERIFY(staging.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(staging.write("abandoned"), qint64{9});
    staging.close();

    const auto recovered = (*store)->recoverAssetStore(assets);
    if (!recovered) {
        QFAIL(qPrintable(recovered.error().message));
    }
    QVERIFY(!QFileInfo::exists(staging_path));
    const auto absent = assets.read(orphan->sha256);
    QVERIFY(!absent.has_value());
    QCOMPARE(absent.error().code, appellate::storage::AssetStoreErrorCode::NotFound);
}

void SessionStoreTest::corruptOrphanFailsRecoveryWithoutCleanup() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto database_path = temporary.filePath(QStringLiteral("corrupt-orphan.sqlite"));
    const auto asset_root = temporary.filePath(QStringLiteral("assets"));
    appellate::storage::AssetStore assets(asset_root, 1024);
    auto store = SessionStore::open(database_path);
    QVERIFY(store.has_value());
    QVERIFY((*store)->recoverAssetStore(assets).has_value());
    const auto earlier_orphan = assets.put(QByteArrayLiteral("valid earlier orphan"));
    const auto corrupt_orphan = assets.put(QByteArrayLiteral("orphan to corrupt"));
    QVERIFY(earlier_orphan.has_value());
    QVERIFY(corrupt_orphan.has_value());
    const auto staging_path =
        QDir(assets.objectsDirectory()).filePath(QStringLiteral(".asset-earlier.tmp"));
    QFile staging(staging_path);
    QVERIFY(staging.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(staging.write("staging"), qint64{7});
    staging.close();
    const auto corrupt_path =
        QDir(assets.objectsDirectory()).filePath(corrupt_orphan->sha256);
    QFile corrupted(corrupt_path);
    QVERIFY(corrupted.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(corrupted.write("tampered orphan"), qint64{15});
    corrupted.close();
    const auto tree_before = treeImages(asset_root);

    const auto rejected = (*store)->recoverAssetStore(assets);
    QVERIFY(!rejected.has_value());
    QCOMPARE(treeImages(asset_root), tree_before);
    QVERIFY(assets.read(earlier_orphan->sha256).has_value());
    QVERIFY(!assets.read(corrupt_orphan->sha256).has_value());
    QVERIFY(QFileInfo::exists(staging_path));
}

void SessionStoreTest::missingReferencedAssetFailsRecoveryBeforeCleanup() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto database_path = temporary.filePath(QStringLiteral("missing-reference.sqlite"));
    const auto asset_root = temporary.filePath(QStringLiteral("assets"));
    appellate::storage::AssetStore assets(asset_root, 1024);
    auto store = SessionStore::open(database_path);
    QVERIFY(store.has_value());
    QVERIFY((*store)->recoverAssetStore(assets).has_value());

    const auto unrelated = assets.put(QByteArrayLiteral("unrelated-orphan"));
    QVERIFY(unrelated.has_value());

    QVERIFY((*store)
                ->createSession(QStringLiteral("missing.asset.session"),
                                QStringLiteral("engine.workflow.v1"),
                                QStringLiteral("2026-08-12T00:00:00Z"), pins(),
                                SessionAuthorityContract::CanonicalV2)
                .has_value());
    QVERIFY((*store)
                ->append(QStringLiteral("missing.asset.session"), 0,
                         acceptedFiling(QStringLiteral("missing.asset.command")))
                .has_value());
    const auto connection =
        QStringLiteral("missing-reference-fixture-%1")
            .arg(QUuid::createUuid().toString(QUuid::Id128));
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(database_path);
        QVERIFY(database.open());
        QSqlQuery insert(database);
        insert.prepare(QStringLiteral(
            "INSERT INTO asset_references(session_id, digest, purpose) VALUES(?, ?, ?)"));
        insert.addBindValue(QStringLiteral("missing.asset.session"));
        insert.addBindValue(QString::fromLatin1(digest));
        insert.addBindValue(QStringLiteral("missing-fixture"));
        QVERIFY(insert.exec());
        database.close();
        database = QSqlDatabase{};
    }
    QSqlDatabase::removeDatabase(connection);
    const auto before = (*store)->loadSession(QStringLiteral("missing.asset.session"));
    QVERIFY(before.has_value());

    const auto recovered = (*store)->recoverAssetStore(assets);
    QVERIFY(!recovered.has_value());
    const auto unrelated_after = assets.read(unrelated->sha256);
    QVERIFY(unrelated_after.has_value());
    QCOMPARE(*unrelated_after, QByteArrayLiteral("unrelated-orphan"));
    const auto after = (*store)->loadSession(QStringLiteral("missing.asset.session"));
    QVERIFY(after.has_value());
    QCOMPARE(after->sequence, before->sequence);
    QCOMPARE(after->commands, before->commands);
    QCOMPARE(after->events, before->events);
    QCOMPARE(after->docket, before->docket);
    QCOMPARE(after->asset_references, before->asset_references);
}

void SessionStoreTest::authoritativeDatabaseIdentityHealsOnlyMissingCasMarker() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto database_path = temporary.filePath(QStringLiteral("identity.sqlite"));
    const auto asset_root = temporary.filePath(QStringLiteral("assets"));
    appellate::storage::AssetStore assets(asset_root, 1024);
    auto store = SessionStore::open(database_path);
    QVERIFY(store.has_value());
    QVERIFY((*store)->recoverAssetStore(assets).has_value());
    const auto marker_path = QDir(asset_root).filePath(QStringLiteral(".appellate-store-id"));
    const auto authoritative_marker = fileImage(marker_path);
    QVERIFY(authoritative_marker.exists);
    QCOMPARE(authoritative_marker.bytes.size(), 33);
    store->reset();

    QVERIFY(QFile::remove(marker_path));
    auto reopened = SessionStore::open(database_path);
    QVERIFY(reopened.has_value());
    const auto healed = (*reopened)->recoverAssetStore(assets);
    if (!healed) {
        QFAIL(qPrintable(healed.error().message));
    }
    QCOMPARE(fileImage(marker_path), authoritative_marker);
    // Repeated validation must use a fresh directory stream, not a consumed dup offset.
    QVERIFY((*reopened)->recoverAssetStore(assets).has_value());
    QCOMPARE(fileImage(marker_path), authoritative_marker);
}

void SessionStoreTest::legacyDatabaseBindsOnlyToExactVerifiedCasObjects() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto database_path = temporary.filePath(QStringLiteral("legacy-pair.sqlite"));
    const auto asset_root = temporary.filePath(QStringLiteral("assets"));
    QVERIFY(createLegacyDatabase(database_path));
    appellate::storage::AssetStore assets(asset_root, 1024);
    const auto referenced = assets.put(QByteArrayLiteral("legacy-referenced-object"));
    const auto extra = assets.put(QByteArrayLiteral("legacy-extra-object"));
    QVERIFY(referenced.has_value());
    QVERIFY(extra.has_value());

    const auto connection =
        QStringLiteral("legacy-asset-ref-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(database_path);
        QVERIFY(database.open());
        QSqlQuery insert(database);
        insert.prepare(QStringLiteral(
            "INSERT INTO asset_references(session_id, digest, purpose) VALUES(?, ?, ?)"));
        insert.addBindValue(QStringLiteral("legacy-session"));
        insert.addBindValue(referenced->sha256);
        insert.addBindValue(QStringLiteral("filing-document"));
        QVERIFY(insert.exec());
        database.close();
        database = QSqlDatabase{};
    }
    QSqlDatabase::removeDatabase(connection);

    auto migrated = SessionStore::open(database_path);
    QVERIFY(migrated.has_value());
    QCOMPARE((*migrated)->schemaVersion(), 3);
    const auto marker_path = QDir(asset_root).filePath(QStringLiteral(".appellate-store-id"));
    const auto mismatch = (*migrated)->recoverAssetStore(assets);
    QVERIFY(!mismatch.has_value());
    QVERIFY(!QFileInfo::exists(marker_path));
    const auto extra_path = QDir(assets.objectsDirectory()).filePath(extra->sha256);
    QVERIFY(QFile::remove(extra_path));

    // The retry must rescan from offset zero and bind only after exact set and byte validation.
    const auto bound = (*migrated)->recoverAssetStore(assets);
    if (!bound) {
        QFAIL(qPrintable(bound.error().message));
    }
    QVERIFY(QFileInfo::exists(marker_path));
    const auto restored = assets.read(referenced->sha256);
    QVERIFY(restored.has_value());
    QCOMPARE(*restored, QByteArrayLiteral("legacy-referenced-object"));
}

void SessionStoreTest::oldCasMarkerIsNeverAdoptedByReplacementDatabase() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto old_database = temporary.filePath(QStringLiteral("old.sqlite"));
    const auto replacement_database = temporary.filePath(QStringLiteral("replacement.sqlite"));
    const auto asset_root = temporary.filePath(QStringLiteral("assets"));
    appellate::storage::AssetStore assets(asset_root, 1024);
    auto old_store = SessionStore::open(old_database);
    QVERIFY(old_store.has_value());
    QVERIFY((*old_store)->recoverAssetStore(assets).has_value());
    old_store->reset();
    const auto marker_path = QDir(asset_root).filePath(QStringLiteral(".appellate-store-id"));
    const auto old_marker = fileImage(marker_path);
    QVERIFY(old_marker.exists);

    for (int launch = 0; launch < 2; ++launch) {
        auto replacement = SessionStore::open(replacement_database);
        QVERIFY(replacement.has_value());
        const auto rejected = (*replacement)->recoverAssetStore(assets);
        QVERIFY(!rejected.has_value());
        QCOMPARE(rejected.error().code, StoreErrorCode::ConstraintViolation);
        QCOMPARE(fileImage(marker_path), old_marker);
    }
    QVERIFY(QFileInfo::exists(replacement_database));
    QVERIFY(QDir(assets.objectsDirectory())
                .entryList(QDir::AllEntries | QDir::NoDotAndDotDot)
                .isEmpty());
}

void SessionStoreTest::pairValidationFailurePreservesEveryCasEntry() {
#if !defined(Q_OS_UNIX)
    QSKIP("Hard-link CAS validation test is Unix-only");
#else
    QTemporaryDir temporary;
    QTemporaryDir outside;
    QVERIFY(temporary.isValid());
    QVERIFY(outside.isValid());
    const auto database_path = temporary.filePath(QStringLiteral("pair-validation.sqlite"));
    const auto asset_root = temporary.filePath(QStringLiteral("assets"));
    appellate::storage::AssetStore assets(asset_root, 1024);
    auto store = SessionStore::open(database_path);
    QVERIFY(store.has_value());
    const auto referenced = assets.put(QByteArrayLiteral("referenced-before-pair"));
    QVERIFY(referenced.has_value());
    QVERIFY((*store)
                ->createSession(QStringLiteral("pair.validation.session"),
                                QStringLiteral("engine.workflow.v1"),
                                QStringLiteral("2026-08-12T00:00:00Z"), pins(),
                                SessionAuthorityContract::CanonicalV2)
                .has_value());
    QVERIFY((*store)
                ->append(QStringLiteral("pair.validation.session"), 0,
                         acceptedFiling(QStringLiteral("pair.validation.command")))
                .has_value());
    const auto reference_connection =
        QStringLiteral("pair-validation-ref-%1")
            .arg(QUuid::createUuid().toString(QUuid::Id128));
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                  reference_connection);
        database.setDatabaseName(database_path);
        QVERIFY(database.open());
        QSqlQuery insert(database);
        insert.prepare(QStringLiteral(
            "INSERT INTO asset_references(session_id, digest, purpose) VALUES(?, ?, ?)"));
        insert.addBindValue(QStringLiteral("pair.validation.session"));
        insert.addBindValue(referenced->sha256);
        insert.addBindValue(QStringLiteral("filing-document"));
        QVERIFY(insert.exec());
        database.close();
        database = QSqlDatabase{};
    }
    QSqlDatabase::removeDatabase(reference_connection);
    const auto snapshot_before =
        (*store)->loadSession(QStringLiteral("pair.validation.session"));
    QVERIFY(snapshot_before.has_value());

    const auto valid_temp =
        QDir(assets.objectsDirectory()).filePath(QStringLiteral(".asset-valid.tmp"));
    QFile valid(valid_temp);
    QVERIFY(valid.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(valid.write("valid-temp"), qint64{10});
    valid.close();
    const auto outside_path = outside.filePath(QStringLiteral("outside-temp"));
    QFile outside_file(outside_path);
    QVERIFY(outside_file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(outside_file.write("outside"), qint64{7});
    outside_file.close();
    const auto linked_temp =
        QDir(assets.objectsDirectory()).filePath(QStringLiteral(".asset-linked.tmp"));
    QCOMPARE(::link(QFile::encodeName(outside_path).constData(),
                    QFile::encodeName(linked_temp).constData()),
             0);
    const auto outside_before = fileImage(outside_path);
    const auto marker_path = QDir(asset_root).filePath(QStringLiteral(".appellate-store-id"));
    QVERIFY(!QFileInfo::exists(marker_path));

    const auto rejected = (*store)->recoverAssetStore(assets);
    QVERIFY(!rejected.has_value());
    QVERIFY(!QFileInfo::exists(marker_path));
    QVERIFY(QFileInfo::exists(valid_temp));
    QVERIFY(QFileInfo::exists(linked_temp));
    QCOMPARE(fileImage(outside_path), outside_before);
    QVERIFY(assets.read(referenced->sha256).has_value());
    const auto snapshot_after =
        (*store)->loadSession(QStringLiteral("pair.validation.session"));
    QVERIFY(snapshot_after.has_value());
    QCOMPARE(snapshot_after->sequence, snapshot_before->sequence);
    QCOMPARE(snapshot_after->commands, snapshot_before->commands);
    QCOMPARE(snapshot_after->events, snapshot_before->events);
    QCOMPARE(snapshot_after->docket, snapshot_before->docket);
    QCOMPARE(snapshot_after->asset_references, snapshot_before->asset_references);
#endif
}

void SessionStoreTest::corruptUnboundCasFailsWithoutPublishingLock() {
    QTemporaryDir temporary;
    QTemporaryDir outside;
    QVERIFY(temporary.isValid());
    QVERIFY(outside.isValid());
    QStringList modes{QStringLiteral("malformed-marker"), QStringLiteral("unexpected-entry"),
                      QStringLiteral("missing-reference")};
#if defined(Q_OS_UNIX)
    modes.push_back(QStringLiteral("hardlinked-temp"));
#endif
    for (const auto& mode : modes) {
        const auto root = temporary.filePath(mode);
        QVERIFY(QDir{}.mkpath(root));
        const auto database_path = QDir(root).filePath(QStringLiteral("sessions.sqlite"));
        const auto asset_root = QDir(root).filePath(QStringLiteral("assets"));
        const auto objects = QDir(asset_root).filePath(QStringLiteral("objects"));
        QVERIFY(QDir{}.mkpath(objects));
        appellate::storage::AssetStore assets(asset_root, 1024);
        auto store = SessionStore::open(database_path);
        QVERIFY(store.has_value());
        QVERIFY((*store)
                    ->createSession(QStringLiteral("corrupt.cas.session"),
                                    QStringLiteral("engine.workflow.v1"),
                                    QStringLiteral("2026-08-12T00:00:00Z"), pins(),
                                    SessionAuthorityContract::CanonicalV2)
                    .has_value());

        if (mode == QStringLiteral("malformed-marker")) {
            QFile marker(QDir(asset_root)
                             .filePath(QStringLiteral(".appellate-store-id")));
            QVERIFY(marker.open(QIODevice::WriteOnly | QIODevice::NewOnly));
            QCOMPARE(marker.write("partial"), qint64{7});
            marker.close();
        } else if (mode == QStringLiteral("unexpected-entry")) {
            QFile unexpected(QDir(objects).filePath(QStringLiteral("unexpected")));
            QVERIFY(unexpected.open(QIODevice::WriteOnly | QIODevice::NewOnly));
            QCOMPARE(unexpected.write("evidence"), qint64{8});
            unexpected.close();
        } else if (mode == QStringLiteral("missing-reference")) {
            const auto connection =
                QStringLiteral("unbound-missing-ref-%1")
                    .arg(QUuid::createUuid().toString(QUuid::Id128));
            {
                auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
                database.setDatabaseName(database_path);
                QVERIFY(database.open());
                QSqlQuery insert(database);
                insert.prepare(QStringLiteral(
            "INSERT INTO asset_references(session_id,digest,purpose) VALUES(?,?,?)"));
                insert.addBindValue(QStringLiteral("corrupt.cas.session"));
                insert.addBindValue(QString::fromLatin1(digest));
                insert.addBindValue(QStringLiteral("missing-fixture"));
                QVERIFY(insert.exec());
                database.close();
                database = QSqlDatabase{};
            }
            QSqlDatabase::removeDatabase(connection);
        }
#if defined(Q_OS_UNIX)
        else {
            const auto outside_path =
                outside.filePath(mode + QStringLiteral("-outside"));
            QFile outside_file(outside_path);
            QVERIFY(outside_file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
            QCOMPARE(outside_file.write("outside"), qint64{7});
            outside_file.close();
            const auto linked =
                QDir(objects).filePath(QStringLiteral(".asset-linked.tmp"));
            QCOMPARE(::link(QFile::encodeName(outside_path).constData(),
                            QFile::encodeName(linked).constData()),
                     0);
        }
#endif
        const auto tree_before = treeImages(asset_root);
        const auto session_before =
            (*store)->loadSession(QStringLiteral("corrupt.cas.session"));
        QVERIFY(session_before.has_value());
        QVERIFY(!QFileInfo::exists(
            QDir(asset_root).filePath(QStringLiteral(".cas.lock"))));

        const auto rejected = (*store)->recoverAssetStore(assets);
        QVERIFY(!rejected.has_value());
        QCOMPARE(treeImages(asset_root), tree_before);
        QVERIFY(!QFileInfo::exists(
            QDir(asset_root).filePath(QStringLiteral(".cas.lock"))));
        const auto session_after =
            (*store)->loadSession(QStringLiteral("corrupt.cas.session"));
        QVERIFY(session_after.has_value());
        QCOMPARE(session_after->created_at_utc, session_before->created_at_utc);
        QCOMPARE(session_after->sequence, session_before->sequence);
        QCOMPARE(session_after->pins, session_before->pins);
        QCOMPARE(session_after->commands, session_before->commands);
        QCOMPARE(session_after->events, session_before->events);
        QCOMPARE(session_after->docket, session_before->docket);
        QCOMPARE(session_after->asset_references, session_before->asset_references);
    }
}

void SessionStoreTest::recoveryValidationFailurePreservesEarlierCleanupCandidates() {
#if !defined(Q_OS_UNIX)
    QSKIP("Hard-link CAS recovery test is Unix-only");
#else
    QTemporaryDir temporary;
    QTemporaryDir outside;
    QVERIFY(temporary.isValid());
    QVERIFY(outside.isValid());
    const auto database_path = temporary.filePath(QStringLiteral("recovery-order.sqlite"));
    const auto asset_root = temporary.filePath(QStringLiteral("assets"));
    appellate::storage::AssetStore assets(asset_root, 1024);
    auto store = SessionStore::open(database_path);
    QVERIFY(store.has_value());
    QVERIFY((*store)->recoverAssetStore(assets).has_value());
    QVERIFY((*store)
                ->createSession(QStringLiteral("recovery.order.session"),
                                QStringLiteral("engine.workflow.v1"),
                                QStringLiteral("2026-08-12T00:00:00Z"), pins(),
                                SessionAuthorityContract::CanonicalV2)
                .has_value());
    auto referenced = assets.stage(QByteArrayLiteral("referenced"));
    QVERIFY(referenced.has_value());
    auto referenced_batch = singleAssetFiling(QStringLiteral("recovery.order.command"),
                                               referenced->sha256());
    QVERIFY((*store)
                ->appendWithStagedAsset(QStringLiteral("recovery.order.session"), 0,
                                        referenced_batch, assets, *referenced)
                .has_value());
    const auto orphan = assets.put(QByteArrayLiteral("orphan-before-corruption"));
    QVERIFY(orphan.has_value());
    const auto valid_temp =
        QDir(assets.objectsDirectory()).filePath(QStringLiteral(".asset-a-valid.tmp"));
    QFile valid(valid_temp);
    QVERIFY(valid.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(valid.write("valid-temp"), qint64{10});
    valid.close();
    const auto outside_path = outside.filePath(QStringLiteral("outside-temp"));
    QFile outside_file(outside_path);
    QVERIFY(outside_file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(outside_file.write("outside"), qint64{7});
    outside_file.close();
    const auto linked_temp =
        QDir(assets.objectsDirectory()).filePath(QStringLiteral(".asset-z-linked.tmp"));
    QCOMPARE(::link(QFile::encodeName(outside_path).constData(),
                    QFile::encodeName(linked_temp).constData()),
             0);
    const auto orphan_path = QDir(assets.objectsDirectory()).filePath(orphan->sha256);
    const auto orphan_before = fileImage(orphan_path);
    const auto outside_before = fileImage(outside_path);

    const auto rejected = (*store)->recoverAssetStore(assets);
    QVERIFY(!rejected.has_value());
    QCOMPARE(fileImage(orphan_path), orphan_before);
    QVERIFY(QFileInfo::exists(valid_temp));
    QVERIFY(QFileInfo::exists(linked_temp));
    QCOMPARE(fileImage(outside_path), outside_before);
    QVERIFY(assets.read(referenced->sha256()).has_value());
#endif
}

void SessionStoreTest::rejectsHardLinkedCasIdentityWithoutChangingAlias() {
#if !defined(Q_OS_UNIX)
    QSKIP("Hard-link CAS identity test is Unix-only");
#else
    QTemporaryDir temporary;
    QTemporaryDir outside;
    QVERIFY(temporary.isValid());
    QVERIFY(outside.isValid());
    const auto database_path = temporary.filePath(QStringLiteral("identity-hardlink.sqlite"));
    const auto asset_root = temporary.filePath(QStringLiteral("assets"));
    appellate::storage::AssetStore assets(asset_root, 1024);
    auto store = SessionStore::open(database_path);
    QVERIFY(store.has_value());
    QVERIFY((*store)->recoverAssetStore(assets).has_value());
    store->reset();
    const auto marker_path = QDir(asset_root).filePath(QStringLiteral(".appellate-store-id"));
    const auto alias_path = outside.filePath(QStringLiteral("identity-alias"));
    QCOMPARE(::link(QFile::encodeName(marker_path).constData(),
                    QFile::encodeName(alias_path).constData()),
             0);
    const auto alias_before = fileImage(alias_path);

    auto reopened = SessionStore::open(database_path);
    QVERIFY(reopened.has_value());
    const auto rejected = (*reopened)->recoverAssetStore(assets);
    QVERIFY(!rejected.has_value());
    QCOMPARE(fileImage(alias_path), alias_before);
    QCOMPARE(fileImage(marker_path), alias_before);
#endif
}

void SessionStoreTest::rejectsPartialCasIdentityWithoutReplacingIt() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto database_path = temporary.filePath(QStringLiteral("identity-partial.sqlite"));
    const auto asset_root = temporary.filePath(QStringLiteral("assets"));
    appellate::storage::AssetStore assets(asset_root, 1024);
    auto store = SessionStore::open(database_path);
    QVERIFY(store.has_value());
    {
        auto lock = assets.acquireLock();
        QVERIFY(lock.has_value());
    }
    const auto marker_path = QDir(asset_root).filePath(QStringLiteral(".appellate-store-id"));
    QFile partial(marker_path);
    QVERIFY(partial.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(partial.write("partial"), qint64{7});
    partial.close();
    const auto before = fileImage(marker_path);

    const auto rejected = (*store)->recoverAssetStore(assets);
    QVERIFY(!rejected.has_value());
    QCOMPARE(fileImage(marker_path), before);
    QVERIFY(QDir(assets.objectsDirectory())
                .entryList(QDir::AllEntries | QDir::NoDotAndDotDot)
                .isEmpty());
}

void SessionStoreTest::cooperativeLockBlocksAnotherAppellateProcess() {
#if !defined(Q_OS_UNIX)
    QSKIP("Cooperative database lock is Unix-only");
#else
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto path = temporary.filePath(QStringLiteral("cooperative.sqlite"));
    const auto marker = temporary.filePath(QStringLiteral("child-opened"));
    auto held = SessionStore::open(path);
    QVERIFY(held.has_value());
    const auto before_contention = databaseImages(path);

    QProcess child;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("APPELLATE_LOCK_TEST_DB"), path);
    environment.insert(QStringLiteral("APPELLATE_LOCK_TEST_MARKER"), marker);
    environment.insert(QStringLiteral("APPELLATE_LOCK_TEST_EXPECT_BUSY"), QStringLiteral("1"));
    child.setProcessEnvironment(environment);
    child.start(QCoreApplication::applicationFilePath(),
                {QStringLiteral("cooperativeLockHelper")});
    QVERIFY(child.waitForStarted());
    QVERIFY2(child.waitForFinished(2'000), qPrintable(child.errorString()));
    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    const auto child_output = child.readAllStandardOutput() + child.readAllStandardError();
    QVERIFY2(child.exitCode() == 0, child_output.constData());
    QVERIFY(!QFileInfo::exists(marker));
    QCOMPARE(databaseImages(path), before_contention);

    held->reset();
    environment.remove(QStringLiteral("APPELLATE_LOCK_TEST_EXPECT_BUSY"));
    QProcess reopened;
    reopened.setProcessEnvironment(environment);
    reopened.start(QCoreApplication::applicationFilePath(),
                   {QStringLiteral("cooperativeLockHelper")});
    QVERIFY(reopened.waitForStarted());
    QVERIFY2(reopened.waitForFinished(10'000), qPrintable(reopened.errorString()));
    QCOMPARE(reopened.exitStatus(), QProcess::NormalExit);
    QCOMPARE(reopened.exitCode(), 0);
    QVERIFY2(QFileInfo::exists(marker), reopened.readAllStandardError().constData());
#endif
}

void SessionStoreTest::ownerForksShareLeaseAndOutliveOwnerSafely() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto path = temporary.filePath(QStringLiteral("owner-and-children.sqlite"));
    auto owner = SessionStore::open(path);
    QVERIFY(owner.has_value());
    auto first_child = (*owner)->forkConnection();
    QVERIFY(first_child.has_value());
    const auto nested = (*first_child)->forkConnection();
    QVERIFY(!nested.has_value());
    QCOMPARE(nested.error().code, StoreErrorCode::InvalidArgument);

    QVERIFY((*first_child)
                ->createSession(QStringLiteral("fork.shared.session"),
                                QStringLiteral("engine.test.v1"),
                                QStringLiteral("2026-08-12T00:00:00Z"), pins(),
                                SessionAuthorityContract::CanonicalV2)
                .has_value());
    // A fork created after an earlier child commit validates current sidecars rather than a
    // frozen pre-migration/pre-write snapshot.
    auto second_child = (*owner)->forkConnection();
    QVERIFY(second_child.has_value());
    const auto visible = (*second_child)->loadSession(QStringLiteral("fork.shared.session"));
    QVERIFY(visible.has_value());
    QCOMPARE(visible->created_at_utc, QStringLiteral("2026-08-12T00:00:00Z"));

    auto first_batch = acceptedFiling(QStringLiteral("fork.command.1"));
    first_batch.docket_changes.front().entry_id = QStringLiteral("fork.entry.1");
    QVERIFY((*second_child)
                ->append(QStringLiteral("fork.shared.session"), 0, first_batch)
                .has_value());
    auto third_child = (*owner)->forkConnection();
    QVERIFY(third_child.has_value());
    const auto after_child_write =
        (*third_child)->loadSession(QStringLiteral("fork.shared.session"));
    QVERIFY(after_child_write.has_value());
    QCOMPARE(after_child_write->sequence, qint64{1});

    QElapsedTimer bounded;
    bounded.start();
    auto independent = SessionStore::open(path);
    QVERIFY(!independent.has_value());
    QCOMPARE(independent.error().code, StoreErrorCode::StateInUse);
    QVERIFY(bounded.elapsed() < 1'000);

    owner->reset();
    independent = SessionStore::open(path);
    QVERIFY(!independent.has_value());
    QCOMPARE(independent.error().code, StoreErrorCode::StateInUse);
    auto second_batch = acceptedFiling(QStringLiteral("fork.command.2"));
    second_batch.docket_changes.front().entry_id = QStringLiteral("fork.entry.2");
    QVERIFY((*first_child)
                ->append(QStringLiteral("fork.shared.session"), 1, second_batch)
                .has_value());
    const auto after_owner_destruction =
        (*third_child)->loadSession(QStringLiteral("fork.shared.session"));
    QVERIFY(after_owner_destruction.has_value());
    QCOMPARE(after_owner_destruction->sequence, qint64{2});
    QCOMPARE(after_owner_destruction->commands.size(), std::size_t{2});
    first_child->reset();
    independent = SessionStore::open(path);
    QVERIFY(!independent.has_value());
    QCOMPARE(independent.error().code, StoreErrorCode::StateInUse);
    second_child->reset();
    independent = SessionStore::open(path);
    QVERIFY(!independent.has_value());
    QCOMPARE(independent.error().code, StoreErrorCode::StateInUse);
    third_child->reset();

    independent = SessionStore::open(path);
    if (!independent) {
        QFAIL(qPrintable(independent.error().message));
    }
    QVERIFY((*independent)->loadSession(QStringLiteral("fork.shared.session")).has_value());
}

void SessionStoreTest::cooperativeLockHelper() {
    const auto path = qEnvironmentVariable("APPELLATE_LOCK_TEST_DB");
    const auto marker = qEnvironmentVariable("APPELLATE_LOCK_TEST_MARKER");
    const auto expect_busy = qEnvironmentVariableIsSet("APPELLATE_LOCK_TEST_EXPECT_BUSY");
    if (path.isEmpty() || marker.isEmpty()) {
        QSKIP("Internal subprocess helper");
    }
    auto opened = SessionStore::open(path);
    if (!opened) {
        if (expect_busy) {
            QCOMPARE(opened.error().code, StoreErrorCode::StateInUse);
            return;
        }
        QFAIL(qPrintable(opened.error().message));
    }
    if (expect_busy) {
        QFAIL("A contending Appellate process opened state that was already in use");
    }
    QFile output(marker);
    QVERIFY(output.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(output.write("opened"), qint64{6});
    output.close();
}

void SessionStoreTest::simultaneousFreshAndLegacyNoLockOpenHaveOneBoundedWinner() {
#if !defined(Q_OS_UNIX)
    QSKIP("Cooperative database lock is Unix-only");
#else
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    for (const auto& mode : {QStringLiteral("fresh"), QStringLiteral("legacy-no-lock")}) {
        const auto directory = temporary.filePath(mode);
        QVERIFY(QDir{}.mkpath(directory));
        const auto path = QDir(directory).filePath(QStringLiteral("shared.sqlite"));
        const QFileInfo database(path);
        const auto lock_path =
            QDir(directory).filePath(QStringLiteral(".%1.appellate-open.lock")
                                         .arg(database.fileName()));
        if (mode == QStringLiteral("legacy-no-lock")) {
            auto initialized = SessionStore::open(path);
            QVERIFY(initialized.has_value());
            initialized->reset();
            QVERIFY(QFile::remove(lock_path));
        }

        const auto start_path = QDir(directory).filePath(QStringLiteral("start"));
        const auto release_path = QDir(directory).filePath(QStringLiteral("release"));
        const auto ready_a = QDir(directory).filePath(QStringLiteral("ready-a"));
        const auto ready_b = QDir(directory).filePath(QStringLiteral("ready-b"));
        const auto result_a = QDir(directory).filePath(QStringLiteral("result-a"));
        const auto result_b = QDir(directory).filePath(QStringLiteral("result-b"));

        auto start_child = [&](QProcess& child, const QString& ready, const QString& result) {
            auto environment = QProcessEnvironment::systemEnvironment();
            environment.insert(QStringLiteral("APPELLATE_LOCK_RACE_DB"), path);
            environment.insert(QStringLiteral("APPELLATE_LOCK_RACE_READY"), ready);
            environment.insert(QStringLiteral("APPELLATE_LOCK_RACE_START"), start_path);
            environment.insert(QStringLiteral("APPELLATE_LOCK_RACE_RESULT"), result);
            environment.insert(QStringLiteral("APPELLATE_LOCK_RACE_RELEASE"), release_path);
            child.setProcessEnvironment(environment);
            child.start(QCoreApplication::applicationFilePath(),
                        {QStringLiteral("cooperativeRaceHelper")});
            QVERIFY(child.waitForStarted());
        };
        QProcess child_a;
        QProcess child_b;
        start_child(child_a, ready_a, result_a);
        start_child(child_b, ready_b, result_b);
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(ready_a) && QFileInfo::exists(ready_b), 5'000);
        QFile start(start_path);
        QVERIFY(start.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        start.close();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(result_a) && QFileInfo::exists(result_b), 10'000);

        auto read_file = [](const QString& file_path) {
            QFile input(file_path);
            if (!input.open(QIODevice::ReadOnly)) {
                return QByteArray{};
            }
            return input.readAll();
        };
        const QList<QByteArray> results{read_file(result_a), read_file(result_b)};
        QCOMPARE(results.count(QByteArrayLiteral("opened")), 1);
        QCOMPARE(results.count(QByteArrayLiteral("busy")), 1);

        QFile release(release_path);
        QVERIFY(release.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        release.close();
        if (child_a.state() != QProcess::NotRunning) {
            QVERIFY2(child_a.waitForFinished(10'000), qPrintable(child_a.errorString()));
        }
        if (child_b.state() != QProcess::NotRunning) {
            QVERIFY2(child_b.waitForFinished(10'000), qPrintable(child_b.errorString()));
        }
        const auto output_a = child_a.readAllStandardOutput() + child_a.readAllStandardError();
        const auto output_b = child_b.readAllStandardOutput() + child_b.readAllStandardError();
        QVERIFY2(child_a.exitCode() == 0, output_a.constData());
        QVERIFY2(child_b.exitCode() == 0, output_b.constData());

        // A later public open starts a new preflight and succeeds in one call.
        auto reopened = SessionStore::open(path);
        if (!reopened) {
            QFAIL(qPrintable(reopened.error().message));
        }
    }
#endif
}

void SessionStoreTest::cooperativeRaceHelper() {
    const auto path = qEnvironmentVariable("APPELLATE_LOCK_RACE_DB");
    const auto ready_path = qEnvironmentVariable("APPELLATE_LOCK_RACE_READY");
    const auto start_path = qEnvironmentVariable("APPELLATE_LOCK_RACE_START");
    const auto result_path = qEnvironmentVariable("APPELLATE_LOCK_RACE_RESULT");
    const auto release_path = qEnvironmentVariable("APPELLATE_LOCK_RACE_RELEASE");
    if (path.isEmpty() || ready_path.isEmpty() || start_path.isEmpty() || result_path.isEmpty() ||
        release_path.isEmpty()) {
        QSKIP("Internal cooperative race helper");
    }
    QFile ready(ready_path);
    QVERIFY(ready.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    ready.close();
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(start_path), 5'000);

    auto opened = SessionStore::open(path);
    QByteArray result_value;
    if (opened) {
        result_value = QByteArrayLiteral("opened");
    } else if (opened.error().code == StoreErrorCode::StateInUse) {
        result_value = QByteArrayLiteral("busy");
    } else {
        QFAIL(qPrintable(opened.error().message));
    }
    QFile result(result_path);
    QVERIFY(result.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(result.write(result_value), static_cast<qint64>(result_value.size()));
    result.close();
    if (opened) {
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(release_path), 10'000);
    }
}

} // namespace

QTEST_GUILESS_MAIN(SessionStoreTest)

#include "tst_session_store.moc"
