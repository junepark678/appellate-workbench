#include "pack_cli.hpp"

#include <QCoreApplication>
#include <QFile>

#include <cstdio>

namespace {

void writeBytes(FILE* stream, const QByteArray& bytes) {
    if (bytes.isEmpty()) {
        return;
    }
    QFile file;
    if (!file.open(stream, QIODevice::WriteOnly, QFileDevice::DontCloseHandle)) {
        return;
    }
    static_cast<void>(file.write(bytes));
    static_cast<void>(file.flush());
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Appellate Workbench"));
    QCoreApplication::setApplicationName(QStringLiteral("appellate-pack"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    auto arguments = application.arguments();
    arguments.removeFirst();
    const auto result = appellate::cli::runPackCli(arguments);
    writeBytes(stdout, result.standard_output);
    writeBytes(stderr, result.standard_error);
    return result.exit_code;
}
