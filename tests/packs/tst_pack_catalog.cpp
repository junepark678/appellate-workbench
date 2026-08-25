#include "appellate/packs/capability_registry.hpp"
#include "appellate/packs/pack_archive.hpp"
#include "appellate/packs/pack_catalog.hpp"
#include "pack_catalog_p.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include <algorithm>
#include <array>
#include <concepts>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

using appellate::model::PackRevision;
using appellate::packs::CatalogErrorCode;
using appellate::packs::PackArchive;
using appellate::packs::PackCatalog;
using appellate::packs::PackCatalogSnapshot;

static_assert(!std::is_default_constructible_v<PackCatalogSnapshot>);
static_assert(!std::is_copy_constructible_v<PackCatalogSnapshot>);
static_assert(!std::is_copy_assignable_v<PackCatalogSnapshot>);
static_assert(!std::is_move_constructible_v<PackCatalogSnapshot>);
static_assert(!std::is_move_assignable_v<PackCatalogSnapshot>);
static_assert(std::is_destructible_v<PackCatalogSnapshot>);

static_assert(std::same_as<
              decltype(PackCatalogSnapshot::openExisting(std::declval<const QString&>())),
              std::expected<std::unique_ptr<PackCatalogSnapshot>, appellate::packs::CatalogError>>);
static_assert(std::same_as<decltype(std::declval<const PackCatalogSnapshot&>().list()),
                           std::expected<std::vector<appellate::packs::InstalledPack>,
                                         appellate::packs::CatalogError>>);
static_assert(
    std::same_as<decltype(std::declval<const PackCatalogSnapshot&>().load(
                     std::declval<const appellate::model::PackId&>(),
                     std::declval<const std::string&>())),
                 std::expected<appellate::packs::LoadedPack, appellate::packs::CatalogError>>);
static_assert(
    std::same_as<decltype(std::declval<const PackCatalogSnapshot&>().loadResolved(
                     std::declval<const PackRevision&>())),
                 std::expected<appellate::packs::ResolvedPack, appellate::packs::CatalogError>>);
static_assert(std::same_as<
              decltype(appellate::packs::detail::PackCatalogSnapshotFactory::openExisting(
                  std::declval<const QString&>(),
                  std::declval<appellate::packs::detail::SecureScratchContext&&>())),
              std::expected<std::unique_ptr<PackCatalogSnapshot>, appellate::packs::CatalogError>>);

template <typename Snapshot>
concept SnapshotExposesInstall =
    requires(Snapshot& snapshot, const QString& archive, const QString& installed_at) {
        snapshot.installArchive(archive, installed_at);
    };

template <typename Snapshot>
concept SnapshotExposesMaterialization =
    requires(const Snapshot& snapshot, const PackRevision& revision) {
        snapshot.materializeBlob(revision, std::string{});
    };

template <typename Snapshot>
concept SnapshotExposesDirectory = requires(const Snapshot& snapshot) {
    snapshot.rootDirectory();
    snapshot.archivesDirectory();
    snapshot.blobObjectsDirectory();
};

template <typename Snapshot>
concept SnapshotExposesSchemaVersion =
    requires(const Snapshot& snapshot) { snapshot.schemaVersion(); };

static_assert(!SnapshotExposesInstall<PackCatalogSnapshot>);
static_assert(!SnapshotExposesMaterialization<PackCatalogSnapshot>);
static_assert(!SnapshotExposesDirectory<PackCatalogSnapshot>);
static_assert(!SnapshotExposesSchemaVersion<PackCatalogSnapshot>);

class PackCatalogTest final : public QObject {
    Q_OBJECT

  private slots:
    void opensImmutableCatalogSnapshot();
    void consumesSecureScratchContextOnce();
    void rejectsNonNormativeMigrationLedgerBeforeSourceSqlite();
    void cleansFailedWritableInitializationAndRetries();
    void rejectsReboundAttemptLockOnLateWritableOpenFailure();
    void anchorsRelativeWritableCatalogAcrossCurrentDirectoryChanges();
    void documentsCatalogRecoveryProcedure();
    void installsLoadsListsAndIsIdempotent();
    void requiresExactDependenciesWithoutPartialInstall();
    void rejectsSelfCycleAndImmutableConflict();
    void detectsCorruptInstalledArchive();
    void materializesStableBlobForExactRevision();
    void rehydratesMissingBlobButNeverOverwritesCorruption();
    void deduplicatesBlobsAndRequiresVerifiedArchives();
    void refusesInstallOverCorruptBlobObject();
    void detectsTamperedBlobDescriptor();
    void rejectsCorruptV1ArchiveWithoutMutation();
    void rollsBackV1MigrationBeforeCommitAndRetries();
    void preservesAppliedV1MigrationAfterReportedCommitFailure();
    void migratesV1BlobDescriptors();
    void negotiatesNewWorkflowCapabilityDeclarations();
    void resolvesCatalogPackWithExtendedWorkflowCapabilities();
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

[[nodiscard]] std::optional<QByteArray> readAll(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    return file.readAll();
}

[[nodiscard]] bool replaceAll(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(bytes) == bytes.size();
}

[[nodiscard]] bool syncDirectory(const QString& path) {
#if defined(Q_OS_UNIX)
    const auto encoded = QFile::encodeName(path);
    auto flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const auto descriptor = ::open(encoded.constData(), flags);
    if (descriptor < 0) {
        return false;
    }
    int result{};
    do {
        result = ::fsync(descriptor);
    } while (result != 0 && errno == EINTR);
    const auto saved_errno = errno;
    static_cast<void>(::close(descriptor));
    errno = saved_errno;
    return result == 0;
#else
    Q_UNUSED(path);
    return true;
#endif
}

struct TreeClosure final {
    QStringList inventory;
    QMap<QString, QByteArray> stable_file_bytes;

    friend bool operator==(const TreeClosure&, const TreeClosure&) = default;
};

[[nodiscard]] std::optional<TreeClosure> captureTreeClosure(const QString& root) {
    if (!QFileInfo(root).isDir()) {
        return std::nullopt;
    }
    TreeClosure closure;
    const QDir root_directory(root);
    QDirIterator iterator(root,
                          QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const auto path = iterator.next();
        const QFileInfo info = iterator.fileInfo();
        const auto relative = root_directory.relativeFilePath(path);
        if (info.isSymLink()) {
            closure.inventory.push_back(QStringLiteral("l:") + relative);
            continue;
        }
        if (info.isDir()) {
            closure.inventory.push_back(QStringLiteral("d:") + relative);
            continue;
        }
        if (!info.isFile()) {
            closure.inventory.push_back(QStringLiteral("o:") + relative);
            continue;
        }
        closure.inventory.push_back(QStringLiteral("f:") + relative);
        // SQLite is allowed to mutate SHM bookkeeping even for an idle external reader. Its safe
        // identity and presence remain part of the inventory, but its bytes are not immutable.
        if (info.fileName() == QStringLiteral("catalog.sqlite-shm")) {
            continue;
        }
        const auto bytes = readAll(path);
        if (!bytes.has_value()) {
            return std::nullopt;
        }
        closure.stable_file_bytes.insert(relative, *bytes);
    }
    std::ranges::sort(closure.inventory);
    return closure;
}

[[nodiscard]] bool sameLoadedPack(const appellate::packs::LoadedPack& left,
                                  const appellate::packs::LoadedPack& right) {
    if (left.manifest_schema_version != right.manifest_schema_version ||
        left.revision != right.revision ||
        left.required_capabilities != right.required_capabilities ||
        left.dependencies != right.dependencies || left.blobs != right.blobs ||
        left.judge_profiles != right.judge_profiles || left.graph_state != right.graph_state ||
        left.resources.size() != right.resources.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.resources.size(); ++index) {
        if (left.resources.at(index).descriptor != right.resources.at(index).descriptor ||
            left.resources.at(index).document != right.resources.at(index).document) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool sameResolvedPack(const appellate::packs::ResolvedPack& left,
                                    const appellate::packs::ResolvedPack& right) {
    if (!sameLoadedPack(left.root(), right.root()) ||
        left.dependenciesDependencyFirst().size() != right.dependenciesDependencyFirst().size() ||
        !std::ranges::equal(left.revisionsByPackId(), right.revisionsByPackId())) {
        return false;
    }
    for (std::size_t index = 0; index < left.dependenciesDependencyFirst().size(); ++index) {
        if (!sameLoadedPack(left.dependenciesDependencyFirst()[index],
                            right.dependenciesDependencyFirst()[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool copyTree(const QString& source, const QString& destination) {
    const QDir root(source);
    QDirIterator iterator(source, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const auto source_path = iterator.next();
        QFile input(source_path);
        const auto destination_path =
            QDir(destination).filePath(root.relativeFilePath(source_path));
        if (!input.open(QIODevice::ReadOnly) ||
            !QDir{}.mkpath(QFileInfo(destination_path).absolutePath()) ||
            !writeAll(destination_path, input.readAll())) {
            return false;
        }
    }
    return true;
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

[[nodiscard]] bool createTrueV1Catalog(const QString& catalog_root, const QString& archive_source,
                                       const PackRevision& revision,
                                       const QString& installed_at_utc) {
    const auto archive_bytes = readAll(archive_source);
    const auto loaded = PackArchive::importArchive(
        archive_source, {}, appellate::packs::PackValidationScope::ResolvedClosure);
    if (!archive_bytes.has_value() || !loaded.has_value() || loaded->revision != revision ||
        !loaded->dependencies.empty()) {
        return false;
    }
    const auto archive_sha = QString::fromLatin1(sha256(*archive_bytes));
    const auto archives = QDir(catalog_root).filePath(QStringLiteral("archives"));
    if (!QDir{}.mkpath(archives) ||
        QFileInfo::exists(QDir(catalog_root).filePath(QStringLiteral("blobs")))) {
        return false;
    }
    const auto archived = QDir(archives).filePath(archive_sha + QStringLiteral(".awpack"));
    if (!QFile::copy(archive_source, archived)) {
        return false;
    }

    const auto connection_name =
        QStringLiteral("catalog-v1-test-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    bool succeeded = false;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name);
        database.setDatabaseName(QDir(catalog_root).filePath(QStringLiteral("catalog.sqlite")));
        succeeded = database.open();
        const auto execute = [&database, &succeeded](const QString& statement) {
            if (!succeeded) {
                return;
            }
            QSqlQuery query(database);
            succeeded = query.exec(statement);
        };
        execute(QStringLiteral("PRAGMA foreign_keys = ON"));
        execute(QStringLiteral("PRAGMA journal_mode = WAL"));
        execute(QStringLiteral("PRAGMA synchronous = FULL"));
        execute(QStringLiteral("PRAGMA busy_timeout = 5000"));
        execute(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS catalog_migrations (version INTEGER PRIMARY KEY, "
            "applied_at_utc TEXT NOT NULL) STRICT"));
        execute(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS pack_revisions ("
            "pack_id TEXT NOT NULL, version TEXT NOT NULL, digest TEXT NOT NULL, "
            "archive_sha256 TEXT NOT NULL, installed_at_utc TEXT NOT NULL, "
            "PRIMARY KEY(pack_id, version), UNIQUE(pack_id, version, digest)) STRICT"));
        execute(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS pack_dependencies ("
            "pack_id TEXT NOT NULL, version TEXT NOT NULL, dependency_pack_id TEXT NOT NULL, "
            "dependency_version TEXT NOT NULL, dependency_digest TEXT NOT NULL, "
            "PRIMARY KEY(pack_id, version, dependency_pack_id, dependency_version), "
            "FOREIGN KEY(pack_id, version) REFERENCES pack_revisions(pack_id, version) "
            "ON DELETE CASCADE, FOREIGN KEY(dependency_pack_id, dependency_version, "
            "dependency_digest) REFERENCES pack_revisions(pack_id, version, digest) "
            "ON DELETE RESTRICT) STRICT"));
        if (succeeded) {
            QSqlQuery migration(database);
            migration.prepare(QStringLiteral(
                "INSERT INTO catalog_migrations(version, applied_at_utc) VALUES(1, ?)"));
            migration.addBindValue(QStringLiteral("2026-08-11T00:00:00Z"));
            succeeded = migration.exec();
        }
        if (succeeded) {
            QSqlQuery pack(database);
            pack.prepare(QStringLiteral(
                "INSERT INTO pack_revisions(pack_id, version, digest, archive_sha256, "
                "installed_at_utc) VALUES(?, ?, ?, ?, ?)"));
            pack.addBindValue(QString::fromStdString(revision.id.value));
            pack.addBindValue(QString::fromStdString(revision.version));
            pack.addBindValue(QString::fromStdString(revision.digest));
            pack.addBindValue(archive_sha);
            pack.addBindValue(installed_at_utc);
            succeeded = pack.exec();
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection_name);
    return succeeded && QFileInfo::exists(archived) &&
           !QFileInfo::exists(QDir(catalog_root).filePath(QStringLiteral("blobs"))) &&
           syncDirectory(archives) && syncDirectory(catalog_root);
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

void PackCatalogTest::opensImmutableCatalogSnapshot() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto leaf = buildArchive(temporary.path(), QStringLiteral("snapshot-leaf"),
                                   QStringLiteral("test.snapshot.leaf"), QStringLiteral("1.0.0"),
                                   QStringLiteral("Snapshot Leaf"));
    QVERIFY(leaf.has_value());
    const auto middle_one =
        buildArchive(temporary.path(), QStringLiteral("snapshot-middle-one"),
                     QStringLiteral("test.snapshot.middle-one"), QStringLiteral("1.0.0"),
                     QStringLiteral("Snapshot Middle One"), {*leaf});
    QVERIFY(middle_one.has_value());
    const auto middle_two =
        buildArchive(temporary.path(), QStringLiteral("snapshot-middle-two"),
                     QStringLiteral("test.snapshot.middle-two"), QStringLiteral("1.0.0"),
                     QStringLiteral("Snapshot Middle Two"), {*middle_one});
    QVERIFY(middle_two.has_value());
    const auto root = buildArchive(temporary.path(), QStringLiteral("snapshot-root"),
                                   QStringLiteral("test.snapshot.root"), QStringLiteral("1.0.0"),
                                   QStringLiteral("Snapshot Root"), {*middle_two});
    QVERIFY(root.has_value());

    const auto catalog_root = QDir(temporary.path()).filePath(QStringLiteral("snapshot-catalog"));
    std::vector<appellate::packs::InstalledPack> expected_list;
    std::optional<appellate::packs::LoadedPack> expected_loaded;
    std::optional<appellate::packs::ResolvedPack> expected_resolved;
    {
        auto catalog = PackCatalog::open(catalog_root);
        QVERIFY2(catalog.has_value(), catalog ? "" : qPrintable(catalog.error().message));
        const std::array fixtures{
            std::pair{QStringLiteral("snapshot-leaf"), QStringLiteral("2026-08-11T01:00:00Z")},
            std::pair{QStringLiteral("snapshot-middle-one"),
                      QStringLiteral("2026-08-11T02:00:00Z")},
            std::pair{QStringLiteral("snapshot-middle-two"),
                      QStringLiteral("2026-08-11T03:00:00Z")},
            std::pair{QStringLiteral("snapshot-root"), QStringLiteral("2026-08-11T04:00:00Z")},
        };
        for (const auto& [stem, installed_at] : fixtures) {
            const auto installed =
                (*catalog)->installArchive(archivePath(temporary.path(), stem), installed_at);
            QVERIFY2(installed.has_value(), installed ? "" : qPrintable(installed.error().message));
        }
        const auto listed = (*catalog)->list();
        QVERIFY(listed.has_value());
        expected_list = *listed;
        const auto loaded = (*catalog)->load(root->id, root->version);
        QVERIFY(loaded.has_value());
        expected_loaded.emplace(std::move(*loaded));
        const auto resolved = (*catalog)->loadResolved(*root);
        QVERIFY(resolved.has_value());
        QCOMPARE(resolved->revisionsByPackId().size(), std::size_t{4});
        expected_resolved.emplace(std::move(*resolved));
    }

    const auto source_before = captureTreeClosure(catalog_root);
    QVERIFY(source_before.has_value());
    auto snapshot = PackCatalogSnapshot::openExisting(catalog_root);
    QVERIFY2(snapshot.has_value(), snapshot ? "" : qPrintable(snapshot.error().message));
    const auto listed = (*snapshot)->list();
    QVERIFY(listed.has_value());
    QVERIFY(*listed == expected_list);
    const auto loaded = (*snapshot)->load(root->id, root->version);
    QVERIFY(loaded.has_value());
    QVERIFY(sameLoadedPack(*loaded, *expected_loaded));
    const auto resolved = (*snapshot)->loadResolved(*root);
    QVERIFY(resolved.has_value());
    QVERIFY(sameResolvedPack(*resolved, *expected_resolved));
    const auto source_while_live = captureTreeClosure(catalog_root);
    QVERIFY(source_while_live.has_value());
    QVERIFY(*source_while_live == *source_before);
    snapshot->reset();
    const auto source_after = captureTreeClosure(catalog_root);
    QVERIFY(source_after.has_value());
    QVERIFY(*source_after == *source_before);

    const auto missing_root =
        QDir(temporary.path()).filePath(QStringLiteral("missing-snapshot-catalog"));
    const auto missing = PackCatalogSnapshot::openExisting(missing_root);
    QVERIFY(!missing.has_value());
    QCOMPARE(missing.error().code, CatalogErrorCode::CannotOpen);
    QVERIFY(!QFileInfo::exists(missing_root));

    const auto empty_root =
        QDir(temporary.path()).filePath(QStringLiteral("empty-snapshot-catalog"));
    QVERIFY(QDir{}.mkdir(empty_root));
    const auto empty = PackCatalogSnapshot::openExisting(empty_root);
    QVERIFY(!empty.has_value());
    QCOMPARE(empty.error().code, CatalogErrorCode::UninitializedCatalog);
    QVERIFY(
        QDir(empty_root).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).empty());

    const auto partial_root =
        QDir(temporary.path()).filePath(QStringLiteral("partial-snapshot-catalog"));
    QVERIFY(QDir{}.mkpath(QDir(partial_root).filePath(QStringLiteral("archives"))));
    const auto partial = PackCatalogSnapshot::openExisting(partial_root);
    QVERIFY(!partial.has_value());
    QCOMPARE(partial.error().code, CatalogErrorCode::CorruptCatalog);
    QVERIFY(!QFileInfo::exists(QDir(partial_root).filePath(QStringLiteral("catalog.sqlite"))));

    const auto v1_revision = buildArchive(
        temporary.path(), QStringLiteral("snapshot-v1"), QStringLiteral("test.snapshot.v1"),
        QStringLiteral("1.0.0"), QStringLiteral("Snapshot V1"), {}, true);
    QVERIFY(v1_revision.has_value());
    const auto v1_root = QDir(temporary.path()).filePath(QStringLiteral("snapshot-v1-catalog"));
    QVERIFY(createTrueV1Catalog(v1_root,
                                archivePath(temporary.path(), QStringLiteral("snapshot-v1")),
                                *v1_revision, QStringLiteral("2026-08-11T05:00:00Z")));
    const auto v1_before = captureTreeClosure(v1_root);
    QVERIFY(v1_before.has_value());
    const auto v1 = PackCatalogSnapshot::openExisting(v1_root);
    QVERIFY(!v1.has_value());
    QCOMPARE(v1.error().code, CatalogErrorCode::UninitializedCatalog);
    const auto v1_after = captureTreeClosure(v1_root);
    QVERIFY(v1_after.has_value());
    QVERIFY(*v1_after == *v1_before);
    QVERIFY(!QFileInfo::exists(QDir(v1_root).filePath(QStringLiteral("blobs"))));

    const auto corrupt_revision =
        buildArchive(temporary.path(), QStringLiteral("snapshot-corrupt"),
                     QStringLiteral("test.snapshot.corrupt"), QStringLiteral("1.0.0"),
                     QStringLiteral("Snapshot Corrupt"));
    QVERIFY(corrupt_revision.has_value());
    const auto corrupt_root =
        QDir(temporary.path()).filePath(QStringLiteral("snapshot-corrupt-catalog"));
    QString corrupt_archive_path;
    {
        auto catalog = PackCatalog::open(corrupt_root);
        QVERIFY(catalog.has_value());
        const auto installed = (*catalog)->installArchive(
            archivePath(temporary.path(), QStringLiteral("snapshot-corrupt")),
            QStringLiteral("2026-08-11T06:00:00Z"));
        QVERIFY(installed.has_value());
        corrupt_archive_path = QDir((*catalog)->archivesDirectory())
                                   .filePath(installed->archive_sha256 + QStringLiteral(".awpack"));
    }
    QVERIFY(replaceAll(corrupt_archive_path, QByteArray("corrupt")));
    const auto corrupt = PackCatalogSnapshot::openExisting(corrupt_root);
    QVERIFY(!corrupt.has_value());
    QCOMPARE(corrupt.error().code, CatalogErrorCode::CorruptCatalog);
    QVERIFY(readAll(corrupt_archive_path) == std::optional<QByteArray>{QByteArray("corrupt")});

    const auto lazy_revision =
        buildArchive(temporary.path(), QStringLiteral("snapshot-lazy-blob"),
                     QStringLiteral("test.snapshot.lazy-blob"), QStringLiteral("1.0.0"),
                     QStringLiteral("Snapshot Lazy Blob"), {}, true);
    QVERIFY(lazy_revision.has_value());
    const auto lazy_root = QDir(temporary.path()).filePath(QStringLiteral("snapshot-lazy-catalog"));
    QString lazy_object;
    {
        auto catalog = PackCatalog::open(lazy_root);
        QVERIFY(catalog.has_value());
        QVERIFY((*catalog)
                    ->installArchive(
                        archivePath(temporary.path(), QStringLiteral("snapshot-lazy-blob")),
                        QStringLiteral("2026-08-11T07:00:00Z"))
                    .has_value());
        lazy_object = QDir((*catalog)->blobObjectsDirectory())
                          .filePath(QString::fromLatin1(sha256(testPdf())));
        QVERIFY(QFile::remove(lazy_object));
    }
    auto lazy = PackCatalogSnapshot::openExisting(lazy_root);
    QVERIFY2(lazy.has_value(), lazy ? "" : qPrintable(lazy.error().message));
    const auto lazy_loaded = (*lazy)->load(lazy_revision->id, lazy_revision->version);
    QVERIFY(lazy_loaded.has_value());
    QVERIFY(lazy_loaded->revision == *lazy_revision);
    QVERIFY(!QFileInfo::exists(lazy_object));

    const auto orphan_root =
        QDir(temporary.path()).filePath(QStringLiteral("snapshot-orphan-catalog"));
    QString orphan_path;
    {
        auto catalog = PackCatalog::open(orphan_root);
        QVERIFY(catalog.has_value());
        orphan_path = QDir((*catalog)->blobObjectsDirectory()).filePath(QString(64, u'a'));
    }
    QVERIFY(writeAll(orphan_path, QByteArray("unreferenced")));
    const auto orphan = PackCatalogSnapshot::openExisting(orphan_root);
    QVERIFY(!orphan.has_value());
    QCOMPARE(orphan.error().code, CatalogErrorCode::CorruptCatalog);
    QVERIFY(readAll(orphan_path) == std::optional<QByteArray>{QByteArray("unreferenced")});
}

void PackCatalogTest::consumesSecureScratchContextOnce() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto catalog_root = QDir(temporary.path()).filePath(QStringLiteral("catalog"));
    {
        auto catalog = PackCatalog::open(catalog_root);
        QVERIFY2(catalog.has_value(), catalog ? "" : qPrintable(catalog.error().message));
    }

    std::size_t temp_path_captures{};
    appellate::packs::detail::SecureScratchHooks scratch_hooks;
    scratch_hooks.observe =
        [&temp_path_captures](const appellate::packs::detail::SecureScratchObservation& seen) {
            if (seen.event == appellate::packs::detail::SecureScratchEvent::TempPathCaptured) {
                ++temp_path_captures;
            }
        };
    auto context = appellate::packs::detail::acquireSecureScratchContext(scratch_hooks);
    QVERIFY2(context.has_value(), context ? "" : qPrintable(context.error().message));
    QVERIFY(context->isValid());
    QCOMPARE(temp_path_captures, std::size_t{1});

    appellate::packs::detail::CatalogReport first_report;
    appellate::packs::detail::CatalogHooks first_hooks;
    first_hooks.report = &first_report;
    auto snapshot = appellate::packs::detail::PackCatalogSnapshotFactory::openExisting(
        catalog_root, std::move(*context), std::move(first_hooks));
    QVERIFY2(snapshot.has_value(), snapshot ? "" : qPrintable(snapshot.error().message));
    QVERIFY(!context->isValid());
    QCOMPARE(temp_path_captures, std::size_t{1});
    QCOMPARE(first_report.scratch_acquisitions, std::size_t{1});
    const auto observed = [&first_report](appellate::packs::detail::CatalogEvent event,
                                          appellate::packs::detail::CatalogSubject subject) {
        return std::ranges::any_of(first_report.observations, [&](const auto& observation) {
            return observation.event == event && observation.subject == subject;
        });
    };
    QVERIFY(observed(appellate::packs::detail::CatalogEvent::MigrationStarted,
                     appellate::packs::detail::CatalogSubject::Version1Reference));
    QVERIFY(observed(appellate::packs::detail::CatalogEvent::MigrationStarted,
                     appellate::packs::detail::CatalogSubject::CurrentReference));
    QVERIFY(observed(appellate::packs::detail::CatalogEvent::QueryPlanVerified,
                     appellate::packs::detail::CatalogSubject::PrivateDatabaseMain));

    appellate::packs::detail::CatalogReport second_report;
    appellate::packs::detail::CatalogHooks second_hooks;
    second_hooks.report = &second_report;
    const auto moved_from = appellate::packs::detail::PackCatalogSnapshotFactory::openExisting(
        catalog_root, std::move(*context), std::move(second_hooks));
    QVERIFY(!moved_from.has_value());
    QCOMPARE(moved_from.error().code, CatalogErrorCode::CannotOpen);
    QCOMPARE(temp_path_captures, std::size_t{1});
    QVERIFY(second_report.observations.empty());
}

void PackCatalogTest::rejectsNonNormativeMigrationLedgerBeforeSourceSqlite() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto catalog_root = QDir(temporary.path()).filePath(QStringLiteral("catalog"));
    {
        auto catalog = PackCatalog::open(catalog_root);
        QVERIFY2(catalog.has_value(), catalog ? "" : qPrintable(catalog.error().message));
    }
    QVERIFY(executeCatalogSql(catalog_root,
                              {QStringLiteral("UPDATE catalog_migrations SET applied_at_utc = "
                                              "'2099-01-01T00:00:00Z' WHERE version = 2")}));
    const auto before = captureTreeClosure(catalog_root);
    QVERIFY(before.has_value());

    auto scratch_context = appellate::packs::detail::acquireSecureScratchContext();
    QVERIFY2(scratch_context.has_value(),
             scratch_context ? "" : qPrintable(scratch_context.error().message));
    appellate::packs::detail::CatalogReport report;
    appellate::packs::detail::CatalogHooks hooks;
    hooks.report = &report;
    const auto rejected = appellate::packs::detail::PackCatalogFactory::open(
        catalog_root, std::move(*scratch_context), std::move(hooks));
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, CatalogErrorCode::CorruptCatalog);
    QVERIFY(std::ranges::none_of(
        report.observations, [](const appellate::packs::detail::CatalogObservation& observation) {
            return observation.operation ==
                       appellate::packs::detail::CatalogOperation::WritableOpen &&
                   observation.event == appellate::packs::detail::CatalogEvent::DatabaseOpened &&
                   observation.subject ==
                       appellate::packs::detail::CatalogSubject::SQLiteConnection;
        }));

    const auto after = captureTreeClosure(catalog_root);
    QVERIFY(after.has_value());
    QVERIFY(*after == *before);
}

void PackCatalogTest::cleansFailedWritableInitializationAndRetries() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto catalog_root = QDir(temporary.path()).filePath(QStringLiteral("nested/catalog"));
    auto scratch = appellate::packs::detail::acquireSecureScratchContext();
    QVERIFY(scratch.has_value());
    appellate::packs::detail::CatalogReport report;
    appellate::packs::detail::CatalogHooks hooks;
    hooks.report = &report;
    hooks.inject = [](const appellate::packs::detail::CatalogObservation& observation) {
        return observation.operation == appellate::packs::detail::CatalogOperation::WritableOpen &&
                       observation.event == appellate::packs::detail::CatalogEvent::DatabaseOpened
                   ? appellate::packs::detail::CatalogInjectedAction::FailBefore
                   : appellate::packs::detail::CatalogInjectedAction::Continue;
    };
    const auto failed = appellate::packs::detail::PackCatalogFactory::open(
        catalog_root, std::move(*scratch), std::move(hooks));
    QVERIFY(!failed.has_value());
    QCOMPARE(failed.error().code, CatalogErrorCode::CannotOpen);
    QVERIFY(!QFileInfo::exists(catalog_root));
    QCOMPARE(report.cleanup, appellate::packs::detail::CatalogCleanupOutcome::Removed);
    QVERIFY(report.remaining_ledger_paths.empty());

    auto retried = PackCatalog::open(catalog_root);
    QVERIFY2(retried.has_value(), retried ? "" : qPrintable(retried.error().message));
    QCOMPARE((*retried)->schemaVersion(), 2);

    const auto empty_root = QDir(temporary.path()).filePath(QStringLiteral("preexisting-empty"));
    QVERIFY(QDir{}.mkdir(empty_root));
    QVERIFY(QFile::setPermissions(empty_root, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                  QFileDevice::ExeOwner));
    auto empty_scratch = appellate::packs::detail::acquireSecureScratchContext();
    QVERIFY(empty_scratch.has_value());
    appellate::packs::detail::CatalogReport empty_report;
    appellate::packs::detail::CatalogHooks empty_hooks;
    empty_hooks.report = &empty_report;
    empty_hooks.inject = [](const appellate::packs::detail::CatalogObservation& observation) {
        return observation.operation == appellate::packs::detail::CatalogOperation::WritableOpen &&
                       observation.event == appellate::packs::detail::CatalogEvent::DatabaseOpened
                   ? appellate::packs::detail::CatalogInjectedAction::FailBefore
                   : appellate::packs::detail::CatalogInjectedAction::Continue;
    };
    const auto empty_failed = appellate::packs::detail::PackCatalogFactory::open(
        empty_root, std::move(*empty_scratch), std::move(empty_hooks));
    QVERIFY(!empty_failed.has_value());
    QCOMPARE(empty_failed.error().code, CatalogErrorCode::CannotOpen);
    QVERIFY(QFileInfo(empty_root).isDir());
    QVERIFY(
        QDir(empty_root).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).empty());
    QCOMPARE(empty_report.cleanup, appellate::packs::detail::CatalogCleanupOutcome::Removed);
    QVERIFY(empty_report.remaining_ledger_paths.empty());

    auto empty_retried = PackCatalog::open(empty_root);
    QVERIFY2(empty_retried.has_value(),
             empty_retried ? "" : qPrintable(empty_retried.error().message));
    QCOMPARE((*empty_retried)->schemaVersion(), 2);
}

void PackCatalogTest::rejectsReboundAttemptLockOnLateWritableOpenFailure() {
#if !defined(Q_OS_UNIX)
    QSKIP("Attempt-lock identity testing requires POSIX rename semantics");
#else
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto catalog_root = QDir(temporary.path()).filePath(QStringLiteral("catalog"));
    {
        auto initialized = PackCatalog::open(catalog_root);
        QVERIFY2(initialized.has_value(),
                 initialized ? "" : qPrintable(initialized.error().message));
    }
    const auto before = captureTreeClosure(catalog_root);
    QVERIFY(before.has_value());
    const auto lock_path = QDir(catalog_root).filePath(QStringLiteral(".install.lock"));
    const auto remove_lock_path =
        QDir(catalog_root).filePath(QStringLiteral(".install.lock.rmlock"));
    const auto retained_attempt =
        QDir(catalog_root).filePath(QStringLiteral(".test-retained-attempt-lock"));
    const auto sentinel_bytes = QByteArray("replacement must survive forced teardown");
    bool mutation_succeeded{};
    std::optional<QByteArray> retained_bytes;

    auto scratch_context = appellate::packs::detail::acquireSecureScratchContext();
    QVERIFY2(scratch_context.has_value(),
             scratch_context ? "" : qPrintable(scratch_context.error().message));
    appellate::packs::detail::CatalogReport report;
    appellate::packs::detail::CatalogHooks hooks;
    hooks.report = &report;
    hooks.inject = [&](const appellate::packs::detail::CatalogObservation& observation) {
        if (!mutation_succeeded &&
            observation.operation == appellate::packs::detail::CatalogOperation::WritableOpen &&
            observation.subject == appellate::packs::detail::CatalogSubject::SQLiteConnection &&
            observation.event == appellate::packs::detail::CatalogEvent::DatabaseOpened &&
            observation.absolute_path ==
                QDir(catalog_root).filePath(QStringLiteral("catalog.sqlite"))) {
            const auto moved = QFile::rename(lock_path, retained_attempt);
            retained_bytes = readAll(retained_attempt);
            const auto replacement_created = QDir{}.mkdir(lock_path);
            const auto sentinel_path = QDir(lock_path).filePath(QStringLiteral("sentinel"));
            mutation_succeeded = moved && retained_bytes.has_value() &&
                                 !retained_bytes->isEmpty() && replacement_created &&
                                 writeAll(sentinel_path, sentinel_bytes) &&
                                 syncDirectory(lock_path) && syncDirectory(catalog_root);
            return appellate::packs::detail::CatalogInjectedAction::FailBefore;
        }
        return appellate::packs::detail::CatalogInjectedAction::Continue;
    };
    const auto rejected = appellate::packs::detail::PackCatalogFactory::open(
        catalog_root, std::move(*scratch_context), std::move(hooks));
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, CatalogErrorCode::CorruptCatalog);
    QVERIFY(mutation_succeeded);
    QVERIFY(retained_bytes.has_value());
    QVERIFY(readAll(retained_attempt) == retained_bytes);
    QCOMPARE(report.final_error, std::optional{CatalogErrorCode::CorruptCatalog});
    QVERIFY(report.forced_teardown);
    QVERIFY(report.residue_identity_ambiguous);
    QCOMPARE(std::ranges::count_if(
                 report.observations,
                 [](const appellate::packs::detail::CatalogObservation& observation) {
                     return observation.operation ==
                                appellate::packs::detail::CatalogOperation::WritableOpen &&
                            observation.event ==
                                appellate::packs::detail::CatalogEvent::ForcedTeardown;
                 }),
             std::ptrdiff_t{1});

    bool replacement_found{};
    for (const auto& candidate : {lock_path, remove_lock_path}) {
        const auto sentinel = QDir(candidate).filePath(QStringLiteral("sentinel"));
        if (QFileInfo(candidate).isDir() && readAll(sentinel) == sentinel_bytes) {
            replacement_found = true;
            QVERIFY(QFile::remove(sentinel));
            QVERIFY(QDir{}.rmdir(candidate));
        } else if (QFileInfo(candidate).isFile()) {
            QVERIFY(QFile::remove(candidate));
        }
    }
    QVERIFY(replacement_found);
    QVERIFY(readAll(retained_attempt) == retained_bytes);
    QVERIFY(QFile::remove(retained_attempt));
    QVERIFY(syncDirectory(catalog_root));

    const auto recovered = captureTreeClosure(catalog_root);
    QVERIFY(recovered.has_value());
    QVERIFY(*recovered == *before);
    auto retried = PackCatalog::open(catalog_root);
    QVERIFY2(retried.has_value(), retried ? "" : qPrintable(retried.error().message));
#endif
}

void PackCatalogTest::anchorsRelativeWritableCatalogAcrossCurrentDirectoryChanges() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto first = QDir(temporary.path()).filePath(QStringLiteral("first"));
    const auto decoy = QDir(temporary.path()).filePath(QStringLiteral("decoy"));
    QVERIFY(QDir{}.mkpath(first));
    QVERIFY(QDir{}.mkpath(decoy));
    const auto revision = buildArchive(
        temporary.path(), QStringLiteral("relative-root"), QStringLiteral("test.relative.root"),
        QStringLiteral("1.0.0"), QStringLiteral("Relative Root"), {}, true);
    QVERIFY(revision.has_value());

    const auto original_cwd = QDir::currentPath();
    struct RestoreCurrentDirectory final {
        QString path;
        ~RestoreCurrentDirectory() { static_cast<void>(QDir::setCurrent(path)); }
    } restore{original_cwd};
    QVERIFY(QDir::setCurrent(first));
    auto catalog = PackCatalog::open(QStringLiteral("catalog"));
    QVERIFY2(catalog.has_value(), catalog ? "" : qPrintable(catalog.error().message));
    const auto expected_root = QDir(first).filePath(QStringLiteral("catalog"));
    QCOMPARE((*catalog)->rootDirectory(), expected_root);
    QCOMPARE((*catalog)->archivesDirectory(),
             QDir(expected_root).filePath(QStringLiteral("archives")));
    QCOMPARE((*catalog)->blobObjectsDirectory(),
             QDir(expected_root).filePath(QStringLiteral("blobs")));

    QVERIFY(QDir::setCurrent(decoy));
    QVERIFY(QDir{}.mkdir(QStringLiteral("catalog")));
    const auto installed =
        (*catalog)->installArchive(archivePath(temporary.path(), QStringLiteral("relative-root")),
                                   QStringLiteral("2026-08-11T08:00:00Z"));
    QVERIFY2(installed.has_value(), installed ? "" : qPrintable(installed.error().message));
    const auto materialized = (*catalog)->materializeBlob(*revision, "objects/document.pdf");
    QVERIFY2(materialized.has_value(),
             materialized ? "" : qPrintable(materialized.error().message));
    QVERIFY(materialized->local_path.startsWith(expected_root + u'/'));
    QVERIFY(QDir(QDir(decoy).filePath(QStringLiteral("catalog")))
                .entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot)
                .empty());
}

void PackCatalogTest::documentsCatalogRecoveryProcedure() {
    const auto path =
        QDir(QStringLiteral(APPELLATE_SOURCE_DIR)).filePath(QStringLiteral("docs/spec/PACKS.md"));
    const auto bytes = readAll(path);
    QVERIFY2(bytes.has_value(), qPrintable(path));
    const auto documentation = QString::fromUtf8(*bytes);
    for (const auto& literal :
         {QStringLiteral(".awpack-*"), QStringLiteral(".blob-*"), QStringLiteral(".install.lock"),
          QStringLiteral(".install.lock.rmlock"), QStringLiteral("<sha256>.awpack"),
          QStringLiteral("blobs/<sha256>")}) {
        QVERIFY2(documentation.contains(literal), qPrintable(literal));
    }

    const auto normalized = documentation.simplified().toLower();
    qsizetype previous = -1;
    for (const auto& step : {
             QStringLiteral("stop all catalog processes"),
             QStringLiteral("preserve a forensic copy"),
             QStringLiteral("verify the digest object and every database reference"),
             QStringLiteral("confirm that no catalog process owns it"),
             QStringLiteral("remove only the confirmed orphan"),
             QStringLiteral("fsync the affected directory"),
             QStringLiteral("retry catalog admission"),
         }) {
        const auto index = normalized.indexOf(step, previous + 1);
        QVERIFY2(index > previous, qPrintable(step));
        previous = index;
    }
    QVERIFY(normalized.contains(QStringLiteral("no automatic catalog repair")));
    QVERIFY(normalized.contains(QStringLiteral("no automatic persistent-lock eviction")));
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
    const auto catalog_root = QDir(temporary.path()).filePath(QStringLiteral("catalog"));
    auto catalog = PackCatalog::open(catalog_root);
    QVERIFY(catalog.has_value());
    const auto installed =
        (*catalog)->installArchive(archivePath(temporary.path(), QStringLiteral("base")),
                                   QStringLiteral("2026-08-11T01:00:00Z"));
    QVERIFY(installed.has_value());

    const auto stored_path = QDir((*catalog)->archivesDirectory())
                                 .filePath(installed->archive_sha256 + QStringLiteral(".awpack"));
    catalog->reset();
    QFile corrupt(stored_path);
    QVERIFY(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(corrupt.write("corrupt"), qint64{7});
    corrupt.close();

    const auto reopened = PackCatalog::open(catalog_root);
    QVERIFY(!reopened.has_value());
    QCOMPARE(reopened.error().code, CatalogErrorCode::CorruptCatalog);
    const auto unchanged = readAll(stored_path);
    QVERIFY(unchanged.has_value());
    QCOMPARE(*unchanged, QByteArray("corrupt"));
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
    const auto catalog_root = QDir(temporary.path()).filePath(QStringLiteral("catalog"));
    auto catalog = PackCatalog::open(catalog_root);
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
    const auto blobs_directory = (*catalog)->blobObjectsDirectory();
    QVERIFY(writeAll(interrupted, QByteArray("partial")));

    catalog->reset();
    const auto refused_open = PackCatalog::open(catalog_root);
    QVERIFY(!refused_open.has_value());
    QCOMPARE(refused_open.error().code, CatalogErrorCode::CorruptCatalog);
    const auto preserved_interrupted = readAll(interrupted);
    QVERIFY(preserved_interrupted.has_value());
    QCOMPARE(*preserved_interrupted, QByteArray("partial"));
    QVERIFY(!QFileInfo::exists(initial->local_path));

    // This is the documented operator-recovery fixture: after all handles are closed and the
    // artifact is confirmed orphaned, remove only that file, fsync its directory, and retry.
    QVERIFY(QFile::remove(interrupted));
    QVERIFY(syncDirectory(blobs_directory));
    auto recovered_catalog = PackCatalog::open(catalog_root);
    QVERIFY2(recovered_catalog.has_value(),
             recovered_catalog ? "" : qPrintable(recovered_catalog.error().message));
    const auto rehydrated =
        (*recovered_catalog)->materializeBlob(*revision, "objects/document.pdf");
    QVERIFY(rehydrated.has_value());
    QCOMPARE(rehydrated->local_path, initial->local_path);
    QVERIFY(!QFileInfo::exists(interrupted));
    QFile restored(rehydrated->local_path);
    QVERIFY(restored.open(QIODevice::ReadOnly));
    QCOMPARE(restored.readAll(), testPdf());
    restored.close();

    QFile corrupt(rehydrated->local_path);
    QVERIFY(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(corrupt.write("corrupt"), qint64{7});
    corrupt.close();
    const auto refused = (*recovered_catalog)->materializeBlob(*revision, "objects/document.pdf");
    QVERIFY(!refused.has_value());
    QCOMPARE(refused.error().code, CatalogErrorCode::CorruptCatalog);
    QFile unchanged(rehydrated->local_path);
    QVERIFY(unchanged.open(QIODevice::ReadOnly));
    QCOMPARE(unchanged.readAll(), QByteArray("corrupt"));

    unchanged.close();
    QVERIFY(QFile::remove(rehydrated->local_path));
    QVERIFY(QFile::link(archivePath(temporary.path(), QStringLiteral("rehydrate")),
                        rehydrated->local_path));
    const auto linked = (*recovered_catalog)->materializeBlob(*revision, "objects/document.pdf");
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
    QCOMPARE(installed.error().code, CatalogErrorCode::CorruptCatalog);
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

    const auto catalog = PackCatalog::open(catalog_root);
    QVERIFY(!catalog.has_value());
    QCOMPARE(catalog.error().code, CatalogErrorCode::CorruptCatalog);
    QFile unchanged(object_path);
    QVERIFY(unchanged.open(QIODevice::ReadOnly));
    QCOMPARE(unchanged.readAll(), testPdf());
}

void PackCatalogTest::rejectsCorruptV1ArchiveWithoutMutation() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto revision = buildArchive(
        temporary.path(), QStringLiteral("corrupt-v1"), QStringLiteral("test.pack.corrupt-v1"),
        QStringLiteral("1.0.0"), QStringLiteral("Corrupt V1 Composite"), {}, true);
    QVERIFY(revision.has_value());
    const auto catalog_root = QDir(temporary.path()).filePath(QStringLiteral("catalog"));
    const auto source_archive = archivePath(temporary.path(), QStringLiteral("corrupt-v1"));
    const auto source_bytes = readAll(source_archive);
    QVERIFY(source_bytes.has_value());
    const auto archive_sha = QString::fromLatin1(sha256(*source_bytes));
    QVERIFY(createTrueV1Catalog(catalog_root, source_archive, *revision,
                                QStringLiteral("2026-08-11T09:30:00Z")));
    const auto installed_archive =
        QDir(catalog_root)
            .filePath(QStringLiteral("archives/") + archive_sha + QStringLiteral(".awpack"));
    QVERIFY(replaceAll(installed_archive, QByteArray("corrupt")));
    const auto before = captureTreeClosure(catalog_root);
    QVERIFY(before.has_value());
    QVERIFY(!QFileInfo::exists(QDir(catalog_root).filePath(QStringLiteral("blobs"))));

    const auto rejected = PackCatalog::open(catalog_root);
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, CatalogErrorCode::CorruptCatalog);

    const auto after = captureTreeClosure(catalog_root);
    QVERIFY(after.has_value());
    QVERIFY(*after == *before);
    QVERIFY(!QFileInfo::exists(QDir(catalog_root).filePath(QStringLiteral("blobs"))));
}

void PackCatalogTest::rollsBackV1MigrationBeforeCommitAndRetries() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto revision =
        buildArchive(temporary.path(), QStringLiteral("migration-rollback"),
                     QStringLiteral("test.pack.migration-rollback"), QStringLiteral("1.0.0"),
                     QStringLiteral("Migration Rollback Composite"), {}, true);
    QVERIFY(revision.has_value());
    const auto catalog_root = QDir(temporary.path()).filePath(QStringLiteral("catalog"));
    const auto source_archive = archivePath(temporary.path(), QStringLiteral("migration-rollback"));
    QVERIFY(createTrueV1Catalog(catalog_root, source_archive, *revision,
                                QStringLiteral("2026-08-11T09:45:00Z")));
    const auto before = captureTreeClosure(catalog_root);
    QVERIFY(before.has_value());

    auto scratch_context = appellate::packs::detail::acquireSecureScratchContext();
    QVERIFY2(scratch_context.has_value(),
             scratch_context ? "" : qPrintable(scratch_context.error().message));
    appellate::packs::detail::CatalogReport report;
    appellate::packs::detail::CatalogHooks hooks;
    hooks.report = &report;
    bool injected{};
    hooks.inject = [&injected](const appellate::packs::detail::CatalogObservation& observation) {
        if (!injected &&
            observation.operation == appellate::packs::detail::CatalogOperation::WritableOpen &&
            observation.subject == appellate::packs::detail::CatalogSubject::SQLiteConnection &&
            observation.event == appellate::packs::detail::CatalogEvent::MigrationCommitAttempted) {
            injected = true;
            return appellate::packs::detail::CatalogInjectedAction::FailBefore;
        }
        return appellate::packs::detail::CatalogInjectedAction::Continue;
    };
    const auto rejected = appellate::packs::detail::PackCatalogFactory::open(
        catalog_root, std::move(*scratch_context), std::move(hooks));
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, CatalogErrorCode::MigrationFailed);
    QVERIFY(injected);
    const auto observed = [&report](appellate::packs::detail::CatalogEvent event) {
        return std::ranges::any_of(
            report.observations,
            [event](const appellate::packs::detail::CatalogObservation& observation) {
                return observation.operation ==
                           appellate::packs::detail::CatalogOperation::WritableOpen &&
                       observation.subject ==
                           appellate::packs::detail::CatalogSubject::SQLiteConnection &&
                       observation.event == event;
            });
    };
    QVERIFY(observed(appellate::packs::detail::CatalogEvent::MigrationStarted));
    QVERIFY(observed(appellate::packs::detail::CatalogEvent::MigrationCommitAttempted));
    QVERIFY(!observed(appellate::packs::detail::CatalogEvent::MigrationCommitted));
    QVERIFY(report.remaining_ledger_paths.empty());
    QVERIFY(!report.residue_identity_ambiguous);
    const auto after = captureTreeClosure(catalog_root);
    QVERIFY(after.has_value());
    QVERIFY(*after == *before);
    QVERIFY(!QFileInfo::exists(QDir(catalog_root).filePath(QStringLiteral("blobs"))));

    auto retried = PackCatalog::open(catalog_root);
    QVERIFY2(retried.has_value(), retried ? "" : qPrintable(retried.error().message));
    QCOMPARE((*retried)->schemaVersion(), 2);
    QVERIFY(QFileInfo(QDir(catalog_root).filePath(QStringLiteral("blobs"))).isDir());
}

void PackCatalogTest::preservesAppliedV1MigrationAfterReportedCommitFailure() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto revision =
        buildArchive(temporary.path(), QStringLiteral("migration-applied"),
                     QStringLiteral("test.pack.migration-applied"), QStringLiteral("1.0.0"),
                     QStringLiteral("Applied Migration Composite"), {}, true);
    QVERIFY(revision.has_value());
    const auto catalog_root = QDir(temporary.path()).filePath(QStringLiteral("catalog"));
    const auto source_archive = archivePath(temporary.path(), QStringLiteral("migration-applied"));
    QVERIFY(createTrueV1Catalog(catalog_root, source_archive, *revision,
                                QStringLiteral("2026-08-11T09:50:00Z")));

    auto scratch_context = appellate::packs::detail::acquireSecureScratchContext();
    QVERIFY2(scratch_context.has_value(),
             scratch_context ? "" : qPrintable(scratch_context.error().message));
    appellate::packs::detail::CatalogReport report;
    appellate::packs::detail::CatalogHooks hooks;
    hooks.report = &report;
    bool injected{};
    hooks.inject = [&injected](const appellate::packs::detail::CatalogObservation& observation) {
        if (!injected &&
            observation.operation == appellate::packs::detail::CatalogOperation::WritableOpen &&
            observation.subject == appellate::packs::detail::CatalogSubject::SQLiteConnection &&
            observation.event == appellate::packs::detail::CatalogEvent::MigrationCommitAttempted) {
            injected = true;
            return appellate::packs::detail::CatalogInjectedAction::FailAfter;
        }
        return appellate::packs::detail::CatalogInjectedAction::Continue;
    };
    const auto reported_failure = appellate::packs::detail::PackCatalogFactory::open(
        catalog_root, std::move(*scratch_context), std::move(hooks));
    QVERIFY(!reported_failure.has_value());
    QCOMPARE(reported_failure.error().code, CatalogErrorCode::CannotOpen);
    QVERIFY(injected);
    const auto observed = [&report](appellate::packs::detail::CatalogEvent event) {
        return std::ranges::any_of(
            report.observations,
            [event](const appellate::packs::detail::CatalogObservation& observation) {
                return observation.operation ==
                           appellate::packs::detail::CatalogOperation::WritableOpen &&
                       observation.subject ==
                           appellate::packs::detail::CatalogSubject::SQLiteConnection &&
                       observation.event == event;
            });
    };
    QVERIFY(observed(appellate::packs::detail::CatalogEvent::MigrationStarted));
    QVERIFY(observed(appellate::packs::detail::CatalogEvent::MigrationCommitAttempted));
    QVERIFY(!observed(appellate::packs::detail::CatalogEvent::MigrationCommitted));
    QVERIFY(QFileInfo(QDir(catalog_root).filePath(QStringLiteral("blobs"))).isDir());
    QVERIFY(report.remaining_ledger_paths.empty());
    QVERIFY(!report.residue_identity_ambiguous);
    const auto lifecycle_index = [&report](appellate::packs::detail::CatalogEvent event) {
        const auto found = std::ranges::find_if(
            report.observations,
            [event](const appellate::packs::detail::CatalogObservation& observation) {
                return observation.operation ==
                           appellate::packs::detail::CatalogOperation::WritableOpen &&
                       observation.subject ==
                           appellate::packs::detail::CatalogSubject::SQLiteConnection &&
                       observation.event == event;
            });
        return found == report.observations.end()
                   ? std::optional<std::size_t>{}
                   : std::optional<std::size_t>{static_cast<std::size_t>(
                         std::distance(report.observations.begin(), found))};
    };
    const auto queries_finished =
        lifecycle_index(appellate::packs::detail::CatalogEvent::QueriesFinished);
    const auto database_closed =
        lifecycle_index(appellate::packs::detail::CatalogEvent::DatabaseClosed);
    const auto sidecars_inventoried =
        lifecycle_index(appellate::packs::detail::CatalogEvent::SidecarsInventoried);
    const auto database_reset =
        lifecycle_index(appellate::packs::detail::CatalogEvent::DatabaseReset);
    const auto database_removed =
        lifecycle_index(appellate::packs::detail::CatalogEvent::DatabaseRemoved);
    QVERIFY(queries_finished && database_closed && sidecars_inventoried && database_reset &&
            database_removed);
    QVERIFY(*queries_finished < *database_closed);
    QVERIFY(*database_closed < *sidecars_inventoried);
    QVERIFY(*sidecars_inventoried < *database_reset);
    QVERIFY(*database_reset < *database_removed);
    QVERIFY(std::ranges::any_of(
        std::ranges::subrange(report.observations.begin() +
                                  static_cast<std::ptrdiff_t>(*database_removed + 1U),
                              report.observations.end()),
        [](const appellate::packs::detail::CatalogObservation& observation) {
            return observation.operation ==
                       appellate::packs::detail::CatalogOperation::WritableOpen &&
                   observation.subject ==
                       appellate::packs::detail::CatalogSubject::PrivateDatabaseMain &&
                   observation.event ==
                       appellate::packs::detail::CatalogEvent::SchemaFingerprintCompared;
        }));

    auto retried = PackCatalog::open(catalog_root);
    QVERIFY2(retried.has_value(), retried ? "" : qPrintable(retried.error().message));
    QCOMPARE((*retried)->schemaVersion(), 2);
    const auto listed = (*retried)->list();
    QVERIFY(listed.has_value());
    QCOMPARE(listed->size(), std::size_t{1});
    QCOMPARE(listed->front().revision, *revision);
}

void PackCatalogTest::migratesV1BlobDescriptors() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto revision = buildArchive(
        temporary.path(), QStringLiteral("migration"), QStringLiteral("test.pack.migration"),
        QStringLiteral("1.0.0"), QStringLiteral("Migration Composite"), {}, true);
    QVERIFY(revision.has_value());
    const auto catalog_root = QDir(temporary.path()).filePath(QStringLiteral("catalog"));
    const auto source_archive = archivePath(temporary.path(), QStringLiteral("migration"));
    const auto source_bytes = readAll(source_archive);
    QVERIFY(source_bytes.has_value());
    const auto archive_sha = QString::fromLatin1(sha256(*source_bytes));
    QVERIFY(createTrueV1Catalog(catalog_root, source_archive, *revision,
                                QStringLiteral("2026-08-11T10:00:00Z")));
    const auto archive_path =
        QDir(catalog_root)
            .filePath(QStringLiteral("archives/") + archive_sha + QStringLiteral(".awpack"));
    QVERIFY(QFileInfo::exists(archive_path));
    QVERIFY(!QFileInfo::exists(QDir(catalog_root).filePath(QStringLiteral("blobs"))));

    auto migrated = PackCatalog::open(catalog_root);
    QVERIFY2(migrated.has_value(), migrated ? "" : qPrintable(migrated.error().message));
    QCOMPARE((*migrated)->schemaVersion(), 2);
    QVERIFY(QFileInfo::exists(QDir(catalog_root).filePath(QStringLiteral("blobs"))));
    const auto object_path =
        QDir((*migrated)->blobObjectsDirectory()).filePath(QString::fromLatin1(sha256(testPdf())));
    QVERIFY(!QFileInfo::exists(object_path));
    const auto blob = (*migrated)->materializeBlob(*revision, "objects/document.pdf");
    QVERIFY(blob.has_value());
    QCOMPARE(blob->local_path, object_path);
    QVERIFY(QFileInfo::exists(archive_path));
    QFile opened(blob->local_path);
    QVERIFY(opened.open(QIODevice::ReadOnly));
    QCOMPARE(opened.readAll(), testPdf());
}

void PackCatalogTest::negotiatesNewWorkflowCapabilityDeclarations() {
    const std::vector<std::string> feature_ids{
        "workbench.pack.route-filing-bindings",
        "workbench.pack.alternative-event-date-deadlines",
        "workbench.pack.operation-legal-time-guards",
    };
    const auto supported = appellate::packs::CapabilityRegistry::supported();
    for (const auto& feature_id : feature_ids) {
        const auto capability = std::ranges::find_if(supported, [&](const auto& candidate) {
            return candidate.id == feature_id && candidate.version == 1;
        });
        QVERIFY(capability != supported.end());
        QCOMPARE(capability->minimum_manifest_schema_version, std::uint32_t{2});
        QCOMPARE(capability->maximum_manifest_schema_version, std::uint32_t{2});

        const std::vector declaration{appellate::model::RequiredCapability{feature_id, 1}};
        QVERIFY(
            appellate::packs::CapabilityRegistry::validateDeclarations(2, declaration).has_value());
        QVERIFY(!appellate::packs::CapabilityRegistry::validateDeclarations(1, declaration)
                     .has_value());
        QVERIFY(!appellate::packs::CapabilityRegistry::validateDeclarations(3, declaration)
                     .has_value());
    }

    const std::vector declarations{
        appellate::model::RequiredCapability{"workbench.pack.declarative-resources", 2},
        appellate::model::RequiredCapability{"workbench.pack.event-date-deadlines", 1},
        appellate::model::RequiredCapability{"workbench.pack.route-filing-bindings", 1},
        appellate::model::RequiredCapability{"workbench.pack.alternative-event-date-deadlines", 1},
        appellate::model::RequiredCapability{"workbench.pack.operation-legal-time-guards", 1},
    };
    const auto validate_coverage = [](const auto& declared) {
        return appellate::packs::CapabilityRegistry::validateCoverage(
            2, declared, std::span<const appellate::model::ResourceKind>{},
            false, // workflow preconditions
            false, // dependent deadlines
            false, // named deadlines
            true,  // event-date deadlines
            false, // argument-date guards
            false, // structured disposition
            false, // grounded questions
            false, // realism evidence
            false, // sealed record twins
            false, // route-role subsets
            false, // workflow-instance preconditions
            false, // static deficiency deadlines
            false, // operation document bindings
            false, // operation disposition bindings
            true,  // route filing bindings
            true,  // alternative event-date deadlines
            true); // operation legal-time guards
    };
    QVERIFY(validate_coverage(declarations).has_value());
    for (const auto& omitted_id : feature_ids) {
        auto incomplete = declarations;
        std::erase_if(incomplete,
                      [&](const auto& capability) { return capability.id == omitted_id; });
        const auto rejected = validate_coverage(incomplete);
        QVERIFY(!rejected.has_value());
        QVERIFY(rejected.error().message.contains(QString::fromStdString(omitted_id)));
    }
}

void PackCatalogTest::resolvesCatalogPackWithExtendedWorkflowCapabilities() {
    const auto supported = appellate::packs::CapabilityRegistry::supported();
    const auto disposition_binding_capability =
        std::ranges::find_if(supported, [](const auto& capability) {
            return capability.id == "workbench.pack.operation-disposition-bindings" &&
                   capability.version == 1;
        });
    QVERIFY(disposition_binding_capability != supported.end());
    QCOMPARE(disposition_binding_capability->minimum_manifest_schema_version, std::uint32_t{2});
    QCOMPARE(disposition_binding_capability->maximum_manifest_schema_version, std::uint32_t{2});

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto fixture_root = QFINDTESTDATA("../fixtures/full-resource-pack-v2");
    QVERIFY(!fixture_root.isEmpty());
    const auto source = QDir(temporary.path()).filePath(QStringLiteral("extended-pack"));
    QVERIFY(copyTree(fixture_root, source));

    const auto workflow_path = QDir(source).filePath(QStringLiteral("resources/workflow.json"));
    QFile workflow_file(workflow_path);
    QVERIFY(workflow_file.open(QIODevice::ReadOnly));
    auto workflow = QJsonDocument::fromJson(workflow_file.readAll()).object();
    workflow_file.close();
    auto operations = workflow.value(QStringLiteral("operations")).toArray();
    QJsonObject calculation;
    for (const auto& value : operations) {
        const auto operation = value.toObject();
        if (operation.value(QStringLiteral("operation_id")).toString() ==
            QStringLiteral("example.operation.calculate-cure")) {
            calculation = operation;
            break;
        }
    }
    QVERIFY(!calculation.isEmpty());
    auto named = calculation;
    named.insert(QStringLiteral("operation_id"),
                 QStringLiteral("example.operation.calculate-named"));
    named.insert(QStringLiteral("produced_deadline_id"), QStringLiteral("example.deadline.named"));
    operations.push_back(named);
    auto event_based = calculation;
    event_based.insert(QStringLiteral("operation_id"),
                       QStringLiteral("example.operation.calculate-from-judgment"));
    event_based.insert(QStringLiteral("stage_id"), QStringLiteral("example.stage.submitted"));
    event_based.insert(QStringLiteral("produced_deadline_id"),
                       QStringLiteral("example.deadline.from-judgment"));
    event_based.insert(QStringLiteral("deadline_event_base"),
                       QJsonObject{{QStringLiteral("kind"), QStringLiteral("judgment_occurred")}});
    event_based.insert(QStringLiteral("authorized_role_ids"),
                       QJsonArray{QStringLiteral("example.role.court")});
    operations.push_back(event_based);

    const auto court_authority = QJsonObject{
        {QStringLiteral("primary_authority_id"), QStringLiteral("example.authority.rule-one")},
        {QStringLiteral("supporting_authority_ids"), QJsonArray{}},
    };
    const auto document_sha256 =
        QStringLiteral("bab85fe6529e9832b26196e8f08448b02bbe79e5ae4d4d37d104b278e11f1366");
    const auto first_legal_time = QJsonObject{
        {QStringLiteral("court_date"), QStringLiteral("2026-01-02")},
        {QStringLiteral("instant_unix_seconds"), QStringLiteral("1767312000")},
    };
    const auto second_legal_time = QJsonObject{
        {QStringLiteral("court_date"), QStringLiteral("2026-01-03")},
        {QStringLiteral("instant_unix_seconds"), QStringLiteral("1767398400")},
    };
    const auto allowed_legal_times = QJsonArray{first_legal_time, second_legal_time};
    const auto alternative_order_id = QStringLiteral("example.order.alternative");
    const auto first_order_operation_id = QStringLiteral("example.operation.enter-alternative-one");
    const auto second_order_operation_id =
        QStringLiteral("example.operation.enter-alternative-two");
    operations.push_back(QJsonObject{
        {QStringLiteral("operation_id"), first_order_operation_id},
        {QStringLiteral("stage_id"), QStringLiteral("example.stage.opened")},
        {QStringLiteral("opcode"), QStringLiteral("enter_order")},
        {QStringLiteral("authority"), court_authority},
        {QStringLiteral("authorized_role_ids"), QJsonArray{QStringLiteral("example.role.court")}},
        {QStringLiteral("document_binding"),
         QJsonObject{
             {QStringLiteral("record_entry_id"), QStringLiteral("example.record.entry-one")},
             {QStringLiteral("document_sha256"), document_sha256},
             {QStringLiteral("expected_court_date"), QStringLiteral("2026-01-02")},
             {QStringLiteral("order_id"), alternative_order_id},
             {QStringLiteral("disposition"), QStringLiteral("other")},
         }},
    });
    operations.push_back(QJsonObject{
        {QStringLiteral("operation_id"), second_order_operation_id},
        {QStringLiteral("stage_id"), QStringLiteral("example.stage.submitted")},
        {QStringLiteral("opcode"), QStringLiteral("enter_order")},
        {QStringLiteral("authority"), court_authority},
        {QStringLiteral("authorized_role_ids"), QJsonArray{QStringLiteral("example.role.court")}},
        {QStringLiteral("document_binding"),
         QJsonObject{
             {QStringLiteral("record_entry_id"), QStringLiteral("example.record.brief-opening")},
             {QStringLiteral("document_sha256"), document_sha256},
             {QStringLiteral("expected_court_date"), QStringLiteral("2026-01-03")},
             {QStringLiteral("order_id"), alternative_order_id},
             {QStringLiteral("disposition"), QStringLiteral("other")},
         }},
    });
    auto alternative_event_based = calculation;
    alternative_event_based.insert(
        QStringLiteral("operation_id"),
        QStringLiteral("example.operation.calculate-from-alternative-order"));
    alternative_event_based.insert(QStringLiteral("stage_id"),
                                   QStringLiteral("example.stage.submitted"));
    alternative_event_based.insert(QStringLiteral("produced_deadline_id"),
                                   QStringLiteral("example.deadline.from-alternative-order"));
    alternative_event_based.insert(
        QStringLiteral("deadline_event_base"),
        QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("order_occurred_one_of")},
            {QStringLiteral("order_id"), alternative_order_id},
            {QStringLiteral("operation_ids"),
             QJsonArray{first_order_operation_id, second_order_operation_id}},
        });
    alternative_event_based.insert(QStringLiteral("authorized_role_ids"),
                                   QJsonArray{QStringLiteral("example.role.court")});
    operations.push_back(alternative_event_based);
    operations.push_back(QJsonObject{
        {QStringLiteral("operation_id"), QStringLiteral("example.operation.accept-bound-notice")},
        {QStringLiteral("stage_id"), QStringLiteral("example.stage.submitted")},
        {QStringLiteral("opcode"), QStringLiteral("accept_filing")},
        {QStringLiteral("authority"), court_authority},
        {QStringLiteral("authorized_role_ids"), QJsonArray{}},
    });
    for (qsizetype index = 0; index < operations.size(); ++index) {
        auto operation = operations.at(index).toObject();
        if (operation.value(QStringLiteral("operation_id")).toString() !=
            QStringLiteral("example.operation.issue-judgment")) {
            continue;
        }
        auto preconditions = operation.value(QStringLiteral("preconditions")).toArray();
        preconditions.push_back(QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("deadline_status")},
            {QStringLiteral("deadline_id"), QStringLiteral("example.deadline.named")},
            {QStringLiteral("status"), QStringLiteral("reached")},
        });
        preconditions.push_back(QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("argument_date_status")},
            {QStringLiteral("status"), QStringLiteral("reached")},
        });
        preconditions.push_back(QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("filing_instance")},
            {QStringLiteral("filing_type_id"), QStringLiteral("example.filing.notice")},
            {QStringLiteral("present"), true},
            {QStringLiteral("actor_id"), QStringLiteral("example.actor.appellant")},
            {QStringLiteral("filing_id"), QStringLiteral("example.filing.notice-instance")},
            {QStringLiteral("accept_operation_id"),
             QStringLiteral("example.operation.accept-notice")},
            {QStringLiteral("record_entry_id"), QStringLiteral("example.record.brief-opening")},
            {QStringLiteral("document_sha256"),
             QStringLiteral("bab85fe6529e9832b26196e8f08448b02bbe79e5ae4d4d37d104b278e11f1366")},
        });
        operation.insert(QStringLiteral("preconditions"), preconditions);
        operation.insert(
            QStringLiteral("document_binding"),
            QJsonObject{
                {QStringLiteral("record_entry_id"), QStringLiteral("example.record.brief-opening")},
                {QStringLiteral("document_sha256"),
                 QStringLiteral(
                     "bab85fe6529e9832b26196e8f08448b02bbe79e5ae4d4d37d104b278e11f1366")},
                {QStringLiteral("expected_court_date"), QStringLiteral("2026-01-03")},
            });
        operation.insert(QStringLiteral("disposition_plan_id"),
                         QStringLiteral("example.disposition.fictional"));
        operations.replace(index, operation);
        break;
    }
    for (qsizetype index = 0; index < operations.size(); ++index) {
        auto operation = operations.at(index).toObject();
        operation.insert(QStringLiteral("allowed_legal_times"), allowed_legal_times);
        operations.replace(index, operation);
    }
    workflow.insert(QStringLiteral("operations"), operations);
    auto routes = workflow.value(QStringLiteral("filing_routes")).toArray();
    auto route = routes.at(0).toObject();
    route.insert(QStringLiteral("authorized_role_scope"), QStringLiteral("catalog_subset"));
    route.insert(
        QStringLiteral("deficiency_deadline"),
        QJsonObject{
            {QStringLiteral("deadline_id"), QStringLiteral("example.deadline.notice-cure-exact")},
            {QStringLiteral("operation_id"), QStringLiteral("example.operation.calculate-cure")},
            {QStringLiteral("id_mode"), QStringLiteral("exact")},
            {QStringLiteral("trigger_filing"),
             QJsonObject{
                 {QStringLiteral("filing_id"), QStringLiteral("example.filing.notice-deficient")},
                 {QStringLiteral("actor_id"), QStringLiteral("example.actor.appellant")},
                 {QStringLiteral("record_entry_id"), QStringLiteral("example.record.entry-one")},
                 {QStringLiteral("document_sha256"),
                  QStringLiteral(
                      "bab85fe6529e9832b26196e8f08448b02bbe79e5ae4d4d37d104b278e11f1366")},
                 {QStringLiteral("expected_court_date"), QStringLiteral("2026-01-02")},
             }},
        });
    routes.replace(0, route);
    routes.push_back(QJsonObject{
        {QStringLiteral("filing_type_id"), QStringLiteral("example.filing.notice")},
        {QStringLiteral("stage_id"), QStringLiteral("example.stage.submitted")},
        {QStringLiteral("authorized_role_ids"),
         QJsonArray{QStringLiteral("example.role.appellant")}},
        {QStringLiteral("required_field_ids"), QJsonArray{QStringLiteral("example.field.caption")}},
        {QStringLiteral("required_service_role_ids"),
         QJsonArray{QStringLiteral("example.role.appellee")}},
        {QStringLiteral("accept_operation_id"),
         QStringLiteral("example.operation.accept-bound-notice")},
        {QStringLiteral("reject_operation_id"),
         QStringLiteral("example.operation.reject-submitted")},
        {QStringLiteral("filing_bindings"),
         QJsonArray{QJsonObject{
             {QStringLiteral("filing_id"), QStringLiteral("example.filing.bound-notice")},
             {QStringLiteral("actor_id"), QStringLiteral("example.actor.appellant")},
             {QStringLiteral("record_entry_id"), QStringLiteral("example.record.brief-opening")},
             {QStringLiteral("document_sha256"), document_sha256},
             {QStringLiteral("expected_legal_time"), second_legal_time},
             {QStringLiteral("preconditions"),
              QJsonArray{QJsonObject{
                  {QStringLiteral("kind"), QStringLiteral("order_instance")},
                  {QStringLiteral("order_id"), alternative_order_id},
                  {QStringLiteral("disposition"), QStringLiteral("other")},
                  {QStringLiteral("operation_id"), first_order_operation_id},
                  {QStringLiteral("record_entry_id"), QStringLiteral("example.record.entry-one")},
                  {QStringLiteral("document_sha256"), document_sha256},
              }}},
         }}},
        {QStringLiteral("reject_after_deadline"), true},
    });
    workflow.insert(QStringLiteral("filing_routes"), routes);
    const auto workflow_bytes = QJsonDocument(workflow).toJson(QJsonDocument::Indented);
    QVERIFY(replaceAll(workflow_path, workflow_bytes));

    const auto manifest_path = QDir(source).filePath(QStringLiteral("manifest.json"));
    QFile manifest_file(manifest_path);
    QVERIFY(manifest_file.open(QIODevice::ReadOnly));
    auto manifest = QJsonDocument::fromJson(manifest_file.readAll()).object();
    manifest_file.close();
    auto capabilities = manifest.value(QStringLiteral("required_capabilities")).toArray();
    for (const auto* capability :
         {"workbench.pack.dependent-deadlines", "workbench.pack.named-deadlines",
          "workbench.pack.event-date-deadlines", "workbench.pack.argument-date-guards",
          "workbench.pack.route-role-subsets", "workbench.pack.workflow-instance-preconditions",
          "workbench.pack.static-deficiency-deadlines",
          "workbench.pack.operation-document-bindings",
          "workbench.pack.operation-disposition-bindings", "workbench.pack.route-filing-bindings",
          "workbench.pack.alternative-event-date-deadlines",
          "workbench.pack.operation-legal-time-guards"}) {
        capabilities.push_back(QJsonObject{{QStringLiteral("id"), QString::fromLatin1(capability)},
                                           {QStringLiteral("version"), 1}});
    }
    manifest.insert(QStringLiteral("required_capabilities"), capabilities);
    auto contents = manifest.value(QStringLiteral("contents")).toArray();
    for (qsizetype index = 0; index < contents.size(); ++index) {
        auto descriptor = contents.at(index).toObject();
        if (descriptor.value(QStringLiteral("path")).toString() ==
            QStringLiteral("resources/workflow.json")) {
            descriptor.insert(QStringLiteral("sha256"),
                              QString::fromLatin1(sha256(workflow_bytes)));
            contents.replace(index, descriptor);
            break;
        }
    }
    manifest.insert(QStringLiteral("contents"), contents);
    QVERIFY(replaceAll(manifest_path, QJsonDocument(manifest).toJson(QJsonDocument::Indented)));

    const auto archive_path = QDir(temporary.path()).filePath(QStringLiteral("extended.awpack"));
    const auto exported = PackArchive::exportDirectory(source, archive_path);
    QVERIFY2(exported.has_value(), exported ? "" : qPrintable(exported.error().message));
    auto catalog = PackCatalog::open(QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    QVERIFY(catalog.has_value());
    const auto installed =
        (*catalog)->installArchive(archive_path, QStringLiteral("2026-08-11T11:00:00Z"));
    QVERIFY2(installed.has_value(), installed ? "" : qPrintable(installed.error().message));
    const auto resolved = (*catalog)->loadResolved(*exported);
    QVERIFY2(resolved.has_value(), resolved ? "" : qPrintable(resolved.error().message));
    QCOMPARE(resolved->root().revision, *exported);
}

} // namespace

QTEST_GUILESS_MAIN(PackCatalogTest)

#include "tst_pack_catalog.moc"
