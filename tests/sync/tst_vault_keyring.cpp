#include "appellate/sync/protocol_codec.hpp"
#include "appellate/sync/secret_bytes.hpp"
#include "appellate/sync/vault_keyring.hpp"

#include <QBuffer>
#include <QByteArray>
#include <QTemporaryDir>
#include <QTest>

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>
#include <vector>

namespace {

using appellate::sync::EncryptedSyncObject;
using appellate::sync::ProtocolCodec;
using appellate::sync::ProtocolError;
using appellate::sync::ProtocolErrorCode;
using appellate::sync::ProtocolKeySet;
using appellate::sync::ScopedProtocolKeySet;
using appellate::sync::SecretBytes;
using appellate::sync::SyncObjectKind;
using appellate::sync::VaultKeyring;
using appellate::sync::VaultKeyringCodec;
using appellate::sync::VaultKeyringErrorCode;
using appellate::sync::VerifiedSyncObject;

struct CipherFixture final {
    EncryptedSyncObject metadata;
    QByteArray ciphertext;
};

[[nodiscard]] std::vector<unsigned char> frozenKeyringBytes() {
    const auto bytes = QByteArray::fromHex(
        // AWKR v1, vault ID 10..1f, object-ID key 20..3f.
        "41574b5200010000"
        "101112131415161718191a1b1c1d1e1f"
        "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"
        // Current slot is the second slot, count two, reserved zero.
        "707172737475767778797a7b7c7d7e7f00020000"
        // Slot one ID 40..4f and DEK 50..6f.
        "404142434445464748494a4b4c4d4e4f"
        "505152535455565758595a5b5c5d5e5f606162636465666768696a6b6c6d6e6f"
        // Slot two ID 70..7f and DEK 80..9f.
        "707172737475767778797a7b7c7d7e7f"
        "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
    const auto* first = reinterpret_cast<const unsigned char*>(bytes.constData());
    return {first, first + bytes.size()};
}

[[nodiscard]] bool allZero(std::span<const unsigned char> bytes) {
    return std::ranges::all_of(bytes, [](unsigned char byte) { return byte == 0U; });
}

void fillBytes(std::vector<unsigned char>& bytes, std::size_t offset, std::size_t count,
               unsigned char value) {
    for (std::size_t index = 0; index < count; ++index) {
        bytes.at(offset + index) = value;
    }
}

void copyBytes(std::vector<unsigned char>& bytes, std::size_t source_offset,
               std::size_t destination_offset, std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
        bytes.at(destination_offset + index) = bytes.at(source_offset + index);
    }
}

[[nodiscard]] auto encrypt(const VaultKeyring& keyring, const QByteArray& payload)
    -> std::expected<CipherFixture, ProtocolError> {
    return keyring.withProtocolKeys([&](const ProtocolKeySet& keys) {
        QByteArray source_bytes = payload;
        QBuffer source(&source_bytes);
        if (!source.open(QIODevice::ReadOnly)) {
            return std::expected<CipherFixture, ProtocolError>{std::unexpected(ProtocolError{
                ProtocolErrorCode::InvalidArgument, QStringLiteral("test payload source")})};
        }
        QByteArray ciphertext;
        QBuffer destination(&ciphertext);
        if (!destination.open(QIODevice::WriteOnly)) {
            return std::expected<CipherFixture, ProtocolError>{std::unexpected(ProtocolError{
                ProtocolErrorCode::InvalidArgument, QStringLiteral("test cipher destination")})};
        }
        auto result = ProtocolCodec::encrypt(SyncObjectKind::SessionEventSegment, 1, source,
                                             static_cast<std::uint64_t>(source_bytes.size()), keys,
                                             destination);
        if (!result) {
            return std::expected<CipherFixture, ProtocolError>{std::unexpected(result.error())};
        }
        return std::expected<CipherFixture, ProtocolError>{
            CipherFixture{*result, std::move(ciphertext)}};
    });
}

[[nodiscard]] auto decrypt(const VaultKeyring& keyring, const CipherFixture& fixture,
                           const QString& quarantine_directory)
    -> std::expected<VerifiedSyncObject, ProtocolError> {
    return keyring.withProtocolKeys([&](const ProtocolKeySet& keys) {
        QByteArray ciphertext = fixture.ciphertext;
        QBuffer source(&ciphertext);
        if (!source.open(QIODevice::ReadOnly)) {
            return std::expected<VerifiedSyncObject, ProtocolError>{std::unexpected(ProtocolError{
                ProtocolErrorCode::InvalidArgument, QStringLiteral("test cipher source")})};
        }
        return ProtocolCodec::decrypt(source, fixture.metadata.identity.remote_object_id, keys,
                                      quarantine_directory);
    });
}

class VaultKeyringTest final : public QObject {
    Q_OBJECT

  private slots:
    void matchesFrozenStrictCodecVector();
    void rejectsEveryTruncationAndMalformedField();
    void rotationRetainsOldReadKeysAndChangesTheWriteSlot();
    void enforcesTheRotationSlotBound();
    void guardedSecretsAndProtocolCopiesAreMoveOnlyAndClearable();
};

void VaultKeyringTest::matchesFrozenStrictCodecVector() {
    const auto frozen = frozenKeyringBytes();
    QCOMPARE(frozen.size(), std::size_t{172});
    auto keyring = VaultKeyringCodec::decode(frozen);
    QVERIFY(keyring.has_value());
    QCOMPARE(keyring->keySlotCount(), std::size_t{2});
    QVERIFY(std::equal(keyring->vaultId().begin(), keyring->vaultId().end(), frozen.begin() + 8));
    QVERIFY(std::equal(keyring->objectIdKey().begin(), keyring->objectIdKey().end(),
                       frozen.begin() + 24));
    QVERIFY(std::equal(keyring->currentWriteSlotId().begin(), keyring->currentWriteSlotId().end(),
                       frozen.begin() + 56));
    QVERIFY(std::equal(keyring->keySlotId(0).begin(), keyring->keySlotId(0).end(),
                       frozen.begin() + 76));
    QVERIFY(std::equal(keyring->encryptionKey(0).begin(), keyring->encryptionKey(0).end(),
                       frozen.begin() + 92));
    QVERIFY(std::equal(keyring->keySlotId(1).begin(), keyring->keySlotId(1).end(),
                       frozen.begin() + 124));
    QVERIFY(std::equal(keyring->encryptionKey(1).begin(), keyring->encryptionKey(1).end(),
                       frozen.begin() + 140));

    auto encoded = VaultKeyringCodec::encode(*keyring);
    QVERIFY(encoded.has_value());
    QVERIFY(std::ranges::equal(encoded->bytes(), frozen));
}

void VaultKeyringTest::rejectsEveryTruncationAndMalformedField() {
    const auto frozen = frozenKeyringBytes();
    for (std::size_t length = 0; length < frozen.size(); ++length) {
        const auto result =
            VaultKeyringCodec::decode(std::span<const unsigned char>{frozen.data(), length});
        QVERIFY2(!result.has_value(), "a truncated keyring was accepted");
    }

    auto trailing = frozen;
    trailing.push_back(0U);
    QVERIFY(!VaultKeyringCodec::decode(trailing).has_value());

    const auto expect = [&](std::vector<unsigned char> malformed,
                            VaultKeyringErrorCode expected_code) {
        const auto decoded = VaultKeyringCodec::decode(malformed);
        QVERIFY(!decoded.has_value());
        QCOMPARE(decoded.error().code, expected_code);
    };

    auto bad_magic = frozen;
    bad_magic[0] ^= 0x01U;
    expect(std::move(bad_magic), VaultKeyringErrorCode::MalformedEncoding);
    auto bad_version = frozen;
    bad_version[5] = 2U;
    expect(std::move(bad_version), VaultKeyringErrorCode::UnsupportedFormat);
    auto bad_flags = frozen;
    bad_flags[6] = 1U;
    expect(std::move(bad_flags), VaultKeyringErrorCode::UnsupportedFormat);
    auto bad_reserved = frozen;
    bad_reserved[74] = 1U;
    expect(std::move(bad_reserved), VaultKeyringErrorCode::UnsupportedFormat);
    auto zero_count = frozen;
    zero_count[72] = 0U;
    zero_count[73] = 0U;
    expect(std::move(zero_count), VaultKeyringErrorCode::InvalidKeySlot);
    auto excessive_count = frozen;
    excessive_count[72] = 0U;
    excessive_count[73] = 33U;
    expect(std::move(excessive_count), VaultKeyringErrorCode::InvalidKeySlot);
    auto zero_vault = frozen;
    fillBytes(zero_vault, 8U, 16U, 0U);
    expect(std::move(zero_vault), VaultKeyringErrorCode::InvalidVaultIdentifier);
    auto zero_object_key = frozen;
    fillBytes(zero_object_key, 24U, 32U, 0U);
    expect(std::move(zero_object_key), VaultKeyringErrorCode::InvalidObjectIdKey);
    auto zero_slot_id = frozen;
    fillBytes(zero_slot_id, 76U, 16U, 0U);
    expect(std::move(zero_slot_id), VaultKeyringErrorCode::InvalidKeySlot);
    auto zero_dek = frozen;
    fillBytes(zero_dek, 92U, 32U, 0U);
    expect(std::move(zero_dek), VaultKeyringErrorCode::InvalidKeySlot);
    auto duplicate = frozen;
    copyBytes(duplicate, 76U, 124U, 16U);
    expect(std::move(duplicate), VaultKeyringErrorCode::DuplicateKeySlot);
    auto unknown_current = frozen;
    fillBytes(unknown_current, 56U, 16U, 0xa5U);
    expect(std::move(unknown_current), VaultKeyringErrorCode::UnknownCurrentSlot);
}

void VaultKeyringTest::rotationRetainsOldReadKeysAndChangesTheWriteSlot() {
    auto keyring = VaultKeyring::create();
    QVERIFY(keyring.has_value());
    auto old_encoding = VaultKeyringCodec::encode(*keyring);
    QVERIFY(old_encoding.has_value());
    auto old_keyring = VaultKeyringCodec::decode(old_encoding->bytes());
    QVERIFY(old_keyring.has_value());
    const auto old_slot = keyring->currentWriteSlotId();
    auto object_id_key = SecretBytes::copyOf(keyring->objectIdKey());
    QVERIFY(object_id_key.has_value());
    const QByteArray payload = QByteArrayLiteral("same immutable object across rotation");

    const auto before = encrypt(*keyring, payload);
    QVERIFY(before.has_value());
    QCOMPARE(before->metadata.key_slot_id, old_slot);

    const auto rotated = keyring->rotate();
    QVERIFY(rotated.has_value());
    QVERIFY(*rotated != old_slot);
    QCOMPARE(keyring->keySlotCount(), std::size_t{2});
    QCOMPARE(keyring->keySlotId(0), old_slot);
    QCOMPARE(keyring->currentWriteSlotId(), *rotated);
    QVERIFY(std::ranges::equal(keyring->objectIdKey(), object_id_key->bytes()));

    const auto after = encrypt(*keyring, payload);
    QVERIFY(after.has_value());
    QCOMPARE(after->metadata.key_slot_id, *rotated);
    QCOMPARE(after->metadata.identity, before->metadata.identity);
    QVERIFY(after->ciphertext != before->ciphertext);

    QTemporaryDir quarantine;
    QVERIFY(quarantine.isValid());
    auto old_decrypted = decrypt(*keyring, *before, quarantine.path());
    QVERIFY(old_decrypted.has_value());
    QCOMPARE(old_decrypted->quarantined_payload->readAll(), payload);
    auto new_decrypted = decrypt(*keyring, *after, quarantine.path());
    QVERIFY(new_decrypted.has_value());
    QCOMPARE(new_decrypted->quarantined_payload->readAll(), payload);

    const auto missing_new_slot = decrypt(*old_keyring, *after, quarantine.path());
    QVERIFY(!missing_new_slot.has_value());
    QCOMPARE(missing_new_slot.error().code, ProtocolErrorCode::UnknownKeySlot);
}

void VaultKeyringTest::enforcesTheRotationSlotBound() {
    auto keyring = VaultKeyring::create();
    QVERIFY(keyring.has_value());
    for (std::size_t index = 1; index < VaultKeyring::maximum_key_slots; ++index) {
        QVERIFY(keyring->rotate().has_value());
    }
    QCOMPARE(keyring->keySlotCount(), VaultKeyring::maximum_key_slots);
    const auto current = keyring->currentWriteSlotId();
    const auto rejected = keyring->rotate();
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, VaultKeyringErrorCode::KeySlotLimitReached);
    QCOMPARE(keyring->keySlotCount(), VaultKeyring::maximum_key_slots);
    QCOMPARE(keyring->currentWriteSlotId(), current);
}

void VaultKeyringTest::guardedSecretsAndProtocolCopiesAreMoveOnlyAndClearable() {
    const std::array<unsigned char, 4> marker{1U, 2U, 3U, 4U};
    auto first = SecretBytes::copyOf(marker);
    QVERIFY(first.has_value());
    SecretBytes second = std::move(*first);
    QVERIFY(first->empty());
    QVERIFY(std::ranges::equal(second.bytes(), marker));
    second.clear();
    QVERIFY(second.empty());

    auto keyring = VaultKeyring::create();
    QVERIFY(keyring.has_value());
    auto scoped = keyring->scopedProtocolKeys();
    QVERIFY(!scoped.empty());
    ScopedProtocolKeySet moved = std::move(scoped);
    QVERIFY(scoped.empty());
    QVERIFY(!moved.empty());
    moved.clear();
    QVERIFY(moved.empty());
    QVERIFY(allZero(moved.get().object_id_key));

    ProtocolKeySet raw_source;
    std::ranges::fill(raw_source.object_id_key, 0x35U);
    appellate::sync::ProtocolKeySlot raw_slot;
    std::ranges::fill(raw_slot.id, 0x46U);
    std::ranges::fill(raw_slot.encryption_key, 0x57U);
    raw_source.key_slots.push_back(raw_slot);
    sodium_memzero(raw_slot.encryption_key.data(), raw_slot.encryption_key.size());
    ScopedProtocolKeySet taken(std::move(raw_source));
    QVERIFY(allZero(raw_source.object_id_key));
    QVERIFY(raw_source.key_slots.empty());
    QVERIFY(!taken.empty());

    const auto active_slot = keyring->withProtocolKeys(
        [](const ProtocolKeySet& keys) { return keys.key_slots.at(keys.active_slot).id; });
    QCOMPARE(active_slot, keyring->currentWriteSlotId());

    VaultKeyring moved_keyring = std::move(*keyring);
    QVERIFY(keyring->objectIdKey().empty());
    QVERIFY(*keyring == *keyring);
    const auto moved_encode = VaultKeyringCodec::encode(*keyring);
    QVERIFY(!moved_encode.has_value());
    const auto moved_rotate = keyring->rotate();
    QVERIFY(!moved_rotate.has_value());
    QVERIFY(keyring->scopedProtocolKeys().empty());
    QVERIFY(VaultKeyringCodec::encode(moved_keyring).has_value());
}

} // namespace

QTEST_GUILESS_MAIN(VaultKeyringTest)

#include "tst_vault_keyring.moc"
