#include "appellate/storage/event_codec.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using appellate::model::ActorId;
using appellate::model::AuthorityBasis;
using appellate::model::AuthorityId;
using appellate::model::AuthorityProvenance;
using appellate::model::AuthorityRef;
using appellate::model::AuthorityType;
using appellate::model::FilingAccepted;
using appellate::model::FilingDeficiencyIssued;
using appellate::model::FilingFieldId;
using appellate::model::FilingRejected;
using appellate::model::FilingRejectionReason;
using appellate::model::FilingTypeId;
using appellate::model::LegalDate;
using appellate::model::LegalEvent;
using appellate::model::LegalTime;
using appellate::model::PrecedentialStatus;
using appellate::model::SessionId;
using appellate::model::SubmissionId;
using appellate::storage::EventCodecErrorCode;

[[nodiscard]] LegalDate date(int year, unsigned month, unsigned day) {
    return LegalDate{std::chrono::year{year} / std::chrono::month{month} / std::chrono::day{day}};
}

[[nodiscard]] AuthorityProvenance
provenance(AuthorityType type = AuthorityType::Rule,
           PrecedentialStatus status = PrecedentialStatus::NotApplicable) {
    return AuthorityProvenance{type,
                               "us.federal",
                               "us.ca4",
                               status,
                               true,
                               "2026-08-11",
                               "Fed. R. App. P. test",
                               "https://www.ca4.uscourts.gov/rules/Rule03.html"};
}

[[nodiscard]] AuthorityRef authorityRef(std::string id, std::string proposition,
                                        bool with_provenance = false) {
    return AuthorityRef{AuthorityId{std::move(id)}, "Fed. R. App. P. test", "2026-08-11",
                        std::move(proposition),
                        with_provenance ? std::optional{provenance()} : std::nullopt};
}

[[nodiscard]] AuthorityBasis authority(bool with_provenance = false) {
    return AuthorityBasis{
        authorityRef("test.authority.primary", "Primary test proposition", with_provenance),
        {authorityRef("test.authority.supporting", "Supporting test proposition", with_provenance)},
    };
}

[[nodiscard]] LegalTime submittedAt() {
    constexpr std::int64_t beyond_exact_json_number = 9'007'199'254'740'993;
    return LegalTime{std::chrono::sys_seconds{std::chrono::seconds{beyond_exact_json_number}},
                     date(2026, 8, 14)};
}

[[nodiscard]] FilingAccepted accepted(std::uint64_t sequence = 1, bool with_provenance = false) {
    return FilingAccepted{
        SessionId{"test.session.one"},
        SubmissionId{"test.submission.one"},
        ActorId{"test.actor.appellant"},
        FilingTypeId{"test.filing.notice"},
        submittedAt(),
        std::string(64, 'a'),
        sequence,
        authority(with_provenance),
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
    void preservesLegacySchemaOneBytes();
    void roundTripsCompleteProvenanceInSchemaTwo();
    void rejectsAuthoritySchemaDowngradesAndMixedForms();
    void rejectsMutatedProvenance();
    void preservesIntegersBeyondJsonNumberPrecision();
    void rejectsUnknownTypeAndVersion();
    void rejectsDuplicateSemanticMembers();
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

void EventCodecTest::preservesLegacySchemaOneBytes() {
    const LegalEvent event = accepted();
    const auto encoded = appellate::storage::encodeEvent(event);
    QVERIFY(encoded.has_value());
    const auto envelope = QJsonDocument::fromJson(*encoded).object();
    QCOMPARE(envelope.value(QStringLiteral("schema_version")).toInt(), 1);
    const auto primary = envelope.value(QStringLiteral("payload"))
                             .toObject()
                             .value(QStringLiteral("authority"))
                             .toObject()
                             .value(QStringLiteral("primary"))
                             .toObject();
    QVERIFY(!primary.contains(QStringLiteral("provenance")));
    const auto decoded = appellate::storage::decodeEvent(*encoded);
    QVERIFY(decoded.has_value());
    const auto reencoded = appellate::storage::encodeEvent(*decoded);
    QVERIFY(reencoded.has_value());
    QCOMPARE(*reencoded, *encoded);
}

void EventCodecTest::roundTripsCompleteProvenanceInSchemaTwo() {
    const std::vector types{AuthorityType::Constitution,
                            AuthorityType::Statute,
                            AuthorityType::Rule,
                            AuthorityType::Regulation,
                            AuthorityType::Case,
                            AuthorityType::Order,
                            AuthorityType::AdministrativeDecision,
                            AuthorityType::Other};
    const std::vector statuses{PrecedentialStatus::NotApplicable, PrecedentialStatus::Precedential,
                               PrecedentialStatus::Nonprecedential};
    for (const auto type : types) {
        for (const auto status : statuses) {
            auto value = accepted(1, true);
            value.authority.primary.provenance = provenance(type, status);
            value.authority.supporting.front().provenance = provenance(type, status);
            const LegalEvent event = value;
            const auto encoded = appellate::storage::encodeEvent(event);
            QVERIFY(encoded.has_value());
            const auto envelope = QJsonDocument::fromJson(*encoded).object();
            QCOMPARE(envelope.value(QStringLiteral("schema_version")).toInt(), 2);
            const auto decoded = appellate::storage::decodeEvent(*encoded);
            QVERIFY(decoded.has_value());
            QVERIFY(*decoded == event);
            const auto reencoded = appellate::storage::encodeEvent(*decoded);
            QVERIFY(reencoded.has_value());
            QCOMPARE(*reencoded, *encoded);
        }
    }

    auto unicode_text = std::string{};
    unicode_text.reserve(2000 * std::string("한").size());
    for (int index = 0; index < 2000; ++index) {
        unicode_text += "한";
    }
    auto unicode = accepted(1, true);
    unicode.authority.primary.citation = unicode_text;
    unicode.authority.primary.proposition = unicode_text;
    unicode.authority.primary.provenance->locator = unicode_text;
    auto encoded = appellate::storage::encodeEvent(LegalEvent{unicode});
    QVERIFY(encoded.has_value());
    auto decoded = appellate::storage::decodeEvent(*encoded);
    QVERIFY(decoded.has_value());
    QVERIFY(*decoded == LegalEvent{unicode});

    unicode.authority.primary.provenance->locator = "invalid\nlocator";
    encoded = appellate::storage::encodeEvent(LegalEvent{unicode});
    QVERIFY(!encoded.has_value());
    QCOMPARE(encoded.error().code, EventCodecErrorCode::IncompleteAuthority);
}

void EventCodecTest::rejectsAuthoritySchemaDowngradesAndMixedForms() {
    auto mixed = accepted(1, true);
    mixed.authority.supporting.front().provenance.reset();
    auto encoded = appellate::storage::encodeEvent(LegalEvent{mixed});
    QVERIFY(!encoded.has_value());
    QCOMPARE(encoded.error().code, EventCodecErrorCode::IncompleteAuthority);

    auto envelope = encodedObject(LegalEvent{accepted(1, true)});
    envelope.insert(QStringLiteral("schema_version"), 1);
    auto decoded = appellate::storage::decodeEvent(compact(envelope));
    QVERIFY(!decoded.has_value());
    QCOMPARE(decoded.error().code, EventCodecErrorCode::IncompleteAuthority);

    envelope = encodedObject(LegalEvent{accepted()});
    envelope.insert(QStringLiteral("schema_version"), 2);
    decoded = appellate::storage::decodeEvent(compact(envelope));
    QVERIFY(!decoded.has_value());
    QCOMPARE(decoded.error().code, EventCodecErrorCode::IncompleteAuthority);

    envelope = encodedObject(LegalEvent{accepted(1, true)});
    auto payload = envelope.value(QStringLiteral("payload")).toObject();
    auto authority_object = payload.value(QStringLiteral("authority")).toObject();
    auto supporting = authority_object.value(QStringLiteral("supporting")).toArray();
    auto supporting_ref = supporting.first().toObject();
    supporting_ref.remove(QStringLiteral("provenance"));
    supporting.replace(0, supporting_ref);
    authority_object.insert(QStringLiteral("supporting"), supporting);
    payload.insert(QStringLiteral("authority"), authority_object);
    envelope.insert(QStringLiteral("payload"), payload);
    decoded = appellate::storage::decodeEvent(compact(envelope));
    QVERIFY(!decoded.has_value());
    QCOMPARE(decoded.error().code, EventCodecErrorCode::IncompleteAuthority);

    envelope = encodedObject(LegalEvent{accepted(1, true)});
    payload = envelope.value(QStringLiteral("payload")).toObject();
    authority_object = payload.value(QStringLiteral("authority")).toObject();
    auto primary = authority_object.value(QStringLiteral("primary")).toObject();
    auto source = primary.value(QStringLiteral("provenance")).toObject();
    source.insert(QStringLiteral("checked_on"), QStringLiteral("2025-08-11"));
    primary.insert(QStringLiteral("provenance"), source);
    authority_object.insert(QStringLiteral("primary"), primary);
    payload.insert(QStringLiteral("authority"), authority_object);
    envelope.insert(QStringLiteral("payload"), payload);
    decoded = appellate::storage::decodeEvent(compact(envelope));
    QVERIFY(!decoded.has_value());
    QCOMPARE(decoded.error().code, EventCodecErrorCode::IncompleteAuthority);
}

void EventCodecTest::rejectsMutatedProvenance() {
    auto envelope = encodedObject(LegalEvent{accepted(1, true)});
    auto payload = envelope.value(QStringLiteral("payload")).toObject();
    auto authority_object = payload.value(QStringLiteral("authority")).toObject();
    auto primary = authority_object.value(QStringLiteral("primary")).toObject();
    auto source = primary.value(QStringLiteral("provenance")).toObject();
    source.insert(QStringLiteral("source_url"), QStringLiteral("javascript:alert(1)"));
    primary.insert(QStringLiteral("provenance"), source);
    authority_object.insert(QStringLiteral("primary"), primary);
    payload.insert(QStringLiteral("authority"), authority_object);
    envelope.insert(QStringLiteral("payload"), payload);
    auto decoded = appellate::storage::decodeEvent(compact(envelope));
    QVERIFY(!decoded.has_value());
    QCOMPARE(decoded.error().code, EventCodecErrorCode::IncompleteAuthority);

    envelope = encodedObject(LegalEvent{accepted(1, true)});
    payload = envelope.value(QStringLiteral("payload")).toObject();
    authority_object = payload.value(QStringLiteral("authority")).toObject();
    primary = authority_object.value(QStringLiteral("primary")).toObject();
    source = primary.value(QStringLiteral("provenance")).toObject();
    source.insert(QStringLiteral("precedential_status"), QStringLiteral("binding"));
    primary.insert(QStringLiteral("provenance"), source);
    authority_object.insert(QStringLiteral("primary"), primary);
    payload.insert(QStringLiteral("authority"), authority_object);
    envelope.insert(QStringLiteral("payload"), payload);
    decoded = appellate::storage::decodeEvent(compact(envelope));
    QVERIFY(!decoded.has_value());
    QCOMPARE(decoded.error().code, EventCodecErrorCode::IncompleteAuthority);
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
    envelope.insert(QStringLiteral("schema_version"), 3);
    decoded = appellate::storage::decodeEvent(compact(envelope));
    QVERIFY(!decoded.has_value());
    QCOMPARE(decoded.error().code, EventCodecErrorCode::UnsupportedVersion);
}

void EventCodecTest::rejectsDuplicateSemanticMembers() {
    const QByteArray duplicate_top =
        R"({"event_type":"filing.accepted","\u0065vent_type":"filing.accepted","payload":{},"schema_version":1})";
    auto decoded = appellate::storage::decodeEvent(duplicate_top);
    QVERIFY(!decoded.has_value());
    QCOMPARE(decoded.error().code, EventCodecErrorCode::DuplicateMember);

    const auto encoded = appellate::storage::encodeEvent(LegalEvent{accepted(1, true)});
    QVERIFY(encoded.has_value());
    const auto replaced = QByteArray{*encoded}.replace(
        QByteArrayLiteral("\"source_url\":"),
        QByteArrayLiteral(
            "\"\\u0073ource_url\":\"https://example.invalid/other\",\"source_url\":"));
    decoded = appellate::storage::decodeEvent(replaced);
    QVERIFY(!decoded.has_value());
    QCOMPARE(decoded.error().code, EventCodecErrorCode::DuplicateMember);
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
