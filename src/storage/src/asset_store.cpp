#include "appellate/storage/asset_store.hpp"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QRegularExpression>
#include <QTemporaryFile>

#include <algorithm>
#include <array>
#include <cerrno>
#include <utility>

#if defined(Q_OS_UNIX)
#include <fcntl.h>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <io.h>
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

[[nodiscard]] QString objectPath(const QString& objects_directory, QStringView digest) {
    return QDir(objects_directory).filePath(digest.toString());
}

} // namespace

AssetStore::AssetStore(QString root_directory, qint64 max_asset_bytes)
    : root_directory_(std::move(root_directory)), max_asset_bytes_(max_asset_bytes) {
    if (!root_directory_.isEmpty()) {
        root_directory_ = QDir(root_directory_).absolutePath();
    }
}

std::expected<void, AssetStoreError> AssetStore::validateConfiguration() const {
    if (root_directory_.isEmpty()) {
        return fail(AssetStoreErrorCode::InvalidConfiguration,
                    QStringLiteral("The asset-store root directory is empty"));
    }
    if (max_asset_bytes_ <= 0) {
        return fail(AssetStoreErrorCode::InvalidConfiguration,
                    QStringLiteral("The maximum asset size must be positive"));
    }
    return {};
}

QString AssetStore::objectsDirectory() const {
    return QDir(root_directory_).filePath(QStringLiteral("objects"));
}

qint64 AssetStore::maxAssetBytes() const noexcept { return max_asset_bytes_; }

std::expected<StoredAsset, AssetStoreError> AssetStore::put(QByteArrayView bytes) {
    if (const auto configuration = validateConfiguration(); !configuration) {
        return std::unexpected(configuration.error());
    }
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
    return put(source);
}

std::expected<StoredAsset, AssetStoreError> AssetStore::put(QIODevice& source) {
    if (const auto configuration = validateConfiguration(); !configuration) {
        return std::unexpected(configuration.error());
    }
    if (!source.isOpen() || !source.isReadable()) {
        return fail(AssetStoreErrorCode::CannotRead,
                    QStringLiteral("The asset source is not open for reading"));
    }

    const auto objects_directory = objectsDirectory();
    if (!QDir().mkpath(objects_directory)) {
        return fail(AssetStoreErrorCode::CannotCreateDirectory,
                    QStringLiteral("Cannot create asset directory %1").arg(objects_directory));
    }

    QTemporaryFile temporary(QDir(objects_directory).filePath(QStringLiteral(".asset-XXXXXX.tmp")));
    temporary.setAutoRemove(true);
    if (!temporary.open()) {
        return fail(AssetStoreErrorCode::CannotWrite,
                    QStringLiteral("Cannot create a temporary asset in %1").arg(objects_directory));
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 total_size{};
    while (true) {
        const auto chunk = source.read(io_buffer_bytes);
        if (chunk.isEmpty()) {
            if (source.atEnd()) {
                break;
            }
            return fail(AssetStoreErrorCode::CannotRead,
                        QStringLiteral("Cannot read the complete asset source"));
        }

        const auto chunk_size = static_cast<qint64>(chunk.size());
        if (chunk_size > max_asset_bytes_ - total_size) {
            return fail(
                AssetStoreErrorCode::AssetTooLarge,
                QStringLiteral("Asset exceeds the configured %1-byte limit").arg(max_asset_bytes_));
        }
        if (temporary.write(chunk) != chunk.size()) {
            return fail(AssetStoreErrorCode::CannotWrite,
                        QStringLiteral("Cannot write the temporary asset"));
        }
        hash.addData(chunk);
        total_size += chunk_size;
    }

    if (!syncFile(temporary)) {
        return fail(AssetStoreErrorCode::CannotSync,
                    QStringLiteral("Cannot durably flush the temporary asset"));
    }
    temporary.close();

    const auto digest = QString::fromLatin1(hash.result().toHex());
    const auto final_path = objectPath(objects_directory, digest);

    if (QFileInfo::exists(final_path)) {
        const auto existing = read(digest);
        if (!existing) {
            return std::unexpected(existing.error());
        }
        return StoredAsset{digest, total_size, true};
    }

    const auto temporary_path = temporary.fileName();
    if (!QFile::rename(temporary_path, final_path)) {
        // Another writer may have installed the same immutable object after our existence check.
        const auto existing = read(digest);
        if (!existing) {
            if (existing.error().code == AssetStoreErrorCode::NotFound) {
                return fail(AssetStoreErrorCode::CannotCommit,
                            QStringLiteral("Cannot atomically commit asset %1").arg(digest));
            }
            return std::unexpected(existing.error());
        }
        return StoredAsset{digest, total_size, true};
    }

    if (!syncDirectory(objects_directory)) {
        return fail(AssetStoreErrorCode::CannotSync,
                    QStringLiteral("Cannot durably flush the asset directory"));
    }
    return StoredAsset{digest, total_size, false};
}

std::expected<QByteArray, AssetStoreError> AssetStore::read(QStringView sha256) const {
    if (const auto configuration = validateConfiguration(); !configuration) {
        return std::unexpected(configuration.error());
    }
    if (!isLowercaseSha256(sha256)) {
        return fail(AssetStoreErrorCode::InvalidDigest,
                    QStringLiteral("Asset IDs must be lowercase SHA-256 digests"));
    }

    const auto digest = sha256.toString();
    const auto path = objectPath(objectsDirectory(), digest);
    const QFileInfo information(path);
    if (!information.exists()) {
        return fail(AssetStoreErrorCode::NotFound,
                    QStringLiteral("Asset %1 does not exist").arg(digest));
    }
    if (!information.isFile() || information.isSymLink()) {
        return fail(AssetStoreErrorCode::CorruptObject,
                    QStringLiteral("Asset %1 is not a regular object file").arg(digest));
    }
    if (information.size() > max_asset_bytes_) {
        return fail(AssetStoreErrorCode::AssetTooLarge,
                    QStringLiteral("Asset %1 exceeds the configured size limit").arg(digest));
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(AssetStoreErrorCode::CannotRead,
                    QStringLiteral("Cannot read asset %1").arg(digest));
    }

    QByteArray contents;
    contents.reserve(static_cast<qsizetype>(information.size()));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 total_size{};
    while (true) {
        const auto chunk = file.read(io_buffer_bytes);
        if (chunk.isEmpty()) {
            if (file.atEnd()) {
                break;
            }
            return fail(AssetStoreErrorCode::CannotRead,
                        QStringLiteral("Cannot read the complete asset %1").arg(digest));
        }

        const auto chunk_size = static_cast<qint64>(chunk.size());
        if (chunk_size > max_asset_bytes_ - total_size) {
            return fail(AssetStoreErrorCode::AssetTooLarge,
                        QStringLiteral("Asset %1 exceeds the configured size limit").arg(digest));
        }
        hash.addData(chunk);
        contents.append(chunk);
        total_size += chunk_size;
    }

    const auto actual_digest = QString::fromLatin1(hash.result().toHex());
    if (actual_digest != digest) {
        return fail(AssetStoreErrorCode::CorruptObject,
                    QStringLiteral("Asset %1 failed digest verification").arg(digest));
    }
    return contents;
}

} // namespace appellate::storage
