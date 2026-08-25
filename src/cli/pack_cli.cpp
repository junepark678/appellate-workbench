#include "pack_cli.hpp"

#include "appellate/model/pack_id.hpp"
#include "appellate/packs/error.hpp"
#include "appellate/packs/pack_archive.hpp"
#include "appellate/packs/pack_catalog.hpp"
#include "appellate/packs/pack_reader.hpp"
#include "appellate/packs/realism_evidence_authoring.hpp"
#include "appellate/packs/schema_validator.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>

#include <array>
#include <cerrno>
#include <cstring>
#include <expected>
#include <optional>
#include <ranges>
#include <string>
#include <utility>

#if defined(Q_OS_UNIX)
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

static void initializeAppellatePackTemplateResources() {
    static const bool initialized = [] {
        Q_INIT_RESOURCE(templates);
        return true;
    }();
    static_cast<void>(initialized);
}

namespace appellate::cli {
namespace {

constexpr auto output_schema_version = 1;

[[nodiscard]] QString fromUtf8(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

[[nodiscard]] QByteArray encoded(QJsonObject object) {
    object.insert(QStringLiteral("schema_version"), output_schema_version);
    return QJsonDocument(std::move(object)).toJson(QJsonDocument::Compact) + '\n';
}

[[nodiscard]] RunResult success(QJsonObject object) {
    object.insert(QStringLiteral("status"), QStringLiteral("ok"));
    return RunResult{static_cast<int>(ExitCode::Success), encoded(std::move(object)), {}};
}

[[nodiscard]] RunResult failure(ExitCode exit_code, const QString& code, const QString& message,
                                const QString& command = {}) {
    QJsonObject object{
        {QStringLiteral("code"), code},
        {QStringLiteral("message"), message},
        {QStringLiteral("status"), QStringLiteral("error")},
    };
    if (!command.isEmpty()) {
        object.insert(QStringLiteral("command"), command);
    }
    return RunResult{static_cast<int>(exit_code), {}, encoded(std::move(object))};
}

[[nodiscard]] QString packErrorCode(packs::ErrorCode code) {
    switch (code) {
    case packs::ErrorCode::CannotRead:
        return QStringLiteral("cannot_read");
    case packs::ErrorCode::InvalidJson:
        return QStringLiteral("invalid_json");
    case packs::ErrorCode::InvalidManifest:
        return QStringLiteral("invalid_manifest");
    case packs::ErrorCode::UnsupportedSchema:
        return QStringLiteral("unsupported_schema");
    case packs::ErrorCode::UnsafePath:
        return QStringLiteral("unsafe_path");
    case packs::ErrorCode::DuplicateContentId:
        return QStringLiteral("duplicate_content_id");
    case packs::ErrorCode::DuplicateContentPath:
        return QStringLiteral("duplicate_content_path");
    case packs::ErrorCode::DuplicatePayloadId:
        return QStringLiteral("duplicate_payload_id");
    case packs::ErrorCode::UndeclaredFile:
        return QStringLiteral("undeclared_file");
    case packs::ErrorCode::ResourceTooLarge:
        return QStringLiteral("resource_too_large");
    case packs::ErrorCode::DigestMismatch:
        return QStringLiteral("digest_mismatch");
    case packs::ErrorCode::InvalidJudgeProfile:
        return QStringLiteral("invalid_judge_profile");
    case packs::ErrorCode::DuplicateJsonKey:
        return QStringLiteral("duplicate_json_key");
    case packs::ErrorCode::SchemaViolation:
        return QStringLiteral("schema_violation");
    case packs::ErrorCode::UnsupportedResourceKind:
        return QStringLiteral("unsupported_resource_kind");
    case packs::ErrorCode::UnsupportedCapability:
        return QStringLiteral("unsupported_capability");
    case packs::ErrorCode::CrossReferenceFailure:
        return QStringLiteral("cross_reference_failure");
    }
    return QStringLiteral("invalid_pack");
}

[[nodiscard]] QString catalogErrorCode(packs::CatalogErrorCode code) {
    switch (code) {
    case packs::CatalogErrorCode::InvalidConfiguration:
        return QStringLiteral("invalid_configuration");
    case packs::CatalogErrorCode::CannotOpen:
        return QStringLiteral("cannot_open_catalog");
    case packs::CatalogErrorCode::CatalogBusy:
        return QStringLiteral("catalog_busy");
    case packs::CatalogErrorCode::UninitializedCatalog:
        return QStringLiteral("uninitialized_catalog");
    case packs::CatalogErrorCode::MigrationFailed:
        return QStringLiteral("catalog_migration_failed");
    case packs::CatalogErrorCode::ArchiveInvalid:
        return QStringLiteral("archive_invalid");
    case packs::CatalogErrorCode::ImmutableConflict:
        return QStringLiteral("immutable_conflict");
    case packs::CatalogErrorCode::MissingDependency:
        return QStringLiteral("missing_dependency");
    case packs::CatalogErrorCode::DependencyCycle:
        return QStringLiteral("dependency_cycle");
    case packs::CatalogErrorCode::DependencyVersionSplit:
        return QStringLiteral("dependency_version_split");
    case packs::CatalogErrorCode::DependencyClosureTooLarge:
        return QStringLiteral("dependency_closure_too_large");
    case packs::CatalogErrorCode::UnsupportedCapability:
        return QStringLiteral("unsupported_capability");
    case packs::CatalogErrorCode::ResourceCollision:
        return QStringLiteral("resource_collision");
    case packs::CatalogErrorCode::InvalidResolvedGraph:
        return QStringLiteral("invalid_resolved_graph");
    case packs::CatalogErrorCode::CorruptCatalog:
        return QStringLiteral("corrupt_catalog");
    case packs::CatalogErrorCode::NotFound:
        return QStringLiteral("not_found");
    case packs::CatalogErrorCode::QueryFailed:
        return QStringLiteral("catalog_query_failed");
    case packs::CatalogErrorCode::CannotStoreArchive:
        return QStringLiteral("cannot_store_archive");
    case packs::CatalogErrorCode::CannotStoreBlob:
        return QStringLiteral("cannot_store_blob");
    }
    return QStringLiteral("catalog_error");
}

[[nodiscard]] QJsonObject revisionObject(const model::PackRevision& revision) {
    return QJsonObject{
        {QStringLiteral("digest"), fromUtf8(revision.digest)},
        {QStringLiteral("pack_id"), fromUtf8(revision.id.value)},
        {QStringLiteral("version"), fromUtf8(revision.version)},
    };
}

[[nodiscard]] QJsonObject installedObject(const packs::InstalledPack& installed) {
    QJsonArray dependencies;
    for (const auto& dependency : installed.dependencies) {
        dependencies.push_back(revisionObject(dependency.revision));
    }
    auto object = revisionObject(installed.revision);
    object.insert(QStringLiteral("archive_sha256"), installed.archive_sha256);
    object.insert(QStringLiteral("dependencies"), dependencies);
    object.insert(QStringLiteral("installed_at_utc"), installed.installed_at_utc);
    return object;
}

[[nodiscard]] QString usageMessage() {
    return QStringLiteral(
        "usage: appellate-pack validate <directory-or-awpack> | export <directory> "
        "<archive> | export-deferred <directory> <archive> | validate-resolved <catalog> "
        "<pack-id> <version> <digest> | install <archive> <catalog> "
        "[--installed-at <canonical UTC>] | list <catalog> | template <new-destination> | "
        "author-realism-evidence <directory> <catalog> <review-resource-id> <trace-json> | "
        "author-realism-evidence-multi <directory> <catalog> <review-resource-id> "
        "<trace-set-json>");
}

[[nodiscard]] bool isCanonicalUtc(const QString& value) {
    if (value.size() != 20 || !value.endsWith(u'Z')) {
        return false;
    }
    const auto parsed = QDateTime::fromString(value, Qt::ISODate);
    return parsed.isValid() && parsed.offsetFromUtc() == 0 &&
           parsed.toUTC().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'")) == value;
}

[[nodiscard]] QString currentCanonicalUtc() {
    return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'"));
}

[[nodiscard]] RunResult invalidArguments(const QString& message, const QString& command = {}) {
    return failure(ExitCode::InvalidArguments, QStringLiteral("invalid_arguments"),
                   message + QStringLiteral("; ") + usageMessage(), command);
}

[[nodiscard]] RunResult validateCommand(const QStringList& arguments) {
    constexpr auto command = "validate";
    if (arguments.size() != 2) {
        return invalidArguments(QStringLiteral("validate requires exactly one source"),
                                QLatin1StringView(command));
    }
    const auto source = arguments.at(1);
    const auto is_directory = QFileInfo(source).isDir();
    const auto loaded = is_directory ? packs::PackReader::readDirectory(source)
                                     : packs::PackArchive::importArchive(source);
    if (!loaded) {
        return failure(ExitCode::InvalidPack, packErrorCode(loaded.error().code),
                       loaded.error().message, QLatin1StringView(command));
    }
    auto object = revisionObject(loaded->revision);
    object.insert(QStringLiteral("command"), QLatin1StringView(command));
    object.insert(QStringLiteral("blob_count"), static_cast<qint64>(loaded->blobs.size()));
    object.insert(QStringLiteral("resource_count"), static_cast<qint64>(loaded->resources.size()));
    object.insert(QStringLiteral("source_kind"),
                  is_directory ? QStringLiteral("directory") : QStringLiteral("archive"));
    object.insert(QStringLiteral("validation_scope"), QStringLiteral("standalone"));
    return success(std::move(object));
}

[[nodiscard]] RunResult exportCommand(const QStringList& arguments, bool deferred) {
    const auto command = deferred ? QStringLiteral("export-deferred") : QStringLiteral("export");
    if (arguments.size() != 3) {
        return invalidArguments(QStringLiteral("%1 requires a source and destination").arg(command),
                                command);
    }
    const auto exported =
        packs::PackArchive::exportDirectory(arguments.at(1), arguments.at(2), {},
                                            deferred ? packs::PackValidationScope::ResolvedClosure
                                                     : packs::PackValidationScope::Standalone);
    if (!exported) {
        return failure(ExitCode::InvalidPack, packErrorCode(exported.error().code),
                       exported.error().message, command);
    }
    auto object = revisionObject(*exported);
    object.insert(QStringLiteral("command"), command);
    object.insert(QStringLiteral("validation_scope"),
                  deferred ? QStringLiteral("deferred_references") : QStringLiteral("standalone"));
    if (deferred) {
        object.insert(QStringLiteral("resolved"), false);
        object.insert(
            QStringLiteral("notice"),
            QStringLiteral("Archive references remain deferred until catalog resolution"));
    }
    return success(std::move(object));
}

[[nodiscard]] RunResult validateResolvedCommand(const QStringList& arguments) {
    constexpr auto command = "validate-resolved";
    if (arguments.size() != 5) {
        return invalidArguments(
            QStringLiteral("validate-resolved requires a catalog and exact root revision"),
            QLatin1StringView(command));
    }
    auto catalog = packs::PackCatalog::open(arguments.at(1));
    if (!catalog) {
        return failure(ExitCode::OperationFailed, catalogErrorCode(catalog.error().code),
                       catalog.error().message, QLatin1StringView(command));
    }
    const auto root =
        model::PackRevision{model::PackId{arguments.at(2).toStdString()},
                            arguments.at(3).toStdString(), arguments.at(4).toStdString()};
    const auto resolved = (*catalog)->loadResolved(root);
    if (!resolved) {
        return failure(ExitCode::OperationFailed, catalogErrorCode(resolved.error().code),
                       resolved.error().message, QLatin1StringView(command));
    }
    QJsonArray pins;
    for (const auto& revision : resolved->revisionsByPackId()) {
        pins.push_back(revisionObject(revision));
    }
    auto object = revisionObject(resolved->root().revision);
    object.insert(QStringLiteral("command"), QLatin1StringView(command));
    object.insert(QStringLiteral("resolved_revision_count"),
                  static_cast<qint64>(resolved->revisionsByPackId().size()));
    object.insert(QStringLiteral("revision_pins"), pins);
    object.insert(QStringLiteral("validation_scope"), QStringLiteral("catalog_resolved"));
    return success(std::move(object));
}

[[nodiscard]] RunResult installCommand(const QStringList& arguments) {
    constexpr auto command = "install";
    if (arguments.size() != 3 && arguments.size() != 5) {
        return invalidArguments(QStringLiteral("install received invalid arguments"),
                                QLatin1StringView(command));
    }
    QString installed_at = currentCanonicalUtc();
    if (arguments.size() == 5) {
        if (arguments.at(3) != QStringLiteral("--installed-at")) {
            return invalidArguments(QStringLiteral("install received an unknown option"),
                                    QLatin1StringView(command));
        }
        installed_at = arguments.at(4);
        if (!isCanonicalUtc(installed_at)) {
            return invalidArguments(
                QStringLiteral("--installed-at must be YYYY-MM-DDTHH:MM:SSZ in UTC"),
                QLatin1StringView(command));
        }
    }
    auto catalog = packs::PackCatalog::open(arguments.at(2));
    if (!catalog) {
        return failure(ExitCode::OperationFailed, catalogErrorCode(catalog.error().code),
                       catalog.error().message, QLatin1StringView(command));
    }
    const auto installed = (*catalog)->installArchive(arguments.at(1), installed_at);
    if (!installed) {
        const auto exit_code = installed.error().code == packs::CatalogErrorCode::ArchiveInvalid
                                   ? ExitCode::InvalidPack
                                   : ExitCode::OperationFailed;
        return failure(exit_code, catalogErrorCode(installed.error().code),
                       installed.error().message, QLatin1StringView(command));
    }
    auto object = installedObject(*installed);
    object.insert(QStringLiteral("command"), QLatin1StringView(command));
    return success(std::move(object));
}

[[nodiscard]] RunResult listCommand(const QStringList& arguments) {
    constexpr auto command = "list";
    if (arguments.size() != 2) {
        return invalidArguments(QStringLiteral("list requires exactly one catalog"),
                                QLatin1StringView(command));
    }
    auto catalog = packs::PackCatalog::open(arguments.at(1));
    if (!catalog) {
        return failure(ExitCode::OperationFailed, catalogErrorCode(catalog.error().code),
                       catalog.error().message, QLatin1StringView(command));
    }
    const auto listed = (*catalog)->list();
    if (!listed) {
        return failure(ExitCode::OperationFailed, catalogErrorCode(listed.error().code),
                       listed.error().message, QLatin1StringView(command));
    }
    QJsonArray packs_array;
    for (const auto& installed : *listed) {
        packs_array.push_back(installedObject(installed));
    }
    return success(QJsonObject{
        {QStringLiteral("command"), QLatin1StringView(command)},
        {QStringLiteral("packs"), packs_array},
    });
}

constexpr qsizetype maximum_manifest_authoring_bytes = 1024 * 1024;
constexpr qsizetype maximum_review_authoring_bytes = 8 * 1024 * 1024;
constexpr qsizetype maximum_transaction_journal_bytes = 1024 * 1024;

[[nodiscard]] QString systemError(const QString& action) {
    return QStringLiteral("%1: %2").arg(action, QString::fromLocal8Bit(std::strerror(errno)));
}

#if defined(Q_OS_UNIX)
[[nodiscard]] bool isSafeRelativeTransactionPath(const QString& path);

class FileDescriptor final {
  public:
    explicit FileDescriptor(int descriptor = -1) : descriptor_(descriptor) {}
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            if (descriptor_ >= 0) {
                static_cast<void>(::close(descriptor_));
            }
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }
    ~FileDescriptor() {
        if (descriptor_ >= 0) {
            static_cast<void>(::close(descriptor_));
        }
    }
    [[nodiscard]] int get() const { return descriptor_; }

  private:
    int descriptor_;
};

[[nodiscard]] bool sameStatSnapshot(const struct stat& left, const struct stat& right) {
    if (left.st_dev != right.st_dev || left.st_ino != right.st_ino ||
        left.st_size != right.st_size || left.st_mtime != right.st_mtime ||
        left.st_ctime != right.st_ctime) {
        return false;
    }
#if defined(Q_OS_DARWIN)
    return left.st_mtimespec.tv_nsec == right.st_mtimespec.tv_nsec &&
           left.st_ctimespec.tv_nsec == right.st_ctimespec.tv_nsec;
#else
    return left.st_mtim.tv_nsec == right.st_mtim.tv_nsec &&
           left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
#endif
}

struct AnchoredRoot final {
    QString canonical_path;
    QString parent_path;
    QString parent_access_path;
    QByteArray basename;
    FileDescriptor parent_descriptor;
    FileDescriptor root_descriptor;
    dev_t parent_device{};
    ino_t parent_inode{};
    dev_t device{};
    ino_t inode{};
};

[[nodiscard]] std::expected<AnchoredRoot, QString> openAnchoredRoot(const QString& canonical_path) {
    const QFileInfo root_info(canonical_path);
    const auto parent_path = root_info.absolutePath();
    const auto basename = QFile::encodeName(root_info.fileName());
    FileDescriptor parent(::open(QFile::encodeName(parent_path).constData(),
                                 O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY));
    if (parent.get() < 0) {
        return std::unexpected(systemError(QStringLiteral("Cannot anchor authoring parent")));
    }
    struct stat parent_status{};
    if (::fstat(parent.get(), &parent_status) != 0 || !S_ISDIR(parent_status.st_mode)) {
        return std::unexpected(systemError(QStringLiteral("Cannot inspect authoring parent")));
    }
    FileDescriptor root(::openat(parent.get(), basename.constData(),
                                 O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY));
    struct stat status{};
    if (root.get() < 0 || ::fstat(root.get(), &status) != 0 || !S_ISDIR(status.st_mode)) {
        return std::unexpected(systemError(QStringLiteral("Cannot anchor authoring root")));
    }
    QString parent_access_path;
#if defined(Q_OS_LINUX)
    parent_access_path = QStringLiteral("/proc/self/fd/%1").arg(parent.get());
    if (!QFileInfo(parent_access_path).isDir()) {
        return std::unexpected(
            QStringLiteral("Cannot establish descriptor-relative sibling access"));
    }
#else
    return std::unexpected(
        QStringLiteral("This platform has no supported descriptor-relative sibling access"));
#endif
    return AnchoredRoot{
        canonical_path,    parent_path,     parent_access_path,   basename,
        std::move(parent), std::move(root), parent_status.st_dev, parent_status.st_ino,
        status.st_dev,     status.st_ino};
}

[[nodiscard]] std::expected<void, QString> validateRootIdentity(const AnchoredRoot& root) {
    struct stat anchored_parent{};
    struct stat named_parent{};
    struct stat anchored{};
    struct stat named{};
    if (::fstat(root.parent_descriptor.get(), &anchored_parent) != 0 ||
        !S_ISDIR(anchored_parent.st_mode) || anchored_parent.st_dev != root.parent_device ||
        anchored_parent.st_ino != root.parent_inode ||
        ::lstat(QFile::encodeName(root.parent_path).constData(), &named_parent) != 0 ||
        !S_ISDIR(named_parent.st_mode) || named_parent.st_dev != root.parent_device ||
        named_parent.st_ino != root.parent_inode ||
        ::fstat(root.root_descriptor.get(), &anchored) != 0 || !S_ISDIR(anchored.st_mode) ||
        anchored.st_dev != root.device || anchored.st_ino != root.inode ||
        ::fstatat(root.parent_descriptor.get(), root.basename.constData(), &named,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISDIR(named.st_mode) || named.st_dev != root.device || named.st_ino != root.inode) {
        return std::unexpected(
            QStringLiteral("Authoring root was renamed, replaced, or redirected"));
    }
    return {};
}

struct AnchoredMemberParent final {
    FileDescriptor descriptor;
    QByteArray basename;
};

[[nodiscard]] std::expected<AnchoredMemberParent, QString>
openAnchoredMemberParent(const AnchoredRoot& root, const QString& relative_path) {
    if (!isSafeRelativeTransactionPath(relative_path)) {
        return std::unexpected(QStringLiteral("Authoring member path is unsafe"));
    }
    auto components = relative_path.split(u'/');
    const auto basename = QFile::encodeName(components.takeLast());
    FileDescriptor current(::fcntl(root.root_descriptor.get(), F_DUPFD_CLOEXEC, 0));
    if (current.get() < 0) {
        return std::unexpected(systemError(QStringLiteral("Cannot duplicate root anchor")));
    }
    for (const auto& component : components) {
        FileDescriptor next(::openat(current.get(), QFile::encodeName(component).constData(),
                                     O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY));
        if (next.get() < 0) {
            return std::unexpected(
                systemError(QStringLiteral("Cannot traverse anchored authoring member")));
        }
        current = std::move(next);
    }
    return AnchoredMemberParent{std::move(current), basename};
}
#else
struct AnchoredRoot final {};

[[nodiscard]] std::expected<AnchoredRoot, QString> openAnchoredRoot(const QString& canonical_path) {
    static_cast<void>(canonical_path);
    return std::unexpected(
        QStringLiteral("This platform has no supported anchored authoring root"));
}

[[nodiscard]] std::expected<void, QString> validateRootIdentity(const AnchoredRoot& root) {
    static_cast<void>(root);
    return std::unexpected(
        QStringLiteral("This platform has no supported anchored authoring root"));
}
#endif

#if defined(Q_OS_UNIX)
[[nodiscard]] std::expected<QByteArray, QString>
readRegularDescriptor(int descriptor, const QString& display_path, qsizetype maximum_bytes) {
    struct stat before{};
    if (::fstat(descriptor, &before) != 0 || !S_ISREG(before.st_mode) || before.st_size < 0 ||
        before.st_size > static_cast<off_t>(maximum_bytes)) {
        return std::unexpected(
            QStringLiteral("File is not a bounded regular file: %1").arg(display_path));
    }
    QByteArray bytes(static_cast<qsizetype>(before.st_size), Qt::Uninitialized);
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::read(descriptor, bytes.data() + offset,
                                  static_cast<std::size_t>(bytes.size() - offset));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return std::unexpected(
                QStringLiteral("File changed or ended during read: %1").arg(display_path));
        }
        offset += static_cast<qsizetype>(count);
    }
    char extra{};
    ssize_t extra_count{};
    do {
        extra_count = ::read(descriptor, &extra, 1);
    } while (extra_count < 0 && errno == EINTR);
    struct stat after{};
    if (extra_count != 0 || ::fstat(descriptor, &after) != 0 || !sameStatSnapshot(before, after)) {
        return std::unexpected(
            QStringLiteral("File changed or grew during read: %1").arg(display_path));
    }
    return bytes;
}
#endif

[[nodiscard]] std::expected<QByteArray, QString> readRegularFileNoFollow(const QString& path,
                                                                         qsizetype maximum_bytes) {
#if defined(Q_OS_UNIX)
    const auto encoded_path = QFile::encodeName(path);
    FileDescriptor file(::open(encoded_path.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (file.get() < 0) {
        return std::unexpected(
            systemError(QStringLiteral("Cannot open regular file %1").arg(path)));
    }
    return readRegularDescriptor(file.get(), path, maximum_bytes);
#else
    static_cast<void>(path);
    static_cast<void>(maximum_bytes);
    return std::unexpected(
        QStringLiteral("This platform has no supported no-follow authoring file reader"));
#endif
}

#if defined(Q_OS_UNIX)
[[nodiscard]] std::expected<QByteArray, QString> readAnchoredRootFile(const AnchoredRoot& root,
                                                                      const QString& relative_path,
                                                                      qsizetype maximum_bytes) {
    if (const auto stable = validateRootIdentity(root); !stable) {
        return std::unexpected(stable.error());
    }
    auto parent = openAnchoredMemberParent(root, relative_path);
    if (!parent) {
        return std::unexpected(parent.error());
    }
    FileDescriptor file(::openat(parent->descriptor.get(), parent->basename.constData(),
                                 O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (file.get() < 0) {
        return std::unexpected(
            systemError(QStringLiteral("Cannot open anchored authoring member")));
    }
    auto bytes = readRegularDescriptor(file.get(), relative_path, maximum_bytes);
    if (!bytes) {
        return bytes;
    }
    if (const auto stable = validateRootIdentity(root); !stable) {
        return std::unexpected(stable.error());
    }
    return bytes;
}
#else
[[nodiscard]] std::expected<QByteArray, QString> readAnchoredRootFile(const AnchoredRoot& root,
                                                                      const QString& relative_path,
                                                                      qsizetype maximum_bytes) {
    static_cast<void>(root);
    static_cast<void>(relative_path);
    static_cast<void>(maximum_bytes);
    return std::unexpected(
        QStringLiteral("This platform has no supported anchored authoring root"));
}
#endif

[[nodiscard]] QString sha256Bytes(QByteArrayView bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

[[nodiscard]] bool isSafeRelativeTransactionPath(const QString& path) {
    if (path.isEmpty() || QDir::isAbsolutePath(path) || path.contains(u'\\') ||
        QDir::cleanPath(path) != path) {
        return false;
    }
    const auto components = path.split(u'/');
    return std::ranges::all_of(components, [](const QString& component) {
        return !component.isEmpty() && component != QStringLiteral(".") &&
               component != QStringLiteral("..");
    });
}

struct RealismTransactionJournal final {
    QString root_directory;
    QString review_resource_id;
    QString review_path;
    QString pack_id;
    QString version;
    QString final_digest;
    QString old_review_sha256;
    QString new_review_sha256;
    QString old_manifest_sha256;
    QString new_manifest_sha256;
};

struct RealismTransaction final {
    RealismTransactionJournal journal;
    QByteArray old_review;
    QByteArray new_review;
    QByteArray old_manifest;
    QByteArray new_manifest;
    bool recovery_wrote{};
};

[[nodiscard]] QJsonObject transactionJournalObject(const RealismTransactionJournal& journal) {
    return QJsonObject{
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("root_directory"), journal.root_directory},
        {QStringLiteral("review_resource_id"), journal.review_resource_id},
        {QStringLiteral("review_path"), journal.review_path},
        {QStringLiteral("pack_id"), journal.pack_id},
        {QStringLiteral("version"), journal.version},
        {QStringLiteral("final_digest"), journal.final_digest},
        {QStringLiteral("old_review_sha256"), journal.old_review_sha256},
        {QStringLiteral("new_review_sha256"), journal.new_review_sha256},
        {QStringLiteral("old_manifest_sha256"), journal.old_manifest_sha256},
        {QStringLiteral("new_manifest_sha256"), journal.new_manifest_sha256},
    };
}

[[nodiscard]] std::expected<RealismTransactionJournal, QString>
parseTransactionJournal(QByteArrayView bytes, const QString& expected_root,
                        const QString& expected_review_id) {
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(bytes.toByteArray(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::unexpected(QStringLiteral("Realism authoring transaction journal is invalid"));
    }
    const auto object = document.object();
    static const QSet<QString> exact_fields{
        QStringLiteral("schema_version"),
        QStringLiteral("root_directory"),
        QStringLiteral("review_resource_id"),
        QStringLiteral("review_path"),
        QStringLiteral("pack_id"),
        QStringLiteral("version"),
        QStringLiteral("final_digest"),
        QStringLiteral("old_review_sha256"),
        QStringLiteral("new_review_sha256"),
        QStringLiteral("old_manifest_sha256"),
        QStringLiteral("new_manifest_sha256"),
    };
    QSet<QString> actual_fields;
    for (auto iterator = object.constBegin(); iterator != object.constEnd(); ++iterator) {
        actual_fields.insert(iterator.key());
    }
    const auto isDigest = [](const QString& value) {
        return value.size() == 64 && std::ranges::all_of(value, [](QChar character) {
                   return (character >= u'0' && character <= u'9') ||
                          (character >= u'a' && character <= u'f');
               });
    };
    RealismTransactionJournal journal{
        object.value(QStringLiteral("root_directory")).toString(),
        object.value(QStringLiteral("review_resource_id")).toString(),
        object.value(QStringLiteral("review_path")).toString(),
        object.value(QStringLiteral("pack_id")).toString(),
        object.value(QStringLiteral("version")).toString(),
        object.value(QStringLiteral("final_digest")).toString(),
        object.value(QStringLiteral("old_review_sha256")).toString(),
        object.value(QStringLiteral("new_review_sha256")).toString(),
        object.value(QStringLiteral("old_manifest_sha256")).toString(),
        object.value(QStringLiteral("new_manifest_sha256")).toString(),
    };
    if (actual_fields != exact_fields ||
        object.value(QStringLiteral("schema_version")).toInt() != 1 ||
        journal.root_directory != expected_root ||
        journal.review_resource_id != expected_review_id ||
        !isSafeRelativeTransactionPath(journal.review_path) || journal.pack_id.isEmpty() ||
        journal.version.isEmpty() || !isDigest(journal.final_digest) ||
        !isDigest(journal.old_review_sha256) || !isDigest(journal.new_review_sha256) ||
        !isDigest(journal.old_manifest_sha256) || !isDigest(journal.new_manifest_sha256)) {
        return std::unexpected(
            QStringLiteral("Realism authoring transaction journal does not match this request"));
    }
    return journal;
}

#if defined(Q_OS_UNIX)
struct AnchoredTransaction final {
    QByteArray basename;
    FileDescriptor descriptor;
    dev_t device{};
    ino_t inode{};
};
#else
struct AnchoredTransaction final {};
#endif

struct RealismTransactionHandle final {
    AnchoredTransaction anchor;
    RealismTransaction transaction;
};

enum class RecoveryErrorCode { Conflict, ImmutableConflict };

struct RecoveryError final {
    RecoveryErrorCode code{};
    QString message;
};

struct TransactionTargetEquality final {
    bool old_bytes{};
    bool new_bytes{};
};

struct TransactionEquality final {
    TransactionTargetEquality review;
    TransactionTargetEquality manifest;
};

[[nodiscard]] bool isOldState(const TransactionEquality& state) {
    return state.review.old_bytes && state.manifest.old_bytes;
}

[[nodiscard]] bool isReviewCommittedState(const TransactionEquality& state) {
    return state.review.new_bytes && state.manifest.old_bytes;
}

[[nodiscard]] bool isFinalState(const TransactionEquality& state) {
    return state.review.new_bytes && state.manifest.new_bytes;
}

[[nodiscard]] bool isDeclaredState(const TransactionEquality& state) {
    return (state.review.old_bytes || state.review.new_bytes) &&
           (state.manifest.old_bytes || state.manifest.new_bytes);
}

[[nodiscard]] std::expected<TransactionEquality, QString>
transactionState(const AnchoredRoot& root, const RealismTransactionJournal& journal) {
    const auto review =
        readAnchoredRootFile(root, journal.review_path, maximum_review_authoring_bytes);
    const auto manifest = readAnchoredRootFile(root, QStringLiteral("manifest.json"),
                                               maximum_manifest_authoring_bytes);
    if (!review || !manifest) {
        return std::unexpected(review ? manifest.error() : review.error());
    }
    const auto review_digest = sha256Bytes(QByteArrayView(*review));
    const auto manifest_digest = sha256Bytes(QByteArrayView(*manifest));
    return TransactionEquality{
        {review_digest == journal.old_review_sha256, review_digest == journal.new_review_sha256},
        {manifest_digest == journal.old_manifest_sha256,
         manifest_digest == journal.new_manifest_sha256},
    };
}

#if defined(Q_OS_UNIX)
[[nodiscard]] bool isSafeTransactionName(const QByteArray& name) {
    return !name.isEmpty() && !name.contains('/') && name != "." && name != "..";
}

[[nodiscard]] std::expected<void, QString>
validateTransactionIdentity(const AnchoredRoot& root, const AnchoredTransaction& transaction) {
    if (const auto stable = validateRootIdentity(root); !stable) {
        return stable;
    }
    struct stat anchored{};
    struct stat named{};
    if (!isSafeTransactionName(transaction.basename) ||
        ::fstat(transaction.descriptor.get(), &anchored) != 0 || !S_ISDIR(anchored.st_mode) ||
        anchored.st_uid != ::geteuid() || (anchored.st_mode & 0777) != 0700 ||
        anchored.st_dev != transaction.device || anchored.st_ino != transaction.inode ||
        ::fstatat(root.parent_descriptor.get(), transaction.basename.constData(), &named,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISDIR(named.st_mode) || named.st_uid != ::geteuid() || (named.st_mode & 0777) != 0700 ||
        named.st_dev != transaction.device || named.st_ino != transaction.inode) {
        return std::unexpected(
            QStringLiteral("Authoring transaction was renamed, replaced, or redirected"));
    }
    return {};
}

[[nodiscard]] std::expected<std::optional<AnchoredTransaction>, QString>
openAnchoredTransaction(const AnchoredRoot& root, const QByteArray& basename) {
    if (const auto stable = validateRootIdentity(root); !stable) {
        return std::unexpected(stable.error());
    }
    if (!isSafeTransactionName(basename)) {
        return std::unexpected(QStringLiteral("Authoring transaction name is unsafe"));
    }
    struct stat named{};
    if (::fstatat(root.parent_descriptor.get(), basename.constData(), &named,
                  AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            return std::optional<AnchoredTransaction>{};
        }
        return std::unexpected(systemError(QStringLiteral("Cannot inspect authoring transaction")));
    }
    if (!S_ISDIR(named.st_mode) || named.st_uid != ::geteuid() || (named.st_mode & 0777) != 0700) {
        return std::unexpected(QStringLiteral("Transaction directory is not private and trusted"));
    }
    FileDescriptor descriptor(::openat(root.parent_descriptor.get(), basename.constData(),
                                       O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY));
    struct stat anchored{};
    if (descriptor.get() < 0 || ::fstat(descriptor.get(), &anchored) != 0 ||
        !S_ISDIR(anchored.st_mode) || anchored.st_dev != named.st_dev ||
        anchored.st_ino != named.st_ino) {
        return std::unexpected(systemError(QStringLiteral("Cannot anchor authoring transaction")));
    }
    AnchoredTransaction transaction{basename, std::move(descriptor), anchored.st_dev,
                                    anchored.st_ino};
    if (const auto validated = validateTransactionIdentity(root, transaction); !validated) {
        return std::unexpected(validated.error());
    }
    return std::optional<AnchoredTransaction>{std::move(transaction)};
}

[[nodiscard]] std::expected<AnchoredTransaction, QString>
createAnchoredTransaction(const AnchoredRoot& root, const QByteArray& basename) {
    if (const auto stable = validateRootIdentity(root); !stable) {
        return std::unexpected(stable.error());
    }
    if (!isSafeTransactionName(basename) ||
        ::mkdirat(root.parent_descriptor.get(), basename.constData(), 0700) != 0) {
        return std::unexpected(systemError(QStringLiteral("Cannot create authoring transaction")));
    }
    auto opened = openAnchoredTransaction(root, basename);
    if (!opened || !opened->has_value()) {
        return std::unexpected(opened ? QStringLiteral("Created transaction cannot be reopened")
                                      : opened.error());
    }
    if (::fsync(root.parent_descriptor.get()) != 0) {
        return std::unexpected(
            systemError(QStringLiteral("Cannot synchronize authoring transaction parent")));
    }
    return std::move(**opened);
}

[[nodiscard]] std::expected<QByteArray, QString>
readTransactionFile(const AnchoredRoot& root, const AnchoredTransaction& transaction,
                    const QByteArray& name, qsizetype maximum_bytes) {
    if (!isSafeTransactionName(name)) {
        return std::unexpected(QStringLiteral("Transaction member name is unsafe"));
    }
    if (const auto validated = validateTransactionIdentity(root, transaction); !validated) {
        return std::unexpected(validated.error());
    }
    FileDescriptor descriptor(::openat(transaction.descriptor.get(), name.constData(),
                                       O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (descriptor.get() < 0) {
        return std::unexpected(systemError(QStringLiteral("Cannot open transaction file")));
    }
    auto bytes = readRegularDescriptor(descriptor.get(), QString::fromUtf8(name), maximum_bytes);
    if (!bytes) {
        return bytes;
    }
    if (const auto validated = validateTransactionIdentity(root, transaction); !validated) {
        return std::unexpected(validated.error());
    }
    return bytes;
}

[[nodiscard]] std::expected<void, QString>
writeTransactionFile(const AnchoredRoot& root, const AnchoredTransaction& transaction,
                     const QByteArray& name, QByteArrayView bytes) {
    if (!isSafeTransactionName(name)) {
        return std::unexpected(QStringLiteral("Transaction member name is unsafe"));
    }
    if (const auto validated = validateTransactionIdentity(root, transaction); !validated) {
        return validated;
    }
    FileDescriptor descriptor(::openat(transaction.descriptor.get(), name.constData(),
                                       O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (descriptor.get() < 0) {
        return std::unexpected(systemError(QStringLiteral("Cannot create transaction file")));
    }
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::write(descriptor.get(), bytes.data() + offset,
                                   static_cast<std::size_t>(bytes.size() - offset));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return std::unexpected(systemError(QStringLiteral("Cannot write transaction file")));
        }
        offset += static_cast<qsizetype>(count);
    }
    if (::fsync(descriptor.get()) != 0) {
        return std::unexpected(systemError(QStringLiteral("Cannot synchronize transaction file")));
    }
    return validateTransactionIdentity(root, transaction);
}

[[nodiscard]] std::expected<void, QString>
removeTransactionFileIfPresent(const AnchoredRoot& root, const AnchoredTransaction& transaction,
                               const QByteArray& name) {
    if (!isSafeTransactionName(name)) {
        return std::unexpected(QStringLiteral("Transaction member name is unsafe"));
    }
    if (const auto validated = validateTransactionIdentity(root, transaction); !validated) {
        return validated;
    }
    struct stat status{};
    if (::fstatat(transaction.descriptor.get(), name.constData(), &status, AT_SYMLINK_NOFOLLOW) !=
        0) {
        if (errno == ENOENT) {
            return {};
        }
        return std::unexpected(systemError(QStringLiteral("Cannot inspect transaction file")));
    }
    if (!S_ISREG(status.st_mode)) {
        return std::unexpected(QStringLiteral("Transaction member is not a regular file"));
    }
    if (::unlinkat(transaction.descriptor.get(), name.constData(), 0) != 0) {
        return std::unexpected(systemError(QStringLiteral("Cannot remove transaction file")));
    }
    return validateTransactionIdentity(root, transaction);
}

[[nodiscard]] std::expected<bool, QString>
transactionFileExists(const AnchoredRoot& root, const AnchoredTransaction& transaction,
                      const QByteArray& name) {
    if (!isSafeTransactionName(name)) {
        return std::unexpected(QStringLiteral("Transaction member name is unsafe"));
    }
    if (const auto validated = validateTransactionIdentity(root, transaction); !validated) {
        return std::unexpected(validated.error());
    }
    struct stat status{};
    if (::fstatat(transaction.descriptor.get(), name.constData(), &status, AT_SYMLINK_NOFOLLOW) !=
        0) {
        if (errno == ENOENT) {
            return false;
        }
        return std::unexpected(systemError(QStringLiteral("Cannot inspect transaction file")));
    }
    if (!S_ISREG(status.st_mode)) {
        return std::unexpected(QStringLiteral("Transaction member is not a regular file"));
    }
    return true;
}

[[nodiscard]] std::expected<QSet<QByteArray>, QString>
transactionMembers(const AnchoredRoot& root, const AnchoredTransaction& transaction) {
    if (const auto validated = validateTransactionIdentity(root, transaction); !validated) {
        return std::unexpected(validated.error());
    }
    const auto duplicated = ::fcntl(transaction.descriptor.get(), F_DUPFD_CLOEXEC, 0);
    if (duplicated < 0) {
        return std::unexpected(systemError(QStringLiteral("Cannot duplicate transaction anchor")));
    }
    DIR* directory = ::fdopendir(duplicated);
    if (directory == nullptr) {
        static_cast<void>(::close(duplicated));
        return std::unexpected(systemError(QStringLiteral("Cannot enumerate transaction")));
    }
    QSet<QByteArray> members;
    errno = 0;
    while (const auto* entry = ::readdir(directory)) {
        const QByteArray name(entry->d_name);
        if (name != "." && name != "..") {
            members.insert(name);
        }
        errno = 0;
    }
    const auto read_error = errno;
    static_cast<void>(::closedir(directory));
    if (read_error != 0) {
        errno = read_error;
        return std::unexpected(systemError(QStringLiteral("Cannot enumerate transaction")));
    }
    if (const auto validated = validateTransactionIdentity(root, transaction); !validated) {
        return std::unexpected(validated.error());
    }
    return members;
}

[[nodiscard]] std::expected<void, QString> syncTransaction(const AnchoredRoot& root,
                                                           const AnchoredTransaction& transaction) {
    if (const auto validated = validateTransactionIdentity(root, transaction); !validated) {
        return validated;
    }
    if (::fsync(transaction.descriptor.get()) != 0) {
        return std::unexpected(systemError(QStringLiteral("Cannot synchronize transaction")));
    }
    return validateTransactionIdentity(root, transaction);
}

[[nodiscard]] std::expected<void, QString>
cleanupTransactionDirectory(const AnchoredRoot& root, const AnchoredTransaction& transaction) {
    static const QSet<QByteArray> allowed_members{
        "review.old",   "review.new",   "manifest.old",   "manifest.new",
        "journal.json", "review.apply", "manifest.apply",
    };
    const auto members = transactionMembers(root, transaction);
    if (!members) {
        return std::unexpected(members.error());
    }
    for (const auto& member : *members) {
        if (!allowed_members.contains(member)) {
            return std::unexpected(
                QStringLiteral("Transaction directory contains an unexpected member"));
        }
        if (const auto removed = removeTransactionFileIfPresent(root, transaction, member);
            !removed) {
            return removed;
        }
    }
    if (const auto synced = syncTransaction(root, transaction); !synced) {
        return synced;
    }
    if (const auto validated = validateTransactionIdentity(root, transaction); !validated) {
        return validated;
    }
    if (::unlinkat(root.parent_descriptor.get(), transaction.basename.constData(), AT_REMOVEDIR) !=
        0) {
        return std::unexpected(systemError(QStringLiteral("Cannot remove transaction directory")));
    }
    if (::fsync(root.parent_descriptor.get()) != 0) {
        return std::unexpected(
            systemError(QStringLiteral("Cannot synchronize authoring transaction parent")));
    }
    return validateRootIdentity(root);
}

[[nodiscard]] std::expected<void, QString>
replaceTargetFromBytes(const AnchoredRoot& root, const AnchoredTransaction& transaction,
                       const QByteArray& label, const QString& relative_target,
                       QByteArrayView bytes) {
    if (const auto validated = validateTransactionIdentity(root, transaction); !validated) {
        return validated;
    }
    auto target_parent = openAnchoredMemberParent(root, relative_target);
    if (!target_parent) {
        return std::unexpected(target_parent.error());
    }
    const auto apply_name = label + ".apply";
    if (const auto removed = removeTransactionFileIfPresent(root, transaction, apply_name);
        !removed) {
        return removed;
    }
    if (const auto written = writeTransactionFile(root, transaction, apply_name, bytes); !written) {
        return written;
    }
    if (const auto synced = syncTransaction(root, transaction); !synced) {
        return synced;
    }
    if (const auto validated = validateTransactionIdentity(root, transaction); !validated) {
        return validated;
    }
    if (::renameat(transaction.descriptor.get(), apply_name.constData(),
                   target_parent->descriptor.get(), target_parent->basename.constData()) != 0) {
        return std::unexpected(systemError(QStringLiteral("Cannot replace authored file")));
    }
    if (::fsync(target_parent->descriptor.get()) != 0 ||
        ::fsync(transaction.descriptor.get()) != 0) {
        return std::unexpected(systemError(QStringLiteral("Cannot synchronize authored update")));
    }
    return validateTransactionIdentity(root, transaction);
}
#else
[[nodiscard]] std::expected<std::optional<AnchoredTransaction>, QString>
openAnchoredTransaction(const AnchoredRoot& root, const QByteArray& basename) {
    static_cast<void>(root);
    static_cast<void>(basename);
    return std::unexpected(
        QStringLiteral("This platform has no supported anchored authoring transaction"));
}

[[nodiscard]] std::expected<AnchoredTransaction, QString>
createAnchoredTransaction(const AnchoredRoot& root, const QByteArray& basename) {
    static_cast<void>(root);
    static_cast<void>(basename);
    return std::unexpected(
        QStringLiteral("This platform has no supported anchored authoring transaction"));
}

[[nodiscard]] std::expected<QByteArray, QString>
readTransactionFile(const AnchoredRoot& root, const AnchoredTransaction& transaction,
                    const QByteArray& name, qsizetype maximum_bytes) {
    static_cast<void>(root);
    static_cast<void>(transaction);
    static_cast<void>(name);
    static_cast<void>(maximum_bytes);
    return std::unexpected(
        QStringLiteral("This platform has no supported anchored authoring transaction"));
}

[[nodiscard]] std::expected<void, QString>
writeTransactionFile(const AnchoredRoot& root, const AnchoredTransaction& transaction,
                     const QByteArray& name, QByteArrayView bytes) {
    static_cast<void>(root);
    static_cast<void>(transaction);
    static_cast<void>(name);
    static_cast<void>(bytes);
    return std::unexpected(
        QStringLiteral("This platform has no supported anchored authoring transaction"));
}

[[nodiscard]] std::expected<void, QString>
removeTransactionFileIfPresent(const AnchoredRoot& root, const AnchoredTransaction& transaction,
                               const QByteArray& name) {
    static_cast<void>(root);
    static_cast<void>(transaction);
    static_cast<void>(name);
    return std::unexpected(
        QStringLiteral("This platform has no supported anchored authoring transaction"));
}

[[nodiscard]] std::expected<bool, QString>
transactionFileExists(const AnchoredRoot& root, const AnchoredTransaction& transaction,
                      const QByteArray& name) {
    static_cast<void>(root);
    static_cast<void>(transaction);
    static_cast<void>(name);
    return std::unexpected(
        QStringLiteral("This platform has no supported anchored authoring transaction"));
}

[[nodiscard]] std::expected<void, QString>
cleanupTransactionDirectory(const AnchoredRoot& root, const AnchoredTransaction& transaction) {
    static_cast<void>(root);
    static_cast<void>(transaction);
    return std::unexpected(
        QStringLiteral("This platform has no supported anchored authoring transaction"));
}

[[nodiscard]] std::expected<void, QString>
replaceTargetFromBytes(const AnchoredRoot& root, const AnchoredTransaction& transaction,
                       const QByteArray& label, const QString& relative_target,
                       QByteArrayView bytes) {
    static_cast<void>(root);
    static_cast<void>(transaction);
    static_cast<void>(label);
    static_cast<void>(relative_target);
    static_cast<void>(bytes);
    return std::unexpected(
        QStringLiteral("This platform has no supported anchored authoring transaction"));
}
#endif

[[nodiscard]] std::expected<RealismTransaction, QString>
loadTransaction(const AnchoredRoot& root, const AnchoredTransaction& transaction,
                const QString& root_directory, const QString& review_resource_id) {
    const auto journal_bytes =
        readTransactionFile(root, transaction, "journal.json", maximum_transaction_journal_bytes);
    if (!journal_bytes) {
        return std::unexpected(journal_bytes.error());
    }
    const auto journal =
        parseTransactionJournal(QByteArrayView(*journal_bytes), root_directory, review_resource_id);
    if (!journal) {
        return std::unexpected(journal.error());
    }
    const auto old_review =
        readTransactionFile(root, transaction, "review.old", maximum_review_authoring_bytes);
    const auto new_review =
        readTransactionFile(root, transaction, "review.new", maximum_review_authoring_bytes);
    const auto old_manifest =
        readTransactionFile(root, transaction, "manifest.old", maximum_manifest_authoring_bytes);
    const auto new_manifest =
        readTransactionFile(root, transaction, "manifest.new", maximum_manifest_authoring_bytes);
    if (!old_review || !new_review || !old_manifest || !new_manifest) {
        return std::unexpected(QStringLiteral("Transaction staged bytes are incomplete"));
    }
    if (sha256Bytes(QByteArrayView(*old_review)) != journal->old_review_sha256 ||
        sha256Bytes(QByteArrayView(*new_review)) != journal->new_review_sha256 ||
        sha256Bytes(QByteArrayView(*old_manifest)) != journal->old_manifest_sha256 ||
        sha256Bytes(QByteArrayView(*new_manifest)) != journal->new_manifest_sha256) {
        return std::unexpected(QStringLiteral("Transaction staged bytes fail their exact digests"));
    }
    return RealismTransaction{*journal,      *old_review,   *new_review,
                              *old_manifest, *new_manifest, false};
}

[[nodiscard]] std::expected<void, RecoveryError>
preflightRecoveryRevision(const packs::PackCatalog& catalog,
                          const RealismTransactionJournal& journal) {
    const auto installed =
        catalog.load(model::PackId{journal.pack_id.toStdString()}, journal.version.toStdString());
    if (installed && fromUtf8(installed->revision.digest) != journal.final_digest) {
        return std::unexpected(RecoveryError{
            RecoveryErrorCode::ImmutableConflict,
            QStringLiteral("Catalog already contains a different immutable revision for %1 %2")
                .arg(journal.pack_id, journal.version),
        });
    }
    if (!installed && installed.error().code != packs::CatalogErrorCode::NotFound) {
        return std::unexpected(
            RecoveryError{RecoveryErrorCode::Conflict, installed.error().message});
    }
    return {};
}

[[nodiscard]] std::expected<std::optional<RealismTransactionHandle>, RecoveryError>
recoverTransaction(const AnchoredRoot& root, const QByteArray& transaction_name,
                   const QString& root_directory, const QString& review_resource_id,
                   const packs::PackCatalog& catalog) {
    auto opened = openAnchoredTransaction(root, transaction_name);
    if (!opened) {
        return std::unexpected(RecoveryError{RecoveryErrorCode::Conflict, opened.error()});
    }
    if (!opened->has_value()) {
        return std::optional<RealismTransactionHandle>{};
    }
    auto transaction_anchor = std::move(**opened);
    for (const auto& name : {QByteArray{"review.apply"}, QByteArray{"manifest.apply"}}) {
        if (const auto removed = removeTransactionFileIfPresent(root, transaction_anchor, name);
            !removed) {
            return std::unexpected(RecoveryError{RecoveryErrorCode::Conflict, removed.error()});
        }
    }
    const auto has_journal =
        transactionFileExists(root, transaction_anchor, QByteArray{"journal.json"});
    if (!has_journal) {
        return std::unexpected(RecoveryError{RecoveryErrorCode::Conflict, has_journal.error()});
    }
    if (!*has_journal) {
        if (const auto cleaned = cleanupTransactionDirectory(root, transaction_anchor); !cleaned) {
            return std::unexpected(RecoveryError{RecoveryErrorCode::Conflict, cleaned.error()});
        }
        return std::optional<RealismTransactionHandle>{};
    }
    auto transaction =
        loadTransaction(root, transaction_anchor, root_directory, review_resource_id);
    if (!transaction) {
        return std::unexpected(RecoveryError{RecoveryErrorCode::Conflict, transaction.error()});
    }
    const auto state = transactionState(root, transaction->journal);
    if (!state) {
        return std::unexpected(RecoveryError{RecoveryErrorCode::Conflict, state.error()});
    }
    if (isOldState(*state)) {
        if (const auto cleaned = cleanupTransactionDirectory(root, transaction_anchor); !cleaned) {
            return std::unexpected(RecoveryError{RecoveryErrorCode::Conflict, cleaned.error()});
        }
        return std::optional<RealismTransactionHandle>{};
    }
    if (isFinalState(*state)) {
        return std::optional<RealismTransactionHandle>{
            RealismTransactionHandle{std::move(transaction_anchor), std::move(*transaction)}};
    }
    if (!isReviewCommittedState(*state)) {
        return std::unexpected(RecoveryError{
            RecoveryErrorCode::Conflict,
            QStringLiteral("Authoring targets contain bytes outside the declared transaction"),
        });
    }
    if (const auto preflight = preflightRecoveryRevision(catalog, transaction->journal);
        !preflight) {
        return std::unexpected(preflight.error());
    }
    if (transaction->old_manifest != transaction->new_manifest) {
        if (const auto replaced = replaceTargetFromBytes(root, transaction_anchor, "manifest",
                                                         QStringLiteral("manifest.json"),
                                                         QByteArrayView(transaction->new_manifest));
            !replaced) {
            return std::unexpected(RecoveryError{RecoveryErrorCode::Conflict, replaced.error()});
        }
        transaction->recovery_wrote = true;
    }
    const auto completed = transactionState(root, transaction->journal);
    if (!completed || !isFinalState(*completed)) {
        return std::unexpected(RecoveryError{
            RecoveryErrorCode::Conflict,
            QStringLiteral("Recovered transaction did not reach its exact authored state"),
        });
    }
    return std::optional<RealismTransactionHandle>{
        RealismTransactionHandle{std::move(transaction_anchor), std::move(*transaction)}};
}

[[nodiscard]] std::expected<RealismTransactionHandle, QString>
prepareTransaction(const AnchoredRoot& root, const QByteArray& transaction_name,
                   const QString& root_directory, const packs::AuthoredRealismEvidence& authored) {
    const RealismTransactionJournal journal{
        root_directory,
        authored.review_resource_id,
        authored.review_path,
        fromUtf8(authored.root_revision.id.value),
        fromUtf8(authored.root_revision.version),
        fromUtf8(authored.root_revision.digest),
        sha256Bytes(QByteArrayView(authored.source_review_bytes)),
        sha256Bytes(QByteArrayView(authored.review_bytes)),
        sha256Bytes(QByteArrayView(authored.source_manifest_bytes)),
        sha256Bytes(QByteArrayView(authored.manifest_bytes)),
    };
    auto transaction_anchor = createAnchoredTransaction(root, transaction_name);
    if (!transaction_anchor) {
        return std::unexpected(transaction_anchor.error());
    }
    const auto stage = [&](const QByteArray& name, QByteArrayView bytes) {
        return writeTransactionFile(root, *transaction_anchor, name, bytes);
    };
    const auto old_review = stage("review.old", authored.source_review_bytes);
    const auto new_review = stage("review.new", authored.review_bytes);
    const auto old_manifest = stage("manifest.old", authored.source_manifest_bytes);
    const auto new_manifest = stage("manifest.new", authored.manifest_bytes);
    const auto journal_bytes =
        QJsonDocument(transactionJournalObject(journal)).toJson(QJsonDocument::Compact) + '\n';
    const auto written_journal = stage("journal.json", journal_bytes);
    if (!old_review || !new_review || !old_manifest || !new_manifest || !written_journal) {
        const auto error = !old_review     ? old_review.error()
                           : !new_review   ? new_review.error()
                           : !old_manifest ? old_manifest.error()
                           : !new_manifest ? new_manifest.error()
                                           : written_journal.error();
        static_cast<void>(cleanupTransactionDirectory(root, *transaction_anchor));
        return std::unexpected(error);
    }
#if defined(Q_OS_UNIX)
    if (const auto synced = syncTransaction(root, *transaction_anchor); !synced) {
        static_cast<void>(cleanupTransactionDirectory(root, *transaction_anchor));
        return std::unexpected(synced.error());
    }
    if (::fsync(root.parent_descriptor.get()) != 0) {
        const auto error =
            systemError(QStringLiteral("Cannot synchronize authoring transaction parent"));
        static_cast<void>(cleanupTransactionDirectory(root, *transaction_anchor));
        return std::unexpected(error);
    }
#endif
    return RealismTransactionHandle{
        std::move(*transaction_anchor),
        RealismTransaction{journal, authored.source_review_bytes, authored.review_bytes,
                           authored.source_manifest_bytes, authored.manifest_bytes, false},
    };
}

[[nodiscard]] std::expected<void, QString>
applyTransaction(const AnchoredRoot& root, const RealismTransactionHandle& handle) {
    const auto& transaction = handle.transaction;
    const auto initial = transactionState(root, transaction.journal);
    if (!initial || !isOldState(*initial)) {
        return std::unexpected(initial ? QStringLiteral("Authoring source changed before commit")
                                       : initial.error());
    }
    if (transaction.old_review != transaction.new_review) {
        if (const auto replaced = replaceTargetFromBytes(root, handle.anchor, "review",
                                                         transaction.journal.review_path,
                                                         QByteArrayView(transaction.new_review));
            !replaced) {
            return replaced;
        }
    }
    const auto review_committed = transactionState(root, transaction.journal);
    if (!review_committed || !isReviewCommittedState(*review_committed)) {
        return std::unexpected(QStringLiteral("Authoring targets changed after the review commit"));
    }
    if (transaction.old_manifest != transaction.new_manifest) {
        if (const auto replaced = replaceTargetFromBytes(root, handle.anchor, "manifest",
                                                         QStringLiteral("manifest.json"),
                                                         QByteArrayView(transaction.new_manifest));
            !replaced) {
            return replaced;
        }
    }
    const auto completed = transactionState(root, transaction.journal);
    if (!completed || !isFinalState(*completed)) {
        return std::unexpected(
            QStringLiteral("Authoring targets changed after the manifest commit"));
    }
    return {};
}

[[nodiscard]] bool rollbackTransaction(const AnchoredRoot& root,
                                       const RealismTransactionHandle& handle) {
    const auto& transaction = handle.transaction;
    auto state = transactionState(root, transaction.journal);
    if (!state || !isDeclaredState(*state) ||
        (!isOldState(*state) && !isReviewCommittedState(*state) && !isFinalState(*state))) {
        return false;
    }
    if (isFinalState(*state) && transaction.old_manifest != transaction.new_manifest) {
        if (!replaceTargetFromBytes(root, handle.anchor, "manifest",
                                    QStringLiteral("manifest.json"),
                                    QByteArrayView(transaction.old_manifest))) {
            return false;
        }
        state = transactionState(root, transaction.journal);
    }
    if (!state || !isDeclaredState(*state)) {
        return false;
    }
    if (!isOldState(*state) && isReviewCommittedState(*state) &&
        transaction.old_review != transaction.new_review) {
        if (!replaceTargetFromBytes(root, handle.anchor, "review", transaction.journal.review_path,
                                    QByteArrayView(transaction.old_review))) {
            return false;
        }
        state = transactionState(root, transaction.journal);
    }
    return state && isOldState(*state) &&
           cleanupTransactionDirectory(root, handle.anchor).has_value();
}

[[nodiscard]] RunResult
authorRealismEvidenceCommand(const QStringList& arguments,
                             packs::RealismEvidenceTraceSetProfile profile) {
    const auto multi_trace =
        profile == packs::RealismEvidenceTraceSetProfile::MultiTraceProductionV1;
    const auto command = multi_trace ? "author-realism-evidence-multi" : "author-realism-evidence";
    constexpr qsizetype maximum_trace_bytes = 64 * 1024 * 1024;
    if (arguments.size() != 5) {
        return invalidArguments(
            multi_trace
                ? QStringLiteral("author-realism-evidence-multi requires a root, catalog, review "
                                 "ID, and trace-set bundle")
                : QStringLiteral(
                      "author-realism-evidence requires a root, catalog, review ID, and trace"),
            QLatin1StringView(command));
    }

    const QFileInfo requested_root(arguments.at(1));
    const auto root_directory = requested_root.canonicalFilePath();
    if (requested_root.isSymLink() || !requested_root.isDir() || root_directory.isEmpty()) {
        return failure(ExitCode::InvalidPack, QStringLiteral("invalid_authoring_root"),
                       QStringLiteral("Authoring root must be an existing non-symlink directory"),
                       QLatin1StringView(command));
    }
    auto root_anchor = openAnchoredRoot(root_directory);
    if (!root_anchor) {
        return failure(ExitCode::OperationFailed, QStringLiteral("cannot_anchor_authoring_root"),
                       root_anchor.error(), QLatin1StringView(command));
    }
    const QFileInfo root_info(root_directory);
    const auto sibling_prefix =
        QStringLiteral(".%1.author-realism-evidence").arg(root_info.fileName());
    const auto lock_path =
        QDir(root_anchor->parent_access_path).filePath(sibling_prefix + QStringLiteral(".lock"));
    const auto transaction_name =
        QFile::encodeName(sibling_prefix + QStringLiteral(".transaction"));
    if (QFileInfo(lock_path).isSymLink()) {
        return failure(ExitCode::OperationFailed, QStringLiteral("invalid_authoring_lock"),
                       QStringLiteral("Authoring lock path must not be a symbolic link"),
                       QLatin1StringView(command));
    }
    QLockFile authoring_lock(lock_path);
    authoring_lock.setStaleLockTime(30'000);
    if (!authoring_lock.tryLock(1000)) {
        return failure(ExitCode::OperationFailed, QStringLiteral("authoring_locked"),
                       QStringLiteral("Another authoring command holds the root lock"),
                       QLatin1StringView(command));
    }
    const auto rootChanged = [&]() -> std::optional<RunResult> {
        const auto stable = validateRootIdentity(*root_anchor);
        if (stable) {
            return std::nullopt;
        }
        return failure(ExitCode::OperationFailed, QStringLiteral("authoring_root_changed"),
                       stable.error(), QLatin1StringView(command));
    };
    if (const auto changed = rootChanged(); changed.has_value()) {
        return *changed;
    }

    const auto trace_bytes = readRegularFileNoFollow(arguments.at(4), maximum_trace_bytes);
    if (!trace_bytes) {
        return failure(
            ExitCode::InvalidPack, QStringLiteral("cannot_read_trace"),
            QStringLiteral("Trace must be a stable no-follow regular JSON file no larger than 64 "
                           "MiB: %1")
                .arg(trace_bytes.error()),
            QLatin1StringView(command));
    }
    const auto trace_input = packs::SchemaValidator::parseObject(
        QByteArrayView(*trace_bytes), arguments.at(4), packs::JsonLimits{64, 500'000});
    if (!trace_input) {
        return failure(ExitCode::InvalidPack, packErrorCode(trace_input.error().code),
                       trace_input.error().message, QLatin1StringView(command));
    }
    QJsonObject single_trace;
    QJsonArray multi_traces;
    if (multi_trace) {
        const auto expected_profile = QString::fromLatin1(
            packs::realism_evidence_multi_trace_authoring_engine_revision.data(),
            static_cast<qsizetype>(
                packs::realism_evidence_multi_trace_authoring_engine_revision.size()));
        if (trace_input->size() != 2 ||
            trace_input->value(QStringLiteral("profile")).toString() != expected_profile ||
            !trace_input->value(QStringLiteral("traces")).isArray()) {
            return failure(
                ExitCode::InvalidPack, QStringLiteral("invalid_realism_evidence"),
                QStringLiteral("Multi-trace input must contain exactly profile %1 and a traces "
                               "array")
                    .arg(expected_profile),
                QLatin1StringView(command));
        }
        multi_traces = trace_input->value(QStringLiteral("traces")).toArray();
    } else {
        single_trace = *trace_input;
    }
    if (const auto changed = rootChanged(); changed.has_value()) {
        return *changed;
    }

    auto catalog = packs::PackCatalog::open(arguments.at(2));
    if (!catalog) {
        return failure(ExitCode::OperationFailed, catalogErrorCode(catalog.error().code),
                       catalog.error().message, QLatin1StringView(command));
    }
    if (const auto changed = rootChanged(); changed.has_value()) {
        return *changed;
    }

    auto recovered = recoverTransaction(*root_anchor, transaction_name, root_directory,
                                        arguments.at(3), **catalog);
    if (!recovered) {
        const auto immutable = recovered.error().code == RecoveryErrorCode::ImmutableConflict;
        return failure(ExitCode::OperationFailed,
                       immutable ? QStringLiteral("immutable_conflict")
                                 : QStringLiteral("realism_evidence_recovery_conflict"),
                       recovered.error().message, QLatin1StringView(command));
    }
    if (const auto changed = rootChanged(); changed.has_value()) {
        return *changed;
    }
    const auto author = [&]() {
        if (multi_trace) {
            return packs::authorRealismEvidence(
                **catalog, packs::RealismEvidenceTraceSetAuthoringInput{
                               root_directory,
                               arguments.at(3),
                               multi_traces,
                               packs::RealismEvidenceTraceSetProfile::MultiTraceProductionV1,
                           });
        }
        return packs::authorRealismEvidence(**catalog, packs::RealismEvidenceAuthoringInput{
                                                           root_directory,
                                                           arguments.at(3),
                                                           single_trace,
                                                       });
    };
    const auto authored = author();
    if (const auto changed = rootChanged(); changed.has_value()) {
        return *changed;
    }
    if (!authored) {
        if (recovered->has_value()) {
            static_cast<void>(rollbackTransaction(*root_anchor, **recovered));
        }
        const auto error_code = authored.error().code;
        const auto operation_failure =
            error_code == packs::RealismEvidenceAuthoringErrorCode::CatalogFailure ||
            error_code == packs::RealismEvidenceAuthoringErrorCode::ImmutableConflict;
        const auto code = error_code == packs::RealismEvidenceAuthoringErrorCode::ImmutableConflict
                              ? QStringLiteral("immutable_conflict")
                          : operation_failure ? QStringLiteral("realism_evidence_catalog_failure")
                                              : QStringLiteral("invalid_realism_evidence");
        return failure(operation_failure ? ExitCode::OperationFailed : ExitCode::InvalidPack, code,
                       authored.error().message, QLatin1StringView(command));
    }

    const auto original_review =
        readAnchoredRootFile(*root_anchor, authored->review_path, maximum_review_authoring_bytes);
    const auto original_manifest = readAnchoredRootFile(
        *root_anchor, QStringLiteral("manifest.json"), maximum_manifest_authoring_bytes);
    if (!original_review || !original_manifest) {
        if (recovered->has_value()) {
            static_cast<void>(rollbackTransaction(*root_anchor, **recovered));
        }
        return failure(ExitCode::OperationFailed, QStringLiteral("cannot_read_authoring_source"),
                       QStringLiteral("Cannot preserve the source files before authoring"),
                       QLatin1StringView(command));
    }
    if (*original_review != authored->source_review_bytes ||
        *original_manifest != authored->source_manifest_bytes) {
        if (recovered->has_value()) {
            static_cast<void>(rollbackTransaction(*root_anchor, **recovered));
        }
        return failure(ExitCode::OperationFailed, QStringLiteral("authoring_source_changed"),
                       QStringLiteral("The review or manifest changed during authoring"),
                       QLatin1StringView(command));
    }
    bool updated = false;
    std::optional<RealismTransactionHandle> transaction;
    if (recovered->has_value()) {
        transaction = std::move(**recovered);
        const auto& journal = transaction->transaction.journal;
        const auto recovery_matches =
            journal.review_path == authored->review_path &&
            journal.pack_id == fromUtf8(authored->root_revision.id.value) &&
            journal.version == fromUtf8(authored->root_revision.version) &&
            journal.final_digest == fromUtf8(authored->root_revision.digest) &&
            journal.new_review_sha256 == sha256Bytes(QByteArrayView(authored->review_bytes)) &&
            journal.new_manifest_sha256 == sha256Bytes(QByteArrayView(authored->manifest_bytes)) &&
            authored->source_review_bytes == authored->review_bytes &&
            authored->source_manifest_bytes == authored->manifest_bytes;
        if (!recovery_matches) {
            const auto rolled_back = rollbackTransaction(*root_anchor, *transaction);
            return failure(
                ExitCode::OperationFailed,
                rolled_back ? QStringLiteral("realism_evidence_recovery_conflict")
                            : QStringLiteral("realism_evidence_rollback_failed"),
                QStringLiteral("Recovered bytes do not match strict final authoring output"),
                QLatin1StringView(command));
        }
        updated = transaction->transaction.recovery_wrote;
    } else {
        updated = *original_review != authored->review_bytes ||
                  *original_manifest != authored->manifest_bytes;
        if (updated) {
            auto prepared =
                prepareTransaction(*root_anchor, transaction_name, root_directory, *authored);
            if (!prepared) {
                return failure(ExitCode::OperationFailed,
                               QStringLiteral("cannot_prepare_realism_evidence"), prepared.error(),
                               QLatin1StringView(command));
            }
            transaction = std::move(*prepared);
            const auto applied = applyTransaction(*root_anchor, *transaction);
            if (!applied) {
                const auto rolled_back = rollbackTransaction(*root_anchor, *transaction);
                if (const auto changed = rootChanged(); changed.has_value()) {
                    return *changed;
                }
                return failure(ExitCode::OperationFailed,
                               rolled_back ? QStringLiteral("cannot_commit_realism_evidence")
                                           : QStringLiteral("realism_evidence_rollback_failed"),
                               applied.error(), QLatin1StringView(command));
            }
        }
    }

    auto verified = author();
    if (const auto changed = rootChanged(); changed.has_value()) {
        return *changed;
    }
    const auto matches = verified && verified->review_bytes == authored->review_bytes &&
                         verified->manifest_bytes == authored->manifest_bytes &&
                         verified->root_revision == authored->root_revision;
    if (!matches) {
        const auto rolled_back =
            !transaction.has_value() || rollbackTransaction(*root_anchor, *transaction);
        if (const auto changed = rootChanged(); changed.has_value()) {
            return *changed;
        }
        return failure(
            ExitCode::OperationFailed,
            rolled_back ? QStringLiteral("realism_evidence_verification_failed")
                        : QStringLiteral("realism_evidence_rollback_failed"),
            verified
                ? QStringLiteral("Final authored bytes or revision changed during verification")
                : verified.error().message,
            QLatin1StringView(command));
    }
    if (transaction.has_value()) {
        const auto state = transactionState(*root_anchor, transaction->transaction.journal);
        const auto cleaned = state && isFinalState(*state)
                                 ? cleanupTransactionDirectory(*root_anchor, transaction->anchor)
                                 : std::expected<void, QString>{std::unexpected(QStringLiteral(
                                       "Final authored targets no longer match the transaction"))};
        if (!cleaned) {
            if (const auto changed = rootChanged(); changed.has_value()) {
                return *changed;
            }
            return failure(ExitCode::OperationFailed,
                           QStringLiteral("realism_evidence_cleanup_failed"), cleaned.error(),
                           QLatin1StringView(command));
        }
    }

    auto object = revisionObject(verified->root_revision);
    object.insert(QStringLiteral("command"), QLatin1StringView(command));
    object.insert(QStringLiteral("updated"), updated);
    object.insert(QStringLiteral("review_resource_id"), verified->review_resource_id);
    object.insert(QStringLiteral("review_path"), verified->review_path);
    object.insert(QStringLiteral("review_sha256"), verified->review_sha256);
    object.insert(QStringLiteral("case_id"), verified->case_id);
    object.insert(QStringLiteral("closure_digest"), verified->closure_digest);
    object.insert(
        QStringLiteral("evidence_counts"),
        QJsonObject{
            {QStringLiteral("packs"), static_cast<qint64>(verified->counts.packs)},
            {QStringLiteral("resources"), static_cast<qint64>(verified->counts.resources)},
            {QStringLiteral("blobs"), static_cast<qint64>(verified->counts.blobs)},
            {QStringLiteral("traces"), static_cast<qint64>(verified->counts.traces)},
            {QStringLiteral("record_checks"), static_cast<qint64>(verified->counts.record_checks)},
            {QStringLiteral("authorities"), static_cast<qint64>(verified->counts.authorities)},
        });
    if (const auto changed = rootChanged(); changed.has_value()) {
        return *changed;
    }
    return success(std::move(object));
}

constexpr std::array<const char*, 14> template_members{
    "manifest.json",
    "objects/final-order.pdf",
    "resources/argument-config.json",
    "resources/argument-config-counterfactual.json",
    "resources/authority-set.json",
    "resources/bench-configuration.json",
    "resources/case.json",
    "resources/court.json",
    "resources/filing-catalog.json",
    "resources/form.json",
    "resources/judge-profile.json",
    "resources/procedure-profile.json",
    "resources/record.json",
    "resources/workflow.json",
};

[[nodiscard]] bool copyTemplateMember(const QString& member, const QString& staging_root,
                                      QString& error_message) {
    const auto source_path = QStringLiteral(":/appellate/templates/full-resource-pack/") + member;
    QFile source(source_path);
    if (!source.open(QIODevice::ReadOnly)) {
        error_message = QStringLiteral("Cannot read embedded template member: %1").arg(member);
        return false;
    }
    const auto bytes = source.readAll();
    if (source.error() != QFileDevice::NoError) {
        error_message =
            QStringLiteral("Cannot read complete embedded template member: %1").arg(member);
        return false;
    }
    const auto destination_path = QDir(staging_root).filePath(member);
    const auto parent = QFileInfo(destination_path).absolutePath();
    if (!QDir{}.mkpath(parent)) {
        error_message = QStringLiteral("Cannot create template member directory");
        return false;
    }
    QSaveFile destination(destination_path);
    destination.setDirectWriteFallback(false);
    if (!destination.open(QIODevice::WriteOnly) ||
        destination.write(bytes) != static_cast<qint64>(bytes.size()) || !destination.commit()) {
        error_message = QStringLiteral("Cannot write template member: %1").arg(member);
        return false;
    }
    return true;
}

[[nodiscard]] RunResult templateCommand(const QStringList& arguments) {
    constexpr auto command = "template";
    if (arguments.size() != 2) {
        return invalidArguments(QStringLiteral("template requires exactly one destination"),
                                QLatin1StringView(command));
    }
    initializeAppellatePackTemplateResources();
    const auto requested = arguments.at(1);
    if (requested.trimmed().isEmpty()) {
        return invalidArguments(QStringLiteral("template destination cannot be empty"),
                                QLatin1StringView(command));
    }
    const auto destination = QFileInfo(requested).absoluteFilePath();
    const QFileInfo destination_info(destination);
    const auto parent_path = destination_info.absolutePath();
    const auto destination_name = destination_info.fileName();
    if (destination_name.isEmpty()) {
        return invalidArguments(QStringLiteral("template destination must name a directory"),
                                QLatin1StringView(command));
    }
    if (destination_info.exists() || destination_info.isSymLink()) {
        return failure(ExitCode::OperationFailed, QStringLiteral("destination_exists"),
                       QStringLiteral("Template destination already exists"),
                       QLatin1StringView(command));
    }
    if (!QDir{}.mkpath(parent_path)) {
        return failure(ExitCode::OperationFailed, QStringLiteral("cannot_create_destination"),
                       QStringLiteral("Cannot create template destination parent"),
                       QLatin1StringView(command));
    }

    const auto staging_pattern =
        QDir(parent_path)
            .filePath(QStringLiteral(".%1.appellate-template-XXXXXX").arg(destination_name));
    QTemporaryDir staging(staging_pattern);
    if (!staging.isValid()) {
        return failure(ExitCode::OperationFailed, QStringLiteral("cannot_create_staging"),
                       QStringLiteral("Cannot create private template staging directory"),
                       QLatin1StringView(command));
    }
    QString copy_error;
    for (const auto* member : template_members) {
        if (!copyTemplateMember(QLatin1StringView(member), staging.path(), copy_error)) {
            return failure(ExitCode::OperationFailed, QStringLiteral("cannot_extract_template"),
                           copy_error, QLatin1StringView(command));
        }
    }
    const auto validated = packs::PackReader::readDirectory(staging.path());
    if (!validated) {
        return failure(ExitCode::InvalidPack, packErrorCode(validated.error().code),
                       QStringLiteral("Embedded starter template failed validation: %1")
                           .arg(validated.error().message),
                       QLatin1StringView(command));
    }

    QDir parent(parent_path);
    const auto staging_name = QFileInfo(staging.path()).fileName();
    if (QFileInfo::exists(destination) || !parent.rename(staging_name, destination_name)) {
        const auto message = QFileInfo::exists(destination)
                                 ? QStringLiteral("Template destination already exists")
                                 : QStringLiteral("Cannot atomically install template directory");
        const auto code = QFileInfo::exists(destination)
                              ? QStringLiteral("destination_exists")
                              : QStringLiteral("cannot_create_destination");
        return failure(ExitCode::OperationFailed, code, message, QLatin1StringView(command));
    }
    staging.setAutoRemove(false);
    auto object = revisionObject(validated->revision);
    object.insert(QStringLiteral("command"), QLatin1StringView(command));
    object.insert(QStringLiteral("blob_count"), static_cast<qint64>(validated->blobs.size()));
    object.insert(QStringLiteral("resource_count"),
                  static_cast<qint64>(validated->resources.size()));
    return success(std::move(object));
}

} // namespace

RunResult runPackCli(const QStringList& arguments) {
    if (arguments.isEmpty()) {
        return invalidArguments(QStringLiteral("a command is required"));
    }
    const auto command = arguments.front();
    if (command == QStringLiteral("validate")) {
        return validateCommand(arguments);
    }
    if (command == QStringLiteral("export")) {
        return exportCommand(arguments, false);
    }
    if (command == QStringLiteral("export-deferred")) {
        return exportCommand(arguments, true);
    }
    if (command == QStringLiteral("validate-resolved")) {
        return validateResolvedCommand(arguments);
    }
    if (command == QStringLiteral("install")) {
        return installCommand(arguments);
    }
    if (command == QStringLiteral("list")) {
        return listCommand(arguments);
    }
    if (command == QStringLiteral("author-realism-evidence")) {
        return authorRealismEvidenceCommand(
            arguments, packs::RealismEvidenceTraceSetProfile::SingleTraceHelperV1);
    }
    if (command == QStringLiteral("author-realism-evidence-multi")) {
        return authorRealismEvidenceCommand(
            arguments, packs::RealismEvidenceTraceSetProfile::MultiTraceProductionV1);
    }
    if (command == QStringLiteral("template")) {
        return templateCommand(arguments);
    }
    return invalidArguments(QStringLiteral("unknown command: %1").arg(command), command);
}

} // namespace appellate::cli
