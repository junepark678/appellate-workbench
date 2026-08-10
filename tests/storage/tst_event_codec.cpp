#include "appellate/storage/event_codec.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using appellate::model::ActorId;
using appellate::model::AuthorityBasis;
using appellate::model::AuthorityId;
using appellate::model::AuthorityRef;
using appellate::model::FilingAccepted;
using appellate::model::FilingDeficiencyIssued;
using appellate::model::FilingFieldId;
using appellate::model::FilingRejected;
using appellate::model::FilingRejectionReason;
using appellate::model::FilingTypeId;
using appellate::model::LegalDate;
using appellate::model::LegalEvent;
using appellate::model::LegalTime;
using appellate::model::SessionId;
using appellate::model::SubmissionId;
using appellate::storage::EventCodecErrorCode;

[[nodiscard]] LegalDate date(int year, unsigned month, unsigned day) {
    return LegalDate{std::chrono::year{year} / std::chrono::month{month} / std::chrono::day{day}};
}

[[nodiscard]] AuthorityRef authorityRef(std::string id, std::string proposition) {
    return AuthorityRef{AuthorityId{std::move(id)}, "Fed. R. App. P. test", "2026-08-11",
                        std::move(proposition)};
}

[[nodiscard]] AuthorityBasis authority() {
    return AuthorityBasis{
        authorityRef("test.authority.primary", "Primary test proposition"),
        {authorityRef("test.authority.supporting", "Supporting test proposition")},
    };
}

[[nodiscard]] LegalTime submittedAt() {
    constexpr std::int64_t beyond_exact_json_number = 9'007'199'254'740'993;
    return LegalTime{std::chrono::sys_seconds{std::chrono::seconds{beyond_exact_json_number}},
                     date(2026, 8, 14)};
}

[[nodiscard]] FilingAccepted accepted(std::uint64_t sequence = 1) {
    return FilingAccepted{
        SessionId{"test.session.one"},
        SubmissionId{"test.submission.one"},
        ActorId{"test.actor.appellant"},
        FilingTypeId{"test.filing.notice"},
        submittedAt(),
        std::string(64, 'a'),
        sequence,
        authority(),
    };
}

[[nodiscard]] FilingDeficiencyIssued deficiency() {
    return FilingDeficiencyIssued{
        SessionId{"test.session.one"},
        SubmissionId{"test.submission.deficient"},
        ActorId{"test.actor.appellant"},
        FilingTypeId{"test.filing.notice"},
        submittedAt(),
        std::string(64, 'b'),
        {FilingFieldId{"test.field.certificate"}, FilingFieldId{"test.field.signature"}},
        date(2026, 8, 18),
        2,
        authority(),
    };
}

[[nodiscard]] FilingRejected rejected(FilingRejectionReason reason) {
    return FilingRejected{
        SessionId{"test.session.one"},
        SubmissionId{"test.submission.rejected"},
        ActorId{"test.actor.appellee"},
        FilingTypeId{"test.filing.notice"},
        submittedAt(),
        reason,
        authority(),
    };
}

[[nodiscard]] QJsonObject encodedObject(const LegalEvent& event) {
    const auto encoded = appellate::storage::encodeEvent(event);
    if (!encoded) {
        QTest::qFail(qPrintable(encoded.error().message), __FILE__, __LINE__);
        return {};
    }
    return QJsonDocument::fromJson(*encoded).object();
}

[[nodiscard]] QByteArray compact(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

class EventCodecTest final : public QObject {
    Q_OBJECT

  private slots:
    void roundTripsEveryEventVariant();
    void preservesIntegersBeyondJsonNumberPrecision();
    void rejectsUnknownTypeAndVersion();
    void rejectsExtraAndMissingKeysAtEveryLevel();
    void rejectsMalformedDateTimeAndInteger();
    void rejectsIncompleteAuthority();
    void enforcesPayloadAndCollectionBounds();
};

void EventCodecTest::roundTripsEveryEventVariant() {
    std::vector<LegalEvent> events{accepted(), deficiency()};
    for (const auto reason :
         {FilingRejectionReason::UnauthorizedActor, FilingRejectionReason::WrongFilingType,
          FilingRejectionReason::CureDeadlineExpired, FilingRejectionReason::DeficiencyNotCured,
          FilingRejectionReason::ProceedingAlreadyDocketed}) {
        events.emplace_back(rejected(reason));
    }

    for (const auto& event : events) {
        const auto first = appellate::storage::encodeEvent(event);
        const auto second = appellate::storage::encodeEvent(event);
        QVERIFY(first.has_value());
        QVERIFY(second.has_value());
        QCOMPARE(*first, *second);

        const auto decoded = appellate::storage::decodeEvent(*first);
        QVERIFY(decoded.has_value());
        QVERIFY(*decoded == event);
        QVERIFY(!appellate::storage::eventType(event).isEmpty());
        QCOMPARE(appellate::storage::primaryAuthorityId(event),
                 QStringLiteral("test.authority.primary"));
    }
}

void EventCodecTest::preservesIntegersBeyondJsonNumberPrecision() {
    const LegalEvent event = accepted(std::numeric_limits<std::uint64_t>::max());
    const auto encoded = appellate::storage::encodeEvent(event);
    QVERIFY(encoded.has_value());
    QVERIFY(encoded->contains(QByteArrayLiteral("\"18446744073709551615\"")));
    QVERIFY(encoded->contains(QByteArrayLiteral("\"9007199254740993\"")));

    const auto decoded = appellate::storage::decodeEvent(*encoded);
    QVERIFY(decoded.has_value());
    QVERIFY(*decoded == event);
}

void EventCodecTest::rejectsUnknownTypeAndVersion() {
    auto envelope = encodedObject(LegalEvent{accepted()});
    envelope.insert(QStringLiteral("event_type"), QStringLiteral("filing.future"));
    auto decoded = appellate::storage::decodeEvent(compact(envelope));
    QVERIFY(!decoded.has_value());
    QCOMPARE(decoded.error().code, EventCodecErrorCode::UnknownEventType);

    envelope = encodedObject(LegalEvent{accepted()});
    envelope.insert(QStringLiteral("schema_version"), 2);
    decoded = appellate::storage::decodeEvent(compact(envelope));
    QVERIFY(!decoded.has_value());
    QCOMPARE(decoded.error().code, EventCodecErrorCode::UnsupportedVersion);
}

void EventCodecTest::rejectsExtraAndMissingKeysAtEveryLevel() {
    auto envelope = encodedObject(LegalEvent{accepted()});
    envelope.insert(QStringLiteral("extra"), true);
    auto decoded = appellate::storage::decodeEvent(compact(envelope));
    QVERIFY(!decoded.has_value());
    QCOMPARE(decoded.error().code, EventCodecErrorCode::UnexpectedField);

    envelope = encodedObject(LegalEvent{accepted()});
    auto payload = envelope.value(QStringLiteral("payload")).toObject();
    payload.remove(QStringLiteral("actor_id"));
    envelope.insert(QStringLiteral("payload"), payload);
    decoded = appellate::storage::decodeEvent(compact(envelope));
    QVERIFY(!decoded.has_value());
    QCOMPARE(decoded.error().code, EventCodecErrorCode::MissingField);

    envelope = encodedObject(LegalEvent{accepted()});
    payload = envelope.value(QStringLiteral("payload")).toObject();
    auto time = payload.value(QStringLiteral("submitted_at")).toObject();
    time.insert(QStringLiteral("timezone_guess"), QStringLiteral("UTC"));
    payload.insert(QStringLiteral("submitted_at"), time);
    envelope.insert(QStringLiteral("payload"), payload);
    decoded = appellate::storage::decodeEvent(compact(envelope));
    QVERIFY(!decoded.has_value());
    QCOMPARE(decoded.error().code, EventCodecErrorCode::UnexpectedField);

    envelope = encodedObject(LegalEvent{accepted()});
    payload = envelope.value(QStringLiteral("payload")).toObject();
    auto authority_object = payload.value(QStringLiteral("authority")).toObject();
    auto primary = authority_object.value(QStringLiteral("primary")).toObject();
    primary.insert(QStringLiteral("extra"), true);
    authority_object.insert(QStringLiteral("primary"), primary);
    payload.insert(QStringLiteral("authority"), authority_object);
    envelope.insert(QStringLiteral("payload"), payload);
    decoded = appellate::storage::decodeEvent(compact(envelope));
    QVERIFY(!decoded.has_value());
    QCOMPARE(decoded.error().code, EventCodecErrorCode::UnexpectedField);
}

void EventCodecTest::rejectsMalformedDateTimeAndInteger() {
    auto envelope = encodedObject(LegalEvent{accepted()});
    auto payload = envelope.value(QStringLiteral("payload")).toObject();
    payload.insert(QStringLiteral("docket_sequence"), 9'007'199'254'740'992.0);
    envelope.insert(QStringLiteral("payload"), payload);
    auto decoded = appellate::storage::decodeEvent(compact(envelope));
    QVERIFY(!decoded.has_value());
    QCOMPARE(decoded.error().code, EventCodecErrorCode::InvalidField);

    envelope = encodedObject(LegalEvent{accepted()});
    payload = envelope.value(QStringLiteral("payload")).toObject();
    payload.insert(QStringLiteral("docket_sequence"), QStringLiteral("01"));
    envelope.insert(QStringLiteral("payload"), payload);
    decoded = appellate::storage::decodeEvent(compact(envelope));
    QVERIFY(!decoded.has_value());
    QCOMPARE(decoded.error().code, EventCodecErrorCode::InvalidField);

    envelope = encodedObject(LegalEvent{accepted()});
    payload = envelope.value(QStringLiteral("payload")).toObject();
    auto time = payload.value(QStringLiteral("submitted_at")).toObject();
    time.insert(QStringLiteral("court_date"), QStringLiteral("2026-02-30"));
    time.insert(QStringLiteral("instant_unix_seconds"), QStringLiteral("9223372036854775808"));
    payload.insert(QStringLiteral("submitted_at"), time);
    envelope.insert(QStringLiteral("payload"), payload);
    decoded = appellate::storage::decodeEvent(compact(envelope));
    QVERIFY(!decoded.has_value());
    QVERIFY(decoded.error().code == EventCodecErrorCode::InvalidField ||
            decoded.error().code == EventCodecErrorCode::OutOfRange);
}

void EventCodecTest::rejectsIncompleteAuthority() {
    auto event = accepted();
    event.authority.primary.proposition.clear();
    const auto encoded = appellate::storage::encodeEvent(LegalEvent{event});
    QVERIFY(!encoded.has_value());
    QCOMPARE(encoded.error().code, EventCodecErrorCode::IncompleteAuthority);

    auto envelope = encodedObject(LegalEvent{accepted()});
    auto payload = envelope.value(QStringLiteral("payload")).toObject();
    auto authority_object = payload.value(QStringLiteral("authority")).toObject();
    auto primary = authority_object.value(QStringLiteral("primary")).toObject();
    primary.insert(QStringLiteral("citation"), QString{});
    authority_object.insert(QStringLiteral("primary"), primary);
    payload.insert(QStringLiteral("authority"), authority_object);
    envelope.insert(QStringLiteral("payload"), payload);
    const auto decoded = appellate::storage::decodeEvent(compact(envelope));
    QVERIFY(!decoded.has_value());
    QCOMPARE(decoded.error().code, EventCodecErrorCode::IncompleteAuthority);
}

void EventCodecTest::enforcesPayloadAndCollectionBounds() {
    const QByteArray oversized(1024 * 1024 + 1, 'x');
    const auto decoded = appellate::storage::decodeEvent(oversized);
    QVERIFY(!decoded.has_value());
    QCOMPARE(decoded.error().code, EventCodecErrorCode::PayloadTooLarge);

    auto event = deficiency();
    event.missing_fields.clear();
    for (int index = 0; index < 257; ++index) {
        event.missing_fields.push_back(FilingFieldId{"test.field." + std::to_string(index)});
    }
    const auto encoded = appellate::storage::encodeEvent(LegalEvent{event});
    QVERIFY(!encoded.has_value());
    QCOMPARE(encoded.error().code, EventCodecErrorCode::InvalidField);
}

} // namespace

QTEST_GUILESS_MAIN(EventCodecTest)

#include "tst_event_codec.moc"
