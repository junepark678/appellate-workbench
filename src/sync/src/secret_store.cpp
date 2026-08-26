#include "appellate/sync/secret_store.hpp"

#include <QChar>

#include <utility>

namespace appellate::sync {
namespace {

constexpr qsizetype maximum_reference_characters = 128;

[[nodiscard]] auto fail(VaultKeyringStoreErrorCode code, QString message)
    -> std::unexpected<VaultKeyringStoreError> {
    return std::unexpected(VaultKeyringStoreError{code, std::move(message)});
}

[[nodiscard]] bool validReference(QStringView reference) {
    if (reference.isEmpty() || reference.size() > maximum_reference_characters) {
        return false;
    }
    bool previous_was_separator = false;
    for (qsizetype index = 0; index < reference.size(); ++index) {
        const auto character = reference.at(index);
        const auto value = character.unicode();
        const auto ascii_letter =
            (value >= u'a' && value <= u'z') || (value >= u'A' && value <= u'Z');
        const auto digit = value >= u'0' && value <= u'9';
        const auto separator = value == u'.' || value == u'_' || value == u':' || value == u'-';
        if ((!ascii_letter && !digit && !separator) || (index == 0 && !ascii_letter && !digit) ||
            (separator && previous_was_separator)) {
            return false;
        }
        previous_was_separator = separator;
    }
    return true;
}

[[nodiscard]] auto mapStoreError(const SecretStoreError& error)
    -> std::unexpected<VaultKeyringStoreError> {
    switch (error.code) {
    case SecretStoreErrorCode::Unavailable:
        return fail(VaultKeyringStoreErrorCode::SecretStoreUnavailable,
                    QStringLiteral("The operating-system secret store is unavailable"));
    case SecretStoreErrorCode::NotFound:
        return fail(VaultKeyringStoreErrorCode::SecretNotFound,
                    QStringLiteral("The requested vault keyring is unavailable"));
    case SecretStoreErrorCode::ReadFailed:
        return fail(VaultKeyringStoreErrorCode::SecretStoreReadFailed,
                    QStringLiteral("The vault keyring cannot be read from the secret store"));
    case SecretStoreErrorCode::WriteFailed:
        return fail(VaultKeyringStoreErrorCode::SecretStoreWriteFailed,
                    QStringLiteral("The vault keyring cannot be written to the secret store"));
    }
    return fail(VaultKeyringStoreErrorCode::SecretStoreUnavailable,
                QStringLiteral("The operating-system secret store is unavailable"));
}

[[nodiscard]] auto requireAdapter(SecretStore* secret_store)
    -> std::expected<void, VaultKeyringStoreError> {
    if (secret_store == nullptr) {
        return fail(VaultKeyringStoreErrorCode::SecretStoreUnavailable,
                    QStringLiteral("No operating-system secret store adapter is configured"));
    }
    return {};
}

[[nodiscard]] auto validateReference(QStringView reference)
    -> std::expected<void, VaultKeyringStoreError> {
    if (!validReference(reference)) {
        return fail(VaultKeyringStoreErrorCode::InvalidReference,
                    QStringLiteral("The opaque secret-store reference is invalid"));
    }
    return {};
}

} // namespace

VaultKeyringStore::VaultKeyringStore(SecretStore* secret_store) noexcept
    : secret_store_(secret_store) {}

std::expected<void, VaultKeyringStoreError> VaultKeyringStore::save(QStringView opaque_reference,
                                                                    const VaultKeyring& keyring) {
    if (const auto available = requireAdapter(secret_store_); !available) {
        return std::unexpected(available.error());
    }
    if (const auto valid = validateReference(opaque_reference); !valid) {
        return std::unexpected(valid.error());
    }
    auto encoded = VaultKeyringCodec::encode(keyring);
    if (!encoded) {
        return fail(VaultKeyringStoreErrorCode::InvalidStoredKeyring,
                    QStringLiteral("The vault keyring cannot be encoded for secure storage"));
    }
    const auto stored = secret_store_->write(opaque_reference, encoded->bytes());
    if (!stored) {
        return mapStoreError(stored.error());
    }
    return {};
}

std::expected<VaultKeyring, VaultKeyringStoreError>
VaultKeyringStore::load(QStringView opaque_reference) {
    if (const auto available = requireAdapter(secret_store_); !available) {
        return std::unexpected(available.error());
    }
    if (const auto valid = validateReference(opaque_reference); !valid) {
        return std::unexpected(valid.error());
    }
    auto encoded = secret_store_->read(opaque_reference);
    if (!encoded) {
        return mapStoreError(encoded.error());
    }
    auto decoded = VaultKeyringCodec::decode(encoded->bytes());
    if (!decoded) {
        return fail(VaultKeyringStoreErrorCode::InvalidStoredKeyring,
                    QStringLiteral("The secret store contains an invalid vault keyring"));
    }
    return std::move(*decoded);
}

std::expected<void, VaultKeyringStoreError> VaultKeyringStore::restoreFromRecovery(
    QStringView opaque_reference, std::span<const unsigned char> recovery_capsule,
    std::span<const unsigned char> password, RecoveryCapsuleLimits limits) {
    if (const auto available = requireAdapter(secret_store_); !available) {
        return std::unexpected(available.error());
    }
    if (const auto valid = validateReference(opaque_reference); !valid) {
        return std::unexpected(valid.error());
    }
    auto recovered = RecoveryCapsuleCodec::importCapsule(recovery_capsule, password, limits);
    if (!recovered) {
        return fail(VaultKeyringStoreErrorCode::RecoveryRejected,
                    QStringLiteral("The recovery capsule was rejected before secure storage"));
    }
    return save(opaque_reference, *recovered);
}

} // namespace appellate::sync
