#include "independent_review_publisher_p.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <ranges>
#include <set>
#include <string_view>
#include <utility>

#if defined(Q_OS_LINUX)
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>
#endif

namespace appellate::cli::detail {
namespace {

constexpr qsizetype maximum_native_path_bytes = 4'095;
constexpr qsizetype maximum_native_component_bytes = 255;
constexpr qsizetype maximum_destination_leaf_bytes = 218;
constexpr std::size_t maximum_native_components = 128;
constexpr std::size_t maximum_protected_entries = 20'000;
constexpr std::size_t maximum_staging_attempts = 128;
constexpr qsizetype maximum_handoff_bytes = 16 * 1024 * 1024;
constexpr qsizetype maximum_template_bytes = 1024 * 1024;
constexpr qsizetype maximum_declaration_bytes = 2 * 1024 * 1024;

#if defined(Q_OS_LINUX)

[[nodiscard]] IndependentReviewPublisherInjectedAction
publisherEvent(const IndependentReviewPublisherHooks& hooks,
               IndependentReviewPublisherObservation observation) {
    if (hooks.report != nullptr) {
        hooks.report->observations.push_back(observation);
    }
    if (hooks.observe) {
        hooks.observe(observation);
    }
    if (hooks.barrier) {
        hooks.barrier(observation);
    }
    return hooks.inject ? hooks.inject(observation)
                        : IndependentReviewPublisherInjectedAction::Continue;
}

[[nodiscard]] bool failsBefore(IndependentReviewPublisherInjectedAction action) {
    return action == IndependentReviewPublisherInjectedAction::FailBefore;
}

[[nodiscard]] bool failsAfter(IndependentReviewPublisherInjectedAction action) {
    return action == IndependentReviewPublisherInjectedAction::FailAfter;
}

[[nodiscard]] auto publisherOutcome(const IndependentReviewPublisherHooks& hooks,
                                    const IndependentReviewPublisherObservation& observation)
    -> std::optional<IndependentReviewPublisherInjectedOutcome> {
    return hooks.outcome ? hooks.outcome(observation) : std::nullopt;
}

#endif

[[nodiscard]] auto publicationFailure(IndependentReviewPublicationErrorCode code, QString message)
    -> std::unexpected<IndependentReviewPublicationError> {
    return std::unexpected(IndependentReviewPublicationError{code, std::move(message)});
}

[[nodiscard]] bool isValidNativeSpelling(const QString& value, qsizetype maximum_bytes) {
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

struct ValidatedPathComponents final {
    QStringList text;
    std::vector<QByteArray> native;
    QByteArray native_path;
};

[[nodiscard]] std::optional<ValidatedPathComponents> suppliedComponents(const QString& path) {
    if (path.isEmpty() || !isValidNativeSpelling(path, maximum_native_path_bytes)) {
        return std::nullopt;
    }
    const auto absolute = path.startsWith(u'/');
    if (absolute && path == QStringLiteral("/")) {
        return ValidatedPathComponents{QStringList{}, {}, QByteArray{"/"}};
    }
    const auto body = absolute ? path.sliced(1) : path;
    const auto components = body.split(u'/', Qt::KeepEmptyParts);
    if (components.isEmpty() ||
        static_cast<std::size_t>(components.size()) > maximum_native_components ||
        std::ranges::any_of(components, [](const QString& component) {
            return component.isEmpty() || component == QStringLiteral(".") ||
                   component == QStringLiteral("..") ||
                   !isValidNativeSpelling(component, maximum_native_component_bytes);
        })) {
        return std::nullopt;
    }
    std::vector<QByteArray> native_components;
    native_components.reserve(static_cast<std::size_t>(components.size()));
    for (const auto& component : components) {
        native_components.push_back(QFile::encodeName(component));
    }
    return ValidatedPathComponents{components, std::move(native_components),
                                   QFile::encodeName(path)};
}

[[nodiscard]] std::optional<IndependentReviewPathToken> encodedPathToken(const QString& path,
                                                                         bool destination_leaf) {
    auto components = suppliedComponents(path);
    if (!components || components->text.isEmpty() ||
        (destination_leaf && components->native.back().size() > maximum_destination_leaf_bytes)) {
        return std::nullopt;
    }
    return IndependentReviewPathToken{path, std::move(components->text),
                                      std::move(components->native),
                                      std::move(components->native_path), destination_leaf};
}

#if defined(Q_OS_LINUX)

[[nodiscard]] bool isConsistentPathToken(const IndependentReviewPathToken& token) {
    if (token.supplied_path.isEmpty() || token.supplied_components.isEmpty() ||
        token.supplied_native_components.size() !=
            static_cast<std::size_t>(token.supplied_components.size()) ||
        token.supplied_native_path.isEmpty() || token.supplied_native_path.contains('\0') ||
        token.supplied_native_path.size() > maximum_native_path_bytes ||
        QFile::decodeName(token.supplied_native_path) != token.supplied_path ||
        (token.destination_leaf &&
         token.supplied_native_components.back().size() > maximum_destination_leaf_bytes)) {
        return false;
    }
    QByteArray rebuilt = token.supplied_path.startsWith(u'/') ? QByteArray{"/"} : QByteArray{};
    for (std::size_t index = 0; index < token.supplied_native_components.size(); ++index) {
        const auto& native = token.supplied_native_components.at(index);
        if (native.isEmpty() || native.size() > maximum_native_component_bytes ||
            native.contains('\0') || native.contains('/') || native == "." || native == ".." ||
            QFile::decodeName(native) !=
                token.supplied_components.at(static_cast<qsizetype>(index))) {
            return false;
        }
        if (!rebuilt.isEmpty() && rebuilt != "/") {
            rebuilt += '/';
        }
        rebuilt += native;
    }
    return rebuilt == token.supplied_native_path;
}

[[nodiscard]] bool destinationTokenHasHeadroom(const IndependentReviewPathToken& token,
                                               IndependentReviewArtifactKind kind) {
    if (!isConsistentPathToken(token) || !token.destination_leaf) {
        return false;
    }
    const auto maximum_components =
        kind == IndependentReviewArtifactKind::PreparedHandoff ? 127U : 126U;
    if (static_cast<std::size_t>(token.supplied_components.size()) > maximum_components) {
        return false;
    }
    if (!token.supplied_path.startsWith(u'/')) {
        return true;
    }
    const auto staging_name = QByteArray{"."} + token.supplied_native_components.back() +
                              QByteArray{".appellate-independent-review-"} + QByteArray(6, 'x');
    QByteArray parent_native{"/"};
    for (std::size_t index = 0; index + 1U < token.supplied_native_components.size(); ++index) {
        if (parent_native.size() > 1) {
            parent_native += '/';
        }
        parent_native += token.supplied_native_components.at(index);
    }
    const auto staging_native =
        parent_native == "/" ? parent_native + staging_name : parent_native + '/' + staging_name;
    const auto longest_member = kind == IndependentReviewArtifactKind::PreparedHandoff
                                    ? QByteArrayView{"/review-declaration.template.json"}
                                    : QByteArrayView{"/resources/realism-review.json"};
    return staging_name.size() <= maximum_native_component_bytes &&
           staging_native.size() + longest_member.size() <= maximum_native_path_bytes;
}

[[nodiscard]] QString nativeError(const QString& action, int native_error = errno) {
    return QStringLiteral("%1: %2").arg(action,
                                        QString::fromLocal8Bit(std::strerror(native_error)));
}

class Descriptor final {
  public:
    explicit Descriptor(int descriptor = -1) noexcept : descriptor_(descriptor) {}
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}
    Descriptor& operator=(Descriptor&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.descriptor_, -1));
        }
        return *this;
    }
    ~Descriptor() { reset(); }

    [[nodiscard]] int get() const noexcept { return descriptor_; }
    [[nodiscard]] explicit operator bool() const noexcept { return descriptor_ >= 0; }

    [[nodiscard]] int release() noexcept { return std::exchange(descriptor_, -1); }

    void reset(int descriptor = -1) noexcept {
        if (descriptor_ >= 0) {
            // Linux releases the descriptor even when close reports EINTR. Retrying could close an
            // unrelated descriptor that another thread acquired under the same number.
            static_cast<void>(::close(descriptor_));
        }
        descriptor_ = descriptor;
    }

  private:
    int descriptor_;
};

struct NodeIdentity final {
    dev_t device{};
    ino_t inode{};
    mode_t type{};
    uid_t owner{};
    mode_t mode{};
    nlink_t link_count{};
    off_t byte_size{};
    timespec modified{};
    timespec changed{};
};

enum class PublisherBindingState {
    Absent,
    Exact,
    Other,
    Ambiguous,
};

[[nodiscard]] NodeIdentity identityOf(const struct stat& status) {
    return NodeIdentity{
        status.st_dev,  status.st_ino,          status.st_mode & S_IFMT,
        status.st_uid,  status.st_mode & 07777, status.st_nlink,
        status.st_size, status.st_mtim,         status.st_ctim,
    };
}

[[nodiscard]] NodeIdentity
applySyntheticStat(NodeIdentity identity,
                   const IndependentReviewPublisherSyntheticStat& synthetic) {
    if (synthetic.device.has_value()) {
        identity.device = static_cast<dev_t>(*synthetic.device);
    }
    if (synthetic.inode.has_value()) {
        identity.inode = static_cast<ino_t>(*synthetic.inode);
    }
    if (synthetic.type.has_value()) {
        switch (*synthetic.type) {
        case IndependentReviewPublisherSyntheticNodeType::Directory:
            identity.type = S_IFDIR;
            break;
        case IndependentReviewPublisherSyntheticNodeType::RegularFile:
            identity.type = S_IFREG;
            break;
        case IndependentReviewPublisherSyntheticNodeType::Other:
            identity.type = S_IFIFO;
            break;
        }
    }
    if (synthetic.owner.has_value()) {
        identity.owner = static_cast<uid_t>(*synthetic.owner);
    }
    if (synthetic.mode.has_value()) {
        identity.mode = static_cast<mode_t>(*synthetic.mode) & 07777;
    }
    if (synthetic.link_count.has_value()) {
        identity.link_count = static_cast<nlink_t>(*synthetic.link_count);
    }
    return identity;
}

[[nodiscard]] bool sameTimespec(const timespec& left, const timespec& right) {
    return left.tv_sec == right.tv_sec && left.tv_nsec == right.tv_nsec;
}

[[nodiscard]] bool sameDirectoryIdentity(const NodeIdentity& left, const NodeIdentity& right) {
    return left.device == right.device && left.inode == right.inode && left.type == S_IFDIR &&
           right.type == S_IFDIR && left.owner == right.owner && left.mode == right.mode;
}

[[nodiscard]] bool sameFileIdentity(const NodeIdentity& left, const NodeIdentity& right) {
    return left.device == right.device && left.inode == right.inode && left.type == S_IFREG &&
           right.type == S_IFREG && left.owner == right.owner && left.mode == right.mode &&
           left.link_count == 1 && right.link_count == 1;
}

[[nodiscard]] bool sameStableFileSnapshot(const NodeIdentity& left, const NodeIdentity& right) {
    return sameFileIdentity(left, right) && left.byte_size == right.byte_size &&
           sameTimespec(left.modified, right.modified) && sameTimespec(left.changed, right.changed);
}

[[nodiscard]] bool statDescriptor(int descriptor, struct stat* status) {
    while (::fstat(descriptor, status) != 0) {
        if (errno != EINTR) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool statNamed(int parent_descriptor, const QByteArray& component,
                             struct stat* status) {
    while (::fstatat(parent_descriptor, component.constData(), status, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno != EINTR) {
            return false;
        }
    }
    return true;
}

enum class AclResult {
    Absent,
    Present,
    Unsupported,
    Failure,
};

enum class PreflightAvailability {
    Available,
    Unsupported,
    Failure,
};

[[nodiscard]] AclResult aclResultForError(int native_error) {
    if (native_error == ENODATA) {
        return AclResult::Absent;
    }
#if defined(ENOATTR) && ENOATTR != ENODATA
    if (native_error == ENOATTR) {
        return AclResult::Absent;
    }
#endif
    if (native_error == E2BIG || native_error == ERANGE) {
        return AclResult::Present;
    }
    if (native_error == ENOTSUP || native_error == EOPNOTSUPP || native_error == ENOSYS) {
        return AclResult::Unsupported;
    }
    return AclResult::Failure;
}

[[nodiscard]] AclResult probePublisherAcl(int descriptor, bool default_acl,
                                          const QString& absolute_path, const QByteArray& component,
                                          std::size_t ordinal,
                                          const IndependentReviewPublisherHooks& hooks) {
    const auto observation = IndependentReviewPublisherObservation{
        default_acl ? IndependentReviewPublisherEvent::DefaultAclProbe
                    : IndependentReviewPublisherEvent::AccessAclProbe,
        absolute_path, component, ordinal};
    while (true) {
        const auto action = publisherEvent(hooks, observation);
        if (failsBefore(action)) {
            return AclResult::Failure;
        }
        const auto injected = publisherOutcome(hooks, observation);
        AclResult result{};
        if (injected.has_value()) {
            if (!injected->operation_succeeded && injected->native_error == EINTR) {
                if (failsAfter(action)) {
                    return AclResult::Failure;
                }
                continue;
            }
            result =
                injected->operation_succeeded
                    ? AclResult::Present
                    : aclResultForError(injected->native_error != 0 ? injected->native_error : EIO);
        } else {
            const auto native_result = ::fgetxattr(
                descriptor, default_acl ? "system.posix_acl_default" : "system.posix_acl_access",
                nullptr, 0);
            if (native_result < 0 && errno == EINTR) {
                if (failsAfter(action)) {
                    return AclResult::Failure;
                }
                continue;
            }
            result = native_result >= 0 ? AclResult::Present : aclResultForError(errno);
        }
        return failsAfter(action) ? AclResult::Failure : result;
    }
}

[[nodiscard]] bool publisherSync(int descriptor, IndependentReviewPublisherEvent event,
                                 const QString& absolute_path, const QByteArray& component,
                                 std::size_t ordinal,
                                 const IndependentReviewPublisherHooks& hooks) {
    const auto observation =
        IndependentReviewPublisherObservation{event, absolute_path, component, ordinal};
    while (true) {
        const auto action = publisherEvent(hooks, observation);
        if (failsBefore(action)) {
            return false;
        }
        const auto injected = publisherOutcome(hooks, observation);
        if (injected.has_value() && !injected->operation_succeeded &&
            injected->native_error == EINTR && !failsAfter(action)) {
            continue;
        }
        bool synchronized = false;
        if (injected.has_value()) {
            synchronized = injected->operation_succeeded;
        } else if (::fsync(descriptor) == 0) {
            synchronized = true;
        } else if (errno == EINTR && !failsAfter(action)) {
            continue;
        }
        return synchronized && !failsAfter(action);
    }
}

[[nodiscard]] bool publisherLock(int descriptor, const QString& absolute_path,
                                 const IndependentReviewPublisherHooks& hooks) {
    const auto observation = IndependentReviewPublisherObservation{
        IndependentReviewPublisherEvent::ParentLeaseAttempted, absolute_path, {}, 0};
    while (true) {
        const auto action = publisherEvent(hooks, observation);
        if (failsBefore(action)) {
            return false;
        }
        const auto injected = publisherOutcome(hooks, observation);
        if (injected.has_value() && !injected->operation_succeeded &&
            injected->native_error == EINTR && !failsAfter(action)) {
            continue;
        }
        bool locked = false;
        if (injected.has_value()) {
            locked = injected->operation_succeeded;
        } else if (::flock(descriptor, LOCK_EX | LOCK_NB) == 0) {
            locked = true;
        } else if (errno == EINTR && !failsAfter(action)) {
            continue;
        }
        return locked && !failsAfter(action);
    }
}

[[nodiscard]] PreflightAvailability
publisherModeNormalizationAvailable(const IndependentReviewPublisherHooks& hooks) {
    const auto observation = IndependentReviewPublisherObservation{
        IndependentReviewPublisherEvent::ModeNormalizePreflight, {}, {}, 0};
    while (true) {
        const auto action = publisherEvent(hooks, observation);
        if (failsBefore(action)) {
            return PreflightAvailability::Failure;
        }
        const auto injected = publisherOutcome(hooks, observation);
        int native_error = 0;
        bool available = false;
        if (injected.has_value()) {
            if (!injected->operation_succeeded && injected->native_error == EINTR &&
                !failsAfter(action)) {
                continue;
            }
            available = injected->operation_succeeded;
            native_error = injected->native_error;
        } else {
#if defined(SYS_fchmodat2)
            errno = 0;
            const auto result = ::syscall(SYS_fchmodat2, -1, "", 0700, AT_EMPTY_PATH);
            native_error = result == 0 ? 0 : errno;
            if (native_error == EINTR && !failsAfter(action)) {
                continue;
            }
            available = result == 0 || native_error == EBADF;
#else
            native_error = ENOSYS;
#endif
        }
        if (failsAfter(action)) {
            return PreflightAvailability::Failure;
        }
        if (available) {
            return PreflightAvailability::Available;
        }
        return native_error == ENOSYS || native_error == EINVAL || native_error == EOPNOTSUPP
                   ? PreflightAvailability::Unsupported
                   : PreflightAvailability::Failure;
    }
}

[[nodiscard]] PublisherBindingState
publisherEntryState(int parent_descriptor, const QByteArray& component, int retained_descriptor,
                    const NodeIdentity& retained_identity, bool directory,
                    const QString& absolute_path, std::size_t ordinal,
                    const IndependentReviewPublisherHooks& hooks) {
    const auto observation = IndependentReviewPublisherObservation{
        IndependentReviewPublisherEvent::EntryRebound, absolute_path, component, ordinal};
    while (true) {
        const auto action = publisherEvent(hooks, observation);
        if (failsBefore(action)) {
            return PublisherBindingState::Ambiguous;
        }
        const auto injected = publisherOutcome(hooks, observation);
        auto state = PublisherBindingState::Ambiguous;
        if (injected.has_value()) {
            if (!injected->operation_succeeded && injected->native_error == EINTR &&
                !failsAfter(action)) {
                continue;
            }
            if (injected->operation_succeeded) {
                const auto held =
                    injected->retained_stat.has_value()
                        ? applySyntheticStat(retained_identity, *injected->retained_stat)
                        : retained_identity;
                const auto named =
                    injected->named_stat.has_value()
                        ? applySyntheticStat(retained_identity, *injected->named_stat)
                        : retained_identity;
                const auto exact = directory ? sameDirectoryIdentity(retained_identity, held) &&
                                                   sameDirectoryIdentity(retained_identity, named)
                                             : sameFileIdentity(retained_identity, held) &&
                                                   sameFileIdentity(retained_identity, named);
                state = exact ? PublisherBindingState::Exact : PublisherBindingState::Other;
            } else {
                state = injected->native_error == ENOENT ? PublisherBindingState::Absent
                                                         : PublisherBindingState::Ambiguous;
            }
        } else {
            struct stat held{};
            struct stat named{};
            const auto retained_exact =
                retained_descriptor < 0 ||
                (statDescriptor(retained_descriptor, &held) &&
                 (directory ? sameDirectoryIdentity(retained_identity, identityOf(held))
                            : sameFileIdentity(retained_identity, identityOf(held))));
            if (!retained_exact) {
                state = PublisherBindingState::Other;
            } else if (statNamed(parent_descriptor, component, &named)) {
                const auto named_exact =
                    directory ? sameDirectoryIdentity(retained_identity, identityOf(named))
                              : sameFileIdentity(retained_identity, identityOf(named));
                state = named_exact ? PublisherBindingState::Exact : PublisherBindingState::Other;
            } else if (errno == ENOENT) {
                state = PublisherBindingState::Absent;
            }
        }
        return failsAfter(action) ? PublisherBindingState::Ambiguous : state;
    }
}

[[nodiscard]] bool publisherEntryRebind(int parent_descriptor, const QByteArray& component,
                                        int retained_descriptor,
                                        const NodeIdentity& retained_identity, bool directory,
                                        const QString& absolute_path, std::size_t ordinal,
                                        const IndependentReviewPublisherHooks& hooks) {
    return publisherEntryState(parent_descriptor, component, retained_descriptor, retained_identity,
                               directory, absolute_path, ordinal,
                               hooks) == PublisherBindingState::Exact;
}

[[nodiscard]] AclResult probeBoundPublisherAcl(int parent_descriptor, const QByteArray& component,
                                               int retained_descriptor,
                                               const NodeIdentity& retained_identity,
                                               bool directory, bool default_acl,
                                               const QString& absolute_path, std::size_t ordinal,
                                               const IndependentReviewPublisherHooks& hooks) {
    if (!publisherEntryRebind(parent_descriptor, component, retained_descriptor, retained_identity,
                              directory, absolute_path, ordinal, hooks)) {
        return AclResult::Failure;
    }
    const auto result = probePublisherAcl(retained_descriptor, default_acl, absolute_path,
                                          component, ordinal, hooks);
    return publisherEntryRebind(parent_descriptor, component, retained_descriptor,
                                retained_identity, directory, absolute_path, ordinal, hooks)
               ? result
               : AclResult::Failure;
}

template <typename ReproveAuthority>
[[nodiscard]] bool publisherRemove(int parent_descriptor, const QByteArray& component,
                                   int retained_descriptor, const NodeIdentity& retained_identity,
                                   bool directory, const QString& absolute_path,
                                   std::size_t ordinal, ReproveAuthority&& reprove_authority,
                                   const IndependentReviewPublisherHooks& hooks) {
    const auto observation = IndependentReviewPublisherObservation{
        IndependentReviewPublisherEvent::CleanupRemoved, absolute_path, component, ordinal};
    while (true) {
        const auto action = publisherEvent(hooks, observation);
        if (failsBefore(action)) {
            return false;
        }
        const auto injected = publisherOutcome(hooks, observation);
        if (injected.has_value() && !injected->operation_succeeded &&
            !injected->state_change_applied && injected->native_error == EINTR &&
            !failsAfter(action)) {
            continue;
        }
        if (injected.has_value() && !injected->operation_succeeded &&
            !injected->state_change_applied) {
            return false;
        }
        // The event and injected outcome are deliberate race seams.  Re-prove the complete cleanup
        // authority after both, then bind the leaf immediately before unlinkat.  A retained parent
        // descriptor alone is not authority to delete from a staging root or ancestor that lost its
        // visible name binding.
        if (!std::forward<ReproveAuthority>(reprove_authority)() ||
            !publisherEntryRebind(parent_descriptor, component, retained_descriptor,
                                  retained_identity, directory, absolute_path, ordinal, hooks)) {
            return false;
        }
        const auto result =
            ::unlinkat(parent_descriptor, component.constData(), directory ? AT_REMOVEDIR : 0);
        if (result != 0 && errno == EINTR && !failsAfter(action)) {
            continue;
        }
        const auto applied = result == 0;
        const auto removed = applied && (!injected.has_value() || injected->operation_succeeded);
        return removed && !failsAfter(action);
    }
}

struct PublisherMutationResult final {
    bool operation_succeeded{};
    bool state_change_applied{};
    int native_error{};
};

struct PublisherOpenResult final {
    Descriptor descriptor;
    PublisherMutationResult mutation;
};

[[nodiscard]] Descriptor publisherOpenDirectory(int parent_descriptor, const QByteArray& component,
                                                int flags, IndependentReviewPublisherEvent event,
                                                const QString& absolute_path, std::size_t ordinal,
                                                const IndependentReviewPublisherHooks& hooks) {
    const auto observation =
        IndependentReviewPublisherObservation{event, absolute_path, component, ordinal};
    while (true) {
        const auto action = publisherEvent(hooks, observation);
        if (failsBefore(action)) {
            return Descriptor{};
        }
        const auto injected = publisherOutcome(hooks, observation);
        if (injected.has_value() && !injected->operation_succeeded) {
            if (injected->native_error == EINTR && !failsAfter(action)) {
                continue;
            }
            return Descriptor{};
        }
        Descriptor descriptor(::openat(parent_descriptor, component.constData(), flags));
        if (!descriptor && errno == EINTR && !failsAfter(action)) {
            continue;
        }
        if (failsAfter(action)) {
            return Descriptor{};
        }
        return descriptor;
    }
}

[[nodiscard]] PublisherMutationResult
publisherMkdir(int parent_descriptor, const QByteArray& component, mode_t mode,
               IndependentReviewPublisherEvent event, const QString& absolute_path,
               std::size_t ordinal, const IndependentReviewPublisherHooks& hooks) {
    const auto observation =
        IndependentReviewPublisherObservation{event, absolute_path, component, ordinal};
    while (true) {
        const auto action = publisherEvent(hooks, observation);
        if (failsBefore(action)) {
            return {false, false, EIO};
        }
        const auto injected = publisherOutcome(hooks, observation);
        const auto apply = failsAfter(action) || !injected.has_value() ||
                           injected->operation_succeeded || injected->state_change_applied;
        if (!apply) {
            if (injected->native_error == EINTR) {
                continue;
            }
            return {false, false, injected->native_error != 0 ? injected->native_error : EIO};
        }
        if (::mkdirat(parent_descriptor, component.constData(), mode) != 0) {
            const auto native_error = errno;
            if (native_error == EINTR && !failsAfter(action)) {
                continue;
            }
            return {false, false, native_error};
        }
        const auto succeeded =
            !failsAfter(action) && (!injected.has_value() || injected->operation_succeeded);
        return {succeeded, true,
                succeeded                                             ? 0
                : injected.has_value() && injected->native_error != 0 ? injected->native_error
                                                                      : EIO};
    }
}

[[nodiscard]] PublisherOpenResult
publisherCreateFile(int parent_descriptor, const QByteArray& component, mode_t mode,
                    const QString& absolute_path, std::size_t ordinal,
                    const IndependentReviewPublisherHooks& hooks) {
    const auto observation = IndependentReviewPublisherObservation{
        IndependentReviewPublisherEvent::FileCreateAttempted, absolute_path, component, ordinal};
    while (true) {
        const auto action = publisherEvent(hooks, observation);
        if (failsBefore(action)) {
            return {Descriptor{}, {false, false, EIO}};
        }
        const auto injected = publisherOutcome(hooks, observation);
        const auto apply = failsAfter(action) || !injected.has_value() ||
                           injected->operation_succeeded || injected->state_change_applied;
        if (!apply) {
            if (injected->native_error == EINTR) {
                continue;
            }
            return {Descriptor{},
                    {false, false, injected->native_error != 0 ? injected->native_error : EIO}};
        }
        Descriptor descriptor(::openat(parent_descriptor, component.constData(),
                                       O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode));
        if (!descriptor) {
            const auto native_error = errno;
            if (native_error == EINTR && !failsAfter(action)) {
                continue;
            }
            return {Descriptor{}, {false, false, native_error}};
        }
        const auto succeeded =
            !failsAfter(action) && (!injected.has_value() || injected->operation_succeeded);
        return {std::move(descriptor),
                {succeeded, true,
                 succeeded                                             ? 0
                 : injected.has_value() && injected->native_error != 0 ? injected->native_error
                                                                       : EIO}};
    }
}

[[nodiscard]] PublisherMutationResult publisherChmod(int descriptor, mode_t mode,
                                                     const QString& absolute_path,
                                                     const QByteArray& component,
                                                     std::size_t ordinal,
                                                     const IndependentReviewPublisherHooks& hooks) {
    const auto observation = IndependentReviewPublisherObservation{
        IndependentReviewPublisherEvent::ModeNormalizeAttempted, absolute_path, component, ordinal};
    while (true) {
        const auto action = publisherEvent(hooks, observation);
        if (failsBefore(action)) {
            return {false, false, EIO};
        }
        const auto injected = publisherOutcome(hooks, observation);
        const auto apply = failsAfter(action) || !injected.has_value() ||
                           injected->operation_succeeded || injected->state_change_applied;
        if (!apply) {
            if (injected->native_error == EINTR) {
                continue;
            }
            return {false, false, injected->native_error != 0 ? injected->native_error : EIO};
        }
#if defined(SYS_fchmodat2)
        const auto chmod_result = ::syscall(SYS_fchmodat2, descriptor, "", mode, AT_EMPTY_PATH);
#else
        errno = ENOSYS;
        const auto chmod_result = -1L;
#endif
        if (chmod_result != 0) {
            const auto native_error = errno;
            if (native_error == EINTR && !failsAfter(action)) {
                continue;
            }
            return {false, false, native_error};
        }
        const auto succeeded =
            !failsAfter(action) && (!injected.has_value() || injected->operation_succeeded);
        return {succeeded, true,
                succeeded                                             ? 0
                : injected.has_value() && injected->native_error != 0 ? injected->native_error
                                                                      : EIO};
    }
}

[[nodiscard]] PublisherMutationResult
publisherWriteAll(int descriptor, QByteArrayView bytes, const QString& absolute_path,
                  const QByteArray& component, std::size_t ordinal,
                  const IndependentReviewPublisherHooks& hooks) {
    qsizetype offset = 0;
    const auto observation = IndependentReviewPublisherObservation{
        IndependentReviewPublisherEvent::FileWrite, absolute_path, component, ordinal};
    while (offset < bytes.size()) {
        const auto action = publisherEvent(hooks, observation);
        if (failsBefore(action)) {
            return {false, offset > 0, EIO};
        }
        const auto injected = publisherOutcome(hooks, observation);
        const auto apply = failsAfter(action) || !injected.has_value() ||
                           injected->operation_succeeded || injected->state_change_applied;
        if (!apply) {
            if (injected->native_error == EINTR) {
                continue;
            }
            return {false, offset > 0, injected->native_error != 0 ? injected->native_error : EIO};
        }
        auto requested_bytes = static_cast<std::size_t>(bytes.size() - offset);
        if (injected.has_value() && injected->maximum_write_bytes.has_value()) {
            requested_bytes = std::min(requested_bytes, *injected->maximum_write_bytes);
        }
        const auto count = ::write(descriptor, bytes.data() + offset, requested_bytes);
        if (count < 0 && errno == EINTR && !failsAfter(action)) {
            continue;
        }
        if (count <= 0) {
            return {false, offset > 0, count < 0 ? errno : EIO};
        }
        offset += static_cast<qsizetype>(count);
        if (failsAfter(action) || (injected.has_value() && !injected->operation_succeeded)) {
            return {false, true,
                    injected.has_value() && injected->native_error != 0 ? injected->native_error
                                                                        : EIO};
        }
    }
    return {true, !bytes.isEmpty(), 0};
}

[[nodiscard]] bool controllerPolicy(const NodeIdentity& identity) {
    const auto effective_uid = ::geteuid();
    if (identity.type != S_IFDIR || (identity.owner != effective_uid && identity.owner != 0)) {
        return false;
    }
    const auto writable_by_others = (identity.mode & 0022) != 0;
    return !writable_by_others || (identity.mode & S_ISVTX) != 0;
}

struct ResolvedOperand final {
    QString absolute_path;
    QStringList components;
    std::vector<QByteArray> native_components;
    QByteArray native_absolute_path;
    QStringList supplied_components;
    std::vector<QByteArray> supplied_native_components;
    Descriptor retained_cwd;
    NodeIdentity cwd_identity;
    std::size_t cwd_component_count{};
    bool relative{};
};

[[nodiscard]] std::optional<ValidatedPathComponents>
absoluteComponents(const QString& path, std::size_t maximum_components) {
    if (!path.startsWith(u'/') || !isValidNativeSpelling(path, maximum_native_path_bytes)) {
        return std::nullopt;
    }
    if (path == QStringLiteral("/")) {
        return ValidatedPathComponents{QStringList{}, {}, QByteArray{"/"}};
    }
    const auto components = path.sliced(1).split(u'/', Qt::KeepEmptyParts);
    if (components.isEmpty() || static_cast<std::size_t>(components.size()) > maximum_components ||
        std::ranges::any_of(components, [](const QString& component) {
            return component.isEmpty() || component == QStringLiteral(".") ||
                   component == QStringLiteral("..") ||
                   !isValidNativeSpelling(component, maximum_native_component_bytes);
        })) {
        return std::nullopt;
    }
    std::vector<QByteArray> native_components;
    native_components.reserve(static_cast<std::size_t>(components.size()));
    for (const auto& component : components) {
        native_components.push_back(QFile::encodeName(component));
    }
    return ValidatedPathComponents{components, std::move(native_components),
                                   QFile::encodeName(path)};
}

[[nodiscard]] auto resolveOperand(IndependentReviewPathToken validated,
                                  std::size_t maximum_components,
                                  const IndependentReviewPublisherHooks* hooks = nullptr)
    -> std::expected<ResolvedOperand, IndependentReviewPublicationError> {
    if (!isConsistentPathToken(validated)) {
        return publicationFailure(IndependentReviewPublicationErrorCode::InvalidArguments,
                                  QStringLiteral("Path operand spelling is invalid"));
    }
    const auto supplied = validated.supplied_path;
    const auto relative = !supplied.startsWith(u'/');
    QString absolute = supplied;
    Descriptor retained_cwd;
    NodeIdentity cwd_identity;
    std::size_t cwd_component_count = 0;
    if (relative) {
        retained_cwd.reset(::open(".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        struct stat cwd_status{};
        if (!retained_cwd || ::fstat(retained_cwd.get(), &cwd_status) != 0) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform,
                QStringLiteral("Cannot retain the current directory"));
        }
        const auto captured_cwd = QDir::currentPath();
        auto cwd_components = absoluteComponents(captured_cwd, maximum_native_components);
        if (!cwd_components) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform,
                QStringLiteral("Current directory exceeds native publication limits"));
        }
        cwd_identity = identityOf(cwd_status);
        cwd_component_count = static_cast<std::size_t>(cwd_components->text.size());
        absolute = captured_cwd == QStringLiteral("/") ? QStringLiteral("/") + supplied
                                                       : captured_cwd + u'/' + supplied;
        if (hooks != nullptr) {
            const auto observation = IndependentReviewPublisherObservation{
                IndependentReviewPublisherEvent::CurrentDirectoryCaptured,
                captured_cwd,
                {},
                cwd_component_count};
            const auto action = publisherEvent(*hooks, observation);
            const auto outcome = publisherOutcome(*hooks, observation);
            if (action != IndependentReviewPublisherInjectedAction::Continue ||
                (outcome.has_value() && !outcome->operation_succeeded)) {
                return publicationFailure(
                    IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform,
                    nativeError(QStringLiteral("Cannot capture the current directory"),
                                outcome.has_value() && outcome->native_error != 0
                                    ? outcome->native_error
                                    : EIO));
            }
        }
        const auto expanded_count =
            cwd_component_count + static_cast<std::size_t>(validated.supplied_components.size());
        const auto native_absolute = (cwd_components->native_path == QByteArray{"/"}
                                          ? cwd_components->native_path
                                          : cwd_components->native_path + QByteArray{"/"}) +
                                     validated.supplied_native_path;
        if (expanded_count > maximum_components ||
            native_absolute.size() > maximum_native_path_bytes) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform,
                QStringLiteral("Absolute path exceeds native publication limits"));
        }
        auto absolute_text = std::move(cwd_components->text);
        absolute_text.append(validated.supplied_components);
        auto absolute_native = std::move(cwd_components->native);
        absolute_native.insert(absolute_native.end(), validated.supplied_native_components.begin(),
                               validated.supplied_native_components.end());
        return ResolvedOperand{absolute,
                               std::move(absolute_text),
                               std::move(absolute_native),
                               native_absolute,
                               validated.supplied_components,
                               std::move(validated.supplied_native_components),
                               std::move(retained_cwd),
                               cwd_identity,
                               cwd_component_count,
                               true};
    } else {
        if (static_cast<std::size_t>(validated.supplied_components.size()) > maximum_components) {
            return publicationFailure(IndependentReviewPublicationErrorCode::InvalidArguments,
                                      QStringLiteral("Absolute path exceeds native limits"));
        }
        return ResolvedOperand{supplied,
                               validated.supplied_components,
                               std::move(validated.supplied_native_components),
                               std::move(validated.supplied_native_path),
                               validated.supplied_components,
                               {},
                               Descriptor{},
                               {},
                               0,
                               false};
    }
}

[[nodiscard]] auto resolveOperand(const QString& supplied, std::size_t maximum_components,
                                  const IndependentReviewPublisherHooks* hooks = nullptr)
    -> std::expected<ResolvedOperand, IndependentReviewPublicationError> {
    auto token = encodedPathToken(supplied, false);
    if (!token) {
        return publicationFailure(IndependentReviewPublicationErrorCode::InvalidArguments,
                                  QStringLiteral("Path operand spelling is invalid"));
    }
    return resolveOperand(std::move(*token), maximum_components, hooks);
}

struct RetainedController final {
    QString absolute_path;
    QByteArray component;
    Descriptor descriptor;
    NodeIdentity identity;
};

struct RetainedChain final {
    QString absolute_path;
    QByteArray native_absolute_path;
    std::vector<RetainedController> controllers;
};

[[nodiscard]] auto childComponentForIdentity(int parent_descriptor,
                                             const NodeIdentity& child_identity)
    -> std::expected<QByteArray, IndependentReviewPublicationError> {
    Descriptor stream_descriptor(
        ::openat(parent_descriptor, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!stream_descriptor) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform,
            QStringLiteral("Cannot enumerate a retained current-directory parent"));
    }
    DIR* raw = ::fdopendir(stream_descriptor.get());
    if (raw == nullptr) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform,
            QStringLiteral("Cannot bind a retained current-directory parent"));
    }
    static_cast<void>(stream_descriptor.release());
    struct DirectoryCloser final {
        void operator()(DIR* value) const noexcept { static_cast<void>(::closedir(value)); }
    };
    std::unique_ptr<DIR, DirectoryCloser> directory(raw);
    std::optional<QByteArray> match;
    errno = 0;
    while (const auto* entry = ::readdir(directory.get())) {
        const QByteArray name(entry->d_name);
        if (name == "." || name == "..") {
            errno = 0;
            continue;
        }
        struct stat named{};
        if (::fstatat(parent_descriptor, name.constData(), &named, AT_SYMLINK_NOFOLLOW) != 0) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform,
                QStringLiteral("Current-directory ancestry changed during ascent"));
        }
        if (named.st_dev == child_identity.device && named.st_ino == child_identity.inode &&
            S_ISDIR(named.st_mode)) {
            if (match.has_value()) {
                return publicationFailure(
                    IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform,
                    QStringLiteral("Current-directory ancestry is not uniquely name-bound"));
            }
            match = name;
        }
        errno = 0;
    }
    if (errno != 0 || !match.has_value() || match->isEmpty() ||
        match->size() > maximum_native_component_bytes || match->contains('\0') ||
        QFile::encodeName(QFile::decodeName(*match)) != *match) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform,
            QStringLiteral("Current-directory ancestry has no reversible retained name"));
    }
    struct stat rebound{};
    if (::fstatat(parent_descriptor, match->constData(), &rebound, AT_SYMLINK_NOFOLLOW) != 0 ||
        rebound.st_dev != child_identity.device || rebound.st_ino != child_identity.inode ||
        !S_ISDIR(rebound.st_mode)) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform,
            QStringLiteral("Current-directory ancestry changed after ascent"));
    }
    return *match;
}

[[nodiscard]] auto openRetainedChain(ResolvedOperand operand, std::size_t component_count)
    -> std::expected<RetainedChain, IndependentReviewPublicationError> {
    if (component_count > static_cast<std::size_t>(operand.components.size())) {
        return publicationFailure(IndependentReviewPublicationErrorCode::CannotPublishDestination,
                                  QStringLiteral("Path component count is inconsistent"));
    }
    Descriptor slash_root(::open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat slash_root_status{};
    if (!slash_root || ::fstat(slash_root.get(), &slash_root_status) != 0 ||
        !S_ISDIR(slash_root_status.st_mode)) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform,
            QStringLiteral("Cannot retain the filesystem root"));
    }
    RetainedChain chain;
    chain.absolute_path = QStringLiteral("/");
    chain.native_absolute_path = QByteArray{"/"};
    chain.controllers.reserve(component_count + 1);
    QString current_path;
    QByteArray current_native_path;

    QStringList descend_components = operand.components;
    std::vector<QByteArray> descend_native_components = operand.native_components;
    if (operand.relative) {
        if (component_count < operand.cwd_component_count || !operand.retained_cwd ||
            component_count - operand.cwd_component_count >
                static_cast<std::size_t>(operand.supplied_components.size())) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform,
                QStringLiteral("Relative path component count is inconsistent"));
        }
        struct stat current_status{};
        if (::fstat(operand.retained_cwd.get(), &current_status) != 0 ||
            !sameDirectoryIdentity(operand.cwd_identity, identityOf(current_status))) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform,
                QStringLiteral("Captured current directory changed before ascent"));
        }
        Descriptor current = std::move(operand.retained_cwd);
        NodeIdentity current_identity = identityOf(current_status);
        const auto root_identity = identityOf(slash_root_status);
        std::vector<RetainedController> reverse_children;
        reverse_children.reserve(maximum_native_components);
        while (current_identity.device != root_identity.device ||
               current_identity.inode != root_identity.inode) {
            if (reverse_children.size() >= maximum_native_components) {
                return publicationFailure(
                    IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform,
                    QStringLiteral("Current-directory ancestry exceeds 128 components"));
            }
            Descriptor parent(
                ::openat(current.get(), "..", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            struct stat parent_status{};
            if (!parent || ::fstat(parent.get(), &parent_status) != 0 ||
                !S_ISDIR(parent_status.st_mode)) {
                return publicationFailure(
                    IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform,
                    QStringLiteral("Cannot ascend from the retained current directory"));
            }
            auto component = childComponentForIdentity(parent.get(), current_identity);
            if (!component) {
                return std::unexpected(component.error());
            }
            reverse_children.push_back(
                RetainedController{{}, *component, std::move(current), current_identity});
            current = std::move(parent);
            current_identity = identityOf(parent_status);
        }
        chain.controllers.push_back(
            RetainedController{QStringLiteral("/"), {}, std::move(current), current_identity});
        for (auto iterator = reverse_children.rbegin(); iterator != reverse_children.rend();
             ++iterator) {
            current_path += u'/' + QFile::decodeName(iterator->component);
            current_native_path += QByteArray{"/"} + iterator->component;
            if (current_native_path.size() > maximum_native_path_bytes) {
                return publicationFailure(
                    IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform,
                    QStringLiteral("Current-directory ancestry exceeds native path limits"));
            }
            iterator->absolute_path = current_path;
            chain.controllers.push_back(std::move(*iterator));
        }
        const auto relative_count = component_count - operand.cwd_component_count;
        descend_components =
            operand.supplied_components.mid(0, static_cast<qsizetype>(relative_count));
        descend_native_components.assign(operand.supplied_native_components.begin(),
                                         operand.supplied_native_components.begin() +
                                             static_cast<std::ptrdiff_t>(relative_count));
        if (chain.controllers.size() - 1U + relative_count > maximum_native_components) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform,
                QStringLiteral("Expanded relative path exceeds 128 components"));
        }
    } else {
        chain.controllers.push_back(RetainedController{
            QStringLiteral("/"), {}, std::move(slash_root), identityOf(slash_root_status)});
        descend_components = operand.components.mid(0, static_cast<qsizetype>(component_count));
        descend_native_components.assign(operand.native_components.begin(),
                                         operand.native_components.begin() +
                                             static_cast<std::ptrdiff_t>(component_count));
    }

    for (std::size_t index = 0; index < static_cast<std::size_t>(descend_components.size());
         ++index) {
        const auto component = descend_components.at(static_cast<qsizetype>(index));
        const auto& encoded = descend_native_components.at(index);
        auto& parent = chain.controllers.back();
        Descriptor child(::openat(parent.descriptor.get(), encoded.constData(),
                                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        struct stat held{};
        struct stat named{};
        if (!child || ::fstat(child.get(), &held) != 0 ||
            ::fstatat(parent.descriptor.get(), encoded.constData(), &named, AT_SYMLINK_NOFOLLOW) !=
                0 ||
            !S_ISDIR(held.st_mode) || held.st_dev != named.st_dev || held.st_ino != named.st_ino) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::CannotPublishDestination,
                nativeError(QStringLiteral("Cannot retain a no-follow path controller")));
        }
        const auto identity = identityOf(held);
        current_path += u'/' + component;
        current_native_path += QByteArray{"/"} + encoded;
        if (current_native_path.size() > maximum_native_path_bytes) {
            return publicationFailure(
                operand.relative
                    ? IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform
                    : IndependentReviewPublicationErrorCode::InvalidArguments,
                QStringLiteral("Expanded retained path exceeds native limits"));
        }
        chain.controllers.push_back(
            RetainedController{current_path, encoded, std::move(child), identity});
    }
    chain.absolute_path = current_path.isEmpty() ? QStringLiteral("/") : current_path;
    chain.native_absolute_path =
        current_native_path.isEmpty() ? QByteArray{"/"} : current_native_path;
    return chain;
}

[[nodiscard]] bool rebindControllerChain(const RetainedChain& chain,
                                         const IndependentReviewPublisherHooks* hooks = nullptr) {
    for (std::size_t index = 0; index < chain.controllers.size(); ++index) {
        const auto& controller = chain.controllers.at(index);
        const auto observation = IndependentReviewPublisherObservation{
            IndependentReviewPublisherEvent::ControllerRebound, controller.absolute_path,
            controller.component, index};
        while (true) {
            const auto action = hooks != nullptr
                                    ? publisherEvent(*hooks, observation)
                                    : IndependentReviewPublisherInjectedAction::Continue;
            if (failsBefore(action)) {
                return false;
            }
            const auto injected =
                hooks != nullptr ? publisherOutcome(*hooks, observation) : std::nullopt;
            if (injected.has_value()) {
                if (!injected->operation_succeeded && injected->native_error == EINTR &&
                    !failsAfter(action)) {
                    continue;
                }
                const auto held =
                    injected->retained_stat.has_value()
                        ? applySyntheticStat(controller.identity, *injected->retained_stat)
                        : controller.identity;
                const auto named =
                    injected->named_stat.has_value()
                        ? applySyntheticStat(controller.identity, *injected->named_stat)
                        : controller.identity;
                const auto exact =
                    sameDirectoryIdentity(controller.identity, held) &&
                    (index == 0 || sameDirectoryIdentity(controller.identity, named));
                if (!injected->operation_succeeded || !exact || failsAfter(action)) {
                    return false;
                }
                break;
            }
            struct stat held{};
            if (!statDescriptor(controller.descriptor.get(), &held) ||
                !sameDirectoryIdentity(controller.identity, identityOf(held))) {
                return false;
            }
            if (index > 0) {
                struct stat named{};
                const auto& parent = chain.controllers.at(index - 1U);
                if (!statNamed(parent.descriptor.get(), controller.component, &named) ||
                    !sameDirectoryIdentity(controller.identity, identityOf(named))) {
                    return false;
                }
            }
            if (failsAfter(action)) {
                return false;
            }
            break;
        }
    }
    return true;
}

[[maybe_unused, nodiscard]] auto
validateControllerChain(const RetainedChain& chain, const IndependentReviewPublisherHooks& hooks)
    -> std::expected<void, IndependentReviewPublicationError> {
    if (!rebindControllerChain(chain, &hooks)) {
        return publicationFailure(IndependentReviewPublicationErrorCode::UnsafeDestinationParent,
                                  QStringLiteral("Destination controller binding changed"));
    }
    for (std::size_t index = 0; index < chain.controllers.size(); ++index) {
        const auto& controller = chain.controllers.at(index);
        if (!controllerPolicy(controller.identity)) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::UnsafeDestinationParent,
                QStringLiteral("Destination controller owner or mode is unsafe"));
        }
        if (index > 0) {
            const auto& parent = chain.controllers.at(index - 1U);
            if ((parent.identity.mode & S_ISVTX) != 0 && controller.identity.owner != ::geteuid() &&
                controller.identity.owner != 0) {
                return publicationFailure(
                    IndependentReviewPublicationErrorCode::UnsafeDestinationParent,
                    QStringLiteral("A sticky controller has an untrusted child owner"));
            }
        }
        if (!rebindControllerChain(chain, &hooks)) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::UnsafeDestinationParent,
                QStringLiteral("Destination controller binding changed before ACL inspection"));
        }
        const auto access =
            probePublisherAcl(controller.descriptor.get(), false, controller.absolute_path,
                              controller.component, index, hooks);
        if (!rebindControllerChain(chain, &hooks)) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::UnsafeDestinationParent,
                QStringLiteral("Destination controller binding changed after ACL inspection"));
        }
        const auto default_acl =
            probePublisherAcl(controller.descriptor.get(), true, controller.absolute_path,
                              controller.component, index, hooks);
        if (!rebindControllerChain(chain, &hooks)) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::UnsafeDestinationParent,
                QStringLiteral("Destination controller binding changed after ACL inspection"));
        }
        if (access == AclResult::Unsupported || default_acl == AclResult::Unsupported) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform,
                QStringLiteral("Destination controller ACL inspection is unsupported"));
        }
        if (access == AclResult::Present || default_acl == AclResult::Present) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::UnsafeDestinationParent,
                QStringLiteral("Destination controller has a POSIX ACL"));
        }
        if (access == AclResult::Failure || default_acl == AclResult::Failure) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::CannotPublishDestination,
                QStringLiteral("Cannot inspect destination controller ACLs"));
        }
    }
    return {};
}

[[nodiscard]] auto readStableDescriptor(int descriptor, qsizetype maximum_bytes,
                                        NodeIdentity* stable_identity = nullptr)
    -> std::expected<QByteArray, IndependentReviewPublicationError> {
    struct stat before_status{};
    if (::fstat(descriptor, &before_status) != 0) {
        return publicationFailure(IndependentReviewPublicationErrorCode::InvalidStagedArtifact,
                                  QStringLiteral("Cannot inspect input file"));
    }
    const auto before = identityOf(before_status);
    if (before.type != S_IFREG || before.link_count != 1 || before.byte_size < 0 ||
        before.byte_size > maximum_bytes) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::InvalidStagedArtifact,
            QStringLiteral("Input is not a bounded single-link regular file"));
    }
    if (::lseek(descriptor, 0, SEEK_SET) < 0) {
        return publicationFailure(IndependentReviewPublicationErrorCode::InvalidStagedArtifact,
                                  QStringLiteral("Cannot seek input file"));
    }
    QByteArray bytes(static_cast<qsizetype>(before.byte_size), Qt::Uninitialized);
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::read(descriptor, bytes.data() + offset,
                                  static_cast<std::size_t>(bytes.size() - offset));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return publicationFailure(IndependentReviewPublicationErrorCode::InvalidStagedArtifact,
                                      QStringLiteral("Input changed or ended during read"));
        }
        offset += static_cast<qsizetype>(count);
    }
    char extra{};
    ssize_t extra_count = -1;
    do {
        extra_count = ::read(descriptor, &extra, 1);
    } while (extra_count < 0 && errno == EINTR);
    struct stat after_status{};
    if (extra_count != 0 || ::fstat(descriptor, &after_status) != 0) {
        return publicationFailure(IndependentReviewPublicationErrorCode::InvalidStagedArtifact,
                                  QStringLiteral("Input changed during stable read"));
    }
    const auto after = identityOf(after_status);
    if (!sameStableFileSnapshot(before, after)) {
        return publicationFailure(IndependentReviewPublicationErrorCode::InvalidStagedArtifact,
                                  QStringLiteral("Input changed during stable read"));
    }
    if (stable_identity != nullptr) {
        *stable_identity = after;
    }
    return bytes;
}

[[nodiscard]] bool rebindStableFile(int parent_descriptor, const QByteArray& name,
                                    int file_descriptor, const NodeIdentity& stable_identity) {
    struct stat held_status{};
    struct stat named_status{};
    return ::fstat(file_descriptor, &held_status) == 0 &&
           ::fstatat(parent_descriptor, name.constData(), &named_status, AT_SYMLINK_NOFOLLOW) ==
               0 &&
           sameStableFileSnapshot(stable_identity, identityOf(held_status)) &&
           sameStableFileSnapshot(stable_identity, identityOf(named_status));
}

[[nodiscard]] auto directoryNames(int descriptor, std::size_t maximum_names,
                                  IndependentReviewPublicationErrorCode failure_code,
                                  QStringView limit_message)
    -> std::expected<std::set<QByteArray>, IndependentReviewPublicationError> {
    // A dup shares the directory stream offset with the retained descriptor.  Open `.` instead so
    // every inventory pass has an independent open-file description and necessarily starts at the
    // beginning of the retained directory.
    Descriptor duplicate(
        ::openat(descriptor, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!duplicate) {
        return publicationFailure(failure_code,
                                  QStringLiteral("Cannot duplicate directory for inventory"));
    }
    DIR* raw = ::fdopendir(duplicate.get());
    if (raw == nullptr) {
        return publicationFailure(failure_code, QStringLiteral("Cannot enumerate directory"));
    }
    static_cast<void>(duplicate.release());
    struct DirectoryCloser final {
        void operator()(DIR* value) const noexcept { static_cast<void>(::closedir(value)); }
    };
    std::unique_ptr<DIR, DirectoryCloser> directory(raw);
    std::set<QByteArray> names;
    errno = 0;
    while (const auto* entry = ::readdir(directory.get())) {
        const QByteArray name(entry->d_name);
        if (name == "." || name == "..") {
            continue;
        }
        if (names.size() >= maximum_names) {
            return publicationFailure(failure_code, limit_message.toString());
        }
        names.insert(name);
        errno = 0;
    }
    if (errno != 0) {
        return publicationFailure(failure_code, QStringLiteral("Directory enumeration failed"));
    }
    return names;
}

[[nodiscard]] auto validateHandoffOperandFeasibility(const ResolvedOperand& operand)
    -> std::expected<void, IndependentReviewPublicationError> {
    constexpr std::array<std::string_view, 2> members{"handoff.json",
                                                      "review-declaration.template.json"};
    const auto failure_code =
        operand.relative ? IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform
                         : IndependentReviewPublicationErrorCode::InvalidArguments;
    if (static_cast<std::size_t>(operand.components.size()) >= maximum_native_components) {
        return publicationFailure(
            failure_code, QStringLiteral("Handoff member expansion exceeds 128 components"));
    }
    for (const auto member : members) {
        if (operand.native_absolute_path.size() + 1 + static_cast<qsizetype>(member.size()) >
            maximum_native_path_bytes) {
            return publicationFailure(
                failure_code,
                QStringLiteral("Handoff member expansion exceeds native path limits"));
        }
    }
    return {};
}

struct ClosedHandoffBytes final {
    QByteArray handoff;
    QByteArray declaration_template;
};

template <typename RebindRoot>
[[nodiscard]] auto
readClosedHandoffRoot(int root_descriptor, const NodeIdentity& initial_root_identity,
                      RebindRoot&& rebind_root, const IndependentReviewInputReaderHooks& hooks,
                      QStringView description)
    -> std::expected<ClosedHandoffBytes, IndependentReviewPublicationError> {
    const std::set<QByteArray> expected{"handoff.json", "review-declaration.template.json"};
    const auto names =
        directoryNames(root_descriptor, expected.size(),
                       IndependentReviewPublicationErrorCode::InvalidStagedArtifact,
                       QStringLiteral("Closed handoff inventory exceeds two entries"));
    if (!names || *names != expected) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::InvalidStagedArtifact,
            QStringLiteral("%1 must contain exactly its two declared files").arg(description));
    }

    constexpr auto handoff_name = "handoff.json";
    constexpr auto template_name = "review-declaration.template.json";
    Descriptor handoff_file(
        ::openat(root_descriptor, handoff_name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    Descriptor template_file(
        ::openat(root_descriptor, template_name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    if (!handoff_file || !template_file) {
        return publicationFailure(IndependentReviewPublicationErrorCode::InvalidStagedArtifact,
                                  QStringLiteral("Cannot open a %1 member").arg(description));
    }

    NodeIdentity handoff_identity;
    NodeIdentity template_identity;
    auto handoff =
        readStableDescriptor(handoff_file.get(), maximum_handoff_bytes, &handoff_identity);
    auto declaration_template =
        readStableDescriptor(template_file.get(), maximum_template_bytes, &template_identity);
    if (!handoff || !declaration_template) {
        return publicationFailure(IndependentReviewPublicationErrorCode::InvalidStagedArtifact,
                                  QStringLiteral("%1 member changed during read").arg(description));
    }
    if (hooks.before_final_rebind) {
        hooks.before_final_rebind();
    }

    struct stat final_root_status{};
    const auto final_names =
        directoryNames(root_descriptor, expected.size(),
                       IndependentReviewPublicationErrorCode::InvalidStagedArtifact,
                       QStringLiteral("Closed handoff inventory exceeds two entries"));
    if (!final_names || *final_names != expected ||
        ::fstat(root_descriptor, &final_root_status) != 0 ||
        !sameDirectoryIdentity(initial_root_identity, identityOf(final_root_status)) ||
        !rebindStableFile(root_descriptor, QByteArray{handoff_name}, handoff_file.get(),
                          handoff_identity) ||
        !rebindStableFile(root_descriptor, QByteArray{template_name}, template_file.get(),
                          template_identity) ||
        !std::forward<RebindRoot>(rebind_root)()) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::InvalidStagedArtifact,
            QStringLiteral("%1 changed during closed scanning").arg(description));
    }
    return ClosedHandoffBytes{std::move(*handoff), std::move(*declaration_template)};
}

#endif

} // namespace

std::expected<IndependentReviewPathToken, IndependentReviewPublicationError>
encodeIndependentReviewPathSpelling(const QString& path, bool destination_leaf) {
    auto token = encodedPathToken(path, destination_leaf);
    if (!token) {
        return publicationFailure(IndependentReviewPublicationErrorCode::InvalidArguments,
                                  QStringLiteral("Path operand violates native spelling bounds"));
    }
    return std::move(*token);
}

std::expected<void, IndependentReviewPublicationError>
validateIndependentReviewPathSpelling(const QString& path, bool destination_leaf) {
    auto encoded = encodeIndependentReviewPathSpelling(path, destination_leaf);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return {};
}

std::expected<IndependentReviewPathToken, IndependentReviewPublicationError>
encodeIndependentReviewDestinationPath(const QString& path, IndependentReviewArtifactKind kind) {
    auto token = encodeIndependentReviewPathSpelling(path, true);
    if (!token) {
        return std::unexpected(token.error());
    }
    const auto maximum_destination_components =
        kind == IndependentReviewArtifactKind::PreparedHandoff ? 127U : 126U;
    if (static_cast<std::size_t>(token->supplied_components.size()) >
        maximum_destination_components) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::InvalidArguments,
            QStringLiteral("Destination has insufficient component headroom"));
    }
    if (!path.startsWith(u'/')) {
        return token;
    }
    const auto& destination_name = token->supplied_native_components.back();
    const auto staging_name = QByteArray{"."} + destination_name +
                              QByteArray{".appellate-independent-review-"} + QByteArray(6, 'x');
    QByteArray parent_native{"/"};
    for (std::size_t index = 0; index + 1U < token->supplied_native_components.size(); ++index) {
        if (parent_native.size() > 1) {
            parent_native += '/';
        }
        parent_native += token->supplied_native_components.at(index);
    }
    const auto staging_native =
        parent_native == "/" ? parent_native + staging_name : parent_native + '/' + staging_name;
    const auto longest_member = kind == IndependentReviewArtifactKind::PreparedHandoff
                                    ? QByteArrayView{"/review-declaration.template.json"}
                                    : QByteArrayView{"/resources/realism-review.json"};
    if (staging_name.size() > maximum_native_component_bytes ||
        staging_native.size() + longest_member.size() > maximum_native_path_bytes) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::InvalidArguments,
            QStringLiteral("Destination has insufficient native publication headroom"));
    }
    return token;
}

std::expected<void, IndependentReviewPublicationError>
validateIndependentReviewDestinationPath(const QString& path, IndependentReviewArtifactKind kind) {
    auto encoded = encodeIndependentReviewDestinationPath(path, kind);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return {};
}

std::expected<IndependentReviewPathToken, IndependentReviewPublicationError>
encodeIndependentReviewHandoffPath(const QString& path) {
    auto token = encodeIndependentReviewPathSpelling(path, false);
    if (!token) {
        return std::unexpected(token.error());
    }
    if (static_cast<std::size_t>(token->supplied_components.size()) > 127U) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::InvalidArguments,
            QStringLiteral("Handoff directory has insufficient member-path headroom"));
    }
    if (path.startsWith(u'/') &&
        token->supplied_native_path.size() +
                static_cast<qsizetype>(sizeof("/review-declaration.template.json") - 1U) >
            maximum_native_path_bytes) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::InvalidArguments,
            QStringLiteral("Handoff directory exceeds native member-path limits"));
    }
    return token;
}

std::expected<void, IndependentReviewPublicationError>
validateIndependentReviewHandoffPath(const QString& path) {
    auto encoded = encodeIndependentReviewHandoffPath(path);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return {};
}

#if defined(Q_OS_LINUX)

std::expected<IndependentReviewHandoffInput, IndependentReviewPublicationError>
readIndependentReviewHandoffDirectory(const QString& directory_path,
                                      const IndependentReviewInputReaderHooks& hooks) {
    auto token = encodeIndependentReviewHandoffPath(directory_path);
    if (!token) {
        return std::unexpected(token.error());
    }
    return readIndependentReviewHandoffDirectory(std::move(*token), hooks);
}

std::expected<IndependentReviewHandoffInput, IndependentReviewPublicationError>
readIndependentReviewHandoffDirectory(IndependentReviewPathToken directory_path,
                                      const IndependentReviewInputReaderHooks& hooks) {
    auto operand = resolveOperand(std::move(directory_path), maximum_native_components);
    if (!operand) {
        return std::unexpected(operand.error());
    }
    if (const auto feasible = validateHandoffOperandFeasibility(*operand); !feasible) {
        return std::unexpected(feasible.error());
    }
    const auto component_count = static_cast<std::size_t>(operand->components.size());
    auto chain = openRetainedChain(std::move(*operand), component_count);
    if (!chain) {
        auto error = chain.error();
        if (error.code != IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform) {
            error.code = IndependentReviewPublicationErrorCode::InvalidStagedArtifact;
        }
        return std::unexpected(std::move(error));
    }
    const auto& root = chain->controllers.back();
    auto bytes = readClosedHandoffRoot(
        root.descriptor.get(), root.identity, [&chain] { return rebindControllerChain(*chain); },
        hooks, QStringLiteral("Handoff directory"));
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    return IndependentReviewHandoffInput{
        std::move(bytes->handoff),
        std::move(bytes->declaration_template),
        IndependentReviewProtectedDirectory{
            static_cast<std::uint64_t>(root.identity.device),
            static_cast<std::uint64_t>(root.identity.inode),
        },
        2U,
    };
}

std::expected<IndependentReviewHandoffInput, IndependentReviewPublicationError>
readIndependentReviewStagedHandoffDirectory(const QString& retained_parent_proc_root,
                                            const IndependentReviewInputReaderHooks& hooks) {
    const auto components = suppliedComponents(retained_parent_proc_root);
    if (!components || !retained_parent_proc_root.startsWith(QStringLiteral("/proc/self/fd/")) ||
        components->text.size() != 5 || components->text.at(0) != QStringLiteral("proc") ||
        components->text.at(1) != QStringLiteral("self") ||
        components->text.at(2) != QStringLiteral("fd") ||
        components->native_path.size() +
                static_cast<qsizetype>(sizeof("/review-declaration.template.json") - 1U) >
            maximum_native_path_bytes) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::InvalidStagedArtifact,
            QStringLiteral("Staged handoff root is not a retained-parent procfs composite"));
    }
    bool descriptor_ok = false;
    const auto parent_descriptor_number = components->text.at(3).toInt(&descriptor_ok);
    if (!descriptor_ok || parent_descriptor_number < 0 ||
        components->text.at(3) != QString::number(parent_descriptor_number)) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::InvalidStagedArtifact,
            QStringLiteral("Staged handoff root has an invalid retained descriptor"));
    }
    Descriptor retained_parent(::fcntl(parent_descriptor_number, F_DUPFD_CLOEXEC, 0));
    struct stat parent_status{};
    if (!retained_parent || ::fstat(retained_parent.get(), &parent_status) != 0 ||
        !S_ISDIR(parent_status.st_mode)) {
        return publicationFailure(IndependentReviewPublicationErrorCode::InvalidStagedArtifact,
                                  QStringLiteral("Cannot retain staged handoff parent"));
    }
    Descriptor root(::open(components->native_path.constData(),
                           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat before_status{};
    struct stat named_status{};
    const auto& staging_name = components->native.at(4);
    if (!root || ::fstat(root.get(), &before_status) != 0 || !S_ISDIR(before_status.st_mode) ||
        ::fstatat(retained_parent.get(), staging_name.constData(), &named_status,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        !sameDirectoryIdentity(identityOf(before_status), identityOf(named_status))) {
        return publicationFailure(IndependentReviewPublicationErrorCode::InvalidStagedArtifact,
                                  QStringLiteral("Cannot retain staged handoff root"));
    }
    const auto root_identity = identityOf(before_status);
    auto bytes = readClosedHandoffRoot(
        root.get(), root_identity,
        [&] {
            struct stat held_parent{};
            struct stat rebound_root{};
            return ::fstat(retained_parent.get(), &held_parent) == 0 &&
                   sameDirectoryIdentity(identityOf(parent_status), identityOf(held_parent)) &&
                   ::fstatat(retained_parent.get(), staging_name.constData(), &rebound_root,
                             AT_SYMLINK_NOFOLLOW) == 0 &&
                   sameDirectoryIdentity(root_identity, identityOf(rebound_root));
        },
        hooks, QStringLiteral("Staged handoff"));
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    return IndependentReviewHandoffInput{
        std::move(bytes->handoff),
        std::move(bytes->declaration_template),
        IndependentReviewProtectedDirectory{
            static_cast<std::uint64_t>(root_identity.device),
            static_cast<std::uint64_t>(root_identity.inode),
        },
        2U,
    };
}

std::expected<QByteArray, IndependentReviewPublicationError>
readIndependentReviewDeclaration(const QString& declaration_path,
                                 const IndependentReviewInputReaderHooks& hooks) {
    auto token = encodeIndependentReviewPathSpelling(declaration_path, false);
    if (!token) {
        return std::unexpected(token.error());
    }
    return readIndependentReviewDeclaration(std::move(*token), hooks);
}

std::expected<QByteArray, IndependentReviewPublicationError>
readIndependentReviewDeclaration(IndependentReviewPathToken declaration_path,
                                 const IndependentReviewInputReaderHooks& hooks) {
    auto operand = resolveOperand(std::move(declaration_path), maximum_native_components);
    if (!operand) {
        return std::unexpected(operand.error());
    }
    const auto component_count = static_cast<std::size_t>(operand->components.size());
    const auto basename = operand->native_components.back();
    auto chain = openRetainedChain(std::move(*operand), component_count - 1U);
    if (!chain) {
        auto error = chain.error();
        if (error.code != IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform) {
            error.code = IndependentReviewPublicationErrorCode::InvalidStagedArtifact;
        }
        return std::unexpected(std::move(error));
    }
    Descriptor file(::openat(chain->controllers.back().descriptor.get(), basename.constData(),
                             O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    if (!file) {
        return publicationFailure(IndependentReviewPublicationErrorCode::InvalidStagedArtifact,
                                  QStringLiteral("Cannot open completed declaration"));
    }
    NodeIdentity file_identity;
    auto bytes = readStableDescriptor(file.get(), maximum_declaration_bytes, &file_identity);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    if (hooks.before_final_rebind) {
        hooks.before_final_rebind();
    }
    if (!rebindStableFile(chain->controllers.back().descriptor.get(), basename, file.get(),
                          file_identity) ||
        !rebindControllerChain(*chain)) {
        return publicationFailure(IndependentReviewPublicationErrorCode::InvalidStagedArtifact,
                                  QStringLiteral("Completed declaration changed during read"));
    }
    return bytes;
}

#else

std::expected<IndependentReviewHandoffInput, IndependentReviewPublicationError>
readIndependentReviewHandoffDirectory(const QString& directory_path,
                                      const IndependentReviewInputReaderHooks& hooks) {
    static_cast<void>(directory_path);
    static_cast<void>(hooks);
    return publicationFailure(IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform,
                              QStringLiteral("Independent-review input traversal requires Linux"));
}

std::expected<IndependentReviewHandoffInput, IndependentReviewPublicationError>
readIndependentReviewHandoffDirectory(IndependentReviewPathToken directory_path,
                                      const IndependentReviewInputReaderHooks& hooks) {
    static_cast<void>(directory_path);
    static_cast<void>(hooks);
    return publicationFailure(IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform,
                              QStringLiteral("Independent-review input traversal requires Linux"));
}

std::expected<IndependentReviewHandoffInput, IndependentReviewPublicationError>
readIndependentReviewStagedHandoffDirectory(const QString& retained_parent_proc_root,
                                            const IndependentReviewInputReaderHooks& hooks) {
    static_cast<void>(retained_parent_proc_root);
    static_cast<void>(hooks);
    return publicationFailure(
        IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform,
        QStringLiteral("Independent-review staged input traversal requires Linux"));
}

std::expected<QByteArray, IndependentReviewPublicationError>
readIndependentReviewDeclaration(const QString& declaration_path,
                                 const IndependentReviewInputReaderHooks& hooks) {
    static_cast<void>(declaration_path);
    static_cast<void>(hooks);
    return publicationFailure(IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform,
                              QStringLiteral("Independent-review input traversal requires Linux"));
}

std::expected<QByteArray, IndependentReviewPublicationError>
readIndependentReviewDeclaration(IndependentReviewPathToken declaration_path,
                                 const IndependentReviewInputReaderHooks& hooks) {
    static_cast<void>(declaration_path);
    static_cast<void>(hooks);
    return publicationFailure(IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform,
                              QStringLiteral("Independent-review input traversal requires Linux"));
}

#endif

#if defined(Q_OS_LINUX)
namespace {

using DirectoryKey = std::pair<std::uint64_t, std::uint64_t>;

[[nodiscard]] auto inventoryProtectedDirectory(int descriptor, std::size_t& entry_count,
                                               std::set<DirectoryKey>& directory_identities,
                                               const IndependentReviewPublisherHooks& hooks)
    -> std::expected<void, IndependentReviewPublicationError> {
    struct stat root_status{};
    if (::fstat(descriptor, &root_status) != 0 || !S_ISDIR(root_status.st_mode)) {
        return publicationFailure(IndependentReviewPublicationErrorCode::CannotPublishDestination,
                                  QStringLiteral("Cannot inspect a protected input directory"));
    }
    const auto root_key = DirectoryKey{static_cast<std::uint64_t>(root_status.st_dev),
                                       static_cast<std::uint64_t>(root_status.st_ino)};
    if (!directory_identities.insert(root_key).second) {
        return {};
    }
    const auto names =
        directoryNames(descriptor, maximum_protected_entries - entry_count,
                       IndependentReviewPublicationErrorCode::DestinationOverlapsProtectedInput,
                       QStringLiteral("Protected input inventory exceeds 20000 entries"));
    if (!names) {
        return std::unexpected(names.error());
    }
    for (const auto& name : *names) {
        if (++entry_count > maximum_protected_entries) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::DestinationOverlapsProtectedInput,
                QStringLiteral("Protected input inventory exceeds 20000 entries"));
        }
        const auto action = publisherEvent(
            hooks, IndependentReviewPublisherObservation{
                       IndependentReviewPublisherEvent::ProtectedInventory, {}, name, entry_count});
        if (failsBefore(action)) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::DestinationOverlapsProtectedInput,
                QStringLiteral("Protected input inventory was interrupted"));
        }
        struct stat status{};
        if (::fstatat(descriptor, name.constData(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::DestinationOverlapsProtectedInput,
                QStringLiteral("Protected input inventory is unstable"));
        }
        if (S_ISDIR(status.st_mode)) {
            Descriptor child(::openat(descriptor, name.constData(),
                                      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (!child) {
                return publicationFailure(
                    IndependentReviewPublicationErrorCode::DestinationOverlapsProtectedInput,
                    QStringLiteral("Cannot retain a protected input descendant"));
            }
            if (auto nested = inventoryProtectedDirectory(child.get(), entry_count,
                                                          directory_identities, hooks);
                !nested) {
                return nested;
            }
        }
        if (failsAfter(action)) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::DestinationOverlapsProtectedInput,
                QStringLiteral("Protected input inventory was interrupted"));
        }
    }
    return {};
}

[[nodiscard]] auto protectedDirectoryIdentities(
    const std::vector<IndependentReviewProtectedDirectory>& retained_directories,
    std::size_t retained_entry_count, const QStringList& protected_paths,
    const IndependentReviewPublisherHooks& hooks)
    -> std::expected<std::set<DirectoryKey>, IndependentReviewPublicationError> {
    std::set<DirectoryKey> identities;
    if (retained_entry_count > maximum_protected_entries) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::DestinationOverlapsProtectedInput,
            QStringLiteral("Protected input inventory exceeds 20000 entries"));
    }
    for (const auto& directory : retained_directories) {
        identities.emplace(directory.device, directory.inode);
    }
    std::size_t entry_count = retained_entry_count;
    for (const auto& path : protected_paths) {
        auto operand = resolveOperand(path, maximum_native_components, &hooks);
        if (!operand) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::DestinationOverlapsProtectedInput,
                QStringLiteral("A protected input cannot be retained"));
        }
        const auto component_count = static_cast<std::size_t>(operand->components.size());
        auto chain = openRetainedChain(std::move(*operand), component_count);
        if (!chain || !rebindControllerChain(*chain, &hooks)) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::DestinationOverlapsProtectedInput,
                QStringLiteral("A protected input changed during retention"));
        }
        if (auto inventoried = inventoryProtectedDirectory(
                chain->controllers.back().descriptor.get(), entry_count, identities, hooks);
            !inventoried) {
            return std::unexpected(inventoried.error());
        }
        if (!rebindControllerChain(*chain, &hooks)) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::DestinationOverlapsProtectedInput,
                QStringLiteral("A protected input changed during inventory"));
        }
    }
    return identities;
}

struct PublicationPlanEntry final {
    QString relative_path;
    QByteArray component;
    bool directory{};
    std::optional<std::size_t> parent_index;
    mode_t required_mode{};
    const QByteArray* bytes{};
};

struct PublicationLedgerEntry final {
    std::size_t declaration_index{};
    Descriptor retained_descriptor;
    Descriptor usable_descriptor;
    NodeIdentity identity;
    bool name_removed{};

    [[nodiscard]] int retained() const noexcept { return retained_descriptor.get(); }
    [[nodiscard]] int usable() const noexcept {
        return usable_descriptor ? usable_descriptor.get() : retained_descriptor.get();
    }
};

enum class BoundTreeState {
    Absent,
    Exact,
    Other,
    Ambiguous,
};

enum class ParentSyncState {
    NotAttempted,
    Ok,
    Failed,
};

[[nodiscard]] QString telemetry(QStringView reachability, QStringView residue,
                                ParentSyncState parent_sync) {
    const auto sync = parent_sync == ParentSyncState::Ok       ? QStringLiteral("ok")
                      : parent_sync == ParentSyncState::Failed ? QStringLiteral("failed")
                                                               : QStringLiteral("not_attempted");
    return QStringLiteral("original_staging_reachability=%1;cleanup_residue=%2;parent_fsync=%3")
        .arg(reachability, residue, sync);
}

[[nodiscard]] bool isValidStagingSuffix(QByteArrayView suffix) {
    return suffix.size() == 6 && std::ranges::all_of(suffix, [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'z');
           });
}

[[nodiscard]] QByteArray randomStagingSuffix() {
    constexpr std::string_view alphabet{"0123456789abcdefghijklmnopqrstuvwxyz"};
    auto value = QRandomGenerator::system()->generate64();
    QByteArray suffix(6, Qt::Uninitialized);
    for (auto& character : suffix) {
        character = alphabet.at(static_cast<std::size_t>(value % alphabet.size()));
        value /= alphabet.size();
    }
    return suffix;
}

class PublicationSession final {
  public:
    PublicationSession(const IndependentReviewPublicationRequest& request,
                       const IndependentReviewPublisherHooks& hooks)
        : request_(request), hooks_(hooks) {}

    [[nodiscard]] auto run() -> std::expected<void, IndependentReviewPublicationError>;

  private:
    [[nodiscard]] auto preflight() -> std::expected<void, IndependentReviewPublicationError>;
    [[nodiscard]] auto declarePlan() -> std::expected<void, IndependentReviewPublicationError>;
    [[nodiscard]] auto createStagingRoot()
        -> std::expected<void, IndependentReviewPublicationError>;
    [[nodiscard]] auto createDirectory(std::size_t declaration_index)
        -> std::expected<void, IndependentReviewPublicationError>;
    [[nodiscard]] auto createFile(std::size_t declaration_index)
        -> std::expected<void, IndependentReviewPublicationError>;
    [[nodiscard]] auto retainCreatedDirectory(std::size_t declaration_index, int parent_descriptor,
                                              const QByteArray& component, bool staging_root,
                                              PublisherMutationResult creation)
        -> std::expected<void, IndependentReviewPublicationError>;
    [[nodiscard]] auto retainCreatedFile(std::size_t declaration_index, int parent_descriptor,
                                         const QByteArray& component, Descriptor descriptor,
                                         PublisherMutationResult creation)
        -> std::expected<void, IndependentReviewPublicationError>;
    [[nodiscard]] int parentDescriptor(const PublicationPlanEntry& declaration) const;
    [[nodiscard]] auto verifyCompleteTree(int root_descriptor, const QByteArray& root_name) const
        -> std::expected<void, IndependentReviewPublicationError>;
    [[nodiscard]] auto verifyPartialStagingTree() const
        -> std::expected<void, IndependentReviewPublicationError>;
    [[nodiscard]] BoundTreeState treeStateAt(const QByteArray& name) const;
    [[nodiscard]] auto validateAndSyncStaging()
        -> std::expected<void, IndependentReviewPublicationError>;
    [[nodiscard]] auto cleanup() -> std::expected<void, IndependentReviewPublicationError>;
    [[nodiscard]] auto cleanupOr(IndependentReviewPublicationError original)
        -> std::expected<void, IndependentReviewPublicationError>;
    [[nodiscard]] auto containmentFailure()
        -> std::expected<void, IndependentReviewPublicationError>;
    [[nodiscard]] QString cleanupTelemetry() const;
    [[nodiscard]] bool retainedCreationDescriptorsExact() const;
    [[nodiscard]] ParentSyncState bestEffortParentSync();
    void updateReportRemaining() const;

    const IndependentReviewPublicationRequest& request_;
    const IndependentReviewPublisherHooks& hooks_;
    RetainedChain parent_chain_;
    QByteArray destination_name_;
    QString destination_path_;
    QByteArray staging_name_;
    QString staging_path_;
    std::vector<PublicationPlanEntry> plan_;
    std::vector<std::optional<PublicationLedgerEntry>> ledger_;
    std::vector<std::optional<NodeIdentity>> creation_identities_;
    std::size_t created_count_{};
    bool unretained_staging_creation_{};
    ParentSyncState parent_sync_{ParentSyncState::NotAttempted};
    bool lease_acquired_{};
};

std::expected<void, IndependentReviewPublicationError> PublicationSession::declarePlan() {
    const auto prepare = request_.kind == IndependentReviewArtifactKind::PreparedHandoff;
    const std::array<QString, 2> expected_paths =
        prepare ? std::array{QStringLiteral("handoff.json"),
                             QStringLiteral("review-declaration.template.json")}
                : std::array{QStringLiteral("manifest.json"),
                             QStringLiteral("resources/realism-review.json")};
    if (request_.members.size() != expected_paths.size() || !request_.validate_staged) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::CannotPublishDestination,
            QStringLiteral("Publication request has the wrong fixed member plan"));
    }
    for (std::size_t index = 0; index < request_.members.size(); ++index) {
        if (request_.members.at(index).relative_path != expected_paths.at(index) ||
            request_.members.at(index).bytes.isEmpty()) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::CannotPublishDestination,
                QStringLiteral("Publication request member order or bytes are invalid"));
        }
    }
    const auto final_review_byte_limit =
        hooks_.injected_final_review_byte_limit.value_or(8 * 1024 * 1024);
    if ((prepare && (request_.members.at(0).bytes.size() > maximum_handoff_bytes ||
                     request_.members.at(1).bytes.size() > maximum_template_bytes)) ||
        (!prepare && (request_.members.at(0).bytes.size() > maximum_template_bytes ||
                      request_.members.at(1).bytes.size() > final_review_byte_limit))) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::InvalidStagedArtifact,
            QStringLiteral("Generated publication member exceeds its byte limit"));
    }

    plan_.clear();
    plan_.reserve(prepare ? 3U : 4U);
    plan_.push_back(PublicationPlanEntry{{}, {}, true, std::nullopt, 0700, nullptr});
    if (prepare) {
        plan_.push_back(PublicationPlanEntry{expected_paths.at(0), QByteArray{"handoff.json"},
                                             false, std::size_t{0}, 0600,
                                             &request_.members.at(0).bytes});
        plan_.push_back(PublicationPlanEntry{expected_paths.at(1),
                                             QByteArray{"review-declaration.template.json"}, false,
                                             std::size_t{0}, 0600, &request_.members.at(1).bytes});
    } else {
        plan_.push_back(PublicationPlanEntry{QStringLiteral("resources"), QByteArray{"resources"},
                                             true, std::size_t{0}, 0700, nullptr});
        plan_.push_back(PublicationPlanEntry{expected_paths.at(0), QByteArray{"manifest.json"},
                                             false, std::size_t{0}, 0600,
                                             &request_.members.at(0).bytes});
        plan_.push_back(PublicationPlanEntry{expected_paths.at(1),
                                             QByteArray{"realism-review.json"}, false,
                                             std::size_t{1}, 0600, &request_.members.at(1).bytes});
    }
    ledger_.clear();
    ledger_.resize(plan_.size());
    creation_identities_.clear();
    creation_identities_.resize(plan_.size());
    if (hooks_.report != nullptr) {
        hooks_.report->retained_creations.clear();
        hooks_.report->retained_creations.reserve(plan_.size());
    }
    created_count_ = 0;
    const auto action = publisherEvent(
        hooks_,
        IndependentReviewPublisherObservation{
            IndependentReviewPublisherEvent::PlanDeclared, destination_path_, {}, plan_.size()});
    if (action != IndependentReviewPublisherInjectedAction::Continue) {
        return publicationFailure(IndependentReviewPublicationErrorCode::CannotPublishDestination,
                                  QStringLiteral("Publication plan declaration was interrupted"));
    }
    return {};
}

std::expected<void, IndependentReviewPublicationError> PublicationSession::preflight() {
#if !defined(SYS_renameat2) || !defined(SYS_fchmodat2)
    return publicationFailure(IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform,
                              QStringLiteral("Required publication syscalls are unavailable"));
#endif
    const auto mode_normalization = publisherModeNormalizationAvailable(hooks_);
    if (mode_normalization != PreflightAvailability::Available) {
        return publicationFailure(
            mode_normalization == PreflightAvailability::Unsupported
                ? IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform
                : IndependentReviewPublicationErrorCode::CannotPublishDestination,
            QStringLiteral("Descriptor-bound mode normalization is unavailable"));
    }
    for (const auto& protected_path : request_.protected_directory_paths) {
        if (const auto valid = validateIndependentReviewPathSpelling(protected_path, false);
            !valid) {
            return valid;
        }
    }
    const auto prepare = request_.kind == IndependentReviewArtifactKind::PreparedHandoff;
    auto destination_token =
        request_.destination_token.has_value()
            ? std::expected<IndependentReviewPathToken,
                            IndependentReviewPublicationError>{*request_.destination_token}
            : encodeIndependentReviewDestinationPath(request_.destination_path, request_.kind);
    if (!destination_token || destination_token->supplied_path != request_.destination_path ||
        !destinationTokenHasHeadroom(*destination_token, request_.kind)) {
        return destination_token
                   ? publicationFailure(IndependentReviewPublicationErrorCode::InvalidArguments,
                                        QStringLiteral("Encoded destination token is inconsistent"))
                   : std::unexpected(destination_token.error());
    }
    auto operand = resolveOperand(std::move(*destination_token), prepare ? 127U : 126U, &hooks_);
    if (!operand) {
        return std::unexpected(operand.error());
    }
    const auto total_components = static_cast<std::size_t>(operand->components.size());
    if (total_components == 0) {
        return publicationFailure(IndependentReviewPublicationErrorCode::InvalidArguments,
                                  QStringLiteral("Destination must name a directory leaf"));
    }
    destination_name_ = operand->native_components.back();
    destination_path_ = operand->absolute_path;
    const auto parent_count = total_components - 1U;
    auto parent_chain = openRetainedChain(std::move(*operand), parent_count);
    if (!parent_chain) {
        return std::unexpected(parent_chain.error());
    }
    parent_chain_ = std::move(*parent_chain);
    const auto parent_path = parent_chain_.absolute_path;
    const auto maximum_staging_component =
        QByteArray{"."} + destination_name_ + ".appellate-independent-review-" + QByteArray(6, 'x');
    const auto longest_member = prepare ? QByteArray{"/review-declaration.template.json"}
                                        : QByteArray{"/resources/realism-review.json"};
    const auto staging_native = (parent_chain_.native_absolute_path == QByteArray{"/"}
                                     ? parent_chain_.native_absolute_path
                                     : parent_chain_.native_absolute_path + QByteArray{"/"}) +
                                maximum_staging_component;
    const auto proc_root = QByteArray{"/proc/self/fd/"} +
                           QByteArray::number(parent_chain_.controllers.back().descriptor.get()) +
                           QByteArray{"/"} + maximum_staging_component;
    if (maximum_staging_component.size() > maximum_native_component_bytes ||
        staging_native.size() + longest_member.size() > maximum_native_path_bytes ||
        proc_root.size() + longest_member.size() > maximum_native_path_bytes) {
        return publicationFailure(
            request_.destination_path.startsWith(u'/')
                ? IndependentReviewPublicationErrorCode::InvalidArguments
                : IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform,
            QStringLiteral("Destination leaves no native headroom for fixed publication members"));
    }
    if (hooks_.report != nullptr) {
        hooks_.report->destination_path = destination_path_;
    }
    const auto operand_action = publisherEvent(
        hooks_, IndependentReviewPublisherObservation{
                    IndependentReviewPublisherEvent::OperandValidated, destination_path_, {}, 0});
    if (operand_action != IndependentReviewPublisherInjectedAction::Continue) {
        return publicationFailure(IndependentReviewPublicationErrorCode::CannotPublishDestination,
                                  QStringLiteral("Destination validation was interrupted"));
    }
    for (std::size_t index = 0; index < parent_chain_.controllers.size(); ++index) {
        auto& controller = parent_chain_.controllers.at(index);
        const auto observation = IndependentReviewPublisherObservation{
            IndependentReviewPublisherEvent::ControllerOpened, controller.absolute_path,
            controller.component, index};
        const auto action = publisherEvent(hooks_, observation);
        const auto outcome = publisherOutcome(hooks_, observation);
        if (action != IndependentReviewPublisherInjectedAction::Continue ||
            (outcome.has_value() && !outcome->operation_succeeded)) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::CannotPublishDestination,
                QStringLiteral("Destination controller retention was interrupted"));
        }
        if (outcome.has_value() && outcome->retained_stat.has_value()) {
            controller.identity = applySyntheticStat(controller.identity, *outcome->retained_stat);
        }
    }
    struct stat destination_status{};
    if (::fstatat(parent_chain_.controllers.back().descriptor.get(), destination_name_.constData(),
                  &destination_status, AT_SYMLINK_NOFOLLOW) == 0) {
        return publicationFailure(IndependentReviewPublicationErrorCode::DestinationExists,
                                  QStringLiteral("Publication destination already exists"));
    }
    if (errno != ENOENT) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::CannotPublishDestination,
            nativeError(QStringLiteral("Cannot prove publication destination absence")));
    }
    auto protected_identities =
        protectedDirectoryIdentities(request_.protected_directories, request_.protected_entry_count,
                                     request_.protected_directory_paths, hooks_);
    if (!protected_identities) {
        return std::unexpected(protected_identities.error());
    }
    for (const auto& controller : parent_chain_.controllers) {
        const auto key = DirectoryKey{static_cast<std::uint64_t>(controller.identity.device),
                                      static_cast<std::uint64_t>(controller.identity.inode)};
        if (protected_identities->contains(key)) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::DestinationOverlapsProtectedInput,
                QStringLiteral("Destination parent overlaps a protected input"));
        }
    }
    const auto overlap_action =
        publisherEvent(hooks_, IndependentReviewPublisherObservation{
                                   IndependentReviewPublisherEvent::ProtectedOverlapChecked,
                                   destination_path_,
                                   {},
                                   protected_identities->size()});
    if (overlap_action != IndependentReviewPublisherInjectedAction::Continue) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::DestinationOverlapsProtectedInput,
            QStringLiteral("Protected-input comparison was interrupted"));
    }
    if (const auto safe = validateControllerChain(parent_chain_, hooks_); !safe) {
        return safe;
    }

    if (!publisherLock(parent_chain_.controllers.back().descriptor.get(), parent_path, hooks_)) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::CannotPublishDestination,
            QStringLiteral("Cannot acquire the destination-parent publication lease"));
    }
    lease_acquired_ = true;
    if (publisherEvent(
            hooks_,
            IndependentReviewPublisherObservation{
                IndependentReviewPublisherEvent::ParentLeaseAcquired, parent_path, {}, 0}) !=
        IndependentReviewPublisherInjectedAction::Continue) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::CannotPublishDestination,
            QStringLiteral("Destination-parent lease retention was interrupted"));
    }

    const auto parent_proc =
        QStringLiteral("/proc/self/fd/%1").arg(parent_chain_.controllers.back().descriptor.get());
    const auto proc_action = publisherEvent(
        hooks_, IndependentReviewPublisherObservation{
                    IndependentReviewPublisherEvent::ProcfsPreflight, parent_proc, {}, 0});
    Descriptor proc_descriptor(failsBefore(proc_action)
                                   ? -1
                                   : ::open(QFile::encodeName(parent_proc).constData(),
                                            O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    struct stat proc_status{};
    struct stat parent_status{};
    if (!proc_descriptor || ::fstat(proc_descriptor.get(), &proc_status) != 0 ||
        ::fstat(parent_chain_.controllers.back().descriptor.get(), &parent_status) != 0 ||
        proc_status.st_dev != parent_status.st_dev || proc_status.st_ino != parent_status.st_ino ||
        failsAfter(proc_action)) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform,
            QStringLiteral("Retained-parent procfs traversal is unavailable"));
    }
    if (!publisherSync(parent_chain_.controllers.back().descriptor.get(),
                       IndependentReviewPublisherEvent::DirectorySync, parent_path, {}, 0,
                       hooks_)) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform,
            QStringLiteral("Destination filesystem does not support directory synchronization"));
    }
    return declarePlan();
}

int PublicationSession::parentDescriptor(const PublicationPlanEntry& declaration) const {
    if (!declaration.parent_index.has_value()) {
        return parent_chain_.controllers.back().descriptor.get();
    }
    const auto& parent = ledger_.at(*declaration.parent_index);
    return parent.has_value() && !parent->name_removed ? parent->usable() : -1;
}

std::expected<void, IndependentReviewPublicationError>
PublicationSession::retainCreatedDirectory(std::size_t declaration_index, int parent_descriptor,
                                           const QByteArray& component, bool staging_root,
                                           PublisherMutationResult creation) {
    const auto& declaration = plan_.at(declaration_index);
    const auto absolute_path =
        staging_root ? staging_path_ : staging_path_ + u'/' + declaration.relative_path;
    auto descriptor = publisherOpenDirectory(
        parent_descriptor, component, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC,
        staging_root ? IndependentReviewPublisherEvent::StagingRetainAttempted
                     : IndependentReviewPublisherEvent::DirectoryRetainAttempted,
        absolute_path, declaration_index, hooks_);
    struct stat initial_status{};
    if (!descriptor || !statDescriptor(descriptor.get(), &initial_status)) {
        return publicationFailure(
            staging_root ? IndependentReviewPublicationErrorCode::PublicationCleanupFailed
                         : IndependentReviewPublicationErrorCode::CannotPublishDestination,
            QStringLiteral("Cannot retain a created publication directory"));
    }
    ledger_.at(declaration_index)
        .emplace(PublicationLedgerEntry{declaration_index, std::move(descriptor), Descriptor{},
                                        identityOf(initial_status), false});
    creation_identities_.at(declaration_index) = ledger_.at(declaration_index)->identity;
    if (hooks_.report != nullptr) {
        const auto& retained_report = *ledger_.at(declaration_index);
        hooks_.report->retained_creations.push_back(IndependentReviewPublisherRetainedCreation{
            declaration_index,
            retained_report.retained(),
            static_cast<std::uint64_t>(retained_report.identity.device),
            static_cast<std::uint64_t>(retained_report.identity.inode),
        });
    }
    ++created_count_;
    if (staging_root) {
        unretained_staging_creation_ = false;
    }
    auto& retained = *ledger_.at(declaration_index);
    const auto created_action =
        publisherEvent(hooks_, IndependentReviewPublisherObservation{
                                   staging_root ? IndependentReviewPublisherEvent::StagingCreated
                                                : IndependentReviewPublisherEvent::DirectoryCreated,
                                   absolute_path, component, declaration_index});
    const auto created_observation = IndependentReviewPublisherObservation{
        staging_root ? IndependentReviewPublisherEvent::StagingCreated
                     : IndependentReviewPublisherEvent::DirectoryCreated,
        absolute_path, component, declaration_index};
    const auto created_outcome = publisherOutcome(hooks_, created_observation);
    if (failsBefore(created_action)) {
        return publicationFailure(IndependentReviewPublicationErrorCode::CannotPublishDestination,
                                  QStringLiteral("Created directory retention was interrupted"));
    }
    if (!creation.operation_succeeded) {
        return publicationFailure(IndependentReviewPublicationErrorCode::CannotPublishDestination,
                                  nativeError(QStringLiteral("Directory creation reported failure"),
                                              creation.native_error));
    }
    const auto observed_initial =
        created_outcome.has_value() && created_outcome->retained_stat.has_value()
            ? applySyntheticStat(retained.identity, *created_outcome->retained_stat)
            : retained.identity;
    if ((created_outcome.has_value() && !created_outcome->operation_succeeded) ||
        observed_initial.type != S_IFDIR || observed_initial.owner != ::geteuid()) {
        return publicationFailure(
            staging_root ? IndependentReviewPublicationErrorCode::PublicationCleanupFailed
                         : IndependentReviewPublicationErrorCode::CannotPublishDestination,
            QStringLiteral("Created publication directory has unsafe initial metadata"));
    }
    const auto parent_identity = declaration.parent_index.has_value()
                                     ? ledger_.at(*declaration.parent_index)->identity
                                     : parent_chain_.controllers.back().identity;
    if (observed_initial.device != parent_identity.device) {
        return publicationFailure(
            staging_root ? IndependentReviewPublicationErrorCode::PublicationCleanupFailed
                         : IndependentReviewPublicationErrorCode::CannotPublishDestination,
            QStringLiteral("Created publication directory is on the wrong filesystem"));
    }
    const auto mode_mutation = publisherChmod(retained.retained(), declaration.required_mode,
                                              absolute_path, component, declaration_index, hooks_);
    if (!mode_mutation.state_change_applied) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::CannotPublishDestination,
            QStringLiteral("Cannot normalize created publication directory mode"));
    }
    const auto mode_observation =
        IndependentReviewPublisherObservation{IndependentReviewPublisherEvent::ModeNormalized,
                                              absolute_path, component, declaration_index};
    const auto mode_action = publisherEvent(hooks_, mode_observation);
    const auto mode_outcome = publisherOutcome(hooks_, mode_observation);
    struct stat normalized_status{};
    if (!statDescriptor(retained.retained(), &normalized_status)) {
        return publicationFailure(
            staging_root ? IndependentReviewPublicationErrorCode::PublicationCleanupFailed
                         : IndependentReviewPublicationErrorCode::CannotPublishDestination,
            QStringLiteral("Cannot rebind created publication directory"));
    }
    const auto physical_normalized = identityOf(normalized_status);
    retained.identity = physical_normalized;
    creation_identities_.at(declaration_index) = physical_normalized;
    const auto normalized =
        mode_outcome.has_value() && mode_outcome->retained_stat.has_value()
            ? applySyntheticStat(physical_normalized, *mode_outcome->retained_stat)
            : physical_normalized;
    if (normalized.owner != ::geteuid() || normalized.mode != declaration.required_mode ||
        !publisherEntryRebind(parent_descriptor, component, retained.retained(), normalized, true,
                              absolute_path, declaration_index, hooks_)) {
        return publicationFailure(
            staging_root ? IndependentReviewPublicationErrorCode::PublicationCleanupFailed
                         : IndependentReviewPublicationErrorCode::CannotPublishDestination,
            QStringLiteral("Created publication directory metadata is not exact"));
    }
    auto usable_descriptor = publisherOpenDirectory(
        parent_descriptor, component, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC,
        IndependentReviewPublisherEvent::DirectoryUsableRetainAttempted, absolute_path,
        declaration_index, hooks_);
    struct stat usable_status{};
    if (!usable_descriptor || !statDescriptor(usable_descriptor.get(), &usable_status) ||
        !sameDirectoryIdentity(physical_normalized, identityOf(usable_status))) {
        return publicationFailure(
            staging_root ? IndependentReviewPublicationErrorCode::PublicationCleanupFailed
                         : IndependentReviewPublicationErrorCode::CannotPublishDestination,
            QStringLiteral("Cannot retain normalized publication directory"));
    }
    retained.usable_descriptor = std::move(usable_descriptor);
    retained.identity = identityOf(usable_status);
    creation_identities_.at(declaration_index) = retained.identity;
    if (!mode_mutation.operation_succeeded ||
        mode_action != IndependentReviewPublisherInjectedAction::Continue ||
        (mode_outcome.has_value() && !mode_outcome->operation_succeeded)) {
        return publicationFailure(IndependentReviewPublicationErrorCode::CannotPublishDestination,
                                  QStringLiteral("Directory mode normalization was interrupted"));
    }
    const auto access =
        probeBoundPublisherAcl(parent_descriptor, component, retained.usable(), retained.identity,
                               true, false, absolute_path, declaration_index, hooks_);
    const auto default_acl =
        probeBoundPublisherAcl(parent_descriptor, component, retained.usable(), retained.identity,
                               true, true, absolute_path, declaration_index, hooks_);
    if (access != AclResult::Absent || default_acl != AclResult::Absent) {
        const auto unsupported =
            access == AclResult::Unsupported || default_acl == AclResult::Unsupported;
        return publicationFailure(
            staging_root  ? IndependentReviewPublicationErrorCode::PublicationCleanupFailed
            : unsupported ? IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform
                          : IndependentReviewPublicationErrorCode::CannotPublishDestination,
            QStringLiteral("Created publication directory ACL policy is not exact"));
    }
    const auto parent_path = staging_root ? parent_chain_.absolute_path : staging_path_;
    if (!publisherSync(retained.usable(), IndependentReviewPublisherEvent::DirectorySync,
                       absolute_path, component, declaration_index, hooks_) ||
        !publisherSync(parent_descriptor, IndependentReviewPublisherEvent::DirectorySync,
                       parent_path, {}, declaration_index, hooks_)) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::CannotPublishDestination,
            QStringLiteral("Cannot synchronize created publication directory"));
    }
    if (failsAfter(created_action)) {
        return publicationFailure(IndependentReviewPublicationErrorCode::CannotPublishDestination,
                                  QStringLiteral("Created directory retention was interrupted"));
    }
    return {};
}

std::expected<void, IndependentReviewPublicationError>
PublicationSession::retainCreatedFile(std::size_t declaration_index, int parent_descriptor,
                                      const QByteArray& component, Descriptor descriptor,
                                      PublisherMutationResult creation) {
    const auto& declaration = plan_.at(declaration_index);
    const auto absolute_path = staging_path_ + u'/' + declaration.relative_path;
    struct stat initial_status{};
    if (!descriptor || !statDescriptor(descriptor.get(), &initial_status)) {
        return publicationFailure(IndependentReviewPublicationErrorCode::CannotPublishDestination,
                                  QStringLiteral("Cannot exclusively create a publication file"));
    }
    ledger_.at(declaration_index)
        .emplace(PublicationLedgerEntry{declaration_index, std::move(descriptor), Descriptor{},
                                        identityOf(initial_status), false});
    creation_identities_.at(declaration_index) = ledger_.at(declaration_index)->identity;
    if (hooks_.report != nullptr) {
        const auto& retained_report = *ledger_.at(declaration_index);
        hooks_.report->retained_creations.push_back(IndependentReviewPublisherRetainedCreation{
            declaration_index,
            retained_report.retained(),
            static_cast<std::uint64_t>(retained_report.identity.device),
            static_cast<std::uint64_t>(retained_report.identity.inode),
        });
    }
    ++created_count_;
    const auto created_observation = IndependentReviewPublisherObservation{
        IndependentReviewPublisherEvent::FileCreated, absolute_path, component, declaration_index};
    const auto created_action = publisherEvent(hooks_, created_observation);
    const auto created_outcome = publisherOutcome(hooks_, created_observation);
    if (failsBefore(created_action)) {
        return publicationFailure(IndependentReviewPublicationErrorCode::CannotPublishDestination,
                                  QStringLiteral("File creation was interrupted"));
    }
    if (!creation.operation_succeeded) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::CannotPublishDestination,
            nativeError(QStringLiteral("File creation reported failure"), creation.native_error));
    }
    auto& retained = *ledger_.at(declaration_index);
    const auto observed_initial =
        created_outcome.has_value() && created_outcome->retained_stat.has_value()
            ? applySyntheticStat(retained.identity, *created_outcome->retained_stat)
            : retained.identity;
    if ((created_outcome.has_value() && !created_outcome->operation_succeeded) ||
        observed_initial.type != S_IFREG || observed_initial.link_count != 1 ||
        observed_initial.owner != ::geteuid()) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::CannotPublishDestination,
            QStringLiteral("Created publication file has unsafe initial metadata"));
    }
    const auto mode_mutation = publisherChmod(retained.retained(), declaration.required_mode,
                                              absolute_path, component, declaration_index, hooks_);
    if (!mode_mutation.state_change_applied) {
        return publicationFailure(IndependentReviewPublicationErrorCode::CannotPublishDestination,
                                  QStringLiteral("Cannot normalize publication file mode"));
    }
    const auto mode_observation =
        IndependentReviewPublisherObservation{IndependentReviewPublisherEvent::ModeNormalized,
                                              absolute_path, component, declaration_index};
    const auto mode_action = publisherEvent(hooks_, mode_observation);
    const auto mode_outcome = publisherOutcome(hooks_, mode_observation);
    struct stat normalized_status{};
    if (!statDescriptor(retained.retained(), &normalized_status)) {
        return publicationFailure(IndependentReviewPublicationErrorCode::CannotPublishDestination,
                                  QStringLiteral("Cannot rebind created publication file"));
    }
    const auto physical_normalized = identityOf(normalized_status);
    retained.identity = physical_normalized;
    creation_identities_.at(declaration_index) = physical_normalized;
    const auto normalized =
        mode_outcome.has_value() && mode_outcome->retained_stat.has_value()
            ? applySyntheticStat(physical_normalized, *mode_outcome->retained_stat)
            : physical_normalized;
    const auto access_acl =
        probeBoundPublisherAcl(parent_descriptor, component, retained.retained(), normalized, false,
                               false, absolute_path, declaration_index, hooks_);
    if (access_acl == AclResult::Unsupported) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform,
            QStringLiteral("Created publication file ACL inspection is unsupported"));
    }
    if (normalized.owner != ::geteuid() || normalized.mode != declaration.required_mode ||
        access_acl != AclResult::Absent ||
        !publisherEntryRebind(parent_descriptor, component, retained.retained(), normalized, false,
                              absolute_path, declaration_index, hooks_)) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::CannotPublishDestination,
            QStringLiteral("Created publication file metadata or ACL is not exact"));
    }
    if (!mode_mutation.operation_succeeded ||
        mode_action != IndependentReviewPublisherInjectedAction::Continue ||
        (mode_outcome.has_value() && !mode_outcome->operation_succeeded)) {
        return publicationFailure(IndependentReviewPublicationErrorCode::CannotPublishDestination,
                                  QStringLiteral("File mode normalization was interrupted"));
    }

    const auto write = publisherWriteAll(retained.usable(), *declaration.bytes, absolute_path,
                                         component, declaration_index, hooks_);
    if (!write.operation_succeeded ||
        !publisherSync(retained.usable(), IndependentReviewPublisherEvent::FileSync, absolute_path,
                       component, declaration_index, hooks_)) {
        return publicationFailure(IndependentReviewPublicationErrorCode::CannotPublishDestination,
                                  QStringLiteral("Cannot synchronize publication file"));
    }
    auto reread = readStableDescriptor(retained.usable(), declaration.bytes->size());
    const auto parent_path = declaration.parent_index == std::size_t{0}
                                 ? staging_path_
                                 : staging_path_ + QStringLiteral("/resources");
    if (!reread || *reread != *declaration.bytes ||
        !publisherSync(parent_descriptor, IndependentReviewPublisherEvent::DirectorySync,
                       parent_path, {}, declaration_index, hooks_)) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::CannotPublishDestination,
            QStringLiteral("Publication file failed exact reread or parent synchronization"));
    }
    if (publisherEvent(
            hooks_, IndependentReviewPublisherObservation{
                        IndependentReviewPublisherEvent::BytesChecked, absolute_path, component,
                        declaration_index}) != IndependentReviewPublisherInjectedAction::Continue) {
        return publicationFailure(IndependentReviewPublicationErrorCode::CannotPublishDestination,
                                  QStringLiteral("Publication byte verification was interrupted"));
    }
    if (failsAfter(created_action)) {
        return publicationFailure(IndependentReviewPublicationErrorCode::CannotPublishDestination,
                                  QStringLiteral("File creation was interrupted"));
    }
    return {};
}

std::expected<void, IndependentReviewPublicationError> PublicationSession::createStagingRoot() {
    const auto& parent = parent_chain_.controllers.back();
    const auto prefix =
        QByteArray{"."} + destination_name_ + QByteArray{".appellate-independent-review-"};
    for (std::size_t attempt = 0; attempt < maximum_staging_attempts; ++attempt) {
        const auto suffix = hooks_.staging_suffix_source ? hooks_.staging_suffix_source(attempt)
                                                         : randomStagingSuffix();
        if (!isValidStagingSuffix(suffix)) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::CannotPublishDestination,
                QStringLiteral("Publisher staging-name source returned an invalid suffix"));
        }
        staging_name_ = prefix + suffix;
        staging_path_ = parent.absolute_path == QStringLiteral("/")
                            ? QStringLiteral("/") + QFile::decodeName(staging_name_)
                            : parent.absolute_path + u'/' + QFile::decodeName(staging_name_);
        if (hooks_.report != nullptr) {
            hooks_.report->staging_path = staging_path_;
        }
        const auto action = publisherEvent(
            hooks_,
            IndependentReviewPublisherObservation{IndependentReviewPublisherEvent::NameCandidate,
                                                  staging_path_, staging_name_, attempt});
        if (failsBefore(action)) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::CannotPublishDestination,
                QStringLiteral("Staging-name selection was interrupted"));
        }
        const auto creation =
            publisherMkdir(parent.descriptor.get(), staging_name_, 0700,
                           IndependentReviewPublisherEvent::StagingCreateAttempted, staging_path_,
                           attempt, hooks_);
        if (!creation.state_change_applied) {
            if (creation.native_error == EEXIST) {
                continue;
            }
            return publicationFailure(
                IndependentReviewPublicationErrorCode::CannotPublishDestination,
                nativeError(QStringLiteral("Cannot create private publication staging"),
                            creation.native_error));
        }
        unretained_staging_creation_ = true;
        plan_.at(0).component = staging_name_;
        auto retained =
            retainCreatedDirectory(0, parent.descriptor.get(), staging_name_, true, creation);
        if (!retained) {
            if (retained.error().code ==
                IndependentReviewPublicationErrorCode::PublicationCleanupFailed) {
                return containmentFailure();
            }
            return cleanupOr(std::move(retained.error()));
        }
        if (failsAfter(action)) {
            return cleanupOr(IndependentReviewPublicationError{
                IndependentReviewPublicationErrorCode::CannotPublishDestination,
                QStringLiteral("Staging-name selection was interrupted"),
            });
        }
        return {};
    }
    return publicationFailure(IndependentReviewPublicationErrorCode::CannotPublishDestination,
                              QStringLiteral("Cannot create staging after 128 name collisions"));
}

std::expected<void, IndependentReviewPublicationError>
PublicationSession::createDirectory(std::size_t declaration_index) {
    const auto& declaration = plan_.at(declaration_index);
    const auto parent_descriptor = parentDescriptor(declaration);
    if (parent_descriptor < 0) {
        return publicationFailure(IndependentReviewPublicationErrorCode::CannotPublishDestination,
                                  QStringLiteral("Cannot create publication member directory"));
    }
    const auto absolute_path = staging_path_ + u'/' + declaration.relative_path;
    const auto creation =
        publisherMkdir(parent_descriptor, declaration.component, declaration.required_mode,
                       IndependentReviewPublisherEvent::DirectoryCreateAttempted, absolute_path,
                       declaration_index, hooks_);
    if (!creation.state_change_applied) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::CannotPublishDestination,
            nativeError(QStringLiteral("Cannot create publication member directory"),
                        creation.native_error));
    }
    return retainCreatedDirectory(declaration_index, parent_descriptor, declaration.component,
                                  false, creation);
}

std::expected<void, IndependentReviewPublicationError>
PublicationSession::createFile(std::size_t declaration_index) {
    const auto& declaration = plan_.at(declaration_index);
    const auto parent_descriptor = parentDescriptor(declaration);
    if (parent_descriptor < 0) {
        return publicationFailure(IndependentReviewPublicationErrorCode::CannotPublishDestination,
                                  QStringLiteral("Publication file parent was not retained"));
    }
    const auto absolute_path = staging_path_ + u'/' + declaration.relative_path;
    auto created =
        publisherCreateFile(parent_descriptor, declaration.component, declaration.required_mode,
                            absolute_path, declaration_index, hooks_);
    if (!created.mutation.state_change_applied) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::CannotPublishDestination,
            nativeError(QStringLiteral("Cannot exclusively create a publication file"),
                        created.mutation.native_error));
    }
    return retainCreatedFile(declaration_index, parent_descriptor, declaration.component,
                             std::move(created.descriptor), created.mutation);
}

std::expected<void, IndependentReviewPublicationError>
PublicationSession::verifyCompleteTree(int root_descriptor, const QByteArray& root_name) const {
    if (ledger_.empty() || !ledger_.at(0).has_value()) {
        return publicationFailure(IndependentReviewPublicationErrorCode::CannotPublishDestination,
                                  QStringLiteral("Publication staging root is not retained"));
    }
    const auto root_path = parent_chain_.absolute_path == QStringLiteral("/")
                               ? QStringLiteral("/") + QFile::decodeName(root_name)
                               : parent_chain_.absolute_path + u'/' + QFile::decodeName(root_name);
    const auto verify_full_pass = [&]() -> std::expected<void, IndependentReviewPublicationError> {
        if (ledger_.at(0)->name_removed ||
            !publisherEntryRebind(parent_chain_.controllers.back().descriptor.get(), root_name,
                                  ledger_.at(0)->retained(), ledger_.at(0)->identity, true,
                                  root_path, 0, hooks_)) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::PublicationCleanupFailed,
                QStringLiteral("Publication root binding is not exact"));
        }
        const auto root_access_acl = probeBoundPublisherAcl(
            parent_chain_.controllers.back().descriptor.get(), root_name, root_descriptor,
            ledger_.at(0)->identity, true, false, root_path, 0, hooks_);
        const auto root_default_acl = probeBoundPublisherAcl(
            parent_chain_.controllers.back().descriptor.get(), root_name, root_descriptor,
            ledger_.at(0)->identity, true, true, root_path, 0, hooks_);
        if (root_access_acl != AclResult::Absent || root_default_acl != AclResult::Absent) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::PublicationCleanupFailed,
                QStringLiteral("Publication root binding or ACL is not exact"));
        }

        std::vector<Descriptor> opened_directories(plan_.size());
        std::vector<int> directory_descriptors(plan_.size(), -1);
        directory_descriptors.at(0) = root_descriptor;
        for (std::size_t directory_index = 0; directory_index < plan_.size(); ++directory_index) {
            if (!plan_.at(directory_index).directory ||
                directory_descriptors.at(directory_index) < 0) {
                continue;
            }
            std::set<QByteArray> expected_names;
            for (std::size_t index = 1; index < plan_.size(); ++index) {
                if (plan_.at(index).parent_index == directory_index) {
                    expected_names.insert(plan_.at(index).component);
                }
            }
            const auto names = directoryNames(
                directory_descriptors.at(directory_index), expected_names.size(),
                IndependentReviewPublicationErrorCode::CannotPublishDestination,
                QStringLiteral("Publication directory inventory exceeds its fixed plan"));
            if (!names || *names != expected_names) {
                return publicationFailure(
                    IndependentReviewPublicationErrorCode::CannotPublishDestination,
                    QStringLiteral("Publication directory inventory is not exact"));
            }
            for (std::size_t index = 1; index < plan_.size(); ++index) {
                const auto& declaration = plan_.at(index);
                if (declaration.parent_index != directory_index || !ledger_.at(index).has_value() ||
                    ledger_.at(index)->name_removed) {
                    continue;
                }
                const auto parent_descriptor = directory_descriptors.at(directory_index);
                const auto member_path = root_path + u'/' + declaration.relative_path;
                if (!publisherEntryRebind(parent_descriptor, declaration.component,
                                          ledger_.at(index)->retained(),
                                          ledger_.at(index)->identity, declaration.directory,
                                          member_path, index, hooks_)) {
                    return publicationFailure(
                        IndependentReviewPublicationErrorCode::CannotPublishDestination,
                        QStringLiteral("Publication member name is no longer bound"));
                }
                if (declaration.directory) {
                    opened_directories.at(index).reset(
                        ::openat(parent_descriptor, declaration.component.constData(),
                                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
                    if (!opened_directories.at(index)) {
                        return publicationFailure(
                            IndependentReviewPublicationErrorCode::CannotPublishDestination,
                            QStringLiteral(
                                "Publication member directory cannot be retained safely"));
                    }
                    const auto access_acl = probeBoundPublisherAcl(
                        parent_descriptor, declaration.component,
                        opened_directories.at(index).get(), ledger_.at(index)->identity, true,
                        false, member_path, index, hooks_);
                    const auto default_acl = probeBoundPublisherAcl(
                        parent_descriptor, declaration.component,
                        opened_directories.at(index).get(), ledger_.at(index)->identity, true, true,
                        member_path, index, hooks_);
                    if (access_acl == AclResult::Unsupported ||
                        default_acl == AclResult::Unsupported) {
                        return publicationFailure(
                            IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform,
                            QStringLiteral(
                                "Publication member directory ACL probing is unsupported"));
                    }
                    if (access_acl != AclResult::Absent || default_acl != AclResult::Absent) {
                        return publicationFailure(
                            IndependentReviewPublicationErrorCode::CannotPublishDestination,
                            QStringLiteral("Publication member directory ACL is not absent"));
                    }
                    directory_descriptors.at(index) = opened_directories.at(index).get();
                } else {
                    Descriptor file(::openat(parent_descriptor, declaration.component.constData(),
                                             O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
                    if (!file) {
                        return publicationFailure(
                            IndependentReviewPublicationErrorCode::CannotPublishDestination,
                            QStringLiteral("Publication file cannot be retained safely"));
                    }
                    const auto access_acl = probeBoundPublisherAcl(
                        parent_descriptor, declaration.component, file.get(),
                        ledger_.at(index)->identity, false, false, member_path, index, hooks_);
                    if (access_acl == AclResult::Unsupported) {
                        return publicationFailure(
                            IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform,
                            QStringLiteral("Publication file ACL probing is unsupported"));
                    }
                    if (access_acl != AclResult::Absent) {
                        return publicationFailure(
                            IndependentReviewPublicationErrorCode::CannotPublishDestination,
                            QStringLiteral("Publication file ACL is not absent"));
                    }
                    auto bytes = readStableDescriptor(file.get(), declaration.bytes->size());
                    if (!bytes || *bytes != *declaration.bytes) {
                        return publicationFailure(
                            IndependentReviewPublicationErrorCode::CannotPublishDestination,
                            QStringLiteral("Publication file bytes changed"));
                    }
                }
            }
        }
        return {};
    };

    if (const auto first_pass = verify_full_pass(); !first_pass) {
        return first_pass;
    }
    if (publisherEvent(hooks_,
                       IndependentReviewPublisherObservation{
                           IndependentReviewPublisherEvent::BeforeFinalTreeBinding, root_path,
                           root_name, 0}) != IndependentReviewPublisherInjectedAction::Continue) {
        return publicationFailure(IndependentReviewPublicationErrorCode::CannotPublishDestination,
                                  QStringLiteral("Publication final tree binding was interrupted"));
    }
    if (const auto second_pass = verify_full_pass(); !second_pass) {
        return second_pass;
    }

    // End the verification on namespace bindings, not on an ACL, inventory, or byte read.  The
    // immediately preceding full pass proves those properties; this closing sweep detects any
    // replacement that landed while they were being read.
    for (std::size_t reverse = plan_.size(); reverse-- > 1;) {
        const auto index = reverse;
        const auto& declaration = plan_.at(index);
        if (!ledger_.at(index).has_value() || ledger_.at(index)->name_removed) {
            continue;
        }
        const auto parent_descriptor = parentDescriptor(declaration);
        const auto member_path = root_path + u'/' + declaration.relative_path;
        if (parent_descriptor < 0 ||
            !publisherEntryRebind(parent_descriptor, declaration.component,
                                  ledger_.at(index)->retained(), ledger_.at(index)->identity,
                                  declaration.directory, member_path, index, hooks_)) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::CannotPublishDestination,
                QStringLiteral("Publication final member binding is not exact"));
        }
    }
    // Bind the visible root last.  All child checks use retained original directory descriptors, so
    // this final name check detects a coherent whole-root exchange at any member-binding seam.
    if (!publisherEntryRebind(parent_chain_.controllers.back().descriptor.get(), root_name,
                              ledger_.at(0)->retained(), ledger_.at(0)->identity, true, root_path,
                              0, hooks_)) {
        return publicationFailure(IndependentReviewPublicationErrorCode::PublicationCleanupFailed,
                                  QStringLiteral("Publication root binding is not exact"));
    }
    return {};
}

std::expected<void, IndependentReviewPublicationError>
PublicationSession::verifyPartialStagingTree() const {
    if (ledger_.empty() || !ledger_.at(0).has_value()) {
        return publicationFailure(IndependentReviewPublicationErrorCode::PublicationCleanupFailed,
                                  QStringLiteral("Publication staging root was not retained"));
    }
    const auto parent_descriptor = parent_chain_.controllers.back().descriptor.get();
    if (ledger_.at(0)->name_removed || ledger_.at(0)->identity.type != S_IFDIR ||
        ledger_.at(0)->identity.owner != ::geteuid() || ledger_.at(0)->identity.mode != 0700 ||
        !publisherEntryRebind(parent_descriptor, staging_name_, ledger_.at(0)->retained(),
                              ledger_.at(0)->identity, true, staging_path_, 0, hooks_)) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::PublicationCleanupFailed,
            QStringLiteral("Publication staging root binding is no longer exact"));
    }
    Descriptor root(::openat(parent_descriptor, staging_name_.constData(),
                             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!root ||
        probeBoundPublisherAcl(parent_descriptor, staging_name_, root.get(),
                               ledger_.at(0)->identity, true, false, staging_path_, 0,
                               hooks_) != AclResult::Absent ||
        probeBoundPublisherAcl(parent_descriptor, staging_name_, root.get(),
                               ledger_.at(0)->identity, true, true, staging_path_, 0,
                               hooks_) != AclResult::Absent) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::PublicationCleanupFailed,
            QStringLiteral("Publication staging root policy is no longer exact"));
    }
    std::vector<Descriptor> opened_directories(plan_.size());
    std::vector<int> directory_descriptors(plan_.size(), -1);
    directory_descriptors.at(0) = root.get();
    for (std::size_t directory_index = 0; directory_index < plan_.size(); ++directory_index) {
        if (!plan_.at(directory_index).directory || !ledger_.at(directory_index).has_value() ||
            ledger_.at(directory_index)->name_removed ||
            directory_descriptors.at(directory_index) < 0) {
            continue;
        }
        std::set<QByteArray> expected_names;
        for (std::size_t index = 1; index < plan_.size(); ++index) {
            if (plan_.at(index).parent_index == directory_index && ledger_.at(index).has_value() &&
                !ledger_.at(index)->name_removed) {
                expected_names.insert(plan_.at(index).component);
            }
        }
        const auto names = directoryNames(
            directory_descriptors.at(directory_index), expected_names.size(),
            IndependentReviewPublicationErrorCode::PublicationCleanupFailed,
            QStringLiteral("Publication cleanup inventory exceeds its creation ledger"));
        if (!names || *names != expected_names) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::PublicationCleanupFailed,
                QStringLiteral("Publication cleanup inventory is not exact"));
        }
        for (std::size_t index = 1; index < plan_.size(); ++index) {
            const auto& declaration = plan_.at(index);
            if (declaration.parent_index != directory_index || !ledger_.at(index).has_value() ||
                ledger_.at(index)->name_removed) {
                continue;
            }
            const auto current_parent = directory_descriptors.at(directory_index);
            const auto member_path = staging_path_ + u'/' + declaration.relative_path;
            if (!publisherEntryRebind(current_parent, declaration.component,
                                      ledger_.at(index)->retained(), ledger_.at(index)->identity,
                                      declaration.directory, member_path, index, hooks_)) {
                return publicationFailure(
                    IndependentReviewPublicationErrorCode::PublicationCleanupFailed,
                    QStringLiteral("Publication cleanup member is missing"));
            }
            if (declaration.directory) {
                opened_directories.at(index).reset(
                    ::openat(current_parent, declaration.component.constData(),
                             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
                if (!opened_directories.at(index)) {
                    return publicationFailure(
                        IndependentReviewPublicationErrorCode::PublicationCleanupFailed,
                        QStringLiteral("Publication cleanup directory cannot be retained"));
                }
                directory_descriptors.at(index) = opened_directories.at(index).get();
            }
        }
    }
    return {};
}

BoundTreeState PublicationSession::treeStateAt(const QByteArray& name) const {
    const auto parent_descriptor = parent_chain_.controllers.back().descriptor.get();
    if (!ledger_.at(0).has_value()) {
        return BoundTreeState::Other;
    }
    const auto root_path = parent_chain_.absolute_path == QStringLiteral("/")
                               ? QStringLiteral("/") + QFile::decodeName(name)
                               : parent_chain_.absolute_path + u'/' + QFile::decodeName(name);
    switch (publisherEntryState(parent_descriptor, name, ledger_.at(0)->retained(),
                                ledger_.at(0)->identity, true, root_path, 0, hooks_)) {
    case PublisherBindingState::Absent:
        return BoundTreeState::Absent;
    case PublisherBindingState::Other:
        return BoundTreeState::Other;
    case PublisherBindingState::Ambiguous:
        return BoundTreeState::Ambiguous;
    case PublisherBindingState::Exact:
        break;
    }
    // The retained root descriptor remains authoritative across the one real rename.  Rebinding
    // the selected name before and throughout verification is sufficient; reopening the name here
    // would add an uninjectable ambient-stat window and would not strengthen the retained binding.
    return verifyCompleteTree(ledger_.at(0)->usable(), name).has_value() ? BoundTreeState::Exact
                                                                         : BoundTreeState::Other;
}

std::expected<void, IndependentReviewPublicationError>
PublicationSession::validateAndSyncStaging() {
    if (const auto controller = validateControllerChain(parent_chain_, hooks_); !controller) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::PublicationCleanupFailed,
            telemetry(QStringLiteral("unknown"), QStringLiteral("unknown"), parent_sync_));
    }
    if (const auto tree = verifyCompleteTree(ledger_.at(0)->usable(), staging_name_); !tree) {
        return std::unexpected(tree.error());
    }
    if (publisherEvent(
            hooks_,
            IndependentReviewPublisherObservation{
                IndependentReviewPublisherEvent::InventoryChecked, staging_path_, {}, 0}) !=
        IndependentReviewPublisherInjectedAction::Continue) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::CannotPublishDestination,
            QStringLiteral("Publication inventory verification was interrupted"));
    }
    const auto proc_root =
        QStringLiteral("/proc/self/fd/%1/").arg(parent_chain_.controllers.back().descriptor.get()) +
        QFile::decodeName(staging_name_);
    for (std::size_t pass = 0; pass < 2; ++pass) {
        const auto before = publisherEvent(
            hooks_,
            IndependentReviewPublisherObservation{
                IndependentReviewPublisherEvent::BeforeStagedValidation, proc_root, {}, pass});
        if (failsBefore(before)) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::CannotPublishDestination,
                QStringLiteral("Staged validation was interrupted"));
        }
        const auto validated = request_.validate_staged(proc_root);
        const auto after = publisherEvent(
            hooks_,
            IndependentReviewPublisherObservation{
                IndependentReviewPublisherEvent::AfterStagedValidation, proc_root, {}, pass});
        if (!validated) {
            if (validated.error().code ==
                IndependentReviewStagedValidationErrorCode::InvalidArtifact) {
                if (const auto unchanged =
                        verifyCompleteTree(ledger_.at(0)->usable(), staging_name_);
                    !unchanged) {
                    return std::unexpected(unchanged.error());
                }
            }
            const auto code = validated.error().code ==
                                      IndependentReviewStagedValidationErrorCode::InvalidArtifact
                                  ? IndependentReviewPublicationErrorCode::InvalidStagedArtifact
                                  : IndependentReviewPublicationErrorCode::CannotPublishDestination;
            return publicationFailure(code, validated.error().message);
        }
        if (failsAfter(before) || after != IndependentReviewPublisherInjectedAction::Continue) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::CannotPublishDestination,
                QStringLiteral("Staged validation was interrupted"));
        }
        if (const auto tree = verifyCompleteTree(ledger_.at(0)->usable(), staging_name_); !tree) {
            return std::unexpected(tree.error());
        }
    }
    for (std::size_t index = 1; index < plan_.size(); ++index) {
        if (plan_.at(index).directory && ledger_.at(index).has_value() &&
            !ledger_.at(index)->name_removed &&
            !publisherSync(ledger_.at(index)->usable(),
                           IndependentReviewPublisherEvent::DirectorySync,
                           staging_path_ + u'/' + plan_.at(index).relative_path,
                           plan_.at(index).component, index, hooks_)) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::CannotPublishDestination,
                QStringLiteral("Cannot synchronize staged member directory"));
        }
    }
    if (!publisherSync(ledger_.at(0)->usable(), IndependentReviewPublisherEvent::DirectorySync,
                       staging_path_, staging_name_, 0, hooks_) ||
        !publisherSync(parent_chain_.controllers.back().descriptor.get(),
                       IndependentReviewPublisherEvent::DirectorySync, parent_chain_.absolute_path,
                       {}, 0, hooks_)) {
        return publicationFailure(IndependentReviewPublicationErrorCode::CannotPublishDestination,
                                  QStringLiteral("Cannot synchronize complete staging tree"));
    }
    return {};
}

ParentSyncState PublicationSession::bestEffortParentSync() {
    const auto synchronized = publisherSync(parent_chain_.controllers.back().descriptor.get(),
                                            IndependentReviewPublisherEvent::ParentSync,
                                            parent_chain_.absolute_path, {}, 0, hooks_);
    parent_sync_ = synchronized ? ParentSyncState::Ok : ParentSyncState::Failed;
    if (hooks_.report != nullptr) {
        hooks_.report->retained_creations_verified_at_completion =
            retainedCreationDescriptorsExact();
    }
    return parent_sync_;
}

bool PublicationSession::retainedCreationDescriptorsExact() const {
    for (std::size_t index = 0; index < ledger_.size(); ++index) {
        if (!ledger_.at(index).has_value()) {
            continue;
        }
        const auto& entry = *ledger_.at(index);
        struct stat retained_status{};
        if (!statDescriptor(entry.retained(), &retained_status)) {
            return false;
        }
        const auto retained_identity = identityOf(retained_status);
        if (plan_.at(index).directory ? !sameDirectoryIdentity(entry.identity, retained_identity)
                                      : !sameFileIdentity(entry.identity, retained_identity)) {
            return false;
        }
        if (entry.usable_descriptor) {
            struct stat usable_status{};
            if (!statDescriptor(entry.usable(), &usable_status) ||
                !sameDirectoryIdentity(entry.identity, identityOf(usable_status))) {
                return false;
            }
        }
    }
    return true;
}

void PublicationSession::updateReportRemaining() const {
    if (hooks_.report == nullptr) {
        return;
    }
    hooks_.report->remaining_ledger_paths.clear();
    for (std::size_t index = 0; index < ledger_.size(); ++index) {
        if (!ledger_.at(index).has_value() || ledger_.at(index)->name_removed) {
            continue;
        }
        hooks_.report->remaining_ledger_paths.push_back(
            index == 0 ? staging_path_ : staging_path_ + u'/' + plan_.at(index).relative_path);
    }
}

QString PublicationSession::cleanupTelemetry() const {
    if (unretained_staging_creation_ || creation_identities_.empty() ||
        !creation_identities_.at(0).has_value() ||
        !validateControllerChain(parent_chain_, hooks_)) {
        return telemetry(QStringLiteral("unknown"), QStringLiteral("unknown"), parent_sync_);
    }

    std::vector<PublisherBindingState> states(plan_.size(), PublisherBindingState::Ambiguous);
    std::size_t exact_count = 0;
    std::size_t absent_count = 0;
    const auto root_descriptor = ledger_.at(0).has_value() ? ledger_.at(0)->retained() : -1;
    const auto staging_state = publisherEntryState(
        parent_chain_.controllers.back().descriptor.get(), staging_name_, root_descriptor,
        *creation_identities_.at(0), true, staging_path_, 0, hooks_);
    const auto destination_state = publisherEntryState(
        parent_chain_.controllers.back().descriptor.get(), destination_name_, root_descriptor,
        *creation_identities_.at(0), true, destination_path_, 0, hooks_);
    if (staging_state == PublisherBindingState::Absent &&
        destination_state == PublisherBindingState::Absent) {
        return telemetry(QStringLiteral("unreachable"), QStringLiteral("absent"), parent_sync_);
    }
    const auto staging_is_root = staging_state == PublisherBindingState::Exact &&
                                 destination_state == PublisherBindingState::Absent;
    const auto destination_is_root = destination_state == PublisherBindingState::Exact &&
                                     staging_state == PublisherBindingState::Absent;
    if (!staging_is_root && !destination_is_root) {
        const auto residue = staging_state == PublisherBindingState::Exact ||
                                     destination_state == PublisherBindingState::Exact
                                 ? QStringLiteral("present")
                                 : QStringLiteral("unknown");
        return telemetry(QStringLiteral("unknown"), residue, parent_sync_);
    }
    states.at(0) = PublisherBindingState::Exact;
    const auto& active_root_path = staging_is_root ? staging_path_ : destination_path_;
    const auto& active_root_name = staging_is_root ? staging_name_ : destination_name_;
    ++exact_count;

    for (std::size_t index = 1; index < plan_.size(); ++index) {
        if (!creation_identities_.at(index).has_value()) {
            continue;
        }
        if (!plan_.at(index).parent_index.has_value()) {
            states.at(index) = PublisherBindingState::Ambiguous;
            continue;
        }
        const auto parent_index = *plan_.at(index).parent_index;
        if (states.at(parent_index) == PublisherBindingState::Absent) {
            states.at(index) = PublisherBindingState::Absent;
            ++absent_count;
            continue;
        }
        if (states.at(parent_index) != PublisherBindingState::Exact) {
            continue;
        }
        const auto parent_descriptor = parentDescriptor(plan_.at(index));
        if (parent_descriptor < 0) {
            continue;
        }
        const auto retained_descriptor =
            ledger_.at(index).has_value() ? ledger_.at(index)->retained() : -1;
        states.at(index) = publisherEntryState(
            parent_descriptor, plan_.at(index).component, retained_descriptor,
            *creation_identities_.at(index), plan_.at(index).directory,
            active_root_path + u'/' + plan_.at(index).relative_path, index, hooks_);
        if (states.at(index) == PublisherBindingState::Exact) {
            ++exact_count;
        } else if (states.at(index) == PublisherBindingState::Absent) {
            ++absent_count;
        }
    }
    const auto complete =
        exact_count == created_count_ && root_descriptor >= 0 &&
        (staging_is_root
             ? verifyPartialStagingTree().has_value()
             : verifyCompleteTree(ledger_.at(0)->usable(), active_root_name).has_value());
    if (complete) {
        return telemetry(QStringLiteral("reachable"), QStringLiteral("present"), parent_sync_);
    }
    if (exact_count > 0) {
        return telemetry(QStringLiteral("unknown"), QStringLiteral("present"), parent_sync_);
    }
    if (absent_count == created_count_) {
        return telemetry(QStringLiteral("unreachable"), QStringLiteral("absent"), parent_sync_);
    }
    return telemetry(QStringLiteral("unknown"), QStringLiteral("unknown"), parent_sync_);
}

std::expected<void, IndependentReviewPublicationError> PublicationSession::cleanup() {
    if (publisherEvent(
            hooks_,
            IndependentReviewPublisherObservation{
                IndependentReviewPublisherEvent::CleanupInspected, staging_path_, {}, 0}) !=
        IndependentReviewPublisherInjectedAction::Continue) {
        return publicationFailure(IndependentReviewPublicationErrorCode::PublicationCleanupFailed,
                                  QStringLiteral("Publication cleanup inspection was interrupted"));
    }
    if (const auto controller = validateControllerChain(parent_chain_, hooks_); !controller) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::PublicationCleanupFailed,
            QStringLiteral("Cannot re-prove destination controllers for cleanup"));
    }
    if (const auto exact = verifyPartialStagingTree(); !exact) {
        return exact;
    }
    for (std::size_t reverse = plan_.size(); reverse-- > 1;) {
        if (!ledger_.at(reverse).has_value() || ledger_.at(reverse)->name_removed) {
            continue;
        }
        const auto& declaration = plan_.at(reverse);
        const auto parent_descriptor = parentDescriptor(declaration);
        const auto member_path = staging_path_ + u'/' + declaration.relative_path;
        if (parent_descriptor < 0 ||
            !publisherEntryRebind(parent_descriptor, declaration.component,
                                  ledger_.at(reverse)->retained(), ledger_.at(reverse)->identity,
                                  declaration.directory, member_path, reverse, hooks_)) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::PublicationCleanupFailed,
                QStringLiteral("Cannot bind publication entry before cleanup"));
        }
        if (!publisherRemove(
                parent_descriptor, declaration.component, ledger_.at(reverse)->retained(),
                ledger_.at(reverse)->identity, declaration.directory, member_path, reverse,
                [this] {
                    return validateControllerChain(parent_chain_, hooks_).has_value() &&
                           verifyPartialStagingTree().has_value();
                },
                hooks_)) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::PublicationCleanupFailed,
                QStringLiteral("Cannot safely remove publication entry"));
        }
        ledger_.at(reverse)->name_removed = true;
        const auto parent_path = declaration.parent_index == std::size_t{0}
                                     ? staging_path_
                                     : staging_path_ + QStringLiteral("/resources");
        if (!publisherSync(parent_descriptor, IndependentReviewPublisherEvent::DirectorySync,
                           parent_path, {}, reverse, hooks_)) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::PublicationCleanupFailed,
                QStringLiteral("Cannot synchronize publication cleanup"));
        }
    }
    const auto parent_descriptor = parent_chain_.controllers.back().descriptor.get();
    if (!publisherEntryRebind(parent_descriptor, staging_name_, ledger_.at(0)->retained(),
                              ledger_.at(0)->identity, true, staging_path_, 0, hooks_) ||
        !publisherRemove(
            parent_descriptor, staging_name_, ledger_.at(0)->retained(), ledger_.at(0)->identity,
            true, staging_path_, 0,
            [this] {
                return validateControllerChain(parent_chain_, hooks_).has_value() &&
                       verifyPartialStagingTree().has_value();
            },
            hooks_)) {
        return publicationFailure(IndependentReviewPublicationErrorCode::PublicationCleanupFailed,
                                  QStringLiteral("Cannot safely remove publication staging root"));
    }
    ledger_.at(0)->name_removed = true;
    unretained_staging_creation_ = false;
    parent_sync_ = publisherSync(parent_descriptor, IndependentReviewPublisherEvent::DirectorySync,
                                 parent_chain_.absolute_path, {}, 0, hooks_)
                       ? ParentSyncState::Ok
                       : ParentSyncState::Failed;
    if (hooks_.report != nullptr) {
        hooks_.report->retained_creations_verified_at_completion =
            retainedCreationDescriptorsExact();
    }
    const auto cleanup_synced = publisherEvent(
        hooks_, IndependentReviewPublisherObservation{
                    IndependentReviewPublisherEvent::CleanupSynced, staging_path_, {}, 0});
    if (parent_sync_ == ParentSyncState::Failed) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::PublicationCleanupFailed,
            QStringLiteral("Cannot synchronize publication parent after cleanup"));
    }
    if (cleanup_synced != IndependentReviewPublisherInjectedAction::Continue) {
        return publicationFailure(
            IndependentReviewPublicationErrorCode::PublicationCleanupFailed,
            QStringLiteral("Publication cleanup synchronization was interrupted"));
    }
    updateReportRemaining();
    return {};
}

std::expected<void, IndependentReviewPublicationError>
PublicationSession::cleanupOr(IndependentReviewPublicationError original) {
    if (const auto cleaned = cleanup(); cleaned) {
        return std::unexpected(std::move(original));
    }
    updateReportRemaining();
    return publicationFailure(IndependentReviewPublicationErrorCode::PublicationCleanupFailed,
                              cleanupTelemetry());
}

std::expected<void, IndependentReviewPublicationError> PublicationSession::containmentFailure() {
    updateReportRemaining();
    return publicationFailure(IndependentReviewPublicationErrorCode::PublicationCleanupFailed,
                              cleanupTelemetry());
}

std::expected<void, IndependentReviewPublicationError> PublicationSession::run() {
    if (const auto ready = preflight(); !ready) {
        return ready;
    }
    if (const auto staging = createStagingRoot(); !staging) {
        return staging;
    }
    const auto fail_before_rename = [this](IndependentReviewPublicationError error)
        -> std::expected<void, IndependentReviewPublicationError> {
        if (error.code == IndependentReviewPublicationErrorCode::PublicationCleanupFailed) {
            return containmentFailure();
        }
        return cleanupOr(std::move(error));
    };

    for (std::size_t declaration_index = 1; declaration_index < plan_.size(); ++declaration_index) {
        auto created = plan_.at(declaration_index).directory ? createDirectory(declaration_index)
                                                             : createFile(declaration_index);
        if (!created) {
            return fail_before_rename(std::move(created.error()));
        }
    }
    if (const auto validated = validateAndSyncStaging(); !validated) {
        return fail_before_rename(validated.error());
    }

    const auto before_rename = IndependentReviewPublisherObservation{
        IndependentReviewPublisherEvent::BeforeRename, destination_path_, destination_name_, 0};
    const auto before_action = publisherEvent(hooks_, before_rename);
    if (before_action != IndependentReviewPublisherInjectedAction::Continue) {
        return fail_before_rename(IndependentReviewPublicationError{
            IndependentReviewPublicationErrorCode::CannotPublishDestination,
            QStringLiteral("Publication was interrupted before rename"),
        });
    }
    if (const auto controllers = validateControllerChain(parent_chain_, hooks_); !controllers) {
        return containmentFailure();
    }
    if (const auto tree = verifyCompleteTree(ledger_.at(0)->usable(), staging_name_); !tree) {
        return fail_before_rename(tree.error());
    }
    if (treeStateAt(destination_name_) != BoundTreeState::Absent) {
        return fail_before_rename(IndependentReviewPublicationError{
            IndependentReviewPublicationErrorCode::CannotPublishDestination,
            QStringLiteral("Publication destination changed before rename"),
        });
    }

    const auto rename_observation = IndependentReviewPublisherObservation{
        IndependentReviewPublisherEvent::RenameAttempted, destination_path_, destination_name_, 0};
    const auto rename_action = publisherEvent(hooks_, rename_observation);
    long rename_result = -1;
    int rename_error = EACCES;
    if (!failsBefore(rename_action)) {
        const auto injected = hooks_.outcome ? hooks_.outcome(rename_observation) : std::nullopt;
        if (injected.has_value()) {
            if (injected->state_change_applied || injected->operation_succeeded) {
#if defined(SYS_renameat2)
                errno = 0;
                const auto applied = ::syscall(
                    SYS_renameat2, parent_chain_.controllers.back().descriptor.get(),
                    staging_name_.constData(), parent_chain_.controllers.back().descriptor.get(),
                    destination_name_.constData(), 1U);
                if (applied != 0) {
                    rename_result = -1;
                    rename_error = errno;
                } else {
                    rename_result = injected->operation_succeeded ? 0 : -1;
                    rename_error = injected->native_error != 0 ? injected->native_error : EIO;
                }
#else
                rename_result = -1;
                rename_error = ENOSYS;
#endif
            } else {
                rename_result = injected->operation_succeeded ? 0 : -1;
                rename_error = injected->native_error != 0 ? injected->native_error : EIO;
            }
        } else {
#if defined(SYS_renameat2)
            errno = 0;
            rename_result = ::syscall(
                SYS_renameat2, parent_chain_.controllers.back().descriptor.get(),
                staging_name_.constData(), parent_chain_.controllers.back().descriptor.get(),
                destination_name_.constData(), 1U);
            rename_error = rename_result == 0 ? 0 : errno;
#else
            rename_result = -1;
            rename_error = ENOSYS;
#endif
        }
    }
    if (failsAfter(rename_action)) {
        rename_result = -1;
        rename_error = EIO;
    }

    const auto returned_observation = IndependentReviewPublisherObservation{
        IndependentReviewPublisherEvent::RenameReturned, destination_path_, destination_name_,
        static_cast<std::size_t>(rename_error)};
    const auto returned_action = publisherEvent(hooks_, returned_observation);
    if (returned_action != IndependentReviewPublisherInjectedAction::Continue) {
        rename_result = -1;
        if (rename_error == 0) {
            rename_error = EIO;
        }
    }

    // The reconciliation event is deliberately emitted before the authoritative reads so tests can
    // deterministically exercise the final stat window without granting cleanup authority.
    const auto reconciled_action =
        publisherEvent(hooks_, IndependentReviewPublisherObservation{
                                   IndependentReviewPublisherEvent::Reconciled, destination_path_,
                                   destination_name_, static_cast<std::size_t>(rename_error)});
    const auto controllers_exact = validateControllerChain(parent_chain_, hooks_).has_value();
    const auto source_state = treeStateAt(staging_name_);
    const auto destination_state = treeStateAt(destination_name_);

    const auto identity_failure =
        [this]() -> std::expected<void, IndependentReviewPublicationError> {
        static_cast<void>(bestEffortParentSync());
        updateReportRemaining();
        return publicationFailure(
            IndependentReviewPublicationErrorCode::PublicationIdentityFailed,
            telemetry(QStringLiteral("unknown"), QStringLiteral("unknown"), parent_sync_));
    };
    if (!controllers_exact ||
        reconciled_action != IndependentReviewPublisherInjectedAction::Continue) {
        return identity_failure();
    }

    if (source_state == BoundTreeState::Exact && destination_state == BoundTreeState::Absent) {
        if (rename_result == 0) {
            return identity_failure();
        }
        const auto code =
            rename_error == ENOSYS || rename_error == EINVAL || rename_error == EOPNOTSUPP
                ? IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform
                : IndependentReviewPublicationErrorCode::CannotPublishDestination;
        return cleanupOr(IndependentReviewPublicationError{
            code, nativeError(QStringLiteral("Publication rename failed"), rename_error)});
    }

    if (source_state == BoundTreeState::Exact && destination_state == BoundTreeState::Other) {
        return cleanupOr(IndependentReviewPublicationError{
            IndependentReviewPublicationErrorCode::CannotPublishDestination,
            QStringLiteral("Publication destination collided with an unrelated object"),
        });
    }

    if (source_state == BoundTreeState::Absent && destination_state == BoundTreeState::Exact) {
        static_cast<void>(bestEffortParentSync());
        updateReportRemaining();
        if (rename_result != 0) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::PublicationOutcomeUncertain,
                telemetry(QStringLiteral("reachable"), QStringLiteral("absent"), parent_sync_));
        }
        if (parent_sync_ == ParentSyncState::Failed) {
            return publicationFailure(
                IndependentReviewPublicationErrorCode::PublicationDurabilityFailed,
                telemetry(QStringLiteral("reachable"), QStringLiteral("absent"), parent_sync_));
        }
        for (auto& entry : ledger_) {
            if (entry.has_value()) {
                entry->name_removed = true;
            }
        }
        updateReportRemaining();
        return {};
    }

    return identity_failure();
}

} // namespace
#endif

std::expected<void, IndependentReviewPublicationError>
publishIndependentReviewArtifacts(const IndependentReviewPublicationRequest& request,
                                  const IndependentReviewPublisherHooks& hooks) {
#if defined(Q_OS_LINUX)
    if (hooks.report != nullptr) {
        *hooks.report = IndependentReviewPublisherReport{};
    }
    return PublicationSession(request, hooks).run();
#else
    static_cast<void>(request);
    static_cast<void>(hooks);
    return publicationFailure(IndependentReviewPublicationErrorCode::UnsupportedAuthoringPlatform,
                              QStringLiteral("Independent-review publication requires Linux"));
#endif
}

} // namespace appellate::cli::detail
