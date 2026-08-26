#include "appellate/sync/local_folder_provider.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QTemporaryFile>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <limits>
#include <system_error>
#include <utility>
#include <vector>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#endif

namespace appellate::sync {
namespace {

constexpr qsizetype copy_buffer_bytes = 64 * 1024;

[[nodiscard]] bool isLowerHex(QChar character) {
    const auto value = character.unicode();
    return (value >= u'0' && value <= u'9') || (value >= u'a' && value <= u'f');
}

[[nodiscard]] bool validRemoteObjectId(QStringView value) {
    return value.size() == 64 &&
           std::ranges::all_of(value, [](QChar character) { return isLowerHex(character); });
}

[[nodiscard]] ProviderError invalidArgument(QString message) {
    return ProviderError{ProviderErrorCode::InvalidArgument, std::move(message)};
}

[[nodiscard]] auto checkedRegularObject(const QString& path, QStringView remote_object_id,
                                        std::uint64_t maximum_bytes)
    -> std::expected<ProviderObjectMetadata, ProviderError> {
    const QFileInfo information(path);
    if (information.isSymLink()) {
        return std::unexpected(ProviderError{ProviderErrorCode::InvalidObject,
                                             QStringLiteral("Sync object cannot be a symlink")});
    }
    if (!information.exists()) {
        return std::unexpected(ProviderError{ProviderErrorCode::NotFound,
                                             QStringLiteral("Sync object was not found")});
    }
    if (!information.isFile()) {
        return std::unexpected(ProviderError{ProviderErrorCode::InvalidObject,
                                             QStringLiteral("Sync object is not a regular file")});
    }
    const auto signed_size = information.size();
    if (signed_size < 0) {
        return std::unexpected(ProviderError{ProviderErrorCode::InvalidObject,
                                             QStringLiteral("Sync object has an invalid size")});
    }
    const auto size = static_cast<std::uint64_t>(signed_size);
    if (size > maximum_bytes) {
        return std::unexpected(ProviderError{ProviderErrorCode::ObjectTooLarge,
                                             QStringLiteral("Sync object exceeds provider limit")});
    }
    return ProviderObjectMetadata{remote_object_id.toString(), size};
}

[[nodiscard]] bool writeAll(QIODevice& destination, const char* data, qsizetype size) {
    qsizetype written{};
    while (written < size) {
        const auto result = destination.write(data + written, size - written);
        if (result <= 0) {
            return false;
        }
        written += result;
    }
    return true;
}

[[nodiscard]] auto copyExact(QIODevice& source, QIODevice& destination, std::uint64_t size,
                             ProviderErrorCode read_error, ProviderErrorCode write_error)
    -> std::expected<void, ProviderError> {
    QByteArray buffer(copy_buffer_bytes, Qt::Uninitialized);
    std::uint64_t copied{};
    while (copied < size) {
        const auto remaining = size - copied;
        const auto requested = static_cast<qsizetype>(
            std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(buffer.size())));
        const auto count = source.read(buffer.data(), requested);
        if (count <= 0) {
            return std::unexpected(
                ProviderError{read_error, QStringLiteral("Cannot read complete sync object")});
        }
        if (!writeAll(destination, buffer.constData(), count)) {
            return std::unexpected(
                ProviderError{write_error, QStringLiteral("Cannot write complete sync object")});
        }
        copied += static_cast<std::uint64_t>(count);
    }
    return {};
}

[[nodiscard]] bool syncFile(QFileDevice& file) {
    if (!file.flush()) {
        return false;
    }
#ifdef Q_OS_UNIX
    return ::fsync(file.handle()) == 0;
#else
    return true;
#endif
}

[[nodiscard]] bool syncDirectory(const QString& directory) {
#ifdef Q_OS_UNIX
    const auto encoded = QFile::encodeName(directory);
    const auto descriptor = ::open(encoded.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) {
        return false;
    }
    const auto result = ::fsync(descriptor);
    static_cast<void>(::close(descriptor));
    return result == 0;
#else
    Q_UNUSED(directory);
    return true;
#endif
}

enum class LinkResult {
    Created,
    AlreadyPresent,
    Failed,
};

[[nodiscard]] LinkResult publishNoReplace(const QString& temporary_path,
                                          const QString& destination_path) {
#ifdef Q_OS_UNIX
    const auto encoded_source = QFile::encodeName(temporary_path);
    const auto encoded_destination = QFile::encodeName(destination_path);
    if (::link(encoded_source.constData(), encoded_destination.constData()) == 0) {
        return LinkResult::Created;
    }
    return errno == EEXIST ? LinkResult::AlreadyPresent : LinkResult::Failed;
#else
    std::error_code error;
#ifdef Q_OS_WIN
    const auto source = std::filesystem::path(temporary_path.toStdWString());
    const auto destination = std::filesystem::path(destination_path.toStdWString());
#else
    const auto source = std::filesystem::path(QFile::encodeName(temporary_path).constData());
    const auto destination = std::filesystem::path(QFile::encodeName(destination_path).constData());
#endif
    std::filesystem::create_hard_link(source, destination, error);
    if (!error) {
        return LinkResult::Created;
    }
    return error == std::errc::file_exists ? LinkResult::AlreadyPresent : LinkResult::Failed;
#endif
}

[[nodiscard]] auto validateDirectory(const QString& path, ProviderErrorCode error_code,
                                     QString message) -> std::expected<void, ProviderError> {
    const QFileInfo information(path);
    if (!information.exists() || !information.isDir() || information.isSymLink()) {
        return std::unexpected(ProviderError{error_code, std::move(message)});
    }
    const auto canonical = information.canonicalFilePath();
    if (canonical.isEmpty() || QDir::cleanPath(canonical) != QDir::cleanPath(path)) {
        return std::unexpected(ProviderError{error_code, std::move(message)});
    }
    return {};
}

} // namespace

LocalFolderProvider::LocalFolderProvider(QString root_directory,
                                         std::uint64_t maximum_ciphertext_bytes)
    : root_directory_(std::move(root_directory)),
      objects_directory_(QDir(root_directory_).filePath(QStringLiteral("objects"))),
      maximum_ciphertext_bytes_(maximum_ciphertext_bytes) {}

auto LocalFolderProvider::open(const QString& root_directory,
                               std::uint64_t maximum_ciphertext_bytes)
    -> std::expected<LocalFolderProvider, ProviderError> {
    if (root_directory.isEmpty() || !QDir::isAbsolutePath(root_directory) ||
        maximum_ciphertext_bytes == 0 ||
        maximum_ciphertext_bytes > static_cast<std::uint64_t>(std::numeric_limits<qint64>::max())) {
        return std::unexpected(invalidArgument(QStringLiteral("Invalid local provider settings")));
    }

    const auto clean_root = QDir::cleanPath(root_directory);
    const QFileInfo original_information(clean_root);
    if (original_information.exists() &&
        (original_information.isSymLink() || !original_information.isDir())) {
        return std::unexpected(ProviderError{ProviderErrorCode::CannotCreateNamespace,
                                             QStringLiteral("Provider root is not a directory")});
    }
    if (!QDir{}.mkpath(clean_root)) {
        return std::unexpected(ProviderError{ProviderErrorCode::CannotCreateNamespace,
                                             QStringLiteral("Cannot create provider root")});
    }
    const auto canonical_root = QFileInfo(clean_root).canonicalFilePath();
    if (canonical_root.isEmpty()) {
        return std::unexpected(ProviderError{ProviderErrorCode::CannotCreateNamespace,
                                             QStringLiteral("Cannot resolve provider root")});
    }
    const auto root_validation =
        validateDirectory(canonical_root, ProviderErrorCode::CannotCreateNamespace,
                          QStringLiteral("Provider root must be a real directory"));
    if (!root_validation) {
        return std::unexpected(root_validation.error());
    }

    const auto objects_directory = QDir(canonical_root).filePath(QStringLiteral("objects"));
    if (!QDir{}.mkpath(objects_directory)) {
        return std::unexpected(ProviderError{ProviderErrorCode::CannotCreateNamespace,
                                             QStringLiteral("Cannot create object namespace")});
    }
    const auto object_validation =
        validateDirectory(objects_directory, ProviderErrorCode::CannotCreateNamespace,
                          QStringLiteral("Object namespace must be a real directory"));
    if (!object_validation) {
        return std::unexpected(object_validation.error());
    }
    return LocalFolderProvider(canonical_root, maximum_ciphertext_bytes);
}

auto LocalFolderProvider::list(QStringView after_remote_object_id, std::size_t limit) const
    -> std::expected<ProviderListPage, ProviderError> {
    if ((!after_remote_object_id.isEmpty() && !validRemoteObjectId(after_remote_object_id)) ||
        limit == 0 || limit > maximum_page_size) {
        return std::unexpected(invalidArgument(QStringLiteral("Invalid provider page request")));
    }
    const auto namespace_validation =
        validateDirectory(objects_directory_, ProviderErrorCode::CannotReadNamespace,
                          QStringLiteral("Object namespace is unavailable"));
    if (!namespace_validation) {
        return std::unexpected(namespace_validation.error());
    }

    std::vector<ProviderObjectMetadata> candidates;
    const auto after = after_remote_object_id.toString();
    QDirIterator prefixes(objects_directory_,
                          QDir::AllEntries | QDir::System | QDir::Hidden | QDir::NoDotAndDotDot);
    while (prefixes.hasNext()) {
        static_cast<void>(prefixes.next());
        const auto prefix = prefixes.fileInfo();
        const auto prefix_name = prefix.fileName();
        if (prefix.isSymLink() || !prefix.isDir() || prefix_name.size() != 2 ||
            !std::ranges::all_of(prefix_name,
                                 [](QChar character) { return isLowerHex(character); })) {
            return std::unexpected(ProviderError{ProviderErrorCode::CannotReadNamespace,
                                                 QStringLiteral("Malformed provider namespace")});
        }
        const auto prefix_validation =
            validateDirectory(prefix.absoluteFilePath(), ProviderErrorCode::CannotReadNamespace,
                              QStringLiteral("Provider prefix is unavailable"));
        if (!prefix_validation) {
            return std::unexpected(prefix_validation.error());
        }
        QDirIterator files(prefix.absoluteFilePath(),
                           QDir::AllEntries | QDir::System | QDir::Hidden | QDir::NoDotAndDotDot);
        while (files.hasNext()) {
            static_cast<void>(files.next());
            const auto file = files.fileInfo();
            if (file.fileName().startsWith(QStringLiteral(".upload-"))) {
                if (file.isSymLink() || !file.isFile()) {
                    return std::unexpected(
                        ProviderError{ProviderErrorCode::CannotReadNamespace,
                                      QStringLiteral("Malformed upload staging object")});
                }
                continue;
            }
            const auto name = file.fileName();
            if (!name.endsWith(QStringLiteral(".awobj"))) {
                return std::unexpected(ProviderError{ProviderErrorCode::CannotReadNamespace,
                                                     QStringLiteral("Unexpected provider object")});
            }
            const auto remote_id = name.first(name.size() - 6);
            if (!validRemoteObjectId(remote_id) || !remote_id.startsWith(prefix_name)) {
                return std::unexpected(ProviderError{ProviderErrorCode::CannotReadNamespace,
                                                     QStringLiteral("Malformed provider object")});
            }
            const auto metadata =
                checkedRegularObject(file.absoluteFilePath(), remote_id, maximum_ciphertext_bytes_);
            if (!metadata) {
                return std::unexpected(metadata.error());
            }
            if (metadata->remote_object_id <= after) {
                continue;
            }
            const auto insertion =
                std::ranges::lower_bound(candidates, metadata->remote_object_id, {},
                                         &ProviderObjectMetadata::remote_object_id);
            candidates.insert(insertion, *metadata);
            if (candidates.size() > limit + 1) {
                candidates.pop_back();
            }
        }
    }

    ProviderListPage page;
    const auto page_size = std::min(limit, candidates.size());
    page.objects.assign(candidates.begin(),
                        candidates.begin() + static_cast<std::ptrdiff_t>(page_size));
    if (candidates.size() > limit && !page.objects.empty()) {
        page.continuation_token = page.objects.back().remote_object_id;
    }
    return page;
}

auto LocalFolderProvider::stat(QStringView remote_object_id) const
    -> std::expected<ProviderObjectMetadata, ProviderError> {
    if (!validRemoteObjectId(remote_object_id)) {
        return std::unexpected(invalidArgument(QStringLiteral("Invalid remote object ID")));
    }
    return checkedRegularObject(objectPath(remote_object_id), remote_object_id,
                                maximum_ciphertext_bytes_);
}

auto LocalFolderProvider::createIfAbsent(QStringView remote_object_id, QIODevice& ciphertext,
                                         std::uint64_t ciphertext_bytes)
    -> std::expected<ProviderCreateResult, ProviderError> {
    if (!validRemoteObjectId(remote_object_id) || !ciphertext.isOpen() ||
        !ciphertext.isReadable() || ciphertext.isSequential() || ciphertext.pos() < 0 ||
        ciphertext.size() < ciphertext.pos() ||
        static_cast<std::uint64_t>(ciphertext.size() - ciphertext.pos()) != ciphertext_bytes) {
        return std::unexpected(invalidArgument(QStringLiteral("Invalid provider upload")));
    }
    if (ciphertext_bytes > maximum_ciphertext_bytes_) {
        return std::unexpected(ProviderError{ProviderErrorCode::ObjectTooLarge,
                                             QStringLiteral("Sync object exceeds provider limit")});
    }

    const auto destination = objectPath(remote_object_id);
    if (QFileInfo::exists(destination)) {
        const auto existing = stat(remote_object_id);
        if (!existing) {
            return std::unexpected(existing.error());
        }
        return ProviderCreateResult::AlreadyPresent;
    }

    const auto prefix = prefixDirectory(remote_object_id);
    if (!QDir{}.mkpath(prefix)) {
        return std::unexpected(ProviderError{ProviderErrorCode::CannotCreateNamespace,
                                             QStringLiteral("Cannot create provider prefix")});
    }
    const auto prefix_validation =
        validateDirectory(prefix, ProviderErrorCode::CannotCreateNamespace,
                          QStringLiteral("Provider prefix must be a real directory"));
    if (!prefix_validation) {
        return std::unexpected(prefix_validation.error());
    }

    QTemporaryFile temporary(QDir(prefix).filePath(QStringLiteral(".upload-XXXXXX")));
    temporary.setAutoRemove(true);
    if (!temporary.open()) {
        return std::unexpected(ProviderError{ProviderErrorCode::PublicationFailed,
                                             QStringLiteral("Cannot create upload staging file")});
    }
    if (!temporary.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        return std::unexpected(ProviderError{ProviderErrorCode::PublicationFailed,
                                             QStringLiteral("Cannot restrict upload permissions")});
    }
    const auto copied =
        copyExact(ciphertext, temporary, ciphertext_bytes, ProviderErrorCode::SourceReadFailed,
                  ProviderErrorCode::DestinationWriteFailed);
    if (!copied) {
        return std::unexpected(copied.error());
    }
    if (!syncFile(temporary)) {
        return std::unexpected(ProviderError{ProviderErrorCode::PublicationFailed,
                                             QStringLiteral("Cannot persist staged sync object")});
    }

    const auto link_result = publishNoReplace(temporary.fileName(), destination);
    if (link_result == LinkResult::AlreadyPresent) {
        const auto existing = stat(remote_object_id);
        if (!existing) {
            return std::unexpected(existing.error());
        }
        return ProviderCreateResult::AlreadyPresent;
    }
    if (link_result == LinkResult::Failed) {
        return std::unexpected(ProviderError{ProviderErrorCode::PublicationFailed,
                                             QStringLiteral("Cannot publish sync object")});
    }
    if (!syncDirectory(prefix)) {
        return std::unexpected(ProviderError{ProviderErrorCode::PublicationFailed,
                                             QStringLiteral("Cannot persist provider directory")});
    }
    return ProviderCreateResult::Created;
}

auto LocalFolderProvider::download(QStringView remote_object_id, QIODevice& destination) const
    -> std::expected<ProviderObjectMetadata, ProviderError> {
    if (!validRemoteObjectId(remote_object_id) || !destination.isOpen() ||
        !destination.isWritable()) {
        return std::unexpected(invalidArgument(QStringLiteral("Invalid provider download")));
    }
    const auto metadata = stat(remote_object_id);
    if (!metadata) {
        return std::unexpected(metadata.error());
    }
    QFile source(objectPath(remote_object_id));
    if (!source.open(QIODevice::ReadOnly)) {
        return std::unexpected(ProviderError{ProviderErrorCode::CannotReadNamespace,
                                             QStringLiteral("Cannot open sync object")});
    }
    const auto copied = copyExact(source, destination, metadata->ciphertext_bytes,
                                  ProviderErrorCode::CannotReadNamespace,
                                  ProviderErrorCode::DestinationWriteFailed);
    if (!copied) {
        return std::unexpected(copied.error());
    }
    if (!source.atEnd()) {
        return std::unexpected(
            ProviderError{ProviderErrorCode::InvalidObject,
                          QStringLiteral("Sync object changed during download")});
    }
    return *metadata;
}

const QString& LocalFolderProvider::rootDirectory() const noexcept { return root_directory_; }

QString LocalFolderProvider::objectPath(QStringView remote_object_id) const {
    return QDir(prefixDirectory(remote_object_id))
        .filePath(remote_object_id.toString() + QStringLiteral(".awobj"));
}

QString LocalFolderProvider::prefixDirectory(QStringView remote_object_id) const {
    return QDir(objects_directory_).filePath(remote_object_id.first(2).toString());
}

} // namespace appellate::sync
