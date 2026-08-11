#include "appellate/sync/protocol_codec.hpp"

#include <QByteArray>
#include <QDir>
#include <QFileDevice>
#include <QFileInfo>
#include <QIODevice>

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <utility>

namespace appellate::sync {
namespace {

constexpr char identity_domain[] = "appellate-workbench-sync-object-v1";
constexpr char remote_id_domain[] = "appellate-workbench-sync-remote-id-v1";
constexpr char associated_data_domain[] = "appellate-workbench-sync-envelope-v1";

constexpr std::array<unsigned char, 8> outer_magic{'A', 'W', 'S', 'O', 0, 1, 0, 0};
constexpr std::array<unsigned char, 8> inner_magic{'A', 'W', 'O', 'B', 'J', 0, 1, 0};
constexpr std::uint64_t inner_header_bytes =
    inner_magic.size() + 1ULL + 2ULL + 1ULL + sync_object_id_bytes + 8ULL;
constexpr std::uint64_t outer_header_bytes =
    outer_magic.size() + sync_key_slot_id_bytes + crypto_secretstream_xchacha20poly1305_HEADERBYTES;
constexpr std::uint64_t maximum_key_slots = 32;

template <typename T> class SodiumState final {
  public:
    SodiumState() = default;
    SodiumState(const SodiumState&) = delete;
    SodiumState& operator=(const SodiumState&) = delete;
    ~SodiumState() { sodium_memzero(&value, sizeof(value)); }

    T value{};
};

[[nodiscard]] auto fail(ProtocolErrorCode code, QString message) -> std::unexpected<ProtocolError> {
    return std::unexpected(ProtocolError{code, std::move(message)});
}

[[nodiscard]] bool validKind(SyncObjectKind kind) {
    switch (kind) {
    case SyncObjectKind::PackRevision:
    case SyncObjectKind::AuthoredRevision:
    case SyncObjectKind::SessionEventSegment:
    case SyncObjectKind::Checkpoint:
        return true;
    }
    return false;
}

[[nodiscard]] bool initializeSodium() { return sodium_init() >= 0; }

void appendBigEndian16(QByteArray& output, std::uint16_t value) {
    output.append(static_cast<char>((value >> 8U) & 0xffU));
    output.append(static_cast<char>(value & 0xffU));
}

void appendBigEndian32(QByteArray& output, std::uint32_t value) {
    for (auto shift : {24U, 16U, 8U, 0U}) {
        output.append(static_cast<char>((value >> shift) & 0xffU));
    }
}

void appendBigEndian64(QByteArray& output, std::uint64_t value) {
    for (auto shift : {56U, 48U, 40U, 32U, 24U, 16U, 8U, 0U}) {
        output.append(static_cast<char>((value >> shift) & 0xffU));
    }
}

[[nodiscard]] std::uint16_t readBigEndian16(const unsigned char* bytes) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8U) |
                                      static_cast<std::uint16_t>(bytes[1]));
}

[[nodiscard]] std::uint32_t readBigEndian32(const unsigned char* bytes) {
    std::uint32_t value{};
    for (std::size_t index = 0; index < 4; ++index) {
        value = (value << 8U) | static_cast<std::uint32_t>(bytes[index]);
    }
    return value;
}

[[nodiscard]] std::uint64_t readBigEndian64(const unsigned char* bytes) {
    std::uint64_t value{};
    for (std::size_t index = 0; index < 8; ++index) {
        value = (value << 8U) | static_cast<std::uint64_t>(bytes[index]);
    }
    return value;
}

void appendBytes(QByteArray& output, const unsigned char* bytes, std::size_t size) {
    output.append(reinterpret_cast<const char*>(bytes), static_cast<qsizetype>(size));
}

[[nodiscard]] auto bytesView(const QByteArray& bytes) -> const unsigned char* {
    return reinterpret_cast<const unsigned char*>(bytes.constData());
}

[[nodiscard]] QString lowerHex(const unsigned char* bytes, std::size_t size) {
    return QString::fromLatin1(
        QByteArray(reinterpret_cast<const char*>(bytes), static_cast<qsizetype>(size)).toHex());
}

[[nodiscard]] auto parseLowerHexId(QStringView value)
    -> std::optional<std::array<unsigned char, sync_object_id_bytes>> {
    if (value.size() != static_cast<qsizetype>(sync_object_id_bytes * 2U)) {
        return std::nullopt;
    }
    std::array<unsigned char, sync_object_id_bytes> decoded{};
    const auto nibble = [](QChar character) -> std::optional<unsigned char> {
        if (character >= u'0' && character <= u'9') {
            return static_cast<unsigned char>(character.unicode() - u'0');
        }
        if (character >= u'a' && character <= u'f') {
            return static_cast<unsigned char>(character.unicode() - u'a' + 10U);
        }
        return std::nullopt;
    };
    for (std::size_t index = 0; index < decoded.size(); ++index) {
        const auto high = nibble(value.at(static_cast<qsizetype>(index * 2U)));
        const auto low = nibble(value.at(static_cast<qsizetype>(index * 2U + 1U)));
        if (!high || !low) {
            return std::nullopt;
        }
        decoded[index] = static_cast<unsigned char>((*high << 4U) | *low);
    }
    return decoded;
}

[[nodiscard]] auto writeAll(QIODevice& destination, QByteArrayView bytes)
    -> std::expected<void, ProtocolError> {
    qsizetype written{};
    while (written < bytes.size()) {
        const auto count = destination.write(bytes.data() + written, bytes.size() - written);
        if (count <= 0) {
            return fail(ProtocolErrorCode::DestinationWriteFailed,
                        QStringLiteral("Cannot write the encrypted sync object"));
        }
        written += count;
    }
    return {};
}

[[nodiscard]] auto readExact(QIODevice& source, qsizetype count, ProtocolErrorCode short_read_code,
                             QString message) -> std::expected<QByteArray, ProtocolError> {
    QByteArray bytes(count, Qt::Uninitialized);
    qsizetype offset{};
    while (offset < count) {
        const auto read = source.read(bytes.data() + offset, count - offset);
        if (read < 0) {
            return fail(ProtocolErrorCode::SourceReadFailed,
                        QStringLiteral("Cannot read the encrypted sync object"));
        }
        if (read == 0) {
            return fail(short_read_code, std::move(message));
        }
        offset += read;
    }
    return bytes;
}

void updateSha256(crypto_hash_sha256_state& state, const unsigned char* bytes, std::uint64_t size) {
    crypto_hash_sha256_update(&state, bytes, static_cast<unsigned long long>(size));
}

void initializeCanonicalHash(crypto_hash_sha256_state& state, SyncObjectKind kind,
                             std::uint16_t schema_version, std::uint64_t payload_bytes) {
    crypto_hash_sha256_init(&state);
    updateSha256(state, reinterpret_cast<const unsigned char*>(identity_domain),
                 sizeof(identity_domain));
    const auto kind_byte = static_cast<unsigned char>(kind);
    updateSha256(state, &kind_byte, 1);
    std::array<unsigned char, 2> schema_bytes{
        static_cast<unsigned char>((schema_version >> 8U) & 0xffU),
        static_cast<unsigned char>(schema_version & 0xffU),
    };
    updateSha256(state, schema_bytes.data(), schema_bytes.size());
    std::array<unsigned char, 8> length_bytes{};
    for (std::size_t index = 0; index < length_bytes.size(); ++index) {
        const auto shift = static_cast<unsigned int>((length_bytes.size() - index - 1U) * 8U);
        length_bytes[index] = static_cast<unsigned char>((payload_bytes >> shift) & 0xffU);
    }
    updateSha256(state, length_bytes.data(), length_bytes.size());
}

[[nodiscard]] auto blindedId(const SyncObjectId& canonical_id, const SyncSecretKey& object_id_key)
    -> std::expected<std::array<unsigned char, sync_object_id_bytes>, ProtocolError> {
    SodiumState<crypto_generichash_state> state;
    std::array<unsigned char, sync_object_id_bytes> result{};
    if (crypto_generichash_init(&state.value, object_id_key.data(), object_id_key.size(),
                                result.size()) != 0) {
        return fail(ProtocolErrorCode::CryptoInitializationFailed,
                    QStringLiteral("Cannot initialize the blinded object identifier"));
    }
    crypto_generichash_update(&state.value,
                              reinterpret_cast<const unsigned char*>(remote_id_domain),
                              sizeof(remote_id_domain));
    crypto_generichash_update(&state.value, canonical_id.data(), canonical_id.size());
    if (crypto_generichash_final(&state.value, result.data(), result.size()) != 0) {
        return fail(ProtocolErrorCode::CryptoInitializationFailed,
                    QStringLiteral("Cannot finalize the blinded object identifier"));
    }
    return result;
}

[[nodiscard]] auto identityFromCanonicalId(const SyncObjectId& canonical_id,
                                           const SyncSecretKey& object_id_key)
    -> std::expected<SyncObjectIdentity, ProtocolError> {
    const auto remote = blindedId(canonical_id, object_id_key);
    if (!remote) {
        return std::unexpected(remote.error());
    }
    return SyncObjectIdentity{canonical_id, lowerHex(canonical_id.data(), canonical_id.size()),
                              lowerHex(remote->data(), remote->size())};
}

[[nodiscard]] auto hashPayload(QIODevice& payload, std::uint64_t payload_bytes, SyncObjectKind kind,
                               std::uint16_t schema_version, const SyncSecretKey& object_id_key)
    -> std::expected<SyncObjectIdentity, ProtocolError> {
    if (payload.isSequential()) {
        return fail(ProtocolErrorCode::SourceNotSeekable,
                    QStringLiteral("Sync payload input must be seekable"));
    }
    const auto initial_position = payload.pos();
    if (initial_position < 0) {
        return fail(ProtocolErrorCode::SourceNotSeekable,
                    QStringLiteral("Cannot identify the sync payload position"));
    }

    SodiumState<crypto_hash_sha256_state> state;
    initializeCanonicalHash(state.value, kind, schema_version, payload_bytes);
    std::array<char, static_cast<std::size_t>(ProtocolCodec::plaintext_chunk_bytes)> buffer{};
    auto remaining = payload_bytes;
    while (remaining > 0) {
        const auto requested = static_cast<qint64>(
            std::min<std::uint64_t>(remaining, ProtocolCodec::plaintext_chunk_bytes));
        const auto read = payload.read(buffer.data(), requested);
        if (read < 0) {
            static_cast<void>(payload.seek(initial_position));
            return fail(ProtocolErrorCode::SourceReadFailed,
                        QStringLiteral("Cannot hash the complete sync payload"));
        }
        if (read == 0) {
            static_cast<void>(payload.seek(initial_position));
            return fail(ProtocolErrorCode::SourceReadFailed,
                        QStringLiteral("Sync payload ended before its declared size"));
        }
        updateSha256(state.value, reinterpret_cast<const unsigned char*>(buffer.data()),
                     static_cast<std::uint64_t>(read));
        remaining -= static_cast<std::uint64_t>(read);
    }
    const auto trailing = payload.read(1);
    if (!trailing.isEmpty()) {
        static_cast<void>(payload.seek(initial_position));
        return fail(ProtocolErrorCode::InvalidArgument,
                    QStringLiteral("Sync payload has bytes beyond its declared size"));
    }
    if (trailing.isNull() && !payload.atEnd()) {
        static_cast<void>(payload.seek(initial_position));
        return fail(ProtocolErrorCode::SourceReadFailed,
                    QStringLiteral("Cannot verify the exact sync payload size"));
    }
    if (!payload.seek(initial_position)) {
        return fail(ProtocolErrorCode::SourceNotSeekable,
                    QStringLiteral("Cannot rewind the sync payload after hashing"));
    }

    SyncObjectId canonical_id{};
    crypto_hash_sha256_final(&state.value, canonical_id.data());
    return identityFromCanonicalId(canonical_id, object_id_key);
}

[[nodiscard]] QByteArray makeInnerHeader(SyncObjectKind kind, std::uint16_t schema_version,
                                         const SyncObjectId& canonical_id,
                                         std::uint64_t payload_bytes) {
    QByteArray header;
    header.reserve(static_cast<qsizetype>(inner_header_bytes));
    appendBytes(header, inner_magic.data(), inner_magic.size());
    header.append(static_cast<char>(kind));
    appendBigEndian16(header, schema_version);
    header.append('\0');
    appendBytes(header, canonical_id.data(), canonical_id.size());
    appendBigEndian64(header, payload_bytes);
    return header;
}

[[nodiscard]] QByteArray makeAssociatedData(const QByteArray& outer_header,
                                            const SyncObjectId& remote_object_id,
                                            std::uint64_t frame_index) {
    QByteArray associated;
    associated.reserve(static_cast<qsizetype>(sizeof(associated_data_domain) +
                                              static_cast<std::size_t>(outer_header.size()) +
                                              remote_object_id.size() + 8U));
    associated.append(associated_data_domain,
                      static_cast<qsizetype>(sizeof(associated_data_domain)));
    associated.append(outer_header);
    appendBytes(associated, remote_object_id.data(), remote_object_id.size());
    appendBigEndian64(associated, frame_index);
    return associated;
}

[[nodiscard]] auto findKeySlot(const ProtocolKeySet& keys, const SyncKeySlotId& id)
    -> const ProtocolKeySlot* {
    for (const auto& slot : keys.key_slots) {
        if (sodium_memcmp(slot.id.data(), id.data(), id.size()) == 0) {
            return &slot;
        }
    }
    return nullptr;
}

[[nodiscard]] bool validKeySet(const ProtocolKeySet& keys, bool require_active) {
    if (keys.key_slots.empty() || keys.key_slots.size() > maximum_key_slots ||
        (require_active && keys.active_slot >= keys.key_slots.size())) {
        return false;
    }
    for (std::size_t left = 0; left < keys.key_slots.size(); ++left) {
        for (std::size_t right = left + 1U; right < keys.key_slots.size(); ++right) {
            if (keys.key_slots[left].id == keys.key_slots[right].id) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] auto parseInnerHeader(QByteArrayView plaintext, ProtocolLimits limits)
    -> std::expected<
        std::tuple<SyncObjectKind, std::uint16_t, SyncObjectId, std::uint64_t, std::uint64_t>,
        ProtocolError> {
    if (plaintext.size() < static_cast<qsizetype>(inner_header_bytes)) {
        return fail(ProtocolErrorCode::MalformedEnvelope,
                    QStringLiteral("The first encrypted frame has no complete object header"));
    }
    const auto* bytes = reinterpret_cast<const unsigned char*>(plaintext.data());
    if (sodium_memcmp(bytes, inner_magic.data(), inner_magic.size()) != 0) {
        return fail(ProtocolErrorCode::MalformedEnvelope,
                    QStringLiteral("The encrypted object header is invalid"));
    }
    const auto kind = static_cast<SyncObjectKind>(bytes[inner_magic.size()]);
    const auto schema_offset = inner_magic.size() + 1U;
    const auto schema_version = readBigEndian16(bytes + schema_offset);
    const auto reserved_offset = schema_offset + 2U;
    if (!validKind(kind) || schema_version == 0 || bytes[reserved_offset] != 0U) {
        return fail(ProtocolErrorCode::MalformedEnvelope,
                    QStringLiteral("The encrypted object type or schema is invalid"));
    }
    const auto identity_offset = reserved_offset + 1U;
    SyncObjectId embedded_identity{};
    std::memcpy(embedded_identity.data(), bytes + identity_offset, embedded_identity.size());
    const auto payload_offset = identity_offset + embedded_identity.size();
    const auto payload_bytes = readBigEndian64(bytes + payload_offset);
    if (payload_bytes > limits.maximum_payload_bytes) {
        return fail(ProtocolErrorCode::ObjectTooLarge,
                    QStringLiteral("The encrypted object exceeds the configured payload limit"));
    }
    const auto padded = ProtocolCodec::paddedPlaintextSize(payload_bytes);
    if (!padded) {
        return std::unexpected(padded.error());
    }
    return std::tuple{kind, schema_version, embedded_identity, payload_bytes, *padded};
}

} // namespace

std::expected<SyncObjectIdentity, ProtocolError>
ProtocolCodec::canonicalIdentity(SyncObjectKind kind, std::uint16_t schema_version,
                                 QByteArrayView payload, const SyncSecretKey& object_id_key) {
    if (!initializeSodium()) {
        return fail(ProtocolErrorCode::CryptoInitializationFailed,
                    QStringLiteral("Cannot initialize libsodium"));
    }
    if (!validKind(kind) || schema_version == 0 || payload.size() < 0) {
        return fail(ProtocolErrorCode::InvalidArgument,
                    QStringLiteral("Object kind, schema, and payload must be canonical"));
    }
    SodiumState<crypto_hash_sha256_state> state;
    initializeCanonicalHash(state.value, kind, schema_version,
                            static_cast<std::uint64_t>(payload.size()));
    updateSha256(state.value, reinterpret_cast<const unsigned char*>(payload.data()),
                 static_cast<std::uint64_t>(payload.size()));
    SyncObjectId canonical_id{};
    crypto_hash_sha256_final(&state.value, canonical_id.data());
    return identityFromCanonicalId(canonical_id, object_id_key);
}

std::expected<std::uint64_t, ProtocolError>
ProtocolCodec::paddedPlaintextSize(std::uint64_t payload_bytes) {
    if (payload_bytes > std::numeric_limits<std::uint64_t>::max() - inner_header_bytes) {
        return fail(ProtocolErrorCode::ObjectTooLarge,
                    QStringLiteral("The object size cannot be represented"));
    }
    const auto minimum = payload_bytes + inner_header_bytes;
    if (minimum <= minimum_padding_bucket_bytes) {
        return minimum_padding_bucket_bytes;
    }
    if (minimum <= maximum_power_of_two_padding_bucket_bytes) {
        auto bucket = minimum_padding_bucket_bytes;
        while (bucket < minimum) {
            bucket *= 2ULL;
        }
        return bucket;
    }
    const auto unit = maximum_power_of_two_padding_bucket_bytes;
    if (minimum > std::numeric_limits<std::uint64_t>::max() - (unit - 1ULL)) {
        return fail(ProtocolErrorCode::ObjectTooLarge,
                    QStringLiteral("The padded object size cannot be represented"));
    }
    return ((minimum + unit - 1ULL) / unit) * unit;
}

std::expected<EncryptedSyncObject, ProtocolError>
ProtocolCodec::encrypt(SyncObjectKind kind, std::uint16_t schema_version, QIODevice& payload,
                       std::uint64_t payload_bytes, const ProtocolKeySet& keys,
                       QIODevice& ciphertext, ProtocolLimits limits) {
    if (!initializeSodium()) {
        return fail(ProtocolErrorCode::CryptoInitializationFailed,
                    QStringLiteral("Cannot initialize libsodium"));
    }
    if (!validKind(kind) || schema_version == 0 || !validKeySet(keys, true) ||
        !payload.isReadable() || !ciphertext.isWritable() || limits.maximum_payload_bytes == 0) {
        return fail(ProtocolErrorCode::InvalidArgument,
                    QStringLiteral("The encryption request is invalid"));
    }
    if (payload_bytes > limits.maximum_payload_bytes) {
        return fail(ProtocolErrorCode::ObjectTooLarge,
                    QStringLiteral("The sync payload exceeds the configured limit"));
    }
    const auto padded_size = paddedPlaintextSize(payload_bytes);
    if (!padded_size) {
        return std::unexpected(padded_size.error());
    }
    const auto identity =
        hashPayload(payload, payload_bytes, kind, schema_version, keys.object_id_key);
    if (!identity) {
        return std::unexpected(identity.error());
    }
    const auto remote_id = parseLowerHexId(identity->remote_object_id);
    if (!remote_id) {
        return fail(ProtocolErrorCode::CryptoInitializationFailed,
                    QStringLiteral("The blinded object identifier is invalid"));
    }

    const auto& active = keys.key_slots.at(keys.active_slot);
    SodiumState<crypto_secretstream_xchacha20poly1305_state> state;
    std::array<unsigned char, crypto_secretstream_xchacha20poly1305_HEADERBYTES> stream_header{};
    if (crypto_secretstream_xchacha20poly1305_init_push(&state.value, stream_header.data(),
                                                        active.encryption_key.data()) != 0) {
        return fail(ProtocolErrorCode::CryptoInitializationFailed,
                    QStringLiteral("Cannot initialize encrypted object output"));
    }

    QByteArray outer_header;
    outer_header.reserve(static_cast<qsizetype>(outer_header_bytes));
    appendBytes(outer_header, outer_magic.data(), outer_magic.size());
    appendBytes(outer_header, active.id.data(), active.id.size());
    appendBytes(outer_header, stream_header.data(), stream_header.size());
    if (const auto written = writeAll(ciphertext, outer_header); !written) {
        return std::unexpected(written.error());
    }
    std::uint64_t ciphertext_bytes = outer_header_bytes;

    const auto inner_header =
        makeInnerHeader(kind, schema_version, identity->canonical_id, payload_bytes);
    std::uint64_t inner_offset{};
    std::uint64_t payload_offset{};
    std::uint64_t frame_index{};
    while (inner_offset < *padded_size) {
        const auto plain_count =
            std::min<std::uint64_t>(plaintext_chunk_bytes, *padded_size - inner_offset);
        QByteArray plaintext(static_cast<qsizetype>(plain_count), Qt::Uninitialized);
        std::uint64_t filled{};

        if (inner_offset < inner_header_bytes) {
            const auto prefix_count =
                std::min<std::uint64_t>(plain_count, inner_header_bytes - inner_offset);
            std::memcpy(plaintext.data(),
                        inner_header.constData() + static_cast<qsizetype>(inner_offset),
                        static_cast<std::size_t>(prefix_count));
            filled += prefix_count;
        }
        while (filled < plain_count && payload_offset < payload_bytes) {
            const auto requested = static_cast<qint64>(
                std::min<std::uint64_t>(plain_count - filled, payload_bytes - payload_offset));
            const auto read =
                payload.read(plaintext.data() + static_cast<qsizetype>(filled), requested);
            if (read < 0) {
                sodium_memzero(plaintext.data(), static_cast<std::size_t>(plaintext.size()));
                return fail(ProtocolErrorCode::SourceReadFailed,
                            QStringLiteral("Cannot read the complete sync payload"));
            }
            if (read == 0) {
                sodium_memzero(plaintext.data(), static_cast<std::size_t>(plaintext.size()));
                return fail(ProtocolErrorCode::SourceReadFailed,
                            QStringLiteral("Sync payload ended before its declared size"));
            }
            filled += static_cast<std::uint64_t>(read);
            payload_offset += static_cast<std::uint64_t>(read);
        }
        if (filled < plain_count) {
            randombytes_buf(plaintext.data() + static_cast<qsizetype>(filled),
                            static_cast<std::size_t>(plain_count - filled));
        }

        QByteArray encrypted(
            static_cast<qsizetype>(plain_count + crypto_secretstream_xchacha20poly1305_ABYTES),
            Qt::Uninitialized);
        unsigned long long encrypted_count{};
        const auto associated = makeAssociatedData(outer_header, *remote_id, frame_index);
        const auto tag =
            static_cast<unsigned char>(inner_offset + plain_count == *padded_size
                                           ? crypto_secretstream_xchacha20poly1305_TAG_FINAL
                                           : crypto_secretstream_xchacha20poly1305_TAG_MESSAGE);
        const auto pushed = crypto_secretstream_xchacha20poly1305_push(
            &state.value, reinterpret_cast<unsigned char*>(encrypted.data()), &encrypted_count,
            reinterpret_cast<const unsigned char*>(plaintext.constData()), plain_count,
            reinterpret_cast<const unsigned char*>(associated.constData()),
            static_cast<unsigned long long>(associated.size()), tag);
        sodium_memzero(plaintext.data(), static_cast<std::size_t>(plaintext.size()));
        if (pushed != 0 ||
            encrypted_count != plain_count + crypto_secretstream_xchacha20poly1305_ABYTES) {
            return fail(ProtocolErrorCode::CryptoInitializationFailed,
                        QStringLiteral("Cannot encrypt the sync object frame"));
        }

        QByteArray frame_size;
        frame_size.reserve(4);
        appendBigEndian32(frame_size, static_cast<std::uint32_t>(encrypted_count));
        if (const auto prefix_written = writeAll(ciphertext, frame_size); !prefix_written) {
            return std::unexpected(prefix_written.error());
        }
        if (const auto frame_written = writeAll(ciphertext, encrypted); !frame_written) {
            return std::unexpected(frame_written.error());
        }
        ciphertext_bytes += 4ULL + static_cast<std::uint64_t>(encrypted_count);
        inner_offset += plain_count;
        ++frame_index;
    }
    if (payload_offset != payload_bytes) {
        return fail(ProtocolErrorCode::SourceReadFailed,
                    QStringLiteral("The encrypted payload length is inconsistent"));
    }

    return EncryptedSyncObject{kind,          schema_version, *identity,       active.id,
                               payload_bytes, *padded_size,   ciphertext_bytes};
}

std::expected<VerifiedSyncObject, ProtocolError>
ProtocolCodec::decrypt(QIODevice& ciphertext, QStringView expected_remote_object_id,
                       const ProtocolKeySet& keys, const QString& quarantine_directory,
                       ProtocolLimits limits) {
    if (!initializeSodium()) {
        return fail(ProtocolErrorCode::CryptoInitializationFailed,
                    QStringLiteral("Cannot initialize libsodium"));
    }
    const auto expected_remote = parseLowerHexId(expected_remote_object_id);
    if (!expected_remote || !validKeySet(keys, false) || !ciphertext.isReadable() ||
        limits.maximum_payload_bytes == 0) {
        return fail(ProtocolErrorCode::InvalidArgument,
                    QStringLiteral("The decryption request is invalid"));
    }

    const auto outer = readExact(ciphertext, static_cast<qsizetype>(outer_header_bytes),
                                 ProtocolErrorCode::MalformedEnvelope,
                                 QStringLiteral("The encrypted object header is truncated"));
    if (!outer) {
        return std::unexpected(outer.error());
    }
    const auto* outer_bytes = bytesView(*outer);
    if (sodium_memcmp(outer_bytes, outer_magic.data(), outer_magic.size()) != 0) {
        return fail(ProtocolErrorCode::MalformedEnvelope,
                    QStringLiteral("The encrypted object protocol header is invalid"));
    }
    SyncKeySlotId slot_id{};
    std::memcpy(slot_id.data(), outer_bytes + outer_magic.size(), slot_id.size());
    const auto* slot = findKeySlot(keys, slot_id);
    if (slot == nullptr) {
        return fail(ProtocolErrorCode::UnknownKeySlot,
                    QStringLiteral("The encrypted object key slot is unavailable"));
    }

    const auto stream_header_offset = outer_magic.size() + slot_id.size();
    SodiumState<crypto_secretstream_xchacha20poly1305_state> state;
    if (crypto_secretstream_xchacha20poly1305_init_pull(
            &state.value, outer_bytes + stream_header_offset, slot->encryption_key.data()) != 0) {
        return fail(ProtocolErrorCode::AuthenticationFailed,
                    QStringLiteral("The encrypted object header cannot be authenticated"));
    }

    const auto quarantine_root = quarantine_directory.isEmpty()
                                     ? QDir::tempPath()
                                     : QFileInfo(quarantine_directory).absoluteFilePath();
    const QFileInfo quarantine_info(quarantine_root);
    if (!quarantine_info.isDir() || quarantine_info.isSymLink()) {
        return fail(ProtocolErrorCode::CannotCreateQuarantine,
                    QStringLiteral("The sync quarantine directory is unavailable or unsafe"));
    }
    auto quarantine = std::make_unique<QTemporaryFile>(
        QDir(quarantine_root).filePath(QStringLiteral(".appellate-sync-XXXXXX.quarantine")));
    quarantine->setAutoRemove(true);
    if (!quarantine->open() ||
        !quarantine->setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        return fail(ProtocolErrorCode::CannotCreateQuarantine,
                    QStringLiteral("Cannot create a private sync quarantine file"));
    }

    bool parsed_header = false;
    SyncObjectKind kind{};
    std::uint16_t schema_version{};
    SyncObjectId embedded_identity{};
    std::uint64_t payload_bytes{};
    std::uint64_t padded_bytes{};
    std::uint64_t plaintext_seen{};
    std::uint64_t payload_written{};
    std::uint64_t frame_index{};
    SodiumState<crypto_hash_sha256_state> canonical_hash;

    while (true) {
        const auto prefix_first = ciphertext.read(1);
        if (prefix_first.isNull() && !ciphertext.atEnd()) {
            return fail(ProtocolErrorCode::SourceReadFailed,
                        QStringLiteral("Cannot read the encrypted frame prefix"));
        }
        if (prefix_first.isEmpty()) {
            return fail(ProtocolErrorCode::MissingFinalTag,
                        QStringLiteral("The encrypted object ended without a final frame"));
        }
        const auto prefix_tail =
            readExact(ciphertext, 3, ProtocolErrorCode::MalformedEnvelope,
                      QStringLiteral("An encrypted frame prefix is truncated"));
        if (!prefix_tail) {
            return std::unexpected(prefix_tail.error());
        }
        QByteArray prefix = prefix_first;
        prefix.append(*prefix_tail);
        const auto encrypted_size = readBigEndian32(bytesView(prefix));
        constexpr auto maximum_encrypted_frame =
            plaintext_chunk_bytes + crypto_secretstream_xchacha20poly1305_ABYTES;
        if (encrypted_size < crypto_secretstream_xchacha20poly1305_ABYTES ||
            encrypted_size > maximum_encrypted_frame) {
            return fail(ProtocolErrorCode::MalformedEnvelope,
                        QStringLiteral("An encrypted frame length is invalid"));
        }
        const auto encrypted = readExact(ciphertext, static_cast<qsizetype>(encrypted_size),
                                         ProtocolErrorCode::MalformedEnvelope,
                                         QStringLiteral("An encrypted frame is truncated"));
        if (!encrypted) {
            return std::unexpected(encrypted.error());
        }

        QByteArray plaintext(
            static_cast<qsizetype>(encrypted_size - crypto_secretstream_xchacha20poly1305_ABYTES),
            Qt::Uninitialized);
        unsigned long long plaintext_size{};
        unsigned char tag{};
        const auto associated = makeAssociatedData(*outer, *expected_remote, frame_index);
        if (crypto_secretstream_xchacha20poly1305_pull(
                &state.value, reinterpret_cast<unsigned char*>(plaintext.data()), &plaintext_size,
                &tag, bytesView(*encrypted), encrypted_size,
                reinterpret_cast<const unsigned char*>(associated.constData()),
                static_cast<unsigned long long>(associated.size())) != 0) {
            sodium_memzero(plaintext.data(), static_cast<std::size_t>(plaintext.size()));
            return fail(ProtocolErrorCode::AuthenticationFailed,
                        QStringLiteral("An encrypted object frame failed authentication"));
        }
        if (plaintext_size != static_cast<unsigned long long>(plaintext.size())) {
            sodium_memzero(plaintext.data(), static_cast<std::size_t>(plaintext.size()));
            return fail(ProtocolErrorCode::MalformedEnvelope,
                        QStringLiteral("An encrypted frame has an invalid plaintext length"));
        }
        if (tag != crypto_secretstream_xchacha20poly1305_TAG_MESSAGE &&
            tag != crypto_secretstream_xchacha20poly1305_TAG_FINAL) {
            sodium_memzero(plaintext.data(), static_cast<std::size_t>(plaintext.size()));
            return fail(ProtocolErrorCode::MalformedEnvelope,
                        QStringLiteral("An encrypted frame uses a forbidden stream tag"));
        }

        if (!parsed_header) {
            const auto parsed = parseInnerHeader(plaintext, limits);
            if (!parsed) {
                sodium_memzero(plaintext.data(), static_cast<std::size_t>(plaintext.size()));
                return std::unexpected(parsed.error());
            }
            std::tie(kind, schema_version, embedded_identity, payload_bytes, padded_bytes) =
                *parsed;
            initializeCanonicalHash(canonical_hash.value, kind, schema_version, payload_bytes);
            parsed_header = true;
        }

        if (plaintext_seen > padded_bytes ||
            static_cast<std::uint64_t>(plaintext.size()) > padded_bytes - plaintext_seen) {
            sodium_memzero(plaintext.data(), static_cast<std::size_t>(plaintext.size()));
            return fail(ProtocolErrorCode::MalformedEnvelope,
                        QStringLiteral("The encrypted object exceeds its padded size"));
        }
        const auto chunk_start = plaintext_seen;
        const auto chunk_end = plaintext_seen + static_cast<std::uint64_t>(plaintext.size());
        const auto payload_start = inner_header_bytes;
        const auto payload_end = payload_start + payload_bytes;
        const auto overlap_start = std::max(chunk_start, payload_start);
        const auto overlap_end = std::min(chunk_end, payload_end);
        if (overlap_start < overlap_end) {
            const auto local_offset = overlap_start - chunk_start;
            const auto count = overlap_end - overlap_start;
            const auto* payload_pointer =
                plaintext.constData() + static_cast<qsizetype>(local_offset);
            updateSha256(canonical_hash.value,
                         reinterpret_cast<const unsigned char*>(payload_pointer), count);
            if (const auto stored = writeAll(
                    *quarantine, QByteArrayView(payload_pointer, static_cast<qsizetype>(count)));
                !stored) {
                sodium_memzero(plaintext.data(), static_cast<std::size_t>(plaintext.size()));
                return std::unexpected(stored.error());
            }
            payload_written += count;
        }
        plaintext_seen = chunk_end;
        sodium_memzero(plaintext.data(), static_cast<std::size_t>(plaintext.size()));

        if (tag == crypto_secretstream_xchacha20poly1305_TAG_FINAL) {
            if (plaintext_seen != padded_bytes || payload_written != payload_bytes) {
                return fail(ProtocolErrorCode::MalformedEnvelope,
                            QStringLiteral("The final frame does not match the declared size"));
            }
            const auto trailing = ciphertext.read(1);
            if (trailing.isNull() && !ciphertext.atEnd()) {
                return fail(ProtocolErrorCode::SourceReadFailed,
                            QStringLiteral("Cannot verify the end of the encrypted object"));
            }
            if (!trailing.isEmpty()) {
                return fail(ProtocolErrorCode::TrailingCiphertext,
                            QStringLiteral("The encrypted object has trailing bytes"));
            }

            SyncObjectId canonical_id{};
            crypto_hash_sha256_final(&canonical_hash.value, canonical_id.data());
            if (sodium_memcmp(canonical_id.data(), embedded_identity.data(), canonical_id.size()) !=
                0) {
                return fail(
                    ProtocolErrorCode::IdentityMismatch,
                    QStringLiteral("The decrypted object identity does not match its payload"));
            }
            const auto identity = identityFromCanonicalId(canonical_id, keys.object_id_key);
            if (!identity) {
                return std::unexpected(identity.error());
            }
            const auto computed_remote = parseLowerHexId(identity->remote_object_id);
            if (!computed_remote || sodium_memcmp(computed_remote->data(), expected_remote->data(),
                                                  expected_remote->size()) != 0) {
                return fail(ProtocolErrorCode::IdentityMismatch,
                            QStringLiteral("The decrypted object does not match its remote key"));
            }
            if (!quarantine->flush() || !quarantine->seek(0)) {
                return fail(ProtocolErrorCode::DestinationWriteFailed,
                            QStringLiteral("Cannot finalize the quarantined sync payload"));
            }
            return VerifiedSyncObject{
                kind,         schema_version,       *identity, slot_id, payload_bytes,
                padded_bytes, std::move(quarantine)};
        }
        if (plaintext_seen == padded_bytes) {
            return fail(ProtocolErrorCode::MissingFinalTag,
                        QStringLiteral("The encrypted object has no final stream tag"));
        }
        ++frame_index;
    }
}

} // namespace appellate::sync
