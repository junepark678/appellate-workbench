#include "appellate/packs/pack_archive.hpp"
#include "appellate/packs/pack_catalog.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include <string>
#include <vector>

namespace {

using appellate::model::PackRevision;
using appellate::packs::CatalogErrorCode;
using appellate::packs::PackArchive;
using appellate::packs::PackCatalog;

class PackCatalogTest final : public QObject {
    Q_OBJECT

  private slots:
    void installsLoadsListsAndIsIdempotent();
    void requiresExactDependenciesWithoutPartialInstall();
    void rejectsSelfCycleAndImmutableConflict();
    void detectsCorruptInstalledArchive();
    void materializesStableBlobForExactRevision();
    void rehydratesMissingBlobButNeverOverwritesCorruption();
    void deduplicatesBlobsAndRequiresVerifiedArchives();
    void refusesInstallOverCorruptBlobObject();
    void detectsTamperedBlobDescriptor();
    void migratesV1BlobDescriptors();
};

[[nodiscard]] QByteArray sha256(const QByteArray& bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

[[nodiscard]] QByteArray testPdf() {
    return QByteArray("%PDF-1.7\n1 0 obj\n<<>>\nendobj\ntrailer\n<<>>\n%%EOF\n");
}

[[nodiscard]] bool writeAll(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
           file.write(bytes) == bytes.size();
}

[[nodiscard]] bool executeCatalogSql(const QString& catalog_root,
                                     const std::vector<QString>& statements) {
    const auto connection_name =
        QStringLiteral("catalog-test-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    bool succeeded = false;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name);
        database.setDatabaseName(QDir(catalog_root).filePath(QStringLiteral("catalog.sqlite")));
        succeeded = database.open();
        for (const auto& statement : statements) {
            if (!succeeded) {
                break;
            }
            QSqlQuery query(database);
            succeeded = query.exec(statement);
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection_name);
    return succeeded;
}

[[nodiscard]] auto dependencyJson(const PackRevision& revision) -> QJsonObject {
    return QJsonObject{
        {QStringLiteral("pack_id"), QString::fromStdString(revision.id.value)},
        {QStringLiteral("version"), QString::fromStdString(revision.version)},
        {QStringLiteral("sha256"), QString::fromStdString(revision.digest)},
    };
}

[[nodiscard]] auto buildArchive(const QString& root, const QString& stem, const QString& pack_id,
                                const QString& version, const QString& display_name,
                                const std::vector<PackRevision>& dependencies = {},
                                bool include_blob = false) -> std::expected<PackRevision, QString> {
    const auto source = QDir(root).filePath(QStringLiteral("sources/") + stem);
    const auto archive_directory = QDir(root).filePath(QStringLiteral("exports"));
    if (!QDir{}.mkpath(QDir(source).filePath(QStringLiteral("judges"))) ||
        !QDir{}.mkpath(archive_directory)) {
        return std::unexpected(QStringLiteral("cannot create pack directories"));
    }

    const auto profile_id = pack_id + QStringLiteral(".judge.measured");
    const auto profile =
        QJsonDocument(
            QJsonObject{
                {QStringLiteral("schema_version"), 1},
                {QStringLiteral("resource_kind"), QStringLiteral("judge_profile")},
                {QStringLiteral("resource_id"), profile_id},
                {QStringLiteral("display_name"), display_name},
                {QStringLiteral("profile_class"), QStringLiteral("fictional_composite")},
                {QStringLiteral("compatibility"),
                 QJsonObject{
                     {QStringLiteral("court_roles"), QJsonArray{QStringLiteral("appellate")}},
                     {QStringLiteral("jurisdiction_ids"), QJsonArray{QStringLiteral("test.court")}},
                 }},
                {QStringLiteral("interaction"),
                 QJsonObject{
                     {QStringLiteral("directness"), 0.5},
                     {QStringLiteral("formality"), 0.5},
                     {QStringLiteral("question_length"), 0.5},
                     {QStringLiteral("interruption_frequency"), 0.2},
                     {QStringLiteral("follow_up_depth"), 0.5},
                     {QStringLiteral("hypothetical_frequency"), 0.4},
                     {QStringLiteral("concession_recall"), 0.6},
                     {QStringLiteral("record_pin_demand"), 0.7},
                     {QStringLiteral("time_strictness"), 0.5},
                     {QStringLiteral("issue_focus"),
                      QJsonArray{QJsonObject{
                          {QStringLiteral("topic_id"), QStringLiteral("test.issue.preservation")},
                          {QStringLiteral("weight"), 0.8},
                      }}},
                 }},
                {QStringLiteral("voice"),
                 QJsonObject{
                     {QStringLiteral("register"), QStringLiteral("formal")},
                     {QStringLiteral("cadence"), QStringLiteral("measured")},
                     {QStringLiteral("question_framing"), QStringLiteral("direct")},
                     {QStringLiteral("address_convention"), QStringLiteral("counsel")},
                     {QStringLiteral("verbosity"), 0.5},
                     {QStringLiteral("sentence_complexity"), 0.5},
                     {QStringLiteral("question_phrases"),
                      QJsonArray{QStringLiteral("address the question")}},
                     {QStringLiteral("interruption_phrases"),
                      QJsonArray{QStringLiteral("pause there")}},
                     {QStringLiteral("clarification_phrases"),
                      QJsonArray{QStringLiteral("clarify that point")}},
                 }},
            })
            .toJson(QJsonDocument::Compact);
    const auto profile_path = QDir(source).filePath(QStringLiteral("judges/measured.json"));
    if (!writeAll(profile_path, profile)) {
        return std::unexpected(QStringLiteral("cannot write profile"));
    }

    QJsonArray dependency_array;
    for (const auto& dependency : dependencies) {
        dependency_array.push_back(dependencyJson(dependency));
    }
    QJsonArray blobs;
    QJsonArray contents{QJsonObject{
        {QStringLiteral("id"), profile_id},
        {QStringLiteral("kind"), QStringLiteral("judge_profile")},
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("path"), QStringLiteral("judges/measured.json")},
        {QStringLiteral("sha256"), QString::fromLatin1(sha256(profile))},
    }};
    if (include_blob) {
        const auto pdf = testPdf();
        const auto pdf_digest = QString::fromLatin1(sha256(pdf));
        if (!QDir{}.mkpath(QDir(source).filePath(QStringLiteral("objects"))) ||
            !QDir{}.mkpath(QDir(source).filePath(QStringLiteral("records"))) ||
            !writeAll(QDir(source).filePath(QStringLiteral("objects/document.pdf")), pdf)) {
            return std::unexpected(QStringLiteral("cannot write blob"));
        }
        const auto record_id = pack_id + QStringLiteral(".record.main");
        const auto record =
            QJsonDocument(
                QJsonObject{
                    {QStringLiteral("schema_version"), 1},
                    {QStringLiteral("resource_kind"), QStringLiteral("record")},
                    {QStringLiteral("resource_id"), record_id},
                    {QStringLiteral("caption"), QStringLiteral("Synthetic record")},
                    {QStringLiteral("docket_entries"),
                     QJsonArray{QJsonObject{
                         {QStringLiteral("entry_id"), pack_id + QStringLiteral(".entry.document")},
                         {QStringLiteral("entry_number"), 1},
                         {QStringLiteral("filed_on"), QStringLiteral("2026-08-11")},
                         {QStringLiteral("title"), QStringLiteral("Synthetic order")},
                         {QStringLiteral("asset_path"), QStringLiteral("objects/document.pdf")},
                         {QStringLiteral("asset_sha256"), pdf_digest},
                         {QStringLiteral("page_count"), 1},
                         {QStringLiteral("sealed"), false},
                     }}},
                })
                .toJson(QJsonDocument::Compact);
        if (!writeAll(QDir(source).filePath(QStringLiteral("records/main.json")), record)) {
            return std::unexpected(QStringLiteral("cannot write record"));
        }
        blobs.push_back(QJsonObject{
            {QStringLiteral("path"), QStringLiteral("objects/document.pdf")},
            {QStringLiteral("media_type"), QStringLiteral("application/pdf")},
            {QStringLiteral("byte_size"), pdf.size()},
            {QStringLiteral("sha256"), pdf_digest},
        });
        contents.push_back(QJsonObject{
            {QStringLiteral("id"), record_id},
            {QStringLiteral("kind"), QStringLiteral("record")},
            {QStringLiteral("schema_version"), 1},
            {QStringLiteral("path"), QStringLiteral("records/main.json")},
            {QStringLiteral("sha256"), QString::fromLatin1(sha256(record))},
        });
    }
    const auto manifest =
        QJsonDocument(QJsonObject{
                          {QStringLiteral("schema_version"), 1},
                          {QStringLiteral("pack_id"), pack_id},
                          {QStringLiteral("version"), version},
                          {QStringLiteral("required_capabilities"),
                           QJsonArray{
                               QJsonObject{{QStringLiteral("id"),
                                            QStringLiteral("workbench.pack.judge-profile")},
                                           {QStringLiteral("version"), 1}},
                               QJsonObject{{QStringLiteral("id"),
                                            QStringLiteral("workbench.pack.voice-style")},
                                           {QStringLiteral("version"), 1}},
                           }},
                          {QStringLiteral("dependencies"), dependency_array},
                          {QStringLiteral("blobs"), blobs},
                          {QStringLiteral("contents"), contents},
                      })
            .toJson(QJsonDocument::Compact);
    if (!writeAll(QDir(source).filePath(QStringLiteral("manifest.json")), manifest)) {
        return std::unexpected(QStringLiteral("cannot write manifest"));
    }

    const auto archive = QDir(archive_directory).filePath(stem + QStringLiteral(".awpack"));
    const auto exported = PackArchive::exportDirectory(source, archive);
    if (!exported) {
        return std::unexpected(exported.error().message);
    }
    return *exported;
}

[[nodiscard]] QString archivePath(const QString& root, const QString& stem) {
    return QDir(root).filePath(QStringLiteral("exports/") + stem + QStringLiteral(".awpack"));
}

void PackCatalogTest::installsLoadsListsAndIsIdempotent() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto base =
        buildArchive(temporary.path(), QStringLiteral("base"), QStringLiteral("test.pack.base"),
                     QStringLiteral("1.0.0"), QStringLiteral("Base Composite"));
    if (!base) {
        QFAIL(qPrintable(base.error()));
    }
    const auto dependent = buildArchive(
        temporary.path(), QStringLiteral("dependent"), QStringLiteral("test.pack.dependent"),
        QStringLiteral("1.0.0"), QStringLiteral("Dependent Composite"), {*base});
    if (!dependent) {
        QFAIL(qPrintable(dependent.error()));
    }

    auto catalog = PackCatalog::open(QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    QVERIFY(catalog.has_value());
    QCOMPARE((*catalog)->schemaVersion(), 2);
    const auto installed_base =
        (*catalog)->installArchive(archivePath(temporary.path(), QStringLiteral("base")),
                                   QStringLiteral("2026-08-11T01:00:00Z"));
    QVERIFY(installed_base.has_value());
    QVERIFY(installed_base->revision == *base);
    QVERIFY(QFileInfo::exists(
        QDir((*catalog)->archivesDirectory())
            .filePath(installed_base->archive_sha256 + QStringLiteral(".awpack"))));

    const auto installed_dependent =
        (*catalog)->installArchive(archivePath(temporary.path(), QStringLiteral("dependent")),
                                   QStringLiteral("2026-08-11T02:00:00Z"));
    QVERIFY(installed_dependent.has_value());
    QCOMPARE(installed_dependent->dependencies.size(), std::size_t{1});
    QVERIFY(installed_dependent->dependencies.front().revision == *base);

    const auto idempotent =
        (*catalog)->installArchive(archivePath(temporary.path(), QStringLiteral("dependent")),
                                   QStringLiteral("2026-08-11T03:00:00Z"));
    QVERIFY(idempotent.has_value());
    QCOMPARE(idempotent->installed_at_utc, QStringLiteral("2026-08-11T02:00:00Z"));

    const auto listed = (*catalog)->list();
    QVERIFY(listed.has_value());
    QCOMPARE(listed->size(), std::size_t{2});
    const auto loaded = (*catalog)->load(dependent->id, dependent->version);
    QVERIFY(loaded.has_value());
    QVERIFY(loaded->revision == *dependent);
}

void PackCatalogTest::requiresExactDependenciesWithoutPartialInstall() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const PackRevision missing{
        appellate::model::PackId{"test.pack.missing"},
        "1.0.0",
        std::string(64, 'a'),
    };
    const auto dependent = buildArchive(
        temporary.path(), QStringLiteral("dependent"), QStringLiteral("test.pack.dependent"),
        QStringLiteral("1.0.0"), QStringLiteral("Dependent Composite"), {missing}, true);
    QVERIFY(dependent.has_value());
    auto catalog = PackCatalog::open(QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    QVERIFY(catalog.has_value());

    const auto installed =
        (*catalog)->installArchive(archivePath(temporary.path(), QStringLiteral("dependent")),
                                   QStringLiteral("2026-08-11T02:00:00Z"));
    QVERIFY(!installed.has_value());
    QCOMPARE(installed.error().code, CatalogErrorCode::MissingDependency);
    const auto listed = (*catalog)->list();
    QVERIFY(listed.has_value());
    QVERIFY(listed->empty());
    QCOMPARE(QDir((*catalog)->blobObjectsDirectory())
                 .entryList(QDir::Files | QDir::NoDotAndDotDot)
                 .size(),
             0);
    QCOMPARE(
        QDir((*catalog)->archivesDirectory()).entryList(QDir::Files | QDir::NoDotAndDotDot).size(),
        0);
}

void PackCatalogTest::rejectsSelfCycleAndImmutableConflict() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const PackRevision self{
        appellate::model::PackId{"test.pack.cycle"},
        "1.0.0",
        std::string(64, 'b'),
    };
    const auto cycle =
        buildArchive(temporary.path(), QStringLiteral("cycle"), QStringLiteral("test.pack.cycle"),
                     QStringLiteral("1.0.0"), QStringLiteral("Cycle Composite"), {self});
    // The semantic reader rejects a direct cycle before an archive can be produced. The catalog
    // repeats this guard for defense in depth when accepting future archive/schema versions.
    QVERIFY(!cycle.has_value());
    auto catalog = PackCatalog::open(QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    QVERIFY(catalog.has_value());

    const auto first = buildArchive(temporary.path(), QStringLiteral("first"),
                                    QStringLiteral("test.pack.conflict"), QStringLiteral("1.0.0"),
                                    QStringLiteral("First Composite"));
    const auto second = buildArchive(temporary.path(), QStringLiteral("second"),
                                     QStringLiteral("test.pack.conflict"), QStringLiteral("1.0.0"),
                                     QStringLiteral("Second Composite"));
    QVERIFY(first.has_value());
    QVERIFY(second.has_value());
    QVERIFY(first->digest != second->digest);
    QVERIFY((*catalog)
                ->installArchive(archivePath(temporary.path(), QStringLiteral("first")),
                                 QStringLiteral("2026-08-11T02:00:00Z"))
                .has_value());
    const auto result =
        (*catalog)->installArchive(archivePath(temporary.path(), QStringLiteral("second")),
                                   QStringLiteral("2026-08-11T03:00:00Z"));
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, CatalogErrorCode::ImmutableConflict);
    const auto listed = (*catalog)->list();
    QVERIFY(listed.has_value());
    QCOMPARE(listed->size(), std::size_t{1});
    QVERIFY(listed->front().revision == *first);
}

void PackCatalogTest::detectsCorruptInstalledArchive() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto revision =
        buildArchive(temporary.path(), QStringLiteral("base"), QStringLiteral("test.pack.base"),
                     QStringLiteral("1.0.0"), QStringLiteral("Base Composite"));
    QVERIFY(revision.has_value());
    auto catalog = PackCatalog::open(QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    QVERIFY(catalog.has_value());
    const auto installed =
        (*catalog)->installArchive(archivePath(temporary.path(), QStringLiteral("base")),
                                   QStringLiteral("2026-08-11T01:00:00Z"));
    QVERIFY(installed.has_value());

    const auto stored_path = QDir((*catalog)->archivesDirectory())
                                 .filePath(installed->archive_sha256 + QStringLiteral(".awpack"));
    QFile corrupt(stored_path);
    QVERIFY(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(corrupt.write("corrupt"), qint64{7});
    corrupt.close();

    const auto loaded = (*catalog)->load(revision->id, revision->version);
    QVERIFY(!loaded.has_value());
    QCOMPARE(loaded.error().code, CatalogErrorCode::CorruptCatalog);
}

void PackCatalogTest::materializesStableBlobForExactRevision() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto revision =
        buildArchive(temporary.path(), QStringLiteral("blob"), QStringLiteral("test.pack.blob"),
                     QStringLiteral("1.0.0"), QStringLiteral("Blob Composite"), {}, true);
    if (!revision) {
        QFAIL(qPrintable(revision.error()));
    }
    auto catalog = PackCatalog::open(QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    QVERIFY(catalog.has_value());
    const auto installed =
        (*catalog)->installArchive(archivePath(temporary.path(), QStringLiteral("blob")),
                                   QStringLiteral("2026-08-11T04:00:00Z"));
    QVERIFY(installed.has_value());

    const auto expected_digest = QString::fromLatin1(sha256(testPdf()));
    const auto installed_object =
        QDir((*catalog)->blobObjectsDirectory()).filePath(expected_digest);
    QVERIFY(QFileInfo(installed_object).isFile());
    QVERIFY(!QFileInfo(installed_object).isSymLink());

    const auto first = (*catalog)->materializeBlob(*revision, "objects/document.pdf");
    QVERIFY(first.has_value());
    QCOMPARE(first->local_path, installed_object);
    QCOMPARE(QString::fromStdString(first->descriptor.path),
             QStringLiteral("objects/document.pdf"));
    QCOMPARE(QString::fromStdString(first->descriptor.media_type),
             QStringLiteral("application/pdf"));
    QCOMPARE(first->descriptor.byte_size, static_cast<std::uint64_t>(testPdf().size()));
    QCOMPARE(QString::fromStdString(first->descriptor.sha256), expected_digest);

    const auto second = (*catalog)->materializeBlob(*revision, "objects/document.pdf");
    QVERIFY(second.has_value());
    QCOMPARE(*second, *first);
    QFile opened(second->local_path);
    QVERIFY(opened.open(QIODevice::ReadOnly));
    QCOMPARE(opened.readAll(), testPdf());

    auto wrong_revision = *revision;
    wrong_revision.digest.front() = wrong_revision.digest.front() == '0' ? '1' : '0';
    const auto wrong = (*catalog)->materializeBlob(wrong_revision, "objects/document.pdf");
    QVERIFY(!wrong.has_value());
    QCOMPARE(wrong.error().code, CatalogErrorCode::NotFound);
    const auto unknown = (*catalog)->materializeBlob(*revision, "objects/unknown.pdf");
    QVERIFY(!unknown.has_value());
    QCOMPARE(unknown.error().code, CatalogErrorCode::NotFound);
}

void PackCatalogTest::rehydratesMissingBlobButNeverOverwritesCorruption() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto revision = buildArchive(
        temporary.path(), QStringLiteral("rehydrate"), QStringLiteral("test.pack.rehydrate"),
        QStringLiteral("1.0.0"), QStringLiteral("Rehydrate Composite"), {}, true);
    QVERIFY(revision.has_value());
    auto catalog = PackCatalog::open(QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    QVERIFY(catalog.has_value());
    QVERIFY((*catalog)
                ->installArchive(archivePath(temporary.path(), QStringLiteral("rehydrate")),
                                 QStringLiteral("2026-08-11T05:00:00Z"))
                .has_value());
    const auto initial = (*catalog)->materializeBlob(*revision, "objects/document.pdf");
    QVERIFY(initial.has_value());
    QVERIFY(QFile::remove(initial->local_path));
    const auto interrupted =
        QDir((*catalog)->blobObjectsDirectory()).filePath(QStringLiteral(".blob-interrupted.tmp"));
    QVERIFY(writeAll(interrupted, QByteArray("partial")));

    const auto rehydrated = (*catalog)->materializeBlob(*revision, "objects/document.pdf");
    QVERIFY(rehydrated.has_value());
    QCOMPARE(rehydrated->local_path, initial->local_path);
    QVERIFY(QFileInfo::exists(interrupted));
    QFile restored(rehydrated->local_path);
    QVERIFY(restored.open(QIODevice::ReadOnly));
    QCOMPARE(restored.readAll(), testPdf());
    restored.close();

    QFile corrupt(rehydrated->local_path);
    QVERIFY(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(corrupt.write("corrupt"), qint64{7});
    corrupt.close();
    const auto refused = (*catalog)->materializeBlob(*revision, "objects/document.pdf");
    QVERIFY(!refused.has_value());
    QCOMPARE(refused.error().code, CatalogErrorCode::CorruptCatalog);
    QFile unchanged(rehydrated->local_path);
    QVERIFY(unchanged.open(QIODevice::ReadOnly));
    QCOMPARE(unchanged.readAll(), QByteArray("corrupt"));

    unchanged.close();
    QVERIFY(QFile::remove(rehydrated->local_path));
    QVERIFY(QFile::link(archivePath(temporary.path(), QStringLiteral("rehydrate")),
                        rehydrated->local_path));
    const auto linked = (*catalog)->materializeBlob(*revision, "objects/document.pdf");
    QVERIFY(!linked.has_value());
    QCOMPARE(linked.error().code, CatalogErrorCode::CorruptCatalog);
}

void PackCatalogTest::deduplicatesBlobsAndRequiresVerifiedArchives() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto first = buildArchive(temporary.path(), QStringLiteral("first-blob"),
                                    QStringLiteral("test.pack.first-blob"), QStringLiteral("1.0.0"),
                                    QStringLiteral("First Blob Composite"), {}, true);
    const auto second = buildArchive(
        temporary.path(), QStringLiteral("second-blob"), QStringLiteral("test.pack.second-blob"),
        QStringLiteral("1.0.0"), QStringLiteral("Second Blob Composite"), {}, true);
    QVERIFY(first.has_value());
    QVERIFY(second.has_value());
    auto catalog = PackCatalog::open(QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    QVERIFY(catalog.has_value());
    const auto installed_first =
        (*catalog)->installArchive(archivePath(temporary.path(), QStringLiteral("first-blob")),
                                   QStringLiteral("2026-08-11T06:00:00Z"));
    const auto installed_second =
        (*catalog)->installArchive(archivePath(temporary.path(), QStringLiteral("second-blob")),
                                   QStringLiteral("2026-08-11T07:00:00Z"));
    QVERIFY(installed_first.has_value());
    QVERIFY(installed_second.has_value());
    const auto first_blob = (*catalog)->materializeBlob(*first, "objects/document.pdf");
    const auto second_blob = (*catalog)->materializeBlob(*second, "objects/document.pdf");
    QVERIFY(first_blob.has_value());
    QVERIFY(second_blob.has_value());
    QCOMPARE(first_blob->local_path, second_blob->local_path);
    QCOMPARE(QDir((*catalog)->blobObjectsDirectory())
                 .entryList(QDir::Files | QDir::NoDotAndDotDot)
                 .size(),
             1);

    const auto first_archive =
        QDir((*catalog)->archivesDirectory())
            .filePath(installed_first->archive_sha256 + QStringLiteral(".awpack"));
    QVERIFY(QFile::remove(first_archive));
    const auto intact_without_archive = (*catalog)->materializeBlob(*first, "objects/document.pdf");
    QVERIFY(intact_without_archive.has_value());
    QCOMPARE(intact_without_archive->local_path, first_blob->local_path);
    QVERIFY(QFile::remove(first_blob->local_path));
    const auto missing_object_and_archive =
        (*catalog)->materializeBlob(*first, "objects/document.pdf");
    QVERIFY(!missing_object_and_archive.has_value());
    QCOMPARE(missing_object_and_archive.error().code, CatalogErrorCode::CorruptCatalog);

    const auto rehydrated = (*catalog)->materializeBlob(*second, "objects/document.pdf");
    QVERIFY(rehydrated.has_value());

    const auto second_archive =
        QDir((*catalog)->archivesDirectory())
            .filePath(installed_second->archive_sha256 + QStringLiteral(".awpack"));
    QFile corrupt(second_archive);
    QVERIFY(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(corrupt.write("corrupt"), qint64{7});
    corrupt.close();
    const auto intact_with_corrupt_archive =
        (*catalog)->materializeBlob(*second, "objects/document.pdf");
    QVERIFY(intact_with_corrupt_archive.has_value());
    QVERIFY(QFile::remove(rehydrated->local_path));
    const auto missing_object_and_corrupt_archive =
        (*catalog)->materializeBlob(*second, "objects/document.pdf");
    QVERIFY(!missing_object_and_corrupt_archive.has_value());
    QCOMPARE(missing_object_and_corrupt_archive.error().code, CatalogErrorCode::CorruptCatalog);
}

void PackCatalogTest::refusesInstallOverCorruptBlobObject() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto revision = buildArchive(temporary.path(), QStringLiteral("blocked"),
                                       QStringLiteral("test.pack.blocked"), QStringLiteral("1.0.0"),
                                       QStringLiteral("Blocked Composite"), {}, true);
    QVERIFY(revision.has_value());
    auto catalog = PackCatalog::open(QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    QVERIFY(catalog.has_value());
    const auto object_path =
        QDir((*catalog)->blobObjectsDirectory()).filePath(QString::fromLatin1(sha256(testPdf())));
    QVERIFY(writeAll(object_path, QByteArray("untrusted")));
    const auto installed =
        (*catalog)->installArchive(archivePath(temporary.path(), QStringLiteral("blocked")),
                                   QStringLiteral("2026-08-11T08:00:00Z"));
    QVERIFY(!installed.has_value());
    QCOMPARE(installed.error().code, CatalogErrorCode::CannotStoreBlob);
    const auto listed = (*catalog)->list();
    QVERIFY(listed.has_value());
    QVERIFY(listed->empty());
    QFile unchanged(object_path);
    QVERIFY(unchanged.open(QIODevice::ReadOnly));
    QCOMPARE(unchanged.readAll(), QByteArray("untrusted"));
}

void PackCatalogTest::detectsTamperedBlobDescriptor() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto revision =
        buildArchive(temporary.path(), QStringLiteral("tampered-descriptor"),
                     QStringLiteral("test.pack.tampered-descriptor"), QStringLiteral("1.0.0"),
                     QStringLiteral("Tampered Descriptor Composite"), {}, true);
    QVERIFY(revision.has_value());
    const auto catalog_root = QDir(temporary.path()).filePath(QStringLiteral("catalog"));
    QString object_path;
    {
        auto catalog = PackCatalog::open(catalog_root);
        QVERIFY(catalog.has_value());
        QVERIFY((*catalog)
                    ->installArchive(
                        archivePath(temporary.path(), QStringLiteral("tampered-descriptor")),
                        QStringLiteral("2026-08-11T09:00:00Z"))
                    .has_value());
        const auto blob = (*catalog)->materializeBlob(*revision, "objects/document.pdf");
        QVERIFY(blob.has_value());
        object_path = blob->local_path;
    }
    QVERIFY(executeCatalogSql(
        catalog_root,
        {QStringLiteral("UPDATE pack_blobs SET byte_size = byte_size - 1 WHERE pack_id = "
                        "'test.pack.tampered-descriptor' AND version = '1.0.0'")}));

    auto catalog = PackCatalog::open(catalog_root);
    QVERIFY(catalog.has_value());
    const auto result = (*catalog)->materializeBlob(*revision, "objects/document.pdf");
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, CatalogErrorCode::CorruptCatalog);
    QFile unchanged(object_path);
    QVERIFY(unchanged.open(QIODevice::ReadOnly));
    QCOMPARE(unchanged.readAll(), testPdf());
}

void PackCatalogTest::migratesV1BlobDescriptors() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto revision = buildArchive(
        temporary.path(), QStringLiteral("migration"), QStringLiteral("test.pack.migration"),
        QStringLiteral("1.0.0"), QStringLiteral("Migration Composite"), {}, true);
    QVERIFY(revision.has_value());
    const auto catalog_root = QDir(temporary.path()).filePath(QStringLiteral("catalog"));
    QString archive_path;
    {
        auto catalog = PackCatalog::open(catalog_root);
        QVERIFY(catalog.has_value());
        const auto installed =
            (*catalog)->installArchive(archivePath(temporary.path(), QStringLiteral("migration")),
                                       QStringLiteral("2026-08-11T10:00:00Z"));
        QVERIFY(installed.has_value());
        archive_path = QDir((*catalog)->archivesDirectory())
                           .filePath(installed->archive_sha256 + QStringLiteral(".awpack"));
    }
    QVERIFY(executeCatalogSql(
        catalog_root,
        {QStringLiteral("DELETE FROM pack_blob_sets"), QStringLiteral("DELETE FROM pack_blobs"),
         QStringLiteral("DELETE FROM catalog_migrations WHERE version = 2")}));

    auto migrated = PackCatalog::open(catalog_root);
    QVERIFY(migrated.has_value());
    QCOMPARE((*migrated)->schemaVersion(), 2);
    QVERIFY(QFile::remove(archive_path));
    const auto blob = (*migrated)->materializeBlob(*revision, "objects/document.pdf");
    QVERIFY(blob.has_value());
    QFile opened(blob->local_path);
    QVERIFY(opened.open(QIODevice::ReadOnly));
    QCOMPARE(opened.readAll(), testPdf());
}

} // namespace

QTEST_GUILESS_MAIN(PackCatalogTest)

#include "tst_pack_catalog.moc"
