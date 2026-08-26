#pragma once

#include "appellate/model/event.hpp"
#include "appellate/model/record_access.hpp"
#include "appellate/storage/session_store.hpp"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <cstddef>
#include <expected>
#include <optional>
#include <string_view>

namespace appellate::storage {

inline constexpr std::size_t maximum_record_access_events = 4'096;

enum class EventCodecErrorCode {
    InvalidJson,
    DuplicateMember,
    PayloadTooLarge,
    UnsupportedVersion,
    UnknownEventType,
    MissingField,
    UnexpectedField,
    InvalidField,
    OutOfRange,
    IncompleteAuthority,
    DigestMismatch,
    SequenceMismatch,
    InvalidTransition,
};

struct EventCodecError final {
    EventCodecErrorCode code;
    QString message;

    friend bool operator==(const EventCodecError&, const EventCodecError&) = default;
};

[[nodiscard]] std::expected<QByteArray, EventCodecError>
encodeEvent(const model::LegalEvent& event);

[[nodiscard]] std::expected<model::LegalEvent, EventCodecError> decodeEvent(QByteArrayView encoded);

[[nodiscard]] QString eventType(const model::LegalEvent& event);

[[nodiscard]] QString primaryAuthorityId(const model::LegalEvent& event);

// Record-access events share the durable session event_log but have their own
// strict, hash-chained codec. The stored sequence, event type, authority, exact
// pinned record policy, and payload all participate in replay validation.
[[nodiscard]] std::expected<QByteArray, EventCodecError>
encodeRecordAccessEvent(const model::RecordAccessEvent& event);

[[nodiscard]] std::expected<model::RecordAccessEvent, EventCodecError>
decodeRecordAccessEvent(QByteArrayView encoded);

[[nodiscard]] QString recordAccessEventType(model::RecordAccessAction action);

[[nodiscard]] std::expected<model::RecordAccessProjection, EventCodecError>
projectRecordAccess(const SessionSnapshot& snapshot, const model::RecordAccessPolicy& policy,
                    std::optional<qint64> through_sequence = std::nullopt);

// Builds the next event only after replaying the current snapshot and checking
// the policy-authority binding and state transition. Callers persist the
// returned event through SessionStore::append using recordAccessEventType(),
// encodeRecordAccessEvent(), and the event's authority_id.
[[nodiscard]] std::expected<model::RecordAccessEvent, EventCodecError>
makeRecordAccessEvent(const SessionSnapshot& snapshot, const model::RecordAccessPolicy& policy,
                      std::string_view event_id, std::string_view sealed_document_id,
                      model::RecordAccessAction action, std::string_view recorded_at_utc);

} // namespace appellate::storage
