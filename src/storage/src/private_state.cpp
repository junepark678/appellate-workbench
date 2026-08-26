#include "appellate/storage/detail/private_state.hpp"

#include <QDir>
#include <QFile>
#include <QUuid>
#include <QtSystemDetection>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

#if defined(Q_OS_LINUX)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>
#endif

namespace appellate::storage::detail {
namespace {

[[nodiscard]] QString nativeError(QStringView action) {
    return QStringLiteral("%1: %2").arg(action, QString::fromLocal8Bit(std::strerror(errno)));
}

#if defined(Q_OS_LINUX)
class Descriptor final {
  public:
    explicit Descriptor(int value = -1) noexcept : value_(value) {}
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Descriptor& operator=(Descriptor&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.value_, -1));
        }
        return *this;
    }
    ~Descriptor() { reset(); }

    [[nodiscard]] int get() const noexcept { return value_; }
    [[nodiscard]] int release() noexcept { return std::exchange(value_, -1); }
    void reset(int replacement = -1) noexcept {
        if (value_ >= 0) {
            static_cast<void>(::close(value_));
        }
        value_ = replacement;
    }

  private:
    int value_{-1};
};

struct CreatedDirectory final {
    Descriptor parent;
    QByteArray name;
    dev_t device{};
    ino_t inode{};
    uid_t owner{};
};

class CreatedDirectoryRollback final {
  public:
    CreatedDirectoryRollback() = default;
    CreatedDirectoryRollback(const CreatedDirectoryRollback&) = delete;
    CreatedDirectoryRollback& operator=(const CreatedDirectoryRollback&) = delete;
    ~CreatedDirectoryRollback() {
        if (committed_ || created_.empty()) {
            return;
        }
        // Detach only the topmost freshly created directory. It contains every deeper fresh
        // component, and keeping the whole tree as a reserved tombstone avoids an unsafe
        // check/rmdir sequence against a same-UID namespace replacement.
        auto& entry = created_.front();
        struct stat named{};
        if (::fstatat(entry.parent.get(), entry.name.constData(), &named, AT_SYMLINK_NOFOLLOW) !=
                0 ||
            !S_ISDIR(named.st_mode) || named.st_dev != entry.device ||
            named.st_ino != entry.inode || named.st_uid != entry.owner) {
            return;
        }
        const auto quarantine_name = QByteArrayLiteral(".appellate-quarantine-") +
                                     QUuid::createUuid().toByteArray(QUuid::Id128) +
                                     QByteArrayLiteral(".tmp");
#if defined(SYS_renameat2)
        int result{};
        do {
            result = static_cast<int>(::syscall(SYS_renameat2, entry.parent.get(),
                                                entry.name.constData(), entry.parent.get(),
                                                quarantine_name.constData(), 1U));
        } while (result != 0 && errno == EINTR);
        if (result == 0) {
            static_cast<void>(::fsync(entry.parent.get()));
        }
#endif
    }

    void add(Descriptor parent, const QByteArray& name, const struct stat& status) {
        created_.push_back(
            CreatedDirectory{std::move(parent), name, status.st_dev, status.st_ino, status.st_uid});
    }

    void commit() noexcept {
        committed_ = true;
        created_.clear();
    }

  private:
    std::vector<CreatedDirectory> created_;
    bool committed_{};
};

enum class AclState {
    Absent,
    Present,
    Unsupported,
    Failure,
};

[[nodiscard]] AclState aclState(int descriptor, const char* attribute) {
    while (true) {
        errno = 0;
        const auto result = ::fgetxattr(descriptor, attribute, nullptr, 0);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result >= 0) {
            return AclState::Present;
        }
        if (errno == ENODATA) {
            return AclState::Absent;
        }
#if defined(ENOATTR) && ENOATTR != ENODATA
        if (errno == ENOATTR) {
            return AclState::Absent;
        }
#endif
        if (errno == ENOTSUP || errno == EOPNOTSUPP || errno == ENOSYS) {
            return AclState::Unsupported;
        }
        return AclState::Failure;
    }
}

[[nodiscard]] auto requireAclAbsent(int descriptor, bool directory)
    -> std::expected<void, QString> {
    const auto access = aclState(descriptor, "system.posix_acl_access");
    const auto default_acl =
        directory ? aclState(descriptor, "system.posix_acl_default") : AclState::Absent;
    if (access == AclState::Absent && default_acl == AclState::Absent) {
        return {};
    }
    if (access == AclState::Present || default_acl == AclState::Present) {
        return std::unexpected(QStringLiteral("Private state path has a POSIX ACL"));
    }
    if (access == AclState::Unsupported || default_acl == AclState::Unsupported) {
        return std::unexpected(QStringLiteral("Private state POSIX ACL inspection is unsupported"));
    }
    return std::unexpected(nativeError(u"Inspect private state POSIX ACL"));
}

[[nodiscard]] bool sameDirectory(const struct stat& left, const struct stat& right) {
    return S_ISDIR(left.st_mode) && S_ISDIR(right.st_mode) && left.st_dev == right.st_dev &&
           left.st_ino == right.st_ino && left.st_uid == right.st_uid &&
           (left.st_mode & 07777) == (right.st_mode & 07777);
}

[[nodiscard]] bool sameRegularFile(const struct stat& left, const struct stat& right,
                                   int expected_link_count) {
    return S_ISREG(left.st_mode) && S_ISREG(right.st_mode) && left.st_dev == right.st_dev &&
           left.st_ino == right.st_ino && left.st_uid == right.st_uid &&
           (left.st_mode & 07777) == (right.st_mode & 07777) &&
           left.st_nlink == static_cast<nlink_t>(expected_link_count) &&
           right.st_nlink == static_cast<nlink_t>(expected_link_count);
}

[[nodiscard]] bool controllerPolicy(const struct stat& status) {
    return S_ISDIR(status.st_mode) && (status.st_uid == ::geteuid() || status.st_uid == 0) &&
           (((status.st_mode & 0022) == 0) || ((status.st_mode & S_ISVTX) != 0));
}

[[nodiscard]] bool privateDirectoryPolicy(const struct stat& status) {
    return S_ISDIR(status.st_mode) && status.st_uid == ::geteuid() &&
           (status.st_mode & 07777) == 0700;
}

[[nodiscard]] bool privateFilePolicy(const struct stat& status, int expected_link_count) {
    return S_ISREG(status.st_mode) && status.st_uid == ::geteuid() &&
           (status.st_mode & 07777) == 0600 &&
           status.st_nlink == static_cast<nlink_t>(expected_link_count);
}

[[nodiscard]] int openDirectoryAt(int parent, const QByteArray& name, bool path_only = false) {
    const auto flags = (path_only ? O_PATH : O_RDONLY) | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    int result{};
    do {
        result = ::openat(parent, name.constData(), flags);
    } while (result < 0 && errno == EINTR);
    return result;
}

[[nodiscard]] int duplicateDescriptor(int descriptor) {
    int result{};
    do {
        result = ::fcntl(descriptor, F_DUPFD_CLOEXEC, 0);
    } while (result < 0 && errno == EINTR);
    return result;
}

[[nodiscard]] bool syncDescriptor(int descriptor) {
    int result{};
    do {
        result = ::fsync(descriptor);
    } while (result != 0 && errno == EINTR);
    return result == 0;
}

[[nodiscard]] auto normalizePrivateDirectoryMode(int descriptor) -> std::expected<void, QString> {
    const auto proc_descriptor =
        QByteArrayLiteral("/proc/self/fd/") + QByteArray::number(descriptor);
    int result{};
    do {
        result = ::chmod(proc_descriptor.constData(), 0700);
    } while (result != 0 && errno == EINTR);
    if (result == 0) {
        return {};
    }
    const auto proc_error = errno;
#if defined(SYS_fchmodat2) && defined(AT_EMPTY_PATH)
    do {
        result = static_cast<int>(::syscall(SYS_fchmodat2, descriptor, "", 0700, AT_EMPTY_PATH));
    } while (result != 0 && errno == EINTR);
    if (result == 0) {
        return {};
    }
#endif
    errno = proc_error;
    return std::unexpected(nativeError(u"Normalize private state directory mode"));
}

[[nodiscard]] auto validateDirectory(int descriptor, bool private_directory)
    -> std::expected<struct stat, QString> {
    struct stat before{};
    struct stat after{};
    if (::fstat(descriptor, &before) != 0 ||
        (private_directory ? !privateDirectoryPolicy(before) : !controllerPolicy(before))) {
        return std::unexpected(QStringLiteral("Private state directory owner or mode is unsafe"));
    }
    if (const auto acl = requireAclAbsent(descriptor, true); !acl) {
        return std::unexpected(acl.error());
    }
    if (::fstat(descriptor, &after) != 0 || !sameDirectory(before, after) ||
        (private_directory ? !privateDirectoryPolicy(after) : !controllerPolicy(after))) {
        return std::unexpected(QStringLiteral("Private state directory changed during validation"));
    }
    return after;
}

[[nodiscard]] auto absoluteComponents(QStringView path) -> std::expected<QStringList, QString> {
    const auto owned = path.toString();
    if (owned.isEmpty() || owned.contains(QChar::Null) || !QDir::isAbsolutePath(owned) ||
        QDir::cleanPath(owned) != owned) {
        return std::unexpected(QStringLiteral("Private state path must be absolute and clean"));
    }
    const auto components = owned.split(u'/', Qt::SkipEmptyParts);
    for (const auto& component : components) {
        const auto native = QFile::encodeName(component);
        if (component == QStringLiteral(".") || component == QStringLiteral("..") ||
            native.isEmpty() || native.contains('\0') || QFile::decodeName(native) != component) {
            return std::unexpected(QStringLiteral("Private state path has an unsafe component"));
        }
    }
    return components;
}

[[nodiscard]] auto walkDirectory(QStringView absolute_path, bool create_missing,
                                 std::optional<QStringView> private_boundary)
    -> std::expected<int, QString> {
    const auto components = absoluteComponents(absolute_path);
    if (!components) {
        return std::unexpected(components.error());
    }
    qsizetype private_index = components->size();
    if (private_boundary.has_value()) {
        const auto boundary_components = absoluteComponents(*private_boundary);
        if (!boundary_components || boundary_components->isEmpty() ||
            boundary_components->size() > components->size() ||
            !std::equal(boundary_components->cbegin(), boundary_components->cend(),
                        components->cbegin())) {
            return std::unexpected(
                QStringLiteral("Private state boundary must contain the target path"));
        }
        private_index = boundary_components->size() - 1;
    }

    Descriptor current(::open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (current.get() < 0) {
        return std::unexpected(nativeError(u"Anchor private state filesystem root"));
    }
    auto parent_status = validateDirectory(current.get(), false);
    if (!parent_status) {
        return std::unexpected(parent_status.error());
    }
    CreatedDirectoryRollback rollback;

    for (qsizetype index = 0; index < components->size(); ++index) {
        const auto name = QFile::encodeName(components->at(index));
        Descriptor child(openDirectoryAt(current.get(), name));
        auto created = false;
        if (child.get() < 0 && errno == ENOENT && create_missing) {
            Descriptor rollback_parent(duplicateDescriptor(current.get()));
            if (rollback_parent.get() < 0) {
                return std::unexpected(nativeError(u"Retain private state directory parent"));
            }
            const auto staging_name = QByteArrayLiteral(".appellate-directory-stage-") +
                                      QUuid::createUuid().toByteArray(QUuid::Id128) +
                                      QByteArrayLiteral(".tmp");
            int result{};
            do {
                result = ::mkdirat(current.get(), staging_name.constData(), 0000);
            } while (result != 0 && errno == EINTR);
            if (result != 0) {
                return std::unexpected(nativeError(u"Create staged private state directory"));
            } else {
                struct stat created_status{};
                if (::fstatat(current.get(), staging_name.constData(), &created_status,
                              AT_SYMLINK_NOFOLLOW) != 0 ||
                    !S_ISDIR(created_status.st_mode) || created_status.st_uid != ::geteuid()) {
                    return std::unexpected(
                        QStringLiteral("Cannot identify staged private state directory"));
                }
                Descriptor retained(openDirectoryAt(current.get(), staging_name, true));
                struct stat held{};
                struct stat named{};
                if (retained.get() < 0 || ::fstat(retained.get(), &held) != 0 ||
                    ::fstatat(current.get(), staging_name.constData(), &named,
                              AT_SYMLINK_NOFOLLOW) != 0 ||
                    !sameDirectory(held, named) || held.st_uid != ::geteuid()) {
                    return std::unexpected(
                        QStringLiteral("Cannot bind staged private state directory"));
                }
                if (const auto normalized = normalizePrivateDirectoryMode(retained.get());
                    !normalized) {
                    return std::unexpected(normalized.error());
                }
                child.reset(openDirectoryAt(current.get(), staging_name));
                struct stat normalized{};
                struct stat reopened{};
                if (child.get() < 0 || ::fstat(retained.get(), &normalized) != 0 ||
                    ::fstat(child.get(), &reopened) != 0 || !sameDirectory(normalized, reopened) ||
                    !validateDirectory(child.get(), true) || !syncDescriptor(child.get()) ||
                    !syncDescriptor(current.get())) {
                    return std::unexpected(
                        QStringLiteral("Staged private state directory is not durable and safe"));
                }
#if defined(SYS_renameat2)
                do {
                    result = static_cast<int>(::syscall(SYS_renameat2, current.get(),
                                                        staging_name.constData(), current.get(),
                                                        name.constData(), 1U));
                } while (result != 0 && errno == EINTR);
#else
                errno = ENOTSUP;
                result = -1;
#endif
                if (result == 0) {
                    created = true;
                    if (::fstatat(current.get(), name.constData(), &named, AT_SYMLINK_NOFOLLOW) !=
                            0 ||
                        !sameDirectory(normalized, named)) {
                        return std::unexpected(
                            QStringLiteral("Published private state directory binding changed"));
                    }
                    rollback.add(std::move(rollback_parent), name, named);
                    if (!syncDescriptor(current.get())) {
                        return std::unexpected(
                            nativeError(u"Flush published private state directory"));
                    }
                } else if (errno == EEXIST) {
                    // A cooperative creator won the no-replace publication race. The staged
                    // owner-only directory is inert crash residue; validate the winner below.
                    child.reset(openDirectoryAt(current.get(), name));
                } else {
                    return std::unexpected(nativeError(u"Publish staged private state directory"));
                }
            }
        }
        if (child.get() < 0) {
            return std::unexpected(nativeError(u"Open no-follow private state directory"));
        }

        struct stat named{};
        auto status = validateDirectory(child.get(), created || index >= private_index);
        if (!status ||
            ::fstatat(current.get(), name.constData(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
            !sameDirectory(*status, named) ||
            (((parent_status->st_mode & S_ISVTX) != 0) && status->st_uid != ::geteuid() &&
             status->st_uid != 0)) {
            return std::unexpected(status
                                       ? QStringLiteral("Private state directory binding is unsafe")
                                       : status.error());
        }
        current = std::move(child);
        parent_status = *status;
    }
    rollback.commit();
    return current.release();
}
#endif

} // namespace

auto openPrivateStateController(QStringView absolute_path) -> std::expected<int, QString> {
#if defined(Q_OS_LINUX)
    return walkDirectory(absolute_path, false, std::nullopt);
#else
    Q_UNUSED(absolute_path);
    return std::unexpected(QStringLiteral("Private state filesystem validation requires Linux"));
#endif
}

auto openPrivateStateDirectory(QStringView absolute_path) -> std::expected<int, QString> {
#if defined(Q_OS_LINUX)
    return walkDirectory(absolute_path, false, absolute_path);
#else
    Q_UNUSED(absolute_path);
    return std::unexpected(QStringLiteral("Private state filesystem validation requires Linux"));
#endif
}

auto ensurePrivateStateDirectory(QStringView absolute_path, QStringView private_boundary)
    -> std::expected<int, QString> {
#if defined(Q_OS_LINUX)
    return walkDirectory(absolute_path, true, private_boundary);
#else
    Q_UNUSED(absolute_path);
    Q_UNUSED(private_boundary);
    return std::unexpected(QStringLiteral("Private state filesystem validation requires Linux"));
#endif
}

auto validatePrivateStateDirectoryDescriptor(int descriptor) -> std::expected<void, QString> {
#if defined(Q_OS_LINUX)
    const auto validated = validateDirectory(descriptor, true);
    return validated ? std::expected<void, QString>{} : std::unexpected(validated.error());
#else
    Q_UNUSED(descriptor);
    return std::unexpected(QStringLiteral("Private state filesystem validation requires Linux"));
#endif
}

auto validatePrivateStateControllerDescriptor(int descriptor) -> std::expected<void, QString> {
#if defined(Q_OS_LINUX)
    const auto validated = validateDirectory(descriptor, false);
    return validated ? std::expected<void, QString>{} : std::unexpected(validated.error());
#else
    Q_UNUSED(descriptor);
    return std::unexpected(QStringLiteral("Private state filesystem validation requires Linux"));
#endif
}

auto validatePrivateStateFileDescriptor(int descriptor, int expected_link_count)
    -> std::expected<void, QString> {
#if defined(Q_OS_LINUX)
    struct stat before{};
    struct stat after{};
    if (::fstat(descriptor, &before) != 0 || !privateFilePolicy(before, expected_link_count)) {
        return std::unexpected(QStringLiteral("Private state file owner or mode is unsafe"));
    }
    if (const auto acl = requireAclAbsent(descriptor, false); !acl) {
        return std::unexpected(acl.error());
    }
    if (::fstat(descriptor, &after) != 0 || !sameRegularFile(before, after, expected_link_count) ||
        !privateFilePolicy(after, expected_link_count)) {
        return std::unexpected(QStringLiteral("Private state file changed during validation"));
    }
    return {};
#else
    Q_UNUSED(descriptor);
    Q_UNUSED(expected_link_count);
    return std::unexpected(QStringLiteral("Private state filesystem validation requires Linux"));
#endif
}

auto normalizeNewPrivateStateFile(int descriptor, int expected_link_count)
    -> std::expected<void, QString> {
#if defined(Q_OS_LINUX)
    int result{};
    do {
        result = ::fchmod(descriptor, 0600);
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        return std::unexpected(nativeError(u"Normalize private state file mode"));
    }
    return validatePrivateStateFileDescriptor(descriptor, expected_link_count);
#else
    Q_UNUSED(descriptor);
    Q_UNUSED(expected_link_count);
    return std::unexpected(QStringLiteral("Private state filesystem validation requires Linux"));
#endif
}

auto validatePrivateStateFileBinding(int descriptor, int parent_descriptor, const QByteArray& name,
                                     int expected_link_count) -> std::expected<void, QString> {
#if defined(Q_OS_LINUX)
    if (name.isEmpty() || name.contains('\0')) {
        return std::unexpected(QStringLiteral("Private state filename is unsafe"));
    }
    struct stat held{};
    struct stat named{};
    if (::fstat(descriptor, &held) != 0 ||
        ::fstatat(parent_descriptor, name.constData(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
        !sameRegularFile(held, named, expected_link_count)) {
        return std::unexpected(QStringLiteral("Private state file binding is unsafe"));
    }
    if (const auto validated = validatePrivateStateFileDescriptor(descriptor, expected_link_count);
        !validated) {
        return validated;
    }
    struct stat rebound{};
    if (::fstatat(parent_descriptor, name.constData(), &rebound, AT_SYMLINK_NOFOLLOW) != 0 ||
        !sameRegularFile(held, rebound, expected_link_count)) {
        return std::unexpected(QStringLiteral("Private state file binding changed"));
    }
    return {};
#else
    Q_UNUSED(descriptor);
    Q_UNUSED(parent_descriptor);
    Q_UNUSED(name);
    Q_UNUSED(expected_link_count);
    return std::unexpected(QStringLiteral("Private state filesystem validation requires Linux"));
#endif
}

} // namespace appellate::storage::detail
