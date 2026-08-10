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
    DuplicateContentPath,
    DuplicatePayloadId,
    UndeclaredFile,
    ResourceTooLarge,
    DigestMismatch,
    InvalidJudgeProfile,
    DuplicateJsonKey,
    SchemaViolation,
    UnsupportedResourceKind,
    CrossReferenceFailure,
};

struct Error final {
    ErrorCode code;
    QString message;

    friend bool operator==(const Error&, const Error&) = default;
};

} // namespace appellate::packs
