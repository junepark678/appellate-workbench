#include "appellate/storage/oral_argument_codec.hpp"

#include <QTest>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

class OralArgumentCodecTest final : public QObject {
    Q_OBJECT

  private slots:
    void roundTripsConfigurationAnswerAndCompleteEvent();
    void preservesLiteralSchemaOneBytes();
    void roundTripsCanonicalSchemaTwoRows();
    void enforcesCanonicalUnicodeScalarBounds();
    void rejectsGenerationDowngradesAndIncompleteCanonicalGrounding();
    void rejectsDuplicateUnknownAndMissingMembers_data();
    void rejectsDuplicateUnknownAndMissingMembers();
    void rejectsNoncanonicalValuesAndUnknownEnums_data();
    void rejectsNoncanonicalValuesAndUnknownEnums();
    void rejectsContradictoryEventShapes();
};

namespace model = appellate::model;
namespace storage = appellate::storage;
using namespace std::chrono_literals;

[[nodiscard]] model::OralArgumentConfiguration configuration() {
    return model::OralArgumentConfiguration{
        1'200s,
        180s,
        0.7,
        3,
        std::string(64, 'a'),
        std::string(64, 'b'),
        std::string(64, 'c'),
        "disposition.synthetic.v1",
    };
}

[[nodiscard]] model::CounselAnswer answer() {
    return model::CounselAnswer{
        model::CounselActKind::Concession,
        "The record does not contain a contemporaneous objection.",
        "issue.preservation",
        {"record.transcript.page-47", "authority.preservation"},
        0.85,
        17s,
    };
}

[[nodiscard]] model::OralArgumentEvent sampleEvent() {
    return model::OralArgumentEvent{
        2,
        answer(),
        model::BenchAct{
            model::BenchActKind::FollowUp,
            "seat.presiding",
            model::GroundedQuestion{
                "issue.preservation",
                "Where, exactly, was the objection preserved?",
                {
                    {model::GroundingKind::RecordPage, "record.transcript.page-47", 47},
                    {model::GroundingKind::Authority, "authority.preservation", std::nullopt},
                },
                1,
                true,
            },
            "Counsel, answer this directly: where was the objection preserved?",
        },
    };
}

[[nodiscard]] model::AuthorityRef canonicalAuthority() {
    return model::AuthorityRef{
        model::AuthorityId{"authority.canonical-standard"},
        "Synthetic Authority, 100 F.4th 1",
        "2026-01-15",
        "A synthetic proposition used only for deterministic persistence tests.",
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

[[nodiscard]] model::OralArgumentEvent canonicalEvent() {
    std::vector<model::AuthoredArgumentGrounding> grounding{
        model::AuthorityArgumentGrounding{"grounding.authority", canonicalAuthority()},
        model::BriefPageArgumentGrounding{"grounding.brief", "record.entry-brief", 12,
                                          std::string(64, 'b')},
        model::RecordPageArgumentGrounding{"grounding.record", "record.anchor-hearing",
                                           "record.entry-hearing", 47, std::string(64, 'c'),
                                           "Hearing Tr. 47"},
    };
    return model::OralArgumentEvent{
        1,
        std::nullopt,
        model::BenchAct{
            model::BenchActKind::Question,
            "seat.presiding",
            model::GroundedQuestion{
                "issue.preservation",
                model::AuthoredQuestionSelection{
                    "question.preservation", model::ArgumentFocusTopic::RecordSupport,
                    model::OralArgumentMode::ActualRecord,
                    "Where, exactly, was the objection preserved?", std::move(grounding)},
                std::nullopt, false},
            "Counsel, where was the objection preserved?",
        },
    };
}

void OralArgumentCodecTest::roundTripsConfigurationAnswerAndCompleteEvent() {
    const auto encoded_configuration = storage::encodeOralArgumentConfiguration(configuration());
    QVERIFY(encoded_configuration.has_value());
    QCOMPARE(*encoded_configuration,
             storage::encodeOralArgumentConfiguration(configuration()).value());
    const auto decoded_configuration =
        storage::decodeOralArgumentConfiguration(*encoded_configuration);
    QVERIFY(decoded_configuration.has_value());
    QVERIFY(*decoded_configuration == configuration());

    const auto encoded_answer = storage::encodeCounselAnswer(answer());
    QVERIFY(encoded_answer.has_value());
    const auto decoded_answer = storage::decodeCounselAnswer(*encoded_answer);
    QVERIFY(decoded_answer.has_value());
    QVERIFY(*decoded_answer == answer());

    const storage::OralArgumentOpeningCommand opening_command{
        QStringLiteral("test.session.oral-argument"),
        QStringLiteral("test.command.opening"),
        QStringLiteral("engine.oral-argument.v1"),
        QStringLiteral("2026-08-11T09:00:00Z"),
        configuration(),
    };
    const auto encoded_opening = storage::encodeOralArgumentOpeningCommand(opening_command);
    QVERIFY(encoded_opening.has_value());
    const auto decoded_opening = storage::decodeOralArgumentOpeningCommand(*encoded_opening);
    QVERIFY(decoded_opening.has_value());
    QVERIFY(*decoded_opening == opening_command);

    const storage::OralArgumentCounselCommand counsel_command{
        QStringLiteral("test.session.oral-argument"),
        QStringLiteral("test.command.answer"),
        QStringLiteral("2026-08-11T09:01:00Z"),
        answer(),
    };
    const auto encoded_counsel = storage::encodeOralArgumentCounselCommand(counsel_command);
    QVERIFY(encoded_counsel.has_value());
    const auto decoded_counsel = storage::decodeOralArgumentCounselCommand(*encoded_counsel);
    QVERIFY(decoded_counsel.has_value());
    QVERIFY(*decoded_counsel == counsel_command);

    const auto encoded_event = storage::encodeOralArgumentEvent(sampleEvent());
    QVERIFY(encoded_event.has_value());
    const auto decoded_event = storage::decodeOralArgumentEvent(*encoded_event);
    QVERIFY(decoded_event.has_value());
    QVERIFY(*decoded_event == sampleEvent());
    QVERIFY(decoded_event->bench.question.has_value());
    QCOMPARE(decoded_event->bench.question->parent_act_sequence, std::optional<std::uint64_t>{1});
    QVERIFY(decoded_event->bench.question->recalls_concession);
    QCOMPARE(decoded_event->bench.rendered_utterance, sampleEvent().bench.rendered_utterance);

    const model::OralArgumentEvent expired{
        3,
        model::CounselAnswer{
            model::CounselActKind::Answer, "Nothing further.", "issue.preservation", {}, 1.0, 180s},
        model::BenchAct{model::BenchActKind::TimeExpired, "seat.presiding", std::nullopt,
                        "Counsel, your rebuttal time has expired."},
    };
    const auto encoded_expired = storage::encodeOralArgumentEvent(expired);
    QVERIFY(encoded_expired.has_value());
    const auto decoded_expired = storage::decodeOralArgumentEvent(*encoded_expired);
    QVERIFY(decoded_expired.has_value());
    QVERIFY(*decoded_expired == expired);
}

void OralArgumentCodecTest::preservesLiteralSchemaOneBytes() {
    const auto encoded_configuration = storage::encodeOralArgumentConfiguration(configuration());
    QVERIFY(encoded_configuration.has_value());
    const auto expected_configuration =
        QByteArray{"{\"payload\":{\"authored_disposition_id\":\"disposition.synthetic.v1\","
                   "\"behavior_definition_digest\":\""} +
        QByteArray(64, 'a') + QByteArray{"\",\"classification_confidence_threshold\":\"0.7\","
                                         "\"grounding_digest\":\""} +
        QByteArray(64, 'b') + QByteArray{"\",\"legal_state_digest\":\""} +
        QByteArray(64, 'c') +
        QByteArray{"\",\"maximum_follow_up_depth\":\"3\",\"principal_seconds\":\"1200\","
                   "\"rebuttal_seconds\":\"180\"},\"schema_version\":\"1\","
                   "\"type\":\"oral_argument.configuration\"}"};
    QCOMPARE(*encoded_configuration, expected_configuration);

    const auto encoded_event = storage::encodeOralArgumentEvent(sampleEvent());
    QVERIFY(encoded_event.has_value());
    const QByteArray expected_event =
        "{\"payload\":{\"bench\":{\"kind\":\"follow_up\",\"question\":{\"grounding\":[{"
        "\"id\":\"record.transcript.page-47\",\"kind\":\"record_page\","
        "\"page_number\":\"47\"},{\"id\":\"authority.preservation\",\"kind\":\"authority\","
        "\"page_number\":null}],\"issue_id\":\"issue.preservation\","
        "\"parent_act_sequence\":\"1\",\"prompt\":\"Where, exactly, was the objection "
        "preserved?\",\"recalls_concession\":true},\"rendered_utterance\":\"Counsel, answer "
        "this directly: where was the objection preserved?\",\"seat_id\":\"seat.presiding\"},"
        "\"counsel\":{\"cited_grounding_ids\":[\"record.transcript.page-47\","
        "\"authority.preservation\"],\"classification_confidence\":\"0.85\","
        "\"elapsed_seconds\":\"17\",\"issue_id\":\"issue.preservation\","
        "\"kind\":\"concession\",\"text\":\"The record does not contain a contemporaneous "
        "objection.\"},\"sequence\":\"2\"},\"schema_version\":\"1\","
        "\"type\":\"oral_argument.event\"}";
    QCOMPARE(*encoded_event, expected_event);
}

void OralArgumentCodecTest::roundTripsCanonicalSchemaTwoRows() {
    const storage::CanonicalOralArgumentOpeningCommand opening{
        QStringLiteral("test.session.canonical-argument"),
        QStringLiteral("test.session.canonical-argument.opening"),
        QStringLiteral("engine.oral-argument.v2"),
        QStringLiteral("2026-08-11T09:00:00Z"),
        model::CaseId{"case.synthetic"},
        "case.synthetic.argument",
        configuration(),
    };
    const auto encoded_opening =
        storage::encodeCanonicalOralArgumentOpeningCommand(opening);
    QVERIFY(encoded_opening.has_value());
    QVERIFY(encoded_opening->contains("\"schema_version\":\"2\""));
    const auto decoded_opening =
        storage::decodeCanonicalOralArgumentOpeningCommand(*encoded_opening);
    QVERIFY(decoded_opening.has_value());
    QVERIFY(*decoded_opening == opening);

    const storage::OralArgumentCounselCommand command{
        QStringLiteral("test.session.canonical-argument"),
        QStringLiteral("test.command.answer"),
        QStringLiteral("2026-08-11T09:01:00Z"),
        answer(),
    };
    const auto encoded_command =
        storage::encodeCanonicalOralArgumentCounselCommand(command);
    QVERIFY(encoded_command.has_value());
    QVERIFY(encoded_command->contains("\"schema_version\":\"2\""));
    const auto decoded_command =
        storage::decodeCanonicalOralArgumentCounselCommand(*encoded_command);
    QVERIFY(decoded_command.has_value());
    QVERIFY(*decoded_command == command);

    const auto event = canonicalEvent();
    const auto encoded_event = storage::encodeCanonicalOralArgumentEvent(event);
    QVERIFY(encoded_event.has_value());
    QVERIFY(encoded_event->contains("\"schema_version\":\"2\""));
    QVERIFY(encoded_event->contains("\"grounding_id\":\"grounding.authority\""));
    QVERIFY(encoded_event->contains("\"source_url\":"));
    QVERIFY(encoded_event->contains("\"asset_sha256\":"));
    const auto decoded_event = storage::decodeCanonicalOralArgumentEvent(*encoded_event);
    QVERIFY(decoded_event.has_value());
    QVERIFY(*decoded_event == event);

    const model::OralArgumentEvent expired{
        2,
        answer(),
        model::BenchAct{model::BenchActKind::TimeExpired, "seat.presiding", std::nullopt,
                        "Counsel, your time has expired."},
    };
    const auto encoded_expired = storage::encodeCanonicalOralArgumentEvent(expired);
    QVERIFY(encoded_expired.has_value());
    const auto decoded_expired =
        storage::decodeCanonicalOralArgumentEvent(*encoded_expired);
    QVERIFY(decoded_expired.has_value());
    QVERIFY(*decoded_expired == expired);
}

void OralArgumentCodecTest::enforcesCanonicalUnicodeScalarBounds() {
    const auto unicode = [](qsizetype count) {
        return QString(count, QChar{0xD55C}).toUtf8().toStdString();
    };

    auto boundary = canonicalEvent();
    auto& boundary_selection = std::get<model::AuthoredQuestionSelection>(
        boundary.bench.question->selection);
    boundary_selection.prompt = unicode(512);
    auto& boundary_authority =
        std::get<model::AuthorityArgumentGrounding>(boundary_selection.grounding.at(0));
    boundary_authority.authority.provenance->locator = unicode(1'024);
    auto& boundary_record =
        std::get<model::RecordPageArgumentGrounding>(boundary_selection.grounding.at(2));
    boundary_record.citation_label = unicode(120);
    const auto encoded = storage::encodeCanonicalOralArgumentEvent(boundary);
    QVERIFY2(encoded.has_value(), encoded ? "" : qPrintable(encoded.error().message));
    const auto decoded = storage::decodeCanonicalOralArgumentEvent(*encoded);
    QVERIFY2(decoded.has_value(), decoded ? "" : qPrintable(decoded.error().message));
    QVERIFY(*decoded == boundary);

    auto overlong_prompt = boundary;
    std::get<model::AuthoredQuestionSelection>(overlong_prompt.bench.question->selection).prompt =
        unicode(513);
    QVERIFY(!storage::encodeCanonicalOralArgumentEvent(overlong_prompt).has_value());

    auto overlong_locator = boundary;
    std::get<model::AuthorityArgumentGrounding>(
        std::get<model::AuthoredQuestionSelection>(overlong_locator.bench.question->selection)
            .grounding.at(0))
        .authority.provenance->locator = unicode(1'025);
    QVERIFY(!storage::encodeCanonicalOralArgumentEvent(overlong_locator).has_value());

    auto overlong_label = boundary;
    std::get<model::RecordPageArgumentGrounding>(
        std::get<model::AuthoredQuestionSelection>(overlong_label.bench.question->selection)
            .grounding.at(2))
        .citation_label = unicode(121);
    QVERIFY(!storage::encodeCanonicalOralArgumentEvent(overlong_label).has_value());

    auto controlled_label = boundary;
    std::get<model::RecordPageArgumentGrounding>(
        std::get<model::AuthoredQuestionSelection>(controlled_label.bench.question->selection)
            .grounding.at(2))
        .citation_label = "Hearing\nTr. 47";
    QVERIFY(!storage::encodeCanonicalOralArgumentEvent(controlled_label).has_value());

    auto padded_prompt = boundary;
    std::get<model::AuthoredQuestionSelection>(padded_prompt.bench.question->selection).prompt =
        " padded prompt ";
    QVERIFY(!storage::encodeCanonicalOralArgumentEvent(padded_prompt).has_value());
}

void OralArgumentCodecTest::rejectsGenerationDowngradesAndIncompleteCanonicalGrounding() {
    const auto legacy_event = storage::encodeOralArgumentEvent(sampleEvent()).value();
    const auto canonical_event =
        storage::encodeCanonicalOralArgumentEvent(canonicalEvent()).value();
    QVERIFY(!storage::decodeCanonicalOralArgumentEvent(legacy_event).has_value());
    QVERIFY(!storage::decodeOralArgumentEvent(canonical_event).has_value());
    QVERIFY(!storage::encodeCanonicalOralArgumentEvent(sampleEvent()).has_value());
    QVERIFY(!storage::encodeOralArgumentEvent(canonicalEvent()).has_value());

    const storage::OralArgumentCounselCommand command{
        QStringLiteral("test.session.canonical-argument"),
        QStringLiteral("test.command.answer"),
        QStringLiteral("2026-08-11T09:01:00Z"),
        answer(),
    };
    const auto legacy_command = storage::encodeOralArgumentCounselCommand(command).value();
    const auto canonical_command =
        storage::encodeCanonicalOralArgumentCounselCommand(command).value();
    QVERIFY(!storage::decodeCanonicalOralArgumentCounselCommand(legacy_command).has_value());
    QVERIFY(!storage::decodeOralArgumentCounselCommand(canonical_command).has_value());

    auto incomplete = canonicalEvent();
    auto& selection = std::get<model::AuthoredQuestionSelection>(
        incomplete.bench.question->selection);
    std::get<model::AuthorityArgumentGrounding>(selection.grounding.front())
        .authority.provenance.reset();
    QVERIFY(!storage::encodeCanonicalOralArgumentEvent(incomplete).has_value());

    auto malformed = canonical_event;
    malformed.replace(QByteArray("\"schema_version\":\"2\""),
                      QByteArray("\"schema_version\":\"1\""));
    QVERIFY(!storage::decodeCanonicalOralArgumentEvent(malformed).has_value());
    QVERIFY(!storage::decodeOralArgumentEvent(malformed).has_value());
}

void OralArgumentCodecTest::rejectsDuplicateUnknownAndMissingMembers_data() {
    QTest::addColumn<QByteArray>("payload");
    QTest::addColumn<storage::OralArgumentCodecErrorCode>("code");

    const auto encoded = storage::encodeOralArgumentEvent(sampleEvent()).value();

    auto duplicate = encoded;
    duplicate.replace(QByteArray("{\"payload\":"),
                      QByteArray("{\"type\":\"oral_argument.event\",\"payload\":"));
    QTest::newRow("duplicate-root-member")
        << duplicate << storage::OralArgumentCodecErrorCode::DuplicateMember;

    auto unknown = encoded;
    unknown.replace(QByteArray("\"sequence\":\"2\""),
                    QByteArray("\"sequence\":\"2\",\"unexpected\":true"));
    QTest::newRow("unknown-event-member")
        << unknown << storage::OralArgumentCodecErrorCode::UnexpectedField;

    auto missing = encoded;
    missing.replace(QByteArray(",\"recalls_concession\":true"), QByteArray{});
    QTest::newRow("missing-question-member")
        << missing << storage::OralArgumentCodecErrorCode::MissingField;
}

void OralArgumentCodecTest::rejectsDuplicateUnknownAndMissingMembers() {
    QFETCH(QByteArray, payload);
    QFETCH(storage::OralArgumentCodecErrorCode, code);

    const auto decoded = storage::decodeOralArgumentEvent(payload);
    QVERIFY(!decoded.has_value());
    QCOMPARE(decoded.error().code, code);
}

void OralArgumentCodecTest::rejectsNoncanonicalValuesAndUnknownEnums_data() {
    QTest::addColumn<QByteArray>("payload");

    const auto encoded_event = storage::encodeOralArgumentEvent(sampleEvent()).value();
    auto leading_zero = encoded_event;
    leading_zero.replace(QByteArray("\"sequence\":\"2\""), QByteArray("\"sequence\":\"02\""));
    QTest::newRow("leading-zero-sequence") << leading_zero;

    auto unknown_kind = encoded_event;
    unknown_kind.replace(QByteArray("\"kind\":\"follow_up\""),
                         QByteArray("\"kind\":\"cross_examination\""));
    QTest::newRow("unknown-bench-enum") << unknown_kind;

    auto bad_page = encoded_event;
    bad_page.replace(QByteArray("\"page_number\":\"47\""), QByteArray("\"page_number\":\"0\""));
    QTest::newRow("zero-record-page") << bad_page;

    auto noncanonical_probability = storage::encodeCounselAnswer(answer()).value();
    noncanonical_probability.replace(QByteArray("\"classification_confidence\":\"0.85\""),
                                     QByteArray("\"classification_confidence\":\"0.850\""));
    QTest::newRow("noncanonical-probability") << noncanonical_probability;

    auto unknown_counsel = storage::encodeCounselAnswer(answer()).value();
    unknown_counsel.replace(QByteArray("\"kind\":\"concession\""),
                            QByteArray("\"kind\":\"speech\""));
    QTest::newRow("unknown-counsel-enum") << unknown_counsel;
}

void OralArgumentCodecTest::rejectsNoncanonicalValuesAndUnknownEnums() {
    QFETCH(QByteArray, payload);

    const auto as_event = storage::decodeOralArgumentEvent(payload);
    const auto as_answer = storage::decodeCounselAnswer(payload);
    QVERIFY(!as_event.has_value());
    QVERIFY(!as_answer.has_value());
}

void OralArgumentCodecTest::rejectsContradictoryEventShapes() {
    auto missing_question = sampleEvent();
    missing_question.bench.question.reset();
    QVERIFY(!storage::encodeOralArgumentEvent(missing_question).has_value());

    auto non_question_with_grounding = sampleEvent();
    non_question_with_grounding.bench.kind = model::BenchActKind::TimeExpired;
    QVERIFY(!storage::encodeOralArgumentEvent(non_question_with_grounding).has_value());

    auto impossible_parent = sampleEvent();
    impossible_parent.bench.question->parent_act_sequence = 2;
    QVERIFY(!storage::encodeOralArgumentEvent(impossible_parent).has_value());

    auto invalid_record_pin = sampleEvent();
    invalid_record_pin.bench.kind = model::BenchActKind::RecordPinDemand;
    std::get<model::LegacyQuestionSelection>(invalid_record_pin.bench.question->selection)
        .grounding = {{model::GroundingKind::Authority, "authority.preservation", std::nullopt}};
    QVERIFY(!storage::encodeOralArgumentEvent(invalid_record_pin).has_value());

    auto invalid_configuration = configuration();
    invalid_configuration.legal_state_digest = "not-a-digest";
    QVERIFY(!storage::encodeOralArgumentConfiguration(invalid_configuration).has_value());

    const storage::OralArgumentCounselCommand invalid_time{
        QStringLiteral("test.session.oral-argument"),
        QStringLiteral("test.command.answer"),
        QStringLiteral("2026-08-11T09:01:00+00:00"),
        answer(),
    };
    QVERIFY(!storage::encodeOralArgumentCounselCommand(invalid_time).has_value());
}

} // namespace

QTEST_APPLESS_MAIN(OralArgumentCodecTest)

#include "tst_oral_argument_codec.moc"
