#include "appellate/packs/pack_catalog.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryFile>
#include <QUuid>
#include <QVariant>

#include <array>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <ranges>
#include <utility>

#if defined(Q_OS_UNIX)
#include <fcntl.h>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <io.h>
#endif

namespace appellate::packs {
namespace {

constexpr auto current_schema_version = 1;
constexpr qsizetype copy_buffer_bytes = 64 * 1024;
constexpr qsizetype maximum_text_characters = 512;

[[nodiscard]] auto fail(CatalogErrorCode code, QString message)
    -> std::unexpected<CatalogError> {
    return std::unexpected(CatalogError{code, std::move(message)});
}

[[nodiscard]] bool validText(const QString& value) {
    return !value.isEmpty() && value.size() <= maximum_text_characters &&
           !value.contains(QChar::Null);
}

[[nodiscard]] bool validDigest(const QString& value) {
    return value.size() == 64 && std::ranges::all_of(value, [](QChar character) {
               return (character >= u'0' && character <= u'9') ||
                      (character >= u'a' && character <= u'f');
           });
}

[[nodiscard]] QString asQString(const std::string& value) {
    return QString::fromUtf8(value);
}

[[nodiscard]] model::PackRevision revisionFromQuery(const QSqlQuery& query, int offset = 0) {
    return model::PackRevision{
        model::PackId{query.value(offset).toString().toUtf8().toStdString()},
        query.value(offset + 1).toString().toUtf8().toStdString(),
        query.value(offset + 2).toString().toLatin1().toStdString(),
    };
}

[[nodiscard]] auto queryFailure(CatalogErrorCode code, const QSqlQuery& query,
                                const QString& action) -> std::unexpected<CatalogError> {
    return fail(code, QStringLiteral("%1: %2").arg(action, query.lastError().text()));
}

[[nodiscard]] auto execStatement(QSqlDatabase& database, const QString& sql,
                                 CatalogErrorCode code, const QString& action)
    -> std::expected<void, CatalogError> {
    QSqlQuery query(database);
    if (!query.exec(sql)) {
        return queryFailure(code, query, action);
    }
    return {};
}

[[nodiscard]] bool syncFile(QFileDevice& file) {
    if (!file.flush()) {
        return false;
    }
    const auto handle = file.handle();
    if (handle < 0) {
        return false;
    }
#if defined(Q_OS_UNIX)
    int result{};
    do {
        result = ::fsync(static_cast<int>(handle));
    } while (result != 0 && errno == EINTR);
    return result == 0;
#elif defined(Q_OS_WIN)
    return ::_commit(static_cast<int>(handle)) == 0;
#else
    return true;
#endif
}

[[nodiscard]] bool syncDirectory(const QString& directory) {
#if defined(Q_OS_UNIX)
    auto flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const auto encoded = QFile::encodeName(directory);
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
    Q_UNUSED(directory);
    return true;
#endif
}

struct StagedArchive final {
    QString path;
    QString sha256;
    LoadedPack loaded;
};

[[nodiscard]] auto stageArchive(const QString& source_path, const QString& archives_directory)
    -> std::expected<StagedArchive, CatalogError> {
    const auto initially_loaded = PackArchive::importArchive(source_path);
    if (!initially_loaded) {
        return fail(CatalogErrorCode::ArchiveInvalid, initially_loaded.error().message);
    }

    QFile source(source_path);
    if (!source.open(QIODevice::ReadOnly)) {
        return fail(CatalogErrorCode::CannotStoreArchive,
                    QStringLiteral("Cannot open archive for installation"));
    }
    QTemporaryFile staged(
        QDir(archives_directory).filePath(QStringLiteral(".awpack-XXXXXX.tmp")));
    staged.setAutoRemove(false);
    if (!staged.open()) {
        return fail(CatalogErrorCode::CannotStoreArchive,
                    QStringLiteral("Cannot create archive staging file"));
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::array<char, copy_buffer_bytes> buffer{};
    while (true) {
        const auto read = source.read(buffer.data(), static_cast<qint64>(buffer.size()));
        if (read < 0) {
            staged.setAutoRemove(true);
            return fail(CatalogErrorCode::CannotStoreArchive,
                        QStringLiteral("Cannot read the complete source archive"));
        }
        if (read == 0) {
            break;
        }
        if (staged.write(buffer.data(), read) != read) {
            staged.setAutoRemove(true);
            return fail(CatalogErrorCode::CannotStoreArchive,
                        QStringLiteral("Cannot write the staged archive"));
        }
        hash.addData(QByteArrayView(buffer.data(), read));
    }
    if (!syncFile(staged)) {
        staged.setAutoRemove(true);
        return fail(CatalogErrorCode::CannotStoreArchive,
                    QStringLiteral("Cannot durably flush the staged archive"));
    }
    const auto staged_path = staged.fileName();
    staged.close();

    const auto reloaded = PackArchive::importArchive(staged_path);
    if (!reloaded || reloaded->revision != initially_loaded->revision) {
        QFile::remove(staged_path);
        return fail(CatalogErrorCode::ArchiveInvalid,
                    QStringLiteral("Archive changed while it was being installed"));
    }
    return StagedArchive{
        staged_path,
        QString::fromLatin1(hash.result().toHex()),
        *reloaded,
    };
}

[[nodiscard]] auto installStagedFile(StagedArchive& staged, const QString& archives_directory)
    -> std::expected<QString, CatalogError> {
    const auto final_path = QDir(archives_directory)
                                .filePath(staged.sha256 + QStringLiteral(".awpack"));
    if (QFileInfo::exists(final_path)) {
        QFile::remove(staged.path);
        const auto existing = PackArchive::importArchive(final_path);
        if (!existing || existing->revision != staged.loaded.revision) {
            return fail(CatalogErrorCode::CannotStoreArchive,
                        QStringLiteral("Existing archive object is corrupt"));
        }
        return final_path;
    }
    if (!QFile::rename(staged.path, final_path)) {
        QFile::remove(staged.path);
        const auto existing = PackArchive::importArchive(final_path);
        if (!existing || existing->revision != staged.loaded.revision) {
            return fail(CatalogErrorCode::CannotStoreArchive,
                        QStringLiteral("Cannot atomically install archive object"));
        }
        return final_path;
    }
    if (!syncDirectory(archives_directory)) {
        return fail(CatalogErrorCode::CannotStoreArchive,
                    QStringLiteral("Cannot durably flush the archive directory"));
    }
    return final_path;
}

[[nodiscard]] auto dependenciesFor(const QSqlDatabase& database,
                                   const model::PackRevision& revision)
    -> std::expected<std::vector<model::PackDependency>, CatalogError> {
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT dependency_pack_id, dependency_version, dependency_digest "
        "FROM pack_dependencies WHERE pack_id = ? AND version = ? "
        "ORDER BY dependency_pack_id, dependency_version, dependency_digest"));
    query.addBindValue(asQString(revision.id.value));
    query.addBindValue(asQString(revision.version));
    if (!query.exec()) {
        return queryFailure(CatalogErrorCode::QueryFailed, query,
                            QStringLiteral("load pack dependencies"));
    }
    std::vector<model::PackDependency> dependencies;
    while (query.next()) {
        dependencies.push_back(model::PackDependency{revisionFromQuery(query)});
    }
    return dependencies;
}

} // namespace

PackCatalog::PackCatalog(QString root_directory, QString connection_name)
    : root_directory_(std::move(root_directory)), connection_name_(std::move(connection_name)) {}

PackCatalog::~PackCatalog() { closeConnection(); }

void PackCatalog::closeConnection() {
    if (connection_name_.isEmpty()) {
        return;
    }
    const auto connection_name = std::exchange(connection_name_, {});
    if (database_.isValid()) {
        database_.close();
        database_ = QSqlDatabase{};
    }
    QSqlDatabase::removeDatabase(connection_name);
}

std::expected<std::unique_ptr<PackCatalog>, CatalogError>
PackCatalog::open(const QString& root_directory) {
    if (root_directory.isEmpty()) {
        return fail(CatalogErrorCode::InvalidConfiguration,
                    QStringLiteral("Pack-catalog root is empty"));
    }
    const auto absolute_root = QDir(root_directory).absolutePath();
    if (QFileInfo(absolute_root).isSymLink() || !QDir{}.mkpath(absolute_root)) {
        return fail(CatalogErrorCode::InvalidConfiguration,
                    QStringLiteral("Pack-catalog root is unsafe or cannot be created"));
    }
    const auto archives = QDir(absolute_root).filePath(QStringLiteral("archives"));
    if (!QDir{}.mkpath(archives) || QFileInfo(archives).isSymLink()) {
        return fail(CatalogErrorCode::InvalidConfiguration,
                    QStringLiteral("Archive-object directory is unsafe or cannot be created"));
    }

    auto catalog = std::unique_ptr<PackCatalog>(new PackCatalog(
        absolute_root,
        QStringLiteral("appellate-packs-%1").arg(QUuid::createUuid().toString(QUuid::Id128))));
    catalog->database_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                   catalog->connection_name_);
    catalog->database_.setDatabaseName(
        QDir(absolute_root).filePath(QStringLiteral("catalog.sqlite")));
    if (!catalog->database_.open()) {
        const auto message = catalog->database_.lastError().text();
        catalog->closeConnection();
        return fail(CatalogErrorCode::CannotOpen, message);
    }
    if (const auto configured = catalog->configure(); !configured) {
        return std::unexpected(configured.error());
    }
    if (const auto migrated = catalog->migrate(); !migrated) {
        return std::unexpected(migrated.error());
    }
    return catalog;
}

std::expected<void, CatalogError> PackCatalog::configure() {
    constexpr std::pair<const char*, const char*> statements[]{
        {"PRAGMA foreign_keys = ON", "enable foreign keys"},
        {"PRAGMA journal_mode = WAL", "enable WAL mode"},
        {"PRAGMA synchronous = FULL", "enable full synchronization"},
        {"PRAGMA busy_timeout = 5000", "set busy timeout"},
    };
    for (const auto& [sql, action] : statements) {
        if (const auto result = execStatement(database_, QLatin1StringView(sql),
                                              CatalogErrorCode::CannotOpen,
                                              QLatin1StringView(action));
            !result) {
            return result;
        }
    }
    return {};
}

std::expected<void, CatalogError> PackCatalog::migrate() {
    if (const auto begun = beginImmediate(); !begun) {
        return begun;
    }
    constexpr std::array statements{
        "CREATE TABLE IF NOT EXISTS catalog_migrations (version INTEGER PRIMARY KEY, "
        "applied_at_utc TEXT NOT NULL) STRICT",
        "CREATE TABLE IF NOT EXISTS pack_revisions ("
        "pack_id TEXT NOT NULL, version TEXT NOT NULL, digest TEXT NOT NULL, "
        "archive_sha256 TEXT NOT NULL, installed_at_utc TEXT NOT NULL, "
        "PRIMARY KEY(pack_id, version), UNIQUE(pack_id, version, digest)) STRICT",
        "CREATE TABLE IF NOT EXISTS pack_dependencies ("
        "pack_id TEXT NOT NULL, version TEXT NOT NULL, dependency_pack_id TEXT NOT NULL, "
        "dependency_version TEXT NOT NULL, dependency_digest TEXT NOT NULL, "
        "PRIMARY KEY(pack_id, version, dependency_pack_id, dependency_version), "
        "FOREIGN KEY(pack_id, version) REFERENCES pack_revisions(pack_id, version) "
        "ON DELETE CASCADE, FOREIGN KEY(dependency_pack_id, dependency_version, "
        "dependency_digest) REFERENCES pack_revisions(pack_id, version, digest) "
        "ON DELETE RESTRICT) STRICT",
    };
    for (const auto* statement : statements) {
        if (const auto result = execStatement(database_, QLatin1StringView(statement),
                                              CatalogErrorCode::MigrationFailed,
                                              QStringLiteral("apply catalog schema"));
            !result) {
            rollback();
            return result;
        }
    }
    QSqlQuery current(database_);
    if (!current.exec(QStringLiteral("SELECT COALESCE(MAX(version), 0) FROM catalog_migrations")) ||
        !current.next()) {
        rollback();
        return queryFailure(CatalogErrorCode::MigrationFailed, current,
                            QStringLiteral("read catalog schema version"));
    }
    const auto version = current.value(0).toInt();
    if (version > current_schema_version) {
        rollback();
        return fail(CatalogErrorCode::MigrationFailed,
                    QStringLiteral("Catalog schema is newer than this application"));
    }
    if (version < current_schema_version) {
        QSqlQuery record(database_);
        record.prepare(QStringLiteral(
            "INSERT INTO catalog_migrations(version, applied_at_utc) VALUES(1, ?)"));
        record.addBindValue(QStringLiteral("2026-08-11T00:00:00Z"));
        if (!record.exec()) {
            rollback();
            return queryFailure(CatalogErrorCode::MigrationFailed, record,
                                QStringLiteral("record catalog migration"));
        }
    }
    return commit();
}

QString PackCatalog::archivesDirectory() const {
    return QDir(root_directory_).filePath(QStringLiteral("archives"));
}

std::expected<InstalledPack, CatalogError>
PackCatalog::installArchive(const QString& archive_path, const QString& installed_at_utc) {
    if (!validText(installed_at_utc)) {
        return fail(CatalogErrorCode::InvalidConfiguration,
                    QStringLiteral("Installed time is missing or invalid"));
    }
    auto staged = stageArchive(archive_path, archivesDirectory());
    if (!staged) {
        return std::unexpected(staged.error());
    }
    const auto final_path = installStagedFile(*staged, archivesDirectory());
    if (!final_path) {
        return std::unexpected(final_path.error());
    }

    const auto& revision = staged->loaded.revision;
    if (!validText(asQString(revision.id.value)) || !validText(asQString(revision.version)) ||
        !validDigest(asQString(revision.digest)) || !validDigest(staged->sha256)) {
        return fail(CatalogErrorCode::ArchiveInvalid,
                    QStringLiteral("Archive revision is invalid"));
    }
    for (const auto& dependency : staged->loaded.dependencies) {
        if (dependency.revision.id == revision.id &&
            dependency.revision.version == revision.version) {
            return fail(CatalogErrorCode::DependencyCycle,
                        QStringLiteral("Pack cannot depend on its own ID and version"));
        }
    }

    if (const auto begun = beginImmediate(); !begun) {
        return std::unexpected(begun.error());
    }
    QSqlQuery existing(database_);
    existing.prepare(QStringLiteral(
        "SELECT digest, archive_sha256, installed_at_utc FROM pack_revisions "
        "WHERE pack_id = ? AND version = ?"));
    existing.addBindValue(asQString(revision.id.value));
    existing.addBindValue(asQString(revision.version));
    if (!existing.exec()) {
        rollback();
        return queryFailure(CatalogErrorCode::QueryFailed, existing,
                            QStringLiteral("check immutable pack version"));
    }
    if (existing.next()) {
        if (existing.value(0).toString() != asQString(revision.digest)) {
            rollback();
            return fail(CatalogErrorCode::ImmutableConflict,
                        QStringLiteral("Pack ID and version already identify another revision"));
        }
        const auto installed = InstalledPack{
            revision,
            existing.value(1).toString(),
            existing.value(2).toString(),
            staged->loaded.dependencies,
        };
        rollback();
        return installed;
    }

    for (const auto& dependency : staged->loaded.dependencies) {
        QSqlQuery required(database_);
        required.prepare(QStringLiteral(
            "SELECT 1 FROM pack_revisions WHERE pack_id = ? AND version = ? AND digest = ?"));
        required.addBindValue(asQString(dependency.revision.id.value));
        required.addBindValue(asQString(dependency.revision.version));
        required.addBindValue(asQString(dependency.revision.digest));
        if (!required.exec()) {
            rollback();
            return queryFailure(CatalogErrorCode::QueryFailed, required,
                                QStringLiteral("check exact pack dependency"));
        }
        if (!required.next()) {
            rollback();
            return fail(CatalogErrorCode::MissingDependency,
                        QStringLiteral("Required exact pack revision is not installed"));
        }
    }

    QSqlQuery insert(database_);
    insert.prepare(QStringLiteral(
        "INSERT INTO pack_revisions(pack_id, version, digest, archive_sha256, installed_at_utc) "
        "VALUES(?, ?, ?, ?, ?)"));
    insert.addBindValue(asQString(revision.id.value));
    insert.addBindValue(asQString(revision.version));
    insert.addBindValue(asQString(revision.digest));
    insert.addBindValue(staged->sha256);
    insert.addBindValue(installed_at_utc);
    if (!insert.exec()) {
        rollback();
        return queryFailure(CatalogErrorCode::QueryFailed, insert,
                            QStringLiteral("install pack revision"));
    }
    QSqlQuery dependency_insert(database_);
    dependency_insert.prepare(QStringLiteral(
        "INSERT INTO pack_dependencies(pack_id, version, dependency_pack_id, "
        "dependency_version, dependency_digest) VALUES(?, ?, ?, ?, ?)"));
    for (const auto& dependency : staged->loaded.dependencies) {
        dependency_insert.bindValue(0, asQString(revision.id.value));
        dependency_insert.bindValue(1, asQString(revision.version));
        dependency_insert.bindValue(2, asQString(dependency.revision.id.value));
        dependency_insert.bindValue(3, asQString(dependency.revision.version));
        dependency_insert.bindValue(4, asQString(dependency.revision.digest));
        if (!dependency_insert.exec()) {
            rollback();
            return queryFailure(CatalogErrorCode::QueryFailed, dependency_insert,
                                QStringLiteral("record pack dependency"));
        }
    }
    if (const auto committed = commit(); !committed) {
        return std::unexpected(committed.error());
    }
    return InstalledPack{revision, staged->sha256, installed_at_utc,
                         staged->loaded.dependencies};
}

std::expected<LoadedPack, CatalogError>
PackCatalog::load(const model::PackId& id, const std::string& version) const {
    if (!validText(asQString(id.value)) || !validText(asQString(version))) {
        return fail(CatalogErrorCode::InvalidConfiguration,
                    QStringLiteral("Pack ID and version are required"));
    }
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "SELECT digest, archive_sha256 FROM pack_revisions WHERE pack_id = ? AND version = ?"));
    query.addBindValue(asQString(id.value));
    query.addBindValue(asQString(version));
    if (!query.exec()) {
        return queryFailure(CatalogErrorCode::QueryFailed, query,
                            QStringLiteral("load installed pack"));
    }
    if (!query.next()) {
        return fail(CatalogErrorCode::NotFound, QStringLiteral("Pack is not installed"));
    }
    const auto expected_digest = query.value(0).toString();
    const auto archive_sha = query.value(1).toString();
    if (!validDigest(expected_digest) || !validDigest(archive_sha)) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Installed pack metadata is corrupt"));
    }
    const auto path = QDir(archivesDirectory()).filePath(archive_sha + QStringLiteral(".awpack"));
    const auto loaded = PackArchive::importArchive(path);
    if (!loaded || asQString(loaded->revision.digest) != expected_digest ||
        loaded->revision.id != id || loaded->revision.version != version) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Installed archive does not match its catalog revision"));
    }
    return *loaded;
}

std::expected<std::vector<InstalledPack>, CatalogError> PackCatalog::list() const {
    QSqlQuery query(database_);
    if (!query.exec(QStringLiteral(
            "SELECT pack_id, version, digest, archive_sha256, installed_at_utc "
            "FROM pack_revisions ORDER BY pack_id, version"))) {
        return queryFailure(CatalogErrorCode::QueryFailed, query,
                            QStringLiteral("list installed packs"));
    }
    std::vector<InstalledPack> packs;
    while (query.next()) {
        const auto revision = revisionFromQuery(query);
        const auto dependencies = dependenciesFor(database_, revision);
        if (!dependencies) {
            return std::unexpected(dependencies.error());
        }
        packs.push_back(InstalledPack{revision, query.value(3).toString(),
                                     query.value(4).toString(), *dependencies});
    }
    return packs;
}

int PackCatalog::schemaVersion() const {
    QSqlQuery query(database_);
    if (!query.exec(QStringLiteral("SELECT COALESCE(MAX(version), 0) FROM catalog_migrations")) ||
        !query.next()) {
        return -1;
    }
    return query.value(0).toInt();
}

std::expected<void, CatalogError> PackCatalog::beginImmediate() {
    return execStatement(database_, QStringLiteral("BEGIN IMMEDIATE"),
                         CatalogErrorCode::QueryFailed, QStringLiteral("begin transaction"));
}

std::expected<void, CatalogError> PackCatalog::commit() {
    return execStatement(database_, QStringLiteral("COMMIT"), CatalogErrorCode::QueryFailed,
                         QStringLiteral("commit transaction"));
}

void PackCatalog::rollback() {
    QSqlQuery query(database_);
    static_cast<void>(query.exec(QStringLiteral("ROLLBACK")));
}

} // namespace appellate::packs
