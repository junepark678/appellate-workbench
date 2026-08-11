#pragma once

#include "appellate/sync/recovery_capsule.hpp"
#include "appellate/sync/secret_bytes.hpp"
#include "appellate/sync/vault_keyring.hpp"

#include <QString>
#include <QStringView>

#include <expected>
#include <span>

namespace appellate::sync {

enum class SecretStoreErrorCode {
    Unavailable,
    NotFound,
    ReadFailed,
    WriteFailed,
};

struct SecretStoreError final {
    SecretStoreErrorCode code{};
    QString message;

    friend bool operator==(const SecretStoreError&, const SecretStoreError&) = default;
};

// Production callers inject a supported OS-backed implementation with insecure fallback disabled.
// This interface deliberately has no environment, settings-file, or plaintext fallback adapter.
class SecretStore {
  public:
    virtual ~SecretStore() = default;

    [[nodiscard]] virtual auto read(QStringView opaque_reference)
        -> std::expected<SecretBytes, SecretStoreError> = 0;
    // Implementations must synchronously consume or copy this view into protected OS-store
    // handling. Retaining the view after write() returns is forbidden. Replacement is
    // all-or-nothing: on every error, any prior value at this reference must remain byte-for-byte
    // readable. An adapter that cannot guarantee that postcondition is unsupported and must fail
    // closed before modifying the store.
    [[nodiscard]] virtual auto write(QStringView opaque_reference,
                                     std::span<const unsigned char> secret)
        -> std::expected<void, SecretStoreError> = 0;
};

enum class VaultKeyringStoreErrorCode {
    InvalidReference,
    SecretStoreUnavailable,
    SecretNotFound,
    SecretStoreReadFailed,
    SecretStoreWriteFailed,
    InvalidStoredKeyring,
    RecoveryRejected,
};

struct VaultKeyringStoreError final {
    VaultKeyringStoreErrorCode code{};
    QString message;

    friend bool operator==(const VaultKeyringStoreError&, const VaultKeyringStoreError&) = default;
};

// A null adapter is the production fail-closed boundary: every operation returns Unavailable and
// no alternate persistence path is attempted.
class VaultKeyringStore final {
  public:
    explicit VaultKeyringStore(SecretStore* secret_store = nullptr) noexcept;

    [[nodiscard]] auto save(QStringView opaque_reference, const VaultKeyring& keyring)
        -> std::expected<void, VaultKeyringStoreError>;
    [[nodiscard]] auto load(QStringView opaque_reference)
        -> std::expected<VaultKeyring, VaultKeyringStoreError>;
    [[nodiscard]] auto restoreFromRecovery(QStringView opaque_reference,
                                           std::span<const unsigned char> recovery_capsule,
                                           std::span<const unsigned char> password,
                                           RecoveryCapsuleLimits limits = {})
        -> std::expected<void, VaultKeyringStoreError>;

  private:
    SecretStore* secret_store_{};
};

} // namespace appellate::sync
