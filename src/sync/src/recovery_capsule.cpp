#include "appellate/sync/recovery_capsule.hpp"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>

namespace appellate::sync {
namespace {

constexpr std::array<unsigned char, 8> capsule_magic{'A', 'W', 'R', 'C', 0, 1, 0, 0};
constexpr char associated_data_domain[] = "appellate-workbench-sync-recovery-v1";
constexpr std::size_t salt_bytes = crypto_pwhash_SALTBYTES;
constexpr std::size_t nonce_bytes = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
constexpr std::size_t derived_key_bytes = crypto_aead_xchacha20poly1305_ietf_KEYBYTES;
constexpr std::size_t authentication_bytes = crypto_aead_xchacha20poly1305_ietf_ABYTES;

static_assert(salt_bytes == 16U);
static_assert(nonce_bytes == 24U);
static_assert(derived_key_bytes == sync_key_bytes);
static_assert(authentication_bytes == 16U);
static_assert(RecoveryKdfParameters::minimum_operations_limit == crypto_pwhash_OPSLIMIT_MIN);
static_assert(RecoveryKdfParameters::minimum_memory_limit == crypto_pwhash_MEMLIMIT_MIN);
static_assert(RecoveryKdfParameters::interactive_operations_limit ==
              crypto_pwhash_OPSLIMIT_INTERACTIVE);
static_assert(RecoveryKdfParameters::interactive_memory_limit ==
              crypto_pwhash_MEMLIMIT_INTERACTIVE);

[[nodiscard]] auto fail(RecoveryCapsuleErrorCode code, QString message)
    -> std::unexpected<RecoveryCapsuleError> {
    return std::unexpected(RecoveryCapsuleError{code, std::move(message)});
}

[[nodiscard]] auto protectedMemoryFailure(const SecretMemoryError& error)
    -> std::unexpected<RecoveryCapsuleError> {
    const auto code = error.code == SecretMemoryErrorCode::CryptoInitializationFailed
                          ? RecoveryCapsuleErrorCode::CryptoInitializationFailed
                          : RecoveryCapsuleErrorCode::ProtectedMemoryUnavailable;
    return fail(code, QStringLiteral("Protected recovery memory is unavailable"));
}

[[nodiscard]] auto keyringFailure(const VaultKeyringError& error, QString invalid_message)
    -> std::unexpected<RecoveryCapsuleError> {
    if (error.code == VaultKeyringErrorCode::CryptoInitializationFailed) {
        return fail(RecoveryCapsuleErrorCode::CryptoInitializationFailed,
                    QStringLiteral("Cannot initialize protected vault key processing"));
    }
    if (error.code == VaultKeyringErrorCode::ProtectedMemoryUnavailable) {
        return fail(RecoveryCapsuleErrorCode::ProtectedMemoryUnavailable,
                    QStringLiteral("Protected recovery memory is unavailable"));
    }
    return fail(RecoveryCapsuleErrorCode::InvalidKeyring, std::move(invalid_message));
}

void writeBigEndian16(unsigned char* destination, std::uint16_t value) {
    destination[0] = static_cast<unsigned char>((value >> 8U) & 0xffU);
    destination[1] = static_cast<unsigned char>(value & 0xffU);
}

void writeBigEndian32(unsigned char* destination, std::uint32_t value) {
    for (std::size_t index = 0; index < 4U; ++index) {
        const auto shift = static_cast<unsigned int>((3U - index) * 8U);
        destination[index] = static_cast<unsigned char>((value >> shift) & 0xffU);
    }
}

void writeBigEndian64(unsigned char* destination, std::uint64_t value) {
    for (std::size_t index = 0; index < 8U; ++index) {
        const auto shift = static_cast<unsigned int>((7U - index) * 8U);
        destination[index] = static_cast<unsigned char>((value >> shift) & 0xffU);
    }
}

[[nodiscard]] std::uint16_t readBigEndian16(const unsigned char* source) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(source[0]) << 8U) |
                                      static_cast<std::uint16_t>(source[1]));
}

[[nodiscard]] std::uint32_t readBigEndian32(const unsigned char* source) {
    std::uint32_t value{};
    for (std::size_t index = 0; index < 4U; ++index) {
        value = (value << 8U) | static_cast<std::uint32_t>(source[index]);
    }
    return value;
}

[[nodiscard]] std::uint64_t readBigEndian64(const unsigned char* source) {
    std::uint64_t value{};
    for (std::size_t index = 0; index < 8U; ++index) {
        value = (value << 8U) | static_cast<std::uint64_t>(source[index]);
    }
    return value;
}

[[nodiscard]] auto validateLimits(const RecoveryCapsuleLimits& limits)
    -> std::expected<void, RecoveryCapsuleError> {
    constexpr auto minimum_capsule_bytes =
        RecoveryCapsuleCodec::fixed_header_bytes + authentication_bytes;
    if (limits.maximum_capsule_bytes < minimum_capsule_bytes ||
        limits.maximum_password_bytes == 0U ||
        limits.maximum_operations_limit < RecoveryKdfParameters::minimum_operations_limit ||
        limits.maximum_memory_limit < RecoveryKdfParameters::minimum_memory_limit ||
        limits.maximum_memory_limit > std::numeric_limits<std::size_t>::max()) {
        return fail(RecoveryCapsuleErrorCode::InvalidArgument,
                    QStringLiteral("The recovery safety limits are invalid"));
    }
    return {};
}

[[nodiscard]] auto validatePassword(std::span<const unsigned char> password,
                                    const RecoveryCapsuleLimits& limits)
    -> std::expected<void, RecoveryCapsuleError> {
    if (password.empty() || password.size() > limits.maximum_password_bytes ||
        password.size() > crypto_pwhash_PASSWD_MAX) {
        return fail(RecoveryCapsuleErrorCode::PasswordOutOfBounds,
                    QStringLiteral("The recovery password length is invalid"));
    }
    return {};
}

[[nodiscard]] auto validateParameters(RecoveryKdfParameters parameters,
                                      const RecoveryCapsuleLimits& limits)
    -> std::expected<void, RecoveryCapsuleError> {
    if (parameters.operations_limit < RecoveryKdfParameters::minimum_operations_limit ||
        parameters.operations_limit > limits.maximum_operations_limit ||
        parameters.operations_limit > std::numeric_limits<unsigned long long>::max() ||
        parameters.memory_limit < RecoveryKdfParameters::minimum_memory_limit ||
        parameters.memory_limit > limits.maximum_memory_limit ||
        parameters.memory_limit > std::numeric_limits<std::size_t>::max()) {
        return fail(RecoveryCapsuleErrorCode::ParametersOutOfBounds,
                    QStringLiteral("The recovery key-derivation parameters are outside limits"));
    }
    return {};
}

[[nodiscard]] auto deriveKey(std::span<const unsigned char> password, const unsigned char* salt,
                             RecoveryKdfParameters parameters)
    -> std::expected<SecretBytes, RecoveryCapsuleError> {
    auto key = SecretBytes::allocate(derived_key_bytes);
    if (!key) {
        return protectedMemoryFailure(key.error());
    }
    if (crypto_pwhash(
            key->mutableBytes().data(), key->size(), reinterpret_cast<const char*>(password.data()),
            static_cast<unsigned long long>(password.size()), salt,
            static_cast<unsigned long long>(parameters.operations_limit),
            static_cast<std::size_t>(parameters.memory_limit), crypto_pwhash_ALG_ARGON2ID13) != 0) {
        return fail(RecoveryCapsuleErrorCode::KeyDerivationFailed,
                    QStringLiteral("The recovery key cannot be derived within local limits"));
    }
    return std::move(*key);
}

[[nodiscard]] std::vector<unsigned char> makeAssociatedData(std::span<const unsigned char> header) {
    std::vector<unsigned char> associated(sizeof(associated_data_domain) + header.size());
    std::memcpy(associated.data(), associated_data_domain, sizeof(associated_data_domain));
    std::memcpy(associated.data() + sizeof(associated_data_domain), header.data(), header.size());
    return associated;
}

} // namespace

std::expected<std::vector<unsigned char>, RecoveryCapsuleError> RecoveryCapsuleCodec::exportCapsule(
    const VaultKeyring& keyring, std::span<const unsigned char> password,
    RecoveryKdfParameters parameters, RecoveryCapsuleLimits limits) {
    if (sodium_init() < 0) {
        return fail(RecoveryCapsuleErrorCode::CryptoInitializationFailed,
                    QStringLiteral("Cannot initialize recovery export"));
    }
    if (const auto validated = validateLimits(limits); !validated) {
        return std::unexpected(validated.error());
    }
    if (const auto validated = validatePassword(password, limits); !validated) {
        return std::unexpected(validated.error());
    }
    if (const auto validated = validateParameters(parameters, limits); !validated) {
        return std::unexpected(validated.error());
    }

    auto plaintext = VaultKeyringCodec::encode(keyring);
    if (!plaintext) {
        return keyringFailure(plaintext.error(),
                              QStringLiteral("The vault keyring cannot be exported"));
    }
    const auto ciphertext_size = plaintext->size() + authentication_bytes;
    if (ciphertext_size > std::numeric_limits<std::uint32_t>::max() ||
        fixed_header_bytes > limits.maximum_capsule_bytes ||
        ciphertext_size > limits.maximum_capsule_bytes - fixed_header_bytes) {
        return fail(RecoveryCapsuleErrorCode::CapsuleTooLarge,
                    QStringLiteral("The recovery capsule exceeds its configured limit"));
    }

    std::vector<unsigned char> capsule(fixed_header_bytes + ciphertext_size);
    std::size_t offset{};
    std::memcpy(capsule.data(), capsule_magic.data(), capsule_magic.size());
    offset += capsule_magic.size();
    writeBigEndian16(capsule.data() + offset, argon2id_1_3_algorithm);
    offset += 2U;
    writeBigEndian16(capsule.data() + offset, xchacha20_poly1305_ietf_algorithm);
    offset += 2U;
    writeBigEndian64(capsule.data() + offset, parameters.operations_limit);
    offset += 8U;
    writeBigEndian64(capsule.data() + offset, parameters.memory_limit);
    offset += 8U;
    auto* salt = capsule.data() + offset;
    randombytes_buf(salt, salt_bytes);
    offset += salt_bytes;
    auto* nonce = capsule.data() + offset;
    randombytes_buf(nonce, nonce_bytes);
    offset += nonce_bytes;
    writeBigEndian32(capsule.data() + offset, static_cast<std::uint32_t>(ciphertext_size));

    auto derived_key = deriveKey(password, salt, parameters);
    if (!derived_key) {
        return std::unexpected(derived_key.error());
    }
    const auto associated =
        makeAssociatedData(std::span<const unsigned char>{capsule.data(), fixed_header_bytes});
    unsigned long long written{};
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            capsule.data() + fixed_header_bytes, &written, plaintext->bytes().data(),
            static_cast<unsigned long long>(plaintext->size()), associated.data(),
            static_cast<unsigned long long>(associated.size()), nullptr, nonce,
            derived_key->bytes().data()) != 0 ||
        written != static_cast<unsigned long long>(ciphertext_size)) {
        return fail(RecoveryCapsuleErrorCode::CryptoInitializationFailed,
                    QStringLiteral("Cannot encrypt the recovery capsule"));
    }
    return capsule;
}

std::expected<VaultKeyring, RecoveryCapsuleError>
// Both inputs are intentionally non-owning byte views; their names and ordering mirror export.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
RecoveryCapsuleCodec::importCapsule(std::span<const unsigned char> capsule,
                                    std::span<const unsigned char> password,
                                    RecoveryCapsuleLimits limits) {
    if (sodium_init() < 0) {
        return fail(RecoveryCapsuleErrorCode::CryptoInitializationFailed,
                    QStringLiteral("Cannot initialize recovery import"));
    }
    if (const auto validated = validateLimits(limits); !validated) {
        return std::unexpected(validated.error());
    }
    if (const auto validated = validatePassword(password, limits); !validated) {
        return std::unexpected(validated.error());
    }
    if (capsule.size() > limits.maximum_capsule_bytes) {
        return fail(RecoveryCapsuleErrorCode::CapsuleTooLarge,
                    QStringLiteral("The recovery capsule exceeds its configured limit"));
    }
    if (capsule.size() < fixed_header_bytes) {
        return fail(RecoveryCapsuleErrorCode::MalformedCapsule,
                    QStringLiteral("The recovery capsule header is truncated"));
    }
    if (!std::equal(capsule_magic.begin(), capsule_magic.begin() + 4, capsule.begin())) {
        return fail(RecoveryCapsuleErrorCode::MalformedCapsule,
                    QStringLiteral("The recovery capsule magic value is invalid"));
    }
    if (!std::equal(capsule_magic.begin() + 4, capsule_magic.end(), capsule.begin() + 4)) {
        return fail(RecoveryCapsuleErrorCode::UnsupportedFormat,
                    QStringLiteral("The recovery capsule version or flags are unsupported"));
    }

    std::size_t offset = capsule_magic.size();
    const auto kdf_algorithm = readBigEndian16(capsule.data() + offset);
    offset += 2U;
    const auto aead_algorithm = readBigEndian16(capsule.data() + offset);
    offset += 2U;
    if (kdf_algorithm != argon2id_1_3_algorithm ||
        aead_algorithm != xchacha20_poly1305_ietf_algorithm) {
        return fail(RecoveryCapsuleErrorCode::UnsupportedFormat,
                    QStringLiteral("The recovery capsule algorithms are unsupported"));
    }
    const RecoveryKdfParameters parameters{readBigEndian64(capsule.data() + offset),
                                           readBigEndian64(capsule.data() + offset + 8U)};
    offset += 16U;
    if (const auto validated = validateParameters(parameters, limits); !validated) {
        return std::unexpected(validated.error());
    }
    const auto* salt = capsule.data() + offset;
    offset += salt_bytes;
    const auto* nonce = capsule.data() + offset;
    offset += nonce_bytes;
    const auto ciphertext_size = readBigEndian32(capsule.data() + offset);

    constexpr auto minimum_plaintext_bytes =
        VaultKeyringCodec::fixed_header_bytes + VaultKeyringCodec::encoded_slot_bytes;
    constexpr auto maximum_ciphertext_bytes =
        VaultKeyringCodec::maximum_encoded_bytes + authentication_bytes;
    if (ciphertext_size < minimum_plaintext_bytes + authentication_bytes ||
        ciphertext_size > maximum_ciphertext_bytes ||
        static_cast<std::size_t>(ciphertext_size) != capsule.size() - fixed_header_bytes) {
        return fail(RecoveryCapsuleErrorCode::MalformedCapsule,
                    QStringLiteral("The recovery capsule ciphertext length is invalid"));
    }

    auto derived_key = deriveKey(password, salt, parameters);
    if (!derived_key) {
        return std::unexpected(derived_key.error());
    }
    auto plaintext = SecretBytes::allocate(ciphertext_size - authentication_bytes);
    if (!plaintext) {
        return protectedMemoryFailure(plaintext.error());
    }
    const auto associated =
        makeAssociatedData(capsule.first(static_cast<std::size_t>(fixed_header_bytes)));
    unsigned long long plaintext_size{};
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            plaintext->mutableBytes().data(), &plaintext_size, nullptr,
            capsule.data() + fixed_header_bytes, ciphertext_size, associated.data(),
            static_cast<unsigned long long>(associated.size()), nonce,
            derived_key->bytes().data()) != 0) {
        return fail(RecoveryCapsuleErrorCode::AuthenticationFailed,
                    QStringLiteral("The recovery capsule cannot be authenticated"));
    }
    if (plaintext_size != plaintext->size()) {
        return fail(RecoveryCapsuleErrorCode::MalformedCapsule,
                    QStringLiteral("The recovery capsule plaintext length is invalid"));
    }
    auto decoded = VaultKeyringCodec::decode(plaintext->bytes());
    if (!decoded) {
        return keyringFailure(
            decoded.error(),
            QStringLiteral("The recovery capsule contains an invalid vault keyring"));
    }
    return std::move(*decoded);
}

} // namespace appellate::sync
