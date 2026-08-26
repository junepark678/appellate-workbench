#pragma once

#include "appellate/sync/vault_keyring.hpp"

#include <QString>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace appellate::sync {

struct RecoveryKdfParameters final {
    static constexpr std::uint64_t minimum_operations_limit = 1;
    static constexpr std::uint64_t minimum_memory_limit = 8ULL * 1024ULL;
    static constexpr std::uint64_t interactive_operations_limit = 2;
    static constexpr std::uint64_t interactive_memory_limit = 64ULL * 1024ULL * 1024ULL;

    std::uint64_t operations_limit{interactive_operations_limit};
    std::uint64_t memory_limit{interactive_memory_limit};

    friend bool operator==(const RecoveryKdfParameters&, const RecoveryKdfParameters&) = default;
};

struct RecoveryCapsuleLimits final {
    static constexpr std::size_t default_maximum_capsule_bytes = std::size_t{4} * 1024U;
    static constexpr std::size_t default_maximum_password_bytes = 1024U;
    static constexpr std::uint64_t default_maximum_operations_limit = 4;
    static constexpr std::uint64_t default_maximum_memory_limit = 64ULL * 1024ULL * 1024ULL;

    std::size_t maximum_capsule_bytes{default_maximum_capsule_bytes};
    std::size_t maximum_password_bytes{default_maximum_password_bytes};
    std::uint64_t maximum_operations_limit{default_maximum_operations_limit};
    std::uint64_t maximum_memory_limit{default_maximum_memory_limit};
};

enum class RecoveryCapsuleErrorCode {
    InvalidArgument,
    CryptoInitializationFailed,
    ProtectedMemoryUnavailable,
    PasswordOutOfBounds,
    ParametersOutOfBounds,
    CapsuleTooLarge,
    MalformedCapsule,
    UnsupportedFormat,
    KeyDerivationFailed,
    AuthenticationFailed,
    InvalidKeyring,
};

struct RecoveryCapsuleError final {
    RecoveryCapsuleErrorCode code{};
    QString message;

    friend bool operator==(const RecoveryCapsuleError&, const RecoveryCapsuleError&) = default;
};

class RecoveryCapsuleCodec final {
  public:
    static constexpr std::size_t fixed_header_bytes = 72;
    static constexpr std::uint16_t argon2id_1_3_algorithm = 1;
    static constexpr std::uint16_t xchacha20_poly1305_ietf_algorithm = 1;

    // Password bytes are consumed synchronously and exactly as supplied. They are not copied,
    // converted to text, normalized, or logged.
    [[nodiscard]] static auto
    exportCapsule(const VaultKeyring& keyring, std::span<const unsigned char> password,
                  RecoveryKdfParameters parameters = {}, RecoveryCapsuleLimits limits = {})
        -> std::expected<std::vector<unsigned char>, RecoveryCapsuleError>;

    [[nodiscard]] static auto importCapsule(std::span<const unsigned char> capsule,
                                            std::span<const unsigned char> password,
                                            RecoveryCapsuleLimits limits = {})
        -> std::expected<VaultKeyring, RecoveryCapsuleError>;
};

} // namespace appellate::sync
