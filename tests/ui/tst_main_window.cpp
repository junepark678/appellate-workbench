#include "appellate/packs/pack_archive.hpp"
#include "appellate/packs/pack_catalog.hpp"
#include "appellate/packs/resolved_pack.hpp"
#include "appellate/storage/session_store.hpp"
#include "bench_profile_editor.hpp"
#include "main_window.hpp"
#include "oral_argument_launch_provider.hpp"
#include "oral_argument_workspace.hpp"
#include "record_workspace.hpp"
#include "session_controller.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <QVariant>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace appellate::ui {

class MainWindowTestAccess final {
  public:
    [[nodiscard]] static auto forkRecordAccessConnection(const MainWindow& window)
        -> std::expected<std::unique_ptr<storage::SessionStore>, storage::StoreError> {
        if (window.record_access_owner_store_ == nullptr) {
            return std::unexpected(storage::StoreError{
                storage::StoreErrorCode::InvalidArgument,
                QStringLiteral("MainWindow has no live record-access session owner"),
            });
        }
        return window.record_access_owner_store_->forkConnection();
    }
};

} // namespace appellate::ui

namespace {

class MainWindowTest final : public QObject {
    Q_OBJECT

  private slots:
    void loadsValidAuthoringDirectory();
    void installsAndLoadsArchiveInInjectedCatalog();
    void malformedSourcePreservesLoadedState();
    void caseAndProfileSelectionStayInSync();
    void installedRecordActionOpensSearchablePdf();
    void recordFailuresPreserveLastGoodWorkspace();
    void sealedRecordAccessPersistsAndRejectsTamperedReplay();
    void argumentLaunchUsesExactSelectedConfigurationAndPreservesWorkspaceOnError();
    void actionsExposeAccessibleUsefulStates();
};

using appellate::packs::PackArchive;
using appellate::ui::MainWindow;

class RecordingLaunchProvider final : public appellate::ui::OralArgumentLaunchProvider {
  public:
    struct Call final {
        appellate::model::PackRevision root_revision;
        appellate::model::CaseId case_id;
        appellate::packs::RuntimeArgumentConfigId argument_configuration_id;
        std::optional<appellate::model::PackRevision> configuration_owner;
    };

    [[nodiscard]] auto
    open(const appellate::packs::ResolvedPack& resolved_pack,
         const appellate::model::CaseId& case_id,
         const appellate::packs::RuntimeArgumentConfigId& argument_configuration_id)
        -> std::expected<std::unique_ptr<appellate::app::OralArgumentSessionController>,
                         appellate::app::OralArgumentSessionError> override {
        calls.push_back(Call{
            resolved_pack.root().revision,
            case_id,
            argument_configuration_id,
            resolved_pack.resourceOwner(argument_configuration_id.value),
        });
        return std::unexpected(appellate::app::OralArgumentSessionError{
            appellate::app::OralArgumentSessionErrorCode::InvalidConfiguration,
            QStringLiteral("Injected provider refusal after exact launch capture"),
        });
    }

    std::vector<Call> calls;
};

class RecordingRecordAccessProvider final : public appellate::ui::RecordAccessTransitionProvider {
  public:
    struct Transition final {
        QString session_id;
        std::uint64_t sequence{};
        std::string disclosure_id;
        appellate::model::RecordAccessAction action{};
    };

    [[nodiscard]] auto createdAtUtc(QStringView session_id)
        -> std::expected<QString, QString> override {
        created_sessions.push_back(session_id.toString());
        return QStringLiteral("2026-08-11T09:00:00Z");
    }

    [[nodiscard]] auto next(QStringView session_id, std::uint64_t next_sequence,
                            const appellate::packs::RuntimeRecordDisclosureId& disclosure_id,
                            appellate::model::RecordAccessAction action)
        -> std::expected<appellate::ui::RecordAccessTransitionStamp, QString> override {
        transitions.push_back(
            Transition{session_id.toString(), next_sequence, disclosure_id.value, action});
        return appellate::ui::RecordAccessTransitionStamp{
            QStringLiteral("test.record.access.event.%1").arg(next_sequence),
            QStringLiteral("2026-08-11T09:00:%1Z").arg(next_sequence, 2, 10, QLatin1Char('0')),
        };
    }

    std::vector<QString> created_sessions;
    std::vector<Transition> transitions;
};

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

[[nodiscard]] bool writePdf(const QString& path, const QStringList& pages) {
    if (pages.isEmpty() || !QDir{}.mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }
    QPdfWriter writer(path);
    writer.setPageSize(QPageSize(QPageSize::Letter));
    writer.setResolution(72);
    QPainter painter(&writer);
    if (!painter.isActive()) {
        return false;
    }
    painter.setFont(QFont(QStringLiteral("DejaVu Sans"), 12));
    for (qsizetype index = 0; index < pages.size(); ++index) {
        painter.drawText(QRect(40, 40, 520, 700), Qt::AlignCenter, pages.at(index));
        if (index + 1 < pages.size() && !writer.newPage()) {
            return false;
        }
    }
    return painter.end();
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

[[nodiscard]] QString createSealedTwinsPack(const QString& root) {
    const auto destination = QDir(root).filePath(QStringLiteral("sealed-twins-pack"));
    if (!copyTree(fixture(QStringLiteral("full-resource-pack-v2")), destination)) {
        return {};
    }
    const auto record_path = QDir(destination).filePath(QStringLiteral("resources/record.json"));
    const auto manifest_path = QDir(destination).filePath(QStringLiteral("manifest.json"));
    auto record = readObject(record_path);
    auto manifest = readObject(manifest_path);
    if (!record || !manifest) {
        return {};
    }

    const auto sealed_asset_path = QStringLiteral("objects/sealed-psr.pdf");
    const auto sealed_file_path = QDir(destination).filePath(sealed_asset_path);
    if (!writePdf(sealed_file_path, {QStringLiteral("Sealed desktop PSR cover"),
                                     QStringLiteral("ultrasecret-main-window-text")})) {
        return {};
    }
    QFile sealed_file(sealed_file_path);
    if (!sealed_file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const auto sealed_bytes = sealed_file.readAll();
    sealed_file.close();
    const auto sealed_digest = QString::fromLatin1(
        QCryptographicHash::hash(sealed_bytes, QCryptographicHash::Sha256).toHex());

    auto entries = record->value(QStringLiteral("docket_entries")).toArray();
    auto sealed = entries.at(0).toObject();
    sealed.insert(QStringLiteral("entry_id"), QStringLiteral("example.record.psr-sealed"));
    sealed.insert(QStringLiteral("entry_number"), 3);
    sealed.insert(QStringLiteral("entry_label"), QStringLiteral("ECF No. 42-S"));
    sealed.insert(QStringLiteral("title"), QStringLiteral("Confidential PSR title"));
    sealed.insert(QStringLiteral("description"),
                  QStringLiteral("Never disclose desktop PSR description"));
    sealed.insert(QStringLiteral("tags"), QJsonArray{QStringLiteral("psr-secret-desktop-tag")});
    sealed.insert(QStringLiteral("asset_path"), sealed_asset_path);
    sealed.insert(QStringLiteral("asset_sha256"), sealed_digest);
    sealed.insert(QStringLiteral("page_count"), 2);
    sealed.insert(QStringLiteral("sealed"), true);
    entries.push_back(sealed);
    record->insert(QStringLiteral("docket_entries"), entries);
    auto anchors = record->value(QStringLiteral("page_anchors")).toArray();
    anchors.push_back(QJsonObject{
        {QStringLiteral("anchor_id"), QStringLiteral("example.record.anchor.psr-sealed")},
        {QStringLiteral("entry_id"), QStringLiteral("example.record.psr-sealed")},
        {QStringLiteral("page_number"), 2},
        {QStringLiteral("citation_label"), QStringLiteral("SECRET-JA-2")},
    });
    record->insert(QStringLiteral("page_anchors"), anchors);
    record->insert(
        QStringLiteral("disclosure_policy"),
        QJsonObject{
            {QStringLiteral("policy_id"), QStringLiteral("example.record.policy.psr")},
            {QStringLiteral("unauthorized_projection"), QStringLiteral("public_counterparts_only")},
            {QStringLiteral("authorized_projection"),
             QStringLiteral("public_and_authorized_sealed")},
            {QStringLiteral("sealed_asset_access"), QStringLiteral("session_event_grant_required")},
        });
    record->insert(
        QStringLiteral("sealed_disclosures"),
        QJsonArray{QJsonObject{
            {QStringLiteral("disclosure_id"), QStringLiteral("example.disclosure.psr")},
            {QStringLiteral("sealed_entry_id"), QStringLiteral("example.record.psr-sealed")},
            {QStringLiteral("public_entry_id"), QStringLiteral("example.record.entry-one")},
            {QStringLiteral("authorization_authority_id"),
             QStringLiteral("example.authority.deficiency")},
            {QStringLiteral("required_items"), QJsonArray{QStringLiteral("redacted_counterpart")}},
            {QStringLiteral("anchor_mappings"),
             QJsonArray{QJsonObject{
                 {QStringLiteral("stable_anchor_id"),
                  QStringLiteral("example.record.anchor.psr-stable")},
                 {QStringLiteral("sealed_anchor_id"),
                  QStringLiteral("example.record.anchor.psr-sealed")},
                 {QStringLiteral("public_anchor_id"), QStringLiteral("example.record.anchor.ja2")},
             }}},
        }});
    const auto record_digest = writeObject(record_path, *record);
    if (!record_digest) {
        return {};
    }
    auto contents = manifest->value(QStringLiteral("contents")).toArray();
    bool updated_record = false;
    for (qsizetype index = 0; index < contents.size(); ++index) {
        auto item = contents.at(index).toObject();
        if (item.value(QStringLiteral("path")).toString() !=
            QStringLiteral("resources/record.json")) {
            continue;
        }
        item.insert(QStringLiteral("sha256"), *record_digest);
        contents.replace(index, item);
        updated_record = true;
        break;
    }
    if (!updated_record) {
        return {};
    }
    manifest->insert(QStringLiteral("contents"), contents);
    auto blobs = manifest->value(QStringLiteral("blobs")).toArray();
    blobs.push_back(QJsonObject{
        {QStringLiteral("path"), sealed_asset_path},
        {QStringLiteral("media_type"), QStringLiteral("application/pdf")},
        {QStringLiteral("byte_size"), sealed_bytes.size()},
        {QStringLiteral("sha256"), sealed_digest},
    });
    manifest->insert(QStringLiteral("blobs"), blobs);
    auto capabilities = manifest->value(QStringLiteral("required_capabilities")).toArray();
    capabilities.push_back(QJsonObject{
        {QStringLiteral("id"), QStringLiteral("workbench.pack.sealed-record-twins")},
        {QStringLiteral("version"), 1},
    });
    manifest->insert(QStringLiteral("required_capabilities"), capabilities);
    return writeObject(manifest_path, *manifest).has_value() ? destination : QString{};
}

[[nodiscard]] bool executeSql(const QString& database_path, const QString& statement) {
    const auto connection = QStringLiteral("main-window-record-access-%1")
                                .arg(QUuid::createUuid().toString(QUuid::Id128));
    bool succeeded = false;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(database_path);
        succeeded = database.open();
        if (succeeded) {
            QSqlQuery query(database);
            succeeded = query.exec(statement) && query.numRowsAffected() == 1;
        }
        database.close();
        database = QSqlDatabase{};
    }
    QSqlDatabase::removeDatabase(connection);
    return succeeded;
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
    QVERIFY(window.benchSummaryLabel()->text().contains(QStringLiteral("Composite Jurist Rowan")));
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

void MainWindowTest::installedRecordActionOpensSearchablePdf() {
    QTemporaryDir state;
    QVERIFY(state.isValid());
    const auto archive_path = QDir(state.path()).filePath(QStringLiteral("fixture.awpack"));
    const auto exported =
        PackArchive::exportDirectory(fixture(QStringLiteral("full-resource-pack")), archive_path);
    if (!exported) {
        QFAIL(qPrintable(exported.error().message));
    }

    const auto catalog = QDir(state.path()).filePath(QStringLiteral("catalog"));
    MainWindow window({}, catalog);
    QVERIFY(!window.openRecordAction()->isEnabled());
    const auto installed = window.loadSource(archive_path);
    if (!installed) {
        QFAIL(qPrintable(installed.error()));
    }
    window.caseList()->setCurrentRow(-1);
    QVERIFY(!window.openRecordAction()->isEnabled());
    window.caseList()->setCurrentRow(0);
    QVERIFY(window.openRecordAction()->isEnabled());
    QCOMPARE(window.openRecordAction()->shortcut(), QKeySequence(QStringLiteral("Ctrl+R")));
    QVERIFY(!window.openRecordAction()->property("accessibleName").toString().isEmpty());
    QVERIFY(!window.workspaceTabs()->accessibleName().isEmpty());

    QVERIFY(QFile::remove(archive_path));
    window.openRecordAction()->trigger();
    QVERIFY2(window.errorLabel()->text().isEmpty(), qPrintable(window.errorLabel()->text()));

    auto* workspace = window.recordWorkspace();
    QVERIFY(workspace != nullptr);
    QCOMPARE(window.workspaceTabs()->currentWidget(), workspace);
    QVERIFY(window.workspaceTabs()->isTabEnabled(window.workspaceTabs()->indexOf(workspace)));
    QCOMPARE(workspace->currentDocumentId(), QStringLiteral("example.record.entry-one"));
    QCOMPARE(workspace->loadedPageCount(), 3);
    QVERIFY(!workspace->accessibleName().isEmpty());
    workspace->setDocumentSearch(QStringLiteral("Fictional Final Order - Page 2"));
    QTRY_COMPARE_WITH_TIMEOUT(workspace->documentSearchResultCount(), 1, 10'000);
}

void MainWindowTest::recordFailuresPreserveLastGoodWorkspace() {
    QTemporaryDir state;
    QVERIFY(state.isValid());
    const auto archive_path = QDir(state.path()).filePath(QStringLiteral("fixture.awpack"));
    const auto exported =
        PackArchive::exportDirectory(fixture(QStringLiteral("full-resource-pack")), archive_path);
    if (!exported) {
        QFAIL(qPrintable(exported.error().message));
    }

    const auto catalog = QDir(state.path()).filePath(QStringLiteral("catalog"));
    MainWindow window({}, catalog);
    const auto installed = window.loadSource(archive_path);
    if (!installed) {
        QFAIL(qPrintable(installed.error()));
    }
    const auto opened = window.openSelectedRecord();
    if (!opened) {
        QFAIL(qPrintable(opened.error()));
    }
    auto* const last_good_workspace = window.recordWorkspace();
    QCOMPARE(last_good_workspace->loadedPageCount(), 3);
    const auto last_good_document = last_good_workspace->currentDocumentId();

    const auto& entry = window.currentRuntime()->cases.front().record.docket_entries.front();
    const auto object_path = QDir(catalog).filePath(QStringLiteral("blobs/") +
                                                    QString::fromStdString(entry.asset_sha256));
    QVERIFY(QFileInfo(object_path).isFile());
    QDir archives(QDir(catalog).filePath(QStringLiteral("archives")));
    const auto installed_archives =
        archives.entryList({QStringLiteral("*.awpack")}, QDir::Files | QDir::NoSymLinks);
    QCOMPARE(installed_archives.size(), 1);
    QVERIFY(QFile::remove(archives.filePath(installed_archives.front())));

    QFile corrupt_object(object_path);
    QVERIFY(corrupt_object.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(corrupt_object.write("not a valid installed PDF"), qint64{25});
    corrupt_object.close();
    const auto corrupt_result = window.openSelectedRecord();
    QVERIFY(!corrupt_result.has_value());
    QVERIFY(corrupt_result.error().contains(QStringLiteral("could not be materialized")));
    QCOMPARE(window.recordWorkspace(), last_good_workspace);
    QCOMPARE(last_good_workspace->loadedPageCount(), 3);
    QCOMPARE(last_good_workspace->currentDocumentId(), last_good_document);
    QVERIFY(
        window.workspaceTabs()->isTabEnabled(window.workspaceTabs()->indexOf(last_good_workspace)));
    QVERIFY(!window.errorLabel()->isHidden());
    QVERIFY(window.errorLabel()->text().contains(QStringLiteral("Installed record")));

    QVERIFY(QFile::remove(object_path));
    const auto missing_result = window.openSelectedRecord();
    QVERIFY(!missing_result.has_value());
    QVERIFY(missing_result.error().contains(QStringLiteral("could not be materialized")));
    QCOMPARE(window.recordWorkspace(), last_good_workspace);
    QCOMPARE(last_good_workspace->loadedPageCount(), 3);
    QCOMPARE(last_good_workspace->currentDocumentId(), last_good_document);
}

void MainWindowTest::sealedRecordAccessPersistsAndRejectsTamperedReplay() {
    QTemporaryDir state;
    QVERIFY(state.isValid());
    const auto source = createSealedTwinsPack(state.path());
    QVERIFY2(!source.isEmpty(), "Failed to create schema-v2 sealed-twins fixture");
    const auto archive_path = QDir(state.path()).filePath(QStringLiteral("sealed-twins.awpack"));
    const auto exported = PackArchive::exportDirectory(source, archive_path);
    QVERIFY2(exported.has_value(), exported ? "" : qPrintable(exported.error().message));
    const auto database_path =
        QDir(state.path()).filePath(QStringLiteral("sessions/record-access.sqlite"));
    auto provider = std::make_shared<RecordingRecordAccessProvider>();

    {
        MainWindow authoring(
            source, QDir(state.path()).filePath(QStringLiteral("authoring-catalog")), nullptr, {},
            provider,
            QDir(state.path()).filePath(QStringLiteral("authoring-record-access.sqlite")));
        QVERIFY(authoring.currentRuntime() != nullptr);
        QVERIFY(!authoring.openRecordAction()->isEnabled());
        QVERIFY(!authoring.recordAccessMenu()->isEnabled());
        QVERIFY(authoring.recordAccessMenu()->actions().empty());
    }
    QVERIFY(provider->created_sessions.empty());

    const auto catalog = QDir(state.path()).filePath(QStringLiteral("installed-catalog"));
    QString session_id;
    {
        MainWindow window({}, catalog, nullptr, {}, provider, database_path);
        const auto installed = window.loadSource(archive_path);
        QVERIFY2(installed.has_value(), installed ? "" : qPrintable(installed.error()));
        const auto opened = window.openSelectedRecord();
        QVERIFY2(opened.has_value(), opened ? "" : qPrintable(opened.error()));
        QCOMPARE(provider->created_sessions.size(), std::size_t{1});
        session_id = provider->created_sessions.front();
        QVERIFY(!session_id.isEmpty());

        auto* grant = window.findChild<QAction*>(
            QStringLiteral("grantRecordAccessAction.example.disclosure.psr"));
        auto* revoke = window.findChild<QAction*>(
            QStringLiteral("revokeRecordAccessAction.example.disclosure.psr"));
        QVERIFY(grant != nullptr);
        QVERIFY(revoke != nullptr);
        QVERIFY(window.recordAccessMenu()->isEnabled());
        QVERIFY(grant->isEnabled());
        QVERIFY(!revoke->isEnabled());
        QVERIFY(grant->property("accessibleName")
                    .toString()
                    .contains(QStringLiteral("example.disclosure.psr")));

        QString public_surface = window.recordSummaryLabel()->text();
        for (const auto* label : window.findChildren<QLabel*>()) {
            public_surface += label->text();
            public_surface += label->accessibleName();
        }
        for (const auto* action : window.findChildren<QAction*>()) {
            public_surface += action->text();
            public_surface += action->statusTip();
            public_surface += action->property("accessibleName").toString();
        }
        for (const auto& secret :
             {QStringLiteral("Confidential PSR"), QStringLiteral("psr-secret-desktop-tag"),
              QStringLiteral("SECRET-JA-2"), QStringLiteral("example.record.psr-sealed")}) {
            QVERIFY2(!public_surface.contains(secret), qPrintable(secret));
        }
        QVERIFY(!window.recordSummaryLabel()->text().contains(QStringLiteral("sealed"),
                                                              Qt::CaseInsensitive));

        auto* workspace = window.recordWorkspace();
        QVERIFY(workspace != nullptr);
        QCOMPARE(workspace->currentDocumentId(), QStringLiteral("example.record.entry-one"));
        for (const auto& query :
             {QStringLiteral("Confidential PSR"), QStringLiteral("psr-secret-desktop-tag"),
              QStringLiteral("SECRET-JA-2")}) {
            workspace->setDocketFilter(query);
            QCOMPARE(workspace->visibleDocketCount(), qsizetype{0});
        }
        workspace->setDocketFilter({});
        QVERIFY(!workspace->navigateToAnchor(QStringLiteral("example.record.anchor.psr-sealed"))
                     .has_value());
        QVERIFY(workspace->navigateToAnchor(QStringLiteral("example.record.anchor.psr-stable"))
                    .has_value());
        QCOMPARE(workspace->currentDocumentId(), QStringLiteral("example.record.entry-one"));

        const auto& sealed_entry =
            window.currentRuntime()->cases.front().record.docket_entries.back();
        QVERIFY(sealed_entry.sealed);
        const auto sealed_object = QDir(catalog).filePath(
            QStringLiteral("blobs/") + QString::fromStdString(sealed_entry.asset_sha256));
        QFile sealed_input(sealed_object);
        QVERIFY(sealed_input.open(QIODevice::ReadOnly));
        const auto sealed_bytes = sealed_input.readAll();
        sealed_input.close();
        QDir installed_archives(QDir(catalog).filePath(QStringLiteral("archives")));
        const auto archives = installed_archives.entryList({QStringLiteral("*.awpack")},
                                                           QDir::Files | QDir::NoSymLinks);
        QCOMPARE(archives.size(), 1);
        const auto installed_archive_path = installed_archives.filePath(archives.front());
        auto second_catalog = appellate::packs::PackCatalog::open(catalog);
        QVERIFY(second_catalog.has_value());
        const auto second_resolved =
            (*second_catalog)->loadResolved(window.currentRuntime()->revision);
        QVERIFY2(second_resolved.has_value(),
                 second_resolved ? "" : qPrintable(second_resolved.error().message));
        QVERIFY(QFile::remove(installed_archive_path));
        QVERIFY(QFile::remove(sealed_object));

        grant->trigger();
        QCOMPARE(provider->transitions.size(), std::size_t{1});
        QCOMPARE(provider->transitions.front().sequence, std::uint64_t{1});
        QCOMPARE(provider->transitions.front().disclosure_id,
                 std::string("example.disclosure.psr"));
        QVERIFY(!grant->isEnabled());
        QVERIFY(revoke->isEnabled());
        const auto missing =
            workspace->navigateToAnchor(QStringLiteral("example.record.anchor.psr-stable"));
        QVERIFY(!missing.has_value());
        QCOMPARE(missing.error().code, appellate::ui::RecordWorkspaceErrorCode::PdfLoadFailed);
        QVERIFY(workspace->openDocketEntry(QStringLiteral("example.record.entry-one")).has_value());

        QFile restored(sealed_object);
        QVERIFY(restored.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(restored.write(sealed_bytes), static_cast<qint64>(sealed_bytes.size()));
        restored.close();
        const auto incomplete_catalog =
            workspace->navigateToAnchor(QStringLiteral("example.record.anchor.psr-stable"));
        QVERIFY(!incomplete_catalog.has_value());
        QCOMPARE(incomplete_catalog.error().code,
                 appellate::ui::RecordWorkspaceErrorCode::PdfLoadFailed);
        QVERIFY(QFile::copy(archive_path, installed_archive_path));
        QVERIFY(workspace->navigateToAnchor(QStringLiteral("example.record.anchor.psr-stable"))
                    .has_value());
        QCOMPARE(workspace->currentDocumentId(), QStringLiteral("example.record.psr-sealed"));
        QCOMPARE(workspace->loadedPageCount(), 2);
        workspace->setDocumentSearch(QStringLiteral("ultrasecret-main-window-text"));
        QTRY_COMPARE_WITH_TIMEOUT(workspace->documentSearchResultCount(), 1, 10'000);

        auto corrupt_bytes = sealed_bytes;
        corrupt_bytes[corrupt_bytes.size() / 2] =
            corrupt_bytes.at(corrupt_bytes.size() / 2) == 'x' ? 'y' : 'x';
        QVERIFY(restored.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(restored.write(corrupt_bytes), static_cast<qint64>(corrupt_bytes.size()));
        restored.close();
        workspace->setDocumentSearch(QStringLiteral("ultrasecret-main-window-text"));
        QTRY_COMPARE_WITH_TIMEOUT(workspace->documentSearchResultCount(), 1, 10'000);

        const auto independent_store = appellate::storage::SessionStore::open(database_path);
        QVERIFY(!independent_store.has_value());
        QCOMPARE(independent_store.error().code, appellate::storage::StoreErrorCode::StateInUse);
        auto second_store = appellate::ui::MainWindowTestAccess::forkRecordAccessConnection(window);
        QVERIFY(second_store.has_value());
        auto second_controller = appellate::app::RecordAccessSessionController::reopen(
            session_id, window.currentRuntime()->cases.front().definition.id,
            std::move(*second_store), QStringLiteral("engine.record-access.v1"), *second_resolved);
        QVERIFY2(second_controller.has_value(),
                 second_controller ? "" : qPrintable(second_controller.error().message));
        const auto external_revoke =
            (*second_controller)
                ->revoke("example.disclosure.psr", "test.record.access.external-revoke",
                         QStringLiteral("2026-08-11T09:00:02Z"));
        QVERIFY2(external_revoke.has_value(),
                 external_revoke ? "" : qPrintable(external_revoke.error().message));

        // This action is stale in A. Its pre-transition reconciliation must
        // apply B's revoke before the redundant local command fails.
        revoke->trigger();
        QCOMPARE(provider->transitions.size(), std::size_t{2});
        QCOMPARE(provider->transitions.back().sequence, std::uint64_t{3});
        QVERIFY(workspace->currentDocumentId().isEmpty());
        QCOMPARE(workspace->loadedPageCount(), 0);
        QCOMPARE(workspace->documentSearchResultCount(), 0);
        QVERIFY(grant->isEnabled());
        QVERIFY(!revoke->isEnabled());

        // A revoked controller has no exportable authorization token. Opening
        // a fresh workspace can apply only the controller's current live head,
        // never the previously granted state.
        const auto reopened_after_revoke = window.openSelectedRecord();
        QVERIFY2(reopened_after_revoke.has_value(),
                 reopened_after_revoke ? "" : qPrintable(reopened_after_revoke.error()));
        workspace = window.recordWorkspace();
        QVERIFY(workspace != nullptr);
        QVERIFY(!workspace->navigateToAnchor(QStringLiteral("example.record.anchor.psr-sealed"))
                     .has_value());
        QVERIFY(workspace->navigateToAnchor(QStringLiteral("example.record.anchor.psr-stable"))
                    .has_value());
        QCOMPARE(workspace->currentDocumentId(), QStringLiteral("example.record.entry-one"));
        grant = window.findChild<QAction*>(
            QStringLiteral("grantRecordAccessAction.example.disclosure.psr"));
        revoke = window.findChild<QAction*>(
            QStringLiteral("revokeRecordAccessAction.example.disclosure.psr"));
        QVERIFY(grant != nullptr);
        QVERIFY(revoke != nullptr);
        QVERIFY(grant->isEnabled());
        QVERIFY(!revoke->isEnabled());

        grant->trigger();
        QCOMPARE(provider->transitions.size(), std::size_t{3});
        const auto corrupt =
            workspace->navigateToAnchor(QStringLiteral("example.record.anchor.psr-stable"));
        QVERIFY(!corrupt.has_value());
        QCOMPARE(corrupt.error().code, appellate::ui::RecordWorkspaceErrorCode::PdfLoadFailed);
        QVERIFY(restored.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(restored.write(sealed_bytes), static_cast<qint64>(sealed_bytes.size()));
        restored.close();
        QVERIFY(workspace->navigateToAnchor(QStringLiteral("example.record.anchor.psr-stable"))
                    .has_value());
    }

    QCOMPARE(provider->created_sessions.size(), std::size_t{1});
    QCOMPARE(provider->transitions.size(), std::size_t{3});
    {
        MainWindow reopened({}, catalog, nullptr, {}, provider, database_path);
        const auto installed = reopened.loadSource(archive_path);
        QVERIFY2(installed.has_value(), installed ? "" : qPrintable(installed.error()));
        const auto opened = reopened.openSelectedRecord();
        QVERIFY2(opened.has_value(), opened ? "" : qPrintable(opened.error()));
        QCOMPARE(provider->created_sessions.size(), std::size_t{1});
        auto* grant = reopened.findChild<QAction*>(
            QStringLiteral("grantRecordAccessAction.example.disclosure.psr"));
        auto* revoke = reopened.findChild<QAction*>(
            QStringLiteral("revokeRecordAccessAction.example.disclosure.psr"));
        QVERIFY(grant != nullptr);
        QVERIFY(revoke != nullptr);
        QVERIFY(!grant->isEnabled());
        QVERIFY(revoke->isEnabled());
        auto* const last_good = reopened.recordWorkspace();
        QVERIFY(last_good->navigateToAnchor(QStringLiteral("example.record.anchor.psr-stable"))
                    .has_value());
        QCOMPARE(last_good->currentDocumentId(), QStringLiteral("example.record.psr-sealed"));
        const auto last_good_document = last_good->currentDocumentId();
        auto* const last_good_revoke = revoke;

        QVERIFY(executeSql(database_path,
                           QStringLiteral("UPDATE command_log SET recorded_at_utc = "
                                          "'2026-08-11T09:00:59Z' WHERE session_id = '%1' AND "
                                          "expected_sequence = 0")
                               .arg(session_id)));
        const auto tampered = reopened.openSelectedRecord();
        QVERIFY(!tampered.has_value());
        QVERIFY(tampered.error().contains(QStringLiteral("exact replay")));
        QCOMPARE(reopened.recordWorkspace(), last_good);
        QCOMPARE(last_good->currentDocumentId(), last_good_document);
        QCOMPARE(reopened.workspaceTabs()->currentWidget(), last_good);
        QCOMPARE(reopened.findChild<QAction*>(
                     QStringLiteral("revokeRecordAccessAction.example.disclosure.psr")),
                 last_good_revoke);
        QVERIFY(last_good_revoke->isEnabled());
        QVERIFY(reopened.recordAccessMenu()->isEnabled());
    }
}

void MainWindowTest::argumentLaunchUsesExactSelectedConfigurationAndPreservesWorkspaceOnError() {
    QTemporaryDir state;
    QVERIFY(state.isValid());
    const auto archive_path = QDir(state.path()).filePath(QStringLiteral("fixture-v2.awpack"));
    const auto exported = PackArchive::exportDirectory(
        fixture(QStringLiteral("full-resource-pack-v2")), archive_path);
    if (!exported) {
        QFAIL(qPrintable(exported.error().message));
    }

    auto provider = std::make_shared<RecordingLaunchProvider>();
    MainWindow window({}, QDir(state.path()).filePath(QStringLiteral("catalog")), nullptr,
                      provider);
    const auto installed = window.loadSource(archive_path);
    if (!installed) {
        QFAIL(qPrintable(installed.error()));
    }
    QVERIFY(window.currentRuntime() != nullptr);
    QCOMPARE(window.currentRuntime()->cases.size(), std::size_t{1});
    const auto& runtime_case = window.currentRuntime()->cases.front();
    QCOMPARE(runtime_case.argument_configurations.size(), std::size_t{2});
    QCOMPARE(window.argumentConfigurationSelector()->count(), 2);
    QVERIFY(
        window.argumentConfigurationSelector()->accessibleName().contains(QStringLiteral("Exact")));
    int actual_index = -1;
    int counterfactual_index = -1;
    for (int index = 0; index < window.argumentConfigurationSelector()->count(); ++index) {
        const auto& configuration =
            runtime_case.argument_configurations.at(static_cast<std::size_t>(index));
        if (configuration.grounded_question_bank->mode ==
            appellate::model::OralArgumentMode::ActualRecord) {
            actual_index = index;
            QVERIFY(window.argumentConfigurationSelector()->itemText(index).contains(
                QStringLiteral("actual record")));
        } else {
            counterfactual_index = index;
            QVERIFY(window.argumentConfigurationSelector()->itemText(index).contains(
                QStringLiteral("counterfactual training")));
        }
    }
    QVERIFY(actual_index >= 0);
    QVERIFY(counterfactual_index >= 0);
    window.argumentConfigurationSelector()->setCurrentIndex(actual_index);
    QVERIFY(window.openOralArgumentAction()->isEnabled());
    QVERIFY(!window.openOralArgumentAction()->property("accessibleName").toString().isEmpty());
    QCOMPARE(window.openOralArgumentAction()->shortcut(),
             QKeySequence(QStringLiteral("Ctrl+Shift+A")));

    auto* const placeholder = window.oralArgumentWorkspace();
    QVERIFY(placeholder != nullptr);
    QVERIFY(!placeholder->isReady());
    const auto actual = window.openSelectedOralArgument();
    QVERIFY(!actual.has_value());
    QVERIFY(actual.error().contains(QStringLiteral("Injected provider refusal")));
    QCOMPARE(provider->calls.size(), std::size_t{1});
    QCOMPARE(provider->calls.front().root_revision, *exported);
    QCOMPARE(provider->calls.front().case_id, runtime_case.definition.id);
    QCOMPARE(provider->calls.front().argument_configuration_id,
             runtime_case.argument_configurations.at(static_cast<std::size_t>(actual_index)).id);
    QVERIFY(provider->calls.front().configuration_owner.has_value());
    QCOMPARE(*provider->calls.front().configuration_owner, *exported);
    QCOMPARE(window.oralArgumentWorkspace(), placeholder);
    QVERIFY(!window.workspaceTabs()->isTabEnabled(
        window.workspaceTabs()->indexOf(window.oralArgumentWorkspace())));

    window.argumentConfigurationSelector()->setCurrentIndex(counterfactual_index);
    QVERIFY(window.openOralArgumentAction()->isEnabled());
    const auto counterfactual = window.openSelectedOralArgument();
    QVERIFY(!counterfactual.has_value());
    QCOMPARE(provider->calls.size(), std::size_t{2});
    QCOMPARE(provider->calls.back().case_id, runtime_case.definition.id);
    QCOMPARE(
        provider->calls.back().argument_configuration_id,
        runtime_case.argument_configurations.at(static_cast<std::size_t>(counterfactual_index)).id);
    QVERIFY(provider->calls.back().argument_configuration_id !=
            provider->calls.front().argument_configuration_id);
    QCOMPARE(runtime_case.argument_configurations.at(static_cast<std::size_t>(actual_index))
                 .grounded_question_bank->mode,
             appellate::model::OralArgumentMode::ActualRecord);
    QCOMPARE(runtime_case.argument_configurations.at(static_cast<std::size_t>(counterfactual_index))
                 .grounded_question_bank->mode,
             appellate::model::OralArgumentMode::CounterfactualTraining);
    QCOMPARE(window.oralArgumentWorkspace(), placeholder);
    QVERIFY(window.errorLabel()->text().contains(QStringLiteral("Injected provider refusal")));
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
    QVERIFY(!window.openRecordAction()->isEnabled());
    QVERIFY(!window.openOralArgumentAction()->isEnabled());

    const std::array actions{
        window.openDirectoryAction(),    window.installArchiveAction(),
        window.importProfileAction(),    window.cloneProfileAction(),
        window.exportProfileAction(),    window.openRecordAction(),
        window.openOralArgumentAction(),
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
    QVERIFY(!window.openRecordAction()->isEnabled());
    QVERIFY(!window.openOralArgumentAction()->isEnabled());
    QVERIFY(!window.caseList()->accessibleName().isEmpty());
    QVERIFY(!window.argumentConfigurationSelector()->accessibleName().isEmpty());
    QVERIFY(!window.profileSelector()->accessibleName().isEmpty());
    QVERIFY(window.caseList()->focusPolicy() != Qt::NoFocus);
    QVERIFY(window.profileSelector()->focusPolicy() != Qt::NoFocus);
    const auto* boundary = window.findChild<QLabel*>(QStringLiteral("oralArgumentLaunchBoundary"));
    QVERIFY(boundary != nullptr);
    QVERIFY(boundary->text().contains(QStringLiteral("disabled")));
    QVERIFY(boundary->text().contains(QStringLiteral("never invents")));

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
    QVERIFY(!unavailable.openOralArgumentAction()->isEnabled());
}

} // namespace

QTEST_MAIN(MainWindowTest)

#include "tst_main_window.moc"
