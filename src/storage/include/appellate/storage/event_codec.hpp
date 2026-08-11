#pragma once

#include "appellate/model/event.hpp"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <expected>

namespace appellate::storage {

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

} // namespace appellate::storage
