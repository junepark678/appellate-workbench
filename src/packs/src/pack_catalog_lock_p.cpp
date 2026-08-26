#include "pack_catalog_lock_p.hpp"

#include <QByteArray>
#include <QFile>
#include <QLockFile>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>

#if defined(Q_OS_LINUX)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>
#endif

namespace appellate::packs::detail {
namespace {

constexpr auto install_lock_component = ".install.lock";
constexpr auto install_remove_lock_component = ".install.lock.rmlock";
constexpr std::uint64_t maximum_legacy_lock_bytes = 4'096;

[[nodiscard]] auto lockFailure(CatalogHooks& hooks, CatalogErrorCode code, QString message)
    -> std::unexpected<CatalogError> {
    if (hooks.report != nullptr) {
        hooks.report->final_error = code;
    }
    return std::unexpected(CatalogError{code, std::move(message)});
}

[[nodiscard]] bool validOperationalFailureCode(CatalogErrorCode code) {
    return code == CatalogErrorCode::CannotOpen || code == CatalogErrorCode::CannotStoreArchive ||
           code == CatalogErrorCode::CannotStoreBlob;
}

[[nodiscard]] CatalogInjectedAction beginObservation(CatalogHooks& hooks,
                                                     const CatalogObservation& observation) {
    if (hooks.report != nullptr) {
        hooks.report->observations.push_back(observation);
    }
    return hooks.inject ? hooks.inject(observation) : CatalogInjectedAction::Continue;
}

[[nodiscard]] bool finishObservation(const CatalogHooks& hooks,
                                     const CatalogObservation& observation,
                                     CatalogInjectedAction action) {
    if (hooks.barrier) {
        hooks.barrier(observation);
    }
    if (hooks.observe) {
        hooks.observe(observation);
    }
    return action != CatalogInjectedAction::FailAfter;
}

[[nodiscard]] bool failsBefore(CatalogInjectedAction action) {
    return action == CatalogInjectedAction::FailBefore;
}

void recordResidue(CatalogHooks& hooks, const QString& path, bool ambiguous) {
    if (hooks.report == nullptr) {
        return;
    }
    if (std::ranges::find(hooks.report->remaining_ledger_paths, path) ==
        hooks.report->remaining_ledger_paths.end()) {
        hooks.report->remaining_ledger_paths.push_back(path);
    }
    hooks.report->cleanup = CatalogCleanupOutcome::Preserved;
    hooks.report->residue_identity_ambiguous |= ambiguous;
}

#if defined(Q_OS_LINUX)

class FileDescriptor final {
  public:
    FileDescriptor() noexcept = default;
    explicit FileDescriptor(int descriptor) noexcept : descriptor_(descriptor) {}
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.descriptor_, -1));
        }
        return *this;
    }
    ~FileDescriptor() { reset(); }

    [[nodiscard]] int get() const noexcept { return descriptor_; }
    [[nodiscard]] explicit operator bool() const noexcept { return descriptor_ >= 0; }

    void reset(int descriptor = -1) noexcept {
        if (descriptor_ >= 0) {
            static_cast<void>(::close(descriptor_));
        }
        descriptor_ = descriptor;
    }

  private:
    int descriptor_{-1};
};

struct NativeIdentity final {
    dev_t device{};
    ino_t inode{};
    mode_t mode{};
    nlink_t links{};
    uid_t owner{};
    off_t size{};
    timespec modified{};
    timespec changed{};
};

[[nodiscard]] NativeIdentity nativeIdentity(const struct stat& status) {
    return NativeIdentity{status.st_dev, status.st_ino,  status.st_mode, status.st_nlink,
                          status.st_uid, status.st_size, status.st_mtim, status.st_ctim};
}

[[nodiscard]] CatalogIdentity reportedIdentity(const NativeIdentity& identity) {
    auto type = CatalogNodeType::Other;
    if (S_ISDIR(identity.mode)) {
        type = CatalogNodeType::Directory;
    } else if (S_ISREG(identity.mode)) {
        type = CatalogNodeType::RegularFile;
    }
    return CatalogIdentity{
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

[[nodiscard]] bool sameIdentity(const NativeIdentity& expected, const struct stat& actual,
                                bool include_metadata = true) {
    const auto same_base = expected.device == actual.st_dev && expected.inode == actual.st_ino &&
                           expected.mode == actual.st_mode && expected.links == actual.st_nlink &&
                           expected.owner == actual.st_uid;
    if (!same_base || !include_metadata) {
        return same_base;
    }
    return expected.size == actual.st_size && expected.modified.tv_sec == actual.st_mtim.tv_sec &&
           expected.modified.tv_nsec == actual.st_mtim.tv_nsec &&
           expected.changed.tv_sec == actual.st_ctim.tv_sec &&
           expected.changed.tv_nsec == actual.st_ctim.tv_nsec;
}

[[nodiscard]] bool sameAttemptInode(const NativeIdentity& expected, const struct stat& actual) {
    return expected.device == actual.st_dev && expected.inode == actual.st_ino &&
           S_ISREG(actual.st_mode) && actual.st_nlink == 1 && actual.st_uid == ::geteuid();
}

[[nodiscard]] bool normalizedAttemptInode(const NativeIdentity& expected,
                                          const struct stat& actual) {
    return sameAttemptInode(expected, actual) && (actual.st_mode & 07777) == 0600 &&
           actual.st_size >= 1 &&
           static_cast<std::uint64_t>(actual.st_size) <= maximum_legacy_lock_bytes;
}

[[nodiscard]] bool retryingFstat(int descriptor, struct stat* status) {
    while (::fstat(descriptor, status) != 0) {
        if (errno != EINTR) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool retryingFstatat(int parent, const char* component, struct stat* status) {
    while (::fstatat(parent, component, status, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno != EINTR) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool retryingFsync(int descriptor) {
    while (::fsync(descriptor) != 0) {
        if (errno != EINTR) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool aclAbsent(int descriptor, const char* attribute) {
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

[[nodiscard]] bool directoryAclsAbsent(int descriptor) {
    return aclAbsent(descriptor, "system.posix_acl_access") &&
           aclAbsent(descriptor, "system.posix_acl_default");
}

[[nodiscard]] bool fileAclAbsent(int descriptor) {
    return aclAbsent(descriptor, "system.posix_acl_access");
}

[[nodiscard]] bool controllerPolicy(const struct stat& status) {
    return S_ISDIR(status.st_mode) && (status.st_uid == ::geteuid() || status.st_uid == 0) &&
           ((status.st_mode & 0022) == 0 || (status.st_mode & S_ISVTX) != 0);
}

[[nodiscard]] std::optional<QByteArray> encodeComponent(const QString& component) {
    if (component.isEmpty() || component == QStringLiteral(".") ||
        component == QStringLiteral("..") || component.contains(QChar::Null)) {
        return std::nullopt;
    }
    const auto encoded = QFile::encodeName(component);
    if (encoded.isEmpty() || encoded.size() > 255 || encoded.contains('\0') ||
        QFile::decodeName(encoded) != component) {
        return std::nullopt;
    }
    return encoded;
}

[[nodiscard]] bool immutableRootRebound(const QString& absolute_root, int borrowed_root_descriptor,
                                        NativeIdentity* retained_root = nullptr) {
    if (!absolute_root.startsWith(u'/') || absolute_root == QStringLiteral("/") ||
        absolute_root.contains(QChar::Null) || absolute_root.endsWith(u'/')) {
        return false;
    }
    const auto components = absolute_root.sliced(1).split(u'/', Qt::KeepEmptyParts);
    if (components.isEmpty() || components.size() > 126) {
        return false;
    }

    struct stat borrowed_status{};
    if (!retryingFstat(borrowed_root_descriptor, &borrowed_status) ||
        !S_ISDIR(borrowed_status.st_mode) || borrowed_status.st_uid != ::geteuid() ||
        (borrowed_status.st_mode & 07777) != 0700 ||
        !directoryAclsAbsent(borrowed_root_descriptor)) {
        return false;
    }

    FileDescriptor current(::open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat parent_status{};
    if (!current || !retryingFstat(current.get(), &parent_status) ||
        !controllerPolicy(parent_status) || !directoryAclsAbsent(current.get())) {
        return false;
    }

    for (qsizetype index = 0; index < components.size(); ++index) {
        const auto encoded = encodeComponent(components.at(index));
        if (!encoded) {
            return false;
        }
        struct stat named{};
        if (!retryingFstatat(current.get(), encoded->constData(), &named)) {
            return false;
        }
        FileDescriptor child(::openat(current.get(), encoded->constData(),
                                      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        struct stat held{};
        struct stat parent_rebound{};
        if (!child || !retryingFstat(child.get(), &held) ||
            !retryingFstat(current.get(), &parent_rebound) ||
            parent_rebound.st_dev != parent_status.st_dev ||
            parent_rebound.st_ino != parent_status.st_ino || held.st_dev != named.st_dev ||
            held.st_ino != named.st_ino || !directoryAclsAbsent(child.get())) {
            return false;
        }
        const auto final = index + 1 == components.size();
        if (final) {
            if (held.st_dev != borrowed_status.st_dev || held.st_ino != borrowed_status.st_ino ||
                held.st_uid != ::geteuid() || (held.st_mode & 07777) != 0700) {
                return false;
            }
        } else if (!controllerPolicy(held) || ((parent_status.st_mode & S_ISVTX) != 0 &&
                                               held.st_uid != ::geteuid() && held.st_uid != 0)) {
            return false;
        }
        parent_status = held;
        current = std::move(child);
    }

    struct stat borrowed_after{};
    if (!retryingFstat(borrowed_root_descriptor, &borrowed_after) ||
        borrowed_after.st_dev != borrowed_status.st_dev ||
        borrowed_after.st_ino != borrowed_status.st_ino ||
        (borrowed_after.st_mode & 07777) != 0700 || borrowed_after.st_uid != ::geteuid()) {
        return false;
    }
    if (retained_root != nullptr) {
        *retained_root = nativeIdentity(borrowed_after);
    }
    return true;
}

enum class NameState {
    Absent,
    SafeLegacy,
    ExactAttempt,
    Unsafe,
    RemoveLockPresent,
    Unprovable,
};

enum class AttemptMatchPolicy {
    ExactTuple,
    NormalizedInode,
    AnyInode,
};

struct NameInspection final {
    NameState state{NameState::Unprovable};
    std::optional<NativeIdentity> identity;
};

[[nodiscard]] bool nameAbsent(int root_descriptor, const char* component) {
    struct stat ignored{};
    if (retryingFstatat(root_descriptor, component, &ignored)) {
        return false;
    }
    return errno == ENOENT;
}

[[nodiscard]] NameInspection
inspectLegacyName(int root_descriptor, const std::optional<NativeIdentity>& attempt = {},
                  AttemptMatchPolicy attempt_policy = AttemptMatchPolicy::ExactTuple) {
    struct stat remove_lock{};
    if (retryingFstatat(root_descriptor, install_remove_lock_component, &remove_lock)) {
        return {NameState::RemoveLockPresent, nativeIdentity(remove_lock)};
    }
    if (errno != ENOENT) {
        return {NameState::Unprovable, std::nullopt};
    }

    struct stat named{};
    if (!retryingFstatat(root_descriptor, install_lock_component, &named)) {
        return errno == ENOENT ? NameInspection{NameState::Absent, std::nullopt}
                               : NameInspection{NameState::Unprovable, std::nullopt};
    }
    if (attempt && sameAttemptInode(*attempt, named)) {
        if (attempt_policy == AttemptMatchPolicy::AnyInode) {
            return {NameState::ExactAttempt, nativeIdentity(named)};
        }
        if ((attempt_policy == AttemptMatchPolicy::ExactTuple &&
             !sameIdentity(*attempt, named, true)) ||
            (attempt_policy == AttemptMatchPolicy::NormalizedInode &&
             !normalizedAttemptInode(*attempt, named))) {
            return {NameState::Unsafe, nativeIdentity(named)};
        }

        FileDescriptor opened(
            ::openat(root_descriptor, install_lock_component, O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
        struct stat held_before{};
        struct stat rebound_before{};
        struct stat held_after{};
        struct stat rebound_after{};
        const auto matches_policy = [&](const struct stat& status) {
            return attempt_policy == AttemptMatchPolicy::ExactTuple
                       ? sameIdentity(*attempt, status, true)
                       : normalizedAttemptInode(*attempt, status);
        };
        if (!opened || !retryingFstat(opened.get(), &held_before) ||
            !retryingFstatat(root_descriptor, install_lock_component, &rebound_before) ||
            !matches_policy(held_before) || !matches_policy(rebound_before) ||
            !fileAclAbsent(opened.get()) || !retryingFstat(opened.get(), &held_after) ||
            !retryingFstatat(root_descriptor, install_lock_component, &rebound_after) ||
            !matches_policy(held_after) || !matches_policy(rebound_after) ||
            !sameIdentity(nativeIdentity(held_before), held_after, true)) {
            return {NameState::Unsafe, nativeIdentity(named)};
        }
        return {NameState::ExactAttempt, nativeIdentity(held_after)};
    }
    if (!S_ISREG(named.st_mode) || named.st_nlink != 1 || named.st_uid != ::geteuid() ||
        named.st_size < 1 ||
        static_cast<std::uint64_t>(named.st_size) > maximum_legacy_lock_bytes ||
        (named.st_mode & 0400) == 0 || (named.st_mode & 0022) != 0) {
        return {NameState::Unsafe, nativeIdentity(named)};
    }

    FileDescriptor opened(
        ::openat(root_descriptor, install_lock_component, O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    struct stat held_before{};
    struct stat rebound_before{};
    struct stat held_after{};
    struct stat rebound_after{};
    if (!opened || !retryingFstat(opened.get(), &held_before) ||
        !retryingFstatat(root_descriptor, install_lock_component, &rebound_before) ||
        !sameIdentity(nativeIdentity(named), held_before, true) ||
        !sameIdentity(nativeIdentity(named), rebound_before, true) ||
        !fileAclAbsent(opened.get()) || !retryingFstat(opened.get(), &held_after) ||
        !retryingFstatat(root_descriptor, install_lock_component, &rebound_after) ||
        !sameIdentity(nativeIdentity(held_before), held_after, true) ||
        !sameIdentity(nativeIdentity(held_before), rebound_after, true)) {
        return {NameState::Unsafe, nativeIdentity(named)};
    }
    return {NameState::SafeLegacy, nativeIdentity(held_after)};
}

[[nodiscard]] bool chmodAtEmptyPath(int descriptor, mode_t mode) {
#if defined(SYS_fchmodat2)
    while (true) {
        if (::syscall(SYS_fchmodat2, descriptor, "", mode, AT_EMPTY_PATH) == 0) {
            return true;
        }
        if (errno != EINTR) {
            return false;
        }
    }
#else
    static_cast<void>(descriptor);
    static_cast<void>(mode);
    errno = ENOSYS;
    return false;
#endif
}

[[nodiscard]] CatalogObservation lockObservation(CatalogEvent event, CatalogOperation operation,
                                                 const QString& path) {
    CatalogObservation observation;
    observation.event = event;
    observation.subject = CatalogSubject::InstallLock;
    observation.operation = operation;
    observation.absolute_path = path;
    observation.component = QByteArrayLiteral(".install.lock");
    return observation;
}

[[nodiscard]] NameInspection
inspectLegacyNameObserved(int root_descriptor, const QString& lock_path, CatalogOperation operation,
                          CatalogHooks& hooks, const std::optional<NativeIdentity>& attempt = {},
                          std::size_t ordinal = 0,
                          AttemptMatchPolicy attempt_policy = AttemptMatchPolicy::ExactTuple) {
    auto observation = lockObservation(CatalogEvent::LegacyLockInspected, operation, lock_path);
    observation.ordinal = ordinal;
    const auto action = beginObservation(hooks, observation);
    const auto inspected = failsBefore(action)
                               ? NameInspection{NameState::Unprovable, std::nullopt}
                               : inspectLegacyName(root_descriptor, attempt, attempt_policy);
    if (inspected.identity) {
        observation.identity_after = reportedIdentity(*inspected.identity);
        if (hooks.report != nullptr && !hooks.report->observations.empty()) {
            hooks.report->observations.back() = observation;
        }
    }
    return finishObservation(hooks, observation, action)
               ? inspected
               : NameInspection{NameState::Unprovable, std::nullopt};
}

#endif

} // namespace

struct CatalogInstallLock::Impl final {
    QString root_path;
    QString lock_path;
    int borrowed_root_descriptor{-1};
    CatalogOperation operation{CatalogOperation::None};
    CatalogErrorCode operational_failure_code{CatalogErrorCode::CannotOpen};
    CatalogHooks hooks;
    CatalogIdentity public_identity;
#if defined(Q_OS_LINUX)
    NativeIdentity root_identity;
    NativeIdentity lock_identity;
    FileDescriptor path_descriptor;
    FileDescriptor real_descriptor;
    std::unique_ptr<QLockFile> lock;
#endif
    bool held{};
    bool released{};

    [[nodiscard]] auto revalidate() -> std::expected<void, CatalogError>;
    [[nodiscard]] auto teardown(bool acquisition_failure) -> std::expected<void, CatalogError>;
};

CatalogInstallLock::CatalogInstallLock(std::unique_ptr<Impl> state) : impl_(std::move(state)) {}

CatalogInstallLock::CatalogInstallLock(CatalogInstallLock&&) noexcept = default;

CatalogInstallLock& CatalogInstallLock::operator=(CatalogInstallLock&& other) noexcept {
    if (this != &other) {
        if (impl_ != nullptr && !impl_->released) {
            static_cast<void>(impl_->teardown(false));
        }
        impl_ = std::move(other.impl_);
    }
    return *this;
}

CatalogInstallLock::~CatalogInstallLock() {
    if (impl_ != nullptr && !impl_->released) {
        static_cast<void>(impl_->teardown(false));
    }
}

bool CatalogInstallLock::isHeld() const noexcept {
    return impl_ != nullptr && impl_->held && !impl_->released;
}

const CatalogIdentity& CatalogInstallLock::identity() const noexcept {
    static const CatalogIdentity empty_identity;
    return impl_ == nullptr ? empty_identity : impl_->public_identity;
}

const QString& CatalogInstallLock::absolutePath() const noexcept {
    static const QString empty_path;
    return impl_ == nullptr ? empty_path : impl_->lock_path;
}

std::expected<void, CatalogError> CatalogInstallLock::revalidate() {
    if (impl_ == nullptr) {
        return std::unexpected(CatalogError{CatalogErrorCode::CorruptCatalog,
                                            QStringLiteral("Catalog lock state is unavailable")});
    }
    return impl_->revalidate();
}

std::expected<void, CatalogError> CatalogInstallLock::Impl::revalidate() {
#if !defined(Q_OS_LINUX)
    return lockFailure(hooks, CatalogErrorCode::CannotOpen,
                       QStringLiteral("Catalog lock revalidation is unsupported"));
#else
    if (released || !held) {
        return lockFailure(hooks, CatalogErrorCode::CorruptCatalog,
                           QStringLiteral("Attempt-owned catalog lock is not held"));
    }
    struct stat root_status{};
    const auto root_safe = retryingFstat(borrowed_root_descriptor, &root_status) &&
                           immutableRootRebound(root_path, borrowed_root_descriptor) &&
                           sameIdentity(root_identity, root_status, false);
    const auto inspected =
        inspectLegacyNameObserved(borrowed_root_descriptor, lock_path, operation, hooks,
                                  lock_identity, 0, AttemptMatchPolicy::ExactTuple);
    if (!root_safe || inspected.state != NameState::ExactAttempt) {
        if (hooks.report != nullptr) {
            hooks.report->residue_identity_ambiguous = true;
        }
        return lockFailure(hooks, CatalogErrorCode::CorruptCatalog,
                           QStringLiteral("Catalog lock binding became ambiguous"));
    }
    return {};
#endif
}

std::expected<void, CatalogError> CatalogInstallLock::release() {
    if (impl_ == nullptr || impl_->released) {
        return {};
    }
    return impl_->teardown(false);
}

std::expected<void, CatalogError> CatalogInstallLock::Impl::teardown(bool acquisition_failure) {
    if (released) {
        return {};
    }
    released = true;
#if !defined(Q_OS_LINUX)
    held = false;
    return lockFailure(hooks, operational_failure_code,
                       QStringLiteral("Catalog lock teardown is unsupported on this platform"));
#else
    const auto root_safe =
        immutableRootRebound(root_path, borrowed_root_descriptor) &&
        sameIdentity(
            root_identity,
            [&] {
                struct stat status{};
                static_cast<void>(retryingFstat(borrowed_root_descriptor, &status));
                return status;
            }(),
            false);
    const auto before = inspectLegacyNameObserved(
        borrowed_root_descriptor, lock_path, operation, hooks, lock_identity, 0,
        acquisition_failure ? AttemptMatchPolicy::AnyInode : AttemptMatchPolicy::ExactTuple);
    const auto exact_attempt = before.state == NameState::ExactAttempt;
    if (!root_safe || (!exact_attempt && before.state != NameState::Absent)) {
        if (hooks.report != nullptr) {
            hooks.report->forced_teardown = true;
            hooks.report->residue_identity_ambiguous = true;
        }
        auto forced = lockObservation(CatalogEvent::ForcedTeardown, operation, lock_path);
        const auto action = beginObservation(hooks, forced);
        lock.reset();
        held = false;
        path_descriptor.reset();
        real_descriptor.reset();
        static_cast<void>(finishObservation(hooks, forced, action));
        return lockFailure(hooks, CatalogErrorCode::CorruptCatalog,
                           QStringLiteral("Catalog lock binding became ambiguous"));
    }

    auto destroyed = lockObservation(CatalogEvent::LockDestroyed, operation, lock_path);
    const auto destroy_action = beginObservation(hooks, destroyed);
    lock.reset();
    held = false;
    path_descriptor.reset();
    real_descriptor.reset();
    const auto destroy_observed = finishObservation(hooks, destroyed, destroy_action);
    const auto destroy_finished = !failsBefore(destroy_action) && destroy_observed;

    auto inspected = lockObservation(CatalogEvent::CleanupInspected, operation, lock_path);
    const auto inspect_action = beginObservation(hooks, inspected);
    const auto after =
        failsBefore(inspect_action)
            ? NameInspection{NameState::Unprovable, std::nullopt}
            : inspectLegacyNameObserved(borrowed_root_descriptor, lock_path, operation, hooks,
                                        lock_identity, 0,
                                        acquisition_failure ? AttemptMatchPolicy::AnyInode
                                                            : AttemptMatchPolicy::NormalizedInode);
    const auto inspect_finished = finishObservation(hooks, inspected, inspect_action);
    if (!destroy_finished || !inspect_finished || after.state == NameState::RemoveLockPresent ||
        after.state == NameState::Unsafe || after.state == NameState::SafeLegacy ||
        after.state == NameState::Unprovable) {
        if (after.state != NameState::Absent) {
            recordResidue(hooks,
                          after.state == NameState::RemoveLockPresent
                              ? root_path + QStringLiteral("/.install.lock.rmlock")
                              : lock_path,
                          true);
        } else if (hooks.report != nullptr) {
            hooks.report->residue_identity_ambiguous = true;
        }
        return lockFailure(hooks, CatalogErrorCode::CorruptCatalog,
                           QStringLiteral("Catalog lock teardown left ambiguous state"));
    }
    if (after.state == NameState::ExactAttempt) {
        recordResidue(hooks, lock_path, false);
        return lockFailure(hooks,
                           acquisition_failure ? CatalogErrorCode::CorruptCatalog
                                               : operational_failure_code,
                           QStringLiteral("Catalog lock removal left attempt-owned residue"));
    }

    if (hooks.report != nullptr) {
        hooks.report->cleanup = CatalogCleanupOutcome::Removed;
    }

    auto synced = lockObservation(CatalogEvent::CleanupSynced, operation, root_path);
    synced.subject = CatalogSubject::CatalogRoot;
    const auto sync_action = beginObservation(hooks, synced);
    const auto sync_succeeded = !failsBefore(sync_action) &&
                                retryingFsync(borrowed_root_descriptor) &&
                                immutableRootRebound(root_path, borrowed_root_descriptor);
    const auto sync_finished = finishObservation(hooks, synced, sync_action);
    if (!sync_succeeded || !sync_finished) {
        return lockFailure(hooks, operational_failure_code,
                           QStringLiteral("Cannot durably finish catalog lock teardown"));
    }
    return {};
#endif
}

std::expected<CatalogInstallLock, CatalogError>
acquireCatalogInstallLock(const QString& immutable_absolute_root, int borrowed_root_descriptor,
                          CatalogOperation operation, CatalogHooks hooks,
                          CatalogErrorCode operational_failure_code) {
    if (!validOperationalFailureCode(operational_failure_code)) {
        return lockFailure(hooks, CatalogErrorCode::InvalidConfiguration,
                           QStringLiteral("Catalog lock adapter received an invalid failure code"));
    }
#if !defined(Q_OS_LINUX)
    static_cast<void>(immutable_absolute_root);
    static_cast<void>(borrowed_root_descriptor);
    static_cast<void>(operation);
    return lockFailure(hooks, operational_failure_code,
                       QStringLiteral("Catalog lock acquisition is unsupported on this platform"));
#else
    NativeIdentity root_identity;
    if (hooks.report != nullptr && hooks.report->immutable_root_path.isEmpty()) {
        hooks.report->immutable_root_path = immutable_absolute_root;
    }
    if (!immutableRootRebound(immutable_absolute_root, borrowed_root_descriptor, &root_identity)) {
        return lockFailure(
            hooks, CatalogErrorCode::CorruptCatalog,
            QStringLiteral("Catalog root cannot be rebound before lock acquisition"));
    }
    const auto lock_path = immutable_absolute_root + QStringLiteral("/.install.lock");
    const auto initial =
        inspectLegacyNameObserved(borrowed_root_descriptor, lock_path, operation, hooks);
    if (initial.state == NameState::SafeLegacy) {
        return lockFailure(hooks, CatalogErrorCode::CatalogBusy,
                           QStringLiteral("Catalog mutation lock is present"));
    }
    if (initial.state != NameState::Absent) {
        recordResidue(hooks,
                      initial.state == NameState::RemoveLockPresent
                          ? immutable_absolute_root + QStringLiteral("/.install.lock.rmlock")
                          : lock_path,
                      true);
        return lockFailure(hooks, CatalogErrorCode::CorruptCatalog,
                           QStringLiteral("Catalog lock namespace is unsafe"));
    }

    for (std::size_t attempt = 0; attempt < 2; ++attempt) {
        if (!immutableRootRebound(immutable_absolute_root, borrowed_root_descriptor) ||
            !nameAbsent(borrowed_root_descriptor, install_lock_component) ||
            !nameAbsent(borrowed_root_descriptor, install_remove_lock_component)) {
            const auto raced = inspectLegacyNameObserved(borrowed_root_descriptor, lock_path,
                                                         operation, hooks, {}, attempt);
            if (raced.state == NameState::SafeLegacy) {
                return lockFailure(hooks, CatalogErrorCode::CatalogBusy,
                                   QStringLiteral("Catalog mutation lock is present"));
            }
            return lockFailure(hooks, CatalogErrorCode::CorruptCatalog,
                               QStringLiteral("Catalog lock namespace changed before acquisition"));
        }

        auto state = std::make_unique<CatalogInstallLock::Impl>();
        state->root_path = immutable_absolute_root;
        state->lock_path = lock_path;
        state->borrowed_root_descriptor = borrowed_root_descriptor;
        state->operation = operation;
        state->operational_failure_code = operational_failure_code;
        state->hooks = hooks;
        state->root_identity = root_identity;

        auto constructed = lockObservation(CatalogEvent::LockConstructed, operation, lock_path);
        constructed.ordinal = attempt;
        auto action = beginObservation(state->hooks, constructed);
        if (failsBefore(action)) {
            return lockFailure(state->hooks, operational_failure_code,
                               QStringLiteral("Injected catalog lock construction failure"));
        }
        state->lock = std::make_unique<QLockFile>(lock_path);
        if (!finishObservation(state->hooks, constructed, action)) {
            state->lock.reset();
            return lockFailure(state->hooks, operational_failure_code,
                               QStringLiteral("Injected catalog lock construction failure"));
        }

        auto stale = lockObservation(CatalogEvent::LockStaleTimeSet, operation, lock_path);
        stale.ordinal = attempt;
        action = beginObservation(state->hooks, stale);
        if (failsBefore(action)) {
            state->lock.reset();
            return lockFailure(state->hooks, operational_failure_code,
                               QStringLiteral("Injected stale-lock configuration failure"));
        }
        state->lock->setStaleLockTime(0);
        if (!finishObservation(state->hooks, stale, action)) {
            state->lock.reset();
            return lockFailure(state->hooks, operational_failure_code,
                               QStringLiteral("Injected stale-lock configuration failure"));
        }

        auto tried = lockObservation(CatalogEvent::LockTried, operation, lock_path);
        tried.ordinal = attempt;
        action = beginObservation(state->hooks, tried);
        auto acquired = false;
        auto error = QLockFile::UnknownError;
        const auto injected = state->hooks.outcome ? state->hooks.outcome(tried) : std::nullopt;
        if (!failsBefore(action) && state->hooks.report != nullptr) {
            ++state->hooks.report->lock_attempts;
        }
        if (!failsBefore(action) && (!injected || injected->operation_succeeded)) {
            acquired = state->lock->tryLock();
            error = state->lock->error();
        } else if (injected) {
            error = static_cast<QLockFile::LockError>(injected->library_error);
        }
        const auto try_observed = finishObservation(state->hooks, tried, action);
        const auto try_finished = !failsBefore(action) && try_observed;
        if (acquired && try_finished) {
            state->held = true;
            state->path_descriptor.reset(::openat(borrowed_root_descriptor, install_lock_component,
                                                  O_PATH | O_NOFOLLOW | O_CLOEXEC));
            struct stat provisional{};
            const auto provisional_retained =
                state->path_descriptor && retryingFstat(state->path_descriptor.get(), &provisional);
            if (provisional_retained) {
                state->lock_identity = nativeIdentity(provisional);
                state->public_identity = reportedIdentity(state->lock_identity);
            }
            const auto retained_name =
                provisional_retained
                    ? inspectLegacyNameObserved(borrowed_root_descriptor, lock_path, operation,
                                                state->hooks, state->lock_identity, attempt,
                                                AttemptMatchPolicy::AnyInode)
                    : NameInspection{NameState::Unprovable, std::nullopt};
            if (!provisional_retained || retained_name.state != NameState::ExactAttempt ||
                !S_ISREG(provisional.st_mode) || provisional.st_nlink != 1 ||
                provisional.st_uid != ::geteuid() || provisional.st_size < 1 ||
                static_cast<std::uint64_t>(provisional.st_size) > maximum_legacy_lock_bytes) {
                const auto cleaned = state->teardown(true);
                if (!cleaned && cleaned.error().code == CatalogErrorCode::CorruptCatalog) {
                    return std::unexpected(cleaned.error());
                }
                return lockFailure(state->hooks, operational_failure_code,
                                   QStringLiteral("Cannot retain attempt-owned catalog lock"));
            }

            auto retained = lockObservation(CatalogEvent::IdentityRetained, operation, lock_path);
            retained.identity_after = state->public_identity;
            action = beginObservation(state->hooks, retained);
            if (failsBefore(action) || !finishObservation(state->hooks, retained, action)) {
                const auto cleaned = state->teardown(true);
                if (!cleaned && cleaned.error().code == CatalogErrorCode::CorruptCatalog) {
                    return std::unexpected(cleaned.error());
                }
                return lockFailure(state->hooks, operational_failure_code,
                                   QStringLiteral("Injected catalog lock-retention failure"));
            }

            auto normalized = lockObservation(CatalogEvent::FileNormalized, operation, lock_path);
            normalized.mode_before = static_cast<unsigned int>(provisional.st_mode & 07777);
            normalized.mode_after = 0600;
            normalized.identity_before = state->public_identity;
            action = beginObservation(state->hooks, normalized);
            const auto chmod_succeeded =
                !failsBefore(action) && chmodAtEmptyPath(state->path_descriptor.get(), 0600);
            struct stat transitioned{};
            const auto transition_proved =
                retryingFstat(state->path_descriptor.get(), &transitioned) &&
                transitioned.st_dev == provisional.st_dev &&
                transitioned.st_ino == provisional.st_ino && transitioned.st_uid == ::geteuid() &&
                transitioned.st_nlink == 1 && (transitioned.st_mode & 07777) == 0600;
            if (transition_proved) {
                state->lock_identity = nativeIdentity(transitioned);
                state->public_identity = reportedIdentity(state->lock_identity);
                normalized.identity_after = state->public_identity;
            }
            if (state->hooks.report != nullptr && !state->hooks.report->observations.empty()) {
                state->hooks.report->observations.back() = normalized;
            }
            const auto normalize_finished = finishObservation(state->hooks, normalized, action);
            if (!chmod_succeeded || !transition_proved || !normalize_finished) {
                const auto cleaned = state->teardown(true);
                if (!cleaned && cleaned.error().code == CatalogErrorCode::CorruptCatalog) {
                    return std::unexpected(cleaned.error());
                }
                return lockFailure(state->hooks, operational_failure_code,
                                   QStringLiteral("Cannot normalize attempt-owned catalog lock"));
            }
            state->real_descriptor.reset(::openat(borrowed_root_descriptor, install_lock_component,
                                                  O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
            struct stat real_status{};
            struct stat named_status{};
            const auto bound =
                state->real_descriptor &&
                retryingFstat(state->real_descriptor.get(), &real_status) &&
                retryingFstatat(borrowed_root_descriptor, install_lock_component, &named_status) &&
                sameIdentity(state->lock_identity, real_status, true) &&
                sameIdentity(state->lock_identity, named_status, true);

            auto probed = lockObservation(CatalogEvent::AclProbed, operation, lock_path);
            probed.acl_kind = CatalogAclKind::Access;
            action = beginObservation(state->hooks, probed);
            const auto acl_absent =
                bound && !failsBefore(action) && fileAclAbsent(state->real_descriptor.get());
            struct stat acl_held_after{};
            struct stat acl_named_after{};
            const auto acl_safe = acl_absent &&
                                  retryingFstat(state->real_descriptor.get(), &acl_held_after) &&
                                  retryingFstatat(borrowed_root_descriptor, install_lock_component,
                                                  &acl_named_after) &&
                                  sameIdentity(state->lock_identity, acl_held_after, true) &&
                                  sameIdentity(state->lock_identity, acl_named_after, true);
            const auto acl_finished = finishObservation(state->hooks, probed, action);

            auto file_synced = lockObservation(CatalogEvent::FileSynced, operation, lock_path);
            action = beginObservation(state->hooks, file_synced);
            const auto file_sync_completed = acl_safe && acl_finished && !failsBefore(action) &&
                                             retryingFsync(state->real_descriptor.get());
            struct stat sync_held_after{};
            struct stat sync_named_after{};
            const auto file_sync_safe =
                file_sync_completed &&
                retryingFstat(state->real_descriptor.get(), &sync_held_after) &&
                retryingFstatat(borrowed_root_descriptor, install_lock_component,
                                &sync_named_after) &&
                sameIdentity(state->lock_identity, sync_held_after, true) &&
                sameIdentity(state->lock_identity, sync_named_after, true);
            const auto file_sync_finished = finishObservation(state->hooks, file_synced, action);

            auto root_synced =
                lockObservation(CatalogEvent::DirectorySynced, operation, immutable_absolute_root);
            root_synced.subject = CatalogSubject::CatalogRoot;
            action = beginObservation(state->hooks, root_synced);
            const auto root_sync_safe =
                file_sync_safe && file_sync_finished && !failsBefore(action) &&
                retryingFsync(borrowed_root_descriptor) &&
                immutableRootRebound(immutable_absolute_root, borrowed_root_descriptor);
            const auto root_sync_finished = finishObservation(state->hooks, root_synced, action);
            const auto final_binding =
                root_sync_safe && root_sync_finished
                    ? inspectLegacyNameObserved(borrowed_root_descriptor, lock_path, operation,
                                                state->hooks, state->lock_identity, attempt,
                                                AttemptMatchPolicy::ExactTuple)
                    : NameInspection{NameState::Unprovable, std::nullopt};
            if (!root_sync_safe || !root_sync_finished ||
                final_binding.state != NameState::ExactAttempt) {
                const auto cleaned = state->teardown(true);
                if (!cleaned && cleaned.error().code == CatalogErrorCode::CorruptCatalog) {
                    return std::unexpected(cleaned.error());
                }
                return lockFailure(state->hooks, operational_failure_code,
                                   QStringLiteral("Cannot validate attempt-owned catalog lock"));
            }
            return CatalogInstallLock(std::move(state));
        }

        if (acquired) {
            state->held = true;
            state->path_descriptor.reset(::openat(borrowed_root_descriptor, install_lock_component,
                                                  O_PATH | O_NOFOLLOW | O_CLOEXEC));
            struct stat status{};
            if (state->path_descriptor && retryingFstat(state->path_descriptor.get(), &status)) {
                state->lock_identity = nativeIdentity(status);
                state->public_identity = reportedIdentity(state->lock_identity);
            }
            const auto cleaned = state->teardown(true);
            if (!cleaned && cleaned.error().code == CatalogErrorCode::CorruptCatalog) {
                return std::unexpected(cleaned.error());
            }
            return lockFailure(state->hooks, operational_failure_code,
                               QStringLiteral("Injected catalog lock failure after acquisition"));
        }

        auto destroyed = lockObservation(CatalogEvent::LockDestroyed, operation, lock_path);
        destroyed.ordinal = attempt;
        const auto destroy_action = beginObservation(state->hooks, destroyed);
        state->lock.reset();
        const auto destroy_observed = finishObservation(state->hooks, destroyed, destroy_action);
        const auto destroy_finished = !failsBefore(destroy_action) && destroy_observed;
        const auto after = inspectLegacyNameObserved(borrowed_root_descriptor, lock_path, operation,
                                                     state->hooks, {}, attempt);
        if (after.state == NameState::SafeLegacy) {
            return lockFailure(state->hooks, CatalogErrorCode::CatalogBusy,
                               QStringLiteral("Catalog mutation lock is present"));
        }
        if (after.state != NameState::Absent) {
            recordResidue(state->hooks,
                          after.state == NameState::RemoveLockPresent
                              ? immutable_absolute_root + QStringLiteral("/.install.lock.rmlock")
                              : lock_path,
                          true);
            return lockFailure(state->hooks, CatalogErrorCode::CorruptCatalog,
                               QStringLiteral("Catalog lock failure left ambiguous state"));
        }
        if (!try_finished || !destroy_finished) {
            return lockFailure(state->hooks, operational_failure_code,
                               QStringLiteral("Injected catalog lock attempt failure"));
        }
        if (error != QLockFile::LockFailedError || attempt != 0) {
            return lockFailure(state->hooks, operational_failure_code,
                               QStringLiteral("Cannot acquire catalog mutation lock"));
        }
    }
    return lockFailure(hooks, operational_failure_code,
                       QStringLiteral("Cannot acquire catalog mutation lock"));
#endif
}

} // namespace appellate::packs::detail
