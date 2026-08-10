#include "appellate/content/render_cli.hpp"

#include <QGuiApplication>

#include <cstdio>

namespace {

[[nodiscard]] bool writeBytes(FILE* stream, const QByteArray& bytes) {
    const auto size = static_cast<std::size_t>(bytes.size());
    return std::fwrite(bytes.constData(), 1, size, stream) == size && std::fflush(stream) == 0;
}

} // namespace

int main(int argc, char* argv[]) {
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        static_cast<void>(qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen")));
    }
    QGuiApplication application(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("appellate-render"));

    auto arguments = application.arguments();
    arguments.removeFirst();
    const auto result = appellate::content::runRenderCli(arguments);
    const auto output_ok =
        result.standard_output.isEmpty() || writeBytes(stdout, result.standard_output);
    const auto error_ok =
        result.standard_error.isEmpty() || writeBytes(stderr, result.standard_error);
    if (!output_ok || !error_ok) {
        return static_cast<int>(appellate::content::RenderCliExitCode::OperationFailed);
    }
    return result.exit_code;
}
