#include "appellate/packs/pack_archive.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

namespace {

[[nodiscard]] QString executablePath() { return QString::fromUtf8(APPELLATE_WORKBENCH_EXECUTABLE); }

[[nodiscard]] QString fixturePath(QStringView name) {
    return QDir(QString::fromUtf8(APPELLATE_TEST_FIXTURES)).filePath(name.toString());
}

struct ProcessResult final {
    int exit_code{};
    QProcess::ExitStatus exit_status{};
    QByteArray standard_output;
    QByteArray standard_error;
};

[[nodiscard]] ProcessResult runDesktop(const QStringList& arguments,
                                       const QString& isolated_data_root, bool offscreen = true) {
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    const auto runtime_directory = QDir(isolated_data_root).filePath(QStringLiteral("runtime"));
    static_cast<void>(QDir{}.mkpath(runtime_directory));
    static_cast<void>(QFile::setPermissions(runtime_directory, QFileDevice::ReadOwner |
                                                                   QFileDevice::WriteOwner |
                                                                   QFileDevice::ExeOwner));
    environment.remove(QStringLiteral("LD_LIBRARY_PATH"));
    environment.remove(QStringLiteral("LD_PRELOAD"));
    environment.remove(QStringLiteral("QT_PLUGIN_PATH"));
    environment.remove(QStringLiteral("QT_QPA_PLATFORM_PLUGIN_PATH"));
    environment.remove(QStringLiteral("DISPLAY"));
    environment.remove(QStringLiteral("WAYLAND_DISPLAY"));
    if (offscreen) {
        environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    } else {
        environment.remove(QStringLiteral("QT_QPA_PLATFORM"));
    }
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"),
                       QDir(isolated_data_root).filePath(QStringLiteral("config")));
    environment.insert(QStringLiteral("XDG_DATA_HOME"),
                       QDir(isolated_data_root).filePath(QStringLiteral("data")));
    environment.insert(QStringLiteral("XDG_CACHE_HOME"),
                       QDir(isolated_data_root).filePath(QStringLiteral("cache")));
    environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtime_directory);
    process.setProcessEnvironment(environment);
    process.setProgram(executablePath());
    process.setArguments(arguments);
    process.start();
    if (!process.waitForStarted(10'000)) {
        return ProcessResult{-1, QProcess::CrashExit, {}, process.errorString().toUtf8()};
    }
    if (!process.waitForFinished(30'000)) {
        process.kill();
        static_cast<void>(process.waitForFinished(5'000));
        return ProcessResult{-1, QProcess::CrashExit, process.readAllStandardOutput(),
                             process.readAllStandardError()};
    }
    return ProcessResult{process.exitCode(), process.exitStatus(), process.readAllStandardOutput(),
                         process.readAllStandardError()};
}

} // namespace

class DesktopCommandLineTest final : public QObject {
    Q_OBJECT

  private slots:
    void exposesVersionAndHelp();
    void smokeLoadsPackWithoutPersistentUserState();
    void smokeRejectsInvalidPack();
    void offlineSelfTestImportsGroundedPackAndReopensLocalSessions();
};

void DesktopCommandLineTest::exposesVersionAndHelp() {
    QTemporaryDir isolated;
    QVERIFY(isolated.isValid());
    QVERIFY(QFileInfo::exists(executablePath()));

    const auto version = runDesktop({QStringLiteral("--version")}, isolated.path(), false);
    QCOMPARE(version.exit_status, QProcess::NormalExit);
    QCOMPARE(version.exit_code, 0);
    QVERIFY(version.standard_output.contains("0.1.0"));

    const auto help = runDesktop({QStringLiteral("--help")}, isolated.path(), false);
    QCOMPARE(help.exit_status, QProcess::NormalExit);
    QCOMPARE(help.exit_code, 0);
    QVERIFY(help.standard_output.contains("--smoke-test"));
    QVERIFY(help.standard_output.contains("--catalog"));
}

void DesktopCommandLineTest::smokeLoadsPackWithoutPersistentUserState() {
    QTemporaryDir isolated;
    QVERIFY(isolated.isValid());
    const auto catalog = QDir(isolated.path()).filePath(QStringLiteral("catalog"));

    const auto result = runDesktop({QStringLiteral("--smoke-test"), QStringLiteral("--catalog"),
                                    catalog, fixturePath(QStringLiteral("full-resource-pack-v2"))},
                                   isolated.path());

    QCOMPARE(result.exit_status, QProcess::NormalExit);
    QCOMPARE(result.exit_code, 0);
    QVERIFY2(result.standard_error.isEmpty(), result.standard_error.constData());
    const QJsonObject expected{
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("command"), QStringLiteral("smoke-test")},
        {QStringLiteral("status"), QStringLiteral("ok")},
        {QStringLiteral("pack_id"), QStringLiteral("example.full.fictional")},
        {QStringLiteral("version"), QStringLiteral("2.0.0")},
        {QStringLiteral("revision_sha256"),
         QStringLiteral("023008f685d42634a271a626d5df1eb770ee5a6141a1b199eaa6d9945c4f15ce")},
        {QStringLiteral("case_count"), 1},
        {QStringLiteral("case_ids"), QJsonArray{QStringLiteral("example.case.fictional")}},
    };
    QCOMPARE(result.standard_output, QJsonDocument(expected).toJson(QJsonDocument::Compact) + '\n');
    const auto parsed = QJsonDocument::fromJson(result.standard_output);
    QVERIFY(parsed.isObject());
    QCOMPARE(parsed.object(), expected);
}

void DesktopCommandLineTest::smokeRejectsInvalidPack() {
    QTemporaryDir isolated;
    QVERIFY(isolated.isValid());
    const auto missing = QDir(isolated.path()).filePath(QStringLiteral("missing.awpack"));

    const auto result =
        runDesktop({QStringLiteral("--smoke-test"), QStringLiteral("--catalog"),
                    QDir(isolated.path()).filePath(QStringLiteral("catalog")), missing},
                   isolated.path());

    QCOMPARE(result.exit_status, QProcess::NormalExit);
    QCOMPARE(result.exit_code, 2);
    QVERIFY(result.standard_output.isEmpty());
    QCOMPARE(result.standard_error.trimmed(),
             QByteArrayLiteral(
                 "Pack source must be an authoring-pack directory or a regular .awpack archive"));
}

void DesktopCommandLineTest::offlineSelfTestImportsGroundedPackAndReopensLocalSessions() {
    QTemporaryDir isolated;
    QVERIFY(isolated.isValid());
    const auto offline_pack =
        QDir(isolated.path()).filePath(QStringLiteral("full-resource-pack-v2.awpack"));
    const auto exported = appellate::packs::PackArchive::exportDirectory(
        fixturePath(QStringLiteral("full-resource-pack-v2")), offline_pack);
    QVERIFY2(exported.has_value(), exported ? "" : qPrintable(exported.error().message));

    const auto result = runDesktop(
        {QStringLiteral("--offline-self-test"), QStringLiteral("--offline-e2e-pack"), offline_pack,
         QStringLiteral("--catalog"), QDir(isolated.path()).filePath(QStringLiteral("catalog")),
         QStringLiteral(APPELLATE_GOLD_ARCHIVE)},
        isolated.path());
    QCOMPARE(result.exit_status, QProcess::NormalExit);
    QVERIFY2(result.exit_code == 0, result.standard_error.constData());
    QVERIFY2(result.standard_error.isEmpty(), result.standard_error.constData());
    const auto output = QJsonDocument::fromJson(result.standard_output).object();
    QCOMPARE(output.size(), 27);
    QCOMPARE(output.value(QStringLiteral("schema_version")).toInt(), 2);
    QCOMPARE(output.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    QCOMPARE(output.value(QStringLiteral("network_isolation")).toString(),
             QStringLiteral("required-from-caller"));
    QCOMPARE(output.value(QStringLiteral("bundled_workflow_commands")).toInt(), 1);
    QCOMPARE(output.value(QStringLiteral("bundled_workflow_events")).toInt(), 1);
    QCOMPARE(output.value(QStringLiteral("bundled_workflow_pins")).toInt(), 1);
    QCOMPARE(output.value(QStringLiteral("bundled_workflow_created_at_utc")).toString(),
             QStringLiteral("2026-08-11T10:00:00Z"));
    QCOMPARE(output.value(QStringLiteral("bundled_workflow_digest")).toString(),
             QStringLiteral("6adc6f9435f2d1e57b6825e830570715cf6e9baeb867bc39e3aa5315835e7580"));
    QCOMPARE(output.value(QStringLiteral("bundled_workflow_session_id")).toString(),
             QStringLiteral("workflow.session."
                            "5f62a8255168bf9cabfe35af7e09ad86d368dcbd37683cc5206010f170e8db70"));
    QCOMPARE(output.value(QStringLiteral("imported_workflow_commands")).toInt(), 1);
    QCOMPARE(output.value(QStringLiteral("imported_workflow_events")).toInt(), 2);
    QCOMPARE(output.value(QStringLiteral("imported_workflow_docket_entries")).toInt(), 2);
    QCOMPARE(output.value(QStringLiteral("imported_workflow_pins")).toInt(), 1);
    QCOMPARE(output.value(QStringLiteral("imported_workflow_asset_references")).toInt(), 1);
    QCOMPARE(output.value(QStringLiteral("imported_workflow_created_at_utc")).toString(),
             QStringLiteral("2026-08-11T10:00:00Z"));
    QCOMPARE(output.value(QStringLiteral("imported_workflow_session_id")).toString(),
             QStringLiteral("workflow.session."
                            "16a9ac9c6f55f8a2390d031e64de8f2deb23f46e34e37a5a5aa87d5e9e3a0df2"));
    QCOMPARE(output.value(QStringLiteral("imported_workflow_digest")).toString(),
             QStringLiteral("8f4f7ed230d52c9ca6dee8e8781ca5a587a5a1b59882c02c81f2a16ff3e0189b"));
    QCOMPARE(output.value(QStringLiteral("imported_workflow_rows_sha256")).toString(),
             QStringLiteral("3fa96066cd77a349a9dce45041c94d3af2d5ee282dfec9aeb57e52473b1b61ed"));
    QCOMPARE(output.value(QStringLiteral("imported_asset_sha256")).toString(),
             QStringLiteral("b45710d93705fc230515730d26e638636005779238f785bfb51dd80006673d4d"));
    QCOMPARE(output.value(QStringLiteral("imported_grounded_pack"))
                 .toObject()
                 .value(QStringLiteral("sha256"))
                 .toString(),
             QString::fromStdString(exported->digest));
    QCOMPARE(output.value(QStringLiteral("oral_session_id")).toString(),
             QStringLiteral("oral.argument.session."
                            "00ab90968780fc8513550f093a3fb02e9c681b714aeb220ee23f781167ce8991"));
    QCOMPARE(output.value(QStringLiteral("oral_journal_entries")).toInt(), 2);
    QCOMPARE(output.value(QStringLiteral("oral_created_at_utc")).toString(),
             QStringLiteral("2026-08-11T10:00:00Z"));
    QCOMPARE(output.value(QStringLiteral("oral_pins")).toInt(), 1);
    QCOMPARE(output.value(QStringLiteral("oral_rows_sha256")).toString(),
             QStringLiteral("fc30b2025e94ecf1df82116776b1fe16381e82884a742ac4841b283007d29d5a"));
    QCOMPARE(output.value(QStringLiteral("oral_transcript_sha256")).toString(),
             QStringLiteral("de2bfe06f60197c7584077d05142915b9dfbff815771b1265f220c51661ddb31"));
}

QTEST_MAIN(DesktopCommandLineTest)

#include "tst_desktop_command_line.moc"
