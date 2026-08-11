#include "appellate/storage/asset_store.hpp"

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
#include <utility>
#include <vector>

#if defined(Q_OS_UNIX)
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
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
    return descriptor;
}

[[nodiscard]] auto directoryIsEmpty(int anchored_descriptor)
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
        if (name != "." && name != "..") {
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
    if (!QDir::isAbsolutePath(absolute_path) || absolute_path.contains(QChar::Null) ||
        QDir::cleanPath(absolute_path) != absolute_path) {
        return fail(AssetStoreErrorCode::InvalidConfiguration,
                    QStringLiteral("The asset-store root must be an absolute clean path"));
    }
    auto flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    auto descriptor = ::open("/", flags);
    if (descriptor < 0) {
        return fail(AssetStoreErrorCode::CannotCreateDirectory,
                    systemError(u"Anchor asset filesystem root"));
    }
    const auto components = absolute_path.split(u'/', Qt::SkipEmptyParts);
    for (const auto& component : components) {
        if (component == QStringLiteral(".") || component == QStringLiteral("..") ||
            component.contains(QChar::Null)) {
            static_cast<void>(::close(descriptor));
            return fail(AssetStoreErrorCode::InvalidConfiguration,
                        QStringLiteral("The asset-store root has an unsafe component"));
        }
        const auto encoded = QFile::encodeName(component);
        auto child = ::openat(descriptor, encoded.constData(), flags);
        if (child < 0 && errno == ENOENT && create) {
            if (::mkdirat(descriptor, encoded.constData(), 0700) != 0 && errno != EEXIST) {
                const auto message = systemError(u"Create asset-store path component");
                static_cast<void>(::close(descriptor));
                return fail(AssetStoreErrorCode::CannotCreateDirectory, message);
            }
            child = ::openat(descriptor, encoded.constData(), flags);
        }
        const auto saved_errno = errno;
        static_cast<void>(::close(descriptor));
        if (child < 0) {
            errno = saved_errno;
            return fail(AssetStoreErrorCode::InvalidConfiguration,
                        systemError(u"Open no-follow asset-store path component"));
        }
        descriptor = child;
    }
    return descriptor;
}

[[nodiscard]] bool sameFileIdentity(int descriptor, int parent_descriptor,
                                    const QByteArray& name) {
    struct stat descriptor_status {};
    struct stat name_status {};
    return ::fstat(descriptor, &descriptor_status) == 0 &&
           ::fstatat(parent_descriptor, name.constData(), &name_status,
                     AT_SYMLINK_NOFOLLOW) == 0 &&
           descriptor_status.st_dev == name_status.st_dev &&
           descriptor_status.st_ino == name_status.st_ino &&
           descriptor_status.st_nlink == 1 && name_status.st_nlink == 1 &&
           S_ISREG(descriptor_status.st_mode) && S_ISREG(name_status.st_mode);
}

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

StagedAsset::StagedAsset(int objects_descriptor, int file_descriptor,
                         QByteArray temporary_name, QString sha256, qint64 size)
    : objects_descriptor_(objects_descriptor), file_descriptor_(file_descriptor),
      temporary_name_(std::move(temporary_name)), sha256_(std::move(sha256)), size_(size) {}

StagedAsset::StagedAsset(StagedAsset&& other) noexcept
    : objects_descriptor_(std::exchange(other.objects_descriptor_, -1)),
      file_descriptor_(std::exchange(other.file_descriptor_, -1)),
      temporary_name_(std::move(other.temporary_name_)), sha256_(std::move(other.sha256_)),
      size_(other.size_), finalized_(other.finalized_),
      newly_finalized_(other.newly_finalized_) {
    other.temporary_name_.clear();
    other.finalized_ = false;
    other.newly_finalized_ = false;
}

StagedAsset& StagedAsset::operator=(StagedAsset&& other) noexcept {
    if (this != &other) {
        reset();
        objects_descriptor_ = std::exchange(other.objects_descriptor_, -1);
        file_descriptor_ = std::exchange(other.file_descriptor_, -1);
        temporary_name_ = std::move(other.temporary_name_);
        sha256_ = std::move(other.sha256_);
        size_ = other.size_;
        finalized_ = other.finalized_;
        newly_finalized_ = other.newly_finalized_;
        other.temporary_name_.clear();
        other.finalized_ = false;
        other.newly_finalized_ = false;
    }
    return *this;
}

StagedAsset::~StagedAsset() { reset(); }

void StagedAsset::reset() noexcept {
#if defined(Q_OS_UNIX)
    if (objects_descriptor_ >= 0 && !temporary_name_.isEmpty()) {
        static_cast<void>(::unlinkat(objects_descriptor_, temporary_name_.constData(), 0));
    }
    if (file_descriptor_ >= 0) {
        static_cast<void>(::close(file_descriptor_));
    }
    if (objects_descriptor_ >= 0) {
        static_cast<void>(::close(objects_descriptor_));
    }
#endif
    objects_descriptor_ = -1;
    file_descriptor_ = -1;
    temporary_name_.clear();
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
        return {};
    }
    const auto root = openAbsoluteDirectoryNoFollow(root_directory_, true);
    if (!root) {
        return std::unexpected(root.error());
    }
    struct stat objects_status {};
    if (::fstatat(*root, "objects", &objects_status, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno != ENOENT) {
            const auto message = systemError(u"Inspect asset objects directory");
            static_cast<void>(::close(*root));
            return fail(AssetStoreErrorCode::InvalidConfiguration, message);
        }
        const auto empty_root = directoryIsEmpty(*root);
        if (!empty_root) {
            static_cast<void>(::close(*root));
            return std::unexpected(empty_root.error());
        }
        if (!*empty_root) {
            static_cast<void>(::close(*root));
            return fail(AssetStoreErrorCode::InvalidConfiguration,
                        QStringLiteral(
                            "Existing nonempty asset-store root has no objects directory"));
        }
        if (::mkdirat(*root, "objects", 0700) != 0) {
            const auto message = systemError(u"Create asset objects directory");
            static_cast<void>(::close(*root));
            return fail(AssetStoreErrorCode::CannotCreateDirectory, message);
        }
    } else if (!S_ISDIR(objects_status.st_mode)) {
        static_cast<void>(::close(*root));
        return fail(AssetStoreErrorCode::InvalidConfiguration,
                    QStringLiteral("Asset objects path is not a no-follow directory"));
    }
    auto flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const auto objects = ::openat(*root, "objects", flags);
    if (objects < 0) {
        const auto message = systemError(u"Open no-follow asset objects directory");
        static_cast<void>(::close(*root));
        return fail(AssetStoreErrorCode::InvalidConfiguration, message);
    }
    root_descriptor_ = *root;
    objects_descriptor_ = objects;
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
    QByteArray temporary_name;
#if defined(Q_OS_LINUX) && defined(O_TMPFILE)
    file_descriptor = ::openat(objects_descriptor_, ".", O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
#else
    errno = ENOTSUP;
#endif
    if (file_descriptor < 0) {
        return fail(AssetStoreErrorCode::CannotWrite,
                    systemError(u"Create unnamed staged asset (O_TMPFILE is required)"));
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 total_size{};
    while (true) {
        const auto chunk = source.read(io_buffer_bytes);
        if (chunk.isEmpty()) {
            if (source.atEnd()) {
                break;
            }
            if (!temporary_name.isEmpty()) {
                static_cast<void>(::unlinkat(objects_descriptor_, temporary_name.constData(), 0));
            }
            static_cast<void>(::close(file_descriptor));
            return fail(AssetStoreErrorCode::CannotRead,
                        QStringLiteral("Cannot read the complete asset source"));
        }
        const auto chunk_size = static_cast<qint64>(chunk.size());
        if (chunk_size > max_asset_bytes_ - total_size) {
            if (!temporary_name.isEmpty()) {
                static_cast<void>(::unlinkat(objects_descriptor_, temporary_name.constData(), 0));
            }
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
                if (!temporary_name.isEmpty()) {
                    static_cast<void>(
                        ::unlinkat(objects_descriptor_, temporary_name.constData(), 0));
                }
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
        if (!temporary_name.isEmpty()) {
            static_cast<void>(::unlinkat(objects_descriptor_, temporary_name.constData(), 0));
        }
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
        if (!temporary_name.isEmpty()) {
            static_cast<void>(::unlinkat(objects_descriptor_, temporary_name.constData(), 0));
        }
        static_cast<void>(::close(file_descriptor));
        return fail(AssetStoreErrorCode::CannotWrite,
                    systemError(u"Retain staged asset directory"));
    }
    return StagedAsset(objects_copy, file_descriptor, std::move(temporary_name),
                       QString::fromLatin1(hash.result().toHex()), total_size);
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
    auto flags = O_RDWR | O_NOFOLLOW;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    auto descriptor = ::openat(root_descriptor_, ".cas.lock", flags);
    if (descriptor < 0 && errno == ENOENT) {
        descriptor = ::openat(root_descriptor_, ".cas.lock", flags | O_CREAT | O_EXCL, 0600);
        if (descriptor < 0 && errno == EEXIST) {
            descriptor = ::openat(root_descriptor_, ".cas.lock", flags);
        }
    }
    if (descriptor < 0) {
        return fail(AssetStoreErrorCode::CannotCommit,
                    systemError(u"Open no-follow asset-store lock"));
    }
    struct stat status {};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_nlink != 1) {
        static_cast<void>(::close(descriptor));
        return fail(AssetStoreErrorCode::InvalidConfiguration,
                    QStringLiteral("Asset-store lock is not a regular file"));
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
    struct stat status {};
    if (::fstatat(root_descriptor_, ".cas.lock", &status, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            return false;
        }
        return fail(AssetStoreErrorCode::CannotRead,
                    systemError(u"Inspect asset-store publication lock"));
    }
    if (!S_ISREG(status.st_mode) || status.st_nlink != 1) {
        return fail(AssetStoreErrorCode::InvalidConfiguration,
                    QStringLiteral("Asset-store lock is not a single-link regular file"));
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
    if (staged.temporary_name_.isEmpty()) {
#if defined(Q_OS_LINUX) && defined(AT_EMPTY_PATH)
        linked = ::linkat(staged.file_descriptor_, "", objects_descriptor_,
                          digest_name.constData(), AT_EMPTY_PATH);
#else
        errno = ENOTSUP;
#endif
    } else {
        linked = ::linkat(objects_descriptor_, staged.temporary_name_.constData(),
                          objects_descriptor_, digest_name.constData(), 0);
    }
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
        if (!staged.temporary_name_.isEmpty()) {
            static_cast<void>(
                ::unlinkat(objects_descriptor_, staged.temporary_name_.constData(), 0));
            staged.temporary_name_.clear();
        }
        staged.finalized_ = true;
        staged.newly_finalized_ = false;
        return StoredAsset{staged.sha256_, staged.size_, true};
    }
    if (!sameFileIdentity(staged.file_descriptor_, objects_descriptor_, digest_name)) {
        static_cast<void>(::unlinkat(objects_descriptor_, digest_name.constData(), 0));
        return fail(AssetStoreErrorCode::CannotCommit,
                    QStringLiteral("Published asset identity changed unexpectedly"));
    }
    if (!staged.temporary_name_.isEmpty()) {
        static_cast<void>(::unlinkat(objects_descriptor_, staged.temporary_name_.constData(), 0));
        staged.temporary_name_.clear();
    }
    if (!syncDescriptor(objects_descriptor_)) {
        static_cast<void>(::unlinkat(objects_descriptor_, digest_name.constData(), 0));
        return fail(AssetStoreErrorCode::CannotSync,
                    systemError(u"Durably flush asset objects directory"));
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
    if (!sameFileIdentity(staged.file_descriptor_, objects_descriptor_, name)) {
        return fail(AssetStoreErrorCode::CannotCommit,
                    QStringLiteral("New asset identity changed before rollback cleanup"));
    }
    if (::unlinkat(objects_descriptor_, name.constData(), 0) != 0) {
        return fail(AssetStoreErrorCode::CannotCommit,
                    systemError(u"Remove rolled-back asset"));
    }
    staged.newly_finalized_ = false;
    return syncDescriptor(objects_descriptor_)
               ? std::expected<void, AssetStoreError>{}
               : fail(AssetStoreErrorCode::CannotSync,
                      systemError(u"Flush rolled-back asset removal"));
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
    auto flags = O_RDONLY | O_NOFOLLOW;
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
    struct stat status {};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_nlink != 1) {
        static_cast<void>(::close(descriptor));
        return fail(AssetStoreErrorCode::CorruptObject,
                    QStringLiteral("Asset %1 is not a regular object file").arg(digest));
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
    auto flags = O_RDONLY | O_NOFOLLOW;
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
        struct stat object_status {};
        if (::fstatat(objects_descriptor_, name.constData(), &object_status,
                      AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(object_status.st_mode) || object_status.st_nlink != 1) {
            static_cast<void>(::closedir(directory));
            return fail(AssetStoreErrorCode::CorruptObject,
                        QStringLiteral(
                            "Asset objects directory contains a linked or nonregular object"));
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
AssetStore::recoverPairedObjects(QStringView database_identity,
                                 const AssetStoreLock& lock,
                                 const QStringList& referenced_digests) const {
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
    std::vector<std::pair<QByteArray, struct stat>> removals;
    QStringList object_digests;
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
                        QStringLiteral("Asset recovery found an unexpected object name"));
        }
        struct stat object_status {};
        if (::fstatat(objects_descriptor_, name.constData(), &object_status,
                      AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(object_status.st_mode) || object_status.st_nlink != 1) {
            static_cast<void>(::closedir(directory));
            return fail(AssetStoreErrorCode::CorruptObject,
                        QStringLiteral(
                            "Asset recovery found a linked or nonregular object"));
        }
        const auto orphan = object_name && !referenced.contains(text);
        if (object_name) {
            object_digests.push_back(text);
        }
        if (temporary || orphan) {
            removals.emplace_back(name, object_status);
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
    // publishing a marker or unlinking anything. A corrupt later orphan can therefore never
    // cause partial cleanup of earlier evidence.
    for (const auto& digest : object_digests) {
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
    if (!marker->has_value()) {
        if (const auto written = writeIdentity(database_identity, lock); !written) {
            return written;
        }
    }
    for (const auto& [name, original_status] : removals) {
        struct stat current_status {};
        if (::fstatat(objects_descriptor_, name.constData(), &current_status,
                      AT_SYMLINK_NOFOLLOW) != 0 ||
            current_status.st_nlink != 1 ||
            !sameStableFileState(original_status, current_status)) {
            return fail(AssetStoreErrorCode::CorruptObject,
                        QStringLiteral("Asset recovery candidate changed before cleanup"));
        }
    }
    for (const auto& [name, original_status] : removals) {
        Q_UNUSED(original_status);
        if (::unlinkat(objects_descriptor_, name.constData(), 0) != 0) {
            return fail(AssetStoreErrorCode::CannotCommit,
                        systemError(u"Remove unreferenced asset object"));
        }
    }
    if (!removals.empty() && !syncDescriptor(objects_descriptor_)) {
        return fail(AssetStoreErrorCode::CannotSync,
                    systemError(u"Flush recovered asset directory"));
    }
    return {};
#else
    Q_UNUSED(lock);
    Q_UNUSED(database_identity);
    Q_UNUSED(referenced_digests);
    return fail(AssetStoreErrorCode::InvalidConfiguration,
                QStringLiteral("Descriptor-relative asset recovery is unavailable"));
#endif
}

} // namespace appellate::storage
