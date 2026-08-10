#pragma once

#include <string>

namespace appellate::engine {

enum class ErrorCode {
    InvalidDefinition,
    MissingAuthority,
    InvalidCase,
    InvalidSession,
    InvalidCommand,
    UnknownActor,
    DuplicateSubmission,
    InvalidEvent,
    InvalidTransition,
};

struct Error final {
    ErrorCode code;
    std::string message;

    friend bool operator==(const Error&, const Error&) = default;
};

} // namespace appellate::engine
