#pragma once

#include <QString>

namespace appellate::packs {

enum class ErrorCode {
    CannotRead,
    InvalidJson,
    InvalidManifest,
    UnsupportedSchema,
    UnsafePath,
    DuplicateContentId,
    DigestMismatch,
    InvalidJudgeProfile,
};

struct Error final {
    ErrorCode code;
    QString message;

    friend bool operator==(const Error&, const Error&) = default;
};

} // namespace appellate::packs
