#include "appellate/storage/session_store.hpp"

#include <QTemporaryDir>
#include <QTest>

namespace {

using appellate::storage::CommitBatch;
using appellate::storage::DocketWrite;
using appellate::storage::EventWrite;
using appellate::storage::RevisionPin;
using appellate::storage::SessionStore;
using appellate::storage::StoreErrorCode;

constexpr auto digest = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

class SessionStoreTest final : public QObject {
    Q_OBJECT

  private slots:
    void migratesFreshDatabase();
    void persistsAndReopensPinnedSession();
    void rejectsStaleSequenceWithoutPartialWrite();
    void rollsBackDuplicateCommand();
};

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
    QCOMPARE(snapshot->events.size(), std::size_t{1});
    QCOMPARE(snapshot->docket.size(), std::size_t{1});
    QCOMPARE(snapshot->docket.front().title, QStringLiteral("Notice of appeal"));
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
    QCOMPARE(snapshot->events.size(), std::size_t{1});
    QCOMPARE(snapshot->docket.size(), std::size_t{1});
}

} // namespace

QTEST_GUILESS_MAIN(SessionStoreTest)

#include "tst_session_store.moc"
