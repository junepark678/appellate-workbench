#pragma once

#include <QByteArray>
#include <QStringList>

namespace appellate::cli {

enum class ExitCode : int {
    Success = 0,
    InvalidArguments = 2,
    InvalidPack = 3,
    OperationFailed = 4,
};

struct RunResult final {
    int exit_code{};
    QByteArray standard_output;
    QByteArray standard_error;

    friend bool operator==(const RunResult&, const RunResult&) = default;
};

// Arguments exclude the executable name. Every response is one compact JSON object followed by
// a newline, written to stdout on success and stderr on failure.
[[nodiscard]] RunResult runPackCli(const QStringList& arguments);

} // namespace appellate::cli
