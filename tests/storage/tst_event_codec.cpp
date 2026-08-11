#include "appellate/storage/event_codec.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <algorithm>
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
    void replaysSessionScopedRecordAccessBranches();
    void boundsRecordAccessJournal();
    void rejectsTamperedReorderedAndRedundantRecordAccess();
};

[[nodiscard]] appellate::model::RecordAccessPolicy accessPolicy() {
    return {"test.record.one",
            "test.record.access-policy",
            {{"test.document.psr", "test.authority.psr-access", "test.disclosure.psr", {}},
             {"test.document.exhibit",
              "test.authority.exhibit-access",
              "test.disclosure.exhibit",
              {}}}};
}

void appendAccess(appellate::storage::SessionSnapshot& snapshot,
                  const appellate::model::RecordAccessEvent& event) {
    const auto encoded = appellate::storage::encodeRecordAccessEvent(event);
    if (!encoded) {
        QTest::qFail(qPrintable(encoded.error().message), __FILE__, __LINE__);
        return;
    }
    snapshot.sequence = static_cast<qint64>(event.sequence);
    snapshot.events.push_back(
        appellate::storage::StoredEvent{static_cast<qint64>(event.sequence),
                                        appellate::storage::recordAccessEventType(event.action),
                                        *encoded, QString::fromUtf8(event.authority_id)});
}

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

void EventCodecTest::replaysSessionScopedRecordAccessBranches() {
    appellate::storage::SessionSnapshot snapshot;
    snapshot.session_id = QStringLiteral("test.session.one");
    const auto policy = accessPolicy();

    const auto grant = appellate::storage::makeRecordAccessEvent(
        snapshot, policy, "test.event.grant-psr", "test.document.psr",
        appellate::model::RecordAccessAction::Grant, "2026-08-11T10:00:00Z");
    QVERIFY(grant.has_value());
    appendAccess(snapshot, *grant);

    const auto reopened = appellate::storage::projectRecordAccess(snapshot, policy);
    QVERIFY(reopened.has_value());
    QCOMPARE(reopened->authorized_document_ids, std::vector<std::string>{"test.document.psr"});

    const auto revoke = appellate::storage::makeRecordAccessEvent(
        snapshot, policy, "test.event.revoke-psr", "test.document.psr",
        appellate::model::RecordAccessAction::Revoke, "2026-08-11T10:01:00Z");
    QVERIFY(revoke.has_value());
    appendAccess(snapshot, *revoke);
    const auto full = appellate::storage::projectRecordAccess(snapshot, policy);
    QVERIFY(full.has_value());
    QVERIFY(full->authorized_document_ids.empty());

    const auto prefix = appellate::storage::projectRecordAccess(snapshot, policy, 1);
    QVERIFY(prefix.has_value());
    QCOMPARE(prefix->authorized_document_ids, std::vector<std::string>{"test.document.psr"});

    appellate::storage::SessionSnapshot divergent;
    divergent.session_id = snapshot.session_id;
    appendAccess(divergent, *grant);
    const auto exhibit = appellate::storage::makeRecordAccessEvent(
        divergent, policy, "test.event.grant-exhibit", "test.document.exhibit",
        appellate::model::RecordAccessAction::Grant, "2026-08-11T10:02:00Z");
    QVERIFY(exhibit.has_value());
    appendAccess(divergent, *exhibit);
    const auto branch = appellate::storage::projectRecordAccess(divergent, policy);
    QVERIFY(branch.has_value());
    QCOMPARE(branch->authorized_document_ids,
             (std::vector<std::string>{"test.document.exhibit", "test.document.psr"}));
}

void EventCodecTest::boundsRecordAccessJournal() {
    appellate::storage::SessionSnapshot at_limit;
    at_limit.session_id = QStringLiteral("test.session.journal-bound");
    at_limit.events.reserve(appellate::storage::maximum_record_access_events);
    for (std::size_t index = 0; index < appellate::storage::maximum_record_access_events; ++index) {
        at_limit.events.push_back(appellate::storage::StoredEvent{
            static_cast<qint64>(index + 1), QStringLiteral("workflow.non-access"),
            QByteArrayLiteral("{}"), QStringLiteral("test.authority.workflow")});
    }
    at_limit.sequence = static_cast<qint64>(appellate::storage::maximum_record_access_events);

    const auto projected = appellate::storage::projectRecordAccess(at_limit, accessPolicy());
    QVERIFY2(projected.has_value(), projected ? "" : qPrintable(projected.error().message));
    QCOMPARE(projected->through_sequence,
             std::uint64_t{appellate::storage::maximum_record_access_events});

    const auto rejected_append = appellate::storage::makeRecordAccessEvent(
        at_limit, accessPolicy(), "test.event.over-journal-bound", "test.document.psr",
        appellate::model::RecordAccessAction::Grant, "2026-08-11T10:03:00Z");
    QVERIFY(!rejected_append.has_value());
    QCOMPARE(rejected_append.error().code, EventCodecErrorCode::OutOfRange);

    auto over_limit = at_limit;
    ++over_limit.sequence;
    over_limit.events.push_back(appellate::storage::StoredEvent{
        over_limit.sequence, QStringLiteral("workflow.non-access"), QByteArrayLiteral("{}"),
        QStringLiteral("test.authority.workflow")});
    const auto rejected_replay =
        appellate::storage::projectRecordAccess(over_limit, accessPolicy());
    QVERIFY(!rejected_replay.has_value());
    QCOMPARE(rejected_replay.error().code, EventCodecErrorCode::OutOfRange);
}

void EventCodecTest::rejectsTamperedReorderedAndRedundantRecordAccess() {
    appellate::storage::SessionSnapshot snapshot;
    snapshot.session_id = QStringLiteral("test.session.one");
    const auto policy = accessPolicy();
    auto deficient_policy = policy;
    deficient_policy.rules.front().blocking_deficiencies.push_back(
        appellate::model::RecordDisclosureDeficiency{
            "test.disclosure.psr",
            appellate::model::RecordDisclosureDeficiencyKind::MissingCertificate});
    const auto blocked = appellate::storage::makeRecordAccessEvent(
        snapshot, deficient_policy, "test.event.blocked-psr", "test.document.psr",
        appellate::model::RecordAccessAction::Grant, "2026-08-11T09:59:00Z");
    QVERIFY(!blocked.has_value());
    QCOMPARE(blocked.error().code, EventCodecErrorCode::InvalidTransition);

    const auto grant = appellate::storage::makeRecordAccessEvent(
        snapshot, policy, "test.event.grant-psr", "test.document.psr",
        appellate::model::RecordAccessAction::Grant, "2026-08-11T10:00:00Z");
    QVERIFY(grant.has_value());
    appendAccess(snapshot, *grant);

    const auto redundant = appellate::storage::makeRecordAccessEvent(
        snapshot, policy, "test.event.grant-psr-again", "test.document.psr",
        appellate::model::RecordAccessAction::Grant, "2026-08-11T10:00:30Z");
    QVERIFY(!redundant.has_value());
    QCOMPARE(redundant.error().code, EventCodecErrorCode::InvalidTransition);

    auto tampered = snapshot;
    auto envelope = QJsonDocument::fromJson(tampered.events.front().payload_json).object();
    auto payload = envelope.value(QStringLiteral("payload")).toObject();
    payload.insert(QStringLiteral("sealed_document_id"), QStringLiteral("test.document.exhibit"));
    envelope.insert(QStringLiteral("payload"), payload);
    tampered.events.front().payload_json = compact(envelope);
    const auto tampered_result = appellate::storage::projectRecordAccess(tampered, policy);
    QVERIFY(!tampered_result.has_value());
    QCOMPARE(tampered_result.error().code, EventCodecErrorCode::DigestMismatch);

    auto timestamp_tampered = snapshot;
    envelope = QJsonDocument::fromJson(timestamp_tampered.events.front().payload_json).object();
    payload = envelope.value(QStringLiteral("payload")).toObject();
    payload.insert(QStringLiteral("recorded_at_utc"), QStringLiteral("2026-08-11T10:00:01Z"));
    envelope.insert(QStringLiteral("payload"), payload);
    timestamp_tampered.events.front().payload_json = compact(envelope);
    const auto timestamp_result =
        appellate::storage::projectRecordAccess(timestamp_tampered, policy);
    QVERIFY(!timestamp_result.has_value());
    QCOMPARE(timestamp_result.error().code, EventCodecErrorCode::DigestMismatch);

    appellate::storage::SessionSnapshot time_snapshot;
    time_snapshot.session_id = QStringLiteral("test.session.time");
    const auto noncanonical_time = appellate::storage::makeRecordAccessEvent(
        time_snapshot, policy, "test.event.bad-time", "test.document.psr",
        appellate::model::RecordAccessAction::Grant, "2026-08-11 10:00:00Z");
    QVERIFY(!noncanonical_time.has_value());
    QCOMPARE(noncanonical_time.error().code, EventCodecErrorCode::InvalidField);

    auto wrong_authority = snapshot;
    wrong_authority.events.front().authority_id = QStringLiteral("test.authority.other");
    const auto authority_result = appellate::storage::projectRecordAccess(wrong_authority, policy);
    QVERIFY(!authority_result.has_value());
    QCOMPARE(authority_result.error().code, EventCodecErrorCode::SequenceMismatch);

    auto two_events = snapshot;
    const auto revoke = appellate::storage::makeRecordAccessEvent(
        two_events, policy, "test.event.revoke-psr", "test.document.psr",
        appellate::model::RecordAccessAction::Revoke, "2026-08-11T10:01:00Z");
    QVERIFY(revoke.has_value());
    appendAccess(two_events, *revoke);
    std::ranges::reverse(two_events.events);
    const auto reordered = appellate::storage::projectRecordAccess(two_events, policy);
    QVERIFY(!reordered.has_value());
    QCOMPARE(reordered.error().code, EventCodecErrorCode::SequenceMismatch);
}

} // namespace

QTEST_GUILESS_MAIN(EventCodecTest)

#include "tst_event_codec.moc"
