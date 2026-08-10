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
    invalid_record_pin.bench.question->grounding = {
        {model::GroundingKind::Authority, "authority.preservation", std::nullopt}};
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
