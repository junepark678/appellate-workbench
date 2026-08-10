#include "appellate/packs/pack_archive.hpp"
#include "appellate/packs/pack_catalog.hpp"
#include "appellate/packs/runtime_pack.hpp"
#include "installed_record_controller.hpp"
#include "record_workspace.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QPdfDocument>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>

namespace {

namespace app = appellate::app;
namespace model = appellate::model;
namespace packs = appellate::packs;
namespace ui = appellate::ui;

[[nodiscard]] QString fullPackPath() {
    return QStringLiteral(APPELLATE_TEST_FIXTURES) + QStringLiteral("/full-resource-pack");
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

struct InstalledFixture final {
    QTemporaryDir temporary;
    QString staging_root;
    packs::InstalledPack installed;
    std::unique_ptr<packs::PackCatalog> catalog;
    packs::LoadedPack loaded;
    packs::RuntimePack runtime;

    [[nodiscard]] std::optional<QString> initialize() {
        if (!temporary.isValid()) {
            return QStringLiteral("cannot create temporary root");
        }
        staging_root = QDir(temporary.path()).filePath(QStringLiteral("ephemeral-staging"));
        const auto source = QDir(staging_root).filePath(QStringLiteral("source"));
        const auto archive = QDir(staging_root).filePath(QStringLiteral("full.awpack"));
        if (!copyTree(fullPackPath(), source)) {
            return QStringLiteral("cannot stage full-resource-pack fixture");
        }
        const auto exported = packs::PackArchive::exportDirectory(source, archive);
        if (!exported) {
            return exported.error().message;
        }
        auto opened = packs::PackCatalog::open(
            QDir(temporary.path()).filePath(QStringLiteral("installed-catalog")));
        if (!opened) {
            return opened.error().message;
        }
        catalog = std::move(*opened);
        const auto installation =
            catalog->installArchive(archive, QStringLiteral("2026-08-11T00:00:00Z"));
        if (!installation) {
            return installation.error().message;
        }
        installed = *installation;
        if (!QDir(staging_root).removeRecursively() || QFileInfo::exists(staging_root)) {
            return QStringLiteral("ephemeral authoring and archive staging survived installation");
        }
        const auto catalog_pack = catalog->load(exported->id, exported->version);
        if (!catalog_pack) {
            return catalog_pack.error().message;
        }
        loaded = *catalog_pack;
        const auto parsed = packs::loadRuntimePack(loaded);
        if (!parsed) {
            return QString::fromStdString(parsed.error().message);
        }
        runtime = *parsed;
        return std::nullopt;
    }

    [[nodiscard]] const model::CaseId& caseId() const {
        return runtime.cases.front().definition.id;
    }
};

class InstalledRecordWorkspaceTest final : public QObject {
    Q_OBJECT

  private slots:
    void loadsSearchablePdfOnlyFromInstalledCatalog();
    void rejectsRevisionAndAssetDigestMismatch();
    void rejectsMissingBlobWrongPageCountAndOrphanRecord();
};

void InstalledRecordWorkspaceTest::loadsSearchablePdfOnlyFromInstalledCatalog() {
    InstalledFixture fixture;
    const auto error = fixture.initialize();
    QVERIFY2(!error.has_value(), error ? qPrintable(*error) : "");
    QVERIFY(!QFileInfo::exists(fixture.staging_root));

    ui::RecordWorkspace workspace;
    app::InstalledRecordController controller(*fixture.catalog, workspace);
    const auto loaded = controller.load(fixture.loaded, fixture.runtime, fixture.caseId());
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error().message));
    QCOMPARE(loaded->revision, fixture.runtime.revision);
    QCOMPARE(loaded->record_id.value, std::string("example.record.fictional"));
    QCOMPARE(loaded->assets.size(), std::size_t{1});
    QCOMPARE(loaded->assets.front().descriptor.sha256,
             std::string("bab85fe6529e9832b26196e8f08448b02bbe79e5ae4d4d37d104b278e11f1366"));
    QCOMPARE(loaded->assets.front().page_count, 3);
    QVERIFY(loaded->assets.front().local_path.startsWith(fixture.catalog->blobObjectsDirectory()));
    QVERIFY(!loaded->assets.front().local_path.startsWith(fullPackPath()));

    QCOMPARE(loaded->definition.documents.size(), std::size_t{1});
    QCOMPARE(loaded->definition.docket.size(), std::size_t{1});
    QCOMPARE(loaded->definition.documents.front().id, QStringLiteral("example.record.entry-one"));
    QCOMPARE(loaded->definition.docket.front().document_id,
             loaded->definition.documents.front().id);
    QCOMPARE(loaded->definition.docket.front().filed_on, QDate(2026, 1, 2));
    QCOMPARE(
        loaded->definition.documents.front().metadata.value(QStringLiteral("declared_page_count")),
        QStringLiteral("3"));

    QPdfDocument exact_text_probe;
    QCOMPARE(exact_text_probe.load(loaded->assets.front().local_path), QPdfDocument::Error::None);
    QCOMPARE(exact_text_probe.status(), QPdfDocument::Status::Ready);
    QCOMPARE(exact_text_probe.pageCount(), 3);
    QCOMPARE(exact_text_probe.getAllText(1).text().simplified(),
             QStringLiteral("Fictional Final Order - Page 2"));

    QVERIFY(workspace.openDocketEntry(QStringLiteral("example.record.entry-one")).has_value());
    QCOMPARE(workspace.loadedPageCount(), 3);
    workspace.setDocumentSearch(QStringLiteral("Fictional Final Order - Page 2"));
    QTRY_COMPARE_WITH_TIMEOUT(workspace.documentSearchResultCount(), 1, 10'000);
}

void InstalledRecordWorkspaceTest::rejectsRevisionAndAssetDigestMismatch() {
    InstalledFixture fixture;
    const auto error = fixture.initialize();
    QVERIFY2(!error.has_value(), error ? qPrintable(*error) : "");

    ui::RecordWorkspace workspace;
    app::InstalledRecordController controller(*fixture.catalog, workspace);

    auto wrong_revision = fixture.runtime;
    wrong_revision.revision.digest = std::string(64, '0');
    const auto revision_result = controller.load(fixture.loaded, wrong_revision, fixture.caseId());
    QVERIFY(!revision_result.has_value());
    QCOMPARE(revision_result.error().code, app::InstalledRecordErrorCode::RevisionMismatch);
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{0});

    auto wrong_asset = fixture.runtime;
    wrong_asset.cases.front().record.docket_entries.front().asset_sha256 = std::string(64, '0');
    const auto digest_result = controller.load(fixture.loaded, wrong_asset, fixture.caseId());
    QVERIFY(!digest_result.has_value());
    QCOMPARE(digest_result.error().code, app::InstalledRecordErrorCode::AssetDigestMismatch);
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{0});
}

void InstalledRecordWorkspaceTest::rejectsMissingBlobWrongPageCountAndOrphanRecord() {
    InstalledFixture fixture;
    const auto error = fixture.initialize();
    QVERIFY2(!error.has_value(), error ? qPrintable(*error) : "");

    ui::RecordWorkspace workspace;
    app::InstalledRecordController controller(*fixture.catalog, workspace);

    auto wrong_pages = fixture.runtime;
    wrong_pages.cases.front().record.docket_entries.front().page_count = 2;
    const auto page_result = controller.load(fixture.loaded, wrong_pages, fixture.caseId());
    QVERIFY(!page_result.has_value());
    QCOMPARE(page_result.error().code, app::InstalledRecordErrorCode::PageCountMismatch);
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{0});

    auto orphaned = fixture.loaded;
    std::erase_if(orphaned.resources, [](const packs::ValidatedResource& resource) {
        return resource.descriptor.kind == model::ResourceKind::Record;
    });
    const auto orphan_result = controller.load(orphaned, fixture.runtime, fixture.caseId());
    QVERIFY(!orphan_result.has_value());
    QCOMPARE(orphan_result.error().code, app::InstalledRecordErrorCode::OrphanRecord);
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{0});

    const auto& descriptor = fixture.loaded.blobs.front();
    const auto object_path = QDir(fixture.catalog->blobObjectsDirectory())
                                 .filePath(QString::fromStdString(descriptor.sha256));
    const auto archive_path =
        QDir(fixture.catalog->archivesDirectory())
            .filePath(fixture.installed.archive_sha256 + QStringLiteral(".awpack"));
    QVERIFY(QFile::remove(object_path));
    QVERIFY(QFile::remove(archive_path));
    const auto missing_result = controller.load(fixture.loaded, fixture.runtime, fixture.caseId());
    QVERIFY(!missing_result.has_value());
    QCOMPARE(missing_result.error().code, app::InstalledRecordErrorCode::MaterializationFailure);
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{0});
}

} // namespace

QTEST_MAIN(InstalledRecordWorkspaceTest)

#include "tst_installed_record_workspace.moc"
