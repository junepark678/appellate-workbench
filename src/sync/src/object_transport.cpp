#include "appellate/sync/object_transport.hpp"

#include "appellate/sync/protocol_codec.hpp"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QIODevice>
#include <QTemporaryFile>

#include <sodium.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <utility>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace appellate::sync {

class ObjectTransport::DirectoryHandle final {
  public:
    DirectoryHandle(const DirectoryHandle&) = delete;
    DirectoryHandle& operator=(const DirectoryHandle&) = delete;

#ifdef Q_OS_UNIX
    ~DirectoryHandle() {
        if (descriptor_ >= 0) {
            static_cast<void>(::close(descriptor_));
        }
    }

    [[nodiscard]] static auto open(QStringView absolute_clean_path)
        -> std::expected<std::shared_ptr<DirectoryHandle>, QString> {
        auto descriptor = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (descriptor < 0) {
            return std::unexpected(QStringLiteral("Cannot open the filesystem root safely"));
        }

        const auto components = absolute_clean_path.toString().split('/', Qt::SkipEmptyParts);
        for (const auto& component : components) {
            const auto encoded = QFile::encodeName(component);
            const auto next = ::openat(descriptor, encoded.constData(),
                                       O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            static_cast<void>(::close(descriptor));
            if (next < 0) {
                return std::unexpected(
                    QStringLiteral("Cannot open the staging directory without symlinks"));
            }
            descriptor = next;
        }

        struct stat status{};
        if (::fstat(descriptor, &status) != 0 || !S_ISDIR(status.st_mode)) {
            static_cast<void>(::close(descriptor));
            return std::unexpected(QStringLiteral("The staging handle is not a directory"));
        }
        return std::shared_ptr<DirectoryHandle>(new DirectoryHandle(descriptor));
    }

    [[nodiscard]] auto createUnlinkedFile() const
        -> std::expected<std::unique_ptr<QTemporaryFile>, QString> {
        if (sodium_init() < 0) {
            return std::unexpected(
                QStringLiteral("Cannot initialize secure temporary file naming"));
        }

        constexpr std::size_t maximum_name_attempts = 32;
        for (std::size_t attempt = 0; attempt < maximum_name_attempts; ++attempt) {
            std::array<unsigned char, 16> random_name{};
            randombytes_buf(random_name.data(), random_name.size());
            const auto suffix = QByteArray(reinterpret_cast<const char*>(random_name.data()),
                                           static_cast<qsizetype>(random_name.size()))
                                    .toHex();
            const auto name =
                QByteArrayLiteral(".appellate-sync-") + suffix + QByteArrayLiteral(".temporary");
            const auto file_descriptor =
                ::openat(descriptor_, name.constData(),
                         O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
            if (file_descriptor < 0) {
                if (errno == EEXIST || errno == EINTR) {
                    continue;
                }
                return std::unexpected(
                    QStringLiteral("Cannot create a descriptor-relative temporary file"));
            }

            struct stat status{};
            const auto private_mode = static_cast<mode_t>(S_IRUSR | S_IWUSR);
            const auto prepared = ::fchmod(file_descriptor, private_mode) == 0 &&
                                  ::fstat(file_descriptor, &status) == 0 &&
                                  S_ISREG(status.st_mode) &&
                                  (status.st_mode & static_cast<mode_t>(0777)) == private_mode;
            if (!prepared) {
                static_cast<void>(::unlinkat(descriptor_, name.constData(), 0));
                static_cast<void>(::close(file_descriptor));
                return std::unexpected(QStringLiteral("Cannot prepare a private temporary file"));
            }

            if (::unlinkat(descriptor_, name.constData(), 0) != 0 ||
                ::fstat(file_descriptor, &status) != 0 || status.st_nlink != 0) {
                static_cast<void>(::unlinkat(descriptor_, name.constData(), 0));
                static_cast<void>(::close(file_descriptor));
                return std::unexpected(
                    QStringLiteral("Cannot unlink a private temporary file safely"));
            }

            auto file = std::make_unique<QTemporaryFile>();
            file->setAutoRemove(false);
            if (!file->QFile::open(file_descriptor, QIODevice::ReadWrite,
                                   QFileDevice::AutoCloseHandle)) {
                static_cast<void>(::close(file_descriptor));
                return std::unexpected(
                    QStringLiteral("Cannot adopt a private temporary file handle"));
            }
            return file;
        }
        return std::unexpected(QStringLiteral("Cannot allocate a unique private temporary file"));
    }

  private:
    explicit DirectoryHandle(int descriptor) : descriptor_(descriptor) {}

    int descriptor_{-1};
#else
    ~DirectoryHandle() = default;
#endif
};

namespace {

class BoundedWriteDevice final : public QIODevice {
  public:
    BoundedWriteDevice(QFileDevice& destination, std::uint64_t maximum_bytes)
        : destination_(destination), maximum_bytes_(maximum_bytes) {
        static_cast<void>(open(QIODevice::WriteOnly));
    }

    [[nodiscard]] bool limitExceeded() const noexcept { return limit_exceeded_; }
    [[nodiscard]] std::uint64_t bytesWritten() const noexcept { return bytes_written_; }
    [[nodiscard]] bool isSequential() const override { return true; }

  protected:
    [[nodiscard]] qint64 readData(char*, qint64) override { return -1; }

    [[nodiscard]] qint64 writeData(const char* data, qint64 maximum_size) override {
        if (maximum_size < 0) {
            return -1;
        }
        const auto requested = static_cast<std::uint64_t>(maximum_size);
        if (requested > maximum_bytes_ - bytes_written_) {
            limit_exceeded_ = true;
            return -1;
        }
        const auto written = destination_.write(data, maximum_size);
        if (written > 0) {
            bytes_written_ += static_cast<std::uint64_t>(written);
        }
        return written;
    }

  private:
    QFileDevice& destination_;
    std::uint64_t maximum_bytes_{};
    std::uint64_t bytes_written_{};
    bool limit_exceeded_{};
};

[[nodiscard]] auto localFailure(ObjectTransportStage stage, std::size_t attempt_count,
                                ObjectTransportLocalErrorCode code, QString message)
    -> std::unexpected<ObjectTransportError> {
    return std::unexpected(ObjectTransportError{
        stage, attempt_count, ObjectTransportLocalError{code, std::move(message)}});
}

[[nodiscard]] auto providerFailure(ObjectTransportStage stage, std::size_t attempt_count,
                                   ProviderError error) -> std::unexpected<ObjectTransportError> {
    return std::unexpected(
        ObjectTransportError{stage, attempt_count, ObjectTransportFailure{std::move(error)}});
}

[[nodiscard]] auto protocolFailure(ObjectTransportStage stage, std::size_t attempt_count,
                                   ProtocolError error) -> std::unexpected<ObjectTransportError> {
    return std::unexpected(
        ObjectTransportError{stage, attempt_count, ObjectTransportFailure{std::move(error)}});
}

[[nodiscard]] bool retryablePublication(const ProviderError& error) {
    return error.retryability == ProviderErrorRetryability::Transient &&
           error.code == ProviderErrorCode::PublicationFailed;
}

[[nodiscard]] bool retryableDownload(const ProviderError& error) {
    return error.retryability == ProviderErrorRetryability::Transient &&
           error.code == ProviderErrorCode::CannotReadNamespace;
}

[[nodiscard]] bool validProviderMetadata(const ProviderObjectMetadata& metadata,
                                         QStringView requested_remote_object_id,
                                         std::uint64_t downloaded_bytes, qint64 staged_size) {
    return metadata.remote_object_id == requested_remote_object_id &&
           metadata.ciphertext_bytes == downloaded_bytes && staged_size >= 0 &&
           static_cast<std::uint64_t>(staged_size) == downloaded_bytes;
}

[[nodiscard]] bool samePublishedObject(const EncryptedSyncObject& encrypted,
                                       const VerifiedSyncObject& verified) {
    return encrypted.kind == verified.kind && encrypted.schema_version == verified.schema_version &&
           encrypted.identity == verified.identity &&
           encrypted.payload_bytes == verified.payload_bytes &&
           encrypted.padded_plaintext_bytes == verified.padded_plaintext_bytes;
}

} // namespace

ObjectTransport::ObjectTransport(ObjectProvider& provider, QString directory,
                                 ObjectTransportRetryPolicy retry_policy,
                                 std::shared_ptr<DirectoryHandle> directory_handle)
    : provider_(&provider), directory_(std::move(directory)), retry_policy_(retry_policy),
      directory_handle_(std::move(directory_handle)) {}

auto ObjectTransport::open(ObjectProvider& provider,
                           const QString& staging_and_quarantine_directory,
                           ObjectTransportRetryPolicy retry_policy)
    -> std::expected<ObjectTransport, ObjectTransportError> {
#ifndef Q_OS_UNIX
    static_cast<void>(provider);
    static_cast<void>(staging_and_quarantine_directory);
    static_cast<void>(retry_policy);
    return localFailure(ObjectTransportStage::Configuration, 0,
                        ObjectTransportLocalErrorCode::UnsupportedPlatform,
                        QStringLiteral("Secure object transport is unavailable on this platform"));
#else
    if (staging_and_quarantine_directory.isEmpty() ||
        !QDir::isAbsolutePath(staging_and_quarantine_directory) ||
        retry_policy.maximum_attempts == 0 ||
        retry_policy.maximum_attempts > ObjectTransportRetryPolicy::hard_maximum_attempts) {
        return localFailure(ObjectTransportStage::Configuration, 0,
                            ObjectTransportLocalErrorCode::InvalidConfiguration,
                            QStringLiteral("Invalid object transport configuration"));
    }

    const auto clean = QDir::cleanPath(staging_and_quarantine_directory);
    const QFileInfo information(clean);
    const auto canonical = information.canonicalFilePath();
    if (clean != staging_and_quarantine_directory || clean == QDir::rootPath() ||
        !information.exists() || !information.isDir() || information.isSymLink() ||
        canonical.isEmpty() || canonical != clean) {
        return localFailure(ObjectTransportStage::Configuration, 0,
                            ObjectTransportLocalErrorCode::InvalidConfiguration,
                            QStringLiteral("Staging directory must be an absolute real directory"));
    }

    auto directory_handle = DirectoryHandle::open(canonical);
    if (!directory_handle) {
        return localFailure(ObjectTransportStage::Configuration, 0,
                            ObjectTransportLocalErrorCode::InvalidConfiguration,
                            std::move(directory_handle.error()));
    }
    return ObjectTransport(provider, canonical, retry_policy, std::move(*directory_handle));
#endif
}

auto ObjectTransport::publish(SyncObjectKind kind, std::uint16_t schema_version, QIODevice& payload,
                              std::uint64_t payload_bytes, const ProtocolKeySet& keys)
    -> std::expected<PublishedSyncObject, ObjectTransportError> {
#ifndef Q_OS_UNIX
    static_cast<void>(kind);
    static_cast<void>(schema_version);
    static_cast<void>(payload);
    static_cast<void>(payload_bytes);
    static_cast<void>(keys);
    return localFailure(ObjectTransportStage::Configuration, 0,
                        ObjectTransportLocalErrorCode::UnsupportedPlatform,
                        QStringLiteral("Secure object transport is unavailable on this platform"));
#else
    const auto initial_position = payload.pos();
    const auto position_can_be_restored = !payload.isSequential() && initial_position >= 0;
    auto staged = directory_handle_->createUnlinkedFile();
    if (!staged) {
        return localFailure(ObjectTransportStage::Encryption, 1,
                            ObjectTransportLocalErrorCode::CannotCreateStagingFile,
                            std::move(staged.error()));
    }

    auto encrypted =
        ProtocolCodec::encrypt(kind, schema_version, payload, payload_bytes, keys, **staged);
    if (position_can_be_restored && payload.pos() != initial_position &&
        !payload.seek(initial_position)) {
        return localFailure(ObjectTransportStage::SourceRestoration, 1,
                            ObjectTransportLocalErrorCode::CannotRestoreSourcePosition,
                            QStringLiteral("Cannot restore the caller payload position"));
    }
    if (!encrypted) {
        return protocolFailure(ObjectTransportStage::Encryption, 1, std::move(encrypted.error()));
    }
    if (!(**staged).flush() || (**staged).size() < 0 ||
        static_cast<std::uint64_t>((**staged).size()) != encrypted->ciphertext_bytes) {
        return localFailure(ObjectTransportStage::Encryption, 1,
                            ObjectTransportLocalErrorCode::CannotPrepareStagingFile,
                            QStringLiteral("Cannot finalize encrypted object staging"));
    }

    ProviderCreateResult disposition{};
    std::size_t publication_attempts{};
    for (std::size_t attempt = 1; attempt <= retry_policy_.maximum_attempts; ++attempt) {
        publication_attempts = attempt;
        if (!(**staged).seek(0)) {
            return localFailure(ObjectTransportStage::Publication, attempt,
                                ObjectTransportLocalErrorCode::CannotPrepareStagingFile,
                                QStringLiteral("Cannot rewind encrypted object staging"));
        }
        auto published = provider_->createIfAbsent(encrypted->identity.remote_object_id, **staged,
                                                   encrypted->ciphertext_bytes);
        if (published) {
            disposition = *published;
            break;
        }
        if (retryablePublication(published.error()) && attempt < retry_policy_.maximum_attempts) {
            continue;
        }
        return providerFailure(ObjectTransportStage::Publication, attempt,
                               std::move(published.error()));
    }

    auto fetched = fetch(encrypted->identity.remote_object_id, keys);
    if (!fetched) {
        return std::unexpected(fetched.error());
    }
    if (!samePublishedObject(*encrypted, fetched->verified)) {
        return localFailure(ObjectTransportStage::Verification, fetched->attempts,
                            ObjectTransportLocalErrorCode::VerificationMismatch,
                            QStringLiteral("Published object does not match the caller payload"));
    }
    return PublishedSyncObject{disposition, publication_attempts, fetched->attempts,
                               std::move(*encrypted), std::move(fetched->verified)};
#endif
}

auto ObjectTransport::fetch(QStringView remote_object_id, const ProtocolKeySet& keys) const
    -> std::expected<FetchedSyncObject, ObjectTransportError> {
#ifndef Q_OS_UNIX
    static_cast<void>(remote_object_id);
    static_cast<void>(keys);
    return localFailure(ObjectTransportStage::Configuration, 0,
                        ObjectTransportLocalErrorCode::UnsupportedPlatform,
                        QStringLiteral("Secure object transport is unavailable on this platform"));
#else
    auto staged = directory_handle_->createUnlinkedFile();
    if (!staged) {
        return localFailure(ObjectTransportStage::Download, 1,
                            ObjectTransportLocalErrorCode::CannotCreateStagingFile,
                            std::move(staged.error()));
    }

    for (std::size_t attempt = 1; attempt <= retry_policy_.maximum_attempts; ++attempt) {
        if (!(**staged).resize(0) || !(**staged).seek(0)) {
            return localFailure(ObjectTransportStage::Download, attempt,
                                ObjectTransportLocalErrorCode::CannotPrepareStagingFile,
                                QStringLiteral("Cannot reset ciphertext download staging"));
        }

        BoundedWriteDevice destination(**staged,
                                       ObjectTransport::maximum_download_ciphertext_bytes);
        auto downloaded = provider_->download(remote_object_id, destination);
        destination.close();

        if (destination.limitExceeded()) {
            return localFailure(ObjectTransportStage::Download, attempt,
                                ObjectTransportLocalErrorCode::InvalidProviderResponse,
                                QStringLiteral("Provider exceeded the ciphertext download limit"));
        }
        if (!downloaded) {
            if (retryableDownload(downloaded.error()) && attempt < retry_policy_.maximum_attempts) {
                continue;
            }
            return providerFailure(ObjectTransportStage::Download, attempt,
                                   std::move(downloaded.error()));
        }
        if (!validProviderMetadata(*downloaded, remote_object_id, destination.bytesWritten(),
                                   (**staged).size())) {
            return localFailure(ObjectTransportStage::Download, attempt,
                                ObjectTransportLocalErrorCode::InvalidProviderResponse,
                                QStringLiteral("Provider returned inconsistent object bytes"));
        }
        if (!(**staged).flush() || !(**staged).seek(0)) {
            return localFailure(ObjectTransportStage::Download, attempt,
                                ObjectTransportLocalErrorCode::CannotPrepareStagingFile,
                                QStringLiteral("Cannot finalize ciphertext download staging"));
        }

        auto quarantine = directory_handle_->createUnlinkedFile();
        if (!quarantine) {
            return localFailure(ObjectTransportStage::Verification, attempt,
                                ObjectTransportLocalErrorCode::CannotCreateQuarantineFile,
                                std::move(quarantine.error()));
        }
        auto verified = ProtocolCodec::decryptIntoQuarantine(**staged, remote_object_id, keys,
                                                             std::move(*quarantine));
        if (!verified) {
            return protocolFailure(ObjectTransportStage::Verification, attempt,
                                   std::move(verified.error()));
        }
        return FetchedSyncObject{attempt, std::move(*verified)};
    }

    return localFailure(ObjectTransportStage::Download, retry_policy_.maximum_attempts,
                        ObjectTransportLocalErrorCode::InvalidProviderResponse,
                        QStringLiteral("The bounded download loop ended unexpectedly"));
#endif
}

const QString& ObjectTransport::stagingAndQuarantineDirectory() const noexcept {
    return directory_;
}

ObjectTransportRetryPolicy ObjectTransport::retryPolicy() const noexcept { return retry_policy_; }

} // namespace appellate::sync
