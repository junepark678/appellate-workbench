#include "appellate/packs/pack_catalog.hpp"
#include "appellate/packs/runtime_pack.hpp"
#include "installed_record_controller.hpp"
#include "record_workspace.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPdfDocument>
#include <QTemporaryDir>
#include <QTest>

#include <ranges>
#include <string>
#include <utility>

namespace {

namespace app = appellate::app;
namespace model = appellate::model;
namespace packs = appellate::packs;
namespace ui = appellate::ui;

constexpr auto pack_id = "us.ca4.rule54b.asterglen";
constexpr auto pack_version = "0.1.0";
constexpr auto case_id = "ca4r54b.case.asterglen";
constexpr auto appendix_entry_id = "ca4r54b.record.a12";

[[nodiscard]] QString goldArchivePath() { return QStringLiteral(APPELLATE_GOLD_ARCHIVE); }

class GoldRecordWorkspaceTest final : public QObject {
    Q_OBJECT

  private slots:
    void installsAndExercisesNativeRecordLoopFromExactGoldArchive();
};

void GoldRecordWorkspaceTest::installsAndExercisesNativeRecordLoopFromExactGoldArchive() {
    const QFileInfo checked_in_archive(goldArchivePath());
    QVERIFY2(checked_in_archive.isFile() && !checked_in_archive.isSymLink(),
             qPrintable(QStringLiteral("Gold archive is unavailable: %1").arg(goldArchivePath())));

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto installation_source =
        QDir(temporary.path()).filePath(QStringLiteral("gold-install-source.awpack"));
    QVERIFY(QFile::copy(goldArchivePath(), installation_source));
    QCOMPARE(QFileInfo(installation_source).size(), checked_in_archive.size());

    auto opened_catalog =
        packs::PackCatalog::open(QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    QVERIFY2(opened_catalog.has_value(),
             opened_catalog ? "" : qPrintable(opened_catalog.error().message));
    auto catalog = std::move(*opened_catalog);

    const auto installed =
        catalog->installArchive(installation_source, QStringLiteral("2026-08-11T00:00:00Z"));
    QVERIFY2(installed.has_value(), installed ? "" : qPrintable(installed.error().message));
    QCOMPARE(installed->revision.id.value, std::string(pack_id));
    QCOMPARE(installed->revision.version, std::string(pack_version));
    QCOMPARE(installed->revision.digest.size(), std::size_t{64});

    QVERIFY(QFile::remove(installation_source));
    QVERIFY(!QFileInfo::exists(installation_source));

    const auto loaded = catalog->load(model::PackId{pack_id}, pack_version);
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error().message));
    QCOMPARE(loaded->revision, installed->revision);
    QCOMPARE(loaded->blobs.size(), std::size_t{18});

    const auto runtime = packs::loadRuntimePack(*loaded);
    QVERIFY2(runtime.has_value(), runtime ? "" : runtime.error().message.c_str());
    QCOMPARE(runtime->revision, installed->revision);
    QCOMPARE(runtime->cases.size(), std::size_t{1});
    QCOMPARE(runtime->cases.front().definition.id.value, std::string(case_id));

    const auto catalog_archive =
        QDir(catalog->archivesDirectory())
            .filePath(installed->archive_sha256 + QStringLiteral(".awpack"));
    QVERIFY(QFileInfo::exists(catalog_archive));
    QVERIFY(QFile::remove(catalog_archive));
    QVERIFY(!QFileInfo::exists(catalog_archive));

    QCOMPARE(
        QDir(catalog->blobObjectsDirectory()).entryList(QDir::Files | QDir::NoDotAndDotDot).size(),
        18);

    ui::RecordWorkspace workspace;
    app::InstalledRecordController controller(*catalog, workspace);
    const auto record = controller.load(*loaded, *runtime, model::CaseId{std::string(case_id)});
    QVERIFY2(record.has_value(), record ? "" : qPrintable(record.error().message));
    QCOMPARE(record->revision, installed->revision);
    QCOMPARE(record->definition.dockets.size(), std::size_t{2});
    QCOMPARE(record->definition.documents.size(), std::size_t{18});
    QCOMPARE(record->definition.docket.size(), std::size_t{18});
    QCOMPARE(record->assets.size(), std::size_t{18});

    const auto district_docket =
        std::ranges::find(record->definition.dockets, QStringLiteral("ca4r54b.docket.edva"),
                          &ui::RecordDocketDescriptor::id);
    QVERIFY(district_docket != record->definition.dockets.end());
    QCOMPARE(district_docket->type, QStringLiteral("district"));
    QCOMPARE(district_docket->public_docket_number, QStringLiteral("SYN-25-0117"));
    const auto appellate_docket =
        std::ranges::find(record->definition.dockets, QStringLiteral("ca4r54b.docket.ca4"),
                          &ui::RecordDocketDescriptor::id);
    QVERIFY(appellate_docket != record->definition.dockets.end());
    QCOMPARE(appellate_docket->type, QStringLiteral("appellate"));
    QCOMPARE(appellate_docket->public_docket_number, QStringLiteral("SYN-26-1427"));

    int searchable_page_count = 0;
    for (const auto& asset : record->assets) {
        const auto expected_path = QDir(catalog->blobObjectsDirectory())
                                       .filePath(QString::fromStdString(asset.descriptor.sha256));
        QCOMPARE(QDir::cleanPath(asset.local_path), QDir::cleanPath(expected_path));
        QVERIFY(!asset.local_path.startsWith(checked_in_archive.absolutePath()));

        QPdfDocument pdf;
        QCOMPARE(pdf.load(asset.local_path), QPdfDocument::Error::None);
        QCOMPARE(pdf.status(), QPdfDocument::Status::Ready);
        QCOMPARE(pdf.pageCount(), asset.page_count);
        for (int page = 0; page < pdf.pageCount(); ++page) {
            const auto text = pdf.getAllText(page).text().simplified();
            QVERIFY2(!text.isEmpty(),
                     qPrintable(QStringLiteral("PDF %1 page %2 has no searchable text")
                                    .arg(QString::fromStdString(asset.entry_id.value))
                                    .arg(page + 1)));
            ++searchable_page_count;
        }
    }
    QCOMPARE(searchable_page_count, 124);

    const auto navigated = workspace.navigateToCitation(QStringLiteral("JA40"));
    QVERIFY2(navigated.has_value(), navigated ? "" : qPrintable(navigated.error().message));
    QCOMPARE(workspace.currentDocumentId(), QString::fromLatin1(appendix_entry_id));
    QCOMPARE(workspace.loadedPageCount(), 47);
    QTRY_COMPARE(workspace.currentPageIndex(), 39);

    const auto appendix_asset = std::ranges::find(
        record->assets, std::string(appendix_entry_id),
        [](const app::InstalledRecordAsset& asset) { return asset.entry_id.value; });
    QVERIFY(appendix_asset != record->assets.end());
    QPdfDocument appendix;
    QCOMPARE(appendix.load(appendix_asset->local_path), QPdfDocument::Error::None);
    QVERIFY(appendix.getAllText(39).text().contains(
        QStringLiteral("The Clerk is DIRECTED to enter judgment")));

    workspace.setDocumentSearch(QStringLiteral("Clerk is DIRECTED to enter judgment"));
    QTRY_VERIFY_WITH_TIMEOUT(workspace.documentSearchResultCount() > 0, 10'000);
    workspace.setDocketFilter(QStringLiteral("SYN-26-1427 ECF No. 15 JA40"));
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{1});
}

} // namespace

QTEST_MAIN(GoldRecordWorkspaceTest)

#include "tst_gold_record_workspace.moc"
