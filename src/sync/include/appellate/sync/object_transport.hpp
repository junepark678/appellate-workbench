#pragma once

#include "appellate/sync/object_provider.hpp"
#include "appellate/sync/sync_object.hpp"

#include <QString>
#include <QStringView>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <variant>

class QIODevice;

namespace appellate::sync {

struct ObjectTransportRetryPolicy final {
    static constexpr std::size_t default_maximum_attempts = 3;
    static constexpr std::size_t hard_maximum_attempts = 8;

    std::size_t maximum_attempts{default_maximum_attempts};
};

enum class ObjectTransportStage {
    Configuration,
    Encryption,
    Publication,
    Download,
    Verification,
    SourceRestoration,
};

enum class ObjectTransportLocalErrorCode {
    InvalidConfiguration,
    UnsupportedPlatform,
    CannotCreateStagingFile,
    CannotCreateQuarantineFile,
    CannotPrepareStagingFile,
    InvalidProviderResponse,
    VerificationMismatch,
    CannotRestoreSourcePosition,
};

struct ObjectTransportLocalError final {
    ObjectTransportLocalErrorCode code{};
    QString message;

    friend bool operator==(const ObjectTransportLocalError&,
                           const ObjectTransportLocalError&) = default;
};

using ObjectTransportFailure =
    std::variant<ObjectTransportLocalError, ProviderError, ProtocolError>;

struct ObjectTransportError final {
    ObjectTransportStage stage{};
    // Zero is used only for configuration failures before a provider/protocol attempt exists.
    std::size_t attempt_count{};
    ObjectTransportFailure failure;

    friend bool operator==(const ObjectTransportError&, const ObjectTransportError&) = default;
};

struct PublishedSyncObject final {
    ProviderCreateResult disposition{};
    std::size_t publication_attempts{};
    std::size_t verification_attempts{};
    EncryptedSyncObject encrypted;
    VerifiedSyncObject verified;
};

struct FetchedSyncObject final {
    std::size_t attempts{};
    VerifiedSyncObject verified;
};

class ObjectTransport final {
  public:
    // This hard ceiling bounds a provider-controlled download before any envelope fields have
    // authenticated. It matches the current local provider ceiling and exceeds every ciphertext
    // produced under the protocol's 3 GiB default payload limit.
    static constexpr std::uint64_t maximum_download_ciphertext_bytes =
        4ULL * 1024ULL * 1024ULL * 1024ULL;

    // The current secure directory-capability implementation is available on Unix. Other
    // platforms fail closed with UnsupportedPlatform until they provide equivalent no-follow,
    // handle-relative private temporary files.
    [[nodiscard]] static auto open(ObjectProvider& provider,
                                   const QString& staging_and_quarantine_directory,
                                   ObjectTransportRetryPolicy retry_policy = {})
        -> std::expected<ObjectTransport, ObjectTransportError>;

    [[nodiscard]] auto publish(SyncObjectKind kind, std::uint16_t schema_version,
                               QIODevice& payload, std::uint64_t payload_bytes,
                               const ProtocolKeySet& keys)
        -> std::expected<PublishedSyncObject, ObjectTransportError>;

    [[nodiscard]] auto fetch(QStringView remote_object_id, const ProtocolKeySet& keys) const
        -> std::expected<FetchedSyncObject, ObjectTransportError>;

    [[nodiscard]] const QString& stagingAndQuarantineDirectory() const noexcept;
    [[nodiscard]] ObjectTransportRetryPolicy retryPolicy() const noexcept;

  private:
    class DirectoryHandle;

    ObjectTransport(ObjectProvider& provider, QString directory,
                    ObjectTransportRetryPolicy retry_policy,
                    std::shared_ptr<DirectoryHandle> directory_handle);

    ObjectProvider* provider_{};
    QString directory_;
    ObjectTransportRetryPolicy retry_policy_;
    std::shared_ptr<DirectoryHandle> directory_handle_;
};

} // namespace appellate::sync
