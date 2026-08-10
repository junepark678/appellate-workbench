#include "appellate/packs/pack_archive.hpp"
#include "bench_profile_editor.hpp"
#include "main_window.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QVariant>

#include <array>
#include <optional>
#include <string>
#include <utility>

namespace {

class MainWindowTest final : public QObject {
    Q_OBJECT

  private slots:
    void loadsValidAuthoringDirectory();
    void installsAndLoadsArchiveInInjectedCatalog();
    void malformedSourcePreservesLoadedState();
    void caseAndProfileSelectionStayInSync();
    void actionsExposeAccessibleUsefulStates();
};

using appellate::packs::PackArchive;
using appellate::ui::MainWindow;

[[nodiscard]] QString fixture(const QString& name) {
    return QStringLiteral(APPELLATE_TEST_FIXTURES) + u'/' + name;
}

[[nodiscard]] bool copyTree(const QString& source, const QString& destination) {
    const QDir source_directory(source);
    if (!source_directory.exists() || !QDir{}.mkpath(destination)) {
        return false;
    }
    QDirIterator iterator(source, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const auto source_path = iterator.next();
        const auto relative_path = source_directory.relativeFilePath(source_path);
        const auto destination_path = QDir(destination).filePath(relative_path);
        if (!QDir{}.mkpath(QFileInfo(destination_path).absolutePath()) ||
            !QFile::copy(source_path, destination_path)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<QJsonObject> readObject(const QString& path) {
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(input.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    return document.object();
}

[[nodiscard]] std::optional<QString> writeObject(const QString& path, const QJsonObject& object) {
    const auto document = QJsonDocument(object).toJson(QJsonDocument::Indented);
    QFile output(path);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        output.write(document) != document.size() || !output.flush()) {
        return std::nullopt;
    }
    return QString::fromLatin1(
        QCryptographicHash::hash(document, QCryptographicHash::Sha256).toHex());
}

[[nodiscard]] QJsonObject descriptor(const QString& id, const QString& kind, const QString& path,
                                     const QString& digest) {
    return QJsonObject{
        {QStringLiteral("id"), id},
        {QStringLiteral("kind"), kind},
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("path"), path},
        {QStringLiteral("sha256"), digest},
    };
}

[[nodiscard]] QString createSelectionPack(const QString& root) {
    const auto destination = QDir(root).filePath(QStringLiteral("selection-pack"));
    if (!copyTree(fixture(QStringLiteral("full-resource-pack")), destination)) {
        return {};
    }

    const auto resources = QDir(destination).filePath(QStringLiteral("resources"));
    const auto manifest_path = QDir(destination).filePath(QStringLiteral("manifest.json"));
    auto manifest = readObject(manifest_path);
    auto profile = readObject(QDir(resources).filePath(QStringLiteral("judge-profile.json")));
    auto bench = readObject(QDir(resources).filePath(QStringLiteral("bench-configuration.json")));
    auto runtime_case = readObject(QDir(resources).filePath(QStringLiteral("case.json")));
    auto argument = readObject(QDir(resources).filePath(QStringLiteral("argument-config.json")));
    if (!manifest || !profile || !bench || !runtime_case || !argument) {
        return {};
    }

    profile->insert(QStringLiteral("resource_id"), QStringLiteral("example.judge.second"));
    profile->insert(QStringLiteral("display_name"), QStringLiteral("Judge Willow"));
    const auto profile_path = QStringLiteral("resources/judge-profile-second.json");
    const auto profile_digest = writeObject(QDir(destination).filePath(profile_path), *profile);
    if (!profile_digest) {
        return {};
    }

    auto seats = bench->value(QStringLiteral("seats")).toArray();
    seats.append(QJsonObject{
        {QStringLiteral("seat_id"), QStringLiteral("example.seat.associate")},
        {QStringLiteral("profile_id"), QStringLiteral("example.judge.second")},
        {QStringLiteral("court_role"), QStringLiteral("appellate")},
    });
    bench->insert(QStringLiteral("seats"), seats);
    const auto bench_digest =
        writeObject(QDir(resources).filePath(QStringLiteral("bench-configuration.json")), *bench);
    if (!bench_digest) {
        return {};
    }

    runtime_case->insert(QStringLiteral("resource_id"), QStringLiteral("example.case.second"));
    runtime_case->insert(QStringLiteral("title"),
                         QStringLiteral("Second Fictional Appellant v. Appellee"));
    const auto case_path = QStringLiteral("resources/case-second.json");
    const auto case_digest = writeObject(QDir(destination).filePath(case_path), *runtime_case);
    if (!case_digest) {
        return {};
    }

    argument->insert(QStringLiteral("resource_id"), QStringLiteral("example.argument.second"));
    argument->insert(QStringLiteral("case_id"), QStringLiteral("example.case.second"));
    const auto argument_path = QStringLiteral("resources/argument-config-second.json");
    const auto argument_digest = writeObject(QDir(destination).filePath(argument_path), *argument);
    if (!argument_digest) {
        return {};
    }

    auto contents = manifest->value(QStringLiteral("contents")).toArray();
    bool updated_bench = false;
    for (qsizetype index = 0; index < contents.size(); ++index) {
        auto item = contents.at(index).toObject();
        if (item.value(QStringLiteral("id")).toString() ==
            QStringLiteral("example.bench.fictional")) {
            item.insert(QStringLiteral("sha256"), *bench_digest);
            contents.replace(index, item);
            updated_bench = true;
            break;
        }
    }
    if (!updated_bench) {
        return {};
    }
    contents.append(descriptor(QStringLiteral("example.judge.second"),
                               QStringLiteral("judge_profile"), profile_path, *profile_digest));
    contents.append(descriptor(QStringLiteral("example.case.second"), QStringLiteral("case"),
                               case_path, *case_digest));
    contents.append(descriptor(QStringLiteral("example.argument.second"),
                               QStringLiteral("argument_config"), argument_path, *argument_digest));
    manifest->insert(QStringLiteral("contents"), contents);
    if (!writeObject(manifest_path, *manifest)) {
        return {};
    }
    return destination;
}

void MainWindowTest::loadsValidAuthoringDirectory() {
    QTemporaryDir state;
    QVERIFY(state.isValid());
    const auto source = fixture(QStringLiteral("full-resource-pack"));
    const auto catalog = QDir(state.path()).filePath(QStringLiteral("catalog"));
    MainWindow window(source, catalog);

    QVERIFY(window.currentRuntime() != nullptr);
    QCOMPARE(window.currentRuntime()->cases.size(), std::size_t{1});
    QVERIFY(window.revisionLabel()->text().contains(QStringLiteral("example.full.fictional")));
    QVERIFY(window.revisionLabel()->text().contains(QStringLiteral("1.0.0")));
    QCOMPARE(window.currentSourcePath(), QFileInfo(source).absoluteFilePath());
    QCOMPARE(window.caseList()->count(), 1);
    QCOMPARE(window.caseList()->currentRow(), 0);
    QVERIFY(
        window.courtSummaryLabel()->text().contains(QStringLiteral("Fictional Court of Appeals")));
    QVERIFY(window.procedureSummaryLabel()->text().contains(QStringLiteral("civil appeal")));
    QVERIFY(window.recordSummaryLabel()->text().contains(QStringLiteral("1 docket entry")));
    QVERIFY(window.benchSummaryLabel()->text().contains(QStringLiteral("Judge Rowan")));
    QVERIFY(window.errorLabel()->text().isEmpty());

    const auto profile = window.profileEditor()->profile();
    QVERIFY(profile.has_value());
    QCOMPARE(profile->id, std::string("example.judge.fictional"));
    QVERIFY(window.profileEditor()->fictionalCompositeLabel()->text().contains(
        QStringLiteral("Fictional/composite")));
}

void MainWindowTest::installsAndLoadsArchiveInInjectedCatalog() {
    QTemporaryDir state;
    QVERIFY(state.isValid());
    const auto archive_path = QDir(state.path()).filePath(QStringLiteral("fixture.awpack"));
    const auto exported =
        PackArchive::exportDirectory(fixture(QStringLiteral("full-resource-pack")), archive_path);
    if (!exported) {
        QFAIL(qPrintable(exported.error().message));
    }
    const auto catalog = QDir(state.path()).filePath(QStringLiteral("isolated-catalog"));

    MainWindow first({}, catalog);
    const auto loaded = first.loadSource(archive_path);
    if (!loaded) {
        QFAIL(qPrintable(loaded.error()));
    }
    QVERIFY(first.currentRuntime() != nullptr);
    QCOMPARE(first.currentRuntime()->revision, *exported);
    QCOMPARE(first.catalogRoot(), QFileInfo(catalog).absoluteFilePath());
    QVERIFY(QFileInfo(QDir(catalog).filePath(QStringLiteral("catalog.sqlite"))).isFile());
    const QDir archive_objects(QDir(catalog).filePath(QStringLiteral("archives")));
    QCOMPARE(archive_objects.entryList({QStringLiteral("*.awpack")}, QDir::Files).size(), 1);

    MainWindow second({}, catalog);
    const auto reloaded = second.loadSource(archive_path);
    if (!reloaded) {
        QFAIL(qPrintable(reloaded.error()));
    }
    QVERIFY(second.currentRuntime() != nullptr);
    QCOMPARE(second.currentRuntime()->revision, *exported);
    QCOMPARE(archive_objects.entryList({QStringLiteral("*.awpack")}, QDir::Files).size(), 1);
}

void MainWindowTest::malformedSourcePreservesLoadedState() {
    QTemporaryDir state;
    QVERIFY(state.isValid());
    MainWindow window({}, QDir(state.path()).filePath(QStringLiteral("catalog")));
    const auto valid = window.loadSource(fixture(QStringLiteral("full-resource-pack")));
    if (!valid) {
        QFAIL(qPrintable(valid.error()));
    }
    QVERIFY(window.currentRuntime() != nullptr);
    const auto before_runtime = *window.currentRuntime();
    const auto before_source = window.currentSourcePath();
    const auto before_revision = window.revisionLabel()->text();
    const auto before_profile = window.profileEditor()->profile();
    QVERIFY(before_profile.has_value());

    const auto malformed = QDir(state.path()).filePath(QStringLiteral("malformed"));
    QVERIFY(QDir{}.mkpath(malformed));
    QFile manifest(QDir(malformed).filePath(QStringLiteral("manifest.json")));
    QVERIFY(manifest.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(manifest.write("{}"), qint64{2});
    manifest.close();

    const auto rejected = window.loadSource(malformed);
    QVERIFY(!rejected.has_value());
    QVERIFY(!rejected.error().isEmpty());
    QVERIFY(window.currentRuntime() != nullptr);
    QCOMPARE(*window.currentRuntime(), before_runtime);
    QCOMPARE(window.currentSourcePath(), before_source);
    QCOMPARE(window.revisionLabel()->text(), before_revision);
    QCOMPARE(window.caseList()->count(), 1);
    const auto after_profile = window.profileEditor()->profile();
    QVERIFY(after_profile.has_value());
    QCOMPARE(*after_profile, *before_profile);
    QVERIFY(window.errorLabel()->text().startsWith(QStringLiteral("Error:")));
}

void MainWindowTest::caseAndProfileSelectionStayInSync() {
    QTemporaryDir state;
    QVERIFY(state.isValid());
    const auto source = createSelectionPack(state.path());
    QVERIFY2(!source.isEmpty(), "Failed to create multi-case selection fixture");
    MainWindow window({}, QDir(state.path()).filePath(QStringLiteral("catalog")));
    const auto loaded = window.loadSource(source);
    if (!loaded) {
        QFAIL(qPrintable(loaded.error()));
    }

    QCOMPARE(window.caseList()->count(), 2);
    QCOMPARE(window.profileSelector()->count(), 2);
    window.profileSelector()->setCurrentIndex(1);
    auto selected = window.profileEditor()->profile();
    QVERIFY(selected.has_value());
    QCOMPARE(selected->id, std::string("example.judge.second"));
    QVERIFY(
        window.profileSelector()->currentText().contains(QStringLiteral("fictional/composite")));

    window.caseList()->setCurrentRow(1);
    QCOMPARE(window.caseList()->currentItem()->text(),
             QStringLiteral("Second Fictional Appellant v. Appellee"));
    QCOMPARE(window.profileSelector()->count(), 2);
    QCOMPARE(window.profileSelector()->currentIndex(), 0);
    window.profileSelector()->setCurrentIndex(1);
    selected = window.profileEditor()->profile();
    QVERIFY(selected.has_value());
    QCOMPARE(selected->display_name, std::string("Judge Willow"));
    QVERIFY(
        window.courtSummaryLabel()->text().contains(QStringLiteral("Fictional Court of Appeals")));
    QVERIFY(window.benchSummaryLabel()->text().contains(QStringLiteral("Judge Willow")));
}

void MainWindowTest::actionsExposeAccessibleUsefulStates() {
    QTemporaryDir state;
    QVERIFY(state.isValid());
    MainWindow window({}, QDir(state.path()).filePath(QStringLiteral("catalog")));

    QVERIFY(window.openDirectoryAction()->isEnabled());
    QVERIFY(window.installArchiveAction()->isEnabled());
    QVERIFY(window.importProfileAction()->isEnabled());
    QVERIFY(!window.cloneProfileAction()->isEnabled());
    QVERIFY(!window.exportProfileAction()->isEnabled());

    const std::array actions{
        window.openDirectoryAction(), window.installArchiveAction(), window.importProfileAction(),
        window.cloneProfileAction(),  window.exportProfileAction(),
    };
    for (const auto* action : actions) {
        QVERIFY(action != nullptr);
        QVERIFY(!action->text().isEmpty());
        QVERIFY(action->text().contains(u'&'));
        QVERIFY(!action->property("accessibleName").toString().isEmpty());
        QVERIFY(!action->statusTip().isEmpty());
        QVERIFY(!action->shortcut().isEmpty());
    }

    const auto loaded = window.loadSource(fixture(QStringLiteral("full-resource-pack")));
    if (!loaded) {
        QFAIL(qPrintable(loaded.error()));
    }
    QVERIFY(window.cloneProfileAction()->isEnabled());
    QVERIFY(window.exportProfileAction()->isEnabled());
    QVERIFY(!window.caseList()->accessibleName().isEmpty());
    QVERIFY(!window.profileSelector()->accessibleName().isEmpty());
    QVERIFY(window.caseList()->focusPolicy() != Qt::NoFocus);
    QVERIFY(window.profileSelector()->focusPolicy() != Qt::NoFocus);

    const auto export_path = QDir(state.path()).filePath(QStringLiteral("profile.json"));
    QVERIFY(window.exportProfile(export_path).has_value());
    QFile exported(export_path);
    QVERIFY(exported.open(QIODevice::ReadOnly));
    const auto original = exported.readAll();
    exported.close();
    const auto overwrite = window.exportProfile(export_path);
    QVERIFY(!overwrite.has_value());
    QVERIFY(exported.open(QIODevice::ReadOnly));
    QCOMPARE(exported.readAll(), original);

    const auto not_a_directory = QDir(state.path()).filePath(QStringLiteral("catalog-file"));
    QFile catalog_file(not_a_directory);
    QVERIFY(catalog_file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    catalog_file.close();
    MainWindow unavailable({}, not_a_directory);
    QVERIFY(unavailable.openDirectoryAction()->isEnabled());
    QVERIFY(!unavailable.installArchiveAction()->isEnabled());
    QVERIFY(unavailable.importProfileAction()->isEnabled());
    QVERIFY(unavailable.errorLabel()->text().contains(QStringLiteral("catalog unavailable")));
}

} // namespace

QTEST_MAIN(MainWindowTest)

#include "tst_main_window.moc"
