#include "main_window.hpp"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <QString>

namespace {

void setApplicationMetadata() {
    QCoreApplication::setOrganizationName(QStringLiteral("Appellate Workbench"));
    QCoreApplication::setApplicationName(QStringLiteral("Appellate Workbench"));
    QCoreApplication::setApplicationVersion(QString::fromLatin1(APPELLATE_APPLICATION_VERSION));
}

void configureParser(QCommandLineParser& parser) {
    parser.setApplicationDescription(
        QStringLiteral("Local-first native appellate practice simulator"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption(QCommandLineOption(
        QStringLiteral("catalog"),
        QStringLiteral("Use an explicit local immutable-pack catalog directory."),
        QStringLiteral("directory")));
    parser.addOption(QCommandLineOption(
        QStringLiteral("smoke-test"),
        QStringLiteral("Load and validate the supplied pack, process one UI cycle, and exit.")));
    parser.addPositionalArgument(
        QStringLiteral("pack"),
        QStringLiteral("Optional authoring directory or .awpack archive to load."),
        QStringLiteral("[pack]"));
}

[[nodiscard]] bool requestsHeadlessInformation(int argc, char* argv[]) {
    for (int index = 1; index < argc; ++index) {
        const auto argument = QString::fromLocal8Bit(argv[index]);
        if (argument == QStringLiteral("-h") || argument == QStringLiteral("--help") ||
            argument == QStringLiteral("--help-all") || argument == QStringLiteral("-v") ||
            argument == QStringLiteral("--version")) {
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char* argv[]) {
    if (requestsHeadlessInformation(argc, argv)) {
        QCoreApplication application(argc, argv);
        setApplicationMetadata();
        QCommandLineParser parser;
        configureParser(parser);
        parser.process(application);
        return 0;
    }

    QApplication application(argc, argv);
    setApplicationMetadata();

    QCommandLineParser parser;
    configureParser(parser);
    parser.process(application);

    const auto positional = parser.positionalArguments();
    if (positional.size() > 1) {
        parser.showHelp(2);
    }
    const auto pack_source = positional.isEmpty() ? QString{} : positional.constFirst();
    appellate::ui::MainWindow window({}, parser.value(QStringLiteral("catalog")));
    if (!pack_source.isEmpty()) {
        const auto loaded = window.loadSource(pack_source);
        if (!loaded) {
            qCritical().noquote() << loaded.error();
            return 2;
        }
    }
    window.show();
    if (parser.isSet(QStringLiteral("smoke-test"))) {
        application.processEvents();
        if (!pack_source.isEmpty() && window.currentRuntime() == nullptr) {
            qCritical().noquote() << QStringLiteral("Pack did not produce a runtime projection");
            return 3;
        }
        return 0;
    }
    return application.exec();
}
