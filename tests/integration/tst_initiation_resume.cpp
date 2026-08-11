#include "session_controller.hpp"

#include "appellate/storage/asset_store.hpp"
#include "appellate/storage/session_store.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace appellate::app {

class SessionControllerTestAccess final {
  public:
    [[nodiscard]] static auto
    create(model::ProcedureDefinition procedure, model::CaseDefinition case_definition,
           model::SessionState initial_state, storage::AssetStore asset_store,
           std::unique_ptr<storage::SessionStore> session_store, QString engine_revision,
           QString created_at_utc, std::vector<storage::RevisionPin> pins)
        -> std::expected<std::unique_ptr<SessionController>, SessionControllerError> {
        return SessionController::create(std::move(procedure), std::move(case_definition),
                                         std::move(initial_state), std::move(asset_store),
                                         std::move(session_store), std::move(engine_revision),
                                         std::move(created_at_utc), std::move(pins));
    }
};

class RecordAccessSessionControllerTestAccess final {
  public:
    [[nodiscard]] static auto create(QString session_id, model::RecordAccessPolicy policy,
                                     std::unique_ptr<storage::SessionStore> session_store,
                                     QString engine_revision, QString created_at_utc,
                                     std::vector<storage::RevisionPin> pins)
        -> std::expected<std::unique_ptr<RecordAccessSessionController>, SessionControllerError> {
        return RecordAccessSessionController::create(
            std::move(session_id), std::move(policy), std::move(session_store),
            std::move(engine_revision), std::move(created_at_utc), std::move(pins));
    }

    [[nodiscard]] static auto reopen(QString session_id, model::RecordAccessPolicy policy,
                                     std::unique_ptr<storage::SessionStore> session_store,
                                     QString engine_revision,
                                     std::vector<storage::RevisionPin> pins)
        -> std::expected<std::unique_ptr<RecordAccessSessionController>, SessionControllerError> {
        return RecordAccessSessionController::reopen(std::move(session_id), std::move(policy),
                                                     std::move(session_store),
                                                     std::move(engine_revision), std::move(pins));
    }
};

template <typename Controller>
concept ExposesRecordAccessProjection =
    requires(const Controller& controller) { controller.projection(); };

static_assert(!ExposesRecordAccessProjection<RecordAccessSessionController>);

} // namespace appellate::app

namespace {

class InitiationResumeTest final : public QObject {
    Q_OBJECT

  private slots:
    void initiationSurvivesCloseAndReopen_data();
    void initiationSurvivesCloseAndReopen();
    void rejectsCanonicalAuthorityDefinitions();
    void recordAccessGrantAndRevokeSurviveCloseAndReopen();
    void recordAccessReopenRejectsTampering();
    void recordAccessReopenRejectsTimestampTampering();
};

using appellate::app::SessionController;
using namespace appellate::model;
using namespace std::chrono_literals;

[[nodiscard]] AuthorityBasis authority(const std::string& id, const std::string& proposition) {
    return AuthorityBasis{AuthorityRef{
                              AuthorityId{id},
                              "Synthetic CA4 Rule " + id,
                              "2026-08-11",
                              proposition,
                          },
                          {}};
}

[[nodiscard]] ProcedureDefinition procedure() {
    return ProcedureDefinition{
        ProcedureId{"us.ca4.civil-appeal"},
        CourtCalendar{},
        InitiatingFilingRule{
            FilingTypeId{"notice-of-appeal"},
            {ActorRoleId{"appellant"}},
            {FilingFieldId{"caption"}, FilingFieldId{"judgment-date"}},
            CureDeadlineRule{14, DeadlineCounting::CalendarDays, true},
            authority("frap.3", "A notice of appeal initiates an appeal."),
            authority("frap.3.c", "A party may file its notice of appeal."),
            authority("ca4.local.45", "The clerk issues a curable deficiency notice."),
        },
    };
}

[[nodiscard]] CaseDefinition caseDefinition() {
    return CaseDefinition{
        CaseId{"synthetic.civil.001"},
        ProcedureId{"us.ca4.civil-appeal"},
        {CaseActor{ActorId{"actor.appellant"}, ActorRoleId{"appellant"}}},
    };
}

[[nodiscard]] SessionState initialState(const std::string& session_id) {
    return SessionState{
        SessionId{session_id},
        ProcedureId{"us.ca4.civil-appeal"},
        CaseId{"synthetic.civil.001"},
        SessionPhase::AwaitingInitiatingFiling,
        1,
        std::nullopt,
        std::nullopt,
        {},
    };
}

[[nodiscard]] LegalTime explicitSubmissionTime() {
    constexpr auto date = std::chrono::year{2026} / std::chrono::August / 11;
    return LegalTime{
        std::chrono::sys_seconds{std::chrono::sys_days{date}} + 14h,
        LegalDate{date},
    };
}

[[nodiscard]] std::string digest(const QByteArray& bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString();
}

void InitiationResumeTest::initiationSurvivesCloseAndReopen_data() {
    QTest::addColumn<bool>("deficient");
    QTest::newRow("accepted") << false;
    QTest::newRow("deficient") << true;
}

void InitiationResumeTest::initiationSurvivesCloseAndReopen() {
    QFETCH(bool, deficient);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const auto database_path = QDir(directory.path()).filePath(QStringLiteral("sessions.sqlite"));
    const auto asset_root = QDir(directory.path()).filePath(QStringLiteral("assets"));
    const auto session_id =
        deficient ? std::string("session.deficient") : std::string("session.accepted");
    const auto definition = procedure();
    const auto case_definition = caseDefinition();
    const auto initial = initialState(session_id);

    auto opened = appellate::storage::SessionStore::open(database_path);
    if (!opened) {
        QFAIL(qPrintable(opened.error().message));
    }
    auto created = appellate::app::SessionControllerTestAccess::create(
        definition, case_definition, initial,
        appellate::storage::AssetStore(asset_root, 1024 * 1024), std::move(*opened),
        QStringLiteral("engine.test.1"), QStringLiteral("2026-08-11T13:59:00Z"),
        {appellate::storage::RevisionPin{
            QStringLiteral("example.ca4"),
            QStringLiteral("1.0.0"),
            QString(64, u'a'),
        }});
    if (!created) {
        QFAIL(qPrintable(created.error().message));
    }
    auto controller = std::move(*created);

    const QByteArray document = deficient ? QByteArray("notice missing judgment date")
                                          : QByteArray("complete notice of appeal");
    std::vector<SubmittedField> fields{
        SubmittedField{FilingFieldId{"caption"}, "Appellant v. Appellee"},
    };
    if (!deficient) {
        fields.push_back(SubmittedField{FilingFieldId{"judgment-date"}, "2026-08-01"});
    }
    const SubmitFiling command{
        SessionId{session_id},
        SubmissionId{deficient ? "submission.deficient" : "submission.accepted"},
        ActorId{"actor.appellant"},
        FilingTypeId{"notice-of-appeal"},
        explicitSubmissionTime(),
        digest(document),
        std::move(fields),
    };

    const auto submitted =
        controller->submit(command, document, QStringLiteral("2026-08-11T14:00:00Z"));
    if (!submitted) {
        QFAIL(qPrintable(submitted.error().message));
    }
    QCOMPARE(submitted->persisted_sequence, qint64{1});
    QCOMPARE(submitted->events.size(), std::size_t{1});
    QCOMPARE(controller->snapshot().docket.size(), std::size_t{1});
    QCOMPARE(controller->snapshot().docket.front().status,
             deficient ? QStringLiteral("deficient") : QStringLiteral("accepted"));
    QCOMPARE(controller->snapshot().asset_references.size(), std::size_t{1});
    QCOMPARE(controller->snapshot().asset_references.front().digest, submitted->asset.sha256);
    QCOMPARE(controller->snapshot().asset_references.front().purpose,
             QStringLiteral("filing-document"));

    const auto state_before_close = controller->state();
    const auto snapshot_before_close = controller->snapshot();
    const auto asset_digest = submitted->asset.sha256;
    controller.reset();

    auto reopened_store = appellate::storage::SessionStore::open(database_path);
    if (!reopened_store) {
        QFAIL(qPrintable(reopened_store.error().message));
    }
    auto reopened = SessionController::reopen(
        definition, case_definition, initial,
        appellate::storage::AssetStore(asset_root, 1024 * 1024), std::move(*reopened_store),
        snapshot_before_close.engine_revision, snapshot_before_close.pins);
    if (!reopened) {
        QFAIL(qPrintable(reopened.error().message));
    }

    QVERIFY((*reopened)->state() == state_before_close);
    QCOMPARE((*reopened)->snapshot().session_id, snapshot_before_close.session_id);
    QCOMPARE((*reopened)->snapshot().engine_revision, snapshot_before_close.engine_revision);
    QCOMPARE((*reopened)->snapshot().sequence, snapshot_before_close.sequence);
    QVERIFY((*reopened)->snapshot().pins == snapshot_before_close.pins);
    QVERIFY((*reopened)->snapshot().commands == snapshot_before_close.commands);
    QVERIFY((*reopened)->snapshot().events == snapshot_before_close.events);
    QVERIFY((*reopened)->snapshot().docket == snapshot_before_close.docket);
    QVERIFY((*reopened)->snapshot().asset_references == snapshot_before_close.asset_references);

    appellate::storage::AssetStore verifier(asset_root, 1024 * 1024);
    const auto restored_document = verifier.read(asset_digest);
    if (!restored_document) {
        QFAIL(qPrintable(restored_document.error().message));
    }
    QCOMPARE(*restored_document, document);
}

void InitiationResumeTest::rejectsCanonicalAuthorityDefinitions() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto database_path = QDir(directory.path()).filePath(QStringLiteral("sessions.sqlite"));
    const auto asset_root = QDir(directory.path()).filePath(QStringLiteral("assets"));
    const auto initial = initialState("session.canonical-rejected");
    auto canonical = procedure();
    canonical.initiating_filing.filing_authority.primary.provenance = AuthorityProvenance{
        AuthorityType::Rule,
        "us.ca4",
        "us.ca4.clerk",
        PrecedentialStatus::NotApplicable,
        true,
        "2026-08-11",
        "FRAP 3",
        "https://www.uscourts.gov/rules-policies/current-rules-practice-procedure",
    };
    const std::vector<appellate::storage::RevisionPin> revision_pins{
        {QStringLiteral("example.ca4"), QStringLiteral("1.0.0"), QString(64, u'a')}};

    auto store = appellate::storage::SessionStore::open(database_path);
    QVERIFY(store.has_value());
    const auto rejected_create = appellate::app::SessionControllerTestAccess::create(
        canonical, caseDefinition(), initial,
        appellate::storage::AssetStore(asset_root, 1024 * 1024), std::move(*store),
        QStringLiteral("engine.test.1"), QStringLiteral("2026-08-11T13:59:00Z"), revision_pins);
    QVERIFY(!rejected_create.has_value());
    QCOMPARE(rejected_create.error().code,
             appellate::app::SessionControllerErrorCode::InvalidConfiguration);

    store = appellate::storage::SessionStore::open(database_path);
    QVERIFY(store.has_value());
    auto created = appellate::app::SessionControllerTestAccess::create(
        procedure(), caseDefinition(), initial,
        appellate::storage::AssetStore(asset_root, 1024 * 1024), std::move(*store),
        QStringLiteral("engine.test.1"), QStringLiteral("2026-08-11T13:59:00Z"), revision_pins);
    QVERIFY(created.has_value());
    QCOMPARE((*created)->snapshot().authority_contract,
             appellate::storage::SessionAuthorityContract::LegacyV1);

    created->reset();
    store = appellate::storage::SessionStore::open(database_path);
    QVERIFY(store.has_value());
    const auto rejected_reopen = SessionController::reopen(
        canonical, caseDefinition(), initial,
        appellate::storage::AssetStore(asset_root, 1024 * 1024), std::move(*store),
        QStringLiteral("engine.test.1"), revision_pins);
    QVERIFY(!rejected_reopen.has_value());
    QCOMPARE(rejected_reopen.error().code,
             appellate::app::SessionControllerErrorCode::InvalidConfiguration);
}

void InitiationResumeTest::recordAccessGrantAndRevokeSurviveCloseAndReopen() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto database_path = directory.filePath(QStringLiteral("record-access.sqlite"));
    const RecordAccessPolicy policy{
        "test.record.psr",
        "test.policy.psr",
        {{"test.document.psr", "test.authority.psr-access", "test.disclosure.psr", {}}}};
    const std::vector<appellate::storage::RevisionPin> revision_pins{
        {QStringLiteral("test.pack.psr"), QStringLiteral("2.0.0"), QString(64, u'a')}};

    auto store = appellate::storage::SessionStore::open(database_path);
    QVERIFY(store.has_value());
    auto controller = appellate::app::RecordAccessSessionControllerTestAccess::create(
        QStringLiteral("test.session.psr"), policy, std::move(*store),
        QStringLiteral("engine.record-access.v1"), QStringLiteral("2026-08-11T10:00:00Z"),
        revision_pins);
    QVERIFY2(controller.has_value(), controller ? "" : qPrintable(controller.error().message));
    const auto raw_id_rejected = (*controller)
                                     ->grant("test.document.psr", "test.event.raw-id-rejected",
                                             QStringLiteral("2026-08-11T10:00:30Z"));
    QVERIFY(!raw_id_rejected.has_value());
    QCOMPARE(raw_id_rejected.error().code,
             appellate::app::SessionControllerErrorCode::InvalidConfiguration);
    QCOMPARE((*controller)->snapshot().sequence, qint64{0});
    const auto granted = (*controller)
                             ->grant("test.disclosure.psr", "test.event.psr-grant",
                                     QStringLiteral("2026-08-11T10:01:00Z"));
    QVERIFY2(granted.has_value(), granted ? "" : qPrintable(granted.error().message));
    QVERIFY((*controller)->disclosures().front().authorized);
    const auto redundant = (*controller)
                               ->grant("test.disclosure.psr", "test.event.psr-grant-again",
                                       QStringLiteral("2026-08-11T10:01:30Z"));
    QVERIFY(!redundant.has_value());
    QCOMPARE((*controller)->snapshot().sequence, qint64{1});
    controller->reset();

    store = appellate::storage::SessionStore::open(database_path);
    QVERIFY(store.has_value());
    controller = appellate::app::RecordAccessSessionControllerTestAccess::reopen(
        QStringLiteral("test.session.psr"), policy, std::move(*store),
        QStringLiteral("engine.record-access.v1"), revision_pins);
    QVERIFY2(controller.has_value(), controller ? "" : qPrintable(controller.error().message));
    QVERIFY((*controller)->disclosures().front().authorized);
    const auto revoked = (*controller)
                             ->revoke("test.disclosure.psr", "test.event.psr-revoke",
                                      QStringLiteral("2026-08-11T10:02:00Z"));
    QVERIFY2(revoked.has_value(), revoked ? "" : qPrintable(revoked.error().message));
    QVERIFY(!(*controller)->disclosures().front().authorized);
    const auto prefix = (*controller)->auditProjectionAt(1);
    QVERIFY2(prefix.has_value(), prefix ? "" : qPrintable(prefix.error().message));
    QCOMPARE(prefix->authorizedDisclosureIds(), std::vector<std::string>{"test.disclosure.psr"});
    controller->reset();

    store = appellate::storage::SessionStore::open(database_path);
    QVERIFY(store.has_value());
    controller = appellate::app::RecordAccessSessionControllerTestAccess::reopen(
        QStringLiteral("test.session.psr"), policy, std::move(*store),
        QStringLiteral("engine.record-access.v1"), revision_pins);
    QVERIFY2(controller.has_value(), controller ? "" : qPrintable(controller.error().message));
    QVERIFY(!(*controller)->disclosures().front().authorized);
    QCOMPARE((*controller)->snapshot().authority_contract,
             appellate::storage::SessionAuthorityContract::CanonicalV2);

    auto blocked_policy = policy;
    blocked_policy.rules.front().blocking_deficiencies.push_back(RecordDisclosureDeficiency{
        "test.disclosure.psr", RecordDisclosureDeficiencyKind::MissingCertificate});
    store = appellate::storage::SessionStore::open(
        directory.filePath(QStringLiteral("blocked-record-access.sqlite")));
    QVERIFY(store.has_value());
    auto blocked_controller = appellate::app::RecordAccessSessionControllerTestAccess::create(
        QStringLiteral("test.session.psr-blocked"), blocked_policy, std::move(*store),
        QStringLiteral("engine.record-access.v1"), QStringLiteral("2026-08-11T10:03:00Z"),
        revision_pins);
    QVERIFY(blocked_controller.has_value());
    const auto blocked = (*blocked_controller)
                             ->grant("test.disclosure.psr", "test.event.psr-blocked",
                                     QStringLiteral("2026-08-11T10:04:00Z"));
    QVERIFY(!blocked.has_value());
    QCOMPARE(blocked.error().code, appellate::app::SessionControllerErrorCode::EventCodecFailure);
    QCOMPARE((*blocked_controller)->snapshot().sequence, qint64{0});
}

void InitiationResumeTest::recordAccessReopenRejectsTampering() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto database_path = directory.filePath(QStringLiteral("tampered-access.sqlite"));
    const RecordAccessPolicy policy{
        "test.record.psr",
        "test.policy.psr",
        {{"test.document.psr", "test.authority.psr-access", "test.disclosure.psr", {}}}};
    const std::vector<appellate::storage::RevisionPin> revision_pins{
        {QStringLiteral("test.pack.psr"), QStringLiteral("2.0.0"), QString(64, u'a')}};
    auto store = appellate::storage::SessionStore::open(database_path);
    QVERIFY(store.has_value());
    auto controller = appellate::app::RecordAccessSessionControllerTestAccess::create(
        QStringLiteral("test.session.psr-tamper"), policy, std::move(*store),
        QStringLiteral("engine.record-access.v1"), QStringLiteral("2026-08-11T10:00:00Z"),
        revision_pins);
    QVERIFY(controller.has_value());
    QVERIFY((*controller)
                ->grant("test.disclosure.psr", "test.event.psr-grant",
                        QStringLiteral("2026-08-11T10:01:00Z"))
                .has_value());
    controller->reset();

    store = appellate::storage::SessionStore::open(database_path);
    QVERIFY(store.has_value());
    auto changed_authority = policy;
    changed_authority.rules.front().authority_id = "test.authority.changed";
    const auto forged_authority = appellate::app::RecordAccessSessionControllerTestAccess::reopen(
        QStringLiteral("test.session.psr-tamper"), changed_authority, std::move(*store),
        QStringLiteral("engine.record-access.v1"), revision_pins);
    QVERIFY(!forged_authority.has_value());
    QCOMPARE(forged_authority.error().code,
             appellate::app::SessionControllerErrorCode::CorruptSession);

    store = appellate::storage::SessionStore::open(database_path);
    QVERIFY(store.has_value());
    auto stripped_policy = policy;
    stripped_policy.rules.clear();
    const auto forged_policy = appellate::app::RecordAccessSessionControllerTestAccess::reopen(
        QStringLiteral("test.session.psr-tamper"), stripped_policy, std::move(*store),
        QStringLiteral("engine.record-access.v1"), revision_pins);
    QVERIFY(!forged_policy.has_value());
    QCOMPARE(forged_policy.error().code,
             appellate::app::SessionControllerErrorCode::CorruptSession);

    const auto connection =
        QStringLiteral("tamper-record-access-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(database_path);
        QVERIFY(database.open());
        QSqlQuery query(database);
        QVERIFY(query.exec(
            QStringLiteral("UPDATE event_log SET authority_id = 'test.authority.forged' "
                           "WHERE session_id = 'test.session.psr-tamper' AND sequence = 1")));
        QCOMPARE(query.numRowsAffected(), qint64{1});
        database.close();
        database = QSqlDatabase{};
    }
    QSqlDatabase::removeDatabase(connection);

    store = appellate::storage::SessionStore::open(database_path);
    QVERIFY(store.has_value());
    const auto reopened = appellate::app::RecordAccessSessionControllerTestAccess::reopen(
        QStringLiteral("test.session.psr-tamper"), policy, std::move(*store),
        QStringLiteral("engine.record-access.v1"), revision_pins);
    QVERIFY(!reopened.has_value());
    QCOMPARE(reopened.error().code, appellate::app::SessionControllerErrorCode::CorruptSession);
}

void InitiationResumeTest::recordAccessReopenRejectsTimestampTampering() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto database_path = directory.filePath(QStringLiteral("tampered-access-time.sqlite"));
    const RecordAccessPolicy policy{
        "test.record.psr",
        "test.policy.psr",
        {{"test.document.psr", "test.authority.psr-access", "test.disclosure.psr", {}}}};
    const std::vector<appellate::storage::RevisionPin> revision_pins{
        {QStringLiteral("test.pack.psr"), QStringLiteral("2.0.0"), QString(64, u'a')}};
    auto store = appellate::storage::SessionStore::open(database_path);
    QVERIFY(store.has_value());
    auto controller = appellate::app::RecordAccessSessionControllerTestAccess::create(
        QStringLiteral("test.session.psr-time-tamper"), policy, std::move(*store),
        QStringLiteral("engine.record-access.v1"), QStringLiteral("2026-08-11T10:00:00Z"),
        revision_pins);
    QVERIFY(controller.has_value());
    QVERIFY((*controller)
                ->grant("test.disclosure.psr", "test.event.psr-time-grant",
                        QStringLiteral("2026-08-11T10:01:00Z"))
                .has_value());
    controller->reset();

    const auto connection = QStringLiteral("tamper-record-access-time-%1")
                                .arg(QUuid::createUuid().toString(QUuid::Id128));
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(database_path);
        QVERIFY(database.open());
        QSqlQuery query(database);
        QVERIFY(query.exec(
            QStringLiteral("UPDATE command_log SET recorded_at_utc = '2026-08-11T10:01:01Z' "
                           "WHERE session_id = 'test.session.psr-time-tamper'")));
        QCOMPARE(query.numRowsAffected(), qint64{1});
        database.close();
        database = QSqlDatabase{};
    }
    QSqlDatabase::removeDatabase(connection);

    store = appellate::storage::SessionStore::open(database_path);
    QVERIFY(store.has_value());
    const auto reopened = appellate::app::RecordAccessSessionControllerTestAccess::reopen(
        QStringLiteral("test.session.psr-time-tamper"), policy, std::move(*store),
        QStringLiteral("engine.record-access.v1"), revision_pins);
    QVERIFY(!reopened.has_value());
    QCOMPARE(reopened.error().code, appellate::app::SessionControllerErrorCode::CorruptSession);
}

} // namespace

QTEST_GUILESS_MAIN(InitiationResumeTest)

#include "tst_initiation_resume.moc"
