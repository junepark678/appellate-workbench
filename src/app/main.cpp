#include "local_session_provider.hpp"
#include "main_window.hpp"
#include "offline_self_test.hpp"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTextStream>

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
    parser.addOption(QCommandLineOption(
        QStringLiteral("offline-self-test"),
        QStringLiteral("Run the artifact-derived starter workflow, CAS, reopen, and grounded "
                       "oral release self-test. The caller must block networking.")));
    parser.addOption(QCommandLineOption(
        QStringLiteral("offline-e2e-pack"),
        QStringLiteral("Explicit schema-2 grounded starter archive exported by appellate-pack "
                       "and imported by --offline-self-test."),
        QStringLiteral("archive")));
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
    if (parser.isSet(QStringLiteral("offline-self-test"))) {
        if (pack_source.isEmpty() || !parser.isSet(QStringLiteral("offline-e2e-pack"))) {
            qCritical().noquote() << QStringLiteral(
                "--offline-self-test requires one bundled workflow pack and an artifact-derived "
                "--offline-e2e-pack <archive>");
            return 4;
        }
        const auto result = appellate::ui::runOfflineSelfTest(
            application, parser.value(QStringLiteral("catalog")), pack_source,
            parser.value(QStringLiteral("offline-e2e-pack")));
        if (!result) {
            qCritical().noquote() << result.error();
            return 4;
        }
        QTextStream output(stdout);
        output << *result << Qt::endl;
        return 0;
    }
    const auto local_sessions = appellate::ui::LocalSessionProvider::fromStandardPaths();
    if (!local_sessions) {
        qCritical().noquote() << QStringLiteral(
                                     "Local workflow and oral-argument sessions unavailable: %1")
                                     .arg(local_sessions.error());
    }
    const auto provider =
        local_sessions ? std::shared_ptr<appellate::ui::LocalSessionProvider>{*local_sessions}
                       : std::shared_ptr<appellate::ui::LocalSessionProvider>{};
    appellate::ui::MainWindow window({}, parser.value(QStringLiteral("catalog")), nullptr, provider,
                                     {}, {}, provider, {}, {}, {}, {}, provider);
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
        if (!pack_source.isEmpty()) {
            const auto& runtime = *window.currentRuntime();
            QJsonArray case_ids;
            for (const auto& runtime_case : runtime.cases) {
                case_ids.append(QString::fromStdString(runtime_case.definition.id.value));
            }
            const QJsonObject result{
                {QStringLiteral("schema_version"), 1},
                {QStringLiteral("command"), QStringLiteral("smoke-test")},
                {QStringLiteral("status"), QStringLiteral("ok")},
                {QStringLiteral("pack_id"), QString::fromStdString(runtime.revision.id.value)},
                {QStringLiteral("version"), QString::fromStdString(runtime.revision.version)},
                {QStringLiteral("revision_sha256"),
                 QString::fromStdString(runtime.revision.digest)},
                {QStringLiteral("case_count"), static_cast<int>(runtime.cases.size())},
                {QStringLiteral("case_ids"), case_ids},
            };
            QTextStream output(stdout);
            output << QJsonDocument(result).toJson(QJsonDocument::Compact) << Qt::endl;
        }
        return 0;
    }
    return application.exec();
}
