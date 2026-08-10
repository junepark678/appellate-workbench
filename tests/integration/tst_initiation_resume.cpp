#include "session_controller.hpp"

#include "appellate/storage/asset_store.hpp"
#include "appellate/storage/session_store.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QTemporaryDir>
#include <QTest>

#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace {

class InitiationResumeTest final : public QObject {
    Q_OBJECT

  private slots:
    void initiationSurvivesCloseAndReopen_data();
    void initiationSurvivesCloseAndReopen();
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
    auto created = SessionController::create(
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

} // namespace

QTEST_GUILESS_MAIN(InitiationResumeTest)

#include "tst_initiation_resume.moc"
