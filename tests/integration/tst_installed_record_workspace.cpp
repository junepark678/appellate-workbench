#include "appellate/packs/pack_archive.hpp"
#include "appellate/packs/pack_catalog.hpp"
#include "appellate/packs/runtime_pack.hpp"
#include "installed_record_controller.hpp"
#include "record_workspace.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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

[[nodiscard]] QByteArray jsonBytes(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

[[nodiscard]] QString sha256(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

[[nodiscard]] bool writeBytes(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(bytes) == static_cast<qint64>(bytes.size());
}

[[nodiscard]] bool rewriteResource(const QString& root, const QString& relative_path,
                                   const QJsonObject& resource) {
    const auto bytes = jsonBytes(resource);
    if (!writeBytes(QDir(root).filePath(relative_path), bytes)) {
        return false;
    }
    const auto manifest_path = QDir(root).filePath(QStringLiteral("manifest.json"));
    QFile manifest_file(manifest_path);
    if (!manifest_file.open(QIODevice::ReadOnly)) {
        return false;
    }
    auto manifest = QJsonDocument::fromJson(manifest_file.readAll()).object();
    manifest_file.close();
    auto contents = manifest.value(QStringLiteral("contents")).toArray();
    for (qsizetype index = 0; index < contents.size(); ++index) {
        auto descriptor = contents.at(index).toObject();
        if (descriptor.value(QStringLiteral("path")).toString() != relative_path) {
            continue;
        }
        descriptor.insert(QStringLiteral("sha256"), sha256(bytes));
        contents.replace(index, descriptor);
        manifest.insert(QStringLiteral("contents"), contents);
        return writeBytes(manifest_path, jsonBytes(manifest));
    }
    return false;
}

enum class FixtureVariant { Rich, Legacy, WrongPageCount, Sealed, HangulBoundary };

[[nodiscard]] bool applyVariant(const QString& source, FixtureVariant variant) {
    if (variant == FixtureVariant::Rich) {
        return true;
    }
    const auto record_path = QStringLiteral("resources/record.json");
    QFile record_file(QDir(source).filePath(record_path));
    if (!record_file.open(QIODevice::ReadOnly)) {
        return false;
    }
    auto record = QJsonDocument::fromJson(record_file.readAll()).object();
    record_file.close();
    auto entries = record.value(QStringLiteral("docket_entries")).toArray();
    auto entry = entries.at(0).toObject();
    if (variant == FixtureVariant::Legacy) {
        record.remove(QStringLiteral("dockets"));
        record.remove(QStringLiteral("page_anchors"));
        for (const auto& field : {QStringLiteral("docket_id"), QStringLiteral("entry_label"),
                                  QStringLiteral("actor"), QStringLiteral("description"),
                                  QStringLiteral("tags")}) {
            entry.remove(field);
        }
    } else if (variant == FixtureVariant::WrongPageCount) {
        entry.insert(QStringLiteral("page_count"), 2);
    } else if (variant == FixtureVariant::Sealed) {
        entry.insert(QStringLiteral("sealed"), true);
    } else if (variant == FixtureVariant::HangulBoundary) {
        entry.insert(QStringLiteral("actor"), QString(240, QChar(0xD55C)));
    }
    entries.replace(0, entry);
    record.insert(QStringLiteral("docket_entries"), entries);
    if (!rewriteResource(source, record_path, record)) {
        return false;
    }
    if (variant != FixtureVariant::Legacy) {
        return true;
    }
    const auto case_path = QStringLiteral("resources/case.json");
    QFile case_file(QDir(source).filePath(case_path));
    if (!case_file.open(QIODevice::ReadOnly)) {
        return false;
    }
    auto case_resource = QJsonDocument::fromJson(case_file.readAll()).object();
    case_file.close();
    auto issues = case_resource.value(QStringLiteral("issues")).toArray();
    auto issue = issues.at(0).toObject();
    issue.insert(QStringLiteral("record_anchor_ids"),
                 QJsonArray{QStringLiteral("example.record.entry-one")});
    issues.replace(0, issue);
    case_resource.insert(QStringLiteral("issues"), issues);
    return rewriteResource(source, case_path, case_resource);
}

struct InstalledFixture final {
    QTemporaryDir temporary;
    QString staging_root;
    packs::InstalledPack installed;
    std::unique_ptr<packs::PackCatalog> catalog;
    packs::LoadedPack loaded;
    packs::RuntimePack runtime;

    [[nodiscard]] std::optional<QString> initialize(
        FixtureVariant variant = FixtureVariant::Rich) {
        if (!temporary.isValid()) {
            return QStringLiteral("cannot create temporary root");
        }
        staging_root = QDir(temporary.path()).filePath(QStringLiteral("ephemeral-staging"));
        const auto source = QDir(staging_root).filePath(QStringLiteral("source"));
        const auto archive = QDir(staging_root).filePath(QStringLiteral("full.awpack"));
        if (!copyTree(fullPackPath(), source)) {
            return QStringLiteral("cannot stage full-resource-pack fixture");
        }
        if (!applyVariant(source, variant)) {
            return QStringLiteral("cannot apply installed fixture variant");
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
    void rejectsRevisionAndRuntimeMutation();
    void rejectsMissingBlobWrongPageCountAndOrphanRecord();
    void mapsLegacyFallbackAndKeepsSealedAnchorsClosed();
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
    QCOMPARE(loaded->definition.dockets.size(), std::size_t{2});
    QCOMPARE(loaded->definition.dockets.front().public_docket_number,
             QStringLiteral("1:25-cv-0042"));
    QCOMPARE(loaded->definition.anchors.size(), std::size_t{1});
    QCOMPARE(loaded->definition.anchors.front().citation_label, QStringLiteral("JA2"));
    QCOMPARE(loaded->definition.documents.front().id, QStringLiteral("example.record.entry-one"));
    QCOMPARE(loaded->definition.documents.front().declared_page_count, 3);
    QCOMPARE(loaded->definition.docket.front().document_id,
             loaded->definition.documents.front().id);
    QCOMPARE(loaded->definition.docket.front().filed_on, QDate(2026, 1, 2));
    QCOMPARE(loaded->definition.docket.front().docket_label,
             QStringLiteral("1:25-cv-0042"));
    QCOMPARE(loaded->definition.docket.front().entry_label, QStringLiteral("ECF No. 42"));
    QCOMPARE(loaded->definition.docket.front().actor, QStringLiteral("District clerk"));
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
    QVERIFY(workspace.navigateToAnchor(QStringLiteral("example.record.anchor.ja2")).has_value());
    QTRY_COMPARE(workspace.currentPageIndex(), 1);
    QVERIFY(workspace.navigateToCitation(QStringLiteral("JA2")).has_value());
    QTRY_COMPARE(workspace.currentPageIndex(), 1);
    workspace.setDocketFilter(QStringLiteral("1:25-cv-0042 ECF No. 42 district clerk JA2"));
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{1});
    workspace.setDocumentSearch(QStringLiteral("Fictional Final Order - Page 2"));
    QTRY_COMPARE_WITH_TIMEOUT(workspace.documentSearchResultCount(), 1, 10'000);
}

void InstalledRecordWorkspaceTest::rejectsRevisionAndRuntimeMutation() {
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
    QCOMPARE(digest_result.error().code, app::InstalledRecordErrorCode::RuntimeMismatch);
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{0});

    auto changed_metadata = fixture.runtime;
    changed_metadata.cases.front().record.docket_entries.front().actor =
        std::string("Injected actor");
    const auto metadata_result =
        controller.load(fixture.loaded, changed_metadata, fixture.caseId());
    QVERIFY(!metadata_result.has_value());
    QCOMPARE(metadata_result.error().code, app::InstalledRecordErrorCode::RuntimeMismatch);
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{0});

    auto changed_anchor = fixture.runtime;
    changed_anchor.cases.front().record.page_anchors.front().citation_label =
        std::string("JA999");
    const auto anchor_result =
        controller.load(fixture.loaded, changed_anchor, fixture.caseId());
    QVERIFY(!anchor_result.has_value());
    QCOMPARE(anchor_result.error().code, app::InstalledRecordErrorCode::RuntimeMismatch);
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{0});

    InstalledFixture sealed_fixture;
    const auto sealed_error = sealed_fixture.initialize(FixtureVariant::Sealed);
    QVERIFY2(!sealed_error.has_value(), sealed_error ? qPrintable(*sealed_error) : "");
    ui::RecordWorkspace sealed_workspace;
    app::InstalledRecordController sealed_controller(*sealed_fixture.catalog, sealed_workspace);
    auto unsealed_runtime = sealed_fixture.runtime;
    QVERIFY(unsealed_runtime.cases.front().record.docket_entries.front().sealed);
    unsealed_runtime.cases.front().record.docket_entries.front().sealed = false;
    const auto sealed_result = sealed_controller.load(
        sealed_fixture.loaded, unsealed_runtime, sealed_fixture.caseId());
    QVERIFY(!sealed_result.has_value());
    QCOMPARE(sealed_result.error().code, app::InstalledRecordErrorCode::RuntimeMismatch);
    QCOMPARE(sealed_workspace.visibleDocketCount(), qsizetype{0});
}

void InstalledRecordWorkspaceTest::rejectsMissingBlobWrongPageCountAndOrphanRecord() {
    InstalledFixture fixture;
    const auto error = fixture.initialize();
    QVERIFY2(!error.has_value(), error ? qPrintable(*error) : "");

    ui::RecordWorkspace workspace;
    app::InstalledRecordController controller(*fixture.catalog, workspace);

    InstalledFixture wrong_pages;
    const auto wrong_page_error = wrong_pages.initialize(FixtureVariant::WrongPageCount);
    QVERIFY2(!wrong_page_error.has_value(),
             wrong_page_error ? qPrintable(*wrong_page_error) : "");
    ui::RecordWorkspace wrong_page_workspace;
    app::InstalledRecordController wrong_page_controller(*wrong_pages.catalog,
                                                          wrong_page_workspace);
    const auto page_result = wrong_page_controller.load(
        wrong_pages.loaded, wrong_pages.runtime, wrong_pages.caseId());
    QVERIFY(!page_result.has_value());
    QCOMPARE(page_result.error().code, app::InstalledRecordErrorCode::PageCountMismatch);
    QCOMPARE(wrong_page_workspace.visibleDocketCount(), qsizetype{0});

    auto orphaned = fixture.loaded;
    std::erase_if(orphaned.resources, [](const packs::ValidatedResource& resource) {
        return resource.descriptor.kind == model::ResourceKind::Record;
    });
    const auto orphan_result = controller.load(orphaned, fixture.runtime, fixture.caseId());
    QVERIFY(!orphan_result.has_value());
    QCOMPARE(orphan_result.error().code, app::InstalledRecordErrorCode::RuntimeMismatch);
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

void InstalledRecordWorkspaceTest::mapsLegacyFallbackAndKeepsSealedAnchorsClosed() {
    InstalledFixture fixture;
    const auto error = fixture.initialize(FixtureVariant::Legacy);
    QVERIFY2(!error.has_value(), error ? qPrintable(*error) : "");

    ui::RecordWorkspace workspace;
    app::InstalledRecordController controller(*fixture.catalog, workspace);
    const auto loaded = controller.load(fixture.loaded, fixture.runtime, fixture.caseId());
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error().message));
    QCOMPARE(loaded->definition.docket.front().docket_label,
             QStringLiteral("Not specified by pack"));
    QCOMPARE(loaded->definition.docket.front().entry_label,
             QStringLiteral("Not specified by pack"));
    QCOMPARE(loaded->definition.docket.front().actor,
             QStringLiteral("Not specified by pack"));
    QCOMPARE(loaded->definition.docket.front().description,
             QStringLiteral("Not specified by pack"));

    InstalledFixture sealed_fixture;
    const auto sealed_error = sealed_fixture.initialize(FixtureVariant::Sealed);
    QVERIFY2(!sealed_error.has_value(), sealed_error ? qPrintable(*sealed_error) : "");
    ui::RecordWorkspace sealed_workspace;
    app::InstalledRecordController sealed_controller(*sealed_fixture.catalog, sealed_workspace);
    const auto sealed_loaded = sealed_controller.load(
        sealed_fixture.loaded, sealed_fixture.runtime, sealed_fixture.caseId());
    QVERIFY2(sealed_loaded.has_value(),
             sealed_loaded ? "" : qPrintable(sealed_loaded.error().message));
    const auto anchor =
        sealed_workspace.navigateToCitation(QStringLiteral("JA2"));
    QVERIFY(!anchor.has_value());
    QCOMPARE(anchor.error().code, ui::RecordWorkspaceErrorCode::SealedDocument);
    QVERIFY(sealed_workspace.currentDocumentId().isEmpty());

    InstalledFixture hangul_fixture;
    const auto hangul_error = hangul_fixture.initialize(FixtureVariant::HangulBoundary);
    QVERIFY2(!hangul_error.has_value(), hangul_error ? qPrintable(*hangul_error) : "");
    ui::RecordWorkspace hangul_workspace;
    app::InstalledRecordController hangul_controller(*hangul_fixture.catalog,
                                                      hangul_workspace);
    const auto hangul_loaded = hangul_controller.load(
        hangul_fixture.loaded, hangul_fixture.runtime, hangul_fixture.caseId());
    QVERIFY2(hangul_loaded.has_value(),
             hangul_loaded ? "" : qPrintable(hangul_loaded.error().message));
    QCOMPARE(hangul_loaded->definition.docket.front().actor,
             QString(240, QChar(0xD55C)));
}

} // namespace

QTEST_MAIN(InstalledRecordWorkspaceTest)

#include "tst_installed_record_workspace.moc"
