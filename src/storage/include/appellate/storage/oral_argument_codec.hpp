#pragma once

#include "appellate/model/oral_argument.hpp"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <expected>

namespace appellate::storage {

enum class OralArgumentCodecErrorCode {
    InvalidJson,
    DuplicateMember,
    MissingField,
    UnexpectedField,
    InvalidField,
    OutOfRange,
};

struct OralArgumentCodecError final {
    OralArgumentCodecErrorCode code;
    QString message;

    friend bool operator==(const OralArgumentCodecError&, const OralArgumentCodecError&) = default;
};

struct OralArgumentOpeningCommand final {
    QString session_id;
    QString command_id;
    QString engine_revision;
    QString recorded_at_utc;
    model::OralArgumentConfiguration configuration;

    friend bool operator==(const OralArgumentOpeningCommand&,
                           const OralArgumentOpeningCommand&) = default;
};

struct OralArgumentCounselCommand final {
    QString session_id;
    QString command_id;
    QString recorded_at_utc;
    model::CounselAnswer answer;

    friend bool operator==(const OralArgumentCounselCommand&,
                           const OralArgumentCounselCommand&) = default;
};

[[nodiscard]] auto
encodeOralArgumentConfiguration(const model::OralArgumentConfiguration& configuration)
    -> std::expected<QByteArray, OralArgumentCodecError>;

[[nodiscard]] auto decodeOralArgumentConfiguration(QByteArrayView encoded)
    -> std::expected<model::OralArgumentConfiguration, OralArgumentCodecError>;

[[nodiscard]] auto encodeCounselAnswer(const model::CounselAnswer& answer)
    -> std::expected<QByteArray, OralArgumentCodecError>;

[[nodiscard]] auto decodeCounselAnswer(QByteArrayView encoded)
    -> std::expected<model::CounselAnswer, OralArgumentCodecError>;

[[nodiscard]] auto encodeOralArgumentOpeningCommand(const OralArgumentOpeningCommand& command)
    -> std::expected<QByteArray, OralArgumentCodecError>;

[[nodiscard]] auto decodeOralArgumentOpeningCommand(QByteArrayView encoded)
    -> std::expected<OralArgumentOpeningCommand, OralArgumentCodecError>;

[[nodiscard]] auto encodeOralArgumentCounselCommand(const OralArgumentCounselCommand& command)
    -> std::expected<QByteArray, OralArgumentCodecError>;

[[nodiscard]] auto decodeOralArgumentCounselCommand(QByteArrayView encoded)
    -> std::expected<OralArgumentCounselCommand, OralArgumentCodecError>;

[[nodiscard]] auto encodeOralArgumentEvent(const model::OralArgumentEvent& event)
    -> std::expected<QByteArray, OralArgumentCodecError>;

[[nodiscard]] auto decodeOralArgumentEvent(QByteArrayView encoded)
    -> std::expected<model::OralArgumentEvent, OralArgumentCodecError>;

} // namespace appellate::storage
