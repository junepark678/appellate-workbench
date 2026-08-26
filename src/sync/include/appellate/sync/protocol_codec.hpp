#pragma once

#include "appellate/sync/sync_object.hpp"

#include <QByteArrayView>
#include <QStringView>

#include <expected>
#include <memory>

class QIODevice;
class QTemporaryFile;

namespace appellate::sync {

class ProtocolCodec final {
  public:
    static constexpr std::uint64_t plaintext_chunk_bytes = 64ULL * 1024ULL;
    static constexpr std::uint64_t minimum_padding_bucket_bytes = 4ULL * 1024ULL;
    static constexpr std::uint64_t maximum_power_of_two_padding_bucket_bytes = 1024ULL * 1024ULL;

    [[nodiscard]] static auto canonicalIdentity(SyncObjectKind kind, std::uint16_t schema_version,
                                                QByteArrayView payload,
                                                const SyncSecretKey& object_id_key)
        -> std::expected<SyncObjectIdentity, ProtocolError>;

    [[nodiscard]] static auto paddedPlaintextSize(std::uint64_t payload_bytes)
        -> std::expected<std::uint64_t, ProtocolError>;

    [[nodiscard]] static auto encrypt(SyncObjectKind kind, std::uint16_t schema_version,
                                      QIODevice& payload, std::uint64_t payload_bytes,
                                      const ProtocolKeySet& keys, QIODevice& ciphertext,
                                      ProtocolLimits limits = {})
        -> std::expected<EncryptedSyncObject, ProtocolError>;

    // Successful decryption returns an auto-removing quarantine file positioned at byte zero.
    // A failed call never returns or publishes any plaintext output.
    [[nodiscard]] static auto decrypt(QIODevice& ciphertext, QStringView expected_remote_object_id,
                                      const ProtocolKeySet& keys,
                                      const QString& quarantine_directory = {},
                                      ProtocolLimits limits = {})
        -> std::expected<VerifiedSyncObject, ProtocolError>;

  private:
    friend class ObjectTransport;

    // ObjectTransport supplies an already-open, owner-only, descriptor-relative file that has
    // been unlinked from its directory. Keeping this seam private prevents callers from weakening
    // the public decrypt-to-quarantine contract with an arbitrary destination.
    [[nodiscard]] static auto
    decryptIntoQuarantine(QIODevice& ciphertext, QStringView expected_remote_object_id,
                          const ProtocolKeySet& keys, std::unique_ptr<QTemporaryFile> quarantine,
                          ProtocolLimits limits = {})
        -> std::expected<VerifiedSyncObject, ProtocolError>;
};

} // namespace appellate::sync
