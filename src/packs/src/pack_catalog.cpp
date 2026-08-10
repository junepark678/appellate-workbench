#include "appellate/packs/pack_catalog.hpp"
#include "appellate/packs/capability_registry.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QLockFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryFile>
#include <QUuid>
#include <QVariant>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <tuple>
#include <unordered_map>
#include <utility>

#if defined(Q_OS_UNIX)
#include <fcntl.h>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <io.h>
#endif

namespace appellate::packs {
namespace {

constexpr auto current_schema_version = 2;
constexpr qsizetype copy_buffer_bytes = 64 * 1024;
constexpr qsizetype maximum_text_characters = 512;
constexpr std::size_t maximum_resolved_revisions = 128;
constexpr std::size_t maximum_resolved_resources = 10'000;

[[nodiscard]] auto fail(CatalogErrorCode code, QString message) -> std::unexpected<CatalogError> {
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

[[nodiscard]] bool validBlobDescriptor(const model::BlobDescriptor& descriptor) {
    const auto limits = PackArchiveLimits{};
    return validText(QString::fromStdString(descriptor.path)) &&
           descriptor.media_type == "application/pdf" && descriptor.byte_size > 0 &&
           descriptor.byte_size <= limits.maximum_file_bytes &&
           descriptor.byte_size <= limits.maximum_total_bytes &&
           validDigest(QString::fromStdString(descriptor.sha256));
}

[[nodiscard]] QString asQString(const std::string& value) { return QString::fromUtf8(value); }

[[nodiscard]] bool revisionLess(const model::PackRevision& left, const model::PackRevision& right) {
    return std::tie(left.id.value, left.version, left.digest) <
           std::tie(right.id.value, right.version, right.digest);
}

struct RevisionLess final {
    [[nodiscard]] bool operator()(const model::PackRevision& left,
                                  const model::PackRevision& right) const {
        return revisionLess(left, right);
    }
};

[[nodiscard]] auto normalizedDependencies(std::vector<model::PackDependency> dependencies) {
    std::ranges::sort(dependencies, [](const auto& left, const auto& right) {
        return revisionLess(left.revision, right.revision);
    });
    return dependencies;
}

[[nodiscard]] model::PackRevision revisionFromQuery(const QSqlQuery& query, int offset = 0) {
    return model::PackRevision{
        model::PackId{query.value(offset).toString().toUtf8().toStdString()},
        query.value(offset + 1).toString().toUtf8().toStdString(),
        query.value(offset + 2).toString().toLatin1().toStdString(),
    };
}

void addUint64(QCryptographicHash& hash, std::uint64_t value) {
    std::array<char, 8> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto shift = static_cast<unsigned>((bytes.size() - index - 1U) * 8U);
        bytes.at(index) = static_cast<char>((value >> shift) & 0xffU);
    }
    hash.addData(QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())));
}

void addFrame(QCryptographicHash& hash, const std::string& value) {
    addUint64(hash, value.size());
    hash.addData(QByteArrayView(value.data(), static_cast<qsizetype>(value.size())));
}

[[nodiscard]] QString blobSetDigest(const model::PackRevision& revision,
                                    std::vector<model::BlobDescriptor> blobs) {
    std::ranges::sort(blobs, [](const auto& left, const auto& right) {
        return std::tie(left.path, left.media_type, left.byte_size, left.sha256) <
               std::tie(right.path, right.media_type, right.byte_size, right.sha256);
    });
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, "appellate-workbench-catalog-blob-set-v1");
    addFrame(hash, revision.id.value);
    addFrame(hash, revision.version);
    addFrame(hash, revision.digest);
    addUint64(hash, blobs.size());
    for (const auto& blob : blobs) {
        addFrame(hash, blob.path);
        addFrame(hash, blob.media_type);
        addUint64(hash, blob.byte_size);
        addFrame(hash, blob.sha256);
    }
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] auto queryFailure(CatalogErrorCode code, const QSqlQuery& query,
                                const QString& action) -> std::unexpected<CatalogError> {
    return fail(code, QStringLiteral("%1: %2").arg(action, query.lastError().text()));
}

[[nodiscard]] auto execStatement(QSqlDatabase& database, const QString& sql, CatalogErrorCode code,
                                 const QString& action) -> std::expected<void, CatalogError> {
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

struct PublishedPath final {
    QString path;
    bool newly_created{};
};

class ScopeExit final {
  public:
    explicit ScopeExit(std::function<void()> action) : action_(std::move(action)) {}
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ~ScopeExit() {
        if (active_) {
            action_();
        }
    }

    void dismiss() noexcept { active_ = false; }

  private:
    std::function<void()> action_;
    bool active_{true};
};

[[nodiscard]] auto hashArchiveFile(const QString& path) -> std::expected<QString, CatalogError> {
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink()) {
        return fail(CatalogErrorCode::CannotStoreArchive,
                    QStringLiteral("Archive object is missing, linked, or not a regular file"));
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(CatalogErrorCode::CannotStoreArchive,
                    QStringLiteral("Cannot read archive object"));
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::array<char, copy_buffer_bytes> buffer{};
    while (true) {
        const auto read = file.read(buffer.data(), static_cast<qint64>(buffer.size()));
        if (read < 0) {
            return fail(CatalogErrorCode::CannotStoreArchive,
                        QStringLiteral("Cannot hash the complete archive object"));
        }
        if (read == 0) {
            break;
        }
        hash.addData(QByteArrayView(buffer.data(), read));
    }
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] auto verifyBlobObject(const QString& path, const model::BlobDescriptor& descriptor,
                                    CatalogErrorCode error_code)
    -> std::expected<void, CatalogError> {
    const QFileInfo before(path);
    if (!before.isFile() || before.isSymLink() || before.size() < 0 ||
        static_cast<std::uint64_t>(before.size()) != descriptor.byte_size ||
        descriptor.media_type != "application/pdf" || !validDigest(asQString(descriptor.sha256))) {
        return fail(error_code, QStringLiteral("Blob object is missing, linked, or corrupt"));
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(error_code, QStringLiteral("Cannot open blob object for verification"));
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::array<char, copy_buffer_bytes> buffer{};
    std::uint64_t total = 0;
    while (true) {
        const auto read = file.read(buffer.data(), static_cast<qint64>(buffer.size()));
        if (read < 0) {
            return fail(error_code, QStringLiteral("Cannot verify the complete blob object"));
        }
        if (read == 0) {
            break;
        }
        const auto chunk = static_cast<std::uint64_t>(read);
        if (chunk > descriptor.byte_size || total > descriptor.byte_size - chunk) {
            return fail(error_code, QStringLiteral("Blob object exceeds its declared size"));
        }
        hash.addData(QByteArrayView(buffer.data(), read));
        total += chunk;
    }
    const QFileInfo after(path);
    if (file.error() != QFileDevice::NoError || total != descriptor.byte_size ||
        after.isSymLink() || !after.isFile() || after.size() != before.size() ||
        QString::fromLatin1(hash.result().toHex()).toStdString() != descriptor.sha256) {
        return fail(error_code, QStringLiteral("Blob object does not match its descriptor"));
    }
    return {};
}

[[nodiscard]] auto
ensureBlobObject(const QString& archive_path, const model::PackRevision& exact_revision,
                 const model::BlobDescriptor& descriptor, const QString& objects_directory,
                 CatalogErrorCode invalid_existing_code, CatalogErrorCode invalid_archive_code,
                 PublishedPath* attempted_publication = nullptr)
    -> std::expected<PublishedPath, CatalogError> {
    if (attempted_publication != nullptr) {
        *attempted_publication = {};
    }
    const auto digest = asQString(descriptor.sha256);
    const QFileInfo objects_info(objects_directory);
    if (!objects_info.isDir() || objects_info.isSymLink()) {
        return fail(invalid_existing_code,
                    QStringLiteral("Blob-object directory is missing or unsafe"));
    }
    if (!validDigest(digest)) {
        return fail(invalid_archive_code,
                    QStringLiteral("Validated blob has an invalid content digest"));
    }
    const auto final_path = QDir(objects_directory).filePath(digest);
    const QFileInfo existing(final_path);
    if (existing.isSymLink()) {
        return fail(invalid_existing_code,
                    QStringLiteral("Content-addressed blob object cannot be a symbolic link"));
    }
    if (existing.exists()) {
        const auto verified = verifyBlobObject(final_path, descriptor, invalid_existing_code);
        if (!verified) {
            return std::unexpected(verified.error());
        }
        const auto publication = PublishedPath{final_path, false};
        if (attempted_publication != nullptr) {
            *attempted_publication = publication;
        }
        return publication;
    }

    QTemporaryFile temporary(QDir(objects_directory).filePath(QStringLiteral(".blob-XXXXXX.tmp")));
    temporary.setAutoRemove(false);
    if (!temporary.open() ||
        !QFile::setPermissions(temporary.fileName(),
                               QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        const auto temporary_path = temporary.fileName();
        temporary.setAutoRemove(true);
        temporary.close();
        if (!temporary_path.isEmpty()) {
            QFile::remove(temporary_path);
        }
        return fail(CatalogErrorCode::CannotStoreBlob,
                    QStringLiteral("Cannot create private blob staging file"));
    }
    const auto temporary_path = temporary.fileName();
    const auto streamed =
        PackArchive::streamValidatedBlob(archive_path, exact_revision, descriptor, temporary);
    if (!streamed) {
        temporary.setAutoRemove(true);
        temporary.close();
        QFile::remove(temporary_path);
        return fail(invalid_archive_code, streamed.error().message);
    }
    if (!syncFile(temporary)) {
        temporary.setAutoRemove(true);
        temporary.close();
        QFile::remove(temporary_path);
        return fail(CatalogErrorCode::CannotStoreBlob,
                    QStringLiteral("Cannot durably flush staged blob object"));
    }
    temporary.close();

    if (QFileInfo(final_path).isSymLink()) {
        QFile::remove(temporary_path);
        return fail(invalid_existing_code,
                    QStringLiteral("Content-addressed blob object became a symbolic link"));
    }
    if (!QFile::rename(temporary_path, final_path)) {
        QFile::remove(temporary_path);
        const auto raced = verifyBlobObject(final_path, descriptor, invalid_existing_code);
        if (!raced) {
            return std::unexpected(raced.error());
        }
        const auto publication = PublishedPath{final_path, false};
        if (attempted_publication != nullptr) {
            *attempted_publication = publication;
        }
        return publication;
    }
    const auto publication = PublishedPath{final_path, true};
    if (attempted_publication != nullptr) {
        *attempted_publication = publication;
    }
    if (!syncDirectory(objects_directory)) {
        return fail(CatalogErrorCode::CannotStoreBlob,
                    QStringLiteral("Cannot durably flush blob-object directory"));
    }
    const auto verified = verifyBlobObject(final_path, descriptor, invalid_existing_code);
    if (!verified) {
        return std::unexpected(verified.error());
    }
    return publication;
}

[[nodiscard]] auto stageArchive(const QString& source_path, const QString& archives_directory)
    -> std::expected<StagedArchive, CatalogError> {
    const auto initially_loaded =
        PackArchive::importArchive(source_path, {}, PackValidationScope::ResolvedClosure);
    if (!initially_loaded) {
        return fail(initially_loaded.error().code == ErrorCode::UnsupportedCapability
                        ? CatalogErrorCode::UnsupportedCapability
                        : CatalogErrorCode::ArchiveInvalid,
                    initially_loaded.error().message);
    }

    QFile source(source_path);
    if (!source.open(QIODevice::ReadOnly)) {
        return fail(CatalogErrorCode::CannotStoreArchive,
                    QStringLiteral("Cannot open archive for installation"));
    }
    QTemporaryFile staged(QDir(archives_directory).filePath(QStringLiteral(".awpack-XXXXXX.tmp")));
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

    const auto reloaded =
        PackArchive::importArchive(staged_path, {}, PackValidationScope::ResolvedClosure);
    if (!reloaded) {
        QFile::remove(staged_path);
        return fail(reloaded.error().code == ErrorCode::UnsupportedCapability
                        ? CatalogErrorCode::UnsupportedCapability
                        : CatalogErrorCode::ArchiveInvalid,
                    reloaded.error().message);
    }
    if (reloaded->revision != initially_loaded->revision) {
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

[[nodiscard]] auto installStagedFile(StagedArchive& staged, const QString& archives_directory,
                                     PublishedPath* attempted_publication)
    -> std::expected<PublishedPath, CatalogError> {
    *attempted_publication = {};
    const auto final_path =
        QDir(archives_directory).filePath(staged.sha256 + QStringLiteral(".awpack"));
    if (QFileInfo::exists(final_path)) {
        QFile::remove(staged.path);
        const auto existing_hash = hashArchiveFile(final_path);
        const auto existing =
            PackArchive::importArchive(final_path, {}, PackValidationScope::ResolvedClosure);
        if (!existing_hash || *existing_hash != staged.sha256 || !existing ||
            existing->revision != staged.loaded.revision) {
            return fail(CatalogErrorCode::CannotStoreArchive,
                        QStringLiteral("Existing archive object is corrupt"));
        }
        const auto publication = PublishedPath{final_path, false};
        *attempted_publication = publication;
        return publication;
    }
    if (!QFile::rename(staged.path, final_path)) {
        QFile::remove(staged.path);
        const auto existing_hash = hashArchiveFile(final_path);
        const auto existing =
            PackArchive::importArchive(final_path, {}, PackValidationScope::ResolvedClosure);
        if (!existing_hash || *existing_hash != staged.sha256 || !existing ||
            existing->revision != staged.loaded.revision) {
            return fail(CatalogErrorCode::CannotStoreArchive,
                        QStringLiteral("Cannot atomically install archive object"));
        }
        const auto publication = PublishedPath{final_path, false};
        *attempted_publication = publication;
        return publication;
    }
    const auto publication = PublishedPath{final_path, true};
    *attempted_publication = publication;
    if (!syncDirectory(archives_directory)) {
        return fail(CatalogErrorCode::CannotStoreArchive,
                    QStringLiteral("Cannot durably flush the archive directory"));
    }
    return publication;
}

[[nodiscard]] bool catalogReferencesArchive(const QSqlDatabase& database,
                                            const QString& archive_sha256) {
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT 1 FROM pack_revisions WHERE archive_sha256 = ? LIMIT 1"));
    query.addBindValue(archive_sha256);
    // Fail closed: an unreadable catalog is never permission to delete a durable object.
    return !query.exec() || query.next();
}

[[nodiscard]] bool catalogReferencesBlob(const QSqlDatabase& database, const QString& blob_sha256) {
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT 1 FROM pack_blobs WHERE sha256 = ? LIMIT 1"));
    query.addBindValue(blob_sha256);
    // Fail closed: an unreadable catalog is never permission to delete a durable object.
    return !query.exec() || query.next();
}

[[nodiscard]] auto dependenciesFor(const QSqlDatabase& database,
                                   const model::PackRevision& revision)
    -> std::expected<std::vector<model::PackDependency>, CatalogError> {
    QSqlQuery query(database);
    query.prepare(
        QStringLiteral("SELECT dependency_pack_id, dependency_version, dependency_digest "
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

[[nodiscard]] auto blobsFor(const QSqlDatabase& database, const model::PackRevision& revision)
    -> std::expected<std::vector<model::BlobDescriptor>, CatalogError> {
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT path, media_type, byte_size, sha256 FROM pack_blobs "
                                 "WHERE pack_id = ? AND version = ? ORDER BY path"));
    query.addBindValue(asQString(revision.id.value));
    query.addBindValue(asQString(revision.version));
    if (!query.exec()) {
        return queryFailure(CatalogErrorCode::QueryFailed, query,
                            QStringLiteral("load pack blob descriptors"));
    }
    std::vector<model::BlobDescriptor> blobs;
    while (query.next()) {
        bool size_ok = false;
        const auto signed_size = query.value(2).toLongLong(&size_ok);
        if (!size_ok || signed_size <= 0) {
            return fail(CatalogErrorCode::CorruptCatalog,
                        QStringLiteral("Installed blob descriptor is corrupt"));
        }
        auto descriptor = model::BlobDescriptor{
            query.value(0).toString().toStdString(),
            query.value(1).toString().toStdString(),
            static_cast<std::uint64_t>(signed_size),
            query.value(3).toString().toStdString(),
        };
        if (!validBlobDescriptor(descriptor)) {
            return fail(CatalogErrorCode::CorruptCatalog,
                        QStringLiteral("Installed blob descriptor is corrupt"));
        }
        blobs.push_back(std::move(descriptor));
    }
    QSqlQuery set(database);
    set.prepare(QStringLiteral("SELECT blob_count, descriptor_sha256 FROM pack_blob_sets "
                               "WHERE pack_id = ? AND version = ?"));
    set.addBindValue(asQString(revision.id.value));
    set.addBindValue(asQString(revision.version));
    if (!set.exec()) {
        return queryFailure(CatalogErrorCode::QueryFailed, set,
                            QStringLiteral("load pack blob-set integrity record"));
    }
    bool count_ok = false;
    if (!set.next()) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Installed pack has no blob-set integrity record"));
    }
    const auto count = set.value(0).toLongLong(&count_ok);
    const auto digest = set.value(1).toString();
    if (!count_ok || count < 0 || static_cast<std::uint64_t>(count) != blobs.size() ||
        !validDigest(digest) || digest != blobSetDigest(revision, blobs) || set.next()) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Installed pack blob-set integrity record is corrupt"));
    }
    return blobs;
}

[[nodiscard]] auto recordBlobs(QSqlDatabase& database, const model::PackRevision& revision,
                               const std::vector<model::BlobDescriptor>& blobs,
                               CatalogErrorCode error_code, const QString& action)
    -> std::expected<void, CatalogError> {
    QSqlQuery insert(database);
    insert.prepare(QStringLiteral(
        "INSERT INTO pack_blobs(pack_id, version, path, media_type, byte_size, sha256) "
        "VALUES(?, ?, ?, ?, ?, ?)"));
    for (const auto& descriptor : blobs) {
        if (!validBlobDescriptor(descriptor)) {
            return fail(error_code, QStringLiteral("Cannot record invalid blob descriptor"));
        }
        insert.bindValue(0, asQString(revision.id.value));
        insert.bindValue(1, asQString(revision.version));
        insert.bindValue(2, asQString(descriptor.path));
        insert.bindValue(3, asQString(descriptor.media_type));
        insert.bindValue(4, static_cast<qint64>(descriptor.byte_size));
        insert.bindValue(5, asQString(descriptor.sha256));
        if (!insert.exec()) {
            return queryFailure(error_code, insert, action);
        }
    }
    QSqlQuery set(database);
    set.prepare(QStringLiteral(
        "INSERT INTO pack_blob_sets(pack_id, version, blob_count, descriptor_sha256) "
        "VALUES(?, ?, ?, ?)"));
    set.addBindValue(asQString(revision.id.value));
    set.addBindValue(asQString(revision.version));
    set.addBindValue(static_cast<qint64>(blobs.size()));
    set.addBindValue(blobSetDigest(revision, blobs));
    if (!set.exec()) {
        return queryFailure(error_code, set, action);
    }
    return {};
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
    const auto blobs = QDir(absolute_root).filePath(QStringLiteral("blobs"));
    if (!QDir{}.mkpath(blobs) || QFileInfo(blobs).isSymLink()) {
        return fail(CatalogErrorCode::InvalidConfiguration,
                    QStringLiteral("Blob-object directory is unsafe or cannot be created"));
    }

    auto catalog = std::unique_ptr<PackCatalog>(new PackCatalog(
        absolute_root,
        QStringLiteral("appellate-packs-%1").arg(QUuid::createUuid().toString(QUuid::Id128))));
    catalog->database_ =
        QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), catalog->connection_name_);
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
        if (const auto result =
                execStatement(database_, QLatin1StringView(sql), CatalogErrorCode::CannotOpen,
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
        "CREATE TABLE IF NOT EXISTS pack_blobs ("
        "pack_id TEXT NOT NULL, version TEXT NOT NULL, path TEXT NOT NULL, "
        "media_type TEXT NOT NULL CHECK(media_type = 'application/pdf'), "
        "byte_size INTEGER NOT NULL CHECK(byte_size BETWEEN 1 AND 536870912), "
        "sha256 TEXT NOT NULL CHECK(length(sha256) = 64 AND sha256 = lower(sha256) "
        "AND sha256 NOT GLOB '*[^0-9a-f]*'), "
        "PRIMARY KEY(pack_id, version, path), "
        "FOREIGN KEY(pack_id, version) REFERENCES pack_revisions(pack_id, version) "
        "ON DELETE CASCADE) STRICT",
        "CREATE INDEX IF NOT EXISTS pack_blobs_sha256 ON pack_blobs(sha256)",
        "CREATE TABLE IF NOT EXISTS pack_blob_sets ("
        "pack_id TEXT NOT NULL, version TEXT NOT NULL, "
        "blob_count INTEGER NOT NULL CHECK(blob_count BETWEEN 0 AND 10000), "
        "descriptor_sha256 TEXT NOT NULL CHECK(length(descriptor_sha256) = 64 "
        "AND descriptor_sha256 = lower(descriptor_sha256) "
        "AND descriptor_sha256 NOT GLOB '*[^0-9a-f]*'), "
        "PRIMARY KEY(pack_id, version), "
        "FOREIGN KEY(pack_id, version) REFERENCES pack_revisions(pack_id, version) "
        "ON DELETE CASCADE) STRICT",
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
    if (version < 2) {
        struct RevisionArchive final {
            model::PackRevision revision;
            QString archive_sha256;
        };
        std::vector<RevisionArchive> installed;
        QSqlQuery revisions(database_);
        if (!revisions.exec(QStringLiteral(
                "SELECT pack_id, version, digest, archive_sha256 FROM pack_revisions "
                "ORDER BY pack_id, version"))) {
            rollback();
            return queryFailure(CatalogErrorCode::MigrationFailed, revisions,
                                QStringLiteral("enumerate packs for blob migration"));
        }
        while (revisions.next()) {
            installed.push_back(
                RevisionArchive{revisionFromQuery(revisions), revisions.value(3).toString()});
        }
        for (const auto& item : installed) {
            if (!validDigest(item.archive_sha256)) {
                rollback();
                return fail(CatalogErrorCode::MigrationFailed,
                            QStringLiteral("Cannot migrate corrupt archive metadata"));
            }
            const auto archive_path =
                QDir(archivesDirectory()).filePath(item.archive_sha256 + QStringLiteral(".awpack"));
            const auto archive_hash = hashArchiveFile(archive_path);
            const auto loaded =
                PackArchive::importArchive(archive_path, {}, PackValidationScope::ResolvedClosure);
            if (!archive_hash || *archive_hash != item.archive_sha256 || !loaded ||
                loaded->revision != item.revision) {
                rollback();
                return fail(CatalogErrorCode::MigrationFailed,
                            QStringLiteral("Cannot validate archive during blob migration"));
            }
            const auto recorded = recordBlobs(database_, item.revision, loaded->blobs,
                                              CatalogErrorCode::MigrationFailed,
                                              QStringLiteral("migrate pack blob descriptors"));
            if (!recorded) {
                rollback();
                return std::unexpected(recorded.error());
            }
        }
    }
    for (auto next_version = version + 1; next_version <= current_schema_version; ++next_version) {
        QSqlQuery record(database_);
        record.prepare(
            QStringLiteral("INSERT INTO catalog_migrations(version, applied_at_utc) VALUES(?, ?)"));
        record.addBindValue(next_version);
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

QString PackCatalog::blobObjectsDirectory() const {
    return QDir(root_directory_).filePath(QStringLiteral("blobs"));
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
    const auto& revision = staged->loaded.revision;
    if (!validText(asQString(revision.id.value)) || !validText(asQString(revision.version)) ||
        !validDigest(asQString(revision.digest)) || !validDigest(staged->sha256)) {
        QFile::remove(staged->path);
        return fail(CatalogErrorCode::ArchiveInvalid,
                    QStringLiteral("Archive revision is invalid"));
    }
    for (const auto& dependency : staged->loaded.dependencies) {
        if (dependency.revision.id == revision.id &&
            dependency.revision.version == revision.version) {
            QFile::remove(staged->path);
            return fail(CatalogErrorCode::DependencyCycle,
                        QStringLiteral("Pack cannot depend on its own ID and version"));
        }
    }

    // Archive/CAS publication, the SQLite commit, and failure cleanup form one catalog-local
    // critical section. Without this lock, a failed installer could observe a shared object as
    // unreferenced and delete it immediately before another process committed its reference.
    QLockFile installation_lock(QDir(root_directory_).filePath(QStringLiteral(".install.lock")));
    installation_lock.setStaleLockTime(0);
    if (!installation_lock.tryLock()) {
        QFile::remove(staged->path);
        return fail(CatalogErrorCode::CannotStoreArchive,
                    QStringLiteral("Another pack installation is active in this catalog"));
    }

    const auto candidate = resolveClosure(revision, &staged->loaded);
    if (!candidate) {
        QFile::remove(staged->path);
        return std::unexpected(candidate.error());
    }

    QSqlQuery preexisting(database_);
    preexisting.prepare(
        QStringLiteral("SELECT digest, archive_sha256, installed_at_utc FROM pack_revisions "
                       "WHERE pack_id = ? AND version = ?"));
    preexisting.addBindValue(asQString(revision.id.value));
    preexisting.addBindValue(asQString(revision.version));
    if (!preexisting.exec()) {
        QFile::remove(staged->path);
        return queryFailure(CatalogErrorCode::QueryFailed, preexisting,
                            QStringLiteral("preflight immutable pack version"));
    }
    if (preexisting.next()) {
        const auto existing_digest = preexisting.value(0).toString();
        const auto existing_archive = preexisting.value(1).toString();
        const auto existing_installed_at = preexisting.value(2).toString();
        if (existing_digest != asQString(revision.digest) || !validDigest(existing_archive) ||
            preexisting.next()) {
            QFile::remove(staged->path);
            return fail(CatalogErrorCode::ImmutableConflict,
                        QStringLiteral("Pack ID and version already identify another revision"));
        }
        const auto resolved = loadResolved(revision);
        if (!resolved) {
            QFile::remove(staged->path);
            return std::unexpected(resolved.error());
        }
        if (normalizedDependencies(resolved->root().dependencies) !=
                normalizedDependencies(staged->loaded.dependencies) ||
            resolved->root().blobs != staged->loaded.blobs) {
            QFile::remove(staged->path);
            return fail(CatalogErrorCode::CorruptCatalog,
                        QStringLiteral("Installed pack differs from staged revision"));
        }
        const auto installed = InstalledPack{revision, existing_archive, existing_installed_at,
                                             resolved->root().dependencies};
        QFile::remove(staged->path);
        return installed;
    }

    std::optional<PublishedPath> published_archive;
    std::vector<std::pair<QString, PublishedPath>> published_blobs;
    bool transaction_started = false;
    ScopeExit rollback_publications([&] {
        if (transaction_started) {
            rollback();
            transaction_started = false;
        }

        bool removed_blob = false;
        for (auto iterator = published_blobs.rbegin(); iterator != published_blobs.rend();
             ++iterator) {
            const auto& [digest, publication] = *iterator;
            if (publication.newly_created && !catalogReferencesBlob(database_, digest) &&
                QFile::remove(publication.path)) {
                removed_blob = true;
            }
        }
        if (removed_blob) {
            static_cast<void>(syncDirectory(blobObjectsDirectory()));
        }

        if (published_archive.has_value() && published_archive->newly_created &&
            !catalogReferencesArchive(database_, staged->sha256) &&
            QFile::remove(published_archive->path)) {
            static_cast<void>(syncDirectory(archivesDirectory()));
        }
        // installStagedFile normally moved or removed this path. Removing it here also covers
        // failures before publication without risking any pre-existing catalog object.
        static_cast<void>(QFile::remove(staged->path));
    });

    PublishedPath attempted_archive;
    const auto final_path = installStagedFile(*staged, archivesDirectory(), &attempted_archive);
    if (attempted_archive.newly_created) {
        published_archive = attempted_archive;
    }
    if (!final_path) {
        return std::unexpected(final_path.error());
    }
    published_archive = *final_path;
    const auto archive_hash_before = hashArchiveFile(final_path->path);
    if (!archive_hash_before || *archive_hash_before != staged->sha256) {
        return fail(CatalogErrorCode::CannotStoreArchive,
                    QStringLiteral("Installed archive object changed before blob materialization"));
    }
    for (const auto& blob : staged->loaded.blobs) {
        PublishedPath attempted_blob;
        const auto materialized = ensureBlobObject(
            final_path->path, revision, blob, blobObjectsDirectory(),
            CatalogErrorCode::CannotStoreBlob, CatalogErrorCode::ArchiveInvalid, &attempted_blob);
        if (attempted_blob.newly_created) {
            published_blobs.emplace_back(asQString(blob.sha256), attempted_blob);
        }
        if (!materialized) {
            return std::unexpected(materialized.error());
        }
    }
    const auto archive_hash_after = hashArchiveFile(final_path->path);
    if (!archive_hash_after || *archive_hash_after != staged->sha256) {
        return fail(CatalogErrorCode::CannotStoreArchive,
                    QStringLiteral("Installed archive object changed during blob materialization"));
    }

    if (const auto begun = beginImmediate(); !begun) {
        return std::unexpected(begun.error());
    }
    transaction_started = true;
    QSqlQuery existing(database_);
    existing.prepare(
        QStringLiteral("SELECT digest, archive_sha256, installed_at_utc FROM pack_revisions "
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
        const auto recorded_blobs = blobsFor(database_, revision);
        if (!recorded_blobs || *recorded_blobs != staged->loaded.blobs) {
            rollback();
            return fail(CatalogErrorCode::CorruptCatalog,
                        QStringLiteral("Installed pack blob descriptors are corrupt"));
        }
        const auto resolved = loadResolved(revision);
        if (!resolved) {
            rollback();
            return std::unexpected(resolved.error());
        }
        const auto installed = InstalledPack{
            revision,
            existing.value(1).toString(),
            existing.value(2).toString(),
            resolved->root().dependencies,
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
    const auto blobs_recorded =
        recordBlobs(database_, revision, staged->loaded.blobs, CatalogErrorCode::QueryFailed,
                    QStringLiteral("record pack blob descriptors"));
    if (!blobs_recorded) {
        rollback();
        return std::unexpected(blobs_recorded.error());
    }
    QSqlQuery dependency_insert(database_);
    dependency_insert.prepare(
        QStringLiteral("INSERT INTO pack_dependencies(pack_id, version, dependency_pack_id, "
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
    const auto resolved = loadResolved(revision);
    if (!resolved) {
        rollback();
        return std::unexpected(resolved.error());
    }
    if (const auto committed = commit(); !committed) {
        return std::unexpected(committed.error());
    }
    transaction_started = false;
    rollback_publications.dismiss();
    return InstalledPack{revision, staged->sha256, installed_at_utc, resolved->root().dependencies};
}

std::expected<LoadedPack, CatalogError> PackCatalog::load(const model::PackId& id,
                                                          const std::string& version) const {
    if (!validText(asQString(id.value)) || !validText(asQString(version))) {
        return fail(CatalogErrorCode::InvalidConfiguration,
                    QStringLiteral("Pack ID and version are required"));
    }
    QSqlQuery query(database_);
    query.prepare(
        QStringLiteral("SELECT digest FROM pack_revisions WHERE pack_id = ? AND version = ?"));
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
    if (!validDigest(expected_digest) || query.next()) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Installed pack metadata is corrupt"));
    }
    return loadExactRevision(
        model::PackRevision{id, version, expected_digest.toLatin1().toStdString()},
        CatalogErrorCode::NotFound);
}

std::expected<LoadedPack, CatalogError>
PackCatalog::loadExactRevision(const model::PackRevision& exact_revision,
                               CatalogErrorCode missing_code) const {
    if (!validText(asQString(exact_revision.id.value)) ||
        !validText(asQString(exact_revision.version)) ||
        !validDigest(asQString(exact_revision.digest))) {
        return fail(CatalogErrorCode::InvalidConfiguration,
                    QStringLiteral("An exact pack revision is required"));
    }
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "SELECT digest, archive_sha256 FROM pack_revisions WHERE pack_id = ? AND version = ?"));
    query.addBindValue(asQString(exact_revision.id.value));
    query.addBindValue(asQString(exact_revision.version));
    if (!query.exec()) {
        return queryFailure(CatalogErrorCode::QueryFailed, query,
                            QStringLiteral("load exact installed pack"));
    }
    if (!query.next() || query.value(0).toString() != asQString(exact_revision.digest)) {
        return fail(missing_code, QStringLiteral("Exact pack revision is not installed"));
    }
    const auto archive_sha = query.value(1).toString();
    if (!validDigest(archive_sha) || query.next()) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Installed pack metadata is corrupt"));
    }
    const auto path = QDir(archivesDirectory()).filePath(archive_sha + QStringLiteral(".awpack"));
    const auto actual_archive_sha = hashArchiveFile(path);
    const auto loaded = PackArchive::importArchive(path, {}, PackValidationScope::ResolvedClosure);
    if (!actual_archive_sha || *actual_archive_sha != archive_sha) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Installed archive does not match its catalog revision"));
    }
    if (!loaded) {
        return fail(loaded.error().code == ErrorCode::UnsupportedCapability
                        ? CatalogErrorCode::UnsupportedCapability
                        : CatalogErrorCode::CorruptCatalog,
                    loaded.error().message);
    }
    if (loaded->revision != exact_revision) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Installed archive does not match its catalog revision"));
    }
    const auto recorded_blobs = blobsFor(database_, loaded->revision);
    if (!recorded_blobs || *recorded_blobs != loaded->blobs) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Installed blob descriptors do not match the archive"));
    }
    const auto recorded_dependencies = dependenciesFor(database_, exact_revision);
    if (!recorded_dependencies || normalizedDependencies(*recorded_dependencies) !=
                                      normalizedDependencies(loaded->dependencies)) {
        return fail(
            CatalogErrorCode::CorruptCatalog,
            QStringLiteral("Installed dependency rows do not match the verified archive manifest"));
    }
    return *loaded;
}

std::expected<ResolvedPack, CatalogError>
PackCatalog::loadResolved(const model::PackRevision& exact_root) const {
    return resolveClosure(exact_root, nullptr);
}

std::expected<ResolvedPack, CatalogError>
PackCatalog::resolveClosure(const model::PackRevision& exact_root,
                            const LoadedPack* staged_root) const {
    if (!validText(asQString(exact_root.id.value)) || !validText(asQString(exact_root.version)) ||
        !validDigest(asQString(exact_root.digest))) {
        return fail(CatalogErrorCode::InvalidConfiguration,
                    QStringLiteral("An exact root pack revision is required"));
    }

    enum class VisitState { Visiting, Complete };
    std::map<model::PackRevision, VisitState, RevisionLess> states;
    std::map<std::string, model::PackRevision> selected_by_pack_id;
    std::vector<LoadedPack> dependencies_dependency_first;
    std::optional<LoadedPack> root;
    std::optional<std::uint32_t> closure_schema_version;
    std::size_t resource_count = 0;

    std::function<std::expected<void, CatalogError>(const model::PackRevision&, bool)> visit;
    visit = [&](const model::PackRevision& exact_revision,
                bool is_root) -> std::expected<void, CatalogError> {
        const auto selected = selected_by_pack_id.find(exact_revision.id.value);
        if (selected != selected_by_pack_id.end() && selected->second != exact_revision) {
            return fail(CatalogErrorCode::DependencyVersionSplit,
                        QStringLiteral("Resolved closure selects more than one revision of pack %1")
                            .arg(asQString(exact_revision.id.value)));
        }

        const auto state = states.find(exact_revision);
        if (state != states.end()) {
            if (state->second == VisitState::Visiting) {
                return fail(CatalogErrorCode::DependencyCycle,
                            QStringLiteral("Resolved pack dependency graph contains a cycle"));
            }
            return {};
        }
        if (states.size() >= maximum_resolved_revisions) {
            return fail(CatalogErrorCode::DependencyClosureTooLarge,
                        QStringLiteral("Resolved pack closure exceeds 128 exact revisions"));
        }

        selected_by_pack_id.insert_or_assign(exact_revision.id.value, exact_revision);
        states.emplace(exact_revision, VisitState::Visiting);
        auto loaded =
            is_root && staged_root != nullptr
                ? std::expected<LoadedPack, CatalogError>{*staged_root}
                : loadExactRevision(exact_revision, is_root ? CatalogErrorCode::NotFound
                                                            : CatalogErrorCode::MissingDependency);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        if (loaded->revision != exact_revision) {
            return fail(CatalogErrorCode::ArchiveInvalid,
                        QStringLiteral("Staged root differs from its exact revision"));
        }
        if (!closure_schema_version.has_value()) {
            closure_schema_version = loaded->manifest_schema_version;
        } else if (*closure_schema_version != loaded->manifest_schema_version) {
            return fail(CatalogErrorCode::InvalidResolvedGraph,
                        QStringLiteral("Resolved pack closure mixes manifest schema generations"));
        }
        std::vector<model::ResourceKind> resource_kinds;
        resource_kinds.reserve(loaded->resources.size());
        for (const auto& resource : loaded->resources) {
            resource_kinds.push_back(resource.descriptor.kind);
        }
        const auto capabilities = CapabilityRegistry::validateCoverage(
            loaded->manifest_schema_version, loaded->required_capabilities, resource_kinds);
        if (!capabilities) {
            return fail(CatalogErrorCode::UnsupportedCapability,
                        QStringLiteral("Pack %1 requires an unsupported capability: %2")
                            .arg(asQString(exact_revision.id.value), capabilities.error().message));
        }
        if (loaded->resources.size() > maximum_resolved_resources - resource_count) {
            return fail(CatalogErrorCode::DependencyClosureTooLarge,
                        QStringLiteral("Resolved pack closure exceeds 10000 resources"));
        }
        resource_count += loaded->resources.size();

        auto direct_dependencies = normalizedDependencies(loaded->dependencies);
        for (const auto& dependency : direct_dependencies) {
            const auto traversed = visit(dependency.revision, false);
            if (!traversed) {
                return std::unexpected(traversed.error());
            }
        }
        states.at(exact_revision) = VisitState::Complete;
        if (is_root) {
            root = std::move(*loaded);
        } else {
            dependencies_dependency_first.push_back(std::move(*loaded));
        }
        return {};
    };

    const auto traversed = visit(exact_root, true);
    if (!traversed) {
        return std::unexpected(traversed.error());
    }
    if (!root.has_value()) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Resolved closure did not produce its exact root"));
    }

    std::unordered_map<std::string, model::PackRevision> resource_owners;
    resource_owners.reserve(resource_count);
    const auto index_resources = [&](const LoadedPack& pack) -> std::expected<void, CatalogError> {
        for (const auto& resource : pack.resources) {
            const auto [found, inserted] =
                resource_owners.emplace(resource.descriptor.id, pack.revision);
            if (!inserted) {
                return fail(CatalogErrorCode::ResourceCollision,
                            QStringLiteral("Resource ID %1 is declared by both %2 and %3")
                                .arg(asQString(resource.descriptor.id),
                                     asQString(found->second.id.value),
                                     asQString(pack.revision.id.value)));
            }
        }
        return {};
    };
    for (const auto& dependency : dependencies_dependency_first) {
        const auto indexed = index_resources(dependency);
        if (!indexed) {
            return std::unexpected(indexed.error());
        }
    }
    if (const auto indexed = index_resources(*root); !indexed) {
        return std::unexpected(indexed.error());
    }

    std::unordered_map<std::string, const model::BlobDescriptor*> root_blobs;
    root_blobs.reserve(root->blobs.size());
    for (const auto& blob : root->blobs) {
        root_blobs.emplace(blob.path, &blob);
    }
    for (const auto& resource : root->resources) {
        if (resource.descriptor.kind == model::ResourceKind::Case) {
            const auto record_id =
                resource.document.value(QStringLiteral("record_id")).toString().toStdString();
            const auto owner = resource_owners.find(record_id);
            if (owner == resource_owners.end() || owner->second != root->revision) {
                return fail(CatalogErrorCode::InvalidResolvedGraph,
                            QStringLiteral("Root case %1 must reference a root-owned record")
                                .arg(asQString(resource.descriptor.id)));
            }
        }
        if (resource.descriptor.kind != model::ResourceKind::Record) {
            continue;
        }
        for (const auto& entry_value :
             resource.document.value(QStringLiteral("docket_entries")).toArray()) {
            const auto entry = entry_value.toObject();
            const auto path = entry.value(QStringLiteral("asset_path")).toString().toStdString();
            const auto digest =
                entry.value(QStringLiteral("asset_sha256")).toString().toStdString();
            const auto blob = root_blobs.find(path);
            if (blob == root_blobs.end() || blob->second->sha256 != digest) {
                return fail(
                    CatalogErrorCode::InvalidResolvedGraph,
                    QStringLiteral("Root record %1 must resolve blobs inside the exact root pack")
                        .arg(asQString(resource.descriptor.id)));
            }
        }
    }

    std::map<model::PackRevision, const LoadedPack*, RevisionLess> packs_by_revision;
    for (const auto& dependency : dependencies_dependency_first) {
        packs_by_revision.emplace(dependency.revision, &dependency);
    }
    packs_by_revision.emplace(root->revision, &*root);
    const auto validate_visible_graph =
        [&](const LoadedPack& owner) -> std::expected<void, CatalogError> {
        std::set<model::PackRevision, RevisionLess> included;
        std::vector<const LoadedPack*> visible_dependency_first;
        std::function<std::expected<void, CatalogError>(const model::PackRevision&)> collect;
        collect = [&](const model::PackRevision& revision) -> std::expected<void, CatalogError> {
            if (included.contains(revision)) {
                return {};
            }
            const auto found = packs_by_revision.find(revision);
            if (found == packs_by_revision.end()) {
                return fail(CatalogErrorCode::CorruptCatalog,
                            QStringLiteral("Resolved dependency is absent from its closure"));
            }
            included.insert(revision);
            for (const auto& dependency : normalizedDependencies(found->second->dependencies)) {
                const auto collected = collect(dependency.revision);
                if (!collected) {
                    return std::unexpected(collected.error());
                }
            }
            visible_dependency_first.push_back(found->second);
            return {};
        };
        for (const auto& dependency : normalizedDependencies(owner.dependencies)) {
            const auto collected = collect(dependency.revision);
            if (!collected) {
                return std::unexpected(collected.error());
            }
        }
        const auto validated = PackReader::validateResolvedGraph(owner, visible_dependency_first);
        if (!validated) {
            return fail(CatalogErrorCode::InvalidResolvedGraph,
                        QStringLiteral("Pack %1 has an invalid resolved resource graph: %2")
                            .arg(asQString(owner.revision.id.value), validated.error().message));
        }
        return {};
    };
    for (const auto& dependency : dependencies_dependency_first) {
        const auto validated = validate_visible_graph(dependency);
        if (!validated) {
            return std::unexpected(validated.error());
        }
    }
    if (const auto validated = validate_visible_graph(*root); !validated) {
        return std::unexpected(validated.error());
    }

    std::vector<model::PackRevision> revisions_by_pack_id;
    revisions_by_pack_id.reserve(selected_by_pack_id.size());
    for (const auto& [pack_id, revision] : selected_by_pack_id) {
        static_cast<void>(pack_id);
        revisions_by_pack_id.push_back(revision);
    }
    return ResolvedPack(std::move(*root), std::move(dependencies_dependency_first),
                        std::move(revisions_by_pack_id));
}

std::expected<MaterializedBlob, CatalogError>
PackCatalog::materializeBlob(const model::PackRevision& exact_revision,
                             const std::string& blob_path) const {
    if (!validText(asQString(exact_revision.id.value)) ||
        !validText(asQString(exact_revision.version)) ||
        !validDigest(asQString(exact_revision.digest)) || !validText(asQString(blob_path))) {
        return fail(CatalogErrorCode::InvalidConfiguration,
                    QStringLiteral("Exact pack revision and blob path are required"));
    }

    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "SELECT digest, archive_sha256 FROM pack_revisions WHERE pack_id = ? AND version = ?"));
    query.addBindValue(asQString(exact_revision.id.value));
    query.addBindValue(asQString(exact_revision.version));
    if (!query.exec()) {
        return queryFailure(CatalogErrorCode::QueryFailed, query,
                            QStringLiteral("resolve installed blob revision"));
    }
    if (!query.next() || query.value(0).toString() != asQString(exact_revision.digest)) {
        return fail(CatalogErrorCode::NotFound,
                    QStringLiteral("Exact pack revision is not installed"));
    }
    const auto descriptors = blobsFor(database_, exact_revision);
    if (!descriptors) {
        return std::unexpected(descriptors.error());
    }
    const auto found = std::ranges::find(*descriptors, blob_path, &model::BlobDescriptor::path);
    if (found == descriptors->end()) {
        return fail(CatalogErrorCode::NotFound,
                    QStringLiteral("Blob path is not declared by the exact pack revision"));
    }
    const auto& descriptor = *found;
    const auto objects_directory = blobObjectsDirectory();
    const QFileInfo objects_info(objects_directory);
    if (!objects_info.isDir() || objects_info.isSymLink()) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Blob-object directory is missing or unsafe"));
    }
    const auto object_path = QDir(objects_directory).filePath(asQString(descriptor.sha256));
    const QFileInfo object_info(object_path);
    if (object_info.isSymLink()) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Content-addressed blob object is a symbolic link"));
    }
    if (object_info.exists()) {
        const auto verified =
            verifyBlobObject(object_path, descriptor, CatalogErrorCode::CorruptCatalog);
        if (!verified) {
            return std::unexpected(verified.error());
        }
        return MaterializedBlob{descriptor, object_path};
    }

    const auto archive_sha = query.value(1).toString();
    if (!validDigest(archive_sha)) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Installed archive digest is corrupt"));
    }
    const auto archive_path =
        QDir(archivesDirectory()).filePath(archive_sha + QStringLiteral(".awpack"));
    const auto hash_before = hashArchiveFile(archive_path);
    if (!hash_before || *hash_before != archive_sha) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Installed archive is missing, linked, or corrupt"));
    }

    const auto archived_descriptor =
        PackArchive::declaredBlob(archive_path, exact_revision, blob_path);
    if (!archived_descriptor) {
        return fail(CatalogErrorCode::CorruptCatalog, archived_descriptor.error().message);
    }
    if (*archived_descriptor != descriptor) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Catalog and archive blob descriptors differ"));
    }
    PublishedPath attempted_blob;
    const auto materialized_path = ensureBlobObject(
        archive_path, exact_revision, descriptor, objects_directory,
        CatalogErrorCode::CorruptCatalog, CatalogErrorCode::CorruptCatalog, &attempted_blob);
    if (!materialized_path) {
        if (attempted_blob.newly_created && QFile::remove(attempted_blob.path)) {
            static_cast<void>(syncDirectory(objects_directory));
        }
        return std::unexpected(materialized_path.error());
    }
    const auto hash_after = hashArchiveFile(archive_path);
    if (!hash_after || *hash_after != archive_sha) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Installed archive changed during blob resolution"));
    }
    return MaterializedBlob{descriptor, materialized_path->path};
}

std::expected<MaterializedBlob, CatalogError>
PackCatalog::materializeBlob(const ResolvedPack& closure,
                             const model::PackRevision& owning_revision,
                             const std::string& blob_path) const {
    if (!closure.containsRevision(owning_revision)) {
        return fail(CatalogErrorCode::InvalidConfiguration,
                    QStringLiteral("Blob owner is outside the resolved pack closure"));
    }
    return materializeBlob(owning_revision, blob_path);
}

std::expected<std::vector<InstalledPack>, CatalogError> PackCatalog::list() const {
    QSqlQuery query(database_);
    if (!query.exec(
            QStringLiteral("SELECT pack_id, version, digest, archive_sha256, installed_at_utc "
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
