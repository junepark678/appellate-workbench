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
#include <QFont>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPageSize>
#include <QPainter>
#include <QPdfDocument>
#include <QPdfWriter>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>

namespace appellate::ui {

class RecordWorkspaceTestAccess final {
  public:
    [[nodiscard]] static auto apply(RecordWorkspace& workspace,
                                    model::RecordAccessProjection projection)
        -> std::expected<void, RecordWorkspaceError> {
        return workspace.setAccessProjectionForTest(std::move(projection));
    }
};

} // namespace appellate::ui

namespace {

namespace app = appellate::app;
namespace model = appellate::model;
namespace packs = appellate::packs;
namespace ui = appellate::ui;

[[nodiscard]] QString fullPackPath() {
    return QStringLiteral(APPELLATE_TEST_FIXTURES) + QStringLiteral("/full-resource-pack");
}

[[nodiscard]] QString fullPackV2Path() {
    return QStringLiteral(APPELLATE_TEST_FIXTURES) + QStringLiteral("/full-resource-pack-v2");
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

enum class FixtureVariant {
    Rich,
    Legacy,
    WrongPageCount,
    Sealed,
    HangulBoundary,
    PreFeatureV2,
    SealedTwinsV2,
    SealedTwinsWrongPageV2,
};

[[nodiscard]] bool applyVariant(const QString& source, FixtureVariant variant) {
    if (variant == FixtureVariant::Rich || variant == FixtureVariant::PreFeatureV2) {
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
    if (variant == FixtureVariant::SealedTwinsV2 ||
        variant == FixtureVariant::SealedTwinsWrongPageV2) {
        const auto sealed_path = QStringLiteral("objects/sealed-psr.pdf");
        if (!writePdf(QDir(source).filePath(sealed_path),
                      {QStringLiteral("Sealed PSR cover"),
                       QStringLiteral("ultrasecret-installed-cas-text")})) {
            return false;
        }
        QFile sealed_file(QDir(source).filePath(sealed_path));
        if (!sealed_file.open(QIODevice::ReadOnly)) {
            return false;
        }
        const auto sealed_bytes = sealed_file.readAll();
        sealed_file.close();
        const auto sealed_digest = sha256(sealed_bytes);

        const auto manifest_path = QDir(source).filePath(QStringLiteral("manifest.json"));
        QFile manifest_file(manifest_path);
        if (!manifest_file.open(QIODevice::ReadOnly)) {
            return false;
        }
        auto manifest = QJsonDocument::fromJson(manifest_file.readAll()).object();
        manifest_file.close();
        auto blobs = manifest.value(QStringLiteral("blobs")).toArray();
        blobs.push_back(QJsonObject{
            {QStringLiteral("path"), sealed_path},
            {QStringLiteral("media_type"), QStringLiteral("application/pdf")},
            {QStringLiteral("byte_size"), sealed_bytes.size()},
            {QStringLiteral("sha256"), sealed_digest},
        });
        manifest.insert(QStringLiteral("blobs"), blobs);
        auto capabilities = manifest.value(QStringLiteral("required_capabilities")).toArray();
        capabilities.push_back(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("workbench.pack.sealed-record-twins")},
            {QStringLiteral("version"), 1},
        });
        manifest.insert(QStringLiteral("required_capabilities"), capabilities);
        if (!writeBytes(manifest_path, jsonBytes(manifest))) {
            return false;
        }

        auto sealed = entry;
        sealed.insert(QStringLiteral("entry_id"), QStringLiteral("example.record.psr-sealed"));
        sealed.insert(QStringLiteral("entry_number"), 3);
        sealed.insert(QStringLiteral("entry_label"), QStringLiteral("ECF No. 42-S"));
        sealed.insert(QStringLiteral("title"), QStringLiteral("Confidential PSR title"));
        sealed.insert(QStringLiteral("description"),
                      QStringLiteral("Never disclose installed PSR description"));
        sealed.insert(QStringLiteral("tags"),
                      QJsonArray{QStringLiteral("psr-secret-installed-tag")});
        sealed.insert(QStringLiteral("asset_path"), sealed_path);
        sealed.insert(QStringLiteral("asset_sha256"), sealed_digest);
        sealed.insert(QStringLiteral("page_count"),
                      variant == FixtureVariant::SealedTwinsWrongPageV2 ? 3 : 2);
        sealed.insert(QStringLiteral("sealed"), true);
        entries.push_back(sealed);
        record.insert(QStringLiteral("docket_entries"), entries);
        auto anchors = record.value(QStringLiteral("page_anchors")).toArray();
        anchors.push_back(QJsonObject{
            {QStringLiteral("anchor_id"), QStringLiteral("example.record.anchor.psr-sealed")},
            {QStringLiteral("entry_id"), QStringLiteral("example.record.psr-sealed")},
            {QStringLiteral("page_number"), 2},
            {QStringLiteral("citation_label"), QStringLiteral("SECRET-JA-2")},
        });
        record.insert(QStringLiteral("page_anchors"), anchors);
        record.insert(
            QStringLiteral("disclosure_policy"),
            QJsonObject{
                {QStringLiteral("policy_id"), QStringLiteral("example.record.policy.psr")},
                {QStringLiteral("unauthorized_projection"),
                 QStringLiteral("public_counterparts_only")},
                {QStringLiteral("authorized_projection"),
                 QStringLiteral("public_and_authorized_sealed")},
                {QStringLiteral("sealed_asset_access"),
                 QStringLiteral("session_event_grant_required")},
            });
        record.insert(
            QStringLiteral("sealed_disclosures"),
            QJsonArray{QJsonObject{
                {QStringLiteral("disclosure_id"), QStringLiteral("example.disclosure.psr")},
                {QStringLiteral("sealed_entry_id"), QStringLiteral("example.record.psr-sealed")},
                {QStringLiteral("public_entry_id"), QStringLiteral("example.record.entry-one")},
                {QStringLiteral("authorization_authority_id"),
                 QStringLiteral("example.authority.deficiency")},
                {QStringLiteral("required_items"),
                 QJsonArray{QStringLiteral("redacted_counterpart")}},
                {QStringLiteral("anchor_mappings"),
                 QJsonArray{QJsonObject{
                     {QStringLiteral("stable_anchor_id"),
                      QStringLiteral("example.record.anchor.psr-stable")},
                     {QStringLiteral("sealed_anchor_id"),
                      QStringLiteral("example.record.anchor.psr-sealed")},
                     {QStringLiteral("public_anchor_id"),
                      QStringLiteral("example.record.anchor.ja2")},
                 }}},
            }});
        return rewriteResource(source, record_path, record);
    }
    if (variant == FixtureVariant::Legacy) {
        record.remove(QStringLiteral("dockets"));
        record.remove(QStringLiteral("page_anchors"));
        for (const auto& field :
             {QStringLiteral("docket_id"), QStringLiteral("entry_label"), QStringLiteral("actor"),
              QStringLiteral("description"), QStringLiteral("tags")}) {
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

    [[nodiscard]] std::optional<QString> initialize(FixtureVariant variant = FixtureVariant::Rich) {
        if (!temporary.isValid()) {
            return QStringLiteral("cannot create temporary root");
        }
        staging_root = QDir(temporary.path()).filePath(QStringLiteral("ephemeral-staging"));
        const auto source = QDir(staging_root).filePath(QStringLiteral("source"));
        const auto archive = QDir(staging_root).filePath(QStringLiteral("full.awpack"));
        const auto v2 = variant == FixtureVariant::PreFeatureV2 ||
                        variant == FixtureVariant::SealedTwinsV2 ||
                        variant == FixtureVariant::SealedTwinsWrongPageV2;
        if (!copyTree(v2 ? fullPackV2Path() : fullPackPath(), source)) {
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
    void defersInstalledSealedCasAndReopensOffline();
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
    QCOMPARE(loaded->definition.docket.front().docket_label, QStringLiteral("1:25-cv-0042"));
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
    changed_anchor.cases.front().record.page_anchors.front().citation_label = std::string("JA999");
    const auto anchor_result = controller.load(fixture.loaded, changed_anchor, fixture.caseId());
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
    const auto sealed_result =
        sealed_controller.load(sealed_fixture.loaded, unsealed_runtime, sealed_fixture.caseId());
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
    QVERIFY2(!wrong_page_error.has_value(), wrong_page_error ? qPrintable(*wrong_page_error) : "");
    ui::RecordWorkspace wrong_page_workspace;
    app::InstalledRecordController wrong_page_controller(*wrong_pages.catalog,
                                                         wrong_page_workspace);
    const auto page_result =
        wrong_page_controller.load(wrong_pages.loaded, wrong_pages.runtime, wrong_pages.caseId());
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
    QCOMPARE(loaded->definition.docket.front().actor, QStringLiteral("Not specified by pack"));
    QCOMPARE(loaded->definition.docket.front().description,
             QStringLiteral("Not specified by pack"));

    InstalledFixture sealed_fixture;
    const auto sealed_error = sealed_fixture.initialize(FixtureVariant::Sealed);
    QVERIFY2(!sealed_error.has_value(), sealed_error ? qPrintable(*sealed_error) : "");
    ui::RecordWorkspace sealed_workspace;
    app::InstalledRecordController sealed_controller(*sealed_fixture.catalog, sealed_workspace);
    const auto sealed_loaded = sealed_controller.load(sealed_fixture.loaded, sealed_fixture.runtime,
                                                      sealed_fixture.caseId());
    QVERIFY2(sealed_loaded.has_value(),
             sealed_loaded ? "" : qPrintable(sealed_loaded.error().message));
    const auto anchor = sealed_workspace.navigateToCitation(QStringLiteral("JA2"));
    QVERIFY(!anchor.has_value());
    QCOMPARE(anchor.error().code, ui::RecordWorkspaceErrorCode::SealedDocument);
    QVERIFY(sealed_workspace.currentDocumentId().isEmpty());

    InstalledFixture hangul_fixture;
    const auto hangul_error = hangul_fixture.initialize(FixtureVariant::HangulBoundary);
    QVERIFY2(!hangul_error.has_value(), hangul_error ? qPrintable(*hangul_error) : "");
    ui::RecordWorkspace hangul_workspace;
    app::InstalledRecordController hangul_controller(*hangul_fixture.catalog, hangul_workspace);
    const auto hangul_loaded = hangul_controller.load(hangul_fixture.loaded, hangul_fixture.runtime,
                                                      hangul_fixture.caseId());
    QVERIFY2(hangul_loaded.has_value(),
             hangul_loaded ? "" : qPrintable(hangul_loaded.error().message));
    QCOMPARE(hangul_loaded->definition.docket.front().actor, QString(240, QChar(0xD55C)));
}

void InstalledRecordWorkspaceTest::defersInstalledSealedCasAndReopensOffline() {
    InstalledFixture fixture;
    const auto fixture_error = fixture.initialize(FixtureVariant::SealedTwinsV2);
    QVERIFY2(!fixture_error.has_value(), fixture_error ? qPrintable(*fixture_error) : "");
    QVERIFY(fixture.runtime.cases.front().record.disclosure_policy.has_value());
    QCOMPARE(fixture.runtime.cases.front().record.sealed_disclosures.size(), std::size_t{1});
    QCOMPARE(fixture.runtime.cases.front().record.docket_entries.size(), std::size_t{3});
    QVERIFY(fixture.runtime.cases.front().record.docket_entries.back().sealed);
    const auto sealed_descriptor = std::ranges::find(
        fixture.loaded.blobs, std::string("objects/sealed-psr.pdf"), &model::BlobDescriptor::path);
    QVERIFY(sealed_descriptor != fixture.loaded.blobs.end());
    const auto sealed_object = QDir(fixture.catalog->blobObjectsDirectory())
                                   .filePath(QString::fromStdString(sealed_descriptor->sha256));
    QFile sealed_input(sealed_object);
    QVERIFY(sealed_input.open(QIODevice::ReadOnly));
    const auto sealed_bytes = sealed_input.readAll();
    sealed_input.close();
    QCOMPARE(sha256(sealed_bytes), QString::fromStdString(sealed_descriptor->sha256));
    const auto archive =
        QDir(fixture.catalog->archivesDirectory())
            .filePath(fixture.installed.archive_sha256 + QStringLiteral(".awpack"));
    QVERIFY(QFile::remove(archive));
    QVERIFY(QFile::remove(sealed_object));

    ui::RecordWorkspace workspace;
    {
        app::InstalledRecordController controller(*fixture.catalog, workspace);
        const auto loaded = controller.load(fixture.loaded, fixture.runtime, fixture.caseId());
        QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error().message));
        QCOMPARE(loaded->assets.size(), std::size_t{2});
        QCOMPARE(loaded->definition.documents.size(), std::size_t{2});
        QCOMPARE(loaded->definition.docket.size(), std::size_t{2});
        QCOMPARE(loaded->definition.anchors.size(), std::size_t{2});
        QVERIFY(!loaded->definition.disclosure_policy.has_value());
        QVERIFY(loaded->definition.sealed_disclosures.empty());
        QString public_text;
        for (const auto& document : loaded->definition.documents) {
            public_text += document.id + document.title + document.file_path;
        }
        for (const auto& entry : loaded->definition.docket) {
            public_text += entry.id + entry.document_id + entry.title + entry.description +
                           entry.tags.join(u' ') + entry.parent_entry_id;
            QVERIFY(entry.parent_entry_id != QStringLiteral("example.record.psr-sealed"));
        }
        for (const auto& anchor : loaded->definition.anchors) {
            public_text += anchor.id + anchor.document_id + anchor.citation_label;
        }
        for (const auto& secret : {QStringLiteral("Confidential"), QStringLiteral("psr-secret"),
                                   QStringLiteral("SECRET-JA"), QStringLiteral("psr-sealed")}) {
            QVERIFY(!public_text.contains(secret));
        }
    }
    QVERIFY(workspace.openDocketEntry(QStringLiteral("example.record.entry-one")).has_value());
    QCOMPARE(workspace.loadedPageCount(), 3);
    workspace.setDocketFilter(QStringLiteral("psr-secret-installed-tag"));
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{0});
    workspace.setDocketFilter({});

    // The controller and original catalog may die before authorization. The
    // resolver owns only immutable catalog-root/revision/descriptor data.
    fixture.catalog.reset();
    const appellate::model::RecordAccessProjection granted{"test.session.installed-sealed",
                                                           "example.record.fictional",
                                                           "example.record.policy.psr",
                                                           1,
                                                           std::string(64, 'a'),
                                                           {"example.record.psr-sealed"}};
    const appellate::model::RecordAccessProjection regranted{"test.session.installed-sealed",
                                                             "example.record.fictional",
                                                             "example.record.policy.psr",
                                                             3,
                                                             std::string(64, 'c'),
                                                             {"example.record.psr-sealed"}};
    QVERIFY(ui::RecordWorkspaceTestAccess::apply(workspace, granted).has_value());
    const auto missing =
        workspace.navigateToAnchor(QStringLiteral("example.record.anchor.psr-stable"));
    QVERIFY(!missing.has_value());
    QCOMPARE(missing.error().code, ui::RecordWorkspaceErrorCode::PdfLoadFailed);
    QVERIFY(workspace.openDocketEntry(QStringLiteral("example.record.entry-one")).has_value());

    QVERIFY(writeBytes(sealed_object, sealed_bytes));
    QVERIFY(
        workspace.navigateToAnchor(QStringLiteral("example.record.anchor.psr-stable")).has_value());
    QCOMPARE(workspace.currentDocumentId(), QStringLiteral("example.record.psr-sealed"));
    QCOMPARE(workspace.loadedPageCount(), 2);
    workspace.setDocumentSearch(QStringLiteral("ultrasecret-installed-cas-text"));
    QTRY_COMPARE_WITH_TIMEOUT(workspace.documentSearchResultCount(), 1, 10'000);

    // The loaded PDF is a private verified snapshot, not the now-mutated CAS
    // pathname. Corrupting CAS cannot alter the active document.
    auto corrupted_bytes = sealed_bytes;
    corrupted_bytes[corrupted_bytes.size() / 2] =
        corrupted_bytes.at(corrupted_bytes.size() / 2) == 'x' ? 'y' : 'x';
    QVERIFY(writeBytes(sealed_object, corrupted_bytes));
    workspace.setDocumentSearch(QStringLiteral("ultrasecret-installed-cas-text"));
    QTRY_COMPARE_WITH_TIMEOUT(workspace.documentSearchResultCount(), 1, 10'000);

    const appellate::model::RecordAccessProjection revoked{"test.session.installed-sealed",
                                                           "example.record.fictional",
                                                           "example.record.policy.psr",
                                                           2,
                                                           std::string(64, 'b'),
                                                           {}};
    QVERIFY(ui::RecordWorkspaceTestAccess::apply(workspace, revoked).has_value());
    QVERIFY(workspace.currentDocumentId().isEmpty());
    QVERIFY(
        workspace.navigateToAnchor(QStringLiteral("example.record.anchor.psr-stable")).has_value());
    QCOMPARE(workspace.currentDocumentId(), QStringLiteral("example.record.entry-one"));
    QVERIFY(ui::RecordWorkspaceTestAccess::apply(workspace, regranted).has_value());
    const auto corrupt =
        workspace.navigateToAnchor(QStringLiteral("example.record.anchor.psr-stable"));
    QVERIFY(!corrupt.has_value());
    QCOMPARE(corrupt.error().code, ui::RecordWorkspaceErrorCode::PdfLoadFailed);
    QVERIFY(writeBytes(sealed_object, sealed_bytes));

    InstalledFixture wrong_pages;
    const auto wrong_error = wrong_pages.initialize(FixtureVariant::SealedTwinsWrongPageV2);
    QVERIFY2(!wrong_error.has_value(), wrong_error ? qPrintable(*wrong_error) : "");
    ui::RecordWorkspace wrong_workspace;
    app::InstalledRecordController wrong_controller(*wrong_pages.catalog, wrong_workspace);
    const auto wrong_loaded =
        wrong_controller.load(wrong_pages.loaded, wrong_pages.runtime, wrong_pages.caseId());
    QVERIFY2(wrong_loaded.has_value(),
             wrong_loaded ? "" : qPrintable(wrong_loaded.error().message));
    QVERIFY(
        wrong_workspace.openDocketEntry(QStringLiteral("example.record.entry-one")).has_value());
    QVERIFY(ui::RecordWorkspaceTestAccess::apply(wrong_workspace, granted).has_value());
    const auto wrong_page =
        wrong_workspace.navigateToAnchor(QStringLiteral("example.record.anchor.psr-stable"));
    QVERIFY(!wrong_page.has_value());
    QCOMPARE(wrong_page.error().code, ui::RecordWorkspaceErrorCode::PdfLoadFailed);

    for (const auto variant : {FixtureVariant::Rich, FixtureVariant::PreFeatureV2}) {
        InstalledFixture compatible;
        const auto compatible_error = compatible.initialize(variant);
        QVERIFY2(!compatible_error.has_value(),
                 compatible_error ? qPrintable(*compatible_error) : "");
        ui::RecordWorkspace compatible_workspace;
        app::InstalledRecordController compatible_controller(*compatible.catalog,
                                                             compatible_workspace);
        const auto compatible_load =
            compatible_controller.load(compatible.loaded, compatible.runtime, compatible.caseId());
        QVERIFY2(compatible_load.has_value(),
                 compatible_load ? "" : qPrintable(compatible_load.error().message));
        QCOMPARE(compatible_load->assets.size(),
                 variant == FixtureVariant::Rich ? std::size_t{1} : std::size_t{2});
        QVERIFY(!compatible_load->definition.documents.front().file_path.isEmpty());
        QVERIFY(!compatible_load->definition.documents.front().deferred_asset);
    }
}

} // namespace

QTEST_MAIN(InstalledRecordWorkspaceTest)

#include "tst_installed_record_workspace.moc"
