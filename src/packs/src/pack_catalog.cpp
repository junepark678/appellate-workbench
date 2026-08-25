#include "appellate/packs/pack_catalog.hpp"
#include "appellate/packs/capability_registry.hpp"
#include "pack_archive_p.hpp"
#include "pack_catalog_fingerprint_p.hpp"
#include "pack_catalog_lock_p.hpp"
#include "pack_catalog_migrations_p.hpp"
#include "pack_catalog_p.hpp"
#include "pack_catalog_reverse_admission_p.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QLockFile>
#include <QRandomGenerator>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTemporaryFile>
#include <QUuid>
#include <QVariant>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(Q_OS_LINUX)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>
#elif defined(Q_OS_UNIX)
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
constexpr std::size_t maximum_catalog_inventory_entries = 20'000;
constexpr std::uint64_t maximum_catalog_database_bytes = 512ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t maximum_catalog_wal_bytes = 512ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t maximum_catalog_capture_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t maximum_catalog_archive_bytes =
    PackArchiveLimits{}.maximum_total_bytes + 64ULL * 1024ULL * 1024ULL + 16ULL * 1024ULL * 1024ULL;

[[nodiscard]] auto fail(CatalogErrorCode code, QString message) -> std::unexpected<CatalogError> {
    return std::unexpected(CatalogError{code, std::move(message)});
}

#if defined(Q_OS_LINUX)
class CatalogDescriptor final {
  public:
    explicit CatalogDescriptor(int value = -1) noexcept : value_(value) {}
    CatalogDescriptor(const CatalogDescriptor&) = delete;
    CatalogDescriptor& operator=(const CatalogDescriptor&) = delete;
    CatalogDescriptor(CatalogDescriptor&& other) noexcept
        : value_(std::exchange(other.value_, -1)) {}
    CatalogDescriptor& operator=(CatalogDescriptor&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.value_, -1));
        }
        return *this;
    }
    ~CatalogDescriptor() { reset(); }

    [[nodiscard]] int get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return value_ >= 0; }

    void reset(int value = -1) noexcept {
        if (value_ >= 0) {
            static_cast<void>(::close(value_));
        }
        value_ = value;
    }

  private:
    int value_;
};

struct CatalogIdentity final {
    dev_t device{};
    ino_t inode{};
    mode_t mode{};
    nlink_t links{};
    uid_t owner{};
    off_t size{};
    timespec modified{};
    timespec changed{};
};

using NativeCatalogIdentity = CatalogIdentity;

[[nodiscard]] CatalogIdentity catalogIdentity(const struct stat& status) {
    return CatalogIdentity{status.st_dev, status.st_ino,  status.st_mode, status.st_nlink,
                           status.st_uid, status.st_size, status.st_mtim, status.st_ctim};
}

[[nodiscard]] bool sameCatalogIdentity(const CatalogIdentity& expected, const struct stat& actual,
                                       bool include_size_and_times = true) {
    const auto links_match = S_ISDIR(expected.mode) || expected.links == actual.st_nlink;
    const auto base = expected.device == actual.st_dev && expected.inode == actual.st_ino &&
                      expected.mode == actual.st_mode && links_match &&
                      expected.owner == actual.st_uid;
    if (!base || !include_size_and_times) {
        return base;
    }
    return expected.size == actual.st_size && expected.modified.tv_sec == actual.st_mtim.tv_sec &&
           expected.modified.tv_nsec == actual.st_mtim.tv_nsec &&
           expected.changed.tv_sec == actual.st_ctim.tv_sec &&
           expected.changed.tv_nsec == actual.st_ctim.tv_nsec;
}

[[nodiscard]] bool catalogAclAbsent(int descriptor, const char* attribute) {
    while (true) {
        errno = 0;
        const auto result = ::fgetxattr(descriptor, attribute, nullptr, 0);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result >= 0) {
            return false;
        }
#if defined(ENOATTR) && ENOATTR != ENODATA
        return errno == ENODATA || errno == ENOATTR;
#else
        return errno == ENODATA;
#endif
    }
}

[[nodiscard]] bool catalogDirectoryAclsAbsent(int descriptor) {
    return catalogAclAbsent(descriptor, "system.posix_acl_access") &&
           catalogAclAbsent(descriptor, "system.posix_acl_default");
}

[[nodiscard]] bool catalogFileAclAbsent(int descriptor) {
    return catalogAclAbsent(descriptor, "system.posix_acl_access");
}

[[nodiscard]] bool catalogControllerPolicy(const struct stat& status) {
    return S_ISDIR(status.st_mode) && (status.st_uid == ::geteuid() || status.st_uid == 0) &&
           (((status.st_mode & 0022) == 0) || (status.st_mode & S_ISVTX) != 0);
}

[[nodiscard]] bool catalogNamespaceDirectoryPolicy(const struct stat& status, bool writable) {
    if (!S_ISDIR(status.st_mode) || (status.st_uid != ::geteuid() && status.st_uid != 0) ||
        (status.st_mode & 0022) != 0) {
        return false;
    }
    if (writable) {
        return status.st_uid == ::geteuid() && (status.st_mode & 0700) == 0700;
    }
    if (status.st_uid == ::geteuid()) {
        return (status.st_mode & 0500) == 0500;
    }
    return ::geteuid() == 0 || (status.st_mode & 0005) == 0005;
}

[[nodiscard]] bool catalogNamespaceFilePolicy(const struct stat& status, bool writable) {
    if (!S_ISREG(status.st_mode) || status.st_nlink != 1 ||
        (status.st_uid != ::geteuid() && status.st_uid != 0) || (status.st_mode & 0022) != 0) {
        return false;
    }
    if (writable) {
        return status.st_uid == ::geteuid() && (status.st_mode & 0600) == 0600;
    }
    return status.st_uid == ::geteuid() ? (status.st_mode & 0400) != 0
                                        : (::geteuid() == 0 || (status.st_mode & 0004) != 0);
}

[[nodiscard]] bool validCatalogNativeSpelling(const QString& value, qsizetype maximum_bytes) {
    if (value.contains(QChar::Null) || value.contains(QChar::ReplacementCharacter)) {
        return false;
    }
    for (qsizetype index = 0; index < value.size(); ++index) {
        const auto unit = value.at(index).unicode();
        if (unit >= 0xd800U && unit <= 0xdbffU) {
            if (++index >= value.size()) {
                return false;
            }
            const auto low = value.at(index).unicode();
            if (low < 0xdc00U || low > 0xdfffU) {
                return false;
            }
        } else if (unit >= 0xdc00U && unit <= 0xdfffU) {
            return false;
        }
    }
    const auto encoded = QFile::encodeName(value);
    return !encoded.contains('\0') && encoded.size() <= maximum_bytes &&
           QFile::decodeName(encoded) == value;
}

[[nodiscard]] std::optional<QStringList> catalogAbsoluteComponents(const QString& path,
                                                                   std::size_t maximum_components) {
    if (!path.startsWith(u'/') || !validCatalogNativeSpelling(path, 4'095)) {
        return std::nullopt;
    }
    if (path == QStringLiteral("/")) {
        return QStringList{};
    }
    const auto components = path.sliced(1).split(u'/', Qt::KeepEmptyParts);
    if (components.isEmpty() || static_cast<std::size_t>(components.size()) > maximum_components ||
        std::ranges::any_of(components, [](const QString& component) {
            return component.isEmpty() || component == QStringLiteral(".") ||
                   component == QStringLiteral("..") || !validCatalogNativeSpelling(component, 255);
        })) {
        return std::nullopt;
    }
    return components;
}

struct CatalogResolvedOperand final {
    QString absolute_path;
    QByteArray encoded_path;
    QStringList components;
    CatalogDescriptor retained_cwd;
    CatalogIdentity cwd_identity;
    std::size_t cwd_component_count{};
    bool relative{};
};

[[maybe_unused, nodiscard]] std::expected<CatalogResolvedOperand, CatalogError>
resolveCatalogOperand(const QString& supplied) {
    if (supplied.isEmpty() || supplied.contains(QChar::Null) ||
        supplied.contains(QChar::ReplacementCharacter)) {
        return fail(CatalogErrorCode::InvalidConfiguration,
                    QStringLiteral("Pack-catalog root spelling is invalid"));
    }
    const auto supplied_is_absolute = supplied.startsWith(u'/');
    const auto supplied_components =
        supplied_is_absolute ? catalogAbsoluteComponents(supplied, 126)
                             : std::optional<QStringList>{supplied.split(u'/', Qt::KeepEmptyParts)};
    if (!supplied_components || supplied_components->empty() ||
        std::ranges::any_of(*supplied_components, [](const QString& component) {
            return component.isEmpty() || component == QStringLiteral(".") ||
                   component == QStringLiteral("..") || !validCatalogNativeSpelling(component, 255);
        })) {
        return fail(CatalogErrorCode::InvalidConfiguration,
                    QStringLiteral("Pack-catalog root spelling is invalid"));
    }
    if (!validCatalogNativeSpelling(supplied, 4'095)) {
        return fail(CatalogErrorCode::InvalidConfiguration,
                    QStringLiteral("Pack-catalog root spelling exceeds native limits"));
    }

    QString absolute = supplied;
    CatalogDescriptor retained_cwd;
    CatalogIdentity cwd_identity;
    std::size_t cwd_component_count = 0;
    if (!supplied_is_absolute) {
        retained_cwd.reset(::open(".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        struct stat cwd_status{};
        if (!retained_cwd || ::fstat(retained_cwd.get(), &cwd_status) != 0) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Cannot retain the current directory"));
        }
        const auto captured_cwd = QDir::currentPath();
        const auto cwd_components = catalogAbsoluteComponents(captured_cwd, 128);
        if (!cwd_components) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Current directory cannot anchor the catalog root"));
        }
        cwd_identity = catalogIdentity(cwd_status);
        cwd_component_count = static_cast<std::size_t>(cwd_components->size());
        absolute = captured_cwd == QStringLiteral("/") ? QStringLiteral("/") + supplied
                                                       : captured_cwd + u'/' + supplied;
    }
    const auto components = catalogAbsoluteComponents(absolute, 126);
    if (!components) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Absolute catalog root exceeds native limits"));
    }
    return CatalogResolvedOperand{
        absolute,     QFile::encodeName(absolute), *components,          std::move(retained_cwd),
        cwd_identity, cwd_component_count,         !supplied_is_absolute};
}

[[nodiscard]] auto validateCatalogGeneratedPathHeadroom(const CatalogResolvedOperand& operand)
    -> std::expected<void, CatalogError> {
    // "/archives/" plus a 64-byte digest and ".awpack" is the longest fixed public catalog
    // descendant. The component-count ceiling was already reduced to 126 by operand parsing.
    constexpr qsizetype longest_descendant_bytes = 81;
    if (operand.encoded_path.size() > 4'095 - longest_descendant_bytes) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Pack-catalog root leaves no native path headroom"));
    }
    return {};
}

[[nodiscard]] bool catalogFsync(int descriptor) {
    while (true) {
        if (::fsync(descriptor) == 0) {
            return true;
        }
        if (errno != EINTR) {
            return false;
        }
    }
}

[[maybe_unused, nodiscard]] bool catalogFlock(int descriptor, int operation) {
    while (true) {
        if (::flock(descriptor, operation) == 0) {
            return true;
        }
        if (errno != EINTR) {
            return false;
        }
    }
}

struct CatalogController final {
    QString absolute_path;
    QByteArray component;
    CatalogDescriptor descriptor;
    CatalogIdentity identity;
    bool created_by_attempt{};
};

struct CatalogRetainedDirectory final {
    QString absolute_path;
    QByteArray component;
    CatalogDescriptor descriptor;
    CatalogIdentity identity;
};

struct CatalogRetainedFile final {
    QString absolute_path;
    QByteArray component;
    CatalogDescriptor descriptor;
    CatalogIdentity identity;
};

struct CatalogAnchor final {
    QString absolute_root;
    QByteArray encoded_root;
    std::vector<CatalogController> controllers;
    std::optional<CatalogRetainedDirectory> archives;
    std::optional<CatalogRetainedDirectory> blobs;
    std::vector<QString> created_paths;
    bool adopted_mode_zero_root{};

    [[nodiscard]] int rootDescriptor() const {
        return controllers.empty() ? -1 : controllers.back().descriptor.get();
    }

    [[nodiscard]] const CatalogIdentity* rootIdentity() const {
        return controllers.empty() ? nullptr : &controllers.back().identity;
    }
};

[[nodiscard]] detail::CatalogInjectedAction
catalogAction(const detail::CatalogHooks& hooks, const detail::CatalogObservation& observation) {
    if (hooks.report != nullptr) {
        hooks.report->observations.push_back(observation);
    }
    return hooks.inject ? hooks.inject(observation) : detail::CatalogInjectedAction::Continue;
}

[[nodiscard]] bool catalogFailsBefore(detail::CatalogInjectedAction action) {
    return action == detail::CatalogInjectedAction::FailBefore;
}

[[nodiscard]] bool catalogFinishes(const detail::CatalogHooks& hooks,
                                   const detail::CatalogObservation& observation,
                                   detail::CatalogInjectedAction action) {
    if (hooks.barrier) {
        hooks.barrier(observation);
    }
    if (hooks.observe) {
        hooks.observe(observation);
    }
    return action != detail::CatalogInjectedAction::FailAfter;
}

class CatalogRootLock final {
  public:
    CatalogRootLock() = default;
    CatalogRootLock(const CatalogRootLock&) = delete;
    CatalogRootLock& operator=(const CatalogRootLock&) = delete;
    CatalogRootLock(CatalogRootLock&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)), mode_(other.mode_),
          identity_(other.identity_), operation_(other.operation_), hooks_(std::move(other.hooks_)),
          absolute_path_(std::move(other.absolute_path_)) {}
    CatalogRootLock& operator=(CatalogRootLock&& other) noexcept {
        if (this != &other) {
            unlockBestEffort();
            descriptor_ = std::exchange(other.descriptor_, -1);
            mode_ = other.mode_;
            identity_ = other.identity_;
            operation_ = other.operation_;
            hooks_ = std::move(other.hooks_);
            absolute_path_ = std::move(other.absolute_path_);
        }
        return *this;
    }
    ~CatalogRootLock() { unlockBestEffort(); }

    [[nodiscard]] bool isHeld() const noexcept { return descriptor_ >= 0; }
    [[nodiscard]] detail::CatalogLockMode mode() const noexcept { return mode_; }
    [[nodiscard]] bool protects(const CatalogAnchor& anchor, bool writable) const {
        struct stat held{};
        return descriptor_ >= 0 && descriptor_ == anchor.rootDescriptor() &&
               (!writable || mode_ == detail::CatalogLockMode::Exclusive) &&
               ::fstat(descriptor_, &held) == 0 && sameCatalogIdentity(identity_, held, false) &&
               anchor.rootIdentity() != nullptr &&
               sameCatalogTupleBase(*anchor.rootIdentity(), identity_);
    }

    [[nodiscard]] bool refreshAfterAuthorizedTransition(const CatalogAnchor& anchor) {
        struct stat held{};
        if (descriptor_ < 0 || descriptor_ != anchor.rootDescriptor() ||
            anchor.rootIdentity() == nullptr || ::fstat(descriptor_, &held) != 0 ||
            held.st_dev != identity_.device || held.st_ino != identity_.inode ||
            !S_ISDIR(held.st_mode) || held.st_uid != ::geteuid() ||
            (held.st_mode & 07777) != 0700 ||
            !sameCatalogIdentity(*anchor.rootIdentity(), held, false)) {
            return false;
        }
        identity_ = catalogIdentity(held);
        return true;
    }

    [[nodiscard]] std::expected<void, CatalogError> release() {
        if (descriptor_ < 0) {
            return {};
        }
        detail::CatalogObservation observation;
        observation.event = detail::CatalogEvent::RootLockReleased;
        observation.subject = detail::CatalogSubject::CatalogRoot;
        observation.operation = operation_;
        observation.lock_mode = mode_;
        observation.absolute_path = absolute_path_;
        const auto action = catalogAction(hooks_, observation);
        const auto unlocked = catalogFlock(descriptor_, LOCK_UN);
        if (unlocked) {
            descriptor_ = -1;
        }
        const auto finished = catalogFinishes(hooks_, observation, action);
        if (catalogFailsBefore(action) || !unlocked || !finished) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Cannot release retained catalog lock"));
        }
        return {};
    }

  private:
    friend auto lockCatalogRoot(int, const QString&, detail::CatalogLockMode,
                                detail::CatalogOperation, const detail::CatalogHooks&, bool)
        -> std::expected<CatalogRootLock, CatalogError>;

    CatalogRootLock(int descriptor, detail::CatalogLockMode mode, CatalogIdentity identity,
                    detail::CatalogOperation operation, detail::CatalogHooks hooks,
                    QString absolute_path)
        : descriptor_(descriptor), mode_(mode), identity_(identity), operation_(operation),
          hooks_(std::move(hooks)), absolute_path_(std::move(absolute_path)) {}

    [[nodiscard]] static bool sameCatalogTupleBase(const CatalogIdentity& left,
                                                   const CatalogIdentity& right) {
        return left.device == right.device && left.inode == right.inode &&
               left.mode == right.mode && (S_ISDIR(left.mode) || left.links == right.links) &&
               left.owner == right.owner;
    }

    void unlockBestEffort() noexcept {
        if (descriptor_ >= 0) {
            static_cast<void>(catalogFlock(std::exchange(descriptor_, -1), LOCK_UN));
        }
    }

    int descriptor_{-1};
    detail::CatalogLockMode mode_{detail::CatalogLockMode::None};
    CatalogIdentity identity_;
    detail::CatalogOperation operation_{detail::CatalogOperation::None};
    detail::CatalogHooks hooks_;
    QString absolute_path_;
};

[[maybe_unused, nodiscard]] auto
lockCatalogRoot(int descriptor, const QString& absolute_path, detail::CatalogLockMode mode,
                detail::CatalogOperation operation, const detail::CatalogHooks& hooks,
                bool nonblocking = true) -> std::expected<CatalogRootLock, CatalogError> {
    detail::CatalogObservation attempted;
    attempted.event = detail::CatalogEvent::RootLockAttempted;
    attempted.subject = detail::CatalogSubject::CatalogRoot;
    attempted.operation = operation;
    attempted.lock_mode = mode;
    attempted.absolute_path = absolute_path;
    if (hooks.report != nullptr) {
        ++hooks.report->lock_attempts;
    }
    const auto action = catalogAction(hooks, attempted);
    struct stat before{};
    if (::fstat(descriptor, &before) != 0 || !S_ISDIR(before.st_mode)) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Cannot bind retained catalog lock"));
    }
    const auto flock_operation =
        (mode == detail::CatalogLockMode::Shared ? LOCK_SH : LOCK_EX) | (nonblocking ? LOCK_NB : 0);
    const auto injected_before = catalogFailsBefore(action);
    errno = 0;
    if (injected_before || !catalogFlock(descriptor, flock_operation)) {
        const auto lock_error = injected_before ? ECANCELED : errno;
        return fail(lock_error == EWOULDBLOCK || lock_error == EAGAIN
                        ? CatalogErrorCode::CatalogBusy
                        : CatalogErrorCode::CannotOpen,
                    lock_error == EWOULDBLOCK || lock_error == EAGAIN
                        ? QStringLiteral("Catalog is busy")
                        : QStringLiteral("Cannot acquire retained catalog lock"));
    }
    struct stat after{};
    if (::fstat(descriptor, &after) != 0 || before.st_dev != after.st_dev ||
        before.st_ino != after.st_ino || before.st_mode != after.st_mode ||
        before.st_uid != after.st_uid || before.st_nlink != after.st_nlink) {
        static_cast<void>(catalogFlock(descriptor, LOCK_UN));
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Catalog root changed during lock acquisition"));
    }
    CatalogRootLock lock(descriptor, mode, catalogIdentity(after), operation, hooks, absolute_path);
    if (!catalogFinishes(hooks, attempted, action)) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Injected catalog lock-acquisition failure"));
    }
    detail::CatalogObservation acquired = attempted;
    acquired.event = detail::CatalogEvent::RootLockAcquired;
    const auto acquired_action = catalogAction(hooks, acquired);
    if (catalogFailsBefore(acquired_action) || !catalogFinishes(hooks, acquired, acquired_action)) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Injected catalog lock-acquisition failure"));
    }
    return lock;
}

[[nodiscard]] detail::CatalogIdentity reportedCatalogIdentity(const CatalogIdentity& identity) {
    auto type = detail::CatalogNodeType::Other;
    if (S_ISDIR(identity.mode)) {
        type = detail::CatalogNodeType::Directory;
    } else if (S_ISREG(identity.mode)) {
        type = detail::CatalogNodeType::RegularFile;
    }
    return detail::CatalogIdentity{
        static_cast<std::uint64_t>(identity.device),
        static_cast<std::uint64_t>(identity.inode),
        type,
        static_cast<std::uint64_t>(identity.links),
        static_cast<std::uint64_t>(identity.owner),
        static_cast<unsigned int>(identity.mode & 07777),
        identity.size < 0 ? 0U : static_cast<std::uint64_t>(identity.size),
        static_cast<std::int64_t>(identity.modified.tv_sec),
        static_cast<std::int64_t>(identity.modified.tv_nsec),
        static_cast<std::int64_t>(identity.changed.tv_sec),
        static_cast<std::int64_t>(identity.changed.tv_nsec),
    };
}

struct CatalogLinuxDirent64 final {
    std::uint64_t inode;
    std::int64_t offset;
    unsigned short record_length;
    unsigned char type;
    char name[1];
};

[[nodiscard]] std::expected<std::vector<QByteArray>, CatalogError>
readCatalogDirectoryNames(int descriptor, std::size_t& aggregate_count) {
    CatalogDescriptor enumeration(
        ::openat(descriptor, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!enumeration) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Cannot enumerate retained catalog directory"));
    }
    std::vector<QByteArray> names;
    names.reserve(maximum_catalog_inventory_entries - aggregate_count);
    std::array<char, 64 * 1024> buffer{};
    while (true) {
        const auto count =
            ::syscall(SYS_getdents64, enumeration.get(), buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Cannot read retained catalog directory"));
        }
        if (count == 0) {
            break;
        }
        std::size_t offset = 0;
        while (offset < static_cast<std::size_t>(count)) {
            constexpr auto name_offset = offsetof(CatalogLinuxDirent64, name);
            CatalogLinuxDirent64 record{};
            if (static_cast<std::size_t>(count) - offset < name_offset) {
                return fail(CatalogErrorCode::CorruptCatalog,
                            QStringLiteral("Catalog directory record is malformed"));
            }
            std::memcpy(&record, buffer.data() + offset, name_offset);
            if (record.record_length < name_offset + 1U ||
                record.record_length > static_cast<std::size_t>(count) - offset) {
                return fail(CatalogErrorCode::CorruptCatalog,
                            QStringLiteral("Catalog directory record is malformed"));
            }
            const auto name_bytes = record.record_length - name_offset;
            const auto* raw = buffer.data() + offset + name_offset;
            const auto* terminator = static_cast<const char*>(std::memchr(raw, '\0', name_bytes));
            if (terminator == nullptr) {
                return fail(CatalogErrorCode::CorruptCatalog,
                            QStringLiteral("Catalog directory name is malformed"));
            }
            const QByteArray name(raw, static_cast<qsizetype>(terminator - raw));
            offset += record.record_length;
            if (name == QByteArrayView(".") || name == QByteArrayView("..")) {
                continue;
            }
            if (aggregate_count == maximum_catalog_inventory_entries) {
                return fail(CatalogErrorCode::CorruptCatalog,
                            QStringLiteral("Catalog namespace exceeds 20000 entries"));
            }
            ++aggregate_count;
            names.push_back(name);
        }
    }
    std::ranges::sort(names);
    return names;
}

[[nodiscard]] std::expected<std::vector<QByteArray>, CatalogError>
catalogDirectoryNames(int descriptor, std::size_t& aggregate_count, const QString& absolute_path,
                      detail::CatalogSubject subject, detail::CatalogOperation operation,
                      const detail::CatalogHooks& hooks) {
    detail::CatalogObservation observation;
    observation.event = detail::CatalogEvent::NamespaceInventoried;
    observation.subject = subject;
    observation.operation = operation;
    observation.absolute_path = absolute_path;
    const auto action = catalogAction(hooks, observation);
    if (catalogFailsBefore(action)) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Injected catalog inventory failure"));
    }
    const auto base_count = aggregate_count;
    auto names = readCatalogDirectoryNames(descriptor, aggregate_count);
    if (!names) {
        return std::unexpected(names.error());
    }
    if (!catalogFinishes(hooks, observation, action)) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Injected catalog inventory failure"));
    }
    auto verification_count = base_count;
    const auto rebound = readCatalogDirectoryNames(descriptor, verification_count);
    if (!rebound || *rebound != *names || verification_count != aggregate_count) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Catalog directory changed during inventory"));
    }
    return names;
}

[[nodiscard]] bool catalogDirectoryEmpty(int descriptor) {
    std::size_t count = 0;
    const auto names = catalogDirectoryNames(descriptor, count, {}, detail::CatalogSubject::None,
                                             detail::CatalogOperation::None, {});
    return names.has_value() && names->empty();
}

[[nodiscard]] bool catalogIdentityRebound(int parent_descriptor, const QByteArray& component,
                                          int held_descriptor, const CatalogIdentity& identity,
                                          bool include_size_and_times = false) {
    struct stat held{};
    struct stat named{};
    return ::fstat(held_descriptor, &held) == 0 &&
           ::fstatat(parent_descriptor, component.constData(), &named, AT_SYMLINK_NOFOLLOW) == 0 &&
           sameCatalogIdentity(identity, held, include_size_and_times) &&
           sameCatalogIdentity(identity, named, include_size_and_times);
}

[[nodiscard]] bool catalogDescriptorChmodAtEmptyPath(int descriptor, mode_t mode) {
#if defined(SYS_fchmodat2)
    return ::syscall(SYS_fchmodat2, descriptor, "", mode, AT_EMPTY_PATH) == 0;
#else
    static_cast<void>(descriptor);
    static_cast<void>(mode);
    errno = ENOSYS;
    return false;
#endif
}

[[nodiscard]] std::expected<CatalogController, CatalogError>
retainCreatedCatalogDirectory(int parent_descriptor, const QByteArray& component,
                              const QString& absolute_path, bool already_existed_mode_zero,
                              detail::CatalogSubject subject, detail::CatalogOperation operation,
                              const detail::CatalogHooks& hooks,
                              CatalogDescriptor path_descriptor = CatalogDescriptor{-1},
                              std::optional<CatalogIdentity> retained_identity = std::nullopt,
                              CatalogIdentity* reconciliation_identity = nullptr) {
    if (!path_descriptor) {
        path_descriptor.reset(::openat(parent_descriptor, component.constData(),
                                       O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    }
    struct stat provisional{};
    if (!path_descriptor || ::fstat(path_descriptor.get(), &provisional) != 0 ||
        !S_ISDIR(provisional.st_mode) || provisional.st_uid != ::geteuid() ||
        (provisional.st_mode & 07777) != 0000 ||
        (retained_identity && !sameCatalogIdentity(*retained_identity, provisional, true))) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Cannot retain a catalog directory transition"));
    }
    const auto provisional_identity = catalogIdentity(provisional);
    if (reconciliation_identity != nullptr) {
        *reconciliation_identity = provisional_identity;
    }
    detail::CatalogObservation normalized;
    normalized.event = detail::CatalogEvent::DirectoryNormalized;
    normalized.subject = subject;
    normalized.operation = operation;
    normalized.absolute_path = absolute_path;
    normalized.component = component;
    normalized.mode_before = static_cast<unsigned int>(provisional.st_mode & 07777);
    normalized.mode_after = 0700;
    normalized.identity_before = reportedCatalogIdentity(provisional_identity);
    const auto action = catalogAction(hooks, normalized);
    const auto changed = !catalogFailsBefore(action) &&
                         catalogDescriptorChmodAtEmptyPath(path_descriptor.get(), 0700);
    if (!changed) {
        struct stat observed{};
        if (::fstat(path_descriptor.get(), &observed) == 0 &&
            observed.st_dev == provisional.st_dev && observed.st_ino == provisional.st_ino &&
            reconciliation_identity != nullptr) {
            *reconciliation_identity = catalogIdentity(observed);
        }
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Cannot normalize a catalog directory transition"));
    }
    struct stat transitioned{};
    struct stat named{};
    const auto transition_retained = ::fstat(path_descriptor.get(), &transitioned) == 0;
    if (transition_retained && reconciliation_identity != nullptr) {
        *reconciliation_identity = catalogIdentity(transitioned);
    }
    if (!transition_retained ||
        ::fstatat(parent_descriptor, component.constData(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
        transitioned.st_dev != provisional.st_dev || transitioned.st_ino != provisional.st_ino ||
        named.st_dev != provisional.st_dev || named.st_ino != provisional.st_ino ||
        transitioned.st_uid != ::geteuid() || (transitioned.st_mode & 07777) != 0700) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Catalog directory transition lost its binding"));
    }
    normalized.identity_after = reportedCatalogIdentity(catalogIdentity(transitioned));
    if (!catalogFinishes(hooks, normalized, action)) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Injected catalog directory normalization failure"));
    }
    CatalogDescriptor descriptor(::openat(parent_descriptor, component.constData(),
                                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat actual{};
    if (!descriptor || ::fstat(descriptor.get(), &actual) != 0 ||
        actual.st_dev != provisional.st_dev || actual.st_ino != provisional.st_ino ||
        !catalogNamespaceDirectoryPolicy(actual, true) ||
        !catalogDirectoryAclsAbsent(descriptor.get()) || !catalogFsync(descriptor.get()) ||
        !catalogFsync(parent_descriptor)) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Catalog directory transition is unsafe"));
    }
    if (already_existed_mode_zero && !catalogDirectoryEmpty(descriptor.get())) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Mode-zero catalog transition is not empty"));
    }
    return CatalogController{absolute_path, component, std::move(descriptor),
                             catalogIdentity(actual), false};
}

[[maybe_unused, nodiscard]] std::expected<CatalogAnchor, CatalogError>
retainCatalogRoot(CatalogResolvedOperand operand, bool writable, bool create_missing,
                  detail::CatalogOperation operation, const detail::CatalogHooks& hooks) {
    CatalogAnchor anchor;
    anchor.absolute_root = operand.absolute_path;
    anchor.encoded_root = operand.encoded_path;
    CatalogDescriptor root(::open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat root_status{};
    if (!root || ::fstat(root.get(), &root_status) != 0 || !catalogControllerPolicy(root_status)) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Cannot retain the catalog controller root"));
    }
    if (!catalogDirectoryAclsAbsent(root.get())) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Catalog controller ACL is present or unprovable"));
    }
    anchor.controllers.push_back(
        CatalogController{QStringLiteral("/"), {}, std::move(root), catalogIdentity(root_status)});
    if (operand.relative && operand.cwd_component_count == 0 &&
        (operand.cwd_identity.device != root_status.st_dev ||
         operand.cwd_identity.inode != root_status.st_ino)) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Current-directory binding changed"));
    }

    QString absolute;
    for (qsizetype index = 0; index < operand.components.size(); ++index) {
        const auto& text = operand.components.at(index);
        const auto component = QFile::encodeName(text);
        const auto final_component = index + 1 == operand.components.size();
        absolute += u'/' + text;
        auto& parent = anchor.controllers.back();
        struct stat named{};
        auto named_result =
            ::fstatat(parent.descriptor.get(), component.constData(), &named, AT_SYMLINK_NOFOLLOW);
        if (named_result != 0 && errno == EINTR) {
            do {
                named_result = ::fstatat(parent.descriptor.get(), component.constData(), &named,
                                         AT_SYMLINK_NOFOLLOW);
            } while (named_result != 0 && errno == EINTR);
        }
        bool created = false;
        if (named_result != 0) {
            if (errno != ENOENT || !writable || !create_missing ||
                (operand.relative &&
                 static_cast<std::size_t>(index) < operand.cwd_component_count)) {
                return fail(CatalogErrorCode::CannotOpen,
                            QStringLiteral("Catalog root does not exist or cannot be retained"));
            }
            detail::CatalogObservation creation;
            creation.event = detail::CatalogEvent::DirectoryCreated;
            creation.subject = final_component ? detail::CatalogSubject::CatalogRoot
                                               : detail::CatalogSubject::ExternalController;
            creation.operation = operation;
            creation.absolute_path = absolute;
            creation.component = component;
            const auto action = catalogAction(hooks, creation);
            if (catalogFailsBefore(action) ||
                ::mkdirat(parent.descriptor.get(), component.constData(), 0000) != 0) {
                return fail(CatalogErrorCode::CannotOpen,
                            QStringLiteral("Cannot create catalog root component"));
            }
            created = true;
            anchor.created_paths.push_back(absolute);
            CatalogDescriptor retained_created(
                ::openat(parent.descriptor.get(), component.constData(),
                         O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            struct stat created_status{};
            if (!retained_created || ::fstat(retained_created.get(), &created_status) != 0 ||
                !S_ISDIR(created_status.st_mode) || created_status.st_uid != ::geteuid() ||
                (created_status.st_mode & 07777) != 0000) {
                return fail(CatalogErrorCode::CorruptCatalog,
                            QStringLiteral("Created catalog directory cannot be retained"));
            }
            creation.identity_after = reportedCatalogIdentity(catalogIdentity(created_status));
            if (!catalogFinishes(hooks, creation, action)) {
                return fail(CatalogErrorCode::CannotOpen,
                            QStringLiteral("Injected catalog directory creation failure"));
            }
            if (::fstatat(parent.descriptor.get(), component.constData(), &named,
                          AT_SYMLINK_NOFOLLOW) != 0 ||
                named.st_dev != created_status.st_dev || named.st_ino != created_status.st_ino) {
                return fail(CatalogErrorCode::CorruptCatalog,
                            QStringLiteral("Created catalog directory cannot be rebound"));
            }
            auto transitioned = retainCreatedCatalogDirectory(
                parent.descriptor.get(), component, absolute, false,
                final_component ? detail::CatalogSubject::CatalogRoot
                                : detail::CatalogSubject::ExternalController,
                operation, hooks, std::move(retained_created), catalogIdentity(created_status));
            if (!transitioned) {
                return std::unexpected(transitioned.error());
            }
            transitioned->created_by_attempt = true;
            anchor.controllers.push_back(std::move(*transitioned));
            const auto completed_count = static_cast<std::size_t>(index + 1);
            if (operand.relative && completed_count == operand.cwd_component_count) {
                const auto& cwd_controller = anchor.controllers.back();
                if (cwd_controller.identity.device != operand.cwd_identity.device ||
                    cwd_controller.identity.inode != operand.cwd_identity.inode) {
                    return fail(CatalogErrorCode::CannotOpen,
                                QStringLiteral("Current-directory binding changed"));
                }
            }
            continue;
        }

        if (S_ISDIR(named.st_mode) && named.st_uid == ::geteuid() &&
            (named.st_mode & 07777) == 0000) {
            if (!writable) {
                return fail(final_component ? CatalogErrorCode::CatalogBusy
                                            : CatalogErrorCode::CannotOpen,
                            QStringLiteral("Catalog directory transition is in progress"));
            }
            auto transitioned = retainCreatedCatalogDirectory(
                parent.descriptor.get(), component, absolute, !created && !final_component,
                final_component ? detail::CatalogSubject::CatalogRoot
                                : detail::CatalogSubject::ExternalController,
                operation, hooks);
            if (!transitioned) {
                return std::unexpected(transitioned.error());
            }
            transitioned->created_by_attempt = false;
            if (final_component) {
                anchor.adopted_mode_zero_root = true;
            }
            anchor.controllers.push_back(std::move(*transitioned));
        } else {
            CatalogDescriptor descriptor(::openat(parent.descriptor.get(), component.constData(),
                                                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            struct stat held{};
            struct stat parent_rebound{};
            if (!descriptor || ::fstat(descriptor.get(), &held) != 0 ||
                ::fstat(parent.descriptor.get(), &parent_rebound) != 0 ||
                held.st_dev != named.st_dev || held.st_ino != named.st_ino ||
                !sameCatalogIdentity(parent.identity, parent_rebound, false)) {
                return fail(CatalogErrorCode::CannotOpen,
                            QStringLiteral("Catalog controller binding changed"));
            }
            if (final_component) {
                if (!catalogDirectoryAclsAbsent(descriptor.get())) {
                    return fail(CatalogErrorCode::CorruptCatalog,
                                QStringLiteral("Catalog root ACL is present or unprovable"));
                }
                if (!catalogNamespaceDirectoryPolicy(held, false)) {
                    return fail(CatalogErrorCode::CorruptCatalog,
                                QStringLiteral("Catalog root policy is unsafe"));
                }
                if (writable && (held.st_uid != ::geteuid() || (held.st_mode & 0700) != 0700)) {
                    return fail(CatalogErrorCode::CannotOpen,
                                QStringLiteral("Catalog root is not writable by this process"));
                }
            } else {
                if (!catalogControllerPolicy(held) ||
                    (((parent.identity.mode & S_ISVTX) != 0) && held.st_uid != ::geteuid() &&
                     held.st_uid != 0)) {
                    return fail(CatalogErrorCode::CannotOpen,
                                QStringLiteral("Catalog controller policy is unsafe"));
                }
                if (!catalogDirectoryAclsAbsent(descriptor.get())) {
                    return fail(CatalogErrorCode::CorruptCatalog,
                                QStringLiteral("Catalog controller ACL is present or unprovable"));
                }
            }
            anchor.controllers.push_back(CatalogController{
                absolute, component, std::move(descriptor), catalogIdentity(held)});
        }
        const auto completed_count = static_cast<std::size_t>(index + 1);
        if (operand.relative && completed_count == operand.cwd_component_count) {
            const auto& cwd_controller = anchor.controllers.back();
            if (cwd_controller.identity.device != operand.cwd_identity.device ||
                cwd_controller.identity.inode != operand.cwd_identity.inode) {
                return fail(CatalogErrorCode::CannotOpen,
                            QStringLiteral("Current-directory binding changed"));
            }
        }
    }
    if (hooks.report != nullptr) {
        hooks.report->immutable_root_path = anchor.absolute_root;
    }
    return anchor;
}

[[nodiscard]] auto revalidateCatalogAnchor(const CatalogAnchor& anchor)
    -> std::expected<void, CatalogError> {
    for (std::size_t index = 0; index < anchor.controllers.size(); ++index) {
        const auto& controller = anchor.controllers.at(index);
        struct stat held{};
        if (::fstat(controller.descriptor.get(), &held) != 0 ||
            !sameCatalogIdentity(controller.identity, held, false)) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Catalog controller binding changed"));
        }
        if (!catalogDirectoryAclsAbsent(controller.descriptor.get())) {
            return fail(CatalogErrorCode::CorruptCatalog,
                        QStringLiteral("Catalog controller ACL is present or unprovable"));
        }
        if (index == 0) {
            if (!catalogControllerPolicy(held)) {
                return fail(CatalogErrorCode::CannotOpen,
                            QStringLiteral("Catalog controller policy changed"));
            }
            continue;
        }
        const auto& parent = anchor.controllers.at(index - 1U);
        struct stat named{};
        if (::fstatat(parent.descriptor.get(), controller.component.constData(), &named,
                      AT_SYMLINK_NOFOLLOW) != 0 ||
            !sameCatalogIdentity(controller.identity, named, false)) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Catalog controller name binding changed"));
        }
        if (index + 1U == anchor.controllers.size()) {
            if (!catalogNamespaceDirectoryPolicy(held, false)) {
                return fail(CatalogErrorCode::CannotOpen,
                            QStringLiteral("Catalog root policy changed"));
            }
        } else if (!catalogControllerPolicy(held) ||
                   (((parent.identity.mode & S_ISVTX) != 0) && held.st_uid != ::geteuid() &&
                    held.st_uid != 0)) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Catalog controller policy changed"));
        }
    }
    return {};
}

[[nodiscard]] std::expected<CatalogRetainedDirectory, CatalogError> retainCatalogNamespaceDirectory(
    const CatalogAnchor& anchor, QByteArray component, detail::CatalogSubject subject,
    bool writable, detail::CatalogOperation operation, const detail::CatalogHooks& hooks) {
    const auto absolute = anchor.absolute_root + u'/' + QString::fromLatin1(component);
    detail::CatalogObservation opened;
    opened.event = detail::CatalogEvent::IdentityRetained;
    opened.subject = subject;
    opened.operation = operation;
    opened.absolute_path = absolute;
    opened.component = component;
    const auto action = catalogAction(hooks, opened);
    if (catalogFailsBefore(action)) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Injected catalog-directory retention failure"));
    }
    CatalogDescriptor descriptor(::openat(anchor.rootDescriptor(), component.constData(),
                                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat held{};
    struct stat named{};
    if (!descriptor || ::fstat(descriptor.get(), &held) != 0 ||
        ::fstatat(anchor.rootDescriptor(), component.constData(), &named, AT_SYMLINK_NOFOLLOW) !=
            0 ||
        held.st_dev != named.st_dev || held.st_ino != named.st_ino ||
        !catalogNamespaceDirectoryPolicy(held, false)) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Catalog namespace directory is unsafe"));
    }
    if (!catalogDirectoryAclsAbsent(descriptor.get())) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Catalog namespace directory ACL is present or unprovable"));
    }
    if (writable && (held.st_uid != ::geteuid() || (held.st_mode & 0700) != 0700)) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Catalog namespace directory is not writable"));
    }
    const auto identity = catalogIdentity(held);
    opened.identity_after = reportedCatalogIdentity(identity);
    if (!catalogFinishes(hooks, opened, action) ||
        !catalogIdentityRebound(anchor.rootDescriptor(), component, descriptor.get(), identity)) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Catalog namespace directory binding changed"));
    }
    if (!catalogDirectoryAclsAbsent(descriptor.get())) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Catalog namespace directory ACL appeared"));
    }
    return CatalogRetainedDirectory{absolute, std::move(component), std::move(descriptor),
                                    identity};
}

[[nodiscard]] std::expected<CatalogRetainedFile, CatalogError>
retainCatalogFile(int parent_descriptor, const QString& parent_path, QByteArray component,
                  detail::CatalogSubject subject, bool writable, detail::CatalogOperation operation,
                  const detail::CatalogHooks& hooks, bool include_mutable_identity = true) {
    const auto absolute = parent_path + u'/' + QString::fromLatin1(component);
    detail::CatalogObservation opened;
    opened.event = detail::CatalogEvent::IdentityRetained;
    opened.subject = subject;
    opened.operation = operation;
    opened.absolute_path = absolute;
    opened.component = component;
    const auto action = catalogAction(hooks, opened);
    if (catalogFailsBefore(action)) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Injected catalog-file retention failure"));
    }
    CatalogDescriptor descriptor(
        ::openat(parent_descriptor, component.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    struct stat held{};
    struct stat named{};
    if (!descriptor || ::fstat(descriptor.get(), &held) != 0 ||
        ::fstatat(parent_descriptor, component.constData(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
        held.st_dev != named.st_dev || held.st_ino != named.st_ino ||
        !catalogNamespaceFilePolicy(held, false)) {
        return fail(CatalogErrorCode::CorruptCatalog, QStringLiteral("Catalog file is unsafe"));
    }
    if (!catalogFileAclAbsent(descriptor.get())) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Catalog file ACL is present or unprovable"));
    }
    if (writable && (held.st_uid != ::geteuid() || (held.st_mode & 0600) != 0600)) {
        return fail(CatalogErrorCode::CannotOpen, QStringLiteral("Catalog file is not writable"));
    }
    const auto identity = catalogIdentity(held);
    opened.identity_after = reportedCatalogIdentity(identity);
    if (!catalogFinishes(hooks, opened, action) ||
        !catalogIdentityRebound(parent_descriptor, component, descriptor.get(), identity,
                                include_mutable_identity)) {
        return fail(CatalogErrorCode::CannotOpen, QStringLiteral("Catalog file binding changed"));
    }
    if (!catalogFileAclAbsent(descriptor.get())) {
        return fail(CatalogErrorCode::CorruptCatalog, QStringLiteral("Catalog file ACL appeared"));
    }
    return CatalogRetainedFile{absolute, std::move(component), std::move(descriptor), identity};
}

[[nodiscard]] bool validCatalogDigest(QByteArrayView value) {
    return value.size() == 64 && std::ranges::all_of(value, [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

struct CatalogObjectIdentity final {
    QByteArray name;
    CatalogIdentity identity;
};

struct CatalogNamespace final {
    detail::CatalogNamespaceShape shape{detail::CatalogNamespaceShape::Unknown};
    std::vector<QByteArray> root_names;
    CatalogIdentity root_identity;
    CatalogIdentity archives_identity;
    std::optional<CatalogIdentity> blobs_identity;
    std::vector<CatalogObjectIdentity> archives;
    std::vector<CatalogObjectIdentity> blobs;
    CatalogRetainedFile main;
    std::optional<CatalogRetainedFile> wal;
    std::optional<CatalogRetainedFile> shm;
    std::optional<CatalogRetainedFile> legacy_lock;
};

[[nodiscard]] bool namesMatchShape(const std::vector<QByteArray>& names,
                                   std::span<const QByteArrayView> required,
                                   std::span<const QByteArrayView> optional) {
    for (const auto required_name : required) {
        if (!std::ranges::contains(names, required_name)) {
            return false;
        }
    }
    return std::ranges::all_of(names, [&](const QByteArray& name) {
        return std::ranges::contains(required, QByteArrayView(name)) ||
               std::ranges::contains(optional, QByteArrayView(name));
    });
}

[[nodiscard]] std::expected<std::vector<CatalogObjectIdentity>, CatalogError>
retainCatalogObjects(const CatalogRetainedDirectory& directory, std::vector<QByteArray> names,
                     bool archives, detail::CatalogOperation operation,
                     const detail::CatalogHooks& hooks) {
    std::vector<CatalogObjectIdentity> objects;
    objects.reserve(names.size());
    for (auto& name : names) {
        const auto valid_name =
            archives ? name.endsWith(QByteArrayView(".awpack")) &&
                           validCatalogDigest(QByteArrayView(name).first(name.size() - 7))
                     : validCatalogDigest(name);
        if (!valid_name) {
            if (hooks.report != nullptr) {
                hooks.report->unexpected_raw_names.push_back(name);
            }
            return fail(CatalogErrorCode::CorruptCatalog,
                        QStringLiteral("Catalog object namespace contains an unexpected name"));
        }
        auto retained = retainCatalogFile(directory.descriptor.get(), directory.absolute_path, name,
                                          archives ? detail::CatalogSubject::ArchiveObject
                                                   : detail::CatalogSubject::BlobObject,
                                          false, operation, hooks);
        if (!retained) {
            return std::unexpected(retained.error());
        }
        if ((archives &&
             (retained->identity.size < 0 || static_cast<std::uint64_t>(retained->identity.size) >
                                                 maximum_catalog_archive_bytes)) ||
            (!archives && retained->identity.size < 0)) {
            return fail(CatalogErrorCode::CorruptCatalog,
                        QStringLiteral("Catalog object exceeds its admission limit"));
        }
        objects.push_back(CatalogObjectIdentity{std::move(name), retained->identity});
    }
    return objects;
}

[[maybe_unused, nodiscard]] std::expected<CatalogNamespace, CatalogError> classifyCatalogNamespace(
    CatalogAnchor& anchor, bool writable, const CatalogRootLock& root_lock,
    detail::CatalogOperation operation, const detail::CatalogHooks& hooks,
    const std::optional<CatalogIdentity>& allowed_attempt_lock = std::nullopt) {
    if (!root_lock.protects(anchor, writable)) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Catalog namespace requires a retained root lock"));
    }
    struct stat root_before{};
    if (::fstat(anchor.rootDescriptor(), &root_before) != 0 || anchor.rootIdentity() == nullptr ||
        !sameCatalogIdentity(*anchor.rootIdentity(), root_before, false)) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Catalog root binding is not stable"));
    }
    if (!catalogDirectoryAclsAbsent(anchor.rootDescriptor())) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Catalog root ACL is present or unprovable"));
    }
    std::size_t aggregate_count = 0;
    auto root_names =
        catalogDirectoryNames(anchor.rootDescriptor(), aggregate_count, anchor.absolute_root,
                              detail::CatalogSubject::CatalogRoot, operation, hooks);
    if (!root_names) {
        return std::unexpected(root_names.error());
    }
    if (const auto valid = revalidateCatalogAnchor(anchor); !valid) {
        return std::unexpected(valid.error());
    }
    struct stat root_after{};
    if (::fstat(anchor.rootDescriptor(), &root_after) != 0 ||
        !sameCatalogIdentity(catalogIdentity(root_before), root_after, true)) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Catalog root changed during inventory"));
    }
    CatalogNamespace result;
    result.root_names = *root_names;
    result.root_identity = catalogIdentity(root_after);
    if (root_names->empty()) {
        result.shape = detail::CatalogNamespaceShape::Empty;
        if (hooks.report != nullptr) {
            hooks.report->admitted_shape = result.shape;
        }
        return result;
    }

    constexpr std::array v1_required{QByteArrayView("archives"), QByteArrayView("catalog.sqlite")};
    constexpr std::array v1_optional{QByteArrayView("catalog.sqlite-wal"),
                                     QByteArrayView("catalog.sqlite-shm")};
    constexpr std::array current_required{QByteArrayView("archives"), QByteArrayView("blobs"),
                                          QByteArrayView("catalog.sqlite")};
    constexpr std::array current_optional{QByteArrayView("catalog.sqlite-wal"),
                                          QByteArrayView("catalog.sqlite-shm"),
                                          QByteArrayView(".install.lock")};
    if (namesMatchShape(*root_names, v1_required, v1_optional)) {
        result.shape = detail::CatalogNamespaceShape::ClosedVersion1;
    } else if (namesMatchShape(*root_names, current_required, current_optional)) {
        result.shape = detail::CatalogNamespaceShape::Current;
    } else {
        if (hooks.report != nullptr) {
            for (const auto& name : *root_names) {
                if (name == QByteArrayView("catalog.sqlite-journal") ||
                    name == QByteArrayView(".install.lock.rmlock") || name.startsWith(".awpack-") ||
                    name.startsWith(".blob-")) {
                    hooks.report->unexpected_raw_names.push_back(name);
                }
            }
        }
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Catalog root namespace is partial or foreign"));
    }
    if (hooks.report != nullptr) {
        hooks.report->admitted_shape = result.shape;
    }

    if (std::ranges::contains(*root_names, QByteArrayView(".install.lock"))) {
        auto lock = retainCatalogFile(anchor.rootDescriptor(), anchor.absolute_root,
                                      QByteArrayLiteral(".install.lock"),
                                      detail::CatalogSubject::InstallLock, false, operation, hooks);
        if (!lock || lock->identity.owner != ::geteuid() || lock->identity.size < 1 ||
            lock->identity.size > 4'096 || (lock->identity.mode & 0400) == 0) {
            return fail(CatalogErrorCode::CorruptCatalog,
                        QStringLiteral("Persistent catalog lock is structurally unsafe"));
        }
        if (!allowed_attempt_lock) {
            return fail(CatalogErrorCode::CatalogBusy,
                        QStringLiteral("Catalog mutation lock is present"));
        }
        struct stat lock_status{};
        if (::fstat(lock->descriptor.get(), &lock_status) != 0 ||
            !sameCatalogIdentity(*allowed_attempt_lock, lock_status, true)) {
            return fail(CatalogErrorCode::CorruptCatalog,
                        QStringLiteral("Attempt-owned catalog lock binding changed"));
        }
        result.legacy_lock = std::move(*lock);
    } else if (allowed_attempt_lock) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Attempt-owned catalog lock disappeared"));
    }

    auto archives = retainCatalogNamespaceDirectory(anchor, QByteArrayLiteral("archives"),
                                                    detail::CatalogSubject::ArchivesDirectory,
                                                    writable, operation, hooks);
    if (!archives) {
        return std::unexpected(archives.error());
    }
    anchor.archives = std::move(*archives);
    result.archives_identity = anchor.archives->identity;
    auto archive_names = catalogDirectoryNames(
        anchor.archives->descriptor.get(), aggregate_count, anchor.archives->absolute_path,
        detail::CatalogSubject::ArchivesDirectory, operation, hooks);
    if (!archive_names) {
        return std::unexpected(archive_names.error());
    }
    if (!catalogIdentityRebound(anchor.rootDescriptor(), anchor.archives->component,
                                anchor.archives->descriptor.get(), anchor.archives->identity,
                                true)) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Archive namespace changed during inventory"));
    }
    if (!catalogDirectoryAclsAbsent(anchor.archives->descriptor.get())) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Archive namespace ACL appeared during inventory"));
    }
    auto archive_objects =
        retainCatalogObjects(*anchor.archives, std::move(*archive_names), true, operation, hooks);
    if (!archive_objects) {
        return std::unexpected(archive_objects.error());
    }
    result.archives = std::move(*archive_objects);

    if (result.shape == detail::CatalogNamespaceShape::Current) {
        auto blobs = retainCatalogNamespaceDirectory(anchor, QByteArrayLiteral("blobs"),
                                                     detail::CatalogSubject::BlobsDirectory,
                                                     writable, operation, hooks);
        if (!blobs) {
            return std::unexpected(blobs.error());
        }
        anchor.blobs = std::move(*blobs);
        result.blobs_identity = anchor.blobs->identity;
        auto blob_names = catalogDirectoryNames(
            anchor.blobs->descriptor.get(), aggregate_count, anchor.blobs->absolute_path,
            detail::CatalogSubject::BlobsDirectory, operation, hooks);
        if (!blob_names) {
            return std::unexpected(blob_names.error());
        }
        if (!catalogIdentityRebound(anchor.rootDescriptor(), anchor.blobs->component,
                                    anchor.blobs->descriptor.get(), anchor.blobs->identity, true)) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Blob namespace changed during inventory"));
        }
        if (!catalogDirectoryAclsAbsent(anchor.blobs->descriptor.get())) {
            return fail(CatalogErrorCode::CorruptCatalog,
                        QStringLiteral("Blob namespace ACL appeared during inventory"));
        }
        auto blob_objects =
            retainCatalogObjects(*anchor.blobs, std::move(*blob_names), false, operation, hooks);
        if (!blob_objects) {
            return std::unexpected(blob_objects.error());
        }
        result.blobs = std::move(*blob_objects);
    }

    auto main = retainCatalogFile(anchor.rootDescriptor(), anchor.absolute_root,
                                  QByteArrayLiteral("catalog.sqlite"),
                                  detail::CatalogSubject::DatabaseMain, writable, operation, hooks);
    if (!main) {
        return std::unexpected(main.error());
    }
    if (main->identity.size < 0 ||
        static_cast<std::uint64_t>(main->identity.size) > maximum_catalog_database_bytes) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Catalog database exceeds its admission limit"));
    }
    result.main = std::move(*main);
    if (std::ranges::contains(*root_names, QByteArrayView("catalog.sqlite-wal"))) {
        auto wal = retainCatalogFile(
            anchor.rootDescriptor(), anchor.absolute_root, QByteArrayLiteral("catalog.sqlite-wal"),
            detail::CatalogSubject::DatabaseWal, writable, operation, hooks);
        if (!wal) {
            return std::unexpected(wal.error());
        }
        if (wal->identity.size < 0 ||
            static_cast<std::uint64_t>(wal->identity.size) > maximum_catalog_wal_bytes) {
            return fail(CatalogErrorCode::CorruptCatalog,
                        QStringLiteral("Catalog WAL exceeds its admission limit"));
        }
        result.wal = std::move(*wal);
    }
    if (std::ranges::contains(*root_names, QByteArrayView("catalog.sqlite-shm"))) {
        auto shm = retainCatalogFile(
            anchor.rootDescriptor(), anchor.absolute_root, QByteArrayLiteral("catalog.sqlite-shm"),
            detail::CatalogSubject::DatabaseShm, writable, operation, hooks, false);
        if (!shm) {
            return std::unexpected(shm.error());
        }
        result.shm = std::move(*shm);
    }
    const auto main_size = static_cast<std::uint64_t>(result.main.identity.size);
    const auto wal_size = result.wal ? static_cast<std::uint64_t>(result.wal->identity.size) : 0U;
    if (main_size > maximum_catalog_capture_bytes / 2U ||
        wal_size > maximum_catalog_capture_bytes / 2U ||
        main_size > maximum_catalog_capture_bytes / 2U - wal_size) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Catalog A/B capture exceeds 2 GiB"));
    }
    if (const auto valid = revalidateCatalogAnchor(anchor); !valid) {
        return std::unexpected(valid.error());
    }
    if (!catalogIdentityRebound(anchor.rootDescriptor(), anchor.archives->component,
                                anchor.archives->descriptor.get(), anchor.archives->identity,
                                true) ||
        (anchor.blobs &&
         !catalogIdentityRebound(anchor.rootDescriptor(), anchor.blobs->component,
                                 anchor.blobs->descriptor.get(), anchor.blobs->identity, true))) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Catalog namespace binding changed during admission"));
    }
    return result;
}

[[maybe_unused, nodiscard]] std::expected<void, CatalogError> normalizeRetainedCatalogDirectory(
    CatalogAnchor& anchor, CatalogRetainedDirectory* directory, detail::CatalogSubject subject,
    detail::CatalogOperation operation, const detail::CatalogHooks& hooks) {
    const auto root = directory == nullptr;
    auto& descriptor = root ? anchor.controllers.back().descriptor : directory->descriptor;
    auto& identity = root ? anchor.controllers.back().identity : directory->identity;
    const auto parent_descriptor =
        root ? anchor.controllers.at(anchor.controllers.size() - 2U).descriptor.get()
             : anchor.rootDescriptor();
    const auto& component = root ? anchor.controllers.back().component : directory->component;
    const auto& absolute = root ? anchor.absolute_root : directory->absolute_path;
    if (identity.owner != ::geteuid() || (identity.mode & 0700) != 0700 ||
        (identity.mode & 0022) != 0) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Catalog directory is not writable by its owner"));
    }
    if ((identity.mode & 07777) == 0700) {
        return {};
    }
    detail::CatalogObservation observation;
    observation.event = detail::CatalogEvent::DirectoryNormalized;
    observation.subject = subject;
    observation.operation = operation;
    observation.absolute_path = absolute;
    observation.component = component;
    observation.mode_before = static_cast<unsigned int>(identity.mode & 07777);
    observation.mode_after = 0700;
    observation.identity_before = reportedCatalogIdentity(identity);
    const auto action = catalogAction(hooks, observation);
    if (catalogFailsBefore(action) || ::fchmod(descriptor.get(), 0700) != 0) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Cannot normalize catalog directory"));
    }
    struct stat after{};
    if (::fstat(descriptor.get(), &after) != 0 || after.st_dev != identity.device ||
        after.st_ino != identity.inode || after.st_uid != ::geteuid() ||
        (after.st_mode & 07777) != 0700 ||
        !catalogIdentityRebound(parent_descriptor, component, descriptor.get(),
                                catalogIdentity(after)) ||
        !catalogDirectoryAclsAbsent(descriptor.get()) || !catalogFsync(descriptor.get()) ||
        !catalogFsync(parent_descriptor)) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Catalog directory normalization is incomplete"));
    }
    identity = catalogIdentity(after);
    observation.identity_after = reportedCatalogIdentity(identity);
    if (!catalogFinishes(hooks, observation, action)) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Injected catalog directory normalization failure"));
    }
    struct stat final_status{};
    if (::fstat(descriptor.get(), &final_status) != 0 || final_status.st_dev != identity.device ||
        final_status.st_ino != identity.inode || final_status.st_uid != ::geteuid() ||
        (final_status.st_mode & 07777) != 0700 ||
        !catalogIdentityRebound(parent_descriptor, component, descriptor.get(),
                                catalogIdentity(final_status)) ||
        !catalogDirectoryAclsAbsent(descriptor.get())) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Catalog directory changed after normalization"));
    }
    identity = catalogIdentity(final_status);
    return {};
}

[[nodiscard]] auto normalizeRetainedCatalogFile(CatalogAnchor& anchor, CatalogRetainedFile& file,
                                                detail::CatalogSubject subject,
                                                detail::CatalogOperation operation,
                                                const detail::CatalogHooks& hooks)
    -> std::expected<void, CatalogError> {
    struct stat before{};
    if (::fstat(file.descriptor.get(), &before) != 0 ||
        !sameCatalogIdentity(file.identity, before, false) || !S_ISREG(before.st_mode) ||
        before.st_uid != ::geteuid() || before.st_nlink != 1 || (before.st_mode & 0600) != 0600 ||
        (before.st_mode & 0022) != 0) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Catalog SQLite file is not owner-writable"));
    }
    detail::CatalogObservation normalized;
    normalized.event = detail::CatalogEvent::FileNormalized;
    normalized.subject = subject;
    normalized.operation = operation;
    normalized.absolute_path = file.absolute_path;
    normalized.component = file.component;
    normalized.mode_before = static_cast<unsigned int>(before.st_mode & 07777);
    normalized.mode_after = 0600;
    normalized.identity_before = reportedCatalogIdentity(catalogIdentity(before));
    const auto action = catalogAction(hooks, normalized);
    if (catalogFailsBefore(action)) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Injected catalog-file normalization failure"));
    }
    if ((before.st_mode & 07777) != 0600 && ::fchmod(file.descriptor.get(), 0600) != 0) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Cannot normalize catalog SQLite file"));
    }
    struct stat after{};
    if (::fstat(file.descriptor.get(), &after) != 0 || after.st_dev != before.st_dev ||
        after.st_ino != before.st_ino || after.st_uid != ::geteuid() || after.st_nlink != 1 ||
        (after.st_mode & 07777) != 0600 ||
        !catalogIdentityRebound(anchor.rootDescriptor(), file.component, file.descriptor.get(),
                                catalogIdentity(after), false) ||
        !catalogFileAclAbsent(file.descriptor.get()) || !catalogFsync(file.descriptor.get()) ||
        !catalogFsync(anchor.rootDescriptor())) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Catalog SQLite file normalization is incomplete"));
    }
    file.identity = catalogIdentity(after);
    normalized.identity_after = reportedCatalogIdentity(file.identity);
    if (!catalogFinishes(hooks, normalized, action)) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Injected catalog-file normalization failure"));
    }
    return {};
}

[[nodiscard]] auto normalizeCatalogSqliteFiles(CatalogAnchor& anchor, CatalogNamespace& state,
                                               detail::CatalogOperation operation,
                                               const detail::CatalogHooks& hooks)
    -> std::expected<void, CatalogError> {
    if (const auto normalized = normalizeRetainedCatalogFile(
            anchor, state.main, detail::CatalogSubject::DatabaseMain, operation, hooks);
        !normalized) {
        return normalized;
    }
    if (state.wal) {
        if (const auto normalized = normalizeRetainedCatalogFile(
                anchor, *state.wal, detail::CatalogSubject::DatabaseWal, operation, hooks);
            !normalized) {
            return normalized;
        }
    }
    if (state.shm) {
        if (const auto normalized = normalizeRetainedCatalogFile(
                anchor, *state.shm, detail::CatalogSubject::DatabaseShm, operation, hooks);
            !normalized) {
            return normalized;
        }
    }
    return {};
}

[[nodiscard]] bool removeExactCatalogDirectory(int parent_descriptor, const QByteArray& component,
                                               const CatalogIdentity& identity);

[[maybe_unused, nodiscard]] std::expected<CatalogRetainedDirectory, CatalogError>
createCatalogNamespaceDirectory(CatalogAnchor& anchor, QByteArray component,
                                detail::CatalogSubject subject, detail::CatalogOperation operation,
                                const detail::CatalogHooks& hooks) {
    if (const auto valid = revalidateCatalogAnchor(anchor); !valid) {
        return std::unexpected(valid.error());
    }
    const auto absolute = anchor.absolute_root + u'/' + QString::fromLatin1(component);
    detail::CatalogObservation creation;
    creation.event = detail::CatalogEvent::DirectoryCreated;
    creation.subject = subject;
    creation.operation = operation;
    creation.absolute_path = absolute;
    creation.component = component;
    const auto action = catalogAction(hooks, creation);
    if (catalogFailsBefore(action) ||
        ::mkdirat(anchor.rootDescriptor(), component.constData(), 0000) != 0) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Cannot create catalog namespace directory"));
    }
    CatalogDescriptor retained(::openat(anchor.rootDescriptor(), component.constData(),
                                        O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat created{};
    if (!retained || ::fstat(retained.get(), &created) != 0 || !S_ISDIR(created.st_mode) ||
        created.st_uid != ::geteuid() || (created.st_mode & 07777) != 0000) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Created catalog directory cannot be retained"));
    }
    const auto created_identity = catalogIdentity(created);
    creation.identity_after = reportedCatalogIdentity(created_identity);
    anchor.created_paths.push_back(absolute);
    if (!catalogFinishes(hooks, creation, action)) {
        static_cast<void>(
            removeExactCatalogDirectory(anchor.rootDescriptor(), component, created_identity));
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Injected catalog directory creation failure"));
    }
    auto transitioned =
        retainCreatedCatalogDirectory(anchor.rootDescriptor(), component, absolute, false, subject,
                                      operation, hooks, std::move(retained), created_identity);
    if (!transitioned) {
        return std::unexpected(transitioned.error());
    }
    return CatalogRetainedDirectory{std::move(transitioned->absolute_path),
                                    std::move(transitioned->component),
                                    std::move(transitioned->descriptor), transitioned->identity};
}

[[maybe_unused, nodiscard]] std::expected<CatalogRetainedFile, CatalogError>
createCatalogDatabaseMain(CatalogAnchor& anchor, detail::CatalogOperation operation,
                          const detail::CatalogHooks& hooks) {
    constexpr auto component = "catalog.sqlite";
    const auto absolute = anchor.absolute_root + QStringLiteral("/catalog.sqlite");
    if (const auto valid = revalidateCatalogAnchor(anchor); !valid) {
        return std::unexpected(valid.error());
    }
    detail::CatalogObservation creation;
    creation.event = detail::CatalogEvent::FileCreated;
    creation.subject = detail::CatalogSubject::DatabaseMain;
    creation.operation = operation;
    creation.absolute_path = absolute;
    creation.component = QByteArrayLiteral("catalog.sqlite");
    creation.mode_after = 0600;
    const auto action = catalogAction(hooks, creation);
    CatalogDescriptor descriptor;
    if (!catalogFailsBefore(action)) {
        descriptor.reset(::openat(anchor.rootDescriptor(), component,
                                  O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    }
    struct stat created{};
    if (!descriptor || ::fstat(descriptor.get(), &created) != 0 || !S_ISREG(created.st_mode) ||
        created.st_uid != ::geteuid() || created.st_nlink != 1) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Cannot reserve catalog database"));
    }
    auto identity = catalogIdentity(created);
    creation.mode_before = static_cast<unsigned int>(created.st_mode & 07777);
    creation.identity_after = reportedCatalogIdentity(identity);
    anchor.created_paths.push_back(absolute);
    if (!catalogFinishes(hooks, creation, action)) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Injected catalog database creation failure"));
    }

    detail::CatalogObservation normalized = creation;
    normalized.event = detail::CatalogEvent::FileNormalized;
    normalized.identity_before = reportedCatalogIdentity(identity);
    const auto normalized_action = catalogAction(hooks, normalized);
    if (catalogFailsBefore(normalized_action) || ::fchmod(descriptor.get(), 0600) != 0) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Cannot normalize catalog database"));
    }
    struct stat transitioned{};
    struct stat named{};
    if (::fstat(descriptor.get(), &transitioned) != 0 ||
        ::fstatat(anchor.rootDescriptor(), component, &named, AT_SYMLINK_NOFOLLOW) != 0 ||
        transitioned.st_dev != created.st_dev || transitioned.st_ino != created.st_ino ||
        named.st_dev != created.st_dev || named.st_ino != created.st_ino ||
        transitioned.st_uid != ::geteuid() || transitioned.st_nlink != 1 ||
        (transitioned.st_mode & 07777) != 0600 || !catalogFileAclAbsent(descriptor.get()) ||
        !catalogFsync(descriptor.get()) || !catalogFsync(anchor.rootDescriptor())) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Catalog database normalization is incomplete"));
    }
    identity = catalogIdentity(transitioned);
    normalized.identity_after = reportedCatalogIdentity(identity);
    if (!catalogFinishes(hooks, normalized, normalized_action) ||
        !catalogIdentityRebound(anchor.rootDescriptor(), QByteArrayLiteral("catalog.sqlite"),
                                descriptor.get(), identity, true)) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Catalog database changed after normalization"));
    }
    return CatalogRetainedFile{absolute, QByteArrayLiteral("catalog.sqlite"), std::move(descriptor),
                               identity};
}

[[maybe_unused, nodiscard]] std::optional<CatalogIdentity>
nativeCatalogIdentity(const detail::CatalogIdentity& identity) {
    if (identity.type != detail::CatalogNodeType::RegularFile || identity.link_count != 1 ||
        identity.byte_size > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
        identity.modification_nanoseconds < 0 ||
        identity.modification_nanoseconds >= 1'000'000'000 || identity.change_nanoseconds < 0 ||
        identity.change_nanoseconds >= 1'000'000'000) {
        return std::nullopt;
    }
    const auto device = static_cast<dev_t>(identity.device);
    const auto inode = static_cast<ino_t>(identity.inode);
    const auto links = static_cast<nlink_t>(identity.link_count);
    const auto owner = static_cast<uid_t>(identity.owner);
    const auto modified_seconds = static_cast<time_t>(identity.modification_seconds);
    const auto changed_seconds = static_cast<time_t>(identity.change_seconds);
    if (static_cast<std::uint64_t>(device) != identity.device ||
        static_cast<std::uint64_t>(inode) != identity.inode ||
        static_cast<std::uint64_t>(links) != identity.link_count ||
        static_cast<std::uint64_t>(owner) != identity.owner ||
        static_cast<std::int64_t>(modified_seconds) != identity.modification_seconds ||
        static_cast<std::int64_t>(changed_seconds) != identity.change_seconds) {
        return std::nullopt;
    }
    CatalogIdentity result;
    result.device = device;
    result.inode = inode;
    result.mode = static_cast<mode_t>(S_IFREG | identity.mode);
    result.links = links;
    result.owner = owner;
    result.size = static_cast<off_t>(identity.byte_size);
    result.modified =
        timespec{modified_seconds, static_cast<long>(identity.modification_nanoseconds)};
    result.changed = timespec{changed_seconds, static_cast<long>(identity.change_nanoseconds)};
    return result;
}
#endif

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

#if defined(Q_OS_LINUX)
[[nodiscard]] auto checkpointCatalog(QSqlDatabase& database, CatalogErrorCode error_code,
                                     detail::CatalogOperation operation,
                                     const detail::CatalogHooks& hooks)
    -> std::expected<void, CatalogError> {
    detail::CatalogObservation observation;
    observation.event = detail::CatalogEvent::CheckpointExecuted;
    observation.subject = detail::CatalogSubject::SQLiteConnection;
    observation.operation = operation;
    const auto action = catalogAction(hooks, observation);
    if (catalogFailsBefore(action)) {
        return fail(error_code, QStringLiteral("Injected catalog checkpoint failure"));
    }
    QSqlQuery checkpoint(database);
    checkpoint.setForwardOnly(true);
    if (!checkpoint.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)")) ||
        checkpoint.record().count() != 3 || !checkpoint.next()) {
        return queryFailure(error_code, checkpoint, QStringLiteral("checkpoint catalog WAL"));
    }
    std::array<qlonglong, 3> values{};
    for (int index = 0; index < 3; ++index) {
        bool converted = false;
        values.at(static_cast<std::size_t>(index)) = checkpoint.value(index).toLongLong(&converted);
        if (!converted || values.at(static_cast<std::size_t>(index)) < 0) {
            return fail(error_code,
                        QStringLiteral("Catalog checkpoint returned a non-integer result"));
        }
    }
    if (values.at(0) != 0 || checkpoint.next()) {
        return fail(error_code, QStringLiteral("Catalog checkpoint remains busy"));
    }
    checkpoint.finish();
    if (!catalogFinishes(hooks, observation, action)) {
        return fail(error_code, QStringLiteral("Injected catalog checkpoint failure"));
    }
    return {};
}
#endif

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

#if defined(Q_OS_LINUX)
struct CatalogScratchDirectory final {
    QString absolute_path;
    QByteArray component;
    int parent_descriptor{-1};
    CatalogDescriptor descriptor;
    CatalogIdentity identity;
    bool removed{};
};

struct CatalogScratchFile final {
    QString absolute_path;
    QByteArray component;
    int parent_descriptor{-1};
    CatalogDescriptor descriptor;
    CatalogIdentity identity;
    detail::CatalogSubject subject{detail::CatalogSubject::None};
    bool may_disappear{};
    bool removed{};
};

struct CatalogScratchCopy final {
    std::size_t file_index{};
    QByteArray sha256;
    std::uint64_t byte_count{};
};

[[nodiscard]] bool removeExactCatalogDirectory(int parent_descriptor, const QByteArray& component,
                                               const CatalogIdentity& identity) {
    struct stat named{};
    if (::fstatat(parent_descriptor, component.constData(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
        !sameCatalogIdentity(identity, named, false) || !S_ISDIR(named.st_mode)) {
        return false;
    }
    if (::unlinkat(parent_descriptor, component.constData(), AT_REMOVEDIR) != 0) {
        return false;
    }
    return catalogFsync(parent_descriptor);
}

[[nodiscard]] bool removeAttemptOwnedCatalogFile(int parent_descriptor,
                                                 const CatalogRetainedFile& retained) {
    struct stat held{};
    struct stat named{};
    if (::fstat(retained.descriptor.get(), &held) != 0 ||
        ::fstatat(parent_descriptor, retained.component.constData(), &named, AT_SYMLINK_NOFOLLOW) !=
            0 ||
        !sameCatalogIdentity(retained.identity, held, false) ||
        !sameCatalogIdentity(retained.identity, named, false) || !S_ISREG(held.st_mode) ||
        held.st_uid != ::geteuid() || held.st_nlink != 1 || (held.st_mode & 0022) != 0 ||
        (held.st_mode & 0600) != 0600 || !catalogFileAclAbsent(retained.descriptor.get()) ||
        ::unlinkat(parent_descriptor, retained.component.constData(), 0) != 0) {
        return false;
    }
    return catalogFsync(parent_descriptor);
}

[[nodiscard]] QByteArray productionCatalogScratchName() {
    return QByteArrayLiteral("appellate-catalog-") +
           QUuid::createUuid().toString(QUuid::Id128).toLatin1();
}

[[nodiscard]] auto hashCatalogDescriptor(int descriptor, std::uint64_t byte_count)
    -> std::expected<QByteArray, CatalogError> {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::array<char, copy_buffer_bytes> buffer{};
    std::uint64_t offset = 0;
    while (offset < byte_count) {
        const auto requested =
            static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), byte_count - offset));
        ssize_t count{};
        do {
            count = ::pread(descriptor, buffer.data(), requested, static_cast<off_t>(offset));
        } while (count < 0 && errno == EINTR);
        if (count != static_cast<ssize_t>(requested)) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Cannot hash complete retained catalog file"));
        }
        hash.addData(QByteArrayView(buffer.data(), static_cast<qsizetype>(count)));
        offset += static_cast<std::uint64_t>(count);
    }
    return hash.result().toHex();
}

class CatalogScratchArena final {
  public:
    CatalogScratchArena(const CatalogScratchArena&) = delete;
    CatalogScratchArena& operator=(const CatalogScratchArena&) = delete;
    CatalogScratchArena(CatalogScratchArena&&) = delete;
    CatalogScratchArena& operator=(CatalogScratchArena&&) = delete;
    ~CatalogScratchArena() { static_cast<void>(cleanup()); }

    [[nodiscard]] static auto
    create(detail::SecureScratchContext& context, detail::CatalogOperation operation,
           detail::CatalogHooks hooks,
           detail::CatalogSubject root_subject = detail::CatalogSubject::LiveSnapshotWorkspace)
        -> std::expected<std::unique_ptr<CatalogScratchArena>, CatalogError> {
        if (!context.isValid()) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Secure scratch context was consumed"));
        }
        const auto validated = context.validateRetainedControllers();
        const auto parent = context.parentDescriptor();
        if (!validated || !parent) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Secure scratch controller binding changed"));
        }
        auto arena = std::unique_ptr<CatalogScratchArena>(
            new CatalogScratchArena(context, operation, std::move(hooks), *parent, root_subject));
        for (std::size_t attempt = 0; attempt < 128; ++attempt) {
            auto component = arena->hooks_.name_source ? arena->hooks_.name_source(attempt)
                                                       : productionCatalogScratchName();
            if (!detail::isValidSecureScratchName(component)) {
                return fail(CatalogErrorCode::CannotOpen,
                            QStringLiteral("Private catalog workspace name is invalid"));
            }
            const auto absolute =
                context.absoluteParent() == QStringLiteral("/")
                    ? QStringLiteral("/") + QString::fromLatin1(component)
                    : context.absoluteParent() + u'/' + QString::fromLatin1(component);
            detail::CatalogObservation creation;
            creation.event = detail::CatalogEvent::DirectoryCreated;
            creation.subject = root_subject;
            creation.operation = operation;
            creation.absolute_path = absolute;
            creation.component = component;
            creation.ordinal = attempt;
            const auto action = catalogAction(arena->hooks_, creation);
            if (catalogFailsBefore(action)) {
                return fail(CatalogErrorCode::CannotOpen,
                            QStringLiteral("Injected private-workspace creation failure"));
            }
            if (::mkdirat(*parent, component.constData(), 0000) != 0) {
                if (errno == EEXIST) {
                    continue;
                }
                return fail(CatalogErrorCode::CannotOpen,
                            QStringLiteral("Cannot create private catalog workspace"));
            }
            CatalogDescriptor retained(::openat(*parent, component.constData(),
                                                O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            struct stat created{};
            if (!retained || ::fstat(retained.get(), &created) != 0 || !S_ISDIR(created.st_mode) ||
                created.st_uid != ::geteuid() || (created.st_mode & 07777) != 0000) {
                return fail(CatalogErrorCode::CorruptCatalog,
                            QStringLiteral("Private workspace creation cannot be retained"));
            }
            const auto created_identity = catalogIdentity(created);
            creation.identity_after = reportedCatalogIdentity(created_identity);
            if (!catalogFinishes(arena->hooks_, creation, action)) {
                static_cast<void>(
                    removeExactCatalogDirectory(*parent, component, created_identity));
                return fail(CatalogErrorCode::CannotOpen,
                            QStringLiteral("Injected private-workspace creation failure"));
            }
            auto cleanup_identity = created_identity;
            auto transitioned = retainCreatedCatalogDirectory(
                *parent, component, absolute, false, root_subject, operation, arena->hooks_,
                std::move(retained), created_identity, &cleanup_identity);
            if (!transitioned) {
                static_cast<void>(
                    removeExactCatalogDirectory(*parent, component, cleanup_identity));
                return std::unexpected(transitioned.error());
            }
            arena->root_ = CatalogScratchDirectory{absolute,
                                                   component,
                                                   *parent,
                                                   std::move(transitioned->descriptor),
                                                   transitioned->identity,
                                                   false};
            if (root_subject == detail::CatalogSubject::LiveSnapshotWorkspace &&
                arena->hooks_.report != nullptr) {
                arena->hooks_.report->live_snapshot_workspace = absolute;
            }
            return arena;
        }
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Private catalog workspace name collisions exceeded 128"));
    }

    [[nodiscard]] const QString& rootPath() const noexcept { return root_.absolute_path; }
    [[nodiscard]] int rootDescriptor() const noexcept { return root_.descriptor.get(); }
    void rebindContext(detail::SecureScratchContext& context) noexcept { context_ = &context; }

    [[nodiscard]] auto validateForAmbientUse() const -> std::expected<void, CatalogError> {
        if (context_ == nullptr || !context_->validateRetainedControllers()) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Secure scratch controller binding changed"));
        }
        const auto parent = context_->parentDescriptor();
        struct stat held{};
        struct stat named{};
        if (!parent || *parent != root_.parent_descriptor ||
            ::fstat(root_.descriptor.get(), &held) != 0 ||
            ::fstatat(root_.parent_descriptor, root_.component.constData(), &named,
                      AT_SYMLINK_NOFOLLOW) != 0 ||
            !sameCatalogIdentity(root_.identity, held, false) || held.st_dev != named.st_dev ||
            held.st_ino != named.st_ino || !catalogNamespaceDirectoryPolicy(held, true) ||
            !catalogDirectoryAclsAbsent(root_.descriptor.get())) {
            return fail(CatalogErrorCode::CorruptCatalog,
                        QStringLiteral("Private catalog workspace binding changed"));
        }
        return {};
    }

    [[nodiscard]] auto
    validateSqliteDirectoryForAmbientUse(std::optional<std::size_t> directory_index) const
        -> std::expected<void, CatalogError> {
        if (const auto valid = validateForAmbientUse(); !valid) {
            return valid;
        }
        const auto parent_descriptor = directoryDescriptor(directory_index);
        if (directory_index) {
            const auto& directory = directories_.at(*directory_index);
            struct stat held{};
            struct stat named{};
            if (directory.removed || ::fstat(directory.descriptor.get(), &held) != 0 ||
                ::fstatat(directory.parent_descriptor, directory.component.constData(), &named,
                          AT_SYMLINK_NOFOLLOW) != 0 ||
                !sameCatalogIdentity(directory.identity, held, false) ||
                held.st_dev != named.st_dev || held.st_ino != named.st_ino ||
                !catalogNamespaceDirectoryPolicy(held, true) ||
                !catalogDirectoryAclsAbsent(directory.descriptor.get())) {
                return fail(CatalogErrorCode::CorruptCatalog,
                            QStringLiteral("Private SQLite directory binding changed"));
            }
        }

        std::size_t count = 0;
        const auto names = readCatalogDirectoryNames(parent_descriptor, count);
        if (!names) {
            return std::unexpected(names.error());
        }
        for (const auto& name : *names) {
            if (name != QByteArrayView("catalog.sqlite") &&
                name != QByteArrayView("catalog.sqlite-wal") &&
                name != QByteArrayView("catalog.sqlite-shm")) {
                return fail(CatalogErrorCode::CorruptCatalog,
                            QStringLiteral("Private SQLite directory has an unexpected member"));
            }
            const auto tracked = std::ranges::find_if(files_, [&](const CatalogScratchFile& file) {
                return !file.removed && file.parent_descriptor == parent_descriptor &&
                       file.component == name;
            });
            struct stat held{};
            struct stat named{};
            if (tracked == files_.end() || ::fstat(tracked->descriptor.get(), &held) != 0 ||
                ::fstatat(parent_descriptor, name.constData(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
                !sameCatalogIdentity(tracked->identity, held, true) ||
                !sameCatalogIdentity(tracked->identity, named, true) || !S_ISREG(held.st_mode) ||
                held.st_uid != ::geteuid() || held.st_nlink != 1 ||
                (held.st_mode & 07777) != 0600 ||
                !catalogFileAclAbsent(tracked->descriptor.get())) {
                return fail(CatalogErrorCode::CorruptCatalog,
                            QStringLiteral("Private SQLite file binding changed"));
            }
        }
        for (const auto& file : files_) {
            if (!file.removed && file.parent_descriptor == parent_descriptor &&
                !std::ranges::contains(*names, file.component)) {
                return fail(CatalogErrorCode::CorruptCatalog,
                            QStringLiteral("Private SQLite file disappeared"));
            }
        }
        return {};
    }

    [[nodiscard]] auto createDirectory(QByteArray component, detail::CatalogSubject subject)
        -> std::expected<std::size_t, CatalogError> {
        if (component.isEmpty() || component.contains('/') || component == QByteArrayView(".") ||
            component == QByteArrayView("..")) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Private catalog directory name is invalid"));
        }
        const auto absolute = root_.absolute_path + u'/' + QString::fromLatin1(component);
        detail::CatalogObservation creation;
        creation.event = detail::CatalogEvent::DirectoryCreated;
        creation.subject = subject;
        creation.operation = operation_;
        creation.absolute_path = absolute;
        creation.component = component;
        const auto action = catalogAction(hooks_, creation);
        if (catalogFailsBefore(action) ||
            ::mkdirat(root_.descriptor.get(), component.constData(), 0000) != 0) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Cannot create private catalog directory"));
        }
        CatalogDescriptor retained(::openat(root_.descriptor.get(), component.constData(),
                                            O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        struct stat created{};
        if (!retained || ::fstat(retained.get(), &created) != 0 || !S_ISDIR(created.st_mode) ||
            created.st_uid != ::geteuid() || (created.st_mode & 07777) != 0000) {
            return fail(CatalogErrorCode::CorruptCatalog,
                        QStringLiteral("Private catalog directory cannot be retained"));
        }
        const auto identity = catalogIdentity(created);
        creation.identity_after = reportedCatalogIdentity(identity);
        if (!catalogFinishes(hooks_, creation, action)) {
            static_cast<void>(
                removeExactCatalogDirectory(root_.descriptor.get(), component, identity));
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Injected private-directory creation failure"));
        }
        auto cleanup_identity = identity;
        auto transitioned = retainCreatedCatalogDirectory(
            root_.descriptor.get(), component, absolute, false, subject, operation_, hooks_,
            std::move(retained), identity, &cleanup_identity);
        if (!transitioned) {
            static_cast<void>(
                removeExactCatalogDirectory(root_.descriptor.get(), component, cleanup_identity));
            return std::unexpected(transitioned.error());
        }
        directories_.push_back(CatalogScratchDirectory{absolute, component, root_.descriptor.get(),
                                                       std::move(transitioned->descriptor),
                                                       transitioned->identity, false});
        return directories_.size() - 1U;
    }

    [[nodiscard]] int directoryDescriptor(std::optional<std::size_t> index) const {
        return index ? directories_.at(*index).descriptor.get() : root_.descriptor.get();
    }

    [[nodiscard]] QString directoryPath(std::optional<std::size_t> index) const {
        return index ? directories_.at(*index).absolute_path : root_.absolute_path;
    }

    [[nodiscard]] auto createFile(std::optional<std::size_t> directory_index, QByteArray component,
                                  detail::CatalogSubject subject)
        -> std::expected<std::size_t, CatalogError> {
        const auto parent_descriptor = directoryDescriptor(directory_index);
        const auto parent_path = directoryPath(directory_index);
        const auto absolute = parent_path + u'/' + QString::fromLatin1(component);
        detail::CatalogObservation creation;
        creation.event = detail::CatalogEvent::FileCreated;
        creation.subject = subject;
        creation.operation = operation_;
        creation.absolute_path = absolute;
        creation.component = component;
        const auto action = catalogAction(hooks_, creation);
        if (catalogFailsBefore(action)) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Injected private-file creation failure"));
        }
        CatalogDescriptor descriptor(::openat(parent_descriptor, component.constData(),
                                              O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                                              0600));
        struct stat created{};
        if (!descriptor || ::fstat(descriptor.get(), &created) != 0 || !S_ISREG(created.st_mode) ||
            created.st_uid != ::geteuid() || created.st_nlink != 1) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Private catalog file cannot be retained"));
        }
        files_.push_back(CatalogScratchFile{absolute, component, parent_descriptor,
                                            std::move(descriptor), catalogIdentity(created),
                                            subject, false, false});
        auto& file = files_.back();
        creation.identity_after = reportedCatalogIdentity(file.identity);
        if (!catalogFinishes(hooks_, creation, action)) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Injected private-file creation failure"));
        }

        detail::CatalogObservation normalized;
        normalized.event = detail::CatalogEvent::FileNormalized;
        normalized.subject = subject;
        normalized.operation = operation_;
        normalized.absolute_path = absolute;
        normalized.component = component;
        normalized.mode_before = static_cast<unsigned int>(created.st_mode & 07777);
        normalized.mode_after = 0600;
        normalized.identity_before = reportedCatalogIdentity(file.identity);
        const auto normalized_action = catalogAction(hooks_, normalized);
        if (catalogFailsBefore(normalized_action) || ::fchmod(file.descriptor.get(), 0600) != 0) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Cannot normalize private catalog file"));
        }
        struct stat transitioned{};
        if (::fstat(file.descriptor.get(), &transitioned) != 0 ||
            transitioned.st_dev != created.st_dev || transitioned.st_ino != created.st_ino ||
            transitioned.st_uid != ::geteuid() || transitioned.st_nlink != 1 ||
            (transitioned.st_mode & 07777) != 0600 ||
            !catalogIdentityRebound(parent_descriptor, component, file.descriptor.get(),
                                    catalogIdentity(transitioned)) ||
            !catalogFileAclAbsent(file.descriptor.get()) || !catalogFsync(file.descriptor.get()) ||
            !catalogFsync(parent_descriptor)) {
            return fail(CatalogErrorCode::CorruptCatalog,
                        QStringLiteral("Private catalog file transition is unsafe"));
        }
        file.identity = catalogIdentity(transitioned);
        normalized.identity_after = reportedCatalogIdentity(file.identity);
        if (!catalogFinishes(hooks_, normalized, normalized_action)) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Injected private-file normalization failure"));
        }
        return files_.size() - 1U;
    }

    [[nodiscard]] auto
    copyRetainedFile(int source_parent_descriptor, const CatalogRetainedFile& source,
                     std::optional<std::size_t> destination_directory,
                     detail::CatalogSubject destination_subject, std::size_t pass)
        -> std::expected<CatalogScratchCopy, CatalogError> {
        auto destination = createFile(destination_directory, source.component, destination_subject);
        if (!destination) {
            return std::unexpected(destination.error());
        }
        auto& output = files_.at(*destination);
        CatalogDescriptor input(::openat(source_parent_descriptor, source.component.constData(),
                                         O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
        struct stat before{};
        if (!input || ::fstat(input.get(), &before) != 0 ||
            !sameCatalogIdentity(source.identity, before, true)) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Catalog source file changed before private copy"));
        }
        if (!catalogFileAclAbsent(input.get())) {
            return fail(CatalogErrorCode::CorruptCatalog,
                        QStringLiteral("Catalog source file ACL appeared before private copy"));
        }
        detail::CatalogObservation copied;
        copied.event = detail::CatalogEvent::CaptureFileCopied;
        copied.subject = destination_subject;
        copied.operation = operation_;
        copied.absolute_path = output.absolute_path;
        copied.component = source.component;
        copied.pass = pass;
        copied.byte_count = before.st_size < 0 ? 0U : static_cast<std::uint64_t>(before.st_size);
        const auto action = catalogAction(hooks_, copied);
        if (catalogFailsBefore(action)) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Injected private catalog copy failure"));
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        std::array<char, copy_buffer_bytes> buffer{};
        std::uint64_t offset = 0;
        while (offset < copied.byte_count) {
            const auto requested = static_cast<std::size_t>(
                std::min<std::uint64_t>(buffer.size(), copied.byte_count - offset));
            ssize_t read_count{};
            do {
                read_count =
                    ::pread(input.get(), buffer.data(), requested, static_cast<off_t>(offset));
            } while (read_count < 0 && errno == EINTR);
            if (read_count <= 0) {
                return fail(CatalogErrorCode::CannotOpen,
                            QStringLiteral("Cannot copy complete catalog source file"));
            }
            std::size_t written = 0;
            while (written < static_cast<std::size_t>(read_count)) {
                ssize_t write_count{};
                do {
                    write_count = ::pwrite(output.descriptor.get(), buffer.data() + written,
                                           static_cast<std::size_t>(read_count) - written,
                                           static_cast<off_t>(offset + written));
                } while (write_count < 0 && errno == EINTR);
                if (write_count <= 0) {
                    return fail(CatalogErrorCode::CannotOpen,
                                QStringLiteral("Cannot write complete private catalog copy"));
                }
                written += static_cast<std::size_t>(write_count);
            }
            hash.addData(QByteArrayView(buffer.data(), static_cast<qsizetype>(read_count)));
            offset += static_cast<std::uint64_t>(read_count);
        }
        struct stat source_after{};
        struct stat output_after{};
        if (::fstat(input.get(), &source_after) != 0 ||
            !sameCatalogIdentity(source.identity, source_after, true) ||
            ::fstat(output.descriptor.get(), &output_after) != 0 ||
            output_after.st_size != before.st_size || !catalogFsync(output.descriptor.get()) ||
            !catalogFsync(output.parent_descriptor)) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Catalog source changed during private copy"));
        }
        output.identity = catalogIdentity(output_after);
        if (hooks_.report != nullptr) {
            hooks_.report->allocations.copied_bytes += copied.byte_count;
        }
        if (!catalogFinishes(hooks_, copied, action)) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Injected private catalog copy failure"));
        }
        const auto expected_hash = hash.result().toHex();
        struct stat source_final{};
        struct stat source_named{};
        struct stat output_final{};
        struct stat output_named{};
        const auto source_hash = hashCatalogDescriptor(input.get(), copied.byte_count);
        const auto output_hash = hashCatalogDescriptor(output.descriptor.get(), copied.byte_count);
        if (::fstat(input.get(), &source_final) != 0 ||
            ::fstatat(source_parent_descriptor, source.component.constData(), &source_named,
                      AT_SYMLINK_NOFOLLOW) != 0 ||
            !sameCatalogIdentity(source.identity, source_final, true) ||
            !sameCatalogIdentity(source.identity, source_named, true) ||
            ::fstat(output.descriptor.get(), &output_final) != 0 ||
            ::fstatat(output.parent_descriptor, output.component.constData(), &output_named,
                      AT_SYMLINK_NOFOLLOW) != 0 ||
            !sameCatalogIdentity(output.identity, output_final, true) ||
            !sameCatalogIdentity(output.identity, output_named, true) || !source_hash ||
            !output_hash || *source_hash != expected_hash || *output_hash != expected_hash) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Catalog source changed after private copy"));
        }
        if (!catalogFileAclAbsent(input.get()) || !catalogFileAclAbsent(output.descriptor.get())) {
            return fail(CatalogErrorCode::CorruptCatalog,
                        QStringLiteral("Catalog file ACL appeared after private copy"));
        }
        return CatalogScratchCopy{*destination, expected_hash, copied.byte_count};
    }

    [[nodiscard]] auto compareCopies(const CatalogScratchCopy& left,
                                     const CatalogScratchCopy& right) const
        -> std::expected<void, CatalogError> {
        if (left.byte_count != right.byte_count || left.sha256 != right.sha256) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Catalog private capture passes differ"));
        }
        const auto& first = files_.at(left.file_index);
        const auto& second = files_.at(right.file_index);
        struct stat first_before{};
        struct stat second_before{};
        if (::fstat(first.descriptor.get(), &first_before) != 0 ||
            ::fstat(second.descriptor.get(), &second_before) != 0 ||
            !sameCatalogIdentity(first.identity, first_before, true) ||
            !sameCatalogIdentity(second.identity, second_before, true) ||
            !catalogIdentityRebound(first.parent_descriptor, first.component,
                                    first.descriptor.get(), first.identity, true) ||
            !catalogIdentityRebound(second.parent_descriptor, second.component,
                                    second.descriptor.get(), second.identity, true)) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Catalog private capture binding changed"));
        }
        const auto first_hash = hashCatalogDescriptor(first.descriptor.get(), left.byte_count);
        const auto second_hash = hashCatalogDescriptor(second.descriptor.get(), right.byte_count);
        if (!first_hash || !second_hash || *first_hash != left.sha256 ||
            *second_hash != right.sha256 || *first_hash != *second_hash) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Catalog private capture hash changed"));
        }
        std::array<char, copy_buffer_bytes> first_buffer{};
        std::array<char, copy_buffer_bytes> second_buffer{};
        std::uint64_t offset = 0;
        while (offset < left.byte_count) {
            const auto requested = static_cast<std::size_t>(
                std::min<std::uint64_t>(first_buffer.size(), left.byte_count - offset));
            const auto first_read = ::pread(first.descriptor.get(), first_buffer.data(), requested,
                                            static_cast<off_t>(offset));
            const auto second_read = ::pread(second.descriptor.get(), second_buffer.data(),
                                             requested, static_cast<off_t>(offset));
            if (first_read != static_cast<ssize_t>(requested) || second_read != first_read ||
                std::memcmp(first_buffer.data(), second_buffer.data(), requested) != 0) {
                return fail(CatalogErrorCode::CannotOpen,
                            QStringLiteral("Catalog private capture bytes differ"));
            }
            offset += requested;
        }
        struct stat first_after{};
        struct stat second_after{};
        if (::fstat(first.descriptor.get(), &first_after) != 0 ||
            ::fstat(second.descriptor.get(), &second_after) != 0 ||
            !sameCatalogIdentity(first.identity, first_after, true) ||
            !sameCatalogIdentity(second.identity, second_after, true) ||
            !catalogIdentityRebound(first.parent_descriptor, first.component,
                                    first.descriptor.get(), first.identity, true) ||
            !catalogIdentityRebound(second.parent_descriptor, second.component,
                                    second.descriptor.get(), second.identity, true)) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Catalog private capture changed during comparison"));
        }
        return {};
    }

    [[nodiscard]] const CatalogScratchFile& file(std::size_t index) const {
        return files_.at(index);
    }

    [[nodiscard]] bool fileNamePresent(std::size_t index) const {
        const auto& candidate = files_.at(index);
        struct stat named{};
        return !candidate.removed &&
               ::fstatat(candidate.parent_descriptor, candidate.component.constData(), &named,
                         AT_SYMLINK_NOFOLLOW) == 0 &&
               named.st_dev == candidate.identity.device &&
               named.st_ino == candidate.identity.inode;
    }

    [[nodiscard]] auto moveFileToRoot(std::size_t index, QByteArray destination_name)
        -> std::expected<void, CatalogError> {
        auto& file = files_.at(index);
        if (file.removed) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Cannot promote captured catalog database"));
        }
        const auto old_parent = file.parent_descriptor;
        if (::syscall(SYS_renameat2, old_parent, file.component.constData(), root_.descriptor.get(),
                      destination_name.constData(), RENAME_NOREPLACE) != 0) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Cannot promote captured catalog database"));
        }
        // Rename is the state transition: update the ledger before any later fallible durability
        // operation so reconciliation never addresses the obsolete source name.
        file.parent_descriptor = root_.descriptor.get();
        file.component = std::move(destination_name);
        file.absolute_path = root_.absolute_path + u'/' + QString::fromLatin1(file.component);
        struct stat moved{};
        struct stat named{};
        if (::fstat(file.descriptor.get(), &moved) != 0 ||
            ::fstatat(file.parent_descriptor, file.component.constData(), &named,
                      AT_SYMLINK_NOFOLLOW) != 0 ||
            moved.st_dev != file.identity.device || moved.st_ino != file.identity.inode ||
            moved.st_mode != file.identity.mode || moved.st_nlink != file.identity.links ||
            moved.st_uid != file.identity.owner || moved.st_size != file.identity.size ||
            named.st_dev != moved.st_dev || named.st_ino != moved.st_ino) {
            preserve_ = true;
            return fail(CatalogErrorCode::CorruptCatalog,
                        QStringLiteral("Promoted catalog copy changed identity"));
        }
        file.identity = catalogIdentity(moved);
        if (!catalogFsync(old_parent) || !catalogFsync(root_.descriptor.get()) ||
            !catalogIdentityRebound(file.parent_descriptor, file.component, file.descriptor.get(),
                                    file.identity, true)) {
            return fail(CatalogErrorCode::CorruptCatalog,
                        QStringLiteral("Promoted catalog copy changed identity"));
        }
        return {};
    }

    [[nodiscard]] auto trackExistingFile(std::optional<std::size_t> directory_index,
                                         QByteArray component, detail::CatalogSubject subject,
                                         bool may_disappear)
        -> std::expected<std::size_t, CatalogError> {
        const auto parent_descriptor = directoryDescriptor(directory_index);
        const auto parent_path = directoryPath(directory_index);
        auto retained = retainCatalogFile(parent_descriptor, parent_path, component, subject, true,
                                          operation_, hooks_);
        if (!retained || retained->identity.owner != ::geteuid() ||
            (retained->identity.mode & 07777) != 0600) {
            return fail(CatalogErrorCode::CorruptCatalog,
                        QStringLiteral("Private SQLite sidecar is unsafe"));
        }
        files_.push_back(CatalogScratchFile{retained->absolute_path, retained->component,
                                            parent_descriptor, std::move(retained->descriptor),
                                            retained->identity, subject, may_disappear, false});
        return files_.size() - 1U;
    }

    [[nodiscard]] auto admitSqliteDirectory(std::optional<std::size_t> directory_index,
                                            bool allow_main_transition = false)
        -> std::expected<void, CatalogError> {
        const auto parent_descriptor = directoryDescriptor(directory_index);
        const auto parent_path = directoryPath(directory_index);
        std::size_t count = 0;
        const auto names = readCatalogDirectoryNames(parent_descriptor, count);
        if (!names) {
            return std::unexpected(names.error());
        }
        for (const auto& name : *names) {
            detail::CatalogSubject subject;
            if (name == QByteArrayView("catalog.sqlite")) {
                subject = detail::CatalogSubject::PrivateDatabaseMain;
            } else if (name == QByteArrayView("catalog.sqlite-wal")) {
                subject = detail::CatalogSubject::PrivateDatabaseWal;
            } else if (name == QByteArrayView("catalog.sqlite-shm")) {
                subject = detail::CatalogSubject::PrivateDatabaseShm;
            } else {
                preserve_ = true;
                return fail(CatalogErrorCode::CorruptCatalog,
                            QStringLiteral("Private SQLite directory has an unexpected member"));
            }
            const auto tracked = std::ranges::find_if(files_, [&](const CatalogScratchFile& file) {
                return !file.removed && file.parent_descriptor == parent_descriptor &&
                       file.component == name;
            });
            if (tracked != files_.end()) {
                struct stat held{};
                struct stat named{};
                if (::fstat(tracked->descriptor.get(), &held) != 0 ||
                    ::fstatat(parent_descriptor, name.constData(), &named, AT_SYMLINK_NOFOLLOW) !=
                        0 ||
                    held.st_dev != named.st_dev || held.st_ino != named.st_ino ||
                    !S_ISREG(held.st_mode) || held.st_uid != ::geteuid() || held.st_nlink != 1 ||
                    (held.st_mode & 07777) != 0600 ||
                    !catalogFileAclAbsent(tracked->descriptor.get()) ||
                    (subject == detail::CatalogSubject::PrivateDatabaseMain &&
                     !allow_main_transition &&
                     !sameCatalogIdentity(tracked->identity, held, true))) {
                    preserve_ = true;
                    return fail(CatalogErrorCode::CorruptCatalog,
                                QStringLiteral("Tracked private SQLite member is unsafe"));
                }
                tracked->identity = catalogIdentity(held);
                tracked->may_disappear = subject != detail::CatalogSubject::PrivateDatabaseMain;
                continue;
            }

            CatalogDescriptor path_descriptor(
                ::openat(parent_descriptor, name.constData(), O_PATH | O_NOFOLLOW | O_CLOEXEC));
            struct stat provisional{};
            if (!path_descriptor || ::fstat(path_descriptor.get(), &provisional) != 0 ||
                !S_ISREG(provisional.st_mode) || provisional.st_uid != ::geteuid() ||
                provisional.st_nlink != 1 || (provisional.st_mode & 0022) != 0) {
                preserve_ = true;
                return fail(CatalogErrorCode::CorruptCatalog,
                            QStringLiteral("Private SQLite member is unsafe"));
            }
            const auto provisional_identity = catalogIdentity(provisional);
            if ((provisional.st_mode & 07777) != 0600 &&
                !catalogDescriptorChmodAtEmptyPath(path_descriptor.get(), 0600)) {
                preserve_ = true;
                return fail(CatalogErrorCode::CannotOpen,
                            QStringLiteral("Cannot normalize private SQLite sidecar"));
            }
            struct stat normalized{};
            struct stat rebound{};
            if (::fstat(path_descriptor.get(), &normalized) != 0 ||
                normalized.st_dev != provisional_identity.device ||
                normalized.st_ino != provisional_identity.inode ||
                normalized.st_uid != ::geteuid() || normalized.st_nlink != 1 ||
                (normalized.st_mode & 07777) != 0600 ||
                ::fstatat(parent_descriptor, name.constData(), &rebound, AT_SYMLINK_NOFOLLOW) !=
                    0 ||
                rebound.st_dev != normalized.st_dev || rebound.st_ino != normalized.st_ino) {
                preserve_ = true;
                return fail(CatalogErrorCode::CorruptCatalog,
                            QStringLiteral("Private SQLite sidecar transition is unsafe"));
            }
            CatalogDescriptor real(
                ::openat(parent_descriptor, name.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
            struct stat actual{};
            if (!real || ::fstat(real.get(), &actual) != 0 || actual.st_dev != normalized.st_dev ||
                actual.st_ino != normalized.st_ino || !catalogFileAclAbsent(real.get()) ||
                !catalogFsync(real.get()) || !catalogFsync(parent_descriptor)) {
                preserve_ = true;
                return fail(CatalogErrorCode::CorruptCatalog,
                            QStringLiteral("Private SQLite sidecar cannot be retained"));
            }
            files_.push_back(CatalogScratchFile{
                parent_path + u'/' + QString::fromLatin1(name), name, parent_descriptor,
                std::move(real), catalogIdentity(actual), subject,
                subject != detail::CatalogSubject::PrivateDatabaseMain, false});
        }
        for (auto& file : files_) {
            if (!file.removed && file.parent_descriptor == parent_descriptor &&
                !std::ranges::contains(*names, file.component)) {
                if (!file.may_disappear) {
                    preserve_ = true;
                    return fail(CatalogErrorCode::CorruptCatalog,
                                QStringLiteral("Private SQLite main disappeared"));
                }
                file.removed = true;
            }
        }
        return {};
    }

    [[nodiscard]] auto removeDirectory(std::size_t index) -> std::expected<void, CatalogError> {
        auto& directory = directories_.at(index);
        if (directory.removed) {
            return {};
        }
        for (auto& file : files_) {
            if (!file.removed && file.parent_descriptor == directory.descriptor.get()) {
                const auto removed = removeFile(file);
                if (!removed) {
                    return removed;
                }
            }
        }
        std::size_t count = 0;
        const auto names = readCatalogDirectoryNames(directory.descriptor.get(), count);
        if (!names || !names->empty() ||
            !removeExactCatalogDirectory(directory.parent_descriptor, directory.component,
                                         directory.identity)) {
            preserve_ = true;
            return fail(CatalogErrorCode::CorruptCatalog,
                        QStringLiteral("Private catalog directory cleanup is ambiguous"));
        }
        directory.removed = true;
        return {};
    }

    [[nodiscard]] auto cleanupTransientDirectories() -> std::expected<void, CatalogError> {
        for (std::size_t index = directories_.size(); index-- > 0;) {
            const auto removed = removeDirectory(index);
            if (!removed) {
                return removed;
            }
        }
        return {};
    }

    [[nodiscard]] auto cleanup() -> std::expected<void, CatalogError> {
        if (cleaned_) {
            return {};
        }
        cleaned_ = true;
        if (preserve_) {
            reportCleanup(detail::CatalogCleanupOutcome::Preserved);
            return fail(CatalogErrorCode::CorruptCatalog,
                        QStringLiteral("Private catalog workspace was preserved"));
        }
        for (std::size_t index = directories_.size(); index-- > 0;) {
            const auto removed = removeDirectory(index);
            if (!removed) {
                reportCleanup(detail::CatalogCleanupOutcome::Preserved);
                return removed;
            }
        }
        for (auto& file : files_) {
            if (!file.removed) {
                const auto removed = removeFile(file);
                if (!removed) {
                    reportCleanup(detail::CatalogCleanupOutcome::Preserved);
                    return removed;
                }
            }
        }
        std::size_t count = 0;
        const auto names = readCatalogDirectoryNames(root_.descriptor.get(), count);
        if (!names || !names->empty() ||
            !removeExactCatalogDirectory(root_.parent_descriptor, root_.component,
                                         root_.identity)) {
            preserve_ = true;
            reportCleanup(detail::CatalogCleanupOutcome::Preserved);
            return fail(CatalogErrorCode::CorruptCatalog,
                        QStringLiteral("Private catalog workspace cleanup is ambiguous"));
        }
        root_.removed = true;
        reportCleanup(detail::CatalogCleanupOutcome::Removed);
        return {};
    }

  private:
    CatalogScratchArena(detail::SecureScratchContext& context, detail::CatalogOperation operation,
                        detail::CatalogHooks hooks, int parent_descriptor,
                        detail::CatalogSubject root_subject)
        : context_(&context), operation_(operation), hooks_(std::move(hooks)),
          root_{QString{}, QByteArray{}, parent_descriptor, CatalogDescriptor{-1}, {}, false} {
        static_cast<void>(root_subject);
        directories_.reserve(8);
        files_.reserve(32);
    }

    [[nodiscard]] auto removeFile(CatalogScratchFile& file) -> std::expected<void, CatalogError> {
        struct stat held{};
        struct stat named{};
        const auto held_present = ::fstat(file.descriptor.get(), &held) == 0;
        const auto named_result = ::fstatat(file.parent_descriptor, file.component.constData(),
                                            &named, AT_SYMLINK_NOFOLLOW);
        if (named_result != 0 && errno == ENOENT && file.may_disappear) {
            file.removed = true;
            return {};
        }
        if (!held_present || named_result != 0 || held.st_dev != named.st_dev ||
            held.st_ino != named.st_ino || held.st_uid != ::geteuid() || held.st_nlink != 1 ||
            !S_ISREG(held.st_mode) || (held.st_mode & 07777) != 0600 ||
            !catalogFileAclAbsent(file.descriptor.get())) {
            preserve_ = true;
            return fail(CatalogErrorCode::CorruptCatalog,
                        QStringLiteral("Private catalog file cleanup is ambiguous"));
        }
        if (::unlinkat(file.parent_descriptor, file.component.constData(), 0) != 0 ||
            !catalogFsync(file.parent_descriptor)) {
            preserve_ = true;
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Cannot remove private catalog file"));
        }
        file.removed = true;
        return {};
    }

    void reportCleanup(detail::CatalogCleanupOutcome outcome) {
        if (hooks_.report != nullptr) {
            hooks_.report->cleanup = outcome;
            if (outcome == detail::CatalogCleanupOutcome::Preserved) {
                hooks_.report->remaining_ledger_paths.push_back(root_.absolute_path);
            }
        }
    }

    detail::SecureScratchContext* context_{};
    detail::CatalogOperation operation_{detail::CatalogOperation::None};
    detail::CatalogHooks hooks_;
    CatalogScratchDirectory root_;
    std::vector<CatalogScratchDirectory> directories_;
    std::vector<CatalogScratchFile> files_;
    bool preserve_{};
    bool cleaned_{};
};

class CatalogPrivateConnection final {
  public:
    CatalogPrivateConnection(const CatalogPrivateConnection&) = delete;
    CatalogPrivateConnection& operator=(const CatalogPrivateConnection&) = delete;
    ~CatalogPrivateConnection() { closeBestEffort(); }

    [[nodiscard]] static auto
    open(const QString& database_path, bool read_only, detail::CatalogSubject subject,
         detail::CatalogOperation operation, detail::CatalogHooks hooks,
         std::function<std::expected<void, CatalogError>()> before_open = {},
         std::function<std::expected<void, CatalogError>()> after_open = {})
        -> std::expected<std::unique_ptr<CatalogPrivateConnection>, CatalogError> {
        auto connection = std::unique_ptr<CatalogPrivateConnection>(
            new CatalogPrivateConnection(QStringLiteral("appellate-catalog-private-%1")
                                             .arg(QUuid::createUuid().toString(QUuid::Id128)),
                                         subject, operation, std::move(hooks)));
        detail::CatalogObservation added;
        added.event = detail::CatalogEvent::DatabaseAdded;
        added.subject = subject;
        added.operation = operation;
        added.absolute_path = database_path;
        const auto added_action = catalogAction(connection->hooks_, added);
        if (catalogFailsBefore(added_action)) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Injected private database creation failure"));
        }
        connection->database_ =
            QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection->connection_name_);
        if (read_only) {
            connection->database_.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        }
        connection->database_.setDatabaseName(database_path);
        if (!catalogFinishes(connection->hooks_, added, added_action)) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Injected private database creation failure"));
        }
        detail::CatalogObservation opened = added;
        opened.event = detail::CatalogEvent::DatabaseOpened;
        const auto opened_action = catalogAction(connection->hooks_, opened);
        if (catalogFailsBefore(opened_action)) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Injected private database open failure"));
        }
        if (before_open) {
            if (const auto rebound = before_open(); !rebound) {
                return std::unexpected(rebound.error());
            }
        }
        if (!connection->database_.open()) {
            const auto message = connection->database_.lastError().text();
            return fail(CatalogErrorCode::CannotOpen,
                        message.isEmpty() ? QStringLiteral("Cannot open private catalog database")
                                          : message);
        }
        if (after_open) {
            if (const auto rebound = after_open(); !rebound) {
                return std::unexpected(rebound.error());
            }
        }
        const auto lifecycle_finished = catalogFinishes(connection->hooks_, opened, opened_action);
        if (after_open) {
            if (const auto rebound = after_open(); !rebound) {
                return std::unexpected(rebound.error());
            }
        }
        if (!lifecycle_finished) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Injected private database open failure"));
        }
        return connection;
    }

    [[nodiscard]] QSqlDatabase& database() noexcept { return database_; }

    [[nodiscard]] auto close() -> std::expected<void, CatalogError> {
        if (connection_name_.isEmpty()) {
            return {};
        }
        detail::CatalogObservation closed;
        closed.event = detail::CatalogEvent::DatabaseClosed;
        closed.subject = subject_;
        closed.operation = operation_;
        const auto closed_action = catalogAction(hooks_, closed);
        const auto injected_failure = catalogFailsBefore(closed_action);
        if (database_.isValid()) {
            database_.close();
        }
        const auto visibly_closed = !database_.isOpen();
        database_ = QSqlDatabase{};
        detail::CatalogObservation reset = closed;
        reset.event = detail::CatalogEvent::DatabaseReset;
        const auto reset_action = catalogAction(hooks_, reset);
        const auto name = std::exchange(connection_name_, {});
        QSqlDatabase::removeDatabase(name);
        detail::CatalogObservation removed = closed;
        removed.event = detail::CatalogEvent::DatabaseRemoved;
        const auto removed_action = catalogAction(hooks_, removed);
        const auto lifecycle_finished =
            catalogFinishes(hooks_, closed, closed_action) && !catalogFailsBefore(reset_action) &&
            catalogFinishes(hooks_, reset, reset_action) && !catalogFailsBefore(removed_action) &&
            catalogFinishes(hooks_, removed, removed_action);
        if (injected_failure || !visibly_closed || QSqlDatabase::contains(name) ||
            !lifecycle_finished) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Private database lifecycle did not close cleanly"));
        }
        return {};
    }

  private:
    CatalogPrivateConnection(QString connection_name, detail::CatalogSubject subject,
                             detail::CatalogOperation operation, detail::CatalogHooks hooks)
        : connection_name_(std::move(connection_name)), subject_(subject), operation_(operation),
          hooks_(std::move(hooks)) {}

    void closeBestEffort() noexcept {
        if (connection_name_.isEmpty()) {
            return;
        }
        if (database_.isValid()) {
            database_.close();
            database_ = QSqlDatabase{};
        }
        const auto name = std::exchange(connection_name_, {});
        QSqlDatabase::removeDatabase(name);
    }

    QString connection_name_;
    detail::CatalogSubject subject_{detail::CatalogSubject::None};
    detail::CatalogOperation operation_{detail::CatalogOperation::None};
    detail::CatalogHooks hooks_;
    QSqlDatabase database_;
};

[[nodiscard]] bool sameCatalogTuple(const CatalogIdentity& left, const CatalogIdentity& right,
                                    bool include_size_and_times = true) {
    const auto base =
        left.device == right.device && left.inode == right.inode && left.mode == right.mode &&
        (S_ISDIR(left.mode) || left.links == right.links) && left.owner == right.owner;
    if (!base || !include_size_and_times) {
        return base;
    }
    return left.size == right.size && left.modified.tv_sec == right.modified.tv_sec &&
           left.modified.tv_nsec == right.modified.tv_nsec &&
           left.changed.tv_sec == right.changed.tv_sec &&
           left.changed.tv_nsec == right.changed.tv_nsec;
}

[[nodiscard]] bool sameCatalogObjects(const std::vector<CatalogObjectIdentity>& left,
                                      const std::vector<CatalogObjectIdentity>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (left.at(index).name != right.at(index).name ||
            !sameCatalogTuple(left.at(index).identity, right.at(index).identity, true)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool sameCatalogNamespace(const CatalogNamespace& left,
                                        const CatalogNamespace& right) {
    if (left.shape != right.shape || left.root_names != right.root_names ||
        !sameCatalogTuple(left.root_identity, right.root_identity, true) ||
        !sameCatalogTuple(left.archives_identity, right.archives_identity, true) ||
        left.blobs_identity.has_value() != right.blobs_identity.has_value() ||
        (left.blobs_identity &&
         !sameCatalogTuple(*left.blobs_identity, *right.blobs_identity, true)) ||
        !sameCatalogObjects(left.archives, right.archives) ||
        !sameCatalogObjects(left.blobs, right.blobs) ||
        !sameCatalogTuple(left.main.identity, right.main.identity, true) ||
        left.wal.has_value() != right.wal.has_value() ||
        (left.wal && !sameCatalogTuple(left.wal->identity, right.wal->identity, true)) ||
        left.shm.has_value() != right.shm.has_value()) {
        return false;
    }
    // SQLite readers can legitimately mutate SHM bytes and timestamps. Its stable binding and
    // safety metadata, but not its mutable content tuple, belongs to source capture equality.
    return !left.shm || sameCatalogTuple(left.shm->identity, right.shm->identity, false);
}

[[nodiscard]] bool sameCatalogPersistentBindingsAcrossSqlite(const CatalogNamespace& before,
                                                             const CatalogNamespace& after) {
    const auto without_sidecars = [](const std::vector<QByteArray>& names) {
        std::vector<QByteArray> filtered;
        filtered.reserve(names.size());
        std::ranges::copy_if(names, std::back_inserter(filtered), [](const QByteArray& name) {
            return name != QByteArrayView("catalog.sqlite-wal") &&
                   name != QByteArrayView("catalog.sqlite-shm");
        });
        return filtered;
    };
    return before.shape == detail::CatalogNamespaceShape::Current &&
           after.shape == detail::CatalogNamespaceShape::Current &&
           without_sidecars(before.root_names) == without_sidecars(after.root_names) &&
           sameCatalogTuple(before.root_identity, after.root_identity, false) &&
           sameCatalogTuple(before.archives_identity, after.archives_identity, true) &&
           before.blobs_identity && after.blobs_identity &&
           sameCatalogTuple(*before.blobs_identity, *after.blobs_identity, true) &&
           sameCatalogObjects(before.archives, after.archives) &&
           sameCatalogObjects(before.blobs, after.blobs) &&
           sameCatalogTuple(before.main.identity, after.main.identity, false) &&
           before.legacy_lock.has_value() == after.legacy_lock.has_value() &&
           (!before.legacy_lock ||
            sameCatalogTuple(before.legacy_lock->identity, after.legacy_lock->identity, true));
}

[[nodiscard]] bool sameCatalogBindingAcrossAuthorizedModeChange(const CatalogIdentity& before,
                                                                const CatalogIdentity& after) {
    return before.device == after.device && before.inode == after.inode && S_ISDIR(before.mode) &&
           S_ISDIR(after.mode) && before.owner == after.owner;
}

[[nodiscard]] bool sameCatalogOptionalFile(const std::optional<CatalogRetainedFile>& before,
                                           const std::optional<CatalogRetainedFile>& after,
                                           bool include_mutable_metadata) {
    return before.has_value() == after.has_value() &&
           (!before ||
            sameCatalogTuple(before->identity, after->identity, include_mutable_metadata));
}

[[nodiscard]] bool
sameCatalogSetupTransition(const CatalogNamespace& before, const CatalogNamespace& after,
                           const std::optional<CatalogIdentity>& created_blobs_identity) {
    auto expected_names = before.root_names;
    if (before.shape == detail::CatalogNamespaceShape::ClosedVersion1) {
        expected_names.push_back(QByteArrayLiteral("blobs"));
        std::ranges::sort(expected_names);
    }
    if (after.shape != detail::CatalogNamespaceShape::Current ||
        expected_names != after.root_names ||
        !sameCatalogBindingAcrossAuthorizedModeChange(before.root_identity, after.root_identity) ||
        !sameCatalogBindingAcrossAuthorizedModeChange(before.archives_identity,
                                                      after.archives_identity) ||
        !sameCatalogObjects(before.archives, after.archives) ||
        !sameCatalogObjects(before.blobs, after.blobs) ||
        !sameCatalogTuple(before.main.identity, after.main.identity, true) ||
        !sameCatalogOptionalFile(before.wal, after.wal, true) ||
        !sameCatalogOptionalFile(before.shm, after.shm, false) || after.legacy_lock) {
        return false;
    }
    if (before.shape == detail::CatalogNamespaceShape::Current) {
        return before.blobs_identity && after.blobs_identity &&
               sameCatalogBindingAcrossAuthorizedModeChange(*before.blobs_identity,
                                                            *after.blobs_identity);
    }
    return before.shape == detail::CatalogNamespaceShape::ClosedVersion1 &&
           created_blobs_identity && after.blobs_identity &&
           sameCatalogTuple(*created_blobs_identity, *after.blobs_identity, true) &&
           after.blobs.empty();
}

[[nodiscard]] bool sameCatalogStateAcrossAttemptLock(const CatalogNamespace& before,
                                                     const CatalogNamespace& after,
                                                     const CatalogIdentity& allowed_lock) {
    auto expected_names = before.root_names;
    expected_names.push_back(QByteArrayLiteral(".install.lock"));
    std::ranges::sort(expected_names);
    return before.shape == detail::CatalogNamespaceShape::Current &&
           after.shape == detail::CatalogNamespaceShape::Current &&
           expected_names == after.root_names &&
           sameCatalogTuple(before.root_identity, after.root_identity, false) &&
           sameCatalogTuple(before.archives_identity, after.archives_identity, true) &&
           before.blobs_identity && after.blobs_identity &&
           sameCatalogTuple(*before.blobs_identity, *after.blobs_identity, true) &&
           sameCatalogObjects(before.archives, after.archives) &&
           sameCatalogObjects(before.blobs, after.blobs) &&
           sameCatalogTuple(before.main.identity, after.main.identity, true) &&
           sameCatalogOptionalFile(before.wal, after.wal, true) &&
           sameCatalogOptionalFile(before.shm, after.shm, false) && after.legacy_lock &&
           sameCatalogTuple(allowed_lock, after.legacy_lock->identity, true);
}

[[nodiscard]] auto revalidateCatalogNamespaceBindings(
    CatalogAnchor& anchor, const CatalogNamespace& expected, detail::CatalogOperation operation,
    const detail::CatalogHooks& hooks, bool include_mutable_metadata = true,
    bool validate_object_bindings = true) -> std::expected<void, CatalogError> {
    std::size_t root_count = 0;
    auto root_names =
        catalogDirectoryNames(anchor.rootDescriptor(), root_count, anchor.absolute_root,
                              detail::CatalogSubject::CatalogRoot, operation, hooks);
    if (!root_names) {
        return std::unexpected(root_names.error());
    }
    if (*root_names != expected.root_names) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Catalog root inventory changed before SQLite access"));
    }
    if (const auto valid = revalidateCatalogAnchor(anchor); !valid) {
        return std::unexpected(valid.error());
    }
    if (anchor.archives == std::nullopt ||
        !catalogIdentityRebound(anchor.rootDescriptor(), anchor.archives->component,
                                anchor.archives->descriptor.get(), expected.archives_identity,
                                include_mutable_metadata) ||
        (expected.blobs_identity.has_value() != anchor.blobs.has_value()) ||
        (anchor.blobs &&
         !catalogIdentityRebound(anchor.rootDescriptor(), anchor.blobs->component,
                                 anchor.blobs->descriptor.get(), *expected.blobs_identity,
                                 include_mutable_metadata)) ||
        !catalogIdentityRebound(anchor.rootDescriptor(), expected.main.component,
                                expected.main.descriptor.get(), expected.main.identity,
                                include_mutable_metadata)) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Catalog binding changed before SQLite access"));
    }
    if (!catalogDirectoryAclsAbsent(anchor.archives->descriptor.get()) ||
        (anchor.blobs && !catalogDirectoryAclsAbsent(anchor.blobs->descriptor.get())) ||
        !catalogFileAclAbsent(expected.main.descriptor.get())) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Catalog ACL appeared before SQLite access"));
    }
    const auto validate_optional =
        [&](const std::optional<CatalogRetainedFile>& file) -> std::expected<void, CatalogError> {
        if (!file) {
            return {};
        }
        if (!catalogIdentityRebound(anchor.rootDescriptor(), file->component,
                                    file->descriptor.get(), file->identity,
                                    include_mutable_metadata)) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Catalog sidecar binding changed before SQLite access"));
        }
        if (!catalogFileAclAbsent(file->descriptor.get())) {
            return fail(CatalogErrorCode::CorruptCatalog,
                        QStringLiteral("Catalog sidecar ACL appeared before SQLite access"));
        }
        return {};
    };
    if (const auto valid = validate_optional(expected.wal); !valid) {
        return valid;
    }
    if (const auto valid = validate_optional(expected.shm); !valid) {
        return valid;
    }
    if (const auto valid = validate_optional(expected.legacy_lock); !valid) {
        return valid;
    }
    const auto validate_objects =
        [&](const CatalogRetainedDirectory& directory,
            const std::vector<CatalogObjectIdentity>& objects,
            detail::CatalogSubject subject) -> std::expected<void, CatalogError> {
        for (const auto& object : objects) {
            auto retained = retainCatalogFile(directory.descriptor.get(), directory.absolute_path,
                                              object.name, subject, false, operation, hooks);
            if (!retained) {
                return std::unexpected(retained.error());
            }
            if (!sameCatalogTuple(object.identity, retained->identity, true)) {
                return fail(CatalogErrorCode::CorruptCatalog,
                            QStringLiteral("Catalog object binding changed before SQLite access"));
            }
        }
        return {};
    };
    if (validate_object_bindings) {
        if (const auto valid = validate_objects(*anchor.archives, expected.archives,
                                                detail::CatalogSubject::ArchiveObject);
            !valid) {
            return valid;
        }
        if (anchor.blobs) {
            if (const auto valid = validate_objects(*anchor.blobs, expected.blobs,
                                                    detail::CatalogSubject::BlobObject);
                !valid) {
                return valid;
            }
        }
    }
    return {};
}

struct CatalogStableCapture final {
    CatalogNamespace source;
    std::size_t pass_a_directory{};
    std::size_t pass_b_directory{};
    CatalogScratchCopy pass_a_main;
    CatalogScratchCopy pass_b_main;
    std::optional<CatalogScratchCopy> pass_a_wal;
    std::optional<CatalogScratchCopy> pass_b_wal;
};

[[nodiscard]] auto
observeCatalogCaptureEvent(detail::CatalogEvent event, detail::CatalogSubject subject,
                           detail::CatalogOperation operation, std::size_t pass,
                           std::uint64_t byte_count, const detail::CatalogHooks& hooks)
    -> std::expected<void, CatalogError> {
    detail::CatalogObservation observation;
    observation.event = event;
    observation.subject = subject;
    observation.operation = operation;
    observation.pass = pass;
    observation.byte_count = byte_count;
    const auto action = catalogAction(hooks, observation);
    if (catalogFailsBefore(action) || !catalogFinishes(hooks, observation, action)) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Injected private catalog capture failure"));
    }
    return {};
}

[[nodiscard]] auto
repeatCatalogNamespace(CatalogAnchor& anchor, const CatalogNamespace& expected, bool writable,
                       const CatalogRootLock& root_lock, detail::CatalogOperation operation,
                       const detail::CatalogHooks& hooks,
                       const std::optional<CatalogIdentity>& allowed_lock = std::nullopt)
    -> std::expected<CatalogNamespace, CatalogError> {
    if (const auto valid = revalidateCatalogAnchor(anchor); !valid) {
        return std::unexpected(valid.error());
    }
    auto repeated =
        classifyCatalogNamespace(anchor, writable, root_lock, operation, hooks, allowed_lock);
    if (!repeated) {
        return std::unexpected(repeated.error());
    }
    if (!sameCatalogNamespace(expected, *repeated)) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Catalog namespace changed during private capture"));
    }
    return repeated;
}

[[maybe_unused, nodiscard]] auto
captureCatalogDatabase(CatalogAnchor& anchor, CatalogNamespace initial, bool writable,
                       const CatalogRootLock& root_lock, CatalogScratchArena& arena,
                       detail::CatalogOperation operation, const detail::CatalogHooks& hooks,
                       const std::optional<CatalogIdentity>& allowed_lock = std::nullopt)
    -> std::expected<CatalogStableCapture, CatalogError> {
    const auto main_bytes = static_cast<std::uint64_t>(initial.main.identity.size);
    const auto wal_bytes =
        initial.wal ? static_cast<std::uint64_t>(initial.wal->identity.size) : 0U;
    if (main_bytes > maximum_catalog_capture_bytes / 2U ||
        wal_bytes > maximum_catalog_capture_bytes / 2U ||
        main_bytes > maximum_catalog_capture_bytes / 2U - wal_bytes) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Catalog A/B capture exceeds 2 GiB"));
    }
    const auto source_bytes = main_bytes + wal_bytes;
    const auto total_bytes = source_bytes * 2U;
    if (const auto observed = observeCatalogCaptureEvent(detail::CatalogEvent::CapturePreflight,
                                                         detail::CatalogSubject::DatabaseMain,
                                                         operation, 0, total_bytes, hooks);
        !observed) {
        return std::unexpected(observed.error());
    }
    auto pass_a =
        arena.createDirectory(QByteArrayLiteral("pass-a"), detail::CatalogSubject::PassAWorkspace);
    if (!pass_a) {
        return std::unexpected(pass_a.error());
    }
    auto pass_b =
        arena.createDirectory(QByteArrayLiteral("pass-b"), detail::CatalogSubject::PassBWorkspace);
    if (!pass_b) {
        return std::unexpected(pass_b.error());
    }

    if (const auto observed = observeCatalogCaptureEvent(detail::CatalogEvent::CapturePassStarted,
                                                         detail::CatalogSubject::PassAWorkspace,
                                                         operation, 1, source_bytes, hooks);
        !observed) {
        return std::unexpected(observed.error());
    }
    auto pass_a_main = arena.copyRetainedFile(anchor.rootDescriptor(), initial.main, *pass_a,
                                              detail::CatalogSubject::PrivateDatabaseMain, 1);
    if (!pass_a_main) {
        return std::unexpected(pass_a_main.error());
    }
    std::optional<CatalogScratchCopy> pass_a_wal;
    if (initial.wal) {
        auto copied = arena.copyRetainedFile(anchor.rootDescriptor(), *initial.wal, *pass_a,
                                             detail::CatalogSubject::PrivateDatabaseWal, 1);
        if (!copied) {
            return std::unexpected(copied.error());
        }
        pass_a_wal = std::move(*copied);
    }
    if (const auto observed = observeCatalogCaptureEvent(detail::CatalogEvent::CapturePassFinished,
                                                         detail::CatalogSubject::PassAWorkspace,
                                                         operation, 1, source_bytes, hooks);
        !observed) {
        return std::unexpected(observed.error());
    }

    if (const auto observed = observeCatalogCaptureEvent(detail::CatalogEvent::CaptureBarrier,
                                                         detail::CatalogSubject::CatalogRoot,
                                                         operation, 1, source_bytes, hooks);
        !observed) {
        return std::unexpected(observed.error());
    }
    auto after_a = repeatCatalogNamespace(anchor, initial, writable, root_lock, operation, hooks,
                                          allowed_lock);
    if (!after_a) {
        return std::unexpected(after_a.error());
    }

    if (const auto observed = observeCatalogCaptureEvent(detail::CatalogEvent::CapturePassStarted,
                                                         detail::CatalogSubject::PassBWorkspace,
                                                         operation, 2, source_bytes, hooks);
        !observed) {
        return std::unexpected(observed.error());
    }
    auto pass_b_main = arena.copyRetainedFile(anchor.rootDescriptor(), after_a->main, *pass_b,
                                              detail::CatalogSubject::PrivateDatabaseMain, 2);
    if (!pass_b_main) {
        return std::unexpected(pass_b_main.error());
    }
    std::optional<CatalogScratchCopy> pass_b_wal;
    if (after_a->wal) {
        auto copied = arena.copyRetainedFile(anchor.rootDescriptor(), *after_a->wal, *pass_b,
                                             detail::CatalogSubject::PrivateDatabaseWal, 2);
        if (!copied) {
            return std::unexpected(copied.error());
        }
        pass_b_wal = std::move(*copied);
    }
    if (const auto observed = observeCatalogCaptureEvent(detail::CatalogEvent::CapturePassFinished,
                                                         detail::CatalogSubject::PassBWorkspace,
                                                         operation, 2, source_bytes, hooks);
        !observed) {
        return std::unexpected(observed.error());
    }
    auto after_b = repeatCatalogNamespace(anchor, *after_a, writable, root_lock, operation, hooks,
                                          allowed_lock);
    if (!after_b) {
        return std::unexpected(after_b.error());
    }
    if (const auto equal = arena.compareCopies(*pass_a_main, *pass_b_main); !equal) {
        return std::unexpected(equal.error());
    }
    if (pass_a_wal.has_value() != pass_b_wal.has_value() ||
        (pass_a_wal && !arena.compareCopies(*pass_a_wal, *pass_b_wal))) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Catalog WAL capture passes differ"));
    }
    return CatalogStableCapture{std::move(*after_b),  *pass_a,      *pass_b,
                                *pass_a_main,         *pass_b_main, std::move(pass_a_wal),
                                std::move(pass_b_wal)};
}

[[maybe_unused, nodiscard]] auto
openAndFingerprintCapturedCatalog(CatalogScratchArena& arena, const CatalogStableCapture& capture,
                                  detail::CatalogSchemaGeneration generation,
                                  detail::CatalogOperation operation,
                                  const detail::CatalogHooks& hooks)
    -> std::expected<std::unique_ptr<CatalogPrivateConnection>, CatalogError> {
    if (const auto admitted = arena.admitSqliteDirectory(capture.pass_a_directory); !admitted) {
        return std::unexpected(admitted.error());
    }
    if (const auto valid = arena.validateSqliteDirectoryForAmbientUse(capture.pass_a_directory);
        !valid) {
        return std::unexpected(valid.error());
    }
    const auto candidate_path = arena.file(capture.pass_a_main.file_index).absolute_path;
    auto candidate = CatalogPrivateConnection::open(
        candidate_path, true, detail::CatalogSubject::PrivateDatabaseMain, operation, hooks,
        [&arena, &capture] {
            return arena.validateSqliteDirectoryForAmbientUse(capture.pass_a_directory);
        },
        [&arena, &capture]() -> std::expected<void, CatalogError> {
            if (const auto admitted = arena.admitSqliteDirectory(capture.pass_a_directory);
                !admitted) {
                return admitted;
            }
            return arena.validateSqliteDirectoryForAmbientUse(capture.pass_a_directory);
        });
    if (!candidate) {
        return std::unexpected(candidate.error());
    }
    if (const auto configured = detail::configureCatalogFingerprintConnection(
            (*candidate)->database(), detail::CatalogSqliteConfiguration{true, true, 5'000},
            detail::CatalogSubject::PrivateDatabaseMain, hooks, operation);
        !configured) {
        return std::unexpected(configured.error());
    }
    if (const auto admitted = arena.admitSqliteDirectory(capture.pass_a_directory); !admitted) {
        return std::unexpected(admitted.error());
    }
    if (const auto valid = arena.validateSqliteDirectoryForAmbientUse(capture.pass_a_directory);
        !valid) {
        return std::unexpected(valid.error());
    }

    struct NormativeReference final {
        detail::CatalogSchemaGeneration generation{};
        detail::CatalogSubject subject{detail::CatalogSubject::None};
        std::size_t directory{};
        QString path;
        std::unique_ptr<CatalogPrivateConnection> reader;
    };
    std::vector<NormativeReference> references;
    references.reserve(2);
    constexpr std::array reference_generations{
        detail::CatalogSchemaGeneration::Version1,
        detail::CatalogSchemaGeneration::Current,
    };
    for (const auto reference_generation : reference_generations) {
        const auto reference_subject =
            reference_generation == detail::CatalogSchemaGeneration::Version1
                ? detail::CatalogSubject::Version1Reference
                : detail::CatalogSubject::CurrentReference;
        auto reference_directory =
            arena.createDirectory(reference_generation == detail::CatalogSchemaGeneration::Version1
                                      ? QByteArrayLiteral("reference-v1")
                                      : QByteArrayLiteral("reference-current"),
                                  reference_subject);
        if (!reference_directory) {
            return std::unexpected(reference_directory.error());
        }
        auto reference_main =
            arena.createFile(*reference_directory, QByteArrayLiteral("catalog.sqlite"),
                             detail::CatalogSubject::PrivateDatabaseMain);
        if (!reference_main) {
            return std::unexpected(reference_main.error());
        }
        const auto reference_path = arena.file(*reference_main).absolute_path;
        if (const auto valid = arena.validateSqliteDirectoryForAmbientUse(*reference_directory);
            !valid) {
            return std::unexpected(valid.error());
        }
        auto reference_writer = CatalogPrivateConnection::open(
            reference_path, false, reference_subject, operation, hooks,
            [&arena, reference_directory = *reference_directory] {
                return arena.validateSqliteDirectoryForAmbientUse(reference_directory);
            },
            [&arena,
             reference_directory = *reference_directory]() -> std::expected<void, CatalogError> {
                if (const auto admitted = arena.admitSqliteDirectory(reference_directory);
                    !admitted) {
                    return admitted;
                }
                return arena.validateSqliteDirectoryForAmbientUse(reference_directory);
            });
        if (!reference_writer) {
            return std::unexpected(reference_writer.error());
        }
        if (const auto created = detail::createNormativeCatalogSchema(
                (*reference_writer)->database(), reference_generation, reference_subject, hooks,
                operation);
            !created) {
            return std::unexpected(created.error());
        }
        if (const auto closed = (*reference_writer)->close(); !closed) {
            return std::unexpected(closed.error());
        }
        if (const auto admitted = arena.admitSqliteDirectory(*reference_directory, true);
            !admitted) {
            return std::unexpected(admitted.error());
        }
        if (const auto valid = arena.validateSqliteDirectoryForAmbientUse(*reference_directory);
            !valid) {
            return std::unexpected(valid.error());
        }
        auto reference_reader = CatalogPrivateConnection::open(
            reference_path, true, reference_subject, operation, hooks,
            [&arena, reference_directory = *reference_directory] {
                return arena.validateSqliteDirectoryForAmbientUse(reference_directory);
            },
            [&arena,
             reference_directory = *reference_directory]() -> std::expected<void, CatalogError> {
                if (const auto admitted = arena.admitSqliteDirectory(reference_directory);
                    !admitted) {
                    return admitted;
                }
                return arena.validateSqliteDirectoryForAmbientUse(reference_directory);
            });
        if (!reference_reader) {
            return std::unexpected(reference_reader.error());
        }
        if (const auto configured = detail::configureCatalogFingerprintConnection(
                (*reference_reader)->database(),
                detail::CatalogSqliteConfiguration{true, true, 5'000}, reference_subject, hooks,
                operation);
            !configured) {
            return std::unexpected(configured.error());
        }
        if (const auto admitted = arena.admitSqliteDirectory(*reference_directory); !admitted) {
            return std::unexpected(admitted.error());
        }
        if (const auto valid = arena.validateSqliteDirectoryForAmbientUse(*reference_directory);
            !valid) {
            return std::unexpected(valid.error());
        }
        references.push_back(NormativeReference{reference_generation, reference_subject,
                                                *reference_directory, reference_path,
                                                std::move(*reference_reader)});
    }
    const auto selected_reference =
        std::ranges::find(references, generation, &NormativeReference::generation);
    if (selected_reference == references.end()) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Normative catalog reference selection failed"));
    }
    if (const auto admitted = arena.admitSqliteDirectory(capture.pass_a_directory); !admitted) {
        return std::unexpected(admitted.error());
    }
    if (const auto admitted = arena.admitSqliteDirectory(selected_reference->directory);
        !admitted) {
        return std::unexpected(admitted.error());
    }
    if (const auto valid = arena.validateSqliteDirectoryForAmbientUse(capture.pass_a_directory);
        !valid) {
        return std::unexpected(valid.error());
    }
    if (const auto valid =
            arena.validateSqliteDirectoryForAmbientUse(selected_reference->directory);
        !valid) {
        return std::unexpected(valid.error());
    }
    if (const auto compared = detail::compareCatalogLogicalFingerprint(
            (*candidate)->database(), selected_reference->reader->database(), generation,
            detail::CatalogSubject::PrivateDatabaseMain, hooks, operation);
        !compared) {
        return std::unexpected(compared.error());
    }
    if (const auto admitted = arena.admitSqliteDirectory(capture.pass_a_directory); !admitted) {
        return std::unexpected(admitted.error());
    }
    if (const auto admitted = arena.admitSqliteDirectory(selected_reference->directory);
        !admitted) {
        return std::unexpected(admitted.error());
    }
    if (const auto valid = arena.validateSqliteDirectoryForAmbientUse(capture.pass_a_directory);
        !valid) {
        return std::unexpected(valid.error());
    }
    if (const auto valid =
            arena.validateSqliteDirectoryForAmbientUse(selected_reference->directory);
        !valid) {
        return std::unexpected(valid.error());
    }
    for (auto& reference : references) {
        if (const auto closed = reference.reader->close(); !closed) {
            return std::unexpected(closed.error());
        }
        if (const auto admitted = arena.admitSqliteDirectory(reference.directory); !admitted) {
            return std::unexpected(admitted.error());
        }
    }
    return std::move(*candidate);
}

[[nodiscard]] const CatalogObjectIdentity*
findCatalogObject(const std::vector<CatalogObjectIdentity>& objects, QByteArrayView name) {
    const auto found =
        std::ranges::lower_bound(objects, name, {}, [](const CatalogObjectIdentity& object) {
            return QByteArrayView(object.name);
        });
    return found != objects.end() && found->name == name ? &*found : nullptr;
}

[[nodiscard]] auto securelyLoadCatalogArchive(
    CatalogAnchor& anchor, const std::vector<CatalogObjectIdentity>& admitted_archives,
    const QString& digest, CatalogScratchArena& arena, std::optional<std::size_t> archive_directory,
    detail::SecureScratchContext& scratch_context, detail::CatalogOperation operation,
    const detail::CatalogHooks& hooks) -> std::expected<LoadedPack, Error> {
    const auto component = digest.toLatin1() + QByteArrayLiteral(".awpack");
    const auto* admitted = findCatalogObject(admitted_archives, component);
    if (admitted == nullptr || !anchor.archives || !revalidateCatalogAnchor(anchor) ||
        !catalogIdentityRebound(anchor.rootDescriptor(), anchor.archives->component,
                                anchor.archives->descriptor.get(), anchor.archives->identity,
                                false) ||
        !catalogDirectoryAclsAbsent(anchor.archives->descriptor.get())) {
        return std::unexpected(
            Error{ErrorCode::UnsafePath, QStringLiteral("Catalog archive is not admitted")});
    }
    auto retained = retainCatalogFile(
        anchor.archives->descriptor.get(), anchor.archives->absolute_path, component,
        detail::CatalogSubject::ArchiveObject, false, operation, hooks);
    if (!retained || !sameCatalogTuple(admitted->identity, retained->identity, true)) {
        return std::unexpected(
            Error{ErrorCode::UnsafePath, QStringLiteral("Catalog archive binding changed")});
    }
    auto copied =
        arena.copyRetainedFile(anchor.archives->descriptor.get(), *retained, archive_directory,
                               detail::CatalogSubject::ArchiveWorkspace, 0);
    if (!copied) {
        return std::unexpected(
            Error{ErrorCode::CannotRead, QStringLiteral("Cannot capture catalog archive")});
    }
    if (copied->sha256 != digest.toLatin1()) {
        return std::unexpected(
            Error{ErrorCode::DigestMismatch, QStringLiteral("Catalog archive digest differs")});
    }
    auto loaded = detail::importArchiveThroughRetainedSecureScratch(
        arena.file(copied->file_index).absolute_path, PackArchiveLimits{},
        PackValidationScope::ResolvedClosure, scratch_context);
    const auto private_hash =
        hashCatalogDescriptor(arena.file(copied->file_index).descriptor.get(), copied->byte_count);
    struct stat source_after{};
    struct stat source_named{};
    if (!private_hash || *private_hash != copied->sha256 ||
        ::fstat(retained->descriptor.get(), &source_after) != 0 ||
        ::fstatat(anchor.archives->descriptor.get(), component.constData(), &source_named,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        !sameCatalogIdentity(admitted->identity, source_after, true) ||
        !sameCatalogIdentity(admitted->identity, source_named, true) ||
        !catalogFileAclAbsent(retained->descriptor.get())) {
        return std::unexpected(
            Error{ErrorCode::UnsafePath, QStringLiteral("Catalog archive changed during import")});
    }
    return loaded;
}

[[nodiscard]] auto securelyVerifyCatalogBlob(
    CatalogAnchor& anchor, const std::vector<CatalogObjectIdentity>& admitted_blobs,
    const QString& digest, std::uint64_t expected_size, detail::CatalogOperation operation,
    const detail::CatalogHooks& hooks) -> std::expected<void, CatalogError> {
    const auto component = digest.toLatin1();
    const auto* admitted = findCatalogObject(admitted_blobs, component);
    if (admitted == nullptr || !anchor.blobs || admitted->identity.size < 0 ||
        static_cast<std::uint64_t>(admitted->identity.size) != expected_size) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Catalog blob object is not admitted at its declared size"));
    }
    auto retained =
        retainCatalogFile(anchor.blobs->descriptor.get(), anchor.blobs->absolute_path, component,
                          detail::CatalogSubject::BlobObject, false, operation, hooks);
    if (!retained || !sameCatalogTuple(admitted->identity, retained->identity, true)) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Catalog blob binding changed"));
    }
    const auto hash = hashCatalogDescriptor(retained->descriptor.get(), expected_size);
    struct stat after{};
    struct stat named{};
    if (!hash || *hash != component || ::fstat(retained->descriptor.get(), &after) != 0 ||
        ::fstatat(anchor.blobs->descriptor.get(), component.constData(), &named,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        !sameCatalogIdentity(admitted->identity, after, true) ||
        !sameCatalogIdentity(admitted->identity, named, true) ||
        !catalogFileAclAbsent(retained->descriptor.get())) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Catalog blob object digest differs"));
    }
    return {};
}

[[nodiscard]] auto reverseAdmitCapturedCatalog(
    CatalogAnchor& anchor, const CatalogStableCapture& capture, CatalogScratchArena& arena,
    detail::SecureScratchContext& scratch_context, QSqlDatabase& private_database,
    detail::CatalogSchemaGeneration generation, detail::CatalogOperation operation,
    const detail::CatalogHooks& hooks) -> std::expected<void, CatalogError> {
    std::set<QString> archive_digests;
    for (const auto& archive : capture.source.archives) {
        archive_digests.insert(QString::fromLatin1(archive.name.first(archive.name.size() - 7)));
    }
    std::set<QString> blob_digests;
    for (const auto& blob : capture.source.blobs) {
        blob_digests.insert(QString::fromLatin1(blob.name));
    }
    auto archive_directory = arena.createDirectory(QByteArrayLiteral("archive-validation"),
                                                   detail::CatalogSubject::ArchiveWorkspace);
    if (!archive_directory) {
        return std::unexpected(archive_directory.error());
    }
    detail::SecureCatalogArchiveLoader archive_loader =
        [&](const QString& digest) -> std::expected<LoadedPack, Error> {
        return securelyLoadCatalogArchive(anchor, capture.source.archives, digest, arena,
                                          *archive_directory, scratch_context, operation, hooks);
    };
    detail::SecureCatalogBlobVerifier blob_verifier =
        [&](const QString& digest, std::uint64_t byte_size) -> std::expected<void, CatalogError> {
        return securelyVerifyCatalogBlob(anchor, capture.source.blobs, digest, byte_size, operation,
                                         hooks);
    };
    return detail::validateCatalogReverseAdmission(private_database, generation, archive_digests,
                                                   blob_digests, archive_loader, blob_verifier,
                                                   hooks, operation);
}

[[maybe_unused, nodiscard]] auto admitCatalogPrivately(
    CatalogAnchor& anchor, CatalogNamespace initial, detail::CatalogSchemaGeneration generation,
    detail::SecureScratchContext& scratch_context, const CatalogRootLock& root_lock,
    detail::CatalogOperation operation, const detail::CatalogHooks& hooks,
    const std::optional<CatalogIdentity>& allowed_lock = std::nullopt)
    -> std::expected<CatalogNamespace, CatalogError> {
    auto arena = CatalogScratchArena::create(scratch_context, operation, hooks);
    if (!arena) {
        return std::unexpected(arena.error());
    }
    auto capture = captureCatalogDatabase(anchor, std::move(initial), false, root_lock, **arena,
                                          operation, hooks, allowed_lock);
    if (!capture) {
        return std::unexpected(capture.error());
    }
    auto candidate =
        openAndFingerprintCapturedCatalog(**arena, *capture, generation, operation, hooks);
    if (!candidate) {
        return std::unexpected(candidate.error());
    }
    if (const auto reverse =
            reverseAdmitCapturedCatalog(anchor, *capture, **arena, scratch_context,
                                        (*candidate)->database(), generation, operation, hooks);
        !reverse) {
        return std::unexpected(reverse.error());
    }
    if (const auto closed = (*candidate)->close(); !closed) {
        return std::unexpected(closed.error());
    }
    if (const auto admitted = (*arena)->admitSqliteDirectory(capture->pass_a_directory);
        !admitted) {
        return std::unexpected(admitted.error());
    }
    auto final_source = repeatCatalogNamespace(anchor, capture->source, false, root_lock, operation,
                                               hooks, allowed_lock);
    if (!final_source) {
        return std::unexpected(final_source.error());
    }
    if (const auto cleaned = (*arena)->cleanup(); !cleaned) {
        return std::unexpected(cleaned.error());
    }
    return final_source;
}

[[nodiscard]] auto
loadAdmittedCatalogArchives(CatalogAnchor& anchor, const CatalogNamespace& admitted,
                            detail::SecureScratchContext& scratch_context,
                            detail::CatalogOperation operation, const detail::CatalogHooks& hooks)
    -> std::expected<std::vector<LoadedPack>, CatalogError> {
    auto arena = CatalogScratchArena::create(scratch_context, operation, hooks,
                                             detail::CatalogSubject::ArchiveWorkspace);
    if (!arena) {
        return std::unexpected(arena.error());
    }
    std::vector<LoadedPack> loaded_archives;
    loaded_archives.reserve(admitted.archives.size());
    for (const auto& archive : admitted.archives) {
        const auto digest = QString::fromLatin1(archive.name.first(archive.name.size() - 7));
        auto loaded = securelyLoadCatalogArchive(anchor, admitted.archives, digest, **arena,
                                                 std::nullopt, scratch_context, operation, hooks);
        if (!loaded) {
            const auto code = loaded.error().code == ErrorCode::UnsupportedCapability
                                  ? CatalogErrorCode::UnsupportedCapability
                              : loaded.error().code == ErrorCode::CannotRead
                                  ? CatalogErrorCode::CannotOpen
                                  : CatalogErrorCode::CorruptCatalog;
            return fail(code, loaded.error().message);
        }
        loaded_archives.push_back(std::move(*loaded));
    }
    if (const auto cleaned = (*arena)->cleanup(); !cleaned) {
        return std::unexpected(cleaned.error());
    }
    return loaded_archives;
}
#endif

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

struct PackCatalog::Impl final {
    detail::SecureScratchContext scratch_context;
#if defined(Q_OS_LINUX)
    CatalogAnchor anchor;
    CatalogNamespace admitted_namespace;
    std::unique_ptr<CatalogScratchArena> live_workspace;
#endif
    detail::CatalogHooks hooks;
    bool snapshot{};
    detail::CatalogOperation active_operation{detail::CatalogOperation::None};

#if defined(Q_OS_LINUX)
    Impl(detail::SecureScratchContext&& retained_scratch, CatalogAnchor&& retained_anchor,
         CatalogNamespace retained_namespace, detail::CatalogHooks retained_hooks, bool is_snapshot)
        : scratch_context(std::move(retained_scratch)), anchor(std::move(retained_anchor)),
          admitted_namespace(std::move(retained_namespace)), hooks(std::move(retained_hooks)),
          snapshot(is_snapshot) {}
#endif
};

struct PackCatalogSnapshot::Impl final {
    std::unique_ptr<PackCatalog> catalog;

    explicit Impl(std::unique_ptr<PackCatalog> private_catalog)
        : catalog(std::move(private_catalog)) {}
};

PackCatalog::PackCatalog(QString root_directory, QString connection_name)
    : root_directory_(std::move(root_directory)), connection_name_(std::move(connection_name)) {}

PackCatalog::PackCatalog(QString root_directory, QString connection_name,
                         std::unique_ptr<Impl> state)
    : root_directory_(std::move(root_directory)), connection_name_(std::move(connection_name)),
      impl_(std::move(state)) {}

PackCatalog::~PackCatalog() {
#if defined(Q_OS_LINUX)
    if (impl_ != nullptr && !impl_->snapshot) {
        auto root_lock =
            lockCatalogRoot(impl_->anchor.rootDescriptor(), impl_->anchor.absolute_root,
                            detail::CatalogLockMode::Exclusive,
                            detail::CatalogOperation::WritableDestruction, impl_->hooks, false);
        if (root_lock && revalidateCatalogAnchor(impl_->anchor)) {
            static_cast<void>(checkpointCatalog(database_, CatalogErrorCode::CannotOpen,
                                                detail::CatalogOperation::WritableDestruction,
                                                impl_->hooks));
            closeConnection();
            auto current = classifyCatalogNamespace(impl_->anchor, false, *root_lock,
                                                    detail::CatalogOperation::WritableDestruction,
                                                    impl_->hooks);
            if (current) {
                auto final_state = admitCatalogPrivately(
                    impl_->anchor, std::move(*current), detail::CatalogSchemaGeneration::Current,
                    impl_->scratch_context, *root_lock,
                    detail::CatalogOperation::WritableDestruction, impl_->hooks);
                if (final_state) {
                    impl_->admitted_namespace = std::move(*final_state);
                }
            }
            if (impl_->anchor.archives) {
                static_cast<void>(catalogFsync(impl_->anchor.archives->descriptor.get()));
            }
            if (impl_->anchor.blobs) {
                static_cast<void>(catalogFsync(impl_->anchor.blobs->descriptor.get()));
            }
            static_cast<void>(catalogFsync(impl_->anchor.rootDescriptor()));
            static_cast<void>(root_lock->release());
        } else {
            closeConnection();
        }
    } else {
        closeConnection();
    }
    if (impl_ != nullptr && impl_->live_workspace != nullptr) {
        static_cast<void>(impl_->live_workspace->validateForAmbientUse());
        static_cast<void>(impl_->live_workspace->admitSqliteDirectory(std::nullopt));
        static_cast<void>(impl_->live_workspace->cleanup());
    }
#else
    closeConnection();
#endif
}

void PackCatalog::closeConnection() {
    if (connection_name_.isEmpty()) {
        return;
    }
    const auto connection_name = std::exchange(connection_name_, {});
    const auto operation = impl_ != nullptr && impl_->snapshot
                               ? detail::CatalogOperation::SnapshotDestruction
                               : detail::CatalogOperation::WritableDestruction;
    detail::CatalogObservation closed;
    closed.event = detail::CatalogEvent::DatabaseClosed;
    closed.subject = detail::CatalogSubject::SQLiteConnection;
    closed.operation = operation;
    const auto closed_action = impl_ != nullptr ? catalogAction(impl_->hooks, closed)
                                                : detail::CatalogInjectedAction::Continue;
    if (database_.isValid()) {
        database_.close();
        database_ = QSqlDatabase{};
    }
    if (impl_ != nullptr) {
        static_cast<void>(catalogFinishes(impl_->hooks, closed, closed_action));
        auto reset = closed;
        reset.event = detail::CatalogEvent::DatabaseReset;
        const auto reset_action = catalogAction(impl_->hooks, reset);
        static_cast<void>(catalogFinishes(impl_->hooks, reset, reset_action));
    }
    QSqlDatabase::removeDatabase(connection_name);
    if (impl_ != nullptr) {
        auto removed = closed;
        removed.event = detail::CatalogEvent::DatabaseRemoved;
        const auto removed_action = catalogAction(impl_->hooks, removed);
        static_cast<void>(catalogFinishes(impl_->hooks, removed, removed_action));
    }
}

PackCatalogSnapshot::PackCatalogSnapshot(std::unique_ptr<Impl> state) : impl_(std::move(state)) {}

PackCatalogSnapshot::~PackCatalogSnapshot() = default;

std::expected<std::vector<InstalledPack>, CatalogError> PackCatalogSnapshot::list() const {
    return impl_->catalog->list();
}

std::expected<LoadedPack, CatalogError>
PackCatalogSnapshot::load(const model::PackId& id, const std::string& version) const {
    return impl_->catalog->load(id, version);
}

std::expected<ResolvedPack, CatalogError>
PackCatalogSnapshot::loadResolved(const model::PackRevision& exact_root) const {
    return impl_->catalog->loadResolved(exact_root);
}

std::expected<std::unique_ptr<PackCatalogSnapshot>, CatalogError>
PackCatalogSnapshot::openExisting(const QString& root_directory) {
    auto scratch = detail::acquireSecureScratchContext();
    if (!scratch) {
        return fail(CatalogErrorCode::CannotOpen, scratch.error().message);
    }
    return detail::PackCatalogSnapshotFactory::openExisting(root_directory, std::move(*scratch));
}

namespace detail {

std::expected<std::unique_ptr<PackCatalogSnapshot>, CatalogError>
PackCatalogSnapshotFactory::openExisting(const QString& root_directory,
                                         SecureScratchContext&& scratch_context) {
    return openExisting(root_directory, std::move(scratch_context), {});
}

std::expected<std::unique_ptr<PackCatalogSnapshot>, CatalogError>
PackCatalogSnapshotFactory::openExisting(const QString& root_directory,
                                         SecureScratchContext&& scratch_context,
                                         CatalogHooks hooks) {
    auto retained_scratch = std::move(scratch_context);
    if (!retained_scratch.isValid()) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Secure scratch context was consumed"));
    }
    if (hooks.report != nullptr) {
        ++hooks.report->scratch_acquisitions;
    }
    const auto reject = [&](CatalogError error)
        -> std::expected<std::unique_ptr<PackCatalogSnapshot>, CatalogError> {
        if (hooks.report != nullptr) {
            hooks.report->final_error = error.code;
        }
        return std::unexpected(std::move(error));
    };
#if !defined(Q_OS_LINUX)
    static_cast<void>(root_directory);
    return reject(CatalogError{CatalogErrorCode::CannotOpen,
                               QStringLiteral("Catalog snapshots require Linux")});
#else
    auto operand = resolveCatalogOperand(root_directory);
    if (!operand) {
        return reject(operand.error());
    }
    if (const auto feasible = validateCatalogGeneratedPathHeadroom(*operand); !feasible) {
        return reject(feasible.error());
    }
    auto anchor =
        retainCatalogRoot(std::move(*operand), false, false, CatalogOperation::SnapshotOpen, hooks);
    if (!anchor) {
        return reject(anchor.error());
    }
    auto root_lock =
        lockCatalogRoot(anchor->rootDescriptor(), anchor->absolute_root, CatalogLockMode::Shared,
                        CatalogOperation::SnapshotOpen, hooks);
    if (!root_lock) {
        return reject(root_lock.error());
    }
    if (anchor->adopted_mode_zero_root && !catalogDirectoryEmpty(anchor->rootDescriptor())) {
        return reject(CatalogError{CatalogErrorCode::CorruptCatalog,
                                   QStringLiteral("Adopted mode-zero catalog root is not empty")});
    }
    auto admitted =
        classifyCatalogNamespace(*anchor, false, *root_lock, CatalogOperation::SnapshotOpen, hooks);
    if (!admitted) {
        return reject(admitted.error());
    }
    if (admitted->shape == CatalogNamespaceShape::Empty) {
        static_cast<void>(root_lock->release());
        return reject(CatalogError{CatalogErrorCode::UninitializedCatalog,
                                   QStringLiteral("Catalog has not been initialized")});
    }
    const auto generation = admitted->shape == CatalogNamespaceShape::ClosedVersion1
                                ? CatalogSchemaGeneration::Version1
                                : CatalogSchemaGeneration::Current;
    auto arena =
        CatalogScratchArena::create(retained_scratch, CatalogOperation::SnapshotOpen, hooks);
    if (!arena) {
        return reject(arena.error());
    }
    auto capture = captureCatalogDatabase(*anchor, std::move(*admitted), false, *root_lock, **arena,
                                          CatalogOperation::SnapshotOpen, hooks);
    if (!capture) {
        return reject(capture.error());
    }
    auto candidate = openAndFingerprintCapturedCatalog(**arena, *capture, generation,
                                                       CatalogOperation::SnapshotOpen, hooks);
    if (!candidate) {
        return reject(candidate.error());
    }
    if (const auto reverse = reverseAdmitCapturedCatalog(
            *anchor, *capture, **arena, retained_scratch, (*candidate)->database(), generation,
            CatalogOperation::SnapshotOpen, hooks);
        !reverse) {
        return reject(reverse.error());
    }
    if (const auto closed = (*candidate)->close(); !closed) {
        return reject(closed.error());
    }
    if (const auto private_state = (*arena)->admitSqliteDirectory(capture->pass_a_directory);
        !private_state) {
        return reject(private_state.error());
    }
    auto final_source = repeatCatalogNamespace(*anchor, capture->source, false, *root_lock,
                                               CatalogOperation::SnapshotOpen, hooks);
    if (!final_source) {
        return reject(final_source.error());
    }
    if (generation == CatalogSchemaGeneration::Version1) {
        if (const auto released = root_lock->release(); !released) {
            return reject(released.error());
        }
        return reject(
            CatalogError{CatalogErrorCode::UninitializedCatalog,
                         QStringLiteral("Catalog schema version 1 requires writable migration")});
    }

    if (const auto promoted = (*arena)->moveFileToRoot(capture->pass_a_main.file_index,
                                                       QByteArrayLiteral("catalog.sqlite"));
        !promoted) {
        return reject(promoted.error());
    }
    if (capture->pass_a_wal && (*arena)->fileNamePresent(capture->pass_a_wal->file_index)) {
        if (const auto promoted = (*arena)->moveFileToRoot(capture->pass_a_wal->file_index,
                                                           QByteArrayLiteral("catalog.sqlite-wal"));
            !promoted) {
            return reject(promoted.error());
        }
    }
    if (const auto cleaned = (*arena)->cleanupTransientDirectories(); !cleaned) {
        return reject(cleaned.error());
    }
    std::size_t live_count = 0;
    const auto live_names = readCatalogDirectoryNames((*arena)->rootDescriptor(), live_count);
    const std::vector<QByteArray> expected_main{QByteArrayLiteral("catalog.sqlite")};
    const std::vector<QByteArray> expected_wal{QByteArrayLiteral("catalog.sqlite"),
                                               QByteArrayLiteral("catalog.sqlite-wal")};
    if (!live_names || (*live_names != expected_main && *live_names != expected_wal) ||
        !(*arena)->validateForAmbientUse()) {
        return reject(CatalogError{CatalogErrorCode::CorruptCatalog,
                                   QStringLiteral("Live private snapshot shape is invalid")});
    }

    auto state = std::make_unique<PackCatalog::Impl>(
        std::move(retained_scratch), std::move(*anchor), std::move(*final_source), hooks, true);
    (*arena)->rebindContext(state->scratch_context);
    state->live_workspace = std::move(*arena);
    const auto immutable_root = state->anchor.absolute_root;
    const auto connection_name = QStringLiteral("appellate-packs-snapshot-%1")
                                     .arg(QUuid::createUuid().toString(QUuid::Id128));
    auto catalog = std::unique_ptr<PackCatalog>(
        new PackCatalog(immutable_root, connection_name, std::move(state)));
    catalog->database_ =
        QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), catalog->connection_name_);
    catalog->database_.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    catalog->database_.setDatabaseName(catalog->impl_->live_workspace->rootPath() +
                                       QStringLiteral("/catalog.sqlite"));
    if (!catalog->impl_->live_workspace->validateForAmbientUse() || !catalog->database_.open()) {
        const auto message = catalog->database_.lastError().text();
        return reject(CatalogError{CatalogErrorCode::CannotOpen,
                                   message.isEmpty()
                                       ? QStringLiteral("Cannot open live private catalog snapshot")
                                       : message});
    }
    if (const auto configured = configureCatalogFingerprintConnection(
            catalog->database_, CatalogSqliteConfiguration{true, true, 5'000},
            CatalogSubject::LiveSnapshotWorkspace, hooks, CatalogOperation::SnapshotOpen);
        !configured) {
        return reject(configured.error());
    }
    if (const auto private_state =
            catalog->impl_->live_workspace->admitSqliteDirectory(std::nullopt);
        !private_state) {
        return reject(private_state.error());
    }
    if (const auto released = root_lock->release(); !released) {
        return reject(released.error());
    }
    auto snapshot_state = std::make_unique<PackCatalogSnapshot::Impl>(std::move(catalog));
    return std::unique_ptr<PackCatalogSnapshot>(new PackCatalogSnapshot(std::move(snapshot_state)));
#endif
}

std::expected<std::unique_ptr<PackCatalog>, CatalogError>
PackCatalogFactory::open(const QString& root_directory, SecureScratchContext&& scratch_context,
                         CatalogHooks hooks) {
    auto retained_scratch = std::move(scratch_context);
    if (!retained_scratch.isValid()) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Secure scratch context was consumed"));
    }
    if (hooks.report != nullptr) {
        ++hooks.report->scratch_acquisitions;
    }
    const auto reject =
        [&](CatalogError error) -> std::expected<std::unique_ptr<PackCatalog>, CatalogError> {
        if (hooks.report != nullptr) {
            hooks.report->final_error = error.code;
        }
        return std::unexpected(std::move(error));
    };
#if !defined(Q_OS_LINUX)
    static_cast<void>(root_directory);
    return reject(CatalogError{CatalogErrorCode::CannotOpen,
                               QStringLiteral("Secure writable catalogs require Linux")});
#else
    auto operand = resolveCatalogOperand(root_directory);
    if (!operand) {
        return reject(operand.error());
    }
    if (const auto feasible = validateCatalogGeneratedPathHeadroom(*operand); !feasible) {
        return reject(feasible.error());
    }
    auto anchor =
        retainCatalogRoot(std::move(*operand), true, true, CatalogOperation::WritableOpen, hooks);
    if (!anchor) {
        return reject(anchor.error());
    }
    auto root_lock =
        lockCatalogRoot(anchor->rootDescriptor(), anchor->absolute_root, CatalogLockMode::Exclusive,
                        CatalogOperation::WritableOpen, hooks);
    if (!root_lock) {
        return reject(root_lock.error());
    }
    if (anchor->adopted_mode_zero_root && !catalogDirectoryEmpty(anchor->rootDescriptor())) {
        return reject(CatalogError{CatalogErrorCode::CorruptCatalog,
                                   QStringLiteral("Adopted mode-zero catalog root is not empty")});
    }
    auto admitted =
        classifyCatalogNamespace(*anchor, false, *root_lock, CatalogOperation::WritableOpen, hooks);
    if (!admitted) {
        return reject(admitted.error());
    }
    const auto starting_shape = admitted->shape;
    if (starting_shape != CatalogNamespaceShape::Empty &&
        starting_shape != CatalogNamespaceShape::ClosedVersion1 &&
        starting_shape != CatalogNamespaceShape::Current) {
        return reject(CatalogError{CatalogErrorCode::CorruptCatalog,
                                   QStringLiteral("Catalog has no admissible writable shape")});
    }

    std::vector<LoadedPack> migration_archives;
    if (starting_shape == CatalogNamespaceShape::ClosedVersion1) {
        auto version_one = admitCatalogPrivately(
            *anchor, std::move(*admitted), CatalogSchemaGeneration::Version1, retained_scratch,
            *root_lock, CatalogOperation::WritableOpen, hooks);
        if (!version_one) {
            return reject(version_one.error());
        }
        admitted = std::move(version_one);
        auto loaded = loadAdmittedCatalogArchives(*anchor, *admitted, retained_scratch,
                                                  CatalogOperation::WritableOpen, hooks);
        if (!loaded) {
            return reject(loaded.error());
        }
        migration_archives = std::move(*loaded);
    }

    if (const auto normalized = normalizeRetainedCatalogDirectory(
            *anchor, nullptr, CatalogSubject::CatalogRoot, CatalogOperation::WritableOpen, hooks);
        !normalized) {
        return reject(normalized.error());
    }
    if (!root_lock->refreshAfterAuthorizedTransition(*anchor)) {
        return reject(
            CatalogError{CatalogErrorCode::CannotOpen,
                         QStringLiteral("Catalog root lock did not survive normalization")});
    }
    if (starting_shape != CatalogNamespaceShape::Empty) {
        if (const auto normalized = normalizeRetainedCatalogDirectory(
                *anchor, &*anchor->archives, CatalogSubject::ArchivesDirectory,
                CatalogOperation::WritableOpen, hooks);
            !normalized) {
            return reject(normalized.error());
        }
    }
    if (starting_shape == CatalogNamespaceShape::Current) {
        if (const auto normalized = normalizeRetainedCatalogDirectory(
                *anchor, &*anchor->blobs, CatalogSubject::BlobsDirectory,
                CatalogOperation::WritableOpen, hooks);
            !normalized) {
            return reject(normalized.error());
        }
    }

    bool created_archives = false;
    bool created_blobs = false;
    std::optional<NativeCatalogIdentity> created_archives_identity;
    std::optional<NativeCatalogIdentity> created_blobs_identity;
    std::optional<CatalogRetainedFile> created_main;
    std::optional<NativeCatalogIdentity> created_wal_identity;
    std::optional<NativeCatalogIdentity> created_shm_identity;
    std::optional<CatalogInstallLock> install_lock;
    bool persistent_state_reached = starting_shape == CatalogNamespaceShape::Current;
    bool initialization_cleanup_allowed = true;
    bool initialization_cleanup_completed = false;
    const auto preserve_initialization_cleanup = [&] {
        if (hooks.report != nullptr) {
            hooks.report->cleanup = CatalogCleanupOutcome::Preserved;
            hooks.report->remaining_ledger_paths = anchor->created_paths;
        }
    };
    const auto cleanup_initialization = [&]() -> std::expected<void, CatalogError> {
        if (initialization_cleanup_completed || persistent_state_reached) {
            return {};
        }
        initialization_cleanup_completed = true;
        if (persistent_state_reached) {
            return {};
        }
        if (!initialization_cleanup_allowed) {
            preserve_initialization_cleanup();
            return fail(CatalogErrorCode::CorruptCatalog,
                        QStringLiteral("Catalog initialization cleanup is ambiguous"));
        }
        bool cleaned = true;
        if (install_lock && install_lock->isHeld()) {
            if (const auto rebound = install_lock->revalidate(); !rebound) {
                initialization_cleanup_allowed = false;
                static_cast<void>(install_lock->release());
                preserve_initialization_cleanup();
                return std::unexpected(rebound.error());
            }
            if (const auto released = install_lock->release(); !released) {
                initialization_cleanup_allowed = false;
                preserve_initialization_cleanup();
                return released;
            }
        }
        const auto remove_sidecar =
            [&](QByteArray component, CatalogSubject subject,
                const std::optional<NativeCatalogIdentity>& created_identity)
            -> std::expected<void, CatalogError> {
            struct stat named{};
            if (::fstatat(anchor->rootDescriptor(), component.constData(), &named,
                          AT_SYMLINK_NOFOLLOW) != 0) {
                if (errno == ENOENT) {
                    return {};
                }
                return fail(CatalogErrorCode::CorruptCatalog,
                            QStringLiteral("Catalog sidecar cleanup is unprovable"));
            }
            if (!created_identity || !sameCatalogIdentity(*created_identity, named, true)) {
                return fail(CatalogErrorCode::CorruptCatalog,
                            QStringLiteral("Catalog sidecar cleanup binding changed"));
            }
            auto retained = retainCatalogFile(anchor->rootDescriptor(), anchor->absolute_root,
                                              std::move(component), subject, false,
                                              CatalogOperation::WritableOpen, hooks, false);
            if (!retained) {
                return std::unexpected(retained.error());
            }
            if (!sameCatalogTuple(*created_identity, retained->identity, true) ||
                !removeAttemptOwnedCatalogFile(anchor->rootDescriptor(), *retained)) {
                return fail(CatalogErrorCode::CorruptCatalog,
                            QStringLiteral("Catalog sidecar cleanup became ambiguous"));
            }
            return {};
        };
        if (starting_shape == CatalogNamespaceShape::Empty) {
            for (auto pending : {
                     std::tuple{QByteArrayLiteral("catalog.sqlite-journal"),
                                CatalogSubject::RollbackJournal,
                                std::optional<NativeCatalogIdentity>{}},
                     std::tuple{QByteArrayLiteral("catalog.sqlite-shm"),
                                CatalogSubject::DatabaseShm, created_shm_identity},
                     std::tuple{QByteArrayLiteral("catalog.sqlite-wal"),
                                CatalogSubject::DatabaseWal, created_wal_identity},
                 }) {
                auto removed = remove_sidecar(std::move(std::get<0>(pending)), std::get<1>(pending),
                                              std::get<2>(pending));
                if (!removed) {
                    preserve_initialization_cleanup();
                    return removed;
                }
            }
            if (created_main) {
                cleaned = removeAttemptOwnedCatalogFile(anchor->rootDescriptor(), *created_main) &&
                          cleaned;
            }
        }
        if (created_blobs && anchor->blobs) {
            cleaned =
                catalogDirectoryEmpty(anchor->blobs->descriptor.get()) &&
                removeExactCatalogDirectory(anchor->rootDescriptor(), anchor->blobs->component,
                                            *created_blobs_identity) &&
                cleaned;
        }
        if (created_archives && anchor->archives) {
            cleaned =
                catalogDirectoryEmpty(anchor->archives->descriptor.get()) &&
                removeExactCatalogDirectory(anchor->rootDescriptor(), anchor->archives->component,
                                            *created_archives_identity) &&
                cleaned;
        }
        if (starting_shape == CatalogNamespaceShape::Empty) {
            for (std::size_t index = anchor->controllers.size(); index-- > 1;) {
                const auto& controller = anchor->controllers.at(index);
                if (!controller.created_by_attempt) {
                    break;
                }
                const auto& parent = anchor->controllers.at(index - 1U);
                cleaned = catalogDirectoryEmpty(controller.descriptor.get()) &&
                          removeExactCatalogDirectory(parent.descriptor.get(), controller.component,
                                                      controller.identity) &&
                          cleaned;
            }
        }
        if (hooks.report != nullptr) {
            hooks.report->cleanup =
                cleaned ? CatalogCleanupOutcome::Removed : CatalogCleanupOutcome::Preserved;
            if (cleaned) {
                hooks.report->remaining_ledger_paths.clear();
            } else {
                hooks.report->remaining_ledger_paths = anchor->created_paths;
            }
        }
        if (!cleaned) {
            return fail(CatalogErrorCode::CorruptCatalog,
                        QStringLiteral("Catalog initialization cleanup became ambiguous"));
        }
        return {};
    };
    ScopeExit reconcile_initialization([&] { static_cast<void>(cleanup_initialization()); });
    const auto reject_after_initialization_cleanup =
        [&](CatalogError error) -> std::expected<std::unique_ptr<PackCatalog>, CatalogError> {
        if (const auto cleaned = cleanup_initialization(); !cleaned) {
            if (cleaned.error().code == CatalogErrorCode::CorruptCatalog) {
                error = cleaned.error();
            }
        }
        return reject(std::move(error));
    };

    if (starting_shape == CatalogNamespaceShape::Empty) {
        auto archives = createCatalogNamespaceDirectory(*anchor, QByteArrayLiteral("archives"),
                                                        CatalogSubject::ArchivesDirectory,
                                                        CatalogOperation::WritableOpen, hooks);
        if (!archives) {
            return reject_after_initialization_cleanup(archives.error());
        }
        anchor->archives = std::move(*archives);
        created_archives = true;
        created_archives_identity = anchor->archives->identity;
        auto blobs = createCatalogNamespaceDirectory(*anchor, QByteArrayLiteral("blobs"),
                                                     CatalogSubject::BlobsDirectory,
                                                     CatalogOperation::WritableOpen, hooks);
        if (!blobs) {
            return reject_after_initialization_cleanup(blobs.error());
        }
        anchor->blobs = std::move(*blobs);
        created_blobs = true;
        created_blobs_identity = anchor->blobs->identity;
        auto main = createCatalogDatabaseMain(*anchor, CatalogOperation::WritableOpen, hooks);
        if (!main) {
            return reject_after_initialization_cleanup(main.error());
        }
        created_main = std::move(*main);
    } else if (starting_shape == CatalogNamespaceShape::ClosedVersion1) {
        auto blobs = createCatalogNamespaceDirectory(*anchor, QByteArrayLiteral("blobs"),
                                                     CatalogSubject::BlobsDirectory,
                                                     CatalogOperation::WritableOpen, hooks);
        if (!blobs) {
            return reject_after_initialization_cleanup(blobs.error());
        }
        anchor->blobs = std::move(*blobs);
        created_blobs = true;
        created_blobs_identity = anchor->blobs->identity;
    }

    auto setup_state =
        classifyCatalogNamespace(*anchor, false, *root_lock, CatalogOperation::WritableOpen, hooks);
    if (!setup_state) {
        initialization_cleanup_allowed = false;
        return reject_after_initialization_cleanup(setup_state.error());
    }
    if (starting_shape == CatalogNamespaceShape::Empty) {
        constexpr std::array expected_empty_setup_names{
            QByteArrayView("archives"), QByteArrayView("blobs"), QByteArrayView("catalog.sqlite")};
        const auto valid_setup =
            setup_state->shape == CatalogNamespaceShape::Current &&
            std::ranges::equal(setup_state->root_names, expected_empty_setup_names) &&
            created_archives_identity && created_blobs_identity && created_main &&
            sameCatalogTuple(*created_archives_identity, setup_state->archives_identity, true) &&
            setup_state->blobs_identity &&
            sameCatalogTuple(*created_blobs_identity, *setup_state->blobs_identity, true) &&
            sameCatalogTuple(created_main->identity, setup_state->main.identity, true) &&
            setup_state->archives.empty() && setup_state->blobs.empty() && !setup_state->wal &&
            !setup_state->shm && !setup_state->legacy_lock;
        if (!valid_setup) {
            initialization_cleanup_allowed = false;
            return reject_after_initialization_cleanup(
                CatalogError{CatalogErrorCode::CorruptCatalog,
                             QStringLiteral("Initialized catalog ledger changed")});
        }
    } else if (!sameCatalogSetupTransition(*admitted, *setup_state, created_blobs_identity)) {
        initialization_cleanup_allowed = false;
        return reject_after_initialization_cleanup(CatalogError{
            CatalogErrorCode::CorruptCatalog, QStringLiteral("Catalog setup binding changed")});
    }

    auto acquired_install_lock = acquireCatalogInstallLock(
        anchor->absolute_root, anchor->rootDescriptor(), CatalogOperation::WritableOpen, hooks,
        CatalogErrorCode::CannotOpen);
    if (!acquired_install_lock) {
        return reject_after_initialization_cleanup(acquired_install_lock.error());
    }
    install_lock.emplace(std::move(*acquired_install_lock));
    const auto allowed_lock = nativeCatalogIdentity(install_lock->identity());
    if (!allowed_lock) {
        initialization_cleanup_allowed = false;
        static_cast<void>(install_lock->release());
        return reject_after_initialization_cleanup(
            CatalogError{CatalogErrorCode::CorruptCatalog,
                         QStringLiteral("Catalog mutation lock identity is invalid")});
    }
    const auto reject_after_lock =
        [&](CatalogError error) -> std::expected<std::unique_ptr<PackCatalog>, CatalogError> {
        if (install_lock && install_lock->isHeld()) {
            if (const auto rebound = install_lock->revalidate(); !rebound) {
                initialization_cleanup_allowed = false;
                error = rebound.error();
            }
            if (const auto released = install_lock->release(); !released) {
                initialization_cleanup_allowed = false;
                return reject_after_initialization_cleanup(released.error());
            }
        }
        return reject_after_initialization_cleanup(std::move(error));
    };

    auto structural = classifyCatalogNamespace(
        *anchor, false, *root_lock, CatalogOperation::WritableOpen, hooks, *allowed_lock);
    if (!structural) {
        if (structural.error().code == CatalogErrorCode::CorruptCatalog) {
            initialization_cleanup_allowed = false;
        }
        return reject_after_lock(structural.error());
    }
    if (!sameCatalogStateAcrossAttemptLock(*setup_state, *structural, *allowed_lock)) {
        initialization_cleanup_allowed = false;
        return reject_after_lock(
            CatalogError{CatalogErrorCode::CorruptCatalog,
                         QStringLiteral("Catalog binding changed across lock acquisition")});
    }
    if (starting_shape == CatalogNamespaceShape::Current) {
        auto current = admitCatalogPrivately(
            *anchor, std::move(*structural), CatalogSchemaGeneration::Current, retained_scratch,
            *root_lock, CatalogOperation::WritableOpen, hooks, *allowed_lock);
        if (!current) {
            return reject_after_lock(current.error());
        }
        structural = std::move(current);
    }
    if (const auto normalized = normalizeCatalogSqliteFiles(*anchor, *structural,
                                                            CatalogOperation::WritableOpen, hooks);
        !normalized) {
        return reject_after_lock(normalized.error());
    }
    const auto record_attempt_sidecars = [&](const CatalogNamespace& state) {
        if (starting_shape != CatalogNamespaceShape::Empty) {
            return true;
        }
        const auto record = [&](const std::optional<CatalogRetainedFile>& file,
                                std::optional<NativeCatalogIdentity>& ledger) {
            if (!file) {
                return true;
            }
            if (ledger && !sameCatalogTuple(*ledger, file->identity, false)) {
                return false;
            }
            ledger = file->identity;
            return true;
        };
        return record(state.wal, created_wal_identity) && record(state.shm, created_shm_identity);
    };
    if (!record_attempt_sidecars(*structural)) {
        initialization_cleanup_allowed = false;
        return reject_after_lock(CatalogError{CatalogErrorCode::CorruptCatalog,
                                              QStringLiteral("Catalog sidecar identity changed")});
    }
    const auto refresh_source_state = [&]() -> std::expected<void, CatalogError> {
        auto observed = classifyCatalogNamespace(
            *anchor, false, *root_lock, CatalogOperation::WritableOpen, hooks, *allowed_lock);
        if (!observed) {
            return std::unexpected(observed.error());
        }
        if (const auto normalized = normalizeCatalogSqliteFiles(
                *anchor, *observed, CatalogOperation::WritableOpen, hooks);
            !normalized) {
            return normalized;
        }
        if (!sameCatalogPersistentBindingsAcrossSqlite(*structural, *observed) ||
            !record_attempt_sidecars(*observed)) {
            return fail(CatalogErrorCode::CorruptCatalog,
                        QStringLiteral("Catalog binding changed across SQLite use"));
        }
        structural = std::move(observed);
        return {};
    };

    const auto immutable_root = anchor->absolute_root;
    const auto connection_name =
        QStringLiteral("appellate-packs-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    auto catalog = std::unique_ptr<PackCatalog>(new PackCatalog(immutable_root, connection_name));
    const auto close_opening_connection = [&]() -> std::expected<void, CatalogError> {
        if (catalog->connection_name_.isEmpty()) {
            return {};
        }
        std::optional<CatalogError> lifecycle_error;
        const auto record_lifecycle_failure = [&](QString message) {
            if (!lifecycle_error) {
                lifecycle_error = CatalogError{CatalogErrorCode::CannotOpen, std::move(message)};
            }
        };
        CatalogObservation queries_finished;
        queries_finished.event = CatalogEvent::QueriesFinished;
        queries_finished.subject = CatalogSubject::SQLiteConnection;
        queries_finished.operation = CatalogOperation::WritableOpen;
        queries_finished.absolute_path = immutable_root + QStringLiteral("/catalog.sqlite");
        const auto queries_action = catalogAction(hooks, queries_finished);
        if (catalogFailsBefore(queries_action) ||
            !catalogFinishes(hooks, queries_finished, queries_action)) {
            record_lifecycle_failure(
                QStringLiteral("Injected writable query-finalization failure"));
        }

        auto closed = queries_finished;
        closed.event = CatalogEvent::DatabaseClosed;
        const auto closed_action = catalogAction(hooks, closed);
        if (catalogFailsBefore(closed_action)) {
            record_lifecycle_failure(QStringLiteral("Injected writable database-close failure"));
        }
        if (catalog->database_.isValid()) {
            catalog->database_.close();
        }
        if (!catalogFinishes(hooks, closed, closed_action)) {
            record_lifecycle_failure(QStringLiteral("Injected writable database-close failure"));
        }

        auto inventoried = closed;
        inventoried.event = CatalogEvent::SidecarsInventoried;
        const auto inventory_action = catalogAction(hooks, inventoried);
        std::expected<void, CatalogError> inventory_result;
        if (catalogFailsBefore(inventory_action)) {
            inventory_result = fail(CatalogErrorCode::CannotOpen,
                                    QStringLiteral("Injected post-close inventory failure"));
        } else {
            inventory_result = refresh_source_state();
        }
        if (!catalogFinishes(hooks, inventoried, inventory_action) && inventory_result) {
            inventory_result = fail(CatalogErrorCode::CannotOpen,
                                    QStringLiteral("Injected post-close inventory failure"));
        }

        const auto removed_connection_name = std::exchange(catalog->connection_name_, {});
        catalog->database_ = QSqlDatabase{};
        auto reset = closed;
        reset.event = CatalogEvent::DatabaseReset;
        const auto reset_action = catalogAction(hooks, reset);
        if (catalogFailsBefore(reset_action) || !catalogFinishes(hooks, reset, reset_action)) {
            record_lifecycle_failure(QStringLiteral("Injected writable database-reset failure"));
        }
        if (QSqlDatabase::contains(removed_connection_name)) {
            QSqlDatabase::removeDatabase(removed_connection_name);
        }
        auto removed = closed;
        removed.event = CatalogEvent::DatabaseRemoved;
        const auto removed_action = catalogAction(hooks, removed);
        if (catalogFailsBefore(removed_action) ||
            !catalogFinishes(hooks, removed, removed_action)) {
            record_lifecycle_failure(QStringLiteral("Injected writable database-removal failure"));
        }
        if (QSqlDatabase::contains(removed_connection_name)) {
            return fail(CatalogErrorCode::CorruptCatalog,
                        QStringLiteral("Writable database connection registry entry survived"));
        }
        if (!inventory_result) {
            return std::unexpected(inventory_result.error());
        }
        if (lifecycle_error) {
            return std::unexpected(std::move(*lifecycle_error));
        }
        return {};
    };
    const auto reject_open_attempt =
        [&](CatalogError error) -> std::expected<std::unique_ptr<PackCatalog>, CatalogError> {
        if (error.code == CatalogErrorCode::CorruptCatalog) {
            initialization_cleanup_allowed = false;
        }
        if (install_lock && install_lock->isHeld()) {
            if (const auto rebound = install_lock->revalidate(); !rebound) {
                initialization_cleanup_allowed = false;
                error = rebound.error();
            }
        }
        const auto closed = close_opening_connection();
        if (!closed && closed.error().code == CatalogErrorCode::CorruptCatalog) {
            initialization_cleanup_allowed = false;
        }
        if (closed && persistent_state_reached) {
            auto closed_state = classifyCatalogNamespace(
                *anchor, false, *root_lock, CatalogOperation::WritableOpen, hooks, *allowed_lock);
            if (!closed_state) {
                initialization_cleanup_allowed = false;
                error = closed_state.error();
            } else {
                auto reconciled = admitCatalogPrivately(
                    *anchor, std::move(*closed_state), CatalogSchemaGeneration::Current,
                    retained_scratch, *root_lock, CatalogOperation::WritableOpen, hooks,
                    *allowed_lock);
                if (!reconciled) {
                    initialization_cleanup_allowed = false;
                    error = reconciled.error();
                }
            }
        }
        if (install_lock && install_lock->isHeld()) {
            if (const auto released = install_lock->release(); !released) {
                initialization_cleanup_allowed = false;
                return reject_after_initialization_cleanup(released.error());
            }
        }
        if (closed && persistent_state_reached && initialization_cleanup_allowed) {
            auto unlocked_state = classifyCatalogNamespace(*anchor, false, *root_lock,
                                                           CatalogOperation::WritableOpen, hooks);
            if (!unlocked_state) {
                initialization_cleanup_allowed = false;
                error = unlocked_state.error();
            } else {
                auto reconciled = admitCatalogPrivately(
                    *anchor, std::move(*unlocked_state), CatalogSchemaGeneration::Current,
                    retained_scratch, *root_lock, CatalogOperation::WritableOpen, hooks);
                if (!reconciled) {
                    initialization_cleanup_allowed = false;
                    error = reconciled.error();
                }
            }
        }
        if (!closed) {
            return reject_after_initialization_cleanup(closed.error());
        }
        return reject_after_initialization_cleanup(std::move(error));
    };

    CatalogObservation added;
    added.event = CatalogEvent::DatabaseAdded;
    added.subject = CatalogSubject::SQLiteConnection;
    added.operation = CatalogOperation::WritableOpen;
    added.absolute_path = immutable_root + QStringLiteral("/catalog.sqlite");
    const auto added_action = catalogAction(hooks, added);
    if (catalogFailsBefore(added_action)) {
        return reject_after_lock(
            CatalogError{CatalogErrorCode::CannotOpen,
                         QStringLiteral("Injected writable database creation failure")});
    }
    catalog->database_ =
        QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), catalog->connection_name_);
    catalog->database_.setDatabaseName(added.absolute_path);
    if (!catalogFinishes(hooks, added, added_action)) {
        return reject_open_attempt(
            CatalogError{CatalogErrorCode::CannotOpen,
                         QStringLiteral("Injected writable database creation failure")});
    }

    if (const auto rebound = revalidateCatalogNamespaceBindings(
            *anchor, *structural, CatalogOperation::WritableOpen, hooks);
        !rebound) {
        return reject_open_attempt(rebound.error());
    }
    CatalogObservation opened = added;
    opened.event = CatalogEvent::DatabaseOpened;
    const auto opened_action = catalogAction(hooks, opened);
    if (catalogFailsBefore(opened_action)) {
        return reject_open_attempt(
            CatalogError{CatalogErrorCode::CannotOpen,
                         QStringLiteral("Injected writable database open failure")});
    }
    if (const auto rebound = revalidateCatalogNamespaceBindings(
            *anchor, *structural, CatalogOperation::WritableOpen, hooks);
        !rebound) {
        return reject_open_attempt(rebound.error());
    }
    if (!catalog->database_.open()) {
        const auto message = catalog->database_.lastError().text();
        return reject_open_attempt(CatalogError{
            CatalogErrorCode::CannotOpen,
            message.isEmpty() ? QStringLiteral("Cannot open retained writable catalog") : message});
    }
    if (const auto refreshed = refresh_source_state(); !refreshed) {
        initialization_cleanup_allowed = false;
        return reject_open_attempt(refreshed.error());
    }
    const auto open_lifecycle_finished = catalogFinishes(hooks, opened, opened_action);
    if (const auto refreshed = refresh_source_state(); !refreshed) {
        initialization_cleanup_allowed = false;
        return reject_open_attempt(refreshed.error());
    }
    if (!open_lifecycle_finished) {
        return reject_open_attempt(
            CatalogError{CatalogErrorCode::CannotOpen,
                         QStringLiteral("Injected writable database open failure")});
    }
    const auto fingerprint_configuration = configureCatalogFingerprintConnection(
        catalog->database_, CatalogSqliteConfiguration{true, false, 5'000},
        CatalogSubject::SQLiteConnection, hooks, CatalogOperation::WritableOpen);
    const auto after_fingerprint_configuration = refresh_source_state();
    if (!after_fingerprint_configuration) {
        initialization_cleanup_allowed = false;
        return reject_open_attempt(after_fingerprint_configuration.error());
    }
    if (!fingerprint_configuration) {
        return reject_open_attempt(fingerprint_configuration.error());
    }
    const auto writable_configuration = catalog->configure();
    const auto after_writable_configuration = refresh_source_state();
    if (!after_writable_configuration) {
        initialization_cleanup_allowed = false;
        return reject_open_attempt(after_writable_configuration.error());
    }
    if (!writable_configuration) {
        return reject_open_attempt(writable_configuration.error());
    }
    std::optional<CatalogError> post_migration_error;
    if (starting_shape != CatalogNamespaceShape::Current) {
        bool migration_commit_applied = false;
        bool migration_commit_ambiguous = false;
        const auto migrated = catalog->migrate(
            &migration_archives, hooks, &migration_commit_applied, &migration_commit_ambiguous);
        if (!migrated && !migration_commit_applied && !migration_commit_ambiguous) {
            catalog->rollback();
        }
        const auto after_migration = refresh_source_state();
        if (!after_migration) {
            initialization_cleanup_allowed = false;
            return reject_open_attempt(after_migration.error());
        }
        if (!migrated) {
            if (migration_commit_ambiguous) {
                initialization_cleanup_allowed = false;
                return reject_open_attempt(
                    CatalogError{CatalogErrorCode::CorruptCatalog,
                                 QStringLiteral("Catalog migration COMMIT outcome is ambiguous")});
            }
            if (migration_commit_applied) {
                persistent_state_reached = true;
                post_migration_error = CatalogError{
                    CatalogErrorCode::CannotOpen,
                    QStringLiteral("Catalog migration committed but reported failure")};
            } else {
                return reject_open_attempt(migrated.error());
            }
        } else {
            persistent_state_reached = true;
        }
    }

    if (const auto checkpointed =
            checkpointCatalog(catalog->database_, CatalogErrorCode::CannotOpen,
                              CatalogOperation::WritableOpen, hooks);
        !checkpointed) {
        return reject_open_attempt(checkpointed.error());
    }
    if (const auto refreshed = refresh_source_state(); !refreshed) {
        initialization_cleanup_allowed = false;
        return reject_open_attempt(refreshed.error());
    }

    if (post_migration_error) {
        return reject_open_attempt(std::move(*post_migration_error));
    }

    auto after_sqlite = classifyCatalogNamespace(
        *anchor, false, *root_lock, CatalogOperation::WritableOpen, hooks, *allowed_lock);
    if (!after_sqlite) {
        return reject_open_attempt(after_sqlite.error());
    }
    if (!sameCatalogPersistentBindingsAcrossSqlite(*structural, *after_sqlite)) {
        return reject_open_attempt(
            CatalogError{CatalogErrorCode::CorruptCatalog,
                         QStringLiteral("Catalog binding changed across SQLite use")});
    }
    if (const auto released = install_lock->release(); !released) {
        initialization_cleanup_allowed = false;
        return reject(released.error());
    }
    auto without_lock =
        classifyCatalogNamespace(*anchor, false, *root_lock, CatalogOperation::WritableOpen, hooks);
    if (!without_lock) {
        return reject(without_lock.error());
    }
    auto final_state =
        admitCatalogPrivately(*anchor, std::move(*without_lock), CatalogSchemaGeneration::Current,
                              retained_scratch, *root_lock, CatalogOperation::WritableOpen, hooks);
    if (!final_state) {
        return reject(final_state.error());
    }
    if (const auto released = root_lock->release(); !released) {
        return reject(released.error());
    }
    catalog->impl_ = std::make_unique<PackCatalog::Impl>(
        std::move(retained_scratch), std::move(*anchor), std::move(*final_state), hooks, false);
    reconcile_initialization.dismiss();
    return catalog;
#endif
}

} // namespace detail

std::expected<std::unique_ptr<PackCatalog>, CatalogError>
PackCatalog::open(const QString& root_directory) {
    auto scratch = detail::acquireSecureScratchContext();
    if (!scratch) {
        return fail(CatalogErrorCode::CannotOpen, scratch.error().message);
    }
    return detail::PackCatalogFactory::open(root_directory, std::move(*scratch), {});
}

std::expected<void, CatalogError> PackCatalog::configure() {
    constexpr std::pair<const char*, const char*> statements[]{
        {"PRAGMA journal_mode = WAL", "enable WAL mode"},
        {"PRAGMA synchronous = FULL", "enable full synchronization"},
    };
    for (const auto& [sql, action] : statements) {
        if (const auto result =
                execStatement(database_, QLatin1StringView(sql), CatalogErrorCode::CannotOpen,
                              QLatin1StringView(action));
            !result) {
            return result;
        }
    }
    QSqlQuery journal(database_);
    if (!journal.exec(QStringLiteral("PRAGMA journal_mode")) || !journal.next() ||
        journal.value(0).toString().compare(QStringLiteral("wal"), Qt::CaseInsensitive) != 0 ||
        journal.next()) {
        return queryFailure(CatalogErrorCode::CannotOpen, journal,
                            QStringLiteral("verify WAL mode"));
    }
    QSqlQuery synchronous(database_);
    bool converted = false;
    if (!synchronous.exec(QStringLiteral("PRAGMA synchronous")) || !synchronous.next() ||
        synchronous.value(0).toLongLong(&converted) != 2 || !converted || synchronous.next()) {
        return queryFailure(CatalogErrorCode::CannotOpen, synchronous,
                            QStringLiteral("verify full synchronization"));
    }
    return {};
}

std::expected<void, CatalogError>
PackCatalog::migrate(const std::vector<LoadedPack>* admitted_archives,
                     const detail::CatalogHooks& hooks, bool* commit_applied,
                     bool* commit_ambiguous) {
    if (commit_applied == nullptr || commit_ambiguous == nullptr) {
        return fail(CatalogErrorCode::MigrationFailed,
                    QStringLiteral("Catalog migration state is unavailable"));
    }
    *commit_applied = false;
    *commit_ambiguous = false;
    detail::CatalogObservation started;
    started.event = detail::CatalogEvent::MigrationStarted;
    started.subject = detail::CatalogSubject::SQLiteConnection;
    started.operation = detail::CatalogOperation::WritableOpen;
    started.absolute_path = QDir(root_directory_).filePath(QStringLiteral("catalog.sqlite"));
    const auto started_action = catalogAction(hooks, started);
    if (catalogFailsBefore(started_action)) {
        return fail(CatalogErrorCode::MigrationFailed,
                    QStringLiteral("Injected failure before catalog migration"));
    }
    if (const auto begun = execStatement(database_, QStringLiteral("BEGIN IMMEDIATE"),
                                         CatalogErrorCode::MigrationFailed,
                                         QStringLiteral("begin catalog migration"));
        !begun) {
        return begun;
    }
    if (!catalogFinishes(hooks, started, started_action)) {
        rollback();
        return fail(CatalogErrorCode::MigrationFailed,
                    QStringLiteral("Injected catalog migration-start failure"));
    }
    for (const auto* statement : detail::catalog_version_one_schema) {
        if (const auto result = execStatement(database_, QLatin1StringView(statement),
                                              CatalogErrorCode::MigrationFailed,
                                              QStringLiteral("apply catalog schema"));
            !result) {
            rollback();
            return result;
        }
    }
    for (const auto* statement : detail::catalog_version_two_schema) {
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
        if (admitted_archives != nullptr && admitted_archives->size() != installed.size()) {
            rollback();
            return fail(CatalogErrorCode::MigrationFailed,
                        QStringLiteral("Admitted v1 archives do not cover every catalog row"));
        }
        for (const auto& item : installed) {
            if (!validDigest(item.archive_sha256)) {
                rollback();
                return fail(CatalogErrorCode::MigrationFailed,
                            QStringLiteral("Cannot migrate corrupt archive metadata"));
            }
            std::optional<LoadedPack> ambient_loaded;
            const LoadedPack* loaded = nullptr;
            if (admitted_archives != nullptr) {
                const auto found =
                    std::ranges::find(*admitted_archives, item.revision, &LoadedPack::revision);
                if (found != admitted_archives->end()) {
                    loaded = &*found;
                }
            } else {
                const auto archive_path =
                    QDir(archivesDirectory())
                        .filePath(item.archive_sha256 + QStringLiteral(".awpack"));
                const auto archive_hash = hashArchiveFile(archive_path);
                auto imported = PackArchive::importArchive(archive_path, {},
                                                           PackValidationScope::ResolvedClosure);
                if (archive_hash && *archive_hash == item.archive_sha256 && imported) {
                    ambient_loaded = std::move(*imported);
                    loaded = &*ambient_loaded;
                }
            }
            if (loaded == nullptr || loaded->revision != item.revision) {
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
        record.addBindValue(QLatin1StringView(detail::catalog_migration_timestamp));
        if (!record.exec()) {
            rollback();
            return queryFailure(CatalogErrorCode::MigrationFailed, record,
                                QStringLiteral("record catalog migration"));
        }
    }
    detail::CatalogObservation attempted;
    attempted.event = detail::CatalogEvent::MigrationCommitAttempted;
    attempted.subject = detail::CatalogSubject::SQLiteConnection;
    attempted.operation = detail::CatalogOperation::WritableOpen;
    attempted.absolute_path = started.absolute_path;
    const auto attempted_action = catalogAction(hooks, attempted);
    if (catalogFailsBefore(attempted_action)) {
        rollback();
        return fail(CatalogErrorCode::MigrationFailed,
                    QStringLiteral("Injected failure before catalog migration COMMIT"));
    }
    if (const auto committed =
            execStatement(database_, QStringLiteral("COMMIT"), CatalogErrorCode::MigrationFailed,
                          QStringLiteral("commit catalog migration"));
        !committed) {
        *commit_ambiguous = true;
        return committed;
    }
    *commit_applied = true;
    if (!catalogFinishes(hooks, attempted, attempted_action)) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Catalog migration COMMIT reported failure"));
    }
    auto finished = attempted;
    finished.event = detail::CatalogEvent::MigrationCommitted;
    const auto finished_action = catalogAction(hooks, finished);
    if (catalogFailsBefore(finished_action) || !catalogFinishes(hooks, finished, finished_action)) {
        return fail(CatalogErrorCode::CannotOpen,
                    QStringLiteral("Injected catalog post-COMMIT failure"));
    }
    return {};
}

QString PackCatalog::archivesDirectory() const {
    return QDir(root_directory_).filePath(QStringLiteral("archives"));
}

QString PackCatalog::blobObjectsDirectory() const {
    return QDir(root_directory_).filePath(QStringLiteral("blobs"));
}

QString PackCatalog::rootDirectory() const { return root_directory_; }

std::expected<InstalledPack, CatalogError>
PackCatalog::installArchive(const QString& archive_path, const QString& installed_at_utc) {
    if (!validText(installed_at_utc)) {
        return fail(CatalogErrorCode::InvalidConfiguration,
                    QStringLiteral("Installed time is missing or invalid"));
    }
#if !defined(Q_OS_LINUX)
    static_cast<void>(archive_path);
    return fail(CatalogErrorCode::CannotStoreArchive,
                QStringLiteral("Secure catalog installation requires Linux"));
#else
    if (impl_ == nullptr || impl_->snapshot || !database_.isOpen()) {
        return fail(CatalogErrorCode::CannotStoreArchive,
                    QStringLiteral("Writable catalog state is unavailable"));
    }
    const auto previous_operation = impl_->active_operation;
    impl_->active_operation = detail::CatalogOperation::InstallArchive;
    ScopeExit restore_operation(
        [this, previous_operation] { impl_->active_operation = previous_operation; });
    auto root_lock = lockCatalogRoot(impl_->anchor.rootDescriptor(), impl_->anchor.absolute_root,
                                     detail::CatalogLockMode::Exclusive,
                                     detail::CatalogOperation::InstallArchive, impl_->hooks);
    if (!root_lock) {
        return std::unexpected(root_lock.error());
    }
    auto admitted = classifyCatalogNamespace(
        impl_->anchor, false, *root_lock, detail::CatalogOperation::InstallArchive, impl_->hooks);
    if (!admitted) {
        return std::unexpected(admitted.error());
    }
    if (admitted->shape != detail::CatalogNamespaceShape::Current) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Writable catalog is not in the current schema shape"));
    }
    if (const auto rebound = revalidateCatalogNamespaceBindings(
            impl_->anchor, impl_->admitted_namespace, detail::CatalogOperation::InstallArchive,
            impl_->hooks, false);
        !rebound) {
        return std::unexpected(rebound.error());
    }
    if (const auto normalized = normalizeRetainedCatalogDirectory(
            impl_->anchor, nullptr, detail::CatalogSubject::CatalogRoot,
            detail::CatalogOperation::InstallArchive, impl_->hooks);
        !normalized) {
        return std::unexpected(normalized.error());
    }
    if (!root_lock->refreshAfterAuthorizedTransition(impl_->anchor)) {
        return fail(CatalogErrorCode::CannotStoreArchive,
                    QStringLiteral("Catalog lock did not survive directory normalization"));
    }
    if (const auto normalized = normalizeRetainedCatalogDirectory(
            impl_->anchor, &*impl_->anchor.archives, detail::CatalogSubject::ArchivesDirectory,
            detail::CatalogOperation::InstallArchive, impl_->hooks);
        !normalized) {
        return std::unexpected(normalized.error());
    }
    if (const auto normalized = normalizeRetainedCatalogDirectory(
            impl_->anchor, &*impl_->anchor.blobs, detail::CatalogSubject::BlobsDirectory,
            detail::CatalogOperation::InstallArchive, impl_->hooks);
        !normalized) {
        return std::unexpected(normalized.error());
    }
    auto install_lock = detail::acquireCatalogInstallLock(
        impl_->anchor.absolute_root, impl_->anchor.rootDescriptor(),
        detail::CatalogOperation::InstallArchive, impl_->hooks,
        CatalogErrorCode::CannotStoreArchive);
    if (!install_lock) {
        return std::unexpected(install_lock.error());
    }
    const auto allowed_lock = nativeCatalogIdentity(install_lock->identity());
    if (!allowed_lock) {
        const auto released = install_lock->release();
        return released ? fail(CatalogErrorCode::CorruptCatalog,
                               QStringLiteral("Catalog mutation lock identity is invalid"))
                        : std::unexpected(released.error());
    }
    bool transaction_started = false;
    const auto finalize_install_state = [&]() -> std::expected<void, CatalogError> {
        if (install_lock->isHeld()) {
            if (const auto rebound = install_lock->revalidate(); !rebound) {
                static_cast<void>(install_lock->release());
                return std::unexpected(rebound.error());
            }
            if (const auto released = install_lock->release(); !released) {
                return released;
            }
        }
        auto unlocked_state =
            classifyCatalogNamespace(impl_->anchor, false, *root_lock,
                                     detail::CatalogOperation::InstallArchive, impl_->hooks);
        if (!unlocked_state) {
            return std::unexpected(unlocked_state.error());
        }
        auto final_state = admitCatalogPrivately(
            impl_->anchor, std::move(*unlocked_state), detail::CatalogSchemaGeneration::Current,
            impl_->scratch_context, *root_lock, detail::CatalogOperation::InstallArchive,
            impl_->hooks);
        if (!final_state) {
            return std::unexpected(final_state.error());
        }
        impl_->admitted_namespace = std::move(*final_state);
        if (const auto released = root_lock->release(); !released) {
            return fail(released.error().code == CatalogErrorCode::CorruptCatalog
                            ? CatalogErrorCode::CorruptCatalog
                            : CatalogErrorCode::CannotStoreArchive,
                        released.error().message);
        }
        return {};
    };
    const auto reject_unpublished =
        [&](CatalogError error) -> std::expected<InstalledPack, CatalogError> {
        if (transaction_started) {
            rollback();
            transaction_started = false;
        }
        if (const auto finalized = finalize_install_state(); !finalized) {
            return std::unexpected(finalized.error());
        }
        return std::unexpected(std::move(error));
    };
    auto locked_state = classifyCatalogNamespace(impl_->anchor, false, *root_lock,
                                                 detail::CatalogOperation::InstallArchive,
                                                 impl_->hooks, *allowed_lock);
    if (!locked_state) {
        return reject_unpublished(locked_state.error());
    }
    auto verified_state = admitCatalogPrivately(
        impl_->anchor, std::move(*locked_state), detail::CatalogSchemaGeneration::Current,
        impl_->scratch_context, *root_lock, detail::CatalogOperation::InstallArchive, impl_->hooks,
        *allowed_lock);
    if (!verified_state) {
        return reject_unpublished(verified_state.error());
    }

    auto staged = stageArchive(archive_path, archivesDirectory());
    if (!staged) {
        return reject_unpublished(staged.error());
    }
    std::optional<PublishedPath> published_archive;
    std::vector<std::pair<QString, PublishedPath>> published_blobs;
    const auto cleanup_publications = [&] {
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
        static_cast<void>(QFile::remove(staged->path));
    };
    ScopeExit rollback_publications(cleanup_publications);
    const auto reject_published =
        [&](CatalogError error) -> std::expected<InstalledPack, CatalogError> {
        if (const auto rebound = install_lock->revalidate(); !rebound) {
            if (transaction_started) {
                rollback();
                transaction_started = false;
            }
            rollback_publications.dismiss();
            const auto released = install_lock->release();
            static_cast<void>(root_lock->release());
            return released ? std::unexpected(rebound.error()) : std::unexpected(released.error());
        }
        cleanup_publications();
        rollback_publications.dismiss();
        if (const auto finalized = finalize_install_state(); !finalized) {
            return std::unexpected(finalized.error());
        }
        return std::unexpected(std::move(error));
    };
    const auto& revision = staged->loaded.revision;
    if (!validText(asQString(revision.id.value)) || !validText(asQString(revision.version)) ||
        !validDigest(asQString(revision.digest)) || !validDigest(staged->sha256)) {
        return reject_published(CatalogError{CatalogErrorCode::ArchiveInvalid,
                                             QStringLiteral("Archive revision is invalid")});
    }
    for (const auto& dependency : staged->loaded.dependencies) {
        if (dependency.revision.id == revision.id &&
            dependency.revision.version == revision.version) {
            return reject_published(
                CatalogError{CatalogErrorCode::DependencyCycle,
                             QStringLiteral("Pack cannot depend on its own ID and version")});
        }
    }

    const auto candidate =
        resolveClosure(revision, &staged->loaded, detail::CatalogOperation::InstallArchive);
    if (!candidate) {
        return reject_published(candidate.error());
    }

    QSqlQuery preexisting(database_);
    preexisting.prepare(
        QStringLiteral("SELECT digest, archive_sha256, installed_at_utc FROM pack_revisions "
                       "WHERE pack_id = ? AND version = ?"));
    preexisting.addBindValue(asQString(revision.id.value));
    preexisting.addBindValue(asQString(revision.version));
    if (!preexisting.exec()) {
        return reject_published(queryFailure(CatalogErrorCode::QueryFailed, preexisting,
                                             QStringLiteral("preflight immutable pack version"))
                                    .error());
    }
    if (preexisting.next()) {
        const auto existing_digest = preexisting.value(0).toString();
        const auto existing_archive = preexisting.value(1).toString();
        const auto existing_installed_at = preexisting.value(2).toString();
        if (existing_digest != asQString(revision.digest) || !validDigest(existing_archive) ||
            preexisting.next()) {
            return reject_published(CatalogError{
                CatalogErrorCode::ImmutableConflict,
                QStringLiteral("Pack ID and version already identify another revision")});
        }
        const auto resolved = loadResolved(revision);
        if (!resolved) {
            return reject_published(resolved.error());
        }
        if (normalizedDependencies(resolved->root().dependencies) !=
                normalizedDependencies(staged->loaded.dependencies) ||
            resolved->root().blobs != staged->loaded.blobs) {
            return reject_published(
                CatalogError{CatalogErrorCode::CorruptCatalog,
                             QStringLiteral("Installed pack differs from staged revision")});
        }
        const auto installed = InstalledPack{revision, existing_archive, existing_installed_at,
                                             resolved->root().dependencies};
        cleanup_publications();
        rollback_publications.dismiss();
        if (const auto finalized = finalize_install_state(); !finalized) {
            return std::unexpected(finalized.error());
        }
        return installed;
    }

    PublishedPath attempted_archive;
    const auto final_path = installStagedFile(*staged, archivesDirectory(), &attempted_archive);
    if (attempted_archive.newly_created) {
        published_archive = attempted_archive;
    }
    if (!final_path) {
        return reject_published(final_path.error());
    }
    published_archive = *final_path;
    const auto archive_hash_before = hashArchiveFile(final_path->path);
    if (!archive_hash_before || *archive_hash_before != staged->sha256) {
        return reject_published(CatalogError{
            CatalogErrorCode::CannotStoreArchive,
            QStringLiteral("Installed archive object changed before blob materialization")});
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
            return reject_published(materialized.error());
        }
    }
    const auto archive_hash_after = hashArchiveFile(final_path->path);
    if (!archive_hash_after || *archive_hash_after != staged->sha256) {
        return reject_published(CatalogError{
            CatalogErrorCode::CannotStoreArchive,
            QStringLiteral("Installed archive object changed during blob materialization")});
    }

    if (const auto begun = beginImmediate(); !begun) {
        return reject_published(begun.error());
    }
    transaction_started = true;
    QSqlQuery existing(database_);
    existing.prepare(
        QStringLiteral("SELECT digest, archive_sha256, installed_at_utc FROM pack_revisions "
                       "WHERE pack_id = ? AND version = ?"));
    existing.addBindValue(asQString(revision.id.value));
    existing.addBindValue(asQString(revision.version));
    if (!existing.exec()) {
        return reject_published(queryFailure(CatalogErrorCode::QueryFailed, existing,
                                             QStringLiteral("check immutable pack version"))
                                    .error());
    }
    if (existing.next()) {
        if (existing.value(0).toString() != asQString(revision.digest)) {
            return reject_published(CatalogError{
                CatalogErrorCode::ImmutableConflict,
                QStringLiteral("Pack ID and version already identify another revision")});
        }
        const auto recorded_blobs = blobsFor(database_, revision);
        if (!recorded_blobs || *recorded_blobs != staged->loaded.blobs) {
            return reject_published(
                CatalogError{CatalogErrorCode::CorruptCatalog,
                             QStringLiteral("Installed pack blob descriptors are corrupt")});
        }
        const auto resolved = loadResolved(revision);
        if (!resolved) {
            return reject_published(resolved.error());
        }
        const auto installed = InstalledPack{
            revision,
            existing.value(1).toString(),
            existing.value(2).toString(),
            resolved->root().dependencies,
        };
        cleanup_publications();
        rollback_publications.dismiss();
        if (const auto finalized = finalize_install_state(); !finalized) {
            return std::unexpected(finalized.error());
        }
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
            return reject_published(queryFailure(CatalogErrorCode::QueryFailed, required,
                                                 QStringLiteral("check exact pack dependency"))
                                        .error());
        }
        if (!required.next()) {
            return reject_published(
                CatalogError{CatalogErrorCode::MissingDependency,
                             QStringLiteral("Required exact pack revision is not installed")});
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
        return reject_published(queryFailure(CatalogErrorCode::QueryFailed, insert,
                                             QStringLiteral("install pack revision"))
                                    .error());
    }
    const auto blobs_recorded =
        recordBlobs(database_, revision, staged->loaded.blobs, CatalogErrorCode::QueryFailed,
                    QStringLiteral("record pack blob descriptors"));
    if (!blobs_recorded) {
        return reject_published(blobs_recorded.error());
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
            return reject_published(queryFailure(CatalogErrorCode::QueryFailed, dependency_insert,
                                                 QStringLiteral("record pack dependency"))
                                        .error());
        }
    }
    const auto resolved =
        resolveClosure(revision, &staged->loaded, detail::CatalogOperation::InstallArchive);
    if (!resolved) {
        return reject_published(resolved.error());
    }
    std::optional<CatalogError> post_commit_error;
    bool commit_applied = false;
    bool commit_ambiguous = false;
    if (const auto committed = commit(&commit_applied, &commit_ambiguous); !committed) {
        if (commit_ambiguous) {
            rollback_publications.dismiss();
            if (transaction_started) {
                rollback();
                transaction_started = false;
            }
            const auto rebound = install_lock->revalidate();
            const auto released = install_lock->release();
            static_cast<void>(root_lock->release());
            if (!rebound) {
                return std::unexpected(rebound.error());
            }
            if (!released) {
                return std::unexpected(released.error());
            }
            return fail(CatalogErrorCode::CorruptCatalog,
                        QStringLiteral("Catalog COMMIT outcome is ambiguous"));
        }
        if (!commit_applied) {
            return reject_published(committed.error());
        }
        transaction_started = false;
        rollback_publications.dismiss();
        post_commit_error = committed.error();
    }
    transaction_started = false;
    rollback_publications.dismiss();
    const auto result =
        InstalledPack{revision, staged->sha256, installed_at_utc, resolved->root().dependencies};
    const auto reject_committed =
        [&](CatalogError error) -> std::expected<InstalledPack, CatalogError> {
        if (const auto rebound = install_lock->revalidate(); !rebound) {
            const auto released = install_lock->release();
            static_cast<void>(root_lock->release());
            return released ? std::unexpected(rebound.error()) : std::unexpected(released.error());
        }
        auto committed_state = classifyCatalogNamespace(impl_->anchor, false, *root_lock,
                                                        detail::CatalogOperation::InstallArchive,
                                                        impl_->hooks, *allowed_lock);
        if (!committed_state) {
            error = committed_state.error();
        } else {
            auto admitted_commit = admitCatalogPrivately(
                impl_->anchor, std::move(*committed_state),
                detail::CatalogSchemaGeneration::Current, impl_->scratch_context, *root_lock,
                detail::CatalogOperation::InstallArchive, impl_->hooks, *allowed_lock);
            if (!admitted_commit) {
                error = admitted_commit.error();
            }
        }
        if (const auto finalized = finalize_install_state(); !finalized) {
            return std::unexpected(finalized.error());
        }
        if (error.code != CatalogErrorCode::CorruptCatalog) {
            error.code = CatalogErrorCode::CannotStoreArchive;
        }
        return std::unexpected(std::move(error));
    };

    if (const auto checkpointed =
            checkpointCatalog(database_, CatalogErrorCode::CannotStoreArchive,
                              detail::CatalogOperation::InstallArchive, impl_->hooks);
        !checkpointed) {
        return reject_committed(checkpointed.error());
    }
    auto committed_state = classifyCatalogNamespace(impl_->anchor, false, *root_lock,
                                                    detail::CatalogOperation::InstallArchive,
                                                    impl_->hooks, *allowed_lock);
    if (!committed_state) {
        return reject_committed(committed_state.error());
    }
    auto admitted_commit = admitCatalogPrivately(
        impl_->anchor, std::move(*committed_state), detail::CatalogSchemaGeneration::Current,
        impl_->scratch_context, *root_lock, detail::CatalogOperation::InstallArchive, impl_->hooks,
        *allowed_lock);
    if (!admitted_commit) {
        return reject_committed(admitted_commit.error());
    }
    if (const auto finalized = finalize_install_state(); !finalized) {
        return std::unexpected(finalized.error());
    }
    if (post_commit_error) {
        return std::unexpected(std::move(*post_commit_error));
    }
    return result;
#endif
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
        CatalogErrorCode::NotFound, detail::CatalogOperation::Load);
}

std::expected<LoadedPack, CatalogError>
PackCatalog::loadExactRevision(const model::PackRevision& exact_revision,
                               CatalogErrorCode missing_code,
                               detail::CatalogOperation catalog_operation) const {
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
    std::expected<LoadedPack, Error> loaded = std::unexpected(
        Error{ErrorCode::CannotRead, QStringLiteral("Catalog archive was not loaded")});
#if defined(Q_OS_LINUX)
    if (impl_ != nullptr) {
        if (impl_->snapshot &&
            (impl_->live_workspace == nullptr || !impl_->live_workspace->validateForAmbientUse())) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Private snapshot workspace binding changed"));
        }
        auto arena =
            CatalogScratchArena::create(impl_->scratch_context, catalog_operation, impl_->hooks,
                                        detail::CatalogSubject::ArchiveWorkspace);
        if (!arena) {
            return std::unexpected(arena.error());
        }
        loaded = securelyLoadCatalogArchive(
            impl_->anchor, impl_->admitted_namespace.archives, archive_sha, **arena, std::nullopt,
            impl_->scratch_context, catalog_operation, impl_->hooks);
        const auto cleaned = (*arena)->cleanup();
        if (!cleaned) {
            return std::unexpected(cleaned.error());
        }
        if (impl_->snapshot && !impl_->live_workspace->validateForAmbientUse()) {
            return fail(CatalogErrorCode::CannotOpen,
                        QStringLiteral("Private snapshot workspace changed during load"));
        }
    } else
#endif
    {
        const auto path =
            QDir(archivesDirectory()).filePath(archive_sha + QStringLiteral(".awpack"));
        const auto actual_archive_sha = hashArchiveFile(path);
        loaded = PackArchive::importArchive(path, {}, PackValidationScope::ResolvedClosure);
        if (!actual_archive_sha || *actual_archive_sha != archive_sha) {
            return fail(CatalogErrorCode::CorruptCatalog,
                        QStringLiteral("Installed archive does not match its catalog revision"));
        }
    }
    if (!loaded) {
        const auto code = loaded.error().code == ErrorCode::UnsupportedCapability
                              ? CatalogErrorCode::UnsupportedCapability
                          : loaded.error().code == ErrorCode::CannotRead
                              ? CatalogErrorCode::CannotOpen
                              : CatalogErrorCode::CorruptCatalog;
        return fail(code, loaded.error().message);
    }
    if (loaded->revision != exact_revision) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Installed archive does not match its catalog revision"));
    }
    const auto recorded_blobs = blobsFor(database_, loaded->revision);
    if (!recorded_blobs) {
        return std::unexpected(recorded_blobs.error());
    }
    if (*recorded_blobs != loaded->blobs) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Installed blob descriptors do not match the archive"));
    }
    const auto recorded_dependencies = dependenciesFor(database_, exact_revision);
    if (!recorded_dependencies) {
        return std::unexpected(recorded_dependencies.error());
    }
    if (normalizedDependencies(*recorded_dependencies) !=
        normalizedDependencies(loaded->dependencies)) {
        return fail(
            CatalogErrorCode::CorruptCatalog,
            QStringLiteral("Installed dependency rows do not match the verified archive manifest"));
    }
    return *loaded;
}

std::expected<ResolvedPack, CatalogError>
PackCatalog::loadResolved(const model::PackRevision& exact_root) const {
    return resolveClosure(exact_root, nullptr, detail::CatalogOperation::LoadResolved);
}

std::expected<ResolvedPack, CatalogError>
PackCatalog::resolveClosure(const model::PackRevision& exact_root,
                            const LoadedPack* staged_root) const {
    return resolveClosure(exact_root, staged_root, detail::CatalogOperation::LoadResolved);
}

std::expected<ResolvedPack, CatalogError>
PackCatalog::resolveClosure(const model::PackRevision& exact_root, const LoadedPack* staged_root,
                            detail::CatalogOperation catalog_operation) const {
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
        auto loaded = is_root && staged_root != nullptr
                          ? std::expected<LoadedPack, CatalogError>{*staged_root}
                          : loadExactRevision(exact_revision,
                                              is_root ? CatalogErrorCode::NotFound
                                                      : CatalogErrorCode::MissingDependency,
                                              catalog_operation);
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
        const auto uses_workflow_preconditions =
            std::ranges::any_of(loaded->resources, [](const ValidatedResource& resource) {
                if (resource.descriptor.kind != model::ResourceKind::Workflow) {
                    return false;
                }
                if (std::ranges::any_of(
                        resource.document.value(QStringLiteral("operations")).toArray(),
                        [](const QJsonValue& operation) {
                            return !operation.toObject()
                                        .value(QStringLiteral("preconditions"))
                                        .toArray()
                                        .isEmpty();
                        })) {
                    return true;
                }
                return std::ranges::any_of(
                    resource.document.value(QStringLiteral("filing_routes")).toArray(),
                    [](const QJsonValue& route) {
                        return std::ranges::any_of(
                            route.toObject().value(QStringLiteral("filing_bindings")).toArray(),
                            [](const QJsonValue& binding) {
                                return !binding.toObject()
                                            .value(QStringLiteral("preconditions"))
                                            .toArray()
                                            .isEmpty();
                            });
                    });
            });
        const auto uses_dependent_deadlines =
            std::ranges::any_of(loaded->resources, [](const ValidatedResource& resource) {
                if (resource.descriptor.kind != model::ResourceKind::Workflow) {
                    return false;
                }
                const auto is_reached = [](const QJsonValue& precondition_value) {
                    const auto precondition = precondition_value.toObject();
                    return precondition.value(QStringLiteral("kind")).toString() ==
                               QStringLiteral("deadline_status") &&
                           precondition.value(QStringLiteral("status")).toString() ==
                               QStringLiteral("reached");
                };
                const auto operation_feature = std::ranges::any_of(
                    resource.document.value(QStringLiteral("operations")).toArray(),
                    [&](const QJsonValue& operation_value) {
                        const auto operation = operation_value.toObject();
                        return operation.contains(QStringLiteral("deadline_base_id")) ||
                               std::ranges::any_of(
                                   operation.value(QStringLiteral("preconditions")).toArray(),
                                   is_reached);
                    });
                return operation_feature ||
                       std::ranges::any_of(
                           resource.document.value(QStringLiteral("filing_routes")).toArray(),
                           [&](const QJsonValue& route) {
                               return std::ranges::any_of(
                                   route.toObject()
                                       .value(QStringLiteral("filing_bindings"))
                                       .toArray(),
                                   [&](const QJsonValue& binding) {
                                       return std::ranges::any_of(
                                           binding.toObject()
                                               .value(QStringLiteral("preconditions"))
                                               .toArray(),
                                           is_reached);
                                   });
                           });
            });
        const auto uses_named_deadlines =
            std::ranges::any_of(loaded->resources, [](const ValidatedResource& resource) {
                if (resource.descriptor.kind != model::ResourceKind::Workflow) {
                    return false;
                }
                return std::ranges::any_of(
                    resource.document.value(QStringLiteral("operations")).toArray(),
                    [](const QJsonValue& operation_value) {
                        return operation_value.toObject().contains(
                            QStringLiteral("produced_deadline_id"));
                    });
            });
        const auto uses_event_date_deadlines =
            std::ranges::any_of(loaded->resources, [](const ValidatedResource& resource) {
                if (resource.descriptor.kind != model::ResourceKind::Workflow) {
                    return false;
                }
                return std::ranges::any_of(
                    resource.document.value(QStringLiteral("operations")).toArray(),
                    [](const QJsonValue& operation_value) {
                        return operation_value.toObject().contains(
                            QStringLiteral("deadline_event_base"));
                    });
            });
        const auto uses_argument_date_guards =
            std::ranges::any_of(loaded->resources, [](const ValidatedResource& resource) {
                if (resource.descriptor.kind != model::ResourceKind::Workflow) {
                    return false;
                }
                const auto is_argument_date = [](const QJsonValue& precondition_value) {
                    return precondition_value.toObject().value(QStringLiteral("kind")).toString() ==
                           QStringLiteral("argument_date_status");
                };
                const auto operation_feature = std::ranges::any_of(
                    resource.document.value(QStringLiteral("operations")).toArray(),
                    [&](const QJsonValue& operation_value) {
                        return std::ranges::any_of(operation_value.toObject()
                                                       .value(QStringLiteral("preconditions"))
                                                       .toArray(),
                                                   is_argument_date);
                    });
                return operation_feature ||
                       std::ranges::any_of(
                           resource.document.value(QStringLiteral("filing_routes")).toArray(),
                           [&](const QJsonValue& route) {
                               return std::ranges::any_of(
                                   route.toObject()
                                       .value(QStringLiteral("filing_bindings"))
                                       .toArray(),
                                   [&](const QJsonValue& binding) {
                                       return std::ranges::any_of(
                                           binding.toObject()
                                               .value(QStringLiteral("preconditions"))
                                               .toArray(),
                                           is_argument_date);
                                   });
                           });
            });
        const auto uses_structured_disposition =
            std::ranges::any_of(loaded->resources, [](const ValidatedResource& resource) {
                if (resource.descriptor.kind != model::ResourceKind::Case) {
                    return false;
                }
                if (resource.document.contains(QStringLiteral("disposition_plans")) ||
                    resource.document.contains(QStringLiteral("authored_disposition_plan_id"))) {
                    return true;
                }
                return std::ranges::any_of(
                    resource.document.value(QStringLiteral("issues")).toArray(),
                    [](const QJsonValue& issue) {
                        return issue.toObject().contains(QStringLiteral("target_ids"));
                    });
            });
        const auto uses_grounded_questions =
            std::ranges::any_of(loaded->resources, [](const ValidatedResource& resource) {
                return resource.descriptor.kind == model::ResourceKind::ArgumentConfig &&
                       resource.document.contains(QStringLiteral("grounded_question_bank"));
            });
        const auto uses_realism_evidence =
            std::ranges::any_of(loaded->resources, [](const ValidatedResource& resource) {
                return resource.descriptor.kind == model::ResourceKind::RealismReview &&
                       (resource.document.contains(QStringLiteral("evidence")) ||
                        resource.document.contains(QStringLiteral("reviewer")) ||
                        std::ranges::any_of(
                            resource.document.value(QStringLiteral("known_uncertainty")).toArray(),
                            [](const QJsonValue& uncertainty) { return uncertainty.isObject(); }));
            });
        const auto uses_sealed_record_twins =
            std::ranges::any_of(loaded->resources, [](const ValidatedResource& resource) {
                return resource.descriptor.kind == model::ResourceKind::Record &&
                       (resource.document.contains(QStringLiteral("disclosure_policy")) ||
                        resource.document.contains(QStringLiteral("sealed_disclosures")));
            });
        const auto uses_route_role_subsets =
            std::ranges::any_of(loaded->resources, [](const ValidatedResource& resource) {
                return resource.descriptor.kind == model::ResourceKind::Workflow &&
                       std::ranges::any_of(
                           resource.document.value(QStringLiteral("filing_routes")).toArray(),
                           [](const QJsonValue& route) {
                               return route.toObject().contains(
                                   QStringLiteral("authorized_role_scope"));
                           });
            });
        const auto uses_workflow_instance_preconditions =
            std::ranges::any_of(loaded->resources, [](const ValidatedResource& resource) {
                if (resource.descriptor.kind != model::ResourceKind::Workflow)
                    return false;
                const auto has_instance = [](const QJsonArray& preconditions) {
                    return std::ranges::any_of(preconditions, [](const QJsonValue& value) {
                        const auto kind = value.toObject().value(QStringLiteral("kind")).toString();
                        return kind == QStringLiteral("filing_instance") ||
                               kind == QStringLiteral("order_instance");
                    });
                };
                return std::ranges::any_of(
                           resource.document.value(QStringLiteral("operations")).toArray(),
                           [&](const QJsonValue& operation) {
                               return has_instance(operation.toObject()
                                                       .value(QStringLiteral("preconditions"))
                                                       .toArray());
                           }) ||
                       std::ranges::any_of(
                           resource.document.value(QStringLiteral("filing_routes")).toArray(),
                           [&](const QJsonValue& route) {
                               return std::ranges::any_of(
                                   route.toObject()
                                       .value(QStringLiteral("filing_bindings"))
                                       .toArray(),
                                   [&](const QJsonValue& binding) {
                                       return has_instance(
                                           binding.toObject()
                                               .value(QStringLiteral("preconditions"))
                                               .toArray());
                                   });
                           });
            });
        const auto uses_static_deficiency_deadlines =
            std::ranges::any_of(loaded->resources, [](const ValidatedResource& resource) {
                return resource.descriptor.kind == model::ResourceKind::Workflow &&
                       std::ranges::any_of(
                           resource.document.value(QStringLiteral("filing_routes")).toArray(),
                           [](const QJsonValue& route) {
                               return route.toObject()
                                          .value(QStringLiteral("deficiency_deadline"))
                                          .toObject()
                                          .value(QStringLiteral("id_mode"))
                                          .toString() == QStringLiteral("exact");
                           });
            });
        const auto uses_operation_document_bindings =
            std::ranges::any_of(loaded->resources, [](const ValidatedResource& resource) {
                return resource.descriptor.kind == model::ResourceKind::Workflow &&
                       std::ranges::any_of(
                           resource.document.value(QStringLiteral("operations")).toArray(),
                           [](const QJsonValue& operation_value) {
                               const auto operation = operation_value.toObject();
                               return operation.contains(QStringLiteral("document_binding")) ||
                                      operation.contains(QStringLiteral("expected_argument_date"));
                           });
            });
        const auto uses_operation_disposition_bindings =
            std::ranges::any_of(loaded->resources, [](const ValidatedResource& resource) {
                return resource.descriptor.kind == model::ResourceKind::Workflow &&
                       std::ranges::any_of(
                           resource.document.value(QStringLiteral("operations")).toArray(),
                           [](const QJsonValue& operation_value) {
                               return operation_value.toObject().contains(
                                   QStringLiteral("disposition_plan_id"));
                           });
            });
        const auto uses_route_filing_bindings =
            std::ranges::any_of(loaded->resources, [](const ValidatedResource& resource) {
                return resource.descriptor.kind == model::ResourceKind::Workflow &&
                       std::ranges::any_of(
                           resource.document.value(QStringLiteral("filing_routes")).toArray(),
                           [](const QJsonValue& route) {
                               return route.toObject().contains(QStringLiteral("filing_bindings"));
                           });
            });
        const auto uses_alternative_event_date_deadlines =
            std::ranges::any_of(loaded->resources, [](const ValidatedResource& resource) {
                return resource.descriptor.kind == model::ResourceKind::Workflow &&
                       std::ranges::any_of(
                           resource.document.value(QStringLiteral("operations")).toArray(),
                           [](const QJsonValue& operation) {
                               return operation.toObject()
                                          .value(QStringLiteral("deadline_event_base"))
                                          .toObject()
                                          .value(QStringLiteral("kind"))
                                          .toString() == QStringLiteral("order_occurred_one_of");
                           });
            });
        const auto uses_operation_legal_time_guards =
            std::ranges::any_of(loaded->resources, [](const ValidatedResource& resource) {
                return resource.descriptor.kind == model::ResourceKind::Workflow &&
                       std::ranges::any_of(
                           resource.document.value(QStringLiteral("operations")).toArray(),
                           [](const QJsonValue& operation) {
                               return operation.toObject().contains(
                                   QStringLiteral("allowed_legal_times"));
                           });
            });
        const auto capabilities = CapabilityRegistry::validateCoverage(
            loaded->manifest_schema_version, loaded->required_capabilities, resource_kinds,
            uses_workflow_preconditions, uses_dependent_deadlines, uses_named_deadlines,
            uses_event_date_deadlines, uses_argument_date_guards, uses_structured_disposition,
            uses_grounded_questions, uses_realism_evidence, uses_sealed_record_twins,
            uses_route_role_subsets, uses_workflow_instance_preconditions,
            uses_static_deficiency_deadlines, uses_operation_document_bindings,
            uses_operation_disposition_bindings, uses_route_filing_bindings,
            uses_alternative_event_date_deadlines, uses_operation_legal_time_guards);
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

    const auto validate_grounded_argument_ownership =
        [&resource_owners](const LoadedPack& pack) -> std::expected<void, CatalogError> {
        for (const auto& resource : pack.resources) {
            if (resource.descriptor.kind != model::ResourceKind::ArgumentConfig ||
                !resource.document.contains(QStringLiteral("grounded_question_bank"))) {
                continue;
            }
            const auto case_id =
                resource.document.value(QStringLiteral("case_id")).toString().toStdString();
            const auto owner = resource_owners.find(case_id);
            if (owner == resource_owners.end() || owner->second != pack.revision) {
                return fail(
                    CatalogErrorCode::InvalidResolvedGraph,
                    QStringLiteral("Grounded argument configuration %1 must target a case owned "
                                   "by exact pack %2")
                        .arg(asQString(resource.descriptor.id), asQString(pack.revision.id.value)));
            }
        }
        return {};
    };
    for (const auto& dependency : dependencies_dependency_first) {
        const auto validated = validate_grounded_argument_ownership(dependency);
        if (!validated) {
            return std::unexpected(validated.error());
        }
    }
    if (const auto validated = validate_grounded_argument_ownership(*root); !validated) {
        return std::unexpected(validated.error());
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
#if !defined(Q_OS_LINUX)
    return fail(CatalogErrorCode::CannotStoreBlob,
                QStringLiteral("Secure blob materialization requires Linux"));
#else
    if (impl_ == nullptr || impl_->snapshot || !database_.isOpen()) {
        return fail(CatalogErrorCode::CannotStoreBlob,
                    QStringLiteral("Writable catalog state is unavailable"));
    }
    auto root_lock = lockCatalogRoot(impl_->anchor.rootDescriptor(), impl_->anchor.absolute_root,
                                     detail::CatalogLockMode::Exclusive,
                                     detail::CatalogOperation::MaterializeBlob, impl_->hooks);
    if (!root_lock) {
        return std::unexpected(root_lock.error());
    }
    auto admitted = classifyCatalogNamespace(
        impl_->anchor, false, *root_lock, detail::CatalogOperation::MaterializeBlob, impl_->hooks);
    if (!admitted) {
        return std::unexpected(admitted.error());
    }
    if (admitted->shape != detail::CatalogNamespaceShape::Current) {
        return fail(CatalogErrorCode::CorruptCatalog,
                    QStringLiteral("Writable catalog is not in the current schema shape"));
    }
    if (const auto rebound = revalidateCatalogNamespaceBindings(
            impl_->anchor, impl_->admitted_namespace, detail::CatalogOperation::MaterializeBlob,
            impl_->hooks, false, false);
        !rebound) {
        return std::unexpected(rebound.error());
    }
    if (const auto normalized = normalizeRetainedCatalogDirectory(
            impl_->anchor, nullptr, detail::CatalogSubject::CatalogRoot,
            detail::CatalogOperation::MaterializeBlob, impl_->hooks);
        !normalized) {
        return std::unexpected(normalized.error());
    }
    if (!root_lock->refreshAfterAuthorizedTransition(impl_->anchor)) {
        return fail(CatalogErrorCode::CannotStoreBlob,
                    QStringLiteral("Catalog lock did not survive directory normalization"));
    }
    if (const auto normalized = normalizeRetainedCatalogDirectory(
            impl_->anchor, &*impl_->anchor.archives, detail::CatalogSubject::ArchivesDirectory,
            detail::CatalogOperation::MaterializeBlob, impl_->hooks);
        !normalized) {
        return std::unexpected(normalized.error());
    }
    if (const auto normalized = normalizeRetainedCatalogDirectory(
            impl_->anchor, &*impl_->anchor.blobs, detail::CatalogSubject::BlobsDirectory,
            detail::CatalogOperation::MaterializeBlob, impl_->hooks);
        !normalized) {
        return std::unexpected(normalized.error());
    }
    auto normalized_state = classifyCatalogNamespace(
        impl_->anchor, false, *root_lock, detail::CatalogOperation::MaterializeBlob, impl_->hooks);
    if (!normalized_state) {
        return std::unexpected(normalized_state.error());
    }
    impl_->admitted_namespace = std::move(*normalized_state);
    const auto finish_materialization =
        [&](MaterializedBlob result) -> std::expected<MaterializedBlob, CatalogError> {
        auto final_namespace =
            classifyCatalogNamespace(impl_->anchor, false, *root_lock,
                                     detail::CatalogOperation::MaterializeBlob, impl_->hooks);
        if (!final_namespace) {
            return std::unexpected(final_namespace.error());
        }
        impl_->admitted_namespace = std::move(*final_namespace);
        if (const auto released = root_lock->release(); !released) {
            return std::unexpected(released.error());
        }
        return result;
    };

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
        return finish_materialization(MaterializedBlob{descriptor, object_path});
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
    return finish_materialization(MaterializedBlob{descriptor, materialized_path->path});
#endif
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
    const auto installing =
        impl_ != nullptr && impl_->active_operation == detail::CatalogOperation::InstallArchive;
    const auto begun = execStatement(database_, QStringLiteral("BEGIN IMMEDIATE"),
                                     installing ? CatalogErrorCode::CannotStoreArchive
                                                : CatalogErrorCode::QueryFailed,
                                     QStringLiteral("begin transaction"));
    if (!begun || !installing) {
        return begun;
    }
    detail::CatalogObservation observation;
    observation.event = detail::CatalogEvent::TransactionBegun;
    observation.subject = detail::CatalogSubject::SQLiteConnection;
    observation.operation = detail::CatalogOperation::InstallArchive;
    const auto action = catalogAction(impl_->hooks, observation);
    if (catalogFailsBefore(action) || !catalogFinishes(impl_->hooks, observation, action)) {
        return fail(CatalogErrorCode::CannotStoreArchive,
                    QStringLiteral("Injected catalog transaction-begin failure"));
    }
    return {};
}

std::expected<void, CatalogError> PackCatalog::commit(bool* commit_applied,
                                                      bool* commit_ambiguous) {
    if (commit_applied == nullptr || commit_ambiguous == nullptr) {
        return fail(CatalogErrorCode::QueryFailed,
                    QStringLiteral("Catalog COMMIT state is unavailable"));
    }
    *commit_applied = false;
    *commit_ambiguous = false;
    const auto installing =
        impl_ != nullptr && impl_->active_operation == detail::CatalogOperation::InstallArchive;
    if (!installing) {
        const auto committed =
            execStatement(database_, QStringLiteral("COMMIT"), CatalogErrorCode::QueryFailed,
                          QStringLiteral("commit transaction"));
        *commit_applied = committed.has_value();
        *commit_ambiguous = !committed.has_value();
        return committed;
    }
    detail::CatalogObservation attempted;
    attempted.event = detail::CatalogEvent::TransactionCommitAttempted;
    attempted.subject = detail::CatalogSubject::SQLiteConnection;
    attempted.operation = detail::CatalogOperation::InstallArchive;
    const auto attempted_action = catalogAction(impl_->hooks, attempted);
    if (catalogFailsBefore(attempted_action)) {
        return fail(CatalogErrorCode::CannotStoreArchive,
                    QStringLiteral("Injected failure before catalog COMMIT"));
    }
    const auto committed =
        execStatement(database_, QStringLiteral("COMMIT"), CatalogErrorCode::CannotStoreArchive,
                      QStringLiteral("commit catalog installation"));
    if (!committed) {
        *commit_ambiguous = true;
        return committed;
    }
    *commit_applied = true;
    if (!catalogFinishes(impl_->hooks, attempted, attempted_action)) {
        return fail(CatalogErrorCode::CannotStoreArchive,
                    QStringLiteral("Injected failure after catalog COMMIT"));
    }
    detail::CatalogObservation finished = attempted;
    finished.event = detail::CatalogEvent::TransactionCommitted;
    const auto finished_action = catalogAction(impl_->hooks, finished);
    if (catalogFailsBefore(finished_action) ||
        !catalogFinishes(impl_->hooks, finished, finished_action)) {
        return fail(CatalogErrorCode::CannotStoreArchive,
                    QStringLiteral("Injected catalog post-COMMIT failure"));
    }
    return {};
}

void PackCatalog::rollback() {
    QSqlQuery query(database_);
    static_cast<void>(query.exec(QStringLiteral("ROLLBACK")));
    if (impl_ != nullptr && impl_->active_operation == detail::CatalogOperation::InstallArchive) {
        detail::CatalogObservation observation;
        observation.event = detail::CatalogEvent::TransactionRolledBack;
        observation.subject = detail::CatalogSubject::SQLiteConnection;
        observation.operation = detail::CatalogOperation::InstallArchive;
        const auto action = catalogAction(impl_->hooks, observation);
        static_cast<void>(catalogFinishes(impl_->hooks, observation, action));
    }
}

} // namespace appellate::packs
