#pragma once

#include <QByteArray>
#include <QString>
#include <QTemporaryFile>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace appellate::sync {

inline constexpr std::size_t sync_object_id_bytes = 32;
inline constexpr std::size_t sync_key_bytes = 32;
inline constexpr std::size_t sync_key_slot_id_bytes = 16;

enum class SyncObjectKind : std::uint8_t {
    PackRevision = 1,
    AuthoredRevision = 2,
    SessionEventSegment = 3,
    Checkpoint = 4,
};

using SyncObjectId = std::array<unsigned char, sync_object_id_bytes>;
using SyncSecretKey = std::array<unsigned char, sync_key_bytes>;
using SyncKeySlotId = std::array<unsigned char, sync_key_slot_id_bytes>;

struct ProtocolKeySlot final {
    SyncKeySlotId id{};
    SyncSecretKey encryption_key{};

    friend bool operator==(const ProtocolKeySlot&, const ProtocolKeySlot&) = default;
};

// This value-based type is an injection seam for the protocol slice. A production key service
// must populate it from locked memory backed by the OS key store and wipe its owner after use.
struct ProtocolKeySet final {
    SyncSecretKey object_id_key{};
    std::vector<ProtocolKeySlot> key_slots;
    std::size_t active_slot{};
};

struct ProtocolLimits final {
    static constexpr std::uint64_t default_maximum_payload_bytes =
        3ULL * 1024ULL * 1024ULL * 1024ULL;

    std::uint64_t maximum_payload_bytes{default_maximum_payload_bytes};
};

struct SyncObjectIdentity final {
    SyncObjectId canonical_id{};
    QString canonical_id_hex;
    QString remote_object_id;

    friend bool operator==(const SyncObjectIdentity&, const SyncObjectIdentity&) = default;
};

struct EncryptedSyncObject final {
    SyncObjectKind kind{};
    std::uint16_t schema_version{};
    SyncObjectIdentity identity;
    SyncKeySlotId key_slot_id{};
    std::uint64_t payload_bytes{};
    std::uint64_t padded_plaintext_bytes{};
    std::uint64_t ciphertext_bytes{};
};

struct VerifiedSyncObject final {
    SyncObjectKind kind{};
    std::uint16_t schema_version{};
    SyncObjectIdentity identity;
    SyncKeySlotId key_slot_id{};
    std::uint64_t payload_bytes{};
    std::uint64_t padded_plaintext_bytes{};
    std::unique_ptr<QTemporaryFile> quarantined_payload;
};

enum class ProtocolErrorCode {
    InvalidArgument,
    CryptoInitializationFailed,
    SourceNotSeekable,
    SourceReadFailed,
    DestinationWriteFailed,
    ObjectTooLarge,
    MalformedEnvelope,
    UnknownKeySlot,
    AuthenticationFailed,
    MissingFinalTag,
    TrailingCiphertext,
    IdentityMismatch,
    CannotCreateQuarantine,
};

struct ProtocolError final {
    ProtocolErrorCode code{};
    QString message;

    friend bool operator==(const ProtocolError&, const ProtocolError&) = default;
};

} // namespace appellate::sync
