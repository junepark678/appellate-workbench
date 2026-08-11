#include "appellate/sync/vault_keyring.hpp"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace appellate::sync {
namespace {

constexpr std::array<unsigned char, 8> keyring_magic{'A', 'W', 'K', 'R', 0, 1, 0, 0};
constexpr std::size_t object_key_offset = 0;
constexpr std::size_t slot_keys_offset = sync_key_bytes;

[[nodiscard]] auto fail(VaultKeyringErrorCode code, QString message)
    -> std::unexpected<VaultKeyringError> {
    return std::unexpected(VaultKeyringError{code, std::move(message)});
}

[[nodiscard]] bool isAllZero(std::span<const unsigned char> bytes) {
    unsigned char aggregate{};
    for (const auto byte : bytes) {
        aggregate = static_cast<unsigned char>(aggregate | byte);
    }
    return aggregate == 0U;
}

template <std::size_t Size>
[[nodiscard]] bool isAllZero(const std::array<unsigned char, Size>& bytes) {
    return isAllZero(std::span<const unsigned char>{bytes});
}

[[nodiscard]] bool hasDuplicateSlots(const std::vector<SyncKeySlotId>& slot_ids) {
    for (std::size_t left = 0; left < slot_ids.size(); ++left) {
        for (std::size_t right = left + 1U; right < slot_ids.size(); ++right) {
            if (slot_ids[left] == slot_ids[right]) {
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] auto validateParts(const VaultId& vault_id,
                                 const std::vector<SyncKeySlotId>& slot_ids,
                                 const SyncKeySlotId& current_write_slot_id,
                                 std::span<const unsigned char> key_material)
    -> std::expected<void, VaultKeyringError> {
    if (isAllZero(vault_id)) {
        return fail(VaultKeyringErrorCode::InvalidVaultIdentifier,
                    QStringLiteral("The vault identifier is invalid"));
    }
    if (slot_ids.empty() || slot_ids.size() > VaultKeyring::maximum_key_slots) {
        return fail(VaultKeyringErrorCode::InvalidKeySlot,
                    QStringLiteral("The vault key-slot count is invalid"));
    }
    const auto expected_key_bytes = sync_key_bytes * (slot_ids.size() + 1U);
    if (key_material.size() != expected_key_bytes) {
        return fail(VaultKeyringErrorCode::InvalidArgument,
                    QStringLiteral("The vault key material has an invalid size"));
    }
    if (isAllZero(key_material.first(sync_key_bytes))) {
        return fail(VaultKeyringErrorCode::InvalidObjectIdKey,
                    QStringLiteral("The vault object identifier key is invalid"));
    }
    for (std::size_t index = 0; index < slot_ids.size(); ++index) {
        if (isAllZero(slot_ids[index]) ||
            isAllZero(
                key_material.subspan(slot_keys_offset + index * sync_key_bytes, sync_key_bytes))) {
            return fail(VaultKeyringErrorCode::InvalidKeySlot,
                        QStringLiteral("A vault encryption key slot is invalid"));
        }
    }
    if (hasDuplicateSlots(slot_ids)) {
        return fail(VaultKeyringErrorCode::DuplicateKeySlot,
                    QStringLiteral("The vault contains duplicate key-slot identifiers"));
    }
    if (std::ranges::find(slot_ids, current_write_slot_id) == slot_ids.end()) {
        return fail(VaultKeyringErrorCode::UnknownCurrentSlot,
                    QStringLiteral("The current vault write slot is unavailable"));
    }
    return {};
}

[[nodiscard]] auto protectedMemoryFailure(const SecretMemoryError& error)
    -> std::unexpected<VaultKeyringError> {
    const auto code = error.code == SecretMemoryErrorCode::CryptoInitializationFailed
                          ? VaultKeyringErrorCode::CryptoInitializationFailed
                          : VaultKeyringErrorCode::ProtectedMemoryUnavailable;
    return fail(code, QStringLiteral("Protected vault key memory is unavailable"));
}

void appendBigEndian16(unsigned char* destination, std::uint16_t value) {
    destination[0] = static_cast<unsigned char>((value >> 8U) & 0xffU);
    destination[1] = static_cast<unsigned char>(value & 0xffU);
}

[[nodiscard]] std::uint16_t readBigEndian16(const unsigned char* source) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(source[0]) << 8U) |
                                      static_cast<std::uint16_t>(source[1]));
}

void wipeProtocolKeys(ProtocolKeySet& keys) noexcept {
    sodium_memzero(keys.object_id_key.data(), keys.object_id_key.size());
    for (auto& slot : keys.key_slots) {
        sodium_memzero(slot.encryption_key.data(), slot.encryption_key.size());
        sodium_memzero(slot.id.data(), slot.id.size());
    }
    keys.key_slots.clear();
    keys.active_slot = 0U;
}

void takeProtocolKeys(ProtocolKeySet& destination, ProtocolKeySet& source) noexcept {
    std::memcpy(destination.object_id_key.data(), source.object_id_key.data(),
                destination.object_id_key.size());
    sodium_memzero(source.object_id_key.data(), source.object_id_key.size());
    destination.key_slots.swap(source.key_slots);
    destination.active_slot = std::exchange(source.active_slot, 0U);
}

void randomNonzero(std::span<unsigned char> destination) {
    do {
        randombytes_buf(destination.data(), destination.size());
    } while (isAllZero(destination));
}

} // namespace

ScopedProtocolKeySet::ScopedProtocolKeySet(ProtocolKeySet&& keys) noexcept {
    takeProtocolKeys(keys_, keys);
}

ScopedProtocolKeySet::ScopedProtocolKeySet(ScopedProtocolKeySet&& other) noexcept {
    takeProtocolKeys(keys_, other.keys_);
}

ScopedProtocolKeySet& ScopedProtocolKeySet::operator=(ScopedProtocolKeySet&& other) noexcept {
    if (this != &other) {
        clear();
        takeProtocolKeys(keys_, other.keys_);
    }
    return *this;
}

ScopedProtocolKeySet::~ScopedProtocolKeySet() { clear(); }

const ProtocolKeySet& ScopedProtocolKeySet::get() const noexcept { return keys_; }

ProtocolKeySet& ScopedProtocolKeySet::mutableGet() noexcept { return keys_; }

bool ScopedProtocolKeySet::empty() const noexcept { return keys_.key_slots.empty(); }

void ScopedProtocolKeySet::clear() noexcept { wipeProtocolKeys(keys_); }

VaultKeyring::VaultKeyring(VaultId vault_id, std::vector<SyncKeySlotId> slot_ids,
                           SyncKeySlotId current_write_slot_id, SecretBytes key_material) noexcept
    : vault_id_(vault_id), slot_ids_(std::move(slot_ids)),
      current_write_slot_id_(current_write_slot_id), key_material_(std::move(key_material)) {}

std::expected<VaultKeyring, VaultKeyringError> VaultKeyring::create() {
    if (sodium_init() < 0) {
        return fail(VaultKeyringErrorCode::CryptoInitializationFailed,
                    QStringLiteral("Cannot initialize vault key generation"));
    }
    auto material = SecretBytes::allocate(sync_key_bytes * 2U);
    if (!material) {
        return protectedMemoryFailure(material.error());
    }

    VaultId vault_id{};
    SyncKeySlotId slot_id{};
    randomNonzero(vault_id);
    randomNonzero(slot_id);
    randomNonzero(material->mutableBytes().first(sync_key_bytes));
    randomNonzero(material->mutableBytes().subspan(slot_keys_offset, sync_key_bytes));
    std::vector<SyncKeySlotId> slot_ids{slot_id};
    return VaultKeyring{vault_id, std::move(slot_ids), slot_id, std::move(*material)};
}

std::expected<VaultKeyring, VaultKeyringError>
VaultKeyring::fromDecoded(VaultId vault_id, std::vector<SyncKeySlotId> slot_ids,
                          SyncKeySlotId current_write_slot_id, SecretBytes key_material) {
    if (const auto validated =
            validateParts(vault_id, slot_ids, current_write_slot_id, key_material.bytes());
        !validated) {
        return std::unexpected(validated.error());
    }
    return VaultKeyring{vault_id, std::move(slot_ids), current_write_slot_id,
                        std::move(key_material)};
}

std::expected<SyncKeySlotId, VaultKeyringError> VaultKeyring::rotate() {
    if (sodium_init() < 0) {
        return fail(VaultKeyringErrorCode::CryptoInitializationFailed,
                    QStringLiteral("Cannot initialize vault key rotation"));
    }
    if (const auto validated =
            validateParts(vault_id_, slot_ids_, current_write_slot_id_, key_material_.bytes());
        !validated) {
        return std::unexpected(validated.error());
    }
    if (slot_ids_.size() >= maximum_key_slots) {
        return fail(VaultKeyringErrorCode::KeySlotLimitReached,
                    QStringLiteral("The vault key-slot limit has been reached"));
    }

    SyncKeySlotId new_slot_id{};
    do {
        randomNonzero(new_slot_id);
    } while (std::ranges::find(slot_ids_, new_slot_id) != slot_ids_.end());

    auto replacement = SecretBytes::allocate(key_material_.size() + sync_key_bytes);
    if (!replacement) {
        return protectedMemoryFailure(replacement.error());
    }
    std::memcpy(replacement->mutableBytes().data(), key_material_.bytes().data(),
                key_material_.size());
    randomNonzero(replacement->mutableBytes().last(sync_key_bytes));

    slot_ids_.push_back(new_slot_id);
    key_material_ = std::move(*replacement);
    current_write_slot_id_ = new_slot_id;
    return new_slot_id;
}

const VaultId& VaultKeyring::vaultId() const noexcept { return vault_id_; }

std::span<const unsigned char> VaultKeyring::objectIdKey() const noexcept {
    if (key_material_.size() < sync_key_bytes) {
        return {};
    }
    return key_material_.bytes().subspan(object_key_offset, sync_key_bytes);
}

std::size_t VaultKeyring::keySlotCount() const noexcept { return slot_ids_.size(); }

const SyncKeySlotId& VaultKeyring::keySlotId(std::size_t index) const {
    return slot_ids_.at(index);
}

std::span<const unsigned char> VaultKeyring::encryptionKey(std::size_t index) const {
    if (index >= slot_ids_.size() ||
        key_material_.size() < slot_keys_offset + (index + 1U) * sync_key_bytes) {
        throw std::out_of_range("vault key-slot index");
    }
    return key_material_.bytes().subspan(slot_keys_offset + index * sync_key_bytes, sync_key_bytes);
}

const SyncKeySlotId& VaultKeyring::currentWriteSlotId() const noexcept {
    return current_write_slot_id_;
}

ScopedProtocolKeySet VaultKeyring::scopedProtocolKeys() const {
    ProtocolKeySet empty_keys;
    ScopedProtocolKeySet scoped{std::move(empty_keys)};
    if (!validateParts(vault_id_, slot_ids_, current_write_slot_id_, key_material_.bytes())) {
        return scoped;
    }
    auto& keys = scoped.mutableGet();
    keys.key_slots.reserve(slot_ids_.size());
    std::memcpy(keys.object_id_key.data(), objectIdKey().data(), sync_key_bytes);
    for (std::size_t index = 0; index < slot_ids_.size(); ++index) {
        auto& slot = keys.key_slots.emplace_back();
        slot.id = slot_ids_[index];
        std::memcpy(slot.encryption_key.data(), encryptionKey(index).data(), sync_key_bytes);
    }
    const auto active = std::ranges::find(slot_ids_, current_write_slot_id_);
    keys.active_slot = static_cast<std::size_t>(std::distance(slot_ids_.begin(), active));
    return scoped;
}

bool operator==(const VaultKeyring& left, const VaultKeyring& right) noexcept {
    if (left.vault_id_ != right.vault_id_ || left.slot_ids_ != right.slot_ids_ ||
        left.current_write_slot_id_ != right.current_write_slot_id_ ||
        left.key_material_.size() != right.key_material_.size()) {
        return false;
    }
    return left.key_material_.empty() ||
           sodium_memcmp(left.key_material_.bytes().data(), right.key_material_.bytes().data(),
                         left.key_material_.size()) == 0;
}

std::expected<SecretBytes, VaultKeyringError>
VaultKeyringCodec::encode(const VaultKeyring& keyring) {
    if (const auto validated =
            validateParts(keyring.vault_id_, keyring.slot_ids_, keyring.current_write_slot_id_,
                          keyring.key_material_.bytes());
        !validated) {
        return std::unexpected(validated.error());
    }
    const auto encoded_size = fixed_header_bytes + keyring.keySlotCount() * encoded_slot_bytes;
    auto encoded = SecretBytes::allocate(encoded_size);
    if (!encoded) {
        return protectedMemoryFailure(encoded.error());
    }
    auto output = encoded->mutableBytes();
    std::size_t offset{};
    std::memcpy(output.data() + offset, keyring_magic.data(), keyring_magic.size());
    offset += keyring_magic.size();
    std::memcpy(output.data() + offset, keyring.vaultId().data(), keyring.vaultId().size());
    offset += keyring.vaultId().size();
    std::memcpy(output.data() + offset, keyring.objectIdKey().data(), sync_key_bytes);
    offset += sync_key_bytes;
    std::memcpy(output.data() + offset, keyring.currentWriteSlotId().data(),
                sync_key_slot_id_bytes);
    offset += sync_key_slot_id_bytes;
    appendBigEndian16(output.data() + offset, static_cast<std::uint16_t>(keyring.keySlotCount()));
    offset += 2U;
    output[offset++] = 0U;
    output[offset++] = 0U;
    for (std::size_t index = 0; index < keyring.keySlotCount(); ++index) {
        std::memcpy(output.data() + offset, keyring.keySlotId(index).data(),
                    sync_key_slot_id_bytes);
        offset += sync_key_slot_id_bytes;
        std::memcpy(output.data() + offset, keyring.encryptionKey(index).data(), sync_key_bytes);
        offset += sync_key_bytes;
    }
    return std::move(*encoded);
}

std::expected<VaultKeyring, VaultKeyringError>
VaultKeyringCodec::decode(std::span<const unsigned char> encoded) {
    if (encoded.size() < fixed_header_bytes) {
        return fail(VaultKeyringErrorCode::MalformedEncoding,
                    QStringLiteral("The vault keyring encoding is truncated"));
    }
    if (!std::equal(keyring_magic.begin(), keyring_magic.begin() + 4, encoded.begin())) {
        return fail(VaultKeyringErrorCode::MalformedEncoding,
                    QStringLiteral("The vault keyring encoding has an invalid magic value"));
    }
    if (!std::equal(keyring_magic.begin() + 4, keyring_magic.end(), encoded.begin() + 4)) {
        return fail(VaultKeyringErrorCode::UnsupportedFormat,
                    QStringLiteral("The vault keyring format is unsupported"));
    }

    std::size_t offset = keyring_magic.size();
    VaultId vault_id{};
    std::memcpy(vault_id.data(), encoded.data() + offset, vault_id.size());
    offset += vault_id.size();
    const auto* object_key = encoded.data() + offset;
    offset += sync_key_bytes;
    SyncKeySlotId current_slot{};
    std::memcpy(current_slot.data(), encoded.data() + offset, current_slot.size());
    offset += current_slot.size();
    const auto slot_count = readBigEndian16(encoded.data() + offset);
    offset += 2U;
    if (encoded[offset] != 0U || encoded[offset + 1U] != 0U) {
        return fail(VaultKeyringErrorCode::UnsupportedFormat,
                    QStringLiteral("The vault keyring reserved flags are unsupported"));
    }
    offset += 2U;
    if (slot_count == 0U || slot_count > VaultKeyring::maximum_key_slots) {
        return fail(VaultKeyringErrorCode::InvalidKeySlot,
                    QStringLiteral("The vault key-slot count is invalid"));
    }
    const auto expected_size =
        fixed_header_bytes + static_cast<std::size_t>(slot_count) * encoded_slot_bytes;
    if (encoded.size() != expected_size) {
        return fail(VaultKeyringErrorCode::MalformedEncoding,
                    QStringLiteral("The vault keyring encoding length is invalid"));
    }

    auto key_material =
        SecretBytes::allocate(sync_key_bytes * (static_cast<std::size_t>(slot_count) + 1U));
    if (!key_material) {
        return protectedMemoryFailure(key_material.error());
    }
    std::memcpy(key_material->mutableBytes().data(), object_key, sync_key_bytes);
    std::vector<SyncKeySlotId> slot_ids;
    slot_ids.reserve(slot_count);
    for (std::size_t index = 0; index < slot_count; ++index) {
        SyncKeySlotId slot_id{};
        std::memcpy(slot_id.data(), encoded.data() + offset, slot_id.size());
        offset += slot_id.size();
        slot_ids.push_back(slot_id);
        std::memcpy(key_material->mutableBytes().data() + slot_keys_offset + index * sync_key_bytes,
                    encoded.data() + offset, sync_key_bytes);
        offset += sync_key_bytes;
    }
    return VaultKeyring::fromDecoded(vault_id, std::move(slot_ids), current_slot,
                                     std::move(*key_material));
}

} // namespace appellate::sync
