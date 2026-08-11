#include "oral_argument_session_controller.hpp"

#include "appellate/engine/oral_argument_engine.hpp"
#include "appellate/packs/runtime_pack.hpp"
#include "appellate/storage/oral_argument_codec.hpp"
#include "appellate/storage/session_store.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace appellate::app {

class OralArgumentSessionControllerTestAccess final {
  public:
    [[nodiscard]] static auto createCanonical(
        QString session_id, model::CanonicalOralArgumentDefinition definition,
        std::unique_ptr<storage::SessionStore> session_store, QString engine_revision,
        QString created_at_utc, std::vector<storage::RevisionPin> pins) {
        return OralArgumentSessionController::createCanonicalForTesting(
            std::move(session_id), std::move(definition), std::move(session_store),
            std::move(engine_revision), std::move(created_at_utc), std::move(pins));
    }

    [[nodiscard]] static auto reopenCanonical(
        QString session_id, model::CanonicalOralArgumentDefinition definition,
        std::unique_ptr<storage::SessionStore> session_store, QString engine_revision,
        std::vector<storage::RevisionPin> pins) {
        return OralArgumentSessionController::reopenCanonicalForTesting(
            std::move(session_id), std::move(definition), std::move(session_store),
            std::move(engine_revision), std::move(pins));
    }

    [[nodiscard]] static auto deriveCanonical(
        const model::CaseId& case_id,
        const packs::RuntimeArgumentConfigId& argument_configuration_id,
        std::string legal_state_digest, const packs::RuntimePack& runtime_pack) {
        return OralArgumentSessionController::deriveCanonicalDefinitionForTesting(
            case_id, argument_configuration_id, std::move(legal_state_digest), runtime_pack);
    }
};

} // namespace appellate::app

namespace {

class OralArgumentSessionResumeTest final : public QObject {
    Q_OBJECT

  private slots:
    void answerJournalAndTranscriptSurviveExactReopen();
    void rejectsPersistedTampering_data();
    void rejectsPersistedTampering();
    void rejectsChangedOperativeProfileGroundingConfigurationAndRevisionPins();
    void profileIdentityCannotAlterPinnedOutcome();
    void canonicalRowsAndDefinitionSurviveExactReopen();
    void canonicalReopenRejectsDowngradeAndSwappedGrounding_data();
    void canonicalReopenRejectsDowngradeAndSwappedGrounding();
    void counterfactualSessionCannotChangePinnedLegalStateOrDisposition();
    void derivesTimingAndStructuredDispositionFromRuntimeBoundary();
    void rejectsCanonicalEvent65BeforeAppend();
};

namespace app = appellate::app;
namespace engine = appellate::engine;
namespace model = appellate::model;
namespace storage = appellate::storage;
using namespace std::chrono_literals;

constexpr auto session_id = "test.session.oral-argument";
constexpr auto engine_revision = "engine.oral-argument.v1";
constexpr auto canonical_session_id = "test.session.canonical-argument";
constexpr auto canonical_engine_revision = "engine.oral-argument.v2";

struct Definitions final {
    model::OralArgumentConfiguration configuration;
    model::BenchConfiguration bench;
    model::ArgumentGrounding grounding;
};

[[nodiscard]] model::JudgeProfile profile() {
    return model::JudgeProfile{
        "fictional.persistence-composite",
        "Persistence Composite",
        model::ProfileClass::FictionalComposite,
        model::ProfileCompatibility{{model::CourtRole::Appellate}, {"us.ca4"}},
        model::InteractionStyle{
            0.9,
            0.8,
            0.3,
            0.8,
            0.8,
            0.2,
            0.9,
            1.0,
            0.9,
            {model::IssueFocus{"issue.preservation", 1.0}},
        },
        model::VoiceStyle{
            model::VoiceRegister::Technical,
            model::VoiceCadence::Clipped,
            model::QuestionFraming::Direct,
            model::CounselAddress::Counsel,
            0.4,
            0.4,
            {"answer the preservation question"},
            {"before you move on"},
            {"clarify the proposition"},
        },
    };
}

[[nodiscard]] model::ArgumentGrounding grounding() {
    return model::ArgumentGrounding{{model::ArgumentIssue{
        "issue.preservation",
        "preservation of the objection",
        {
            {model::GroundingKind::Authority, "authority.preservation", std::nullopt},
            {model::GroundingKind::RecordPage, "record.transcript.page-47", 47},
        },
        {"Where did the appellant preserve the objection?"},
        {"Would the standard change if the objection appeared only after judgment?"},
    }}};
}

[[nodiscard]] Definitions definitions() {
    auto available = grounding();
    model::BenchConfiguration bench{
        "us.ca4",
        model::CourtRole::Appellate,
        {model::BenchSeat{"seat.presiding", profile()}},
        "seat.presiding",
    };
    const auto behavior_digest = engine::behaviorDefinitionDigest(bench);
    const auto grounding_digest = engine::groundingDigest(available);
    Q_ASSERT(behavior_digest.has_value());
    Q_ASSERT(grounding_digest.has_value());
    return Definitions{
        model::OralArgumentConfiguration{
            90s,
            20s,
            0.7,
            3,
            *behavior_digest,
            *grounding_digest,
            std::string(64, 'd'),
            "disposition.authored.synthetic-v1",
        },
        std::move(bench),
        std::move(available),
    };
}

[[nodiscard]] model::AuthorityRef canonicalAuthority() {
    return model::AuthorityRef{
        model::AuthorityId{"authority.canonical-standard"},
        "Synthetic Authority, 100 F.4th 1",
        "2026-01-15",
        "The court reviews preserved legal questions under the authored standard.",
        model::AuthorityProvenance{
            model::AuthorityType::Case,
            "us.ca4",
            "court.synthetic-appellate",
            model::PrecedentialStatus::Precedential,
            true,
            "2026-01-16",
            "100 F.4th 1",
            "https://court.example.test/opinions/100-f4th-1",
        },
    };
}

[[nodiscard]] model::CanonicalOralArgumentDefinition canonicalDefinition(
    model::OralArgumentMode mode = model::OralArgumentMode::ActualRecord) {
    auto canonical_profile = profile();
    canonical_profile.id = "fictional.reusable-composite";
    canonical_profile.display_name = "Reusable Composite";
    canonical_profile.interaction.issue_focus = {
        model::IssueFocus{"workbench.topic.record-support", 1.0},
        model::IssueFocus{"workbench.topic.remedy", 0.25},
    };
    model::BenchConfiguration bench{
        "us.ca4",
        model::CourtRole::Appellate,
        {model::BenchSeat{"seat.presiding", std::move(canonical_profile)}},
        "seat.presiding",
    };
    model::AuthoredQuestionBank bank{
        model::CaseId{"case.synthetic"},
        "case.synthetic.argument",
        mode,
        {},
        {model::ArgumentIssueTopics{
            "issue.synthetic",
            {model::ArgumentFocusTopic::RecordSupport, model::ArgumentFocusTopic::Remedy},
        }},
        {
            model::AuthoredArgumentQuestion{
                "case.synthetic.question-record",
                "issue.synthetic",
                model::ArgumentFocusTopic::RecordSupport,
                "Where does the exact record support the asserted proposition?",
                {
                    model::AuthorityArgumentGrounding{
                        "case.synthetic.grounding-authority", canonicalAuthority()},
                    model::RecordPageArgumentGrounding{
                        "case.synthetic.grounding-record", "case.synthetic.anchor-hearing",
                        "case.synthetic.entry-hearing", 47, std::string(64, 'a'),
                        std::string{"Hearing Tr. 47"}},
                },
            },
            model::AuthoredArgumentQuestion{
                "case.synthetic.question-remedy",
                "issue.synthetic",
                model::ArgumentFocusTopic::Remedy,
                "What relief follows if the court accepts that record proposition?",
                {model::BriefPageArgumentGrounding{
                    "case.synthetic.grounding-brief", "case.synthetic.entry-opening-brief", 12,
                    std::string(64, 'b')}},
            },
        },
    };
    const auto behavior_digest = engine::behaviorDefinitionDigest(bench);
    const auto grounding_digest = engine::groundingDigest(bank);
    Q_ASSERT(behavior_digest.has_value());
    Q_ASSERT(grounding_digest.has_value());
    bank.grounding_digest = *grounding_digest;
    return model::CanonicalOralArgumentDefinition{
        model::OralArgumentConfiguration{
            90s,
            20s,
            0.7,
            3,
            *behavior_digest,
            *grounding_digest,
            std::string(64, 'c'),
            "plan.authored-judgment",
        },
        std::move(bench),
        std::move(bank),
    };
}

[[nodiscard]] model::CounselAnswer canonicalAnswer() {
    return model::CounselAnswer{
        model::CounselActKind::Answer,
        "The requested relief follows from the cited resolved materials.",
        "issue.synthetic",
        {"case.synthetic.grounding-authority"},
        1.0,
        6s,
    };
}

[[nodiscard]] appellate::packs::RuntimePack canonicalRuntimePack(
    std::uint32_t total_seconds, std::uint32_t rebuttal_seconds,
    bool structured_disposition = true) {
    const auto definition = canonicalDefinition();
    appellate::packs::RuntimeBenchConfiguration runtime_bench{};
    runtime_bench.id = appellate::packs::RuntimeBenchConfigurationId{"bench.synthetic"};
    runtime_bench.court_id = appellate::packs::RuntimeCourtId{"court.synthetic-appellate"};
    runtime_bench.presiding_seat_id =
        appellate::packs::RuntimeBenchSeatId{"seat.presiding"};
    runtime_bench.seats.push_back(appellate::packs::RuntimeBenchSeat{
        appellate::packs::RuntimeBenchSeatId{"seat.presiding"},
        appellate::packs::RuntimeJudgeProfileId{"fictional.reusable-composite"},
        model::CourtRole::Appellate,
        definition.bench.seats.front().profile,
    });
    appellate::packs::RuntimeArgumentConfiguration argument{};
    argument.id = appellate::packs::RuntimeArgumentConfigId{"case.synthetic.argument"};
    argument.case_id = model::CaseId{"case.synthetic"};
    argument.bench = std::move(runtime_bench);
    argument.total_seconds = total_seconds;
    argument.rebuttal_seconds = rebuttal_seconds;
    argument.permitted_issue_ids = {
        appellate::packs::RuntimeIssueId{"issue.synthetic"}};
    argument.grounded_question_bank = definition.question_bank;

    appellate::packs::RuntimeCase runtime_case{};
    runtime_case.definition.id = model::CaseId{"case.synthetic"};
    if (structured_disposition) {
        runtime_case.definition.authored_disposition_plan_id =
            model::DispositionPlanId{"plan.structured-judgment"};
    }
    runtime_case.court.id =
        appellate::packs::RuntimeCourtId{"court.synthetic-appellate"};
    runtime_case.court.jurisdiction_id = appellate::packs::RuntimeJurisdictionId{"us.ca4"};
    runtime_case.court.role = model::CourtRole::Appellate;
    runtime_case.authored_disposition_id =
        model::WorkflowOperationId{"operation.legacy-judgment"};
    runtime_case.argument_configurations.push_back(std::move(argument));
    appellate::packs::RuntimePack runtime{};
    runtime.cases.push_back(std::move(runtime_case));
    return runtime;
}

[[nodiscard]] std::vector<storage::RevisionPin> pins() {
    return {
        storage::RevisionPin{QStringLiteral("test.pack.case"), QStringLiteral("1.0.0"),
                             QString(64, u'a')},
        storage::RevisionPin{QStringLiteral("test.pack.jurisdiction"), QStringLiteral("2.1.0"),
                             QString(64, u'b')},
    };
}

[[nodiscard]] model::CounselAnswer concession() {
    return model::CounselAnswer{
        model::CounselActKind::Concession,
        "The record contains no objection before entry of judgment.",
        "issue.preservation",
        {"authority.preservation"},
        0.95,
        8s,
    };
}

[[nodiscard]] model::CounselAnswer recordClaim() {
    return model::CounselAnswer{
        model::CounselActKind::RecordClaim,
        "The relevant exchange appears in the hearing transcript.",
        "issue.preservation",
        {"authority.preservation"},
        1.0,
        7s,
    };
}

[[nodiscard]] auto createController(const QString& database_path,
                                    Definitions supplied = definitions())
    -> std::expected<std::unique_ptr<app::OralArgumentSessionController>,
                     app::OralArgumentSessionError> {
    auto store = storage::SessionStore::open(database_path);
    if (!store) {
        return std::unexpected(app::OralArgumentSessionError{
            app::OralArgumentSessionErrorCode::SessionStoreFailure, store.error().message});
    }
    return app::OralArgumentSessionController::create(
        QString::fromLatin1(session_id), std::move(supplied.configuration),
        std::move(supplied.bench), std::move(supplied.grounding), std::move(*store),
        QString::fromLatin1(engine_revision), QStringLiteral("2026-08-11T09:00:00Z"), pins());
}

[[nodiscard]] auto reopenController(const QString& database_path,
                                    Definitions supplied = definitions(),
                                    std::vector<storage::RevisionPin> expected_pins = pins())
    -> std::expected<std::unique_ptr<app::OralArgumentSessionController>,
                     app::OralArgumentSessionError> {
    auto store = storage::SessionStore::open(database_path);
    if (!store) {
        return std::unexpected(app::OralArgumentSessionError{
            app::OralArgumentSessionErrorCode::SessionStoreFailure, store.error().message});
    }
    return app::OralArgumentSessionController::reopen(
        QString::fromLatin1(session_id), std::move(supplied.configuration),
        std::move(supplied.bench), std::move(supplied.grounding), std::move(*store),
        QString::fromLatin1(engine_revision), std::move(expected_pins));
}

[[nodiscard]] auto createCanonicalController(
    const QString& database_path,
    model::CanonicalOralArgumentDefinition definition = canonicalDefinition())
    -> std::expected<std::unique_ptr<app::OralArgumentSessionController>,
                     app::OralArgumentSessionError> {
    auto store = storage::SessionStore::open(database_path);
    if (!store) {
        return std::unexpected(app::OralArgumentSessionError{
            app::OralArgumentSessionErrorCode::SessionStoreFailure, store.error().message});
    }
    return app::OralArgumentSessionControllerTestAccess::createCanonical(
        QString::fromLatin1(canonical_session_id), std::move(definition), std::move(*store),
        QString::fromLatin1(canonical_engine_revision),
        QStringLiteral("2026-08-11T10:00:00Z"), pins());
}

[[nodiscard]] auto reopenCanonicalController(
    const QString& database_path,
    model::CanonicalOralArgumentDefinition definition = canonicalDefinition(),
    std::vector<storage::RevisionPin> expected_pins = pins())
    -> std::expected<std::unique_ptr<app::OralArgumentSessionController>,
                     app::OralArgumentSessionError> {
    auto store = storage::SessionStore::open(database_path);
    if (!store) {
        return std::unexpected(app::OralArgumentSessionError{
            app::OralArgumentSessionErrorCode::SessionStoreFailure, store.error().message});
    }
    return app::OralArgumentSessionControllerTestAccess::reopenCanonical(
        QString::fromLatin1(canonical_session_id), std::move(definition), std::move(*store),
        QString::fromLatin1(canonical_engine_revision), std::move(expected_pins));
}

[[nodiscard]] bool submitHistory(app::OralArgumentSessionController& controller) {
    const auto first = controller.submit(QStringLiteral("command.answer.concession"), concession(),
                                         QStringLiteral("2026-08-11T09:01:00Z"));
    if (!first) {
        return false;
    }
    const auto second = controller.submit(QStringLiteral("command.answer.record-claim"),
                                          recordClaim(), QStringLiteral("2026-08-11T09:02:00Z"));
    return second.has_value();
}

[[nodiscard]] bool mutateJsonRow(const QString& database_path, const QString& table,
                                 const QString& where_clause,
                                 const std::function<void(QJsonObject&)>& mutate) {
    const auto connection_name =
        QStringLiteral("oral-tamper-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    bool succeeded = false;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name);
        database.setDatabaseName(database_path);
        if (!database.open()) {
            return false;
        }
        QSqlQuery read(database);
        if (read.exec(QStringLiteral("SELECT rowid, payload_json FROM %1 WHERE %2")
                          .arg(table, where_clause)) &&
            read.next()) {
            const auto rowid = read.value(0).toLongLong();
            auto root = QJsonDocument::fromJson(read.value(1).toByteArray()).object();
            mutate(root);
            read.finish();
            QSqlQuery write(database);
            write.prepare(QStringLiteral("UPDATE %1 SET payload_json=? WHERE rowid=?").arg(table));
            write.addBindValue(QJsonDocument(root).toJson(QJsonDocument::Compact));
            write.addBindValue(rowid);
            succeeded = write.exec() && write.numRowsAffected() == 1;
        }
        database.close();
        database = QSqlDatabase{};
    }
    QSqlDatabase::removeDatabase(connection_name);
    return succeeded;
}

[[nodiscard]] bool executeSql(const QString& database_path, const QString& statement) {
    const auto connection_name =
        QStringLiteral("oral-sql-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    bool succeeded = false;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name);
        database.setDatabaseName(database_path);
        if (database.open()) {
            QSqlQuery query(database);
            succeeded = query.exec(statement);
        }
        database.close();
        database = QSqlDatabase{};
    }
    QSqlDatabase::removeDatabase(connection_name);
    return succeeded;
}

[[nodiscard]] bool tamperDatabase(const QString& database_path, const QString& kind) {
    if (kind == QStringLiteral("command-id") || kind == QStringLiteral("recorded-time") ||
        kind == QStringLiteral("authority-contract")) {
        const auto connection_name =
            QStringLiteral("oral-tamper-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
        bool succeeded = false;
        {
            auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name);
            database.setDatabaseName(database_path);
            if (!database.open()) {
                return false;
            }
            QSqlQuery query(database);
            if (kind == QStringLiteral("command-id")) {
                succeeded = query.exec(
                    QStringLiteral("UPDATE command_log SET command_id='command.answer.forged' "
                                   "WHERE expected_sequence=1"));
            } else if (kind == QStringLiteral("recorded-time")) {
                succeeded =
                    query.exec(QStringLiteral("UPDATE command_log SET recorded_at_utc="
                                              "'2026-08-11T10:01:00Z' WHERE expected_sequence=1"));
            } else {
                succeeded = query.exec(
                    QStringLiteral("UPDATE sessions SET authority_contract='canonical-v2'"));
            }
            database.close();
            database = QSqlDatabase{};
        }
        QSqlDatabase::removeDatabase(connection_name);
        return succeeded;
    }
    if (kind == QStringLiteral("event-type")) {
        const auto connection_name =
            QStringLiteral("oral-tamper-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
        bool succeeded = false;
        {
            auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name);
            database.setDatabaseName(database_path);
            if (!database.open()) {
                return false;
            }
            QSqlQuery query(database);
            succeeded = query.exec(QStringLiteral(
                "UPDATE event_log SET event_type='oral_argument.substituted.v1' WHERE sequence=2"));
            database.close();
            database = QSqlDatabase{};
        }
        QSqlDatabase::removeDatabase(connection_name);
        return succeeded;
    }
    if (kind == QStringLiteral("event-authority")) {
        const auto connection_name =
            QStringLiteral("oral-tamper-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
        bool succeeded = false;
        {
            auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name);
            database.setDatabaseName(database_path);
            if (!database.open()) {
                return false;
            }
            QSqlQuery query(database);
            succeeded = query.exec(QStringLiteral(
                "UPDATE event_log SET authority_id='oral.argument.forged.v1' WHERE sequence=2"));
            database.close();
            database = QSqlDatabase{};
        }
        QSqlDatabase::removeDatabase(connection_name);
        return succeeded;
    }
    if (kind == QStringLiteral("rendered-utterance")) {
        return mutateJsonRow(database_path, QStringLiteral("event_log"),
                             QStringLiteral("sequence=2"), [](QJsonObject& root) {
                                 auto payload = root.value(u"payload").toObject();
                                 auto bench = payload.value(u"bench").toObject();
                                 bench.insert(u"rendered_utterance",
                                              bench.value(u"rendered_utterance").toString() +
                                                  QStringLiteral(" Forged."));
                                 payload.insert(u"bench", bench);
                                 root.insert(u"payload", payload);
                             });
    }
    if (kind == QStringLiteral("event-grounding")) {
        return mutateJsonRow(database_path, QStringLiteral("event_log"),
                             QStringLiteral("sequence=2"), [](QJsonObject& root) {
                                 auto payload = root.value(u"payload").toObject();
                                 auto bench = payload.value(u"bench").toObject();
                                 auto question = bench.value(u"question").toObject();
                                 auto refs = question.value(u"grounding").toArray();
                                 auto reference = refs.at(0).toObject();
                                 reference.insert(u"id", QStringLiteral("authority.substituted"));
                                 refs.replace(0, reference);
                                 question.insert(u"grounding", refs);
                                 bench.insert(u"question", question);
                                 payload.insert(u"bench", bench);
                                 root.insert(u"payload", payload);
                             });
    }
    if (kind == QStringLiteral("answer")) {
        return mutateJsonRow(database_path, QStringLiteral("command_log"),
                             QStringLiteral("expected_sequence=1"), [](QJsonObject& root) {
                                 auto payload = root.value(u"payload").toObject();
                                 auto answer = payload.value(u"answer").toObject();
                                 answer.insert(u"text", QStringLiteral("A substituted answer."));
                                 payload.insert(u"answer", answer);
                                 root.insert(u"payload", payload);
                             });
    }
    if (kind == QStringLiteral("configuration-pin")) {
        return mutateJsonRow(database_path, QStringLiteral("command_log"),
                             QStringLiteral("expected_sequence=0"), [](QJsonObject& root) {
                                 auto payload = root.value(u"payload").toObject();
                                 auto configuration = payload.value(u"configuration").toObject();
                                 configuration.insert(u"behavior_definition_digest",
                                                      QString(64, u'e'));
                                 payload.insert(u"configuration", configuration);
                                 root.insert(u"payload", payload);
                             });
    }
    if (kind == QStringLiteral("grounding-pin")) {
        return mutateJsonRow(database_path, QStringLiteral("command_log"),
                             QStringLiteral("expected_sequence=0"), [](QJsonObject& root) {
                                 auto payload = root.value(u"payload").toObject();
                                 auto configuration = payload.value(u"configuration").toObject();
                                 configuration.insert(u"grounding_digest", QString(64, u'e'));
                                 payload.insert(u"configuration", configuration);
                                 root.insert(u"payload", payload);
                             });
    }
    return false;
}

void OralArgumentSessionResumeTest::answerJournalAndTranscriptSurviveExactReopen() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto database_path = temporary.filePath(QStringLiteral("sessions.sqlite"));

    auto controller = createController(database_path);
    QVERIFY2(controller.has_value(),
             qPrintable(controller.has_value() ? QString{} : controller.error().message));
    QCOMPARE((*controller)->state().journal.size(), std::size_t{1});
    QCOMPARE((*controller)->state().transcript.size(), std::size_t{1});
    QVERIFY(submitHistory(**controller));
    const auto expected_initial = (*controller)->initialState();
    const auto expected_state = (*controller)->state();
    const auto expected_snapshot = (*controller)->snapshot();
    QCOMPARE(expected_snapshot.authority_contract, storage::SessionAuthorityContract::LegacyV1);
    QCOMPARE(expected_state.journal.size(), std::size_t{3});
    QCOMPARE(expected_state.transcript.size(), std::size_t{5});
    QCOMPARE(expected_state.concessions.size(), std::size_t{1});
    QCOMPARE(expected_state.authored_disposition_id,
             std::string("disposition.authored.synthetic-v1"));
    controller->reset();

    const auto reopened = reopenController(database_path);
    QVERIFY2(reopened.has_value(),
             qPrintable(reopened.has_value() ? QString{} : reopened.error().message));
    QVERIFY((*reopened)->initialState() == expected_initial);
    QVERIFY((*reopened)->state() == expected_state);
    QVERIFY((*reopened)->snapshot().events == expected_snapshot.events);
    QVERIFY((*reopened)->snapshot().commands == expected_snapshot.commands);
    QCOMPARE((*reopened)->snapshot().sequence, qint64{3});
}

void OralArgumentSessionResumeTest::rejectsPersistedTampering_data() {
    QTest::addColumn<QString>("kind");

    for (const auto* kind : {"command-id", "recorded-time", "authority-contract", "event-type",
                             "event-authority", "rendered-utterance", "event-grounding", "answer",
                             "configuration-pin", "grounding-pin"}) {
        QTest::newRow(kind) << QString::fromLatin1(kind);
    }
}

void OralArgumentSessionResumeTest::rejectsPersistedTampering() {
    QFETCH(QString, kind);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto database_path = temporary.filePath(QStringLiteral("sessions.sqlite"));

    auto controller = createController(database_path);
    QVERIFY(controller.has_value());
    QVERIFY(submitHistory(**controller));
    controller->reset();
    QVERIFY(tamperDatabase(database_path, kind));

    const auto reopened = reopenController(database_path);
    QVERIFY(!reopened.has_value());
    QCOMPARE(reopened.error().code, app::OralArgumentSessionErrorCode::CorruptSession);
}

void OralArgumentSessionResumeTest::
    rejectsChangedOperativeProfileGroundingConfigurationAndRevisionPins() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto database_path = temporary.filePath(QStringLiteral("sessions.sqlite"));
    auto controller = createController(database_path);
    QVERIFY(controller.has_value());
    QVERIFY(submitHistory(**controller));
    controller->reset();

    auto changed_profile = definitions();
    changed_profile.bench.seats.front().profile.interaction.directness = 0.1;
    QVERIFY(!reopenController(database_path, std::move(changed_profile)).has_value());

    auto changed_grounding = definitions();
    changed_grounding.grounding.issues.front().question_prompts.front() =
        "A substituted grounded prompt?";
    QVERIFY(!reopenController(database_path, std::move(changed_grounding)).has_value());

    auto changed_configuration = definitions();
    changed_configuration.configuration.legal_state_digest.assign(64, 'e');
    const auto configuration_result =
        reopenController(database_path, std::move(changed_configuration));
    QVERIFY(!configuration_result.has_value());
    QCOMPARE(configuration_result.error().code, app::OralArgumentSessionErrorCode::CorruptSession);

    auto changed_pins = pins();
    changed_pins.front().digest = QString(64, u'f');
    const auto pin_result = reopenController(database_path, definitions(), std::move(changed_pins));
    QVERIFY(!pin_result.has_value());
    QCOMPARE(pin_result.error().code, app::OralArgumentSessionErrorCode::CorruptSession);
}

void OralArgumentSessionResumeTest::profileIdentityCannotAlterPinnedOutcome() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto database_path = temporary.filePath(QStringLiteral("sessions.sqlite"));
    auto controller = createController(database_path);
    QVERIFY(controller.has_value());
    QVERIFY(submitHistory(**controller));
    const auto expected_disposition = (*controller)->state().authored_disposition_id;
    const auto expected_legal_state = (*controller)->state().legal_state_digest;
    controller->reset();

    auto renamed = definitions();
    renamed.bench.seats.front().profile.id = "fictional.renamed-composite";
    renamed.bench.seats.front().profile.display_name = "Renamed Composite";
    const auto reopened = reopenController(database_path, std::move(renamed));
    QVERIFY2(reopened.has_value(),
             qPrintable(reopened.has_value() ? QString{} : reopened.error().message));
    QCOMPARE((*reopened)->state().authored_disposition_id, expected_disposition);
    QCOMPARE((*reopened)->state().legal_state_digest, expected_legal_state);
}

void OralArgumentSessionResumeTest::canonicalRowsAndDefinitionSurviveExactReopen() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto database_path = temporary.filePath(QStringLiteral("canonical.sqlite"));
    const auto definition = canonicalDefinition();
    auto controller = createCanonicalController(database_path, definition);
    QVERIFY2(controller.has_value(),
             qPrintable(controller.has_value() ? QString{} : controller.error().message));
    QVERIFY((*controller)->canonicalDefinition() != nullptr);
    QVERIFY(*(*controller)->canonicalDefinition() == definition);
    QCOMPARE((*controller)->snapshot().authority_contract,
             storage::SessionAuthorityContract::CanonicalV2);
    QCOMPARE((*controller)->snapshot().commands.size(), std::size_t{1});
    QCOMPARE((*controller)->snapshot().events.size(), std::size_t{1});
    QVERIFY((*controller)->snapshot().commands.front().payload_json.contains(
        "\"schema_version\":\"2\""));
    QVERIFY((*controller)->snapshot().events.front().payload_json.contains(
        "\"schema_version\":\"2\""));
    QCOMPARE((*controller)->snapshot().events.front().event_type,
             QStringLiteral("oral_argument.event.v2"));
    QCOMPARE((*controller)->snapshot().events.front().authority_id,
             QStringLiteral("oral.argument.engine.v2"));
    QCOMPARE((*controller)->state().authored_disposition_id,
             std::string("plan.authored-judgment"));

    const auto submitted = (*controller)->submit(
        QStringLiteral("canonical.command.answer"), canonicalAnswer(),
        QStringLiteral("2026-08-11T10:01:00Z"));
    QVERIFY2(submitted.has_value(),
             qPrintable(submitted.has_value() ? QString{} : submitted.error().message));
    QVERIFY((*controller)->snapshot().commands.back().payload_json.contains(
        "\"schema_version\":\"2\""));
    const auto expected_initial = (*controller)->initialState();
    const auto expected_state = (*controller)->state();
    const auto expected_snapshot = (*controller)->snapshot();
    controller->reset();

    const auto reopened = reopenCanonicalController(database_path, definition);
    QVERIFY2(reopened.has_value(),
             qPrintable(reopened.has_value() ? QString{} : reopened.error().message));
    QVERIFY((*reopened)->canonicalDefinition() != nullptr);
    QVERIFY(*(*reopened)->canonicalDefinition() == definition);
    QVERIFY((*reopened)->initialState() == expected_initial);
    QVERIFY((*reopened)->state() == expected_state);
    QCOMPARE((*reopened)->snapshot().session_id, expected_snapshot.session_id);
    QCOMPARE((*reopened)->snapshot().engine_revision, expected_snapshot.engine_revision);
    QCOMPARE((*reopened)->snapshot().authority_contract,
             expected_snapshot.authority_contract);
    QVERIFY((*reopened)->snapshot().pins == expected_snapshot.pins);
    QVERIFY((*reopened)->snapshot().commands == expected_snapshot.commands);
    QVERIFY((*reopened)->snapshot().events == expected_snapshot.events);
    QCOMPARE((*reopened)->snapshot().sequence, expected_snapshot.sequence);
}

void OralArgumentSessionResumeTest::canonicalReopenRejectsDowngradeAndSwappedGrounding_data() {
    QTest::addColumn<QString>("kind");
    for (const auto* kind : {"authority-contract", "event-type", "event-authority",
                             "opening-schema", "counsel-schema", "swapped-grounding"}) {
        QTest::newRow(kind) << QString::fromLatin1(kind);
    }
}

void OralArgumentSessionResumeTest::canonicalReopenRejectsDowngradeAndSwappedGrounding() {
    QFETCH(QString, kind);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto database_path = temporary.filePath(QStringLiteral("canonical-tamper.sqlite"));
    const auto definition = canonicalDefinition();
    auto controller = createCanonicalController(database_path, definition);
    QVERIFY(controller.has_value());
    const auto submitted = (*controller)->submit(
        QStringLiteral("canonical.command.answer"), canonicalAnswer(),
        QStringLiteral("2026-08-11T10:01:00Z"));
    QVERIFY(submitted.has_value());
    controller->reset();

    bool changed = false;
    if (kind == QStringLiteral("authority-contract")) {
        changed = executeSql(
            database_path,
            QStringLiteral("UPDATE sessions SET authority_contract='legacy-v1'"));
    } else if (kind == QStringLiteral("event-type")) {
        changed = executeSql(
            database_path,
            QStringLiteral("UPDATE event_log SET event_type='oral_argument.event.v1' WHERE "
                           "sequence=1"));
    } else if (kind == QStringLiteral("event-authority")) {
        changed = executeSql(
            database_path,
            QStringLiteral("UPDATE event_log SET authority_id='oral.argument.engine.v1' WHERE "
                           "sequence=1"));
    } else if (kind == QStringLiteral("opening-schema")) {
        changed = mutateJsonRow(
            database_path, QStringLiteral("command_log"),
            QStringLiteral("expected_sequence=0"), [](QJsonObject& root) {
                root.insert(u"schema_version", QStringLiteral("1"));
            });
    } else if (kind == QStringLiteral("counsel-schema")) {
        changed = mutateJsonRow(
            database_path, QStringLiteral("command_log"),
            QStringLiteral("expected_sequence=1"), [](QJsonObject& root) {
                root.insert(u"schema_version", QStringLiteral("1"));
            });
    } else {
        const model::OralArgumentEvent substitute{
            1,
            std::nullopt,
            model::BenchAct{
                model::BenchActKind::Question,
                "seat.presiding",
                model::GroundedQuestion{
                    "issue.synthetic",
                    model::AuthoredQuestionSelection{
                        "case.synthetic.question-record",
                        model::ArgumentFocusTopic::RecordSupport,
                        model::OralArgumentMode::ActualRecord,
                        definition.question_bank.questions.front().prompt,
                        definition.question_bank.questions.back().grounding},
                    std::nullopt,
                    false,
                },
                "substitute",
            },
        };
        const auto encoded = storage::encodeCanonicalOralArgumentEvent(substitute);
        QVERIFY(encoded.has_value());
        const auto substitute_root = QJsonDocument::fromJson(*encoded).object();
        const auto substitute_grounding = substitute_root.value(u"payload")
                                              .toObject()
                                              .value(u"bench")
                                              .toObject()
                                              .value(u"question")
                                              .toObject()
                                              .value(u"grounding")
                                              .toArray();
        changed = mutateJsonRow(
            database_path, QStringLiteral("event_log"), QStringLiteral("sequence=1"),
            [&](QJsonObject& root) {
                auto payload = root.value(u"payload").toObject();
                auto bench = payload.value(u"bench").toObject();
                auto question = bench.value(u"question").toObject();
                question.insert(u"grounding", substitute_grounding);
                bench.insert(u"question", question);
                payload.insert(u"bench", bench);
                root.insert(u"payload", payload);
            });
    }
    QVERIFY(changed);
    const auto reopened = reopenCanonicalController(database_path, definition);
    QVERIFY(!reopened.has_value());
    QCOMPARE(reopened.error().code, app::OralArgumentSessionErrorCode::CorruptSession);
}

void OralArgumentSessionResumeTest::counterfactualSessionCannotChangePinnedLegalStateOrDisposition() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto database_path = temporary.filePath(QStringLiteral("counterfactual.sqlite"));
    const auto definition =
        canonicalDefinition(model::OralArgumentMode::CounterfactualTraining);
    const auto expected_legal_state = definition.configuration.legal_state_digest;
    const auto expected_disposition = definition.configuration.authored_disposition_id;
    auto controller = createCanonicalController(database_path, definition);
    QVERIFY2(controller.has_value(),
             qPrintable(controller.has_value() ? QString{} : controller.error().message));
    QVERIFY((*controller)->state().canonical_contract.has_value());
    QCOMPARE((*controller)->state().canonical_contract->mode,
             model::OralArgumentMode::CounterfactualTraining);
    QCOMPARE((*controller)->state().legal_state_digest, expected_legal_state);
    QCOMPARE((*controller)->state().authored_disposition_id, expected_disposition);
    const auto initial_legal_state = (*controller)->initialState().legal_state_digest;
    const auto initial_disposition = (*controller)->initialState().authored_disposition_id;

    const auto submitted = (*controller)->submit(
        QStringLiteral("counterfactual.command.answer"), canonicalAnswer(),
        QStringLiteral("2026-08-11T10:01:00Z"));
    QVERIFY(submitted.has_value());
    QCOMPARE((*controller)->state().legal_state_digest, initial_legal_state);
    QCOMPARE((*controller)->state().authored_disposition_id, initial_disposition);
    controller->reset();

    const auto reopened = reopenCanonicalController(database_path, definition);
    QVERIFY2(reopened.has_value(),
             qPrintable(reopened.has_value() ? QString{} : reopened.error().message));
    QCOMPARE((*reopened)->state().legal_state_digest, expected_legal_state);
    QCOMPARE((*reopened)->state().authored_disposition_id, expected_disposition);
    QCOMPARE((*reopened)->canonicalDefinition()->configuration.legal_state_digest,
             expected_legal_state);
    QCOMPARE((*reopened)->canonicalDefinition()->configuration.authored_disposition_id,
             expected_disposition);
}

void OralArgumentSessionResumeTest::derivesTimingAndStructuredDispositionFromRuntimeBoundary() {
    const model::CaseId case_id{"case.synthetic"};
    const appellate::packs::RuntimeArgumentConfigId argument_id{"case.synthetic.argument"};
    const auto legal_state_digest = std::string(64, 'c');

    const auto zero_rebuttal_runtime = canonicalRuntimePack(120, 0);
    const auto zero_rebuttal =
        app::OralArgumentSessionControllerTestAccess::deriveCanonical(
            case_id, argument_id, legal_state_digest, zero_rebuttal_runtime);
    QVERIFY2(zero_rebuttal.has_value(),
             qPrintable(zero_rebuttal.has_value() ? QString{} : zero_rebuttal.error().message));
    QCOMPARE(zero_rebuttal->configuration.principal_time, 120s);
    QCOMPARE(zero_rebuttal->configuration.rebuttal_time, 0s);
    QCOMPARE(zero_rebuttal->configuration.authored_disposition_id,
             std::string("plan.structured-judgment"));
    QVERIFY(engine::initializeOralArgument(*zero_rebuttal).has_value());

    const auto reserved_runtime = canonicalRuntimePack(120, 20);
    const auto reserved = app::OralArgumentSessionControllerTestAccess::deriveCanonical(
        case_id, argument_id, legal_state_digest, reserved_runtime);
    QVERIFY(reserved.has_value());
    QCOMPARE(reserved->configuration.principal_time, 100s);
    QCOMPARE(reserved->configuration.rebuttal_time, 20s);

    const auto legacy_runtime = canonicalRuntimePack(120, 20, false);
    const auto legacy = app::OralArgumentSessionControllerTestAccess::deriveCanonical(
        case_id, argument_id, legal_state_digest, legacy_runtime);
    QVERIFY(legacy.has_value());
    QCOMPARE(legacy->configuration.authored_disposition_id,
             std::string("operation.legacy-judgment"));

    const auto empty_principal_runtime = canonicalRuntimePack(120, 120);
    const auto empty_principal =
        app::OralArgumentSessionControllerTestAccess::deriveCanonical(
            case_id, argument_id, legal_state_digest, empty_principal_runtime);
    QVERIFY(!empty_principal.has_value());
    QCOMPARE(empty_principal.error().code,
             app::OralArgumentSessionErrorCode::InvalidConfiguration);
}

void OralArgumentSessionResumeTest::rejectsCanonicalEvent65BeforeAppend() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto database_path = temporary.filePath(QStringLiteral("canonical-cap.sqlite"));
    auto long_definition = canonicalDefinition();
    long_definition.configuration.principal_time = 7'200s;
    auto state = engine::initializeOralArgument(long_definition);
    QVERIFY(state.has_value());
    auto store = storage::SessionStore::open(database_path);
    QVERIFY(store.has_value());
    const auto created = (*store)->createSession(
        QString::fromLatin1(canonical_session_id),
        QString::fromLatin1(canonical_engine_revision),
        QStringLiteral("2026-08-11T10:00:00Z"), pins(),
        storage::SessionAuthorityContract::CanonicalV2);
    QVERIFY(created.has_value());
    auto answer = canonicalAnswer();
    answer.elapsed = 1s;
    for (std::uint32_t index = 0; index < 64; ++index) {
        const auto command_id =
            index == 0 ? QString::fromLatin1(canonical_session_id) + QStringLiteral(".opening")
                       : QStringLiteral("canonical.cap-command-%1").arg(index);
        std::expected<model::OralArgumentEvent, engine::Error> event =
            index == 0 ? engine::planOpeningQuestion(long_definition, *state)
                       : engine::decideCounselAnswer(long_definition, *state, answer);
        QVERIFY(event.has_value());
        const auto command_payload =
            index == 0
                ? storage::encodeCanonicalOralArgumentOpeningCommand(
                      storage::CanonicalOralArgumentOpeningCommand{
                          QString::fromLatin1(canonical_session_id), command_id,
                          QString::fromLatin1(canonical_engine_revision),
                          QStringLiteral("2026-08-11T10:01:00Z"),
                          long_definition.question_bank.case_id,
                          long_definition.question_bank.argument_configuration_id,
                          long_definition.configuration})
                : storage::encodeCanonicalOralArgumentCounselCommand(
                      storage::OralArgumentCounselCommand{
                          QString::fromLatin1(canonical_session_id), command_id,
                          QStringLiteral("2026-08-11T10:01:00Z"), answer});
        QVERIFY(command_payload.has_value());
        const auto event_payload = storage::encodeCanonicalOralArgumentEvent(*event);
        QVERIFY(event_payload.has_value());
        const storage::CommitBatch batch{
            command_id,
            *command_payload,
            QStringLiteral("2026-08-11T10:01:00Z"),
            {storage::EventWrite{QStringLiteral("oral_argument.event.v2"), *event_payload,
                                 QStringLiteral("oral.argument.engine.v2")}},
            {},
            {},
        };
        const auto appended = (*store)->append(
            QString::fromLatin1(canonical_session_id), static_cast<qint64>(index), batch);
        QVERIFY(appended.has_value());
        const auto applied = engine::applyOralArgumentEvent(long_definition, *state, *event);
        QVERIFY(applied.has_value());
        state = *applied;
    }
    store->reset();
    auto controller = reopenCanonicalController(database_path, long_definition);
    QVERIFY2(controller.has_value(),
             qPrintable(controller.has_value() ? QString{} : controller.error().message));
    QCOMPARE((*controller)->snapshot().sequence, qint64{64});
    const auto rows_before = (*controller)->snapshot().events;
    const auto rejected = (*controller)->submit(
        QStringLiteral("canonical.cap-command-64"), answer,
        QStringLiteral("2026-08-11T10:01:00Z"));
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, app::OralArgumentSessionErrorCode::InvalidConfiguration);
    QCOMPARE((*controller)->snapshot().sequence, qint64{64});
    QVERIFY((*controller)->snapshot().events == rows_before);
}

} // namespace

QTEST_GUILESS_MAIN(OralArgumentSessionResumeTest)

#include "tst_oral_argument_session_resume.moc"
