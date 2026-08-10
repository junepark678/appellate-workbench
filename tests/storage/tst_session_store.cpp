#include "appellate/storage/session_store.hpp"

#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

namespace {

using appellate::storage::AssetReference;
using appellate::storage::CommitBatch;
using appellate::storage::DocketWrite;
using appellate::storage::EventWrite;
using appellate::storage::RevisionPin;
using appellate::storage::SessionStore;
using appellate::storage::StoredCommand;
using appellate::storage::StoreErrorCode;

constexpr auto digest = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr auto second_digest = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
constexpr auto third_digest = "1111111111111111111111111111111111111111111111111111111111111111";

class SessionStoreTest final : public QObject {
    Q_OBJECT

  private slots:
    void migratesFreshDatabase();
    void refusesNewerSchemaWithoutMutation();
    void persistsAndReopensPinnedSession();
    void rejectsStaleSequenceWithoutPartialWrite();
    void rollsBackDuplicateCommand();
    void rejectsInvalidAndDuplicateAssetReferencesWithoutWrites();
    void rollsBackDuplicateStoredAssetReference();
    void backsUpAndRestoresConsistentSnapshot();
    void rejectsCorruptRestoreWithoutCreatingDestination();
};

[[nodiscard]] bool createFutureDatabase(const QString& path) {
    const auto connection =
        QStringLiteral("future-schema-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    bool created = false;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(path);
        if (database.open()) {
            QSqlQuery query(database);
            created = query.exec(QStringLiteral(
                          "CREATE TABLE schema_migrations (version INTEGER PRIMARY KEY, "
                          "applied_at_utc TEXT NOT NULL) STRICT")) &&
                      query.exec(QStringLiteral(
                          "INSERT INTO schema_migrations VALUES(2, 'future')")) &&
                      query.exec(QStringLiteral(
                          "CREATE TABLE future_sentinel (value TEXT NOT NULL) STRICT")) &&
                      query.exec(QStringLiteral(
                          "INSERT INTO future_sentinel VALUES('untouched')"));
            database.close();
        }
        database = QSqlDatabase{};
    }
    QSqlDatabase::removeDatabase(connection);
    return created;
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
        {
            AssetReference{QString::fromLatin1(digest), QStringLiteral("record.exhibit")},
            AssetReference{QString::fromLatin1(second_digest), QStringLiteral("brief.primary")},
        },
    };
}

void SessionStoreTest::migratesFreshDatabase() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    auto store = SessionStore::open(temporary.filePath(QStringLiteral("sessions.sqlite")));
    if (!store.has_value()) {
        QFAIL(qPrintable(store.error().message));
    }
    QCOMPARE((*store)->schemaVersion(), 1);
}

void SessionStoreTest::refusesNewerSchemaWithoutMutation() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto path = temporary.filePath(QStringLiteral("future.sqlite"));
    QVERIFY(createFutureDatabase(path));

    const auto opened = SessionStore::open(path);
    QVERIFY(!opened.has_value());
    QCOMPARE(opened.error().code, StoreErrorCode::MigrationFailed);
    QVERIFY(futureDatabaseIsUntouched(path));
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
    const std::vector expected_assets{
        AssetReference{QString::fromLatin1(second_digest), QStringLiteral("brief.primary")},
        AssetReference{QString::fromLatin1(digest), QStringLiteral("record.exhibit")},
    };
    QCOMPARE(snapshot->asset_references, expected_assets);
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
    QCOMPARE(snapshot->asset_references.size(), std::size_t{2});
}

void SessionStoreTest::rejectsInvalidAndDuplicateAssetReferencesWithoutWrites() {
    QTemporaryDir temporary;
    auto store = SessionStore::open(temporary.filePath(QStringLiteral("sessions.sqlite")));
    QVERIFY(store.has_value());
    QVERIFY((*store)
                ->createSession(QStringLiteral("session-1"), QStringLiteral("engine-1"),
                                QStringLiteral("2026-08-11T00:00:00Z"), pins())
                .has_value());

    auto invalid_digest = acceptedFiling(QStringLiteral("command-invalid-digest"));
    invalid_digest.asset_references.front().digest = QString(64, u'A');
    auto result = (*store)->append(QStringLiteral("session-1"), 0, invalid_digest);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, StoreErrorCode::InvalidArgument);

    auto invalid_purpose = acceptedFiling(QStringLiteral("command-invalid-purpose"));
    invalid_purpose.asset_references.front().purpose = QStringLiteral("Record Exhibit");
    result = (*store)->append(QStringLiteral("session-1"), 0, invalid_purpose);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, StoreErrorCode::InvalidArgument);

    auto oversized_purpose = acceptedFiling(QStringLiteral("command-oversized-purpose"));
    oversized_purpose.asset_references.front().purpose = QString(129, u'a');
    result = (*store)->append(QStringLiteral("session-1"), 0, oversized_purpose);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, StoreErrorCode::InvalidArgument);

    auto duplicate = acceptedFiling(QStringLiteral("command-duplicate-reference"));
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

void SessionStoreTest::rollsBackDuplicateStoredAssetReference() {
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

    auto duplicate = acceptedFiling(QStringLiteral("command-2"));
    const auto rejected = (*store)->append(QStringLiteral("session-1"), 1, duplicate);
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, StoreErrorCode::ConstraintViolation);

    auto snapshot = (*store)->loadSession(QStringLiteral("session-1"));
    QVERIFY(snapshot.has_value());
    QCOMPARE(snapshot->sequence, 1);
    QCOMPARE(snapshot->commands.size(), std::size_t{1});
    QCOMPARE(snapshot->events.size(), std::size_t{1});
    QCOMPARE(snapshot->asset_references.size(), std::size_t{2});

    duplicate.asset_references = {
        AssetReference{QString::fromLatin1(third_digest), QStringLiteral("record.supplement")}};
    const auto retried = (*store)->append(QStringLiteral("session-1"), 1, duplicate);
    QVERIFY(retried.has_value());
    QCOMPARE(*retried, 2);

    snapshot = (*store)->loadSession(QStringLiteral("session-1"));
    QVERIFY(snapshot.has_value());
    QCOMPARE(snapshot->sequence, 2);
    QCOMPARE(snapshot->commands.size(), std::size_t{2});
    QCOMPARE(snapshot->events.size(), std::size_t{2});
    QCOMPARE(snapshot->asset_references.size(), std::size_t{3});
}

void SessionStoreTest::backsUpAndRestoresConsistentSnapshot() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto live_path = temporary.filePath(QStringLiteral("live.sqlite"));
    const auto backup_path = temporary.filePath(QStringLiteral("backup.sqlite"));
    const auto restored_path = temporary.filePath(QStringLiteral("restored.sqlite"));
    {
        auto store = SessionStore::open(live_path);
        QVERIFY(store.has_value());
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

    const auto restored = SessionStore::restoreBackup(backup_path, restored_path);
    if (!restored.has_value()) {
        QFAIL(qPrintable(restored.error().message));
    }
    const auto overwrite = SessionStore::restoreBackup(backup_path, restored_path);
    QVERIFY(!overwrite.has_value());
    QCOMPARE(overwrite.error().code, StoreErrorCode::RestoreFailed);

    auto store = SessionStore::open(restored_path);
    QVERIFY(store.has_value());
    const auto snapshot = (*store)->loadSession(QStringLiteral("session-1"));
    QVERIFY(snapshot.has_value());
    QCOMPARE(snapshot->sequence, qint64{1});
    QCOMPARE(snapshot->commands.size(), std::size_t{1});
    QCOMPARE(snapshot->events.size(), std::size_t{1});
    QCOMPARE(snapshot->docket.size(), std::size_t{1});
    QCOMPARE(snapshot->asset_references.size(), std::size_t{2});
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

} // namespace

QTEST_GUILESS_MAIN(SessionStoreTest)

#include "tst_session_store.moc"
