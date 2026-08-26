#include "appellate/storage/asset_store.hpp"

#include "appellate/storage/detail/private_state.hpp"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QRegularExpression>
#include <QUuid>

#include <array>
#include <cerrno>
#include <cstring>
#include <exception>
#include <utility>
#include <vector>

#if defined(Q_OS_UNIX)
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace appellate::storage {
namespace {

constexpr qsizetype io_buffer_bytes = 64 * 1024;

[[nodiscard]] auto fail(AssetStoreErrorCode code, QString message)
    -> std::unexpected<AssetStoreError> {
    return std::unexpected(AssetStoreError{code, std::move(message)});
}

[[nodiscard]] bool isLowercaseSha256(QStringView digest) {
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
    return pattern.matchView(digest).hasMatch();
}

#if defined(Q_OS_UNIX)
[[nodiscard]] QString systemError(QStringView action) {
    return QStringLiteral("%1: %2").arg(action, QString::fromLocal8Bit(std::strerror(errno)));
}

[[nodiscard]] bool syncDescriptor(int descriptor) {
    int result{};
    do {
        result = ::fsync(descriptor);
    } while (result != 0 && errno == EINTR);
    return result == 0;
}

[[nodiscard]] int duplicateDescriptor(int descriptor) {
#ifdef F_DUPFD_CLOEXEC
    return ::fcntl(descriptor, F_DUPFD_CLOEXEC, 0);
#else
    return ::dup(descriptor);
#endif
}

[[nodiscard]] auto independentDirectoryDescriptor(int anchored_descriptor)
    -> std::expected<int, AssetStoreError> {
    auto flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const auto descriptor = ::openat(anchored_descriptor, ".", flags);
    if (descriptor < 0) {
        return fail(AssetStoreErrorCode::CannotRead,
                    systemError(u"Open independent asset directory scan"));
    }
    struct stat anchored_status {};
    struct stat opened_status {};
    if (::fstat(anchored_descriptor, &anchored_status) != 0 ||
        ::fstat(descriptor, &opened_status) != 0 ||
        !S_ISDIR(anchored_status.st_mode) || !S_ISDIR(opened_status.st_mode) ||
        anchored_status.st_dev != opened_status.st_dev ||
        anchored_status.st_ino != opened_status.st_ino) {
        static_cast<void>(::close(descriptor));
        return fail(AssetStoreErrorCode::CannotRead,
                    QStringLiteral("Asset directory identity changed before enumeration"));
    }
    if (const auto private_directory = detail::validatePrivateStateDirectoryDescriptor(descriptor);
        !private_directory) {
        static_cast<void>(::close(descriptor));
        return fail(AssetStoreErrorCode::InvalidConfiguration, private_directory.error());
    }
    return descriptor;
}

[[nodiscard]] auto rootHasOnlyInitializationResidue(int anchored_descriptor)
    -> std::expected<bool, AssetStoreError> {
    const auto scan_descriptor = independentDirectoryDescriptor(anchored_descriptor);
    if (!scan_descriptor) {
        return std::unexpected(scan_descriptor.error());
    }
    auto* directory = ::fdopendir(*scan_descriptor);
    if (directory == nullptr) {
        static_cast<void>(::close(*scan_descriptor));
        return fail(AssetStoreErrorCode::CannotRead,
                    systemError(u"Inspect asset-store root"));
    }
    errno = 0;
    while (const auto* entry = ::readdir(directory)) {
        const auto name = QByteArray(entry->d_name);
        if (name == "." || name == "..") {
            errno = 0;
            continue;
        }
        static const QRegularExpression reserved_pattern(
            QStringLiteral("^\\.appellate-(?:directory-stage|quarantine)-[0-9a-f]{32}\\.tmp$"));
        struct stat status{};
        if (!reserved_pattern.match(QString::fromLatin1(name)).hasMatch() ||
            ::fstatat(anchored_descriptor, name.constData(), &status, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISDIR(status.st_mode) || status.st_uid != ::geteuid() ||
            status.st_nlink != static_cast<nlink_t>(2) ||
            ((status.st_mode & 07777) != 0000 && (status.st_mode & 07777) != 0700)) {
            static_cast<void>(::closedir(directory));
            return false;
        }
        errno = 0;
    }
    const auto scan_errno = errno;
    static_cast<void>(::closedir(directory));
    if (scan_errno != 0) {
        errno = scan_errno;
        return fail(AssetStoreErrorCode::CannotRead,
                    systemError(u"Complete asset-store root inspection"));
    }
    return true;
}

[[nodiscard]] auto openAbsoluteDirectoryNoFollow(const QString& absolute_path, bool create)
    -> std::expected<int, AssetStoreError> {
    const auto descriptor = create
                                ? detail::ensurePrivateStateDirectory(absolute_path, absolute_path)
                                : detail::openPrivateStateDirectory(absolute_path);
    return descriptor ? std::expected<int, AssetStoreError>{*descriptor}
                      : fail(create ? AssetStoreErrorCode::CannotCreateDirectory
                                    : AssetStoreErrorCode::InvalidConfiguration,
                             descriptor.error());
}

[[nodiscard]] bool sameFileIdentity(int descriptor, int parent_descriptor,
                                    const QByteArray& name) {
    return detail::validatePrivateStateFileBinding(descriptor, parent_descriptor, name).has_value();
}

[[nodiscard]] int renameNoReplace(int source_directory, const QByteArray& source_name,
                                  int destination_directory, const QByteArray& destination_name) {
#if defined(Q_OS_LINUX) && defined(SYS_renameat2)
    int result{};
    do {
        result =
            static_cast<int>(::syscall(SYS_renameat2, source_directory, source_name.constData(),
                                       destination_directory, destination_name.constData(), 1U));
    } while (result != 0 && errno == EINTR);
    return result;
#else
    Q_UNUSED(source_directory);
    Q_UNUSED(source_name);
    Q_UNUSED(destination_directory);
    Q_UNUSED(destination_name);
    errno = ENOTSUP;
    return -1;
#endif
}

[[nodiscard]] bool isRecoveryQuarantineName(const QByteArray& name) {
    static const QRegularExpression pattern(
        QStringLiteral("^\\.asset-quarantine-[0-9a-f]{32}\\.tmp$"));
    return pattern.match(QString::fromLatin1(name)).hasMatch();
}

struct RecoveryRemoval final {
    QByteArray original_name;
    struct stat original_status{};
    int descriptor{-1};

    RecoveryRemoval(QByteArray name, const struct stat& status, int retained_descriptor)
        : original_name(std::move(name)), original_status(status), descriptor(retained_descriptor) {
    }
    RecoveryRemoval(const RecoveryRemoval&) = delete;
    RecoveryRemoval& operator=(const RecoveryRemoval&) = delete;
    RecoveryRemoval(RecoveryRemoval&& other) noexcept
        : original_name(std::move(other.original_name)), original_status(other.original_status),
          descriptor(std::exchange(other.descriptor, -1)) {}
    RecoveryRemoval& operator=(RecoveryRemoval&&) = delete;
    ~RecoveryRemoval() {
        if (descriptor >= 0) {
            static_cast<void>(::close(descriptor));
        }
    }
};

struct RecoveryFinal final {
    QByteArray name;
    QString digest;
    struct stat original_status{};
    int descriptor{-1};

    RecoveryFinal(QByteArray final_name, QString final_digest, const struct stat& status,
                  int retained_descriptor)
        : name(std::move(final_name)), digest(std::move(final_digest)), original_status(status),
          descriptor(retained_descriptor) {}
    RecoveryFinal(const RecoveryFinal&) = delete;
    RecoveryFinal& operator=(const RecoveryFinal&) = delete;
    RecoveryFinal(RecoveryFinal&& other) noexcept
        : name(std::move(other.name)), digest(std::move(other.digest)),
          original_status(other.original_status), descriptor(std::exchange(other.descriptor, -1)) {}
    RecoveryFinal& operator=(RecoveryFinal&&) = delete;
    ~RecoveryFinal() {
        if (descriptor >= 0) {
            static_cast<void>(::close(descriptor));
        }
    }
};

[[nodiscard]] bool sameStableFileState(const struct stat& left, const struct stat& right) {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
           left.st_size == right.st_size && left.st_mode == right.st_mode &&
#if defined(Q_OS_DARWIN)
           left.st_mtimespec.tv_sec == right.st_mtimespec.tv_sec &&
           left.st_mtimespec.tv_nsec == right.st_mtimespec.tv_nsec &&
           left.st_ctimespec.tv_sec == right.st_ctimespec.tv_sec &&
           left.st_ctimespec.tv_nsec == right.st_ctimespec.tv_nsec;
#else
           left.st_mtim.tv_sec == right.st_mtim.tv_sec &&
           left.st_mtim.tv_nsec == right.st_mtim.tv_nsec &&
           left.st_ctim.tv_sec == right.st_ctim.tv_sec &&
           left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
#endif
}

[[nodiscard]] auto quarantineBoundName(int directory_descriptor, const QByteArray& source_name,
                                       int retained_descriptor, bool& source_detached)
    -> std::expected<QByteArray, AssetStoreError> {
    source_detached = false;
    const auto quarantine_name = QByteArrayLiteral(".asset-quarantine-") +
                                 QUuid::createUuid().toByteArray(QUuid::Id128) +
                                 QByteArrayLiteral(".tmp");
    if (renameNoReplace(directory_descriptor, source_name, directory_descriptor, quarantine_name) !=
        0) {
        return fail(AssetStoreErrorCode::CannotCommit,
                    systemError(u"Quarantine asset-store entry"));
    }
    source_detached = true;

    // The rename is the commit point. Never try to restore the source name: after any namespace
    // race that could move an unverified replacement back into a live digest. A private,
    // no-replace tombstone is non-addressable and can be inspected or reclaimed out of band.
    if (!syncDescriptor(directory_descriptor)) {
        return fail(AssetStoreErrorCode::CannotSync,
                    systemError(u"Flush quarantined asset-store entry"));
    }
    struct stat old_name{};
    const auto source_absent = ::fstatat(directory_descriptor, source_name.constData(), &old_name,
                                         AT_SYMLINK_NOFOLLOW) != 0 &&
                               errno == ENOENT;
    if (!source_absent ||
        !sameFileIdentity(retained_descriptor, directory_descriptor, quarantine_name)) {
        return fail(AssetStoreErrorCode::CorruptObject,
                    QStringLiteral("Asset-store quarantine binding is unsafe"));
    }
    return quarantine_name;
}

[[nodiscard]] auto verifyRetainedFinal(const RecoveryFinal& final, int objects_descriptor,
                                       qint64 max_asset_bytes)
    -> std::expected<void, AssetStoreError> {
    struct stat before{};
    if (::fstat(final.descriptor, &before) != 0 ||
        !sameStableFileState(final.original_status, before) ||
        !sameFileIdentity(final.descriptor, objects_descriptor, final.name)) {
        return fail(
            AssetStoreErrorCode::CorruptObject,
            QStringLiteral("Referenced asset %1 changed during recovery").arg(final.digest));
    }
    if (before.st_size < 0 || before.st_size > max_asset_bytes) {
        return fail(AssetStoreErrorCode::AssetTooLarge,
                    QStringLiteral("Referenced asset %1 exceeds the configured size limit")
                        .arg(final.digest));
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::array<char, io_buffer_bytes> buffer{};
    qint64 offset{};
    while (true) {
        ssize_t count{};
        do {
            count =
                ::pread(final.descriptor, buffer.data(), buffer.size(), static_cast<off_t>(offset));
        } while (count < 0 && errno == EINTR);
        if (count < 0) {
            return fail(AssetStoreErrorCode::CannotRead,
                        systemError(u"Read retained asset during recovery"));
        }
        if (count == 0) {
            break;
        }
        if (count > max_asset_bytes - offset) {
            return fail(AssetStoreErrorCode::AssetTooLarge,
                        QStringLiteral("Referenced asset %1 exceeds the configured size limit")
                            .arg(final.digest));
        }
        hash.addData(QByteArrayView(buffer.data(), count));
        offset += count;
    }

    struct stat after{};
    if (offset != before.st_size || ::fstat(final.descriptor, &after) != 0 ||
        !sameStableFileState(before, after) || !sameStableFileState(final.original_status, after) ||
        !sameFileIdentity(final.descriptor, objects_descriptor, final.name)) {
        return fail(AssetStoreErrorCode::CorruptObject,
                    QStringLiteral("Referenced asset %1 changed while recovery verified it")
                        .arg(final.digest));
    }
    if (QString::fromLatin1(hash.result().toHex()) != final.digest) {
        return fail(
            AssetStoreErrorCode::CorruptObject,
            QStringLiteral("Referenced asset %1 failed digest verification").arg(final.digest));
    }
    return {};
}

[[nodiscard]] auto lockDescriptor(int descriptor) -> std::expected<void, AssetStoreError> {
    while (::flock(descriptor, LOCK_EX) != 0) {
        if (errno != EINTR) {
            return fail(AssetStoreErrorCode::CannotCommit,
                        systemError(u"Acquire asset-store publication lock"));
        }
    }
    return {};
}

void unlockDescriptor(int descriptor) noexcept {
    static_cast<void>(::flock(descriptor, LOCK_UN));
}
#endif

} // namespace

AssetStoreLock::AssetStoreLock(int descriptor) : descriptor_(descriptor) {}

AssetStoreLock::AssetStoreLock(AssetStoreLock&& other) noexcept
    : descriptor_(std::exchange(other.descriptor_, -1)) {}

AssetStoreLock& AssetStoreLock::operator=(AssetStoreLock&& other) noexcept {
    if (this != &other) {
        reset();
        descriptor_ = std::exchange(other.descriptor_, -1);
    }
    return *this;
}

AssetStoreLock::~AssetStoreLock() { reset(); }

void AssetStoreLock::reset() noexcept {
#if defined(Q_OS_UNIX)
    if (descriptor_ >= 0) {
        unlockDescriptor(descriptor_);
        static_cast<void>(::close(descriptor_));
    }
#endif
    descriptor_ = -1;
}

StagedAsset::StagedAsset(int objects_descriptor, int file_descriptor, QString sha256, qint64 size)
    : objects_descriptor_(objects_descriptor), file_descriptor_(file_descriptor),
      sha256_(std::move(sha256)), size_(size) {}

StagedAsset::StagedAsset(StagedAsset&& other) noexcept
    : objects_descriptor_(std::exchange(other.objects_descriptor_, -1)),
      file_descriptor_(std::exchange(other.file_descriptor_, -1)),
      sha256_(std::move(other.sha256_)), size_(other.size_), finalized_(other.finalized_),
      newly_finalized_(other.newly_finalized_) {
    other.finalized_ = false;
    other.newly_finalized_ = false;
}

StagedAsset& StagedAsset::operator=(StagedAsset&& other) noexcept {
    if (this != &other) {
        reset();
        objects_descriptor_ = std::exchange(other.objects_descriptor_, -1);
        file_descriptor_ = std::exchange(other.file_descriptor_, -1);
        sha256_ = std::move(other.sha256_);
        size_ = other.size_;
        finalized_ = other.finalized_;
        newly_finalized_ = other.newly_finalized_;
        other.finalized_ = false;
        other.newly_finalized_ = false;
    }
    return *this;
}

StagedAsset::~StagedAsset() { reset(); }

void StagedAsset::reset() noexcept {
#if defined(Q_OS_UNIX)
    if (file_descriptor_ >= 0) {
        static_cast<void>(::close(file_descriptor_));
    }
    if (objects_descriptor_ >= 0) {
        static_cast<void>(::close(objects_descriptor_));
    }
#endif
    objects_descriptor_ = -1;
    file_descriptor_ = -1;
}

const QString& StagedAsset::sha256() const noexcept { return sha256_; }
qint64 StagedAsset::size() const noexcept { return size_; }
bool StagedAsset::wasDeduplicated() const noexcept { return finalized_ && !newly_finalized_; }

AssetStore::AssetStore(QString root_directory, qint64 max_asset_bytes)
    : root_directory_(std::move(root_directory)), max_asset_bytes_(max_asset_bytes) {
    if (!root_directory_.isEmpty()) {
        root_directory_ = QDir::cleanPath(QFileInfo(root_directory_).absoluteFilePath());
    }
}

AssetStore::AssetStore(AssetStore&& other) noexcept
    : root_directory_(std::move(other.root_directory_)), max_asset_bytes_(other.max_asset_bytes_),
      root_descriptor_(std::exchange(other.root_descriptor_, -1)),
      objects_descriptor_(std::exchange(other.objects_descriptor_, -1)) {}

AssetStore& AssetStore::operator=(AssetStore&& other) noexcept {
    if (this != &other) {
        closeDescriptors();
        root_directory_ = std::move(other.root_directory_);
        max_asset_bytes_ = other.max_asset_bytes_;
        root_descriptor_ = std::exchange(other.root_descriptor_, -1);
        objects_descriptor_ = std::exchange(other.objects_descriptor_, -1);
    }
    return *this;
}

AssetStore::~AssetStore() { closeDescriptors(); }

void AssetStore::closeDescriptors() noexcept {
#if defined(Q_OS_UNIX)
    if (objects_descriptor_ >= 0) {
        static_cast<void>(::close(objects_descriptor_));
    }
    if (root_descriptor_ >= 0) {
        static_cast<void>(::close(root_descriptor_));
    }
#endif
    objects_descriptor_ = -1;
    root_descriptor_ = -1;
}

std::expected<void, AssetStoreError> AssetStore::validateConfiguration() const {
    if (root_directory_.isEmpty() || root_directory_.contains(QChar::Null) ||
        !QDir::isAbsolutePath(root_directory_)) {
        return fail(AssetStoreErrorCode::InvalidConfiguration,
                    QStringLiteral("The asset-store root directory is invalid"));
    }
    if (max_asset_bytes_ <= 0) {
        return fail(AssetStoreErrorCode::InvalidConfiguration,
                    QStringLiteral("The maximum asset size must be positive"));
    }
    return {};
}

std::expected<void, AssetStoreError> AssetStore::ensureReady() const {
    if (const auto configuration = validateConfiguration(); !configuration) {
        return configuration;
    }
#if defined(Q_OS_UNIX)
    if (root_descriptor_ >= 0 && objects_descriptor_ >= 0) {
        const auto root_private = detail::validatePrivateStateDirectoryDescriptor(root_descriptor_);
        const auto objects_private =
            detail::validatePrivateStateDirectoryDescriptor(objects_descriptor_);
        if (!root_private || !objects_private) {
            return fail(AssetStoreErrorCode::InvalidConfiguration,
                        !root_private ? root_private.error() : objects_private.error());
        }
        return {};
    }
    const auto root = openAbsoluteDirectoryNoFollow(root_directory_, true);
    if (!root) {
        return std::unexpected(root.error());
    }
    struct stat objects_status {};
    const auto objects_missing =
        ::fstatat(*root, "objects", &objects_status, AT_SYMLINK_NOFOLLOW) != 0;
    if (objects_missing) {
        if (errno != ENOENT) {
            const auto message = systemError(u"Inspect asset objects directory");
            static_cast<void>(::close(*root));
            return fail(AssetStoreErrorCode::InvalidConfiguration, message);
        }
        const auto safe_root = rootHasOnlyInitializationResidue(*root);
        if (!safe_root) {
            static_cast<void>(::close(*root));
            return std::unexpected(safe_root.error());
        }
        if (!*safe_root) {
            static_cast<void>(::close(*root));
            return fail(
                AssetStoreErrorCode::InvalidConfiguration,
                QStringLiteral(
                    "Existing asset-store root has unknown entries but no objects directory"));
        }
    } else if (!S_ISDIR(objects_status.st_mode)) {
        static_cast<void>(::close(*root));
        return fail(AssetStoreErrorCode::InvalidConfiguration,
                    QStringLiteral("Asset objects path is not a no-follow directory"));
    }
    const auto objects_path = QDir(root_directory_).filePath(QStringLiteral("objects"));
    const auto objects = detail::ensurePrivateStateDirectory(objects_path, root_directory_);
    if (!objects) {
        const auto message = objects.error();
        static_cast<void>(::close(*root));
        return fail(AssetStoreErrorCode::InvalidConfiguration, message);
    }
    struct stat anchored_objects{};
    struct stat named_objects{};
    if (::fstat(*objects, &anchored_objects) != 0 ||
        ::fstatat(*root, "objects", &named_objects, AT_SYMLINK_NOFOLLOW) != 0 ||
        anchored_objects.st_dev != named_objects.st_dev ||
        anchored_objects.st_ino != named_objects.st_ino) {
        static_cast<void>(::close(*objects));
        static_cast<void>(::close(*root));
        return fail(AssetStoreErrorCode::InvalidConfiguration,
                    QStringLiteral("Asset objects directory binding changed"));
    }
    root_descriptor_ = *root;
    objects_descriptor_ = *objects;
    return {};
#else
    return fail(AssetStoreErrorCode::InvalidConfiguration,
                QStringLiteral("Descriptor-relative asset storage is unavailable"));
#endif
}

QString AssetStore::objectsDirectory() const {
    return QDir(root_directory_).filePath(QStringLiteral("objects"));
}

qint64 AssetStore::maxAssetBytes() const noexcept { return max_asset_bytes_; }

std::expected<StagedAsset, AssetStoreError> AssetStore::stage(QByteArrayView bytes) {
    if (bytes.size() > max_asset_bytes_) {
        return fail(
            AssetStoreErrorCode::AssetTooLarge,
            QStringLiteral("Asset exceeds the configured %1-byte limit").arg(max_asset_bytes_));
    }
    QByteArray owned(bytes.data(), bytes.size());
    QBuffer source(&owned);
    if (!source.open(QIODevice::ReadOnly)) {
        return fail(AssetStoreErrorCode::CannotRead,
                    QStringLiteral("Cannot open the in-memory asset"));
    }
    return stage(source);
}

std::expected<StagedAsset, AssetStoreError> AssetStore::stage(QIODevice& source) {
    if (const auto ready = ensureReady(); !ready) {
        return std::unexpected(ready.error());
    }
    if (!source.isOpen() || !source.isReadable()) {
        return fail(AssetStoreErrorCode::CannotRead,
                    QStringLiteral("The asset source is not open for reading"));
    }
#if defined(Q_OS_UNIX)
    int file_descriptor = -1;
#if defined(Q_OS_LINUX) && defined(O_TMPFILE)
    file_descriptor = ::openat(objects_descriptor_, ".", O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
#else
    errno = ENOTSUP;
#endif
    if (file_descriptor < 0) {
        return fail(AssetStoreErrorCode::CannotWrite,
                    systemError(u"Create unnamed staged asset (O_TMPFILE is required)"));
    }
    if (const auto private_file = detail::normalizeNewPrivateStateFile(file_descriptor, 0);
        !private_file) {
        static_cast<void>(::close(file_descriptor));
        return fail(AssetStoreErrorCode::CannotWrite, private_file.error());
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 total_size{};
    while (true) {
        const auto chunk = source.read(io_buffer_bytes);
        if (chunk.isEmpty()) {
            if (source.atEnd()) {
                break;
            }
            static_cast<void>(::close(file_descriptor));
            return fail(AssetStoreErrorCode::CannotRead,
                        QStringLiteral("Cannot read the complete asset source"));
        }
        const auto chunk_size = static_cast<qint64>(chunk.size());
        if (chunk_size > max_asset_bytes_ - total_size) {
            static_cast<void>(::close(file_descriptor));
            return fail(
                AssetStoreErrorCode::AssetTooLarge,
                QStringLiteral("Asset exceeds the configured %1-byte limit").arg(max_asset_bytes_));
        }
        qsizetype offset{};
        while (offset < chunk.size()) {
            ssize_t written{};
            do {
                written = ::write(file_descriptor, chunk.constData() + offset,
                                  static_cast<std::size_t>(chunk.size() - offset));
            } while (written < 0 && errno == EINTR);
            if (written <= 0) {
                static_cast<void>(::close(file_descriptor));
                return fail(AssetStoreErrorCode::CannotWrite,
                            systemError(u"Write staged asset"));
            }
            offset += written;
        }
        hash.addData(chunk);
        total_size += chunk_size;
    }
    if (!syncDescriptor(file_descriptor)) {
        static_cast<void>(::close(file_descriptor));
        return fail(AssetStoreErrorCode::CannotSync,
                    systemError(u"Durably flush staged asset"));
    }
    struct stat staged_status {};
    if (::fstat(file_descriptor, &staged_status) != 0 ||
        !S_ISREG(staged_status.st_mode) || staged_status.st_nlink != 0 ||
        staged_status.st_size != total_size) {
        static_cast<void>(::close(file_descriptor));
        return fail(AssetStoreErrorCode::CannotWrite,
                    QStringLiteral(
                        "Unnamed staged asset is not an exact single unpublished regular file"));
    }
    const auto objects_copy = duplicateDescriptor(objects_descriptor_);
    if (objects_copy < 0) {
        static_cast<void>(::close(file_descriptor));
        return fail(AssetStoreErrorCode::CannotWrite,
                    systemError(u"Retain staged asset directory"));
    }
    return StagedAsset(objects_copy, file_descriptor, QString::fromLatin1(hash.result().toHex()),
                       total_size);
#else
    Q_UNUSED(source);
    return fail(AssetStoreErrorCode::InvalidConfiguration,
                QStringLiteral("Descriptor-relative asset staging is unavailable"));
#endif
}

std::expected<AssetStoreLock, AssetStoreError> AssetStore::acquireLock() const {
    if (const auto ready = ensureReady(); !ready) {
        return std::unexpected(ready.error());
    }
#if defined(Q_OS_UNIX)
    auto flags = O_RDWR | O_NOFOLLOW | O_NONBLOCK;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    auto descriptor = ::openat(root_descriptor_, ".cas.lock", flags);
    if (descriptor < 0 && errno == ENOENT) {
#if defined(Q_OS_LINUX) && defined(O_TMPFILE) && defined(AT_EMPTY_PATH)
        auto temporary_flags = O_TMPFILE | O_RDWR | O_NONBLOCK;
#ifdef O_CLOEXEC
        temporary_flags |= O_CLOEXEC;
#endif
        descriptor = ::openat(root_descriptor_, ".", temporary_flags, 0600);
        if (descriptor < 0) {
            return fail(AssetStoreErrorCode::CannotCommit,
                        systemError(u"Create staged asset-store lock"));
        }
        if (const auto private_file = detail::normalizeNewPrivateStateFile(descriptor, 0);
            !private_file) {
            static_cast<void>(::close(descriptor));
            return fail(AssetStoreErrorCode::InvalidConfiguration, private_file.error());
        }
        if (!syncDescriptor(descriptor)) {
            const auto message = systemError(u"Flush staged asset-store lock");
            static_cast<void>(::close(descriptor));
            return fail(AssetStoreErrorCode::CannotSync, message);
        }
        int linked{};
        do {
            linked = ::linkat(descriptor, "", root_descriptor_, ".cas.lock", AT_EMPTY_PATH);
        } while (linked != 0 && errno == EINTR);
        if (linked == 0) {
            if (!syncDescriptor(root_descriptor_)) {
                const auto message = systemError(u"Flush published asset-store lock");
                static_cast<void>(::close(descriptor));
                return fail(AssetStoreErrorCode::CannotSync, message);
            }
        } else {
            const auto link_errno = errno;
            static_cast<void>(::close(descriptor));
            if (link_errno != EEXIST) {
                errno = link_errno;
                return fail(AssetStoreErrorCode::CannotCommit,
                            systemError(u"Publish asset-store lock"));
            }
            descriptor = ::openat(root_descriptor_, ".cas.lock", flags);
        }
#else
        return fail(AssetStoreErrorCode::InvalidConfiguration,
                    QStringLiteral("Atomic asset-store locks are unavailable"));
#endif
    }
    if (descriptor < 0) {
        return fail(AssetStoreErrorCode::CannotCommit,
                    systemError(u"Open no-follow asset-store lock"));
    }
    const auto private_file = detail::validatePrivateStateFileDescriptor(descriptor, 1);
    if (!private_file || !detail::validatePrivateStateFileBinding(descriptor, root_descriptor_,
                                                                  QByteArrayLiteral(".cas.lock"))) {
        static_cast<void>(::close(descriptor));
        return fail(AssetStoreErrorCode::InvalidConfiguration,
                    private_file ? QStringLiteral("Asset-store lock binding is unsafe")
                                 : private_file.error());
    }
    if (const auto locked = lockDescriptor(descriptor); !locked) {
        static_cast<void>(::close(descriptor));
        return std::unexpected(locked.error());
    }
    if (!sameFileIdentity(descriptor, root_descriptor_, QByteArray(".cas.lock"))) {
        unlockDescriptor(descriptor);
        static_cast<void>(::close(descriptor));
        return fail(AssetStoreErrorCode::CannotCommit,
                    QStringLiteral("Asset-store lock identity changed during acquisition"));
    }
    return AssetStoreLock(descriptor);
#else
    return fail(AssetStoreErrorCode::InvalidConfiguration,
                QStringLiteral("Asset-store locking is unavailable"));
#endif
}

std::expected<bool, AssetStoreError> AssetStore::hasPublishedLock() const {
    if (const auto ready = ensureReady(); !ready) {
        return std::unexpected(ready.error());
    }
#if defined(Q_OS_UNIX)
    auto flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const auto descriptor = ::openat(root_descriptor_, ".cas.lock", flags);
    if (descriptor < 0) {
        if (errno == ENOENT) {
            return false;
        }
        return fail(AssetStoreErrorCode::CannotRead,
                    systemError(u"Inspect asset-store publication lock"));
    }
    const auto private_file = detail::validatePrivateStateFileBinding(
        descriptor, root_descriptor_, QByteArrayLiteral(".cas.lock"));
    static_cast<void>(::close(descriptor));
    if (!private_file) {
        return fail(AssetStoreErrorCode::InvalidConfiguration, private_file.error());
    }
    return true;
#else
    return fail(AssetStoreErrorCode::InvalidConfiguration,
                QStringLiteral("Asset-store locking is unavailable"));
#endif
}

std::expected<StoredAsset, AssetStoreError>
AssetStore::finalize(StagedAsset& staged, const AssetStoreLock& lock) const {
    if (const auto ready = ensureReady(); !ready) {
        return std::unexpected(ready.error());
    }
#if defined(Q_OS_UNIX)
    if (lock.descriptor_ < 0 || staged.file_descriptor_ < 0 || staged.finalized_ ||
        !isLowercaseSha256(staged.sha256_) ||
        !sameFileIdentity(lock.descriptor_, root_descriptor_, QByteArray(".cas.lock"))) {
        return fail(AssetStoreErrorCode::InvalidConfiguration,
                    QStringLiteral("Staged asset and publication lock are invalid"));
    }
    struct stat store_objects {};
    struct stat staged_objects {};
    if (::fstat(objects_descriptor_, &store_objects) != 0 ||
        ::fstat(staged.objects_descriptor_, &staged_objects) != 0 ||
        store_objects.st_dev != staged_objects.st_dev ||
        store_objects.st_ino != staged_objects.st_ino) {
        return fail(AssetStoreErrorCode::InvalidConfiguration,
                    QStringLiteral("Staged asset belongs to another object directory"));
    }
    struct stat staged_file {};
    if (::fstat(staged.file_descriptor_, &staged_file) != 0 ||
        !S_ISREG(staged_file.st_mode) || staged_file.st_nlink != 0 ||
        staged_file.st_size != staged.size_) {
        return fail(AssetStoreErrorCode::InvalidConfiguration,
                    QStringLiteral("Staged asset changed before publication"));
    }
    const auto digest_name = staged.sha256_.toLatin1();
    int linked = -1;
#if defined(Q_OS_LINUX) && defined(AT_EMPTY_PATH)
    linked = ::linkat(staged.file_descriptor_, "", objects_descriptor_, digest_name.constData(),
                      AT_EMPTY_PATH);
#else
    errno = ENOTSUP;
#endif
    if (linked != 0) {
        if (errno != EEXIST) {
            return fail(AssetStoreErrorCode::CannotCommit,
                        systemError(u"Publish immutable asset"));
        }
        const auto existing = read(staged.sha256_);
        if (!existing || existing->size() != staged.size_) {
            return existing ? fail(AssetStoreErrorCode::CorruptObject,
                                   QStringLiteral("Existing asset has an unexpected size"))
                            : std::unexpected(existing.error());
        }
        staged.finalized_ = true;
        staged.newly_finalized_ = false;
        return StoredAsset{staged.sha256_, staged.size_, true};
    }
    if (!sameFileIdentity(staged.file_descriptor_, objects_descriptor_, digest_name)) {
        bool detached{};
        const auto quarantined = quarantineBoundName(objects_descriptor_, digest_name,
                                                     staged.file_descriptor_, detached);
        return fail(AssetStoreErrorCode::CannotCommit,
                    quarantined ? QStringLiteral("Published asset identity changed unexpectedly")
                                : QStringLiteral("Published asset identity changed unexpectedly; "
                                                 "commit-forward quarantine failed: %1")
                                      .arg(quarantined.error().message));
    }
    if (!syncDescriptor(objects_descriptor_)) {
        const auto sync_error = systemError(u"Durably flush asset objects directory");
        bool detached{};
        const auto quarantined = quarantineBoundName(objects_descriptor_, digest_name,
                                                     staged.file_descriptor_, detached);
        return fail(AssetStoreErrorCode::CannotSync,
                    quarantined ? sync_error
                                : QStringLiteral("%1; commit-forward quarantine failed: %2")
                                      .arg(sync_error, quarantined.error().message));
    }
    staged.finalized_ = true;
    staged.newly_finalized_ = true;
    return StoredAsset{staged.sha256_, staged.size_, false};
#else
    Q_UNUSED(staged);
    Q_UNUSED(lock);
    return fail(AssetStoreErrorCode::InvalidConfiguration,
                QStringLiteral("Descriptor-relative asset publication is unavailable"));
#endif
}

std::expected<void, AssetStoreError>
AssetStore::removeNewlyFinalized(StagedAsset& staged, const AssetStoreLock& lock) const {
#if defined(Q_OS_UNIX)
    if (!staged.newly_finalized_) {
        return {};
    }
    if (lock.descriptor_ < 0 ||
        !sameFileIdentity(lock.descriptor_, root_descriptor_, QByteArray(".cas.lock"))) {
        return fail(AssetStoreErrorCode::CannotCommit,
                    QStringLiteral("Asset-store rollback lock is invalid"));
    }
    const auto name = staged.sha256_.toLatin1();
    bool detached{};
    const auto quarantined =
        quarantineBoundName(objects_descriptor_, name, staged.file_descriptor_, detached);
    if (detached) {
        // Once the live digest name is detached, retries must never act on a later replacement.
        staged.newly_finalized_ = false;
    }
    if (!quarantined) {
        return std::unexpected(quarantined.error());
    }
    return {};
#else
    Q_UNUSED(staged);
    Q_UNUSED(lock);
    return fail(AssetStoreErrorCode::InvalidConfiguration,
                QStringLiteral("Descriptor-relative asset rollback is unavailable"));
#endif
}

std::expected<StoredAsset, AssetStoreError> AssetStore::put(QByteArrayView bytes) {
    auto staged = stage(bytes);
    if (!staged) {
        return std::unexpected(staged.error());
    }
    auto lock = acquireLock();
    if (!lock) {
        return std::unexpected(lock.error());
    }
    return finalize(*staged, *lock);
}

std::expected<StoredAsset, AssetStoreError> AssetStore::put(QIODevice& source) {
    auto staged = stage(source);
    if (!staged) {
        return std::unexpected(staged.error());
    }
    auto lock = acquireLock();
    if (!lock) {
        return std::unexpected(lock.error());
    }
    return finalize(*staged, *lock);
}

std::expected<QByteArray, AssetStoreError> AssetStore::read(QStringView sha256) const {
    if (!isLowercaseSha256(sha256)) {
        return fail(AssetStoreErrorCode::InvalidDigest,
                    QStringLiteral("Asset IDs must be lowercase SHA-256 digests"));
    }
    if (const auto ready = ensureReady(); !ready) {
        return std::unexpected(ready.error());
    }
#if defined(Q_OS_UNIX)
    const auto digest = sha256.toString();
    const auto name = digest.toLatin1();
    auto flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const auto descriptor = ::openat(objects_descriptor_, name.constData(), flags);
    if (descriptor < 0) {
        if (errno == ENOENT) {
            return fail(AssetStoreErrorCode::NotFound,
                        QStringLiteral("Asset %1 does not exist").arg(digest));
        }
        return fail(errno == ELOOP ? AssetStoreErrorCode::CorruptObject
                                  : AssetStoreErrorCode::CannotRead,
                    systemError(u"Open no-follow asset object"));
    }
    if (const auto private_file =
            detail::validatePrivateStateFileBinding(descriptor, objects_descriptor_, name);
        !private_file) {
        static_cast<void>(::close(descriptor));
        return fail(AssetStoreErrorCode::CorruptObject,
                    QStringLiteral("Asset %1 permissions are unsafe: %2")
                        .arg(digest, private_file.error()));
    }
    struct stat status{};
    if (::fstat(descriptor, &status) != 0) {
        static_cast<void>(::close(descriptor));
        return fail(AssetStoreErrorCode::CannotRead, systemError(u"Inspect private asset object"));
    }
    if (status.st_size < 0 || status.st_size > max_asset_bytes_) {
        static_cast<void>(::close(descriptor));
        return fail(AssetStoreErrorCode::AssetTooLarge,
                    QStringLiteral("Asset %1 exceeds the configured size limit").arg(digest));
    }
    QByteArray contents;
    contents.reserve(static_cast<qsizetype>(status.st_size));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::array<char, io_buffer_bytes> buffer{};
    qint64 total{};
    while (true) {
        ssize_t count{};
        do {
            count = ::read(descriptor, buffer.data(), buffer.size());
        } while (count < 0 && errno == EINTR);
        if (count < 0) {
            static_cast<void>(::close(descriptor));
            return fail(AssetStoreErrorCode::CannotRead,
                        systemError(u"Read complete asset object"));
        }
        if (count == 0) {
            break;
        }
        if (count > max_asset_bytes_ - total) {
            static_cast<void>(::close(descriptor));
            return fail(AssetStoreErrorCode::AssetTooLarge,
                        QStringLiteral("Asset %1 exceeds the configured size limit").arg(digest));
        }
        hash.addData(QByteArrayView(buffer.data(), count));
        contents.append(buffer.data(), count);
        total += count;
    }
    struct stat final_status {};
    const auto stable = ::fstat(descriptor, &final_status) == 0 &&
                        sameStableFileState(status, final_status) &&
                        sameFileIdentity(descriptor, objects_descriptor_, name);
    static_cast<void>(::close(descriptor));
    if (!stable) {
        return fail(AssetStoreErrorCode::CorruptObject,
                    QStringLiteral("Asset %1 changed while it was read").arg(digest));
    }
    if (QString::fromLatin1(hash.result().toHex()) != digest) {
        return fail(AssetStoreErrorCode::CorruptObject,
                    QStringLiteral("Asset %1 failed digest verification").arg(digest));
    }
    return contents;
#else
    Q_UNUSED(sha256);
    return fail(AssetStoreErrorCode::InvalidConfiguration,
                QStringLiteral("Descriptor-relative asset reads are unavailable"));
#endif
}

std::expected<std::optional<QString>, AssetStoreError>
AssetStore::identity(const AssetStoreLock& lock) const {
    if (const auto ready = ensureReady(); !ready) {
        return std::unexpected(ready.error());
    }
#if defined(Q_OS_UNIX)
    if (lock.descriptor_ < 0 ||
        !sameFileIdentity(lock.descriptor_, root_descriptor_, QByteArray(".cas.lock"))) {
        return fail(AssetStoreErrorCode::CannotRead,
                    QStringLiteral("Asset-store identity lock is invalid"));
    }
    return identityUnlocked();
#else
    Q_UNUSED(lock);
    return fail(AssetStoreErrorCode::InvalidConfiguration,
                QStringLiteral("Asset-store identity is unavailable"));
#endif
}

std::expected<std::optional<QString>, AssetStoreError>
AssetStore::identityUnlocked() const {
    if (const auto ready = ensureReady(); !ready) {
        return std::unexpected(ready.error());
    }
#if defined(Q_OS_UNIX)
    auto flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const auto descriptor = ::openat(root_descriptor_, ".appellate-store-id", flags);
    if (descriptor < 0) {
        if (errno == ENOENT) {
            return std::optional<QString>{};
        }
        return fail(errno == ELOOP ? AssetStoreErrorCode::CorruptObject
                                  : AssetStoreErrorCode::CannotRead,
                    systemError(u"Open no-follow asset-store identity"));
    }
    if (const auto private_file = detail::validatePrivateStateFileBinding(
            descriptor, root_descriptor_, QByteArrayLiteral(".appellate-store-id"));
        !private_file) {
        static_cast<void>(::close(descriptor));
        return fail(AssetStoreErrorCode::CorruptObject, private_file.error());
    }
    struct stat before {};
    std::array<char, 34> bytes{};
    if (::fstat(descriptor, &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_nlink != 1 || before.st_size != 33) {
        static_cast<void>(::close(descriptor));
        return fail(AssetStoreErrorCode::CorruptObject,
                    QStringLiteral("Asset-store identity file is malformed"));
    }
    std::size_t count{};
    while (count < bytes.size()) {
        ssize_t read_count{};
        do {
            read_count = ::read(descriptor, bytes.data() + count,
                                bytes.size() - count);
        } while (read_count < 0 && errno == EINTR);
        if (read_count < 0) {
            const auto message = systemError(u"Read asset-store identity");
            static_cast<void>(::close(descriptor));
            return fail(AssetStoreErrorCode::CannotRead, message);
        }
        if (read_count == 0) {
            break;
        }
        count += static_cast<std::size_t>(read_count);
    }
    struct stat after {};
    const auto stat_succeeded = ::fstat(descriptor, &after) == 0 &&
                                sameFileIdentity(descriptor, root_descriptor_,
                                                 QByteArray(".appellate-store-id"));
    const auto saved_errno = errno;
    static_cast<void>(::close(descriptor));
    if (!stat_succeeded || !sameStableFileState(before, after) || count != 33) {
        errno = saved_errno;
        return fail(AssetStoreErrorCode::CorruptObject,
                    QStringLiteral("Asset-store identity file is malformed"));
    }
    const QByteArray framed(bytes.data(), static_cast<qsizetype>(count));
    const auto value = QString::fromLatin1(framed.first(32));
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{32}$"));
    if (framed.back() != '\n' || !pattern.match(value).hasMatch()) {
        return fail(AssetStoreErrorCode::CorruptObject,
                    QStringLiteral("Asset-store identity value is malformed"));
    }
    return std::optional<QString>{value};
#else
    return fail(AssetStoreErrorCode::InvalidConfiguration,
                QStringLiteral("Asset-store identity is unavailable"));
#endif
}

std::expected<void, AssetStoreError>
AssetStore::writeIdentity(QStringView identity_value, const AssetStoreLock& lock) const {
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{32}$"));
    if (!pattern.matchView(identity_value).hasMatch()) {
        return fail(AssetStoreErrorCode::InvalidConfiguration,
                    QStringLiteral("Asset-store identity must be 32 lowercase hex characters"));
    }
    const auto existing = identity(lock);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (existing->has_value()) {
        if (**existing != identity_value) {
            return fail(AssetStoreErrorCode::InvalidConfiguration,
                        QStringLiteral("Asset-store identity already differs"));
        }
#if defined(Q_OS_UNIX)
        return syncDescriptor(root_descriptor_)
                   ? std::expected<void, AssetStoreError>{}
                   : fail(AssetStoreErrorCode::CannotSync,
                          systemError(u"Retry asset-store identity directory flush"));
#else
        return {};
#endif
    }
#if defined(Q_OS_UNIX)
#if defined(Q_OS_LINUX) && defined(O_TMPFILE)
    const auto descriptor =
        ::openat(root_descriptor_, ".", O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
#else
    errno = ENOTSUP;
    const auto descriptor = -1;
#endif
    if (descriptor < 0) {
        return fail(AssetStoreErrorCode::CannotWrite,
                    systemError(u"Create unnamed asset-store identity (O_TMPFILE is required)"));
    }
    if (const auto private_file = detail::normalizeNewPrivateStateFile(descriptor, 0);
        !private_file) {
        static_cast<void>(::close(descriptor));
        return fail(AssetStoreErrorCode::CannotWrite, private_file.error());
    }
    const auto framed = identity_value.toLatin1() + QByteArray("\n");
    qsizetype offset{};
    while (offset < framed.size()) {
        ssize_t written{};
        do {
            written = ::write(descriptor, framed.constData() + offset,
                              static_cast<std::size_t>(framed.size() - offset));
        } while (written < 0 && errno == EINTR);
        if (written <= 0) {
            const auto message = systemError(u"Write asset-store identity");
            static_cast<void>(::close(descriptor));
            return fail(AssetStoreErrorCode::CannotWrite, message);
        }
        offset += written;
    }
    if (!syncDescriptor(descriptor)) {
        const auto message = systemError(u"Flush asset-store identity");
        static_cast<void>(::close(descriptor));
        return fail(AssetStoreErrorCode::CannotSync, message);
    }
    struct stat staged_status {};
    if (::fstat(descriptor, &staged_status) != 0 ||
        !S_ISREG(staged_status.st_mode) || staged_status.st_nlink != 0 ||
        staged_status.st_size != static_cast<off_t>(framed.size())) {
        static_cast<void>(::close(descriptor));
        return fail(AssetStoreErrorCode::CannotWrite,
                    QStringLiteral("Staged asset-store identity is malformed"));
    }
#if defined(Q_OS_LINUX) && defined(AT_EMPTY_PATH)
    const auto linked = ::linkat(descriptor, "", root_descriptor_,
                                 ".appellate-store-id", AT_EMPTY_PATH);
#else
    errno = ENOTSUP;
    const auto linked = -1;
#endif
    if (linked != 0) {
        const auto link_errno = errno;
        static_cast<void>(::close(descriptor));
        if (link_errno != EEXIST) {
            errno = link_errno;
            return fail(AssetStoreErrorCode::CannotWrite,
                        systemError(u"Publish asset-store identity"));
        }
        const auto raced_identity = identity(lock);
        if (!raced_identity || !raced_identity->has_value() ||
            **raced_identity != identity_value) {
            return raced_identity
                       ? fail(AssetStoreErrorCode::InvalidConfiguration,
                              QStringLiteral("Concurrent asset-store identity differs"))
                       : std::unexpected(raced_identity.error());
        }
        return syncDescriptor(root_descriptor_)
                   ? std::expected<void, AssetStoreError>{}
                   : fail(AssetStoreErrorCode::CannotSync,
                          systemError(u"Flush concurrently published asset-store identity"));
    }
    if (!sameFileIdentity(descriptor, root_descriptor_, QByteArray(".appellate-store-id"))) {
        static_cast<void>(::close(descriptor));
        return fail(AssetStoreErrorCode::CannotWrite,
                    QStringLiteral("Published asset-store identity changed unexpectedly"));
    }
    static_cast<void>(::close(descriptor));
    if (!syncDescriptor(root_descriptor_)) {
        return fail(AssetStoreErrorCode::CannotSync,
                    systemError(u"Flush asset-store identity directory"));
    }
    return {};
#else
    Q_UNUSED(lock);
    return fail(AssetStoreErrorCode::InvalidConfiguration,
                QStringLiteral("Asset-store identity writes are unavailable"));
#endif
}

std::expected<QStringList, AssetStoreError>
AssetStore::objectDigests(const AssetStoreLock& lock) const {
    if (const auto ready = ensureReady(); !ready) {
        return std::unexpected(ready.error());
    }
#if defined(Q_OS_UNIX)
    if (lock.descriptor_ < 0 ||
        !sameFileIdentity(lock.descriptor_, root_descriptor_, QByteArray(".cas.lock"))) {
        return fail(AssetStoreErrorCode::CannotRead,
                    QStringLiteral("Asset-store enumeration lock is invalid"));
    }
    return objectDigestsUnlocked();
#else
    Q_UNUSED(lock);
    return fail(AssetStoreErrorCode::InvalidConfiguration,
                QStringLiteral("Asset object enumeration is unavailable"));
#endif
}

std::expected<QStringList, AssetStoreError>
AssetStore::objectDigestsUnlocked() const {
    if (const auto ready = ensureReady(); !ready) {
        return std::unexpected(ready.error());
    }
#if defined(Q_OS_UNIX)
    const auto scan_descriptor = independentDirectoryDescriptor(objects_descriptor_);
    if (!scan_descriptor) {
        return std::unexpected(scan_descriptor.error());
    }
    auto* directory = ::fdopendir(*scan_descriptor);
    if (directory == nullptr) {
        static_cast<void>(::close(*scan_descriptor));
        return fail(AssetStoreErrorCode::CannotRead,
                    systemError(u"Enumerate asset objects"));
    }
    QStringList digests;
    errno = 0;
    while (const auto* entry = ::readdir(directory)) {
        const auto name = QByteArray(entry->d_name);
        if (name == "." || name == "..") {
            continue;
        }
        const auto text = QString::fromLatin1(name);
        const auto temporary = name.startsWith(".asset-") && name.endsWith(".tmp");
        const auto object_name = isLowercaseSha256(text);
        if (!temporary && !object_name) {
            static_cast<void>(::closedir(directory));
            return fail(AssetStoreErrorCode::CorruptObject,
                        QStringLiteral("Asset objects directory contains an unexpected entry"));
        }
        auto flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        const auto object_descriptor = ::openat(objects_descriptor_, name.constData(), flags);
        const auto private_file =
            object_descriptor >= 0
                ? detail::validatePrivateStateFileBinding(object_descriptor, objects_descriptor_,
                                                          name)
                : std::expected<void, QString>{std::unexpected(
                      systemError(u"Open private asset object during enumeration"))};
        if (object_descriptor >= 0) {
            static_cast<void>(::close(object_descriptor));
        }
        if (!private_file) {
            static_cast<void>(::closedir(directory));
            return fail(AssetStoreErrorCode::CorruptObject,
                        QStringLiteral("Asset object permissions are unsafe: %1")
                            .arg(private_file.error()));
        }
        if (object_name) {
            digests.push_back(text);
        }
        errno = 0;
    }
    const auto scan_errno = errno;
    static_cast<void>(::closedir(directory));
    if (scan_errno != 0) {
        errno = scan_errno;
        return fail(AssetStoreErrorCode::CannotRead,
                    systemError(u"Complete asset object enumeration"));
    }
    digests.sort();
    return digests;
#else
    return fail(AssetStoreErrorCode::InvalidConfiguration,
                QStringLiteral("Asset object enumeration is unavailable"));
#endif
}

std::expected<void, AssetStoreError>
AssetStore::preflightPair(QStringView database_identity,
                          const QStringList& referenced_digests,
                          bool require_exact_object_set) const {
    static const QRegularExpression identity_pattern(QStringLiteral("^[0-9a-f]{32}$"));
    if (!identity_pattern.matchView(database_identity).hasMatch()) {
        return fail(AssetStoreErrorCode::InvalidConfiguration,
                    QStringLiteral("Database asset-store identity is invalid"));
    }
    const auto marker = identityUnlocked();
    if (!marker) {
        return std::unexpected(marker.error());
    }
    if (marker->has_value() && **marker != database_identity) {
        return fail(AssetStoreErrorCode::InvalidConfiguration,
                    QStringLiteral("Database and asset-store identities do not match"));
    }
    const auto objects = objectDigestsUnlocked();
    if (!objects) {
        return std::unexpected(objects.error());
    }
    auto expected = referenced_digests;
    expected.removeDuplicates();
    expected.sort();
    if ((!marker->has_value() || require_exact_object_set) && *objects != expected) {
        return fail(AssetStoreErrorCode::CorruptObject,
                    QStringLiteral(
                        "Database asset references do not exactly match asset-store objects"));
    }
    for (const auto& digest : expected) {
        if (const auto contents = read(digest); !contents) {
            return fail(AssetStoreErrorCode::CorruptObject,
                        QStringLiteral("Referenced asset %1 is absent or corrupt: %2")
                            .arg(digest, contents.error().message));
        }
    }
    return {};
}

std::expected<void, AssetStoreError>
AssetStore::recoverPairedObjects(QStringView database_identity, const AssetStoreLock& lock,
                                 const QStringList& referenced_digests,
                                 const detail::AssetRecoveryHooks* hooks) const {
    if (const auto ready = ensureReady(); !ready) {
        return std::unexpected(ready.error());
    }
#if defined(Q_OS_UNIX)
    static const QRegularExpression identity_pattern(QStringLiteral("^[0-9a-f]{32}$"));
    if (!identity_pattern.matchView(database_identity).hasMatch()) {
        return fail(AssetStoreErrorCode::InvalidConfiguration,
                    QStringLiteral("Database asset-store identity is invalid"));
    }
    if (lock.descriptor_ < 0 ||
        !sameFileIdentity(lock.descriptor_, root_descriptor_, QByteArray(".cas.lock"))) {
        return fail(AssetStoreErrorCode::CannotCommit,
                    QStringLiteral("Asset-store recovery lock is invalid"));
    }
    const auto marker = identityUnlocked();
    if (!marker) {
        return std::unexpected(marker.error());
    }
    if (marker->has_value() && **marker != database_identity) {
        return fail(AssetStoreErrorCode::InvalidConfiguration,
                    QStringLiteral("Database and asset-store identities do not match"));
    }
    auto expected = referenced_digests;
    expected.removeDuplicates();
    expected.sort();
    const QSet<QString> referenced(expected.begin(), expected.end());
    const auto scan_descriptor = independentDirectoryDescriptor(objects_descriptor_);
    if (!scan_descriptor) {
        return std::unexpected(scan_descriptor.error());
    }
    auto* directory = ::fdopendir(*scan_descriptor);
    if (directory == nullptr) {
        static_cast<void>(::close(*scan_descriptor));
        return fail(AssetStoreErrorCode::CannotRead,
                    systemError(u"Scan asset directory for recovery"));
    }
    std::vector<RecoveryRemoval> removals;
    std::vector<RecoveryFinal> referenced_finals;
    QStringList object_digests;
    errno = 0;
    while (const auto* entry = ::readdir(directory)) {
        const auto name = QByteArray(entry->d_name);
        if (name == "." || name == "..") {
            continue;
        }
        const auto text = QString::fromLatin1(name);
        const auto temporary = name.startsWith(".asset-") && name.endsWith(".tmp");
        const auto quarantined = isRecoveryQuarantineName(name);
        const auto object_name = isLowercaseSha256(text);
        if (!temporary && !object_name) {
            static_cast<void>(::closedir(directory));
            return fail(AssetStoreErrorCode::CorruptObject,
                        QStringLiteral("Asset recovery found an unexpected object name"));
        }
        auto flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        const auto object_descriptor = ::openat(objects_descriptor_, name.constData(), flags);
        const auto private_file =
            object_descriptor >= 0
                ? detail::validatePrivateStateFileBinding(object_descriptor, objects_descriptor_,
                                                          name)
                : std::expected<void, QString>{
                      std::unexpected(systemError(u"Open private recovery object"))};
        struct stat object_status {};
        const auto inspected = object_descriptor >= 0 && private_file.has_value() &&
                               ::fstat(object_descriptor, &object_status) == 0;
        if (!inspected) {
            if (object_descriptor >= 0) {
                static_cast<void>(::close(object_descriptor));
            }
            static_cast<void>(::closedir(directory));
            return fail(AssetStoreErrorCode::CorruptObject,
                        QStringLiteral("Asset recovery found an unsafe object: %1")
                            .arg(private_file ? QStringLiteral("cannot inspect private object")
                                              : private_file.error()));
        }
        const auto orphan = object_name && !referenced.contains(text);
        if (object_name) {
            object_digests.push_back(text);
        }
        if (object_name && !orphan) {
            referenced_finals.emplace_back(name, text, object_status, object_descriptor);
        } else if ((temporary && !quarantined) || orphan) {
            removals.emplace_back(name, object_status, object_descriptor);
        } else {
            static_cast<void>(::close(object_descriptor));
        }
        errno = 0;
    }
    const auto scan_errno = errno;
    static_cast<void>(::closedir(directory));
    if (scan_errno != 0) {
        errno = scan_errno;
        return fail(AssetStoreErrorCode::CannotRead,
                    systemError(u"Complete asset recovery scan"));
    }

    object_digests.sort();
    if (!marker->has_value() && object_digests != expected) {
        return fail(AssetStoreErrorCode::CorruptObject,
                    QStringLiteral(
                        "Database asset references do not exactly match asset-store objects"));
    }

    // Hash and stability-verify every final, including objects that appear to be orphans, before
    // publishing a marker or moving anything. A corrupt later orphan can therefore never cause
    // partial cleanup of earlier evidence.
    for (const auto& final : referenced_finals) {
        if (const auto verified = verifyRetainedFinal(final, objects_descriptor_, max_asset_bytes_);
            !verified) {
            return std::unexpected(verified.error());
        }
    }
    for (const auto& digest : object_digests) {
        if (referenced.contains(digest)) {
            continue;
        }
        if (const auto object = read(digest); !object) {
            return fail(AssetStoreErrorCode::CorruptObject,
                        QStringLiteral("Asset object %1 is corrupt: %2")
                            .arg(digest, object.error().message));
        }
    }
    const QSet<QString> available_objects(object_digests.begin(), object_digests.end());
    for (const auto& digest : expected) {
        if (!available_objects.contains(digest)) {
            return fail(AssetStoreErrorCode::CorruptObject,
                        QStringLiteral("Referenced asset %1 is absent").arg(digest));
        }
    }
    for (qsizetype index = 0; index < static_cast<qsizetype>(removals.size()); ++index) {
        auto& removal = removals[static_cast<std::size_t>(index)];
        struct stat retained_status{};
        if (::fstat(removal.descriptor, &retained_status) != 0 ||
            !sameStableFileState(removal.original_status, retained_status) ||
            !sameFileIdentity(removal.descriptor, objects_descriptor_, removal.original_name)) {
            return fail(AssetStoreErrorCode::CorruptObject,
                        QStringLiteral("Asset recovery candidate changed before quarantine"));
        }
        const auto original_text = QString::fromLatin1(removal.original_name);
        try {
            if (hooks != nullptr && hooks->before_quarantine) {
                hooks->before_quarantine(index, original_text);
            }
            if (hooks != nullptr && hooks->permit_quarantine &&
                !hooks->permit_quarantine(index, original_text)) {
                return fail(AssetStoreErrorCode::CannotCommit,
                            QStringLiteral("Asset recovery quarantine was interrupted"));
            }
        } catch (const std::exception& exception) {
            return fail(AssetStoreErrorCode::CannotCommit,
                        QStringLiteral("Asset recovery hook failed: %1")
                            .arg(QString::fromLocal8Bit(exception.what())));
        } catch (...) {
            return fail(AssetStoreErrorCode::CannotCommit,
                        QStringLiteral("Asset recovery hook failed"));
        }

        bool detached{};
        const auto quarantined = quarantineBoundName(objects_descriptor_, removal.original_name,
                                                     removal.descriptor, detached);
        if (!quarantined) {
            return std::unexpected(quarantined.error());
        }
    }

    // Hooks and earlier candidate renames can race unrelated live digest names. Keep the original
    // referenced descriptors alive and re-hash their bound inodes after all test barriers and
    // namespace mutations, immediately before an identity marker could make the pair authoritative.
    for (const auto& final : referenced_finals) {
        if (const auto verified = verifyRetainedFinal(final, objects_descriptor_, max_asset_bytes_);
            !verified) {
            return std::unexpected(verified.error());
        }
    }
    if (!marker->has_value()) {
        if (const auto written = writeIdentity(database_identity, lock); !written) {
            return written;
        }
    }
    // Physical unlink is deliberately outside the recovery commit path. Linux cannot unlink by
    // retained descriptor or atomically remove several names; reserved quarantine tombstones keep
    // proven garbage non-addressable without risking deletion of a raced replacement.
    return {};
#else
    Q_UNUSED(lock);
    Q_UNUSED(database_identity);
    Q_UNUSED(referenced_digests);
    Q_UNUSED(hooks);
    return fail(AssetStoreErrorCode::InvalidConfiguration,
                QStringLiteral("Descriptor-relative asset recovery is unavailable"));
#endif
}

} // namespace appellate::storage
