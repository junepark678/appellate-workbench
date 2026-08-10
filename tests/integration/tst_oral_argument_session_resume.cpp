#include "oral_argument_session_controller.hpp"

#include "appellate/engine/oral_argument_engine.hpp"
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

namespace {

class OralArgumentSessionResumeTest final : public QObject {
    Q_OBJECT

  private slots:
    void answerJournalAndTranscriptSurviveExactReopen();
    void rejectsPersistedTampering_data();
    void rejectsPersistedTampering();
    void rejectsChangedOperativeProfileGroundingConfigurationAndRevisionPins();
    void profileIdentityCannotAlterPinnedOutcome();
};

namespace app = appellate::app;
namespace engine = appellate::engine;
namespace model = appellate::model;
namespace storage = appellate::storage;
using namespace std::chrono_literals;

constexpr auto session_id = "test.session.oral-argument";
constexpr auto engine_revision = "engine.oral-argument.v1";

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

[[nodiscard]] bool tamperDatabase(const QString& database_path, const QString& kind) {
    if (kind == QStringLiteral("command-id") || kind == QStringLiteral("recorded-time")) {
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
            succeeded = kind == QStringLiteral("command-id")
                            ? query.exec(QStringLiteral(
                                  "UPDATE command_log SET command_id='command.answer.forged' "
                                  "WHERE expected_sequence=1"))
                            : query.exec(QStringLiteral(
                                  "UPDATE command_log SET recorded_at_utc="
                                  "'2026-08-11T10:01:00Z' WHERE expected_sequence=1"));
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

    for (const auto* kind :
         {"command-id", "recorded-time", "event-type", "event-authority", "rendered-utterance",
          "event-grounding", "answer", "configuration-pin", "grounding-pin"}) {
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

} // namespace

QTEST_GUILESS_MAIN(OralArgumentSessionResumeTest)

#include "tst_oral_argument_session_resume.moc"
