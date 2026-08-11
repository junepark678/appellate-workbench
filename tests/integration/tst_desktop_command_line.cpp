#include <QDir>
#include <QFile>
#include <QFileInfo>
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
    QCOMPARE(result.standard_error.trimmed(),
             QByteArrayLiteral(
                 "Pack source must be an authoring-pack directory or a regular .awpack archive"));
}

QTEST_MAIN(DesktopCommandLineTest)

#include "tst_desktop_command_line.moc"
