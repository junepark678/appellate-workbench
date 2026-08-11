#include "appellate/sync/protocol_codec.hpp"

#include <QBuffer>
#include <QByteArray>
#include <QDir>
#include <QTemporaryDir>
#include <QTest>

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <utility>

namespace {

using appellate::sync::EncryptedSyncObject;
using appellate::sync::ProtocolCodec;
using appellate::sync::ProtocolError;
using appellate::sync::ProtocolErrorCode;
using appellate::sync::ProtocolKeySet;
using appellate::sync::ProtocolKeySlot;
using appellate::sync::ProtocolLimits;
using appellate::sync::SyncObjectKind;
using appellate::sync::VerifiedSyncObject;

constexpr qsizetype outer_header_bytes = 8 + 16 + 24;

struct CipherFixture final {
    EncryptedSyncObject metadata;
    QByteArray bytes;
};

[[nodiscard]] ProtocolKeySet deterministicKeys(unsigned char offset = 0) {
    ProtocolKeySet keys;
    for (std::size_t index = 0; index < keys.object_id_key.size(); ++index) {
        keys.object_id_key[index] = static_cast<unsigned char>(index + offset);
    }
    ProtocolKeySlot slot;
    for (std::size_t index = 0; index < slot.id.size(); ++index) {
        slot.id[index] = static_cast<unsigned char>(0xa0U + index + offset);
    }
    for (std::size_t index = 0; index < slot.encryption_key.size(); ++index) {
        slot.encryption_key[index] = static_cast<unsigned char>(0x40U + index + offset);
    }
    keys.key_slots.push_back(slot);
    return keys;
}

[[nodiscard]] auto encryptFixture(QByteArray payload, const ProtocolKeySet& keys,
                                  SyncObjectKind kind = SyncObjectKind::PackRevision,
                                  std::uint16_t schema_version = 1, ProtocolLimits limits = {})
    -> std::expected<CipherFixture, ProtocolError> {
    QBuffer source(&payload);
    if (!source.open(QIODevice::ReadOnly)) {
        return std::unexpected(
            ProtocolError{ProtocolErrorCode::InvalidArgument, QStringLiteral("test source")});
    }
    QByteArray encrypted;
    QBuffer destination(&encrypted);
    if (!destination.open(QIODevice::WriteOnly)) {
        return std::unexpected(
            ProtocolError{ProtocolErrorCode::InvalidArgument, QStringLiteral("test destination")});
    }
    const auto result = ProtocolCodec::encrypt(kind, schema_version, source,
                                               static_cast<std::uint64_t>(payload.size()), keys,
                                               destination, limits);
    if (!result) {
        return std::unexpected(result.error());
    }
    return CipherFixture{*result, encrypted};
}

[[nodiscard]] auto decryptFixture(const CipherFixture& fixture, const ProtocolKeySet& keys,
                                  const QString& quarantine_directory, ProtocolLimits limits = {})
    -> std::expected<VerifiedSyncObject, ProtocolError> {
    QByteArray encrypted = fixture.bytes;
    QBuffer source(&encrypted);
    if (!source.open(QIODevice::ReadOnly)) {
        return std::unexpected(
            ProtocolError{ProtocolErrorCode::InvalidArgument, QStringLiteral("test ciphertext")});
    }
    return ProtocolCodec::decrypt(source, fixture.metadata.identity.remote_object_id, keys,
                                  quarantine_directory, limits);
}

[[nodiscard]] std::uint32_t bigEndian32(const QByteArray& bytes, qsizetype offset) {
    std::uint32_t value{};
    for (qsizetype index = 0; index < 4; ++index) {
        value = (value << 8U) |
                static_cast<std::uint32_t>(static_cast<unsigned char>(bytes.at(offset + index)));
    }
    return value;
}

void appendBigEndian16(QByteArray& bytes, std::uint16_t value) {
    bytes.append(static_cast<char>((value >> 8U) & 0xffU));
    bytes.append(static_cast<char>(value & 0xffU));
}

void appendBigEndian32(QByteArray& bytes, std::uint32_t value) {
    for (auto shift : {24U, 16U, 8U, 0U}) {
        bytes.append(static_cast<char>((value >> shift) & 0xffU));
    }
}

void appendBigEndian64(QByteArray& bytes, std::uint64_t value) {
    for (auto shift : {56U, 48U, 40U, 32U, 24U, 16U, 8U, 0U}) {
        bytes.append(static_cast<char>((value >> shift) & 0xffU));
    }
}

void replaceBigEndian32(QByteArray& bytes, qsizetype offset, std::uint32_t value) {
    QByteArray encoded;
    appendBigEndian32(encoded, value);
    bytes.replace(offset, encoded.size(), encoded);
}

[[nodiscard]] auto taggedFirstFrameFixture(const QByteArray& payload, unsigned char tag)
    -> std::expected<CipherFixture, ProtocolError> {
    const auto keys = deterministicKeys();
    const auto baseline = encryptFixture(payload, keys);
    if (!baseline) {
        return std::unexpected(baseline.error());
    }

    crypto_secretstream_xchacha20poly1305_state state{};
    std::array<unsigned char, crypto_secretstream_xchacha20poly1305_HEADERBYTES> stream_header{};
    if (crypto_secretstream_xchacha20poly1305_init_push(
            &state, stream_header.data(), keys.key_slots.front().encryption_key.data()) != 0) {
        return std::unexpected(ProtocolError{ProtocolErrorCode::CryptoInitializationFailed,
                                             QStringLiteral("test secretstream")});
    }

    QByteArray outer = QByteArray::fromHex("4157534f00010000");
    outer.append(reinterpret_cast<const char*>(keys.key_slots.front().id.data()),
                 static_cast<qsizetype>(keys.key_slots.front().id.size()));
    outer.append(reinterpret_cast<const char*>(stream_header.data()),
                 static_cast<qsizetype>(stream_header.size()));

    QByteArray plaintext = QByteArray::fromHex("41574f424a000100");
    plaintext.append(static_cast<char>(SyncObjectKind::PackRevision));
    appendBigEndian16(plaintext, 1);
    plaintext.append('\0');
    plaintext.append(QByteArray::fromHex(baseline->metadata.identity.canonical_id_hex.toLatin1()));
    appendBigEndian64(plaintext, static_cast<std::uint64_t>(payload.size()));
    plaintext.append(payload);
    plaintext.resize(static_cast<qsizetype>(baseline->metadata.padded_plaintext_bytes), '\x5a');
    if (plaintext.size() > static_cast<qsizetype>(ProtocolCodec::plaintext_chunk_bytes)) {
        plaintext.truncate(static_cast<qsizetype>(ProtocolCodec::plaintext_chunk_bytes));
    }

    QByteArray associated("appellate-workbench-sync-envelope-v1",
                          static_cast<qsizetype>(sizeof("appellate-workbench-sync-envelope-v1")));
    associated.append(outer);
    associated.append(QByteArray::fromHex(baseline->metadata.identity.remote_object_id.toLatin1()));
    appendBigEndian64(associated, 0);

    QByteArray encrypted(plaintext.size() + crypto_secretstream_xchacha20poly1305_ABYTES,
                         Qt::Uninitialized);
    unsigned long long encrypted_size{};
    if (crypto_secretstream_xchacha20poly1305_push(
            &state, reinterpret_cast<unsigned char*>(encrypted.data()), &encrypted_size,
            reinterpret_cast<const unsigned char*>(plaintext.constData()),
            static_cast<unsigned long long>(plaintext.size()),
            reinterpret_cast<const unsigned char*>(associated.constData()),
            static_cast<unsigned long long>(associated.size()), tag) != 0) {
        sodium_memzero(&state, sizeof(state));
        return std::unexpected(ProtocolError{ProtocolErrorCode::CryptoInitializationFailed,
                                             QStringLiteral("test tagged frame")});
    }
    sodium_memzero(&state, sizeof(state));
    QByteArray wire = outer;
    appendBigEndian32(wire, static_cast<std::uint32_t>(encrypted_size));
    wire.append(encrypted);
    auto fixture = *baseline;
    fixture.bytes = wire;
    fixture.metadata.ciphertext_bytes = static_cast<std::uint64_t>(wire.size());
    return fixture;
}

[[nodiscard]] qsizetype lastFrameOffset(const QByteArray& bytes) {
    auto offset = outer_header_bytes;
    auto last = offset;
    while (offset < bytes.size()) {
        if (bytes.size() - offset < 4) {
            return -1;
        }
        last = offset;
        const auto frame_size = bigEndian32(bytes, offset);
        offset += 4 + static_cast<qsizetype>(frame_size);
    }
    return offset == bytes.size() ? last : -1;
}

[[nodiscard]] QStringList quarantineFiles(const QString& directory) {
    return QDir(directory).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
}

class SyncProtocolCodecTest final : public QObject {
    Q_OBJECT

  private slots:
    void matchesFrozenCanonicalIdentityVector();
    void roundTripsEveryObjectKind_data();
    void roundTripsEveryObjectKind();
    void freezesPaddingBuckets();
    void randomizesCiphertextWithoutChangingIdentity();
    void rejectsWrongKeysSlotsAndRemoteIds();
    void rejectsBitFlipsTruncationAndTrailingBytes();
    void requiresFinalTag();
    void enforcesLimitsWithoutPublishingPlaintext();
    void removesQuarantineAfterLateAuthenticationFailure();
    void validatesArgumentsBeforeWriting();
    void requiresExactDeclaredSourceLength();
    void rejectsDuplicateKeySlots();
    void rejectsForbiddenAndEarlyStreamTags();
    void rejectsInvalidFrameLengths();
};

void SyncProtocolCodecTest::matchesFrozenCanonicalIdentityVector() {
    auto keys = deterministicKeys();
    const auto identity = ProtocolCodec::canonicalIdentity(
        SyncObjectKind::PackRevision, 1, QByteArrayView("abc", 3), keys.object_id_key);
    QVERIFY(identity.has_value());
    QCOMPARE(identity->canonical_id_hex,
             QStringLiteral("206e6f9ad620996c78eb3cc120de5477977d3ae0b097f4e2d1a9a447d5825173"));
    QCOMPARE(identity->remote_object_id,
             QStringLiteral("bc1fc006d361b6041c8161fa0ef04bdbc1199dadd93adac9698f2c28325c5527"));
}

void SyncProtocolCodecTest::roundTripsEveryObjectKind_data() {
    QTest::addColumn<int>("kind");
    QTest::newRow("pack revision") << static_cast<int>(SyncObjectKind::PackRevision);
    QTest::newRow("authored revision") << static_cast<int>(SyncObjectKind::AuthoredRevision);
    QTest::newRow("event segment") << static_cast<int>(SyncObjectKind::SessionEventSegment);
    QTest::newRow("checkpoint") << static_cast<int>(SyncObjectKind::Checkpoint);
}

void SyncProtocolCodecTest::roundTripsEveryObjectKind() {
    QFETCH(int, kind);
    const auto object_kind = static_cast<SyncObjectKind>(kind);
    const QByteArray payload = QByteArrayLiteral("exact immutable logical payload\0with binary") +
                               QByteArray::fromHex("00ff8042");
    const auto encrypted = encryptFixture(payload, deterministicKeys(), object_kind, 7);
    QVERIFY(encrypted.has_value());
    QCOMPARE(encrypted->metadata.kind, object_kind);
    QCOMPARE(encrypted->metadata.schema_version, std::uint16_t{7});
    QCOMPARE(encrypted->metadata.payload_bytes, static_cast<std::uint64_t>(payload.size()));
    QCOMPARE(encrypted->metadata.ciphertext_bytes,
             static_cast<std::uint64_t>(encrypted->bytes.size()));

    QTemporaryDir quarantine;
    QVERIFY(quarantine.isValid());
    auto decrypted = decryptFixture(*encrypted, deterministicKeys(), quarantine.path());
    QVERIFY(decrypted.has_value());
    QCOMPARE(decrypted->kind, object_kind);
    QCOMPARE(decrypted->schema_version, std::uint16_t{7});
    QCOMPARE(decrypted->identity, encrypted->metadata.identity);
    QCOMPARE(decrypted->quarantined_payload->readAll(), payload);
}

void SyncProtocolCodecTest::freezesPaddingBuckets() {
    struct Boundary final {
        std::uint64_t payload;
        std::uint64_t expected;
    };
    constexpr Boundary boundaries[]{
        {0, 4096},     {4044, 4096},           {4045, 8192},           {8140, 8192},
        {8141, 16384}, {1'048'524, 1'048'576}, {1'048'525, 2'097'152}, {2'097'101, 3'145'728},
    };
    for (const auto& boundary : boundaries) {
        const auto padded = ProtocolCodec::paddedPlaintextSize(boundary.payload);
        QVERIFY(padded.has_value());
        QCOMPARE(*padded, boundary.expected);
    }
}

void SyncProtocolCodecTest::randomizesCiphertextWithoutChangingIdentity() {
    const QByteArray payload(130'000, 'p');
    const auto first = encryptFixture(payload, deterministicKeys());
    const auto second = encryptFixture(payload, deterministicKeys());
    QVERIFY(first.has_value());
    QVERIFY(second.has_value());
    QCOMPARE(first->metadata.identity, second->metadata.identity);
    QVERIFY(first->bytes != second->bytes);
}

void SyncProtocolCodecTest::rejectsWrongKeysSlotsAndRemoteIds() {
    const auto encrypted = encryptFixture(QByteArrayLiteral("protected"), deterministicKeys());
    QVERIFY(encrypted.has_value());
    QTemporaryDir quarantine;
    QVERIFY(quarantine.isValid());

    auto wrong_encryption_key = deterministicKeys();
    wrong_encryption_key.key_slots.front().encryption_key.front() ^= 0x01U;
    const auto wrong_key = decryptFixture(*encrypted, wrong_encryption_key, quarantine.path());
    QVERIFY(!wrong_key.has_value());
    QCOMPARE(wrong_key.error().code, ProtocolErrorCode::AuthenticationFailed);

    auto unknown_slot_fixture = *encrypted;
    unknown_slot_fixture.bytes[8] =
        static_cast<char>(static_cast<unsigned char>(unknown_slot_fixture.bytes.at(8)) ^ 0x01U);
    const auto unknown_slot =
        decryptFixture(unknown_slot_fixture, deterministicKeys(), quarantine.path());
    QVERIFY(!unknown_slot.has_value());
    QCOMPARE(unknown_slot.error().code, ProtocolErrorCode::UnknownKeySlot);

    QByteArray path_changed = encrypted->bytes;
    QBuffer path_source(&path_changed);
    QVERIFY(path_source.open(QIODevice::ReadOnly));
    auto wrong_remote_id = encrypted->metadata.identity.remote_object_id;
    wrong_remote_id[0] = wrong_remote_id.at(0) == u'0' ? u'1' : u'0';
    const auto wrong_path = ProtocolCodec::decrypt(path_source, wrong_remote_id,
                                                   deterministicKeys(), quarantine.path());
    QVERIFY(!wrong_path.has_value());
    QCOMPARE(wrong_path.error().code, ProtocolErrorCode::AuthenticationFailed);

    auto wrong_id_key = deterministicKeys();
    wrong_id_key.object_id_key.front() ^= 0x01U;
    const auto wrong_identity_key = decryptFixture(*encrypted, wrong_id_key, quarantine.path());
    QVERIFY(!wrong_identity_key.has_value());
    QCOMPARE(wrong_identity_key.error().code, ProtocolErrorCode::IdentityMismatch);
    QVERIFY(quarantineFiles(quarantine.path()).isEmpty());
}

void SyncProtocolCodecTest::rejectsBitFlipsTruncationAndTrailingBytes() {
    const auto encrypted = encryptFixture(QByteArray(90'000, 'x'), deterministicKeys());
    QVERIFY(encrypted.has_value());
    QTemporaryDir quarantine;
    QVERIFY(quarantine.isValid());

    auto flipped = *encrypted;
    flipped.bytes[flipped.bytes.size() - 5] = static_cast<char>(
        static_cast<unsigned char>(flipped.bytes.at(flipped.bytes.size() - 5)) ^ 0x80U);
    const auto corrupt = decryptFixture(flipped, deterministicKeys(), quarantine.path());
    QVERIFY(!corrupt.has_value());
    QCOMPARE(corrupt.error().code, ProtocolErrorCode::AuthenticationFailed);

    for (const auto length :
         {outer_header_bytes - 1, outer_header_bytes + 2, encrypted->bytes.size() - 1}) {
        auto truncated = *encrypted;
        truncated.bytes.truncate(length);
        const auto result = decryptFixture(truncated, deterministicKeys(), quarantine.path());
        QVERIFY(!result.has_value());
        QVERIFY(result.error().code == ProtocolErrorCode::MalformedEnvelope ||
                result.error().code == ProtocolErrorCode::MissingFinalTag);
    }

    auto trailing = *encrypted;
    trailing.bytes.append('\0');
    const auto extra = decryptFixture(trailing, deterministicKeys(), quarantine.path());
    QVERIFY(!extra.has_value());
    QCOMPARE(extra.error().code, ProtocolErrorCode::TrailingCiphertext);
    QVERIFY(quarantineFiles(quarantine.path()).isEmpty());
}

void SyncProtocolCodecTest::requiresFinalTag() {
    const auto encrypted = encryptFixture(QByteArray(90'000, 'f'), deterministicKeys());
    QVERIFY(encrypted.has_value());
    auto no_final = *encrypted;
    const auto final_offset = lastFrameOffset(no_final.bytes);
    QVERIFY(final_offset > outer_header_bytes);
    no_final.bytes.truncate(final_offset);

    QTemporaryDir quarantine;
    QVERIFY(quarantine.isValid());
    const auto result = decryptFixture(no_final, deterministicKeys(), quarantine.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, ProtocolErrorCode::MissingFinalTag);
    QVERIFY(quarantineFiles(quarantine.path()).isEmpty());
}

void SyncProtocolCodecTest::enforcesLimitsWithoutPublishingPlaintext() {
    const QByteArray payload(12'000, 'l');
    const auto encrypted = encryptFixture(payload, deterministicKeys());
    QVERIFY(encrypted.has_value());
    QTemporaryDir quarantine;
    QVERIFY(quarantine.isValid());

    const auto rejected =
        decryptFixture(*encrypted, deterministicKeys(), quarantine.path(), ProtocolLimits{100});
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, ProtocolErrorCode::ObjectTooLarge);
    QVERIFY(quarantineFiles(quarantine.path()).isEmpty());

    const auto encryption_rejected = encryptFixture(
        payload, deterministicKeys(), SyncObjectKind::PackRevision, 1, ProtocolLimits{100});
    QVERIFY(!encryption_rejected.has_value());
    QCOMPARE(encryption_rejected.error().code, ProtocolErrorCode::ObjectTooLarge);
}

void SyncProtocolCodecTest::removesQuarantineAfterLateAuthenticationFailure() {
    const auto encrypted = encryptFixture(QByteArray(180'000, 'q'), deterministicKeys());
    QVERIFY(encrypted.has_value());
    auto corrupt_late = *encrypted;
    const auto final_offset = lastFrameOffset(corrupt_late.bytes);
    QVERIFY(final_offset > outer_header_bytes);
    corrupt_late.bytes[final_offset + 8] = static_cast<char>(
        static_cast<unsigned char>(corrupt_late.bytes.at(final_offset + 8)) ^ 0x08U);

    QTemporaryDir quarantine;
    QVERIFY(quarantine.isValid());
    const auto rejected = decryptFixture(corrupt_late, deterministicKeys(), quarantine.path());
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, ProtocolErrorCode::AuthenticationFailed);
    QVERIFY(quarantineFiles(quarantine.path()).isEmpty());
}

void SyncProtocolCodecTest::validatesArgumentsBeforeWriting() {
    QByteArray payload = QByteArrayLiteral("payload");
    QBuffer source(&payload);
    QVERIFY(source.open(QIODevice::ReadOnly));
    QByteArray output;
    QBuffer destination(&output);
    QVERIFY(destination.open(QIODevice::WriteOnly));

    auto no_slots = deterministicKeys();
    no_slots.key_slots.clear();
    const auto rejected =
        ProtocolCodec::encrypt(SyncObjectKind::PackRevision, 1, source,
                               static_cast<std::uint64_t>(payload.size()), no_slots, destination);
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, ProtocolErrorCode::InvalidArgument);
    QVERIFY(output.isEmpty());

    const auto invalid_identity = ProtocolCodec::canonicalIdentity(
        static_cast<SyncObjectKind>(99), 1, payload, deterministicKeys().object_id_key);
    QVERIFY(!invalid_identity.has_value());
    QCOMPARE(invalid_identity.error().code, ProtocolErrorCode::InvalidArgument);
}

void SyncProtocolCodecTest::requiresExactDeclaredSourceLength() {
    const auto keys = deterministicKeys();
    for (const auto declared_size : {std::uint64_t{2}, std::uint64_t{4}}) {
        QByteArray payload = QByteArrayLiteral("abc");
        QBuffer source(&payload);
        QVERIFY(source.open(QIODevice::ReadOnly));
        QByteArray output;
        QBuffer destination(&output);
        QVERIFY(destination.open(QIODevice::WriteOnly));
        const auto result = ProtocolCodec::encrypt(SyncObjectKind::PackRevision, 1, source,
                                                   declared_size, keys, destination);
        QVERIFY(!result.has_value());
        QCOMPARE(result.error().code, declared_size < 3 ? ProtocolErrorCode::InvalidArgument
                                                        : ProtocolErrorCode::SourceReadFailed);
        QVERIFY(output.isEmpty());
    }
}

void SyncProtocolCodecTest::rejectsDuplicateKeySlots() {
    auto keys = deterministicKeys();
    keys.key_slots.push_back(keys.key_slots.front());
    QByteArray payload = QByteArrayLiteral("payload");
    QBuffer source(&payload);
    QVERIFY(source.open(QIODevice::ReadOnly));
    QByteArray output;
    QBuffer destination(&output);
    QVERIFY(destination.open(QIODevice::WriteOnly));
    const auto encrypted =
        ProtocolCodec::encrypt(SyncObjectKind::PackRevision, 1, source,
                               static_cast<std::uint64_t>(payload.size()), keys, destination);
    QVERIFY(!encrypted.has_value());
    QCOMPARE(encrypted.error().code, ProtocolErrorCode::InvalidArgument);
    QVERIFY(output.isEmpty());
}

void SyncProtocolCodecTest::rejectsForbiddenAndEarlyStreamTags() {
    const auto forbidden = taggedFirstFrameFixture(QByteArrayLiteral("payload"),
                                                   crypto_secretstream_xchacha20poly1305_TAG_PUSH);
    QVERIFY(forbidden.has_value());
    const auto early = taggedFirstFrameFixture(QByteArray(90'000, 'e'),
                                               crypto_secretstream_xchacha20poly1305_TAG_FINAL);
    QVERIFY(early.has_value());
    QTemporaryDir quarantine;
    QVERIFY(quarantine.isValid());

    const auto forbidden_result =
        decryptFixture(*forbidden, deterministicKeys(), quarantine.path());
    QVERIFY(!forbidden_result.has_value());
    QCOMPARE(forbidden_result.error().code, ProtocolErrorCode::MalformedEnvelope);
    const auto early_result = decryptFixture(*early, deterministicKeys(), quarantine.path());
    QVERIFY(!early_result.has_value());
    QCOMPARE(early_result.error().code, ProtocolErrorCode::MalformedEnvelope);
    QVERIFY(quarantineFiles(quarantine.path()).isEmpty());
}

void SyncProtocolCodecTest::rejectsInvalidFrameLengths() {
    const auto encrypted = encryptFixture(QByteArrayLiteral("payload"), deterministicKeys());
    QVERIFY(encrypted.has_value());
    QTemporaryDir quarantine;
    QVERIFY(quarantine.isValid());
    for (const auto invalid_size : {
             std::uint32_t{crypto_secretstream_xchacha20poly1305_ABYTES - 1U},
             static_cast<std::uint32_t>(ProtocolCodec::plaintext_chunk_bytes +
                                        crypto_secretstream_xchacha20poly1305_ABYTES + 1ULL),
         }) {
        auto malformed = *encrypted;
        replaceBigEndian32(malformed.bytes, outer_header_bytes, invalid_size);
        const auto result = decryptFixture(malformed, deterministicKeys(), quarantine.path());
        QVERIFY(!result.has_value());
        QCOMPARE(result.error().code, ProtocolErrorCode::MalformedEnvelope);
    }
    QVERIFY(quarantineFiles(quarantine.path()).isEmpty());
}

} // namespace

QTEST_GUILESS_MAIN(SyncProtocolCodecTest)

#include "tst_protocol_codec.moc"
