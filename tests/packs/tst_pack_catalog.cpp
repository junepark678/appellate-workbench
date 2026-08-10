#include "appellate/packs/pack_archive.hpp"
#include "appellate/packs/pack_catalog.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

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
};

[[nodiscard]] QByteArray sha256(const QByteArray& bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

[[nodiscard]] bool writeAll(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
           file.write(bytes) == bytes.size();
}

[[nodiscard]] auto dependencyJson(const PackRevision& revision) -> QJsonObject {
    return QJsonObject{
        {QStringLiteral("pack_id"), QString::fromStdString(revision.id.value)},
        {QStringLiteral("version"), QString::fromStdString(revision.version)},
        {QStringLiteral("sha256"), QString::fromStdString(revision.digest)},
    };
}

[[nodiscard]] auto buildArchive(const QString& root, const QString& stem,
                                const QString& pack_id, const QString& version,
                                const QString& display_name,
                                const std::vector<PackRevision>& dependencies = {})
    -> std::expected<PackRevision, QString> {
    const auto source = QDir(root).filePath(QStringLiteral("sources/") + stem);
    const auto archive_directory = QDir(root).filePath(QStringLiteral("exports"));
    if (!QDir{}.mkpath(QDir(source).filePath(QStringLiteral("judges"))) ||
        !QDir{}.mkpath(archive_directory)) {
        return std::unexpected(QStringLiteral("cannot create pack directories"));
    }

    const auto profile_id = pack_id + QStringLiteral(".judge.measured");
    const auto profile = QJsonDocument(QJsonObject{
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
             {QStringLiteral("verbosity"), 0.5},
             {QStringLiteral("sentence_complexity"), 0.5},
         }},
    }).toJson(QJsonDocument::Compact);
    const auto profile_path = QDir(source).filePath(QStringLiteral("judges/measured.json"));
    if (!writeAll(profile_path, profile)) {
        return std::unexpected(QStringLiteral("cannot write profile"));
    }

    QJsonArray dependency_array;
    for (const auto& dependency : dependencies) {
        dependency_array.push_back(dependencyJson(dependency));
    }
    const auto manifest = QJsonDocument(QJsonObject{
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("pack_id"), pack_id},
        {QStringLiteral("version"), version},
        {QStringLiteral("required_capabilities"),
         QJsonArray{
             QJsonObject{{QStringLiteral("id"), QStringLiteral("workbench.pack.judge-profile")},
                         {QStringLiteral("version"), 1}},
             QJsonObject{{QStringLiteral("id"), QStringLiteral("workbench.pack.voice-style")},
                         {QStringLiteral("version"), 1}},
         }},
        {QStringLiteral("dependencies"), dependency_array},
        {QStringLiteral("contents"),
         QJsonArray{QJsonObject{
             {QStringLiteral("id"), profile_id},
             {QStringLiteral("kind"), QStringLiteral("judge_profile")},
             {QStringLiteral("schema_version"), 1},
             {QStringLiteral("path"), QStringLiteral("judges/measured.json")},
             {QStringLiteral("sha256"), QString::fromLatin1(sha256(profile))},
         }}},
    }).toJson(QJsonDocument::Compact);
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
    const auto base = buildArchive(temporary.path(), QStringLiteral("base"),
                                   QStringLiteral("test.pack.base"), QStringLiteral("1.0.0"),
                                   QStringLiteral("Base Composite"));
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
    QCOMPARE((*catalog)->schemaVersion(), 1);
    const auto installed_base = (*catalog)->installArchive(
        archivePath(temporary.path(), QStringLiteral("base")),
        QStringLiteral("2026-08-11T01:00:00Z"));
    QVERIFY(installed_base.has_value());
    QVERIFY(installed_base->revision == *base);
    QVERIFY(QFileInfo::exists(QDir((*catalog)->archivesDirectory())
                                 .filePath(installed_base->archive_sha256 +
                                           QStringLiteral(".awpack"))));

    const auto installed_dependent = (*catalog)->installArchive(
        archivePath(temporary.path(), QStringLiteral("dependent")),
        QStringLiteral("2026-08-11T02:00:00Z"));
    QVERIFY(installed_dependent.has_value());
    QCOMPARE(installed_dependent->dependencies.size(), std::size_t{1});
    QVERIFY(installed_dependent->dependencies.front().revision == *base);

    const auto idempotent = (*catalog)->installArchive(
        archivePath(temporary.path(), QStringLiteral("dependent")),
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
        QStringLiteral("1.0.0"), QStringLiteral("Dependent Composite"), {missing});
    QVERIFY(dependent.has_value());
    auto catalog = PackCatalog::open(QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    QVERIFY(catalog.has_value());

    const auto installed = (*catalog)->installArchive(
        archivePath(temporary.path(), QStringLiteral("dependent")),
        QStringLiteral("2026-08-11T02:00:00Z"));
    QVERIFY(!installed.has_value());
    QCOMPARE(installed.error().code, CatalogErrorCode::MissingDependency);
    const auto listed = (*catalog)->list();
    QVERIFY(listed.has_value());
    QVERIFY(listed->empty());
}

void PackCatalogTest::rejectsSelfCycleAndImmutableConflict() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const PackRevision self{
        appellate::model::PackId{"test.pack.cycle"},
        "1.0.0",
        std::string(64, 'b'),
    };
    const auto cycle = buildArchive(temporary.path(), QStringLiteral("cycle"),
                                    QStringLiteral("test.pack.cycle"),
                                    QStringLiteral("1.0.0"),
                                    QStringLiteral("Cycle Composite"), {self});
    // The semantic reader rejects a direct cycle before an archive can be produced. The catalog
    // repeats this guard for defense in depth when accepting future archive/schema versions.
    QVERIFY(!cycle.has_value());
    auto catalog = PackCatalog::open(QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    QVERIFY(catalog.has_value());

    const auto first = buildArchive(temporary.path(), QStringLiteral("first"),
                                    QStringLiteral("test.pack.conflict"),
                                    QStringLiteral("1.0.0"),
                                    QStringLiteral("First Composite"));
    const auto second = buildArchive(temporary.path(), QStringLiteral("second"),
                                     QStringLiteral("test.pack.conflict"),
                                     QStringLiteral("1.0.0"),
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
    const auto revision = buildArchive(temporary.path(), QStringLiteral("base"),
                                       QStringLiteral("test.pack.base"),
                                       QStringLiteral("1.0.0"),
                                       QStringLiteral("Base Composite"));
    QVERIFY(revision.has_value());
    auto catalog = PackCatalog::open(QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    QVERIFY(catalog.has_value());
    const auto installed = (*catalog)->installArchive(
        archivePath(temporary.path(), QStringLiteral("base")),
        QStringLiteral("2026-08-11T01:00:00Z"));
    QVERIFY(installed.has_value());

    const auto stored_path = QDir((*catalog)->archivesDirectory())
                                 .filePath(installed->archive_sha256 +
                                           QStringLiteral(".awpack"));
    QFile corrupt(stored_path);
    QVERIFY(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(corrupt.write("corrupt"), qint64{7});
    corrupt.close();

    const auto loaded = (*catalog)->load(revision->id, revision->version);
    QVERIFY(!loaded.has_value());
    QCOMPARE(loaded.error().code, CatalogErrorCode::CorruptCatalog);
}

} // namespace

QTEST_GUILESS_MAIN(PackCatalogTest)

#include "tst_pack_catalog.moc"
