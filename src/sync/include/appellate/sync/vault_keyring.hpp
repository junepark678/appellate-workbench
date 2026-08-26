#pragma once

#include "appellate/sync/secret_bytes.hpp"
#include "appellate/sync/sync_object.hpp"

#include <QString>

#include <array>
#include <cstddef>
#include <expected>
#include <functional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace appellate::sync {

inline constexpr std::size_t vault_id_bytes = 16;
using VaultId = std::array<unsigned char, vault_id_bytes>;

enum class VaultKeyringErrorCode {
    InvalidArgument,
    CryptoInitializationFailed,
    ProtectedMemoryUnavailable,
    MalformedEncoding,
    UnsupportedFormat,
    InvalidVaultIdentifier,
    InvalidObjectIdKey,
    InvalidKeySlot,
    DuplicateKeySlot,
    UnknownCurrentSlot,
    KeySlotLimitReached,
};

struct VaultKeyringError final {
    VaultKeyringErrorCode code{};
    QString message;

    friend bool operator==(const VaultKeyringError&, const VaultKeyringError&) = default;
};

class VaultKeyring;

// Owns the value-shaped compatibility copy required by ProtocolCodec and wipes it promptly.
class ScopedProtocolKeySet final {
  public:
    // Takes ownership of a value-shaped protocol key set and wipes the exact moved-from source.
    explicit ScopedProtocolKeySet(ProtocolKeySet&& keys) noexcept;
    ScopedProtocolKeySet(const ScopedProtocolKeySet&) = delete;
    ScopedProtocolKeySet& operator=(const ScopedProtocolKeySet&) = delete;
    ScopedProtocolKeySet(ScopedProtocolKeySet&& other) noexcept;
    ScopedProtocolKeySet& operator=(ScopedProtocolKeySet&& other) noexcept;
    ~ScopedProtocolKeySet();

    [[nodiscard]] auto get() const noexcept -> const ProtocolKeySet&;
    [[nodiscard]] auto empty() const noexcept -> bool;
    void clear() noexcept;

  private:
    friend class VaultKeyring;
    [[nodiscard]] auto mutableGet() noexcept -> ProtocolKeySet&;

    ProtocolKeySet keys_;
};

// The owning key bytes live in sodium_malloc() guarded memory. Slot IDs and the vault ID are
// opaque public identifiers; they intentionally remain in ordinary value storage.
class VaultKeyring final {
  public:
    static constexpr std::size_t maximum_key_slots = 32;

    VaultKeyring(const VaultKeyring&) = delete;
    VaultKeyring& operator=(const VaultKeyring&) = delete;
    VaultKeyring(VaultKeyring&&) noexcept = default;
    VaultKeyring& operator=(VaultKeyring&&) noexcept = default;
    ~VaultKeyring() = default;

    [[nodiscard]] static auto create() -> std::expected<VaultKeyring, VaultKeyringError>;

    // Appends a fresh write slot. Existing slots remain available for reads.
    [[nodiscard]] auto rotate() -> std::expected<SyncKeySlotId, VaultKeyringError>;

    [[nodiscard]] auto vaultId() const noexcept -> const VaultId&;
    [[nodiscard]] auto objectIdKey() const noexcept -> std::span<const unsigned char>;
    [[nodiscard]] auto keySlotCount() const noexcept -> std::size_t;
    [[nodiscard]] auto keySlotId(std::size_t index) const -> const SyncKeySlotId&;
    [[nodiscard]] auto encryptionKey(std::size_t index) const -> std::span<const unsigned char>;
    [[nodiscard]] auto currentWriteSlotId() const noexcept -> const SyncKeySlotId&;

    [[nodiscard]] auto scopedProtocolKeys() const -> ScopedProtocolKeySet;

    // The callback is synchronous and must not retain or copy the supplied key set.
    template <typename Callback>
    [[nodiscard]] auto withProtocolKeys(Callback&& callback) const
        -> std::invoke_result_t<Callback, const ProtocolKeySet&> {
        using Result = std::invoke_result_t<Callback, const ProtocolKeySet&>;
        static_assert(!std::is_reference_v<Result>,
                      "Protocol key callbacks must not return a reference to scoped keys");
        static_assert(!std::is_pointer_v<Result>,
                      "Protocol key callbacks must not return a pointer from scoped keys");
        static_assert(!std::is_same_v<std::remove_cv_t<Result>, ProtocolKeySet>,
                      "Protocol key callbacks must not return a copy of scoped keys");
        auto scoped = scopedProtocolKeys();
        if constexpr (std::is_void_v<Result>) {
            std::invoke(std::forward<Callback>(callback), scoped.get());
        } else {
            return std::invoke(std::forward<Callback>(callback), scoped.get());
        }
    }

    friend bool operator==(const VaultKeyring& left, const VaultKeyring& right) noexcept;

  private:
    friend class VaultKeyringCodec;

    VaultKeyring(VaultId vault_id, std::vector<SyncKeySlotId> slot_ids,
                 SyncKeySlotId current_write_slot_id, SecretBytes key_material) noexcept;

    [[nodiscard]] static auto fromDecoded(VaultId vault_id, std::vector<SyncKeySlotId> slot_ids,
                                          SyncKeySlotId current_write_slot_id,
                                          SecretBytes key_material)
        -> std::expected<VaultKeyring, VaultKeyringError>;

    VaultId vault_id_{};
    std::vector<SyncKeySlotId> slot_ids_;
    SyncKeySlotId current_write_slot_id_{};
    SecretBytes key_material_;
};

class VaultKeyringCodec final {
  public:
    static constexpr std::size_t fixed_header_bytes = 76;
    static constexpr std::size_t encoded_slot_bytes = sync_key_slot_id_bytes + sync_key_bytes;
    static constexpr std::size_t maximum_encoded_bytes =
        fixed_header_bytes + VaultKeyring::maximum_key_slots * encoded_slot_bytes;

    [[nodiscard]] static auto encode(const VaultKeyring& keyring)
        -> std::expected<SecretBytes, VaultKeyringError>;
    [[nodiscard]] static auto decode(std::span<const unsigned char> encoded)
        -> std::expected<VaultKeyring, VaultKeyringError>;
};

} // namespace appellate::sync
