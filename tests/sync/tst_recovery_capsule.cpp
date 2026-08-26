#include "appellate/sync/recovery_capsule.hpp"
#include "appellate/sync/secret_bytes.hpp"
#include "appellate/sync/secret_store.hpp"
#include "appellate/sync/vault_keyring.hpp"

#include <QByteArray>
#include <QMessageLogContext>
#include <QStringList>
#include <QTest>

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace {

using appellate::sync::RecoveryCapsuleCodec;
using appellate::sync::RecoveryCapsuleErrorCode;
using appellate::sync::RecoveryCapsuleLimits;
using appellate::sync::RecoveryKdfParameters;
using appellate::sync::SecretBytes;
using appellate::sync::SecretMemoryError;
using appellate::sync::SecretStore;
using appellate::sync::SecretStoreError;
using appellate::sync::SecretStoreErrorCode;
using appellate::sync::VaultKeyring;
using appellate::sync::VaultKeyringCodec;
using appellate::sync::VaultKeyringStore;
using appellate::sync::VaultKeyringStoreErrorCode;

constexpr char fixed_password_text[] = "correct horse battery staple";

[[nodiscard]] std::span<const unsigned char> fixedPassword() {
    return {reinterpret_cast<const unsigned char*>(fixed_password_text),
            sizeof(fixed_password_text) - 1U};
}

[[nodiscard]] std::vector<unsigned char> frozenKeyringBytes() {
    const auto bytes =
        QByteArray::fromHex("41574b5200010000"
                            "101112131415161718191a1b1c1d1e1f"
                            "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"
                            "707172737475767778797a7b7c7d7e7f00020000"
                            "404142434445464748494a4b4c4d4e4f"
                            "505152535455565758595a5b5c5d5e5f606162636465666768696a6b6c6d6e6f"
                            "707172737475767778797a7b7c7d7e7f"
                            "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
    const auto* first = reinterpret_cast<const unsigned char*>(bytes.constData());
    return {first, first + bytes.size()};
}

[[nodiscard]] std::vector<unsigned char> frozenRecoveryCapsule() {
    const auto bytes =
        QByteArray::fromHex("41575243000100000001000100000000000000010000000000002000"
                            "000102030405060708090a0b0c0d0e0f"
                            "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7"
                            "000000bc"
                            "5923b3c5f2425fe460375fc1a7a6729b52c30cbabe4b129cb314f54a51c0590"
                            "477c1f92ffab63797dcef990e1eeae9a546adf70d770c7496b5d981dfc804181"
                            "4ea2f824a2b4a345abed162a5c11ffde67e17273e8192534a5f20b5422d25e4"
                            "43411bfb928625460750807a685a5eaf638a0566d6a26df5babe4f4543fafbf2"
                            "b4737cd488b26bc47ebea8a5ff3c71dfda3eab577d56525419e8bcd173c75e1"
                            "f6919c563073516c968129757ec18b947468390f512ef8452caef87bb44");
    const auto* first = reinterpret_cast<const unsigned char*>(bytes.constData());
    return {first, first + bytes.size()};
}

void writeBigEndian32(std::span<unsigned char> destination, std::uint32_t value) {
    for (std::size_t index = 0; index < 4U; ++index) {
        const auto shift = static_cast<unsigned int>((3U - index) * 8U);
        destination[index] = static_cast<unsigned char>((value >> shift) & 0xffU);
    }
}

void writeBigEndian64(std::span<unsigned char> destination, std::uint64_t value) {
    for (std::size_t index = 0; index < 8U; ++index) {
        const auto shift = static_cast<unsigned int>((7U - index) * 8U);
        destination[index] = static_cast<unsigned char>((value >> shift) & 0xffU);
    }
}

[[nodiscard]] std::uint64_t readBigEndian64(std::span<const unsigned char> source) {
    std::uint64_t value{};
    for (const auto byte : source.first(8U)) {
        value = (value << 8U) | static_cast<std::uint64_t>(byte);
    }
    return value;
}

void duplicateFirstSlotId(std::vector<unsigned char>& encoded_keyring) {
    for (std::size_t index = 0; index < 16U; ++index) {
        encoded_keyring.at(124U + index) = encoded_keyring.at(76U + index);
    }
}

// Independent test-only construction used to verify the frozen capsule and to create an
// authenticated capsule whose plaintext keyring is intentionally malformed.
[[nodiscard]] std::vector<unsigned char> referenceCapsule(
    std::span<const unsigned char> plaintext, std::span<const unsigned char> password,
    const std::array<unsigned char, crypto_pwhash_SALTBYTES>& salt,
    const std::array<unsigned char, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES>& nonce) {
    constexpr char domain[] = "appellate-workbench-sync-recovery-v1";
    constexpr std::size_t header_bytes = RecoveryCapsuleCodec::fixed_header_bytes;
    const auto ciphertext_bytes = plaintext.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES;
    std::vector<unsigned char> capsule(header_bytes + ciphertext_bytes);
    const std::array<unsigned char, 8> magic{'A', 'W', 'R', 'C', 0, 1, 0, 0};
    std::copy(magic.begin(), magic.end(), capsule.begin());
    capsule[8] = 0U;
    capsule[9] = 1U;
    capsule[10] = 0U;
    capsule[11] = 1U;
    writeBigEndian64(std::span<unsigned char>{capsule}.subspan(12U, 8U), 1U);
    writeBigEndian64(std::span<unsigned char>{capsule}.subspan(20U, 8U), 8192U);
    std::copy(salt.begin(), salt.end(), capsule.begin() + 28);
    std::copy(nonce.begin(), nonce.end(), capsule.begin() + 44);
    writeBigEndian32(std::span<unsigned char>{capsule}.subspan(68U, 4U),
                     static_cast<std::uint32_t>(ciphertext_bytes));

    std::vector<unsigned char> associated(sizeof(domain) + header_bytes);
    std::memcpy(associated.data(), domain, sizeof(domain));
    std::memcpy(associated.data() + sizeof(domain), capsule.data(), header_bytes);
    std::array<unsigned char, crypto_aead_xchacha20poly1305_ietf_KEYBYTES> key{};
    const auto derived =
        crypto_pwhash(key.data(), key.size(), reinterpret_cast<const char*>(password.data()),
                      static_cast<unsigned long long>(password.size()), salt.data(), 1U, 8192U,
                      crypto_pwhash_ALG_ARGON2ID13);
    if (derived != 0) {
        qFatal("Cannot derive the independent recovery test key");
    }
    unsigned long long written{};
    const auto encrypted = crypto_aead_xchacha20poly1305_ietf_encrypt(
        capsule.data() + header_bytes, &written, plaintext.data(),
        static_cast<unsigned long long>(plaintext.size()), associated.data(),
        static_cast<unsigned long long>(associated.size()), nullptr, nonce.data(), key.data());
    sodium_memzero(key.data(), key.size());
    if (encrypted != 0 || written != ciphertext_bytes) {
        qFatal("Cannot create the independent recovery test vector");
    }
    return capsule;
}

[[nodiscard]] RecoveryKdfParameters fastParameters() { return {1U, 8192U}; }

[[nodiscard]] auto memoryError() -> SecretStoreError {
    return {SecretStoreErrorCode::ReadFailed,
            QStringLiteral("The in-memory test store cannot allocate guarded bytes")};
}

class InMemorySecretStore final : public SecretStore {
  public:
    [[nodiscard]] auto read(QStringView opaque_reference)
        -> std::expected<SecretBytes, SecretStoreError> override {
        ++read_count;
        if (next_read_error) {
            auto error = std::move(*next_read_error);
            next_read_error.reset();
            return std::unexpected(std::move(error));
        }
        const auto found = secrets_.find(opaque_reference.toString());
        if (found == secrets_.end()) {
            return std::unexpected(SecretStoreError{SecretStoreErrorCode::NotFound,
                                                    QStringLiteral("test secret missing")});
        }
        auto copy = SecretBytes::copyOf(found->second.bytes());
        if (!copy) {
            return std::unexpected(memoryError());
        }
        return std::move(*copy);
    }

    [[nodiscard]] auto write(QStringView opaque_reference, std::span<const unsigned char> secret)
        -> std::expected<void, SecretStoreError> override {
        ++write_count;
        if (next_write_error) {
            auto error = std::move(*next_write_error);
            next_write_error.reset();
            return std::unexpected(std::move(error));
        }
        auto copy = SecretBytes::copyOf(secret);
        if (!copy) {
            return std::unexpected(
                SecretStoreError{SecretStoreErrorCode::WriteFailed, memoryError().message});
        }
        secrets_.insert_or_assign(opaque_reference.toString(), std::move(*copy));
        return {};
    }

    void putRaw(const QString& opaque_reference, std::span<const unsigned char> bytes) {
        auto copy = SecretBytes::copyOf(bytes);
        if (!copy) {
            qFatal("Cannot allocate in-memory fake secret bytes");
        }
        secrets_.insert_or_assign(opaque_reference, std::move(*copy));
    }

    [[nodiscard]] auto peek(const QString& opaque_reference) const
        -> std::span<const unsigned char> {
        const auto found = secrets_.find(opaque_reference);
        return found == secrets_.end() ? std::span<const unsigned char>{} : found->second.bytes();
    }

    std::size_t read_count{};
    std::size_t write_count{};
    std::optional<SecretStoreError> next_read_error;
    std::optional<SecretStoreError> next_write_error;

  private:
    std::map<QString, SecretBytes> secrets_;
};

QStringList* captured_messages{};

void captureMessage(QtMsgType, const QMessageLogContext&, const QString& message) {
    if (captured_messages != nullptr) {
        captured_messages->push_back(message);
    }
}

class MessageCapture final {
  public:
    explicit MessageCapture(QStringList& messages)
        : previous_(qInstallMessageHandler(captureMessage)) {
        captured_messages = &messages;
    }
    MessageCapture(const MessageCapture&) = delete;
    MessageCapture& operator=(const MessageCapture&) = delete;
    ~MessageCapture() {
        captured_messages = nullptr;
        qInstallMessageHandler(previous_);
    }

  private:
    QtMessageHandler previous_{};
};

class RecoveryCapsuleTest final : public QObject {
    Q_OBJECT

  private slots:
    void matchesIndependentFrozenVector();
    void usesAndPersistsTheDefaultKdfParameters();
    void randomizedExportImportsExactly();
    void rejectsWrongPasswordTamperingTruncationAndTrailingBytes();
    void enforcesPasswordKdfAndCapsuleBoundsBeforeWork();
    void rejectsAuthenticatedButInvalidKeyring();
    void secretStoreRoundTripsAndFailsClosedWithoutAnAdapter();
    void recoveryRestoreDoesNotMutateTheStoreBeforeValidation();
    void diagnosticsAndLogsContainNoSecretMaterial();
};

void RecoveryCapsuleTest::matchesIndependentFrozenVector() {
    std::array<unsigned char, crypto_pwhash_SALTBYTES> salt{};
    for (std::size_t index = 0; index < salt.size(); ++index) {
        salt[index] = static_cast<unsigned char>(index);
    }
    std::array<unsigned char, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES> nonce{};
    for (std::size_t index = 0; index < nonce.size(); ++index) {
        nonce[index] = static_cast<unsigned char>(0xa0U + index);
    }
    const auto reference = referenceCapsule(frozenKeyringBytes(), fixedPassword(), salt, nonce);
    const auto frozen = frozenRecoveryCapsule();
    QCOMPARE(frozen.size(), std::size_t{260});
    QVERIFY(std::ranges::equal(reference, frozen));

    auto imported = RecoveryCapsuleCodec::importCapsule(frozen, fixedPassword());
    QVERIFY(imported.has_value());
    auto encoded = VaultKeyringCodec::encode(*imported);
    QVERIFY(encoded.has_value());
    QVERIFY(std::ranges::equal(encoded->bytes(), frozenKeyringBytes()));
}

void RecoveryCapsuleTest::usesAndPersistsTheDefaultKdfParameters() {
    auto keyring = VaultKeyring::create();
    QVERIFY(keyring.has_value());
    const auto capsule = RecoveryCapsuleCodec::exportCapsule(*keyring, fixedPassword());
    QVERIFY(capsule.has_value());
    QCOMPARE(readBigEndian64(std::span<const unsigned char>{*capsule}.subspan(12U, 8U)),
             RecoveryKdfParameters::interactive_operations_limit);
    QCOMPARE(readBigEndian64(std::span<const unsigned char>{*capsule}.subspan(20U, 8U)),
             RecoveryKdfParameters::interactive_memory_limit);
    auto imported = RecoveryCapsuleCodec::importCapsule(*capsule, fixedPassword());
    QVERIFY(imported.has_value());
    QVERIFY(*imported == *keyring);
}

void RecoveryCapsuleTest::randomizedExportImportsExactly() {
    auto keyring = VaultKeyring::create();
    QVERIFY(keyring.has_value());
    QVERIFY(keyring->rotate().has_value());
    const auto first =
        RecoveryCapsuleCodec::exportCapsule(*keyring, fixedPassword(), fastParameters());
    const auto second =
        RecoveryCapsuleCodec::exportCapsule(*keyring, fixedPassword(), fastParameters());
    QVERIFY(first.has_value());
    QVERIFY(second.has_value());
    QVERIFY(*first != *second);
    QCOMPARE(first->size(), second->size());
    QCOMPARE(readBigEndian64(std::span<const unsigned char>{*first}.subspan(12U, 8U)),
             std::uint64_t{1});
    QCOMPARE(readBigEndian64(std::span<const unsigned char>{*first}.subspan(20U, 8U)),
             std::uint64_t{8192});
    QVERIFY(!std::ranges::equal(std::span<const unsigned char>{*first}.subspan(28U, 16U),
                                std::span<const unsigned char>{*second}.subspan(28U, 16U)) ||
            !std::ranges::equal(std::span<const unsigned char>{*first}.subspan(44U, 24U),
                                std::span<const unsigned char>{*second}.subspan(44U, 24U)));

    auto imported = RecoveryCapsuleCodec::importCapsule(*first, fixedPassword());
    QVERIFY(imported.has_value());
    QVERIFY(*imported == *keyring);
}

void RecoveryCapsuleTest::rejectsWrongPasswordTamperingTruncationAndTrailingBytes() {
    const auto capsule = frozenRecoveryCapsule();
    constexpr char wrong_text[] = "wrong password";
    const std::span<const unsigned char> wrong{reinterpret_cast<const unsigned char*>(wrong_text),
                                               sizeof(wrong_text) - 1U};
    const auto wrong_password = RecoveryCapsuleCodec::importCapsule(capsule, wrong);
    QVERIFY(!wrong_password.has_value());
    QCOMPARE(wrong_password.error().code, RecoveryCapsuleErrorCode::AuthenticationFailed);

    for (std::size_t length = 0; length < capsule.size(); ++length) {
        const auto truncated = RecoveryCapsuleCodec::importCapsule(
            std::span<const unsigned char>{capsule.data(), length}, fixedPassword());
        QVERIFY2(!truncated.has_value(), "a truncated recovery capsule was accepted");
    }
    auto trailing = capsule;
    trailing.push_back(0U);
    const auto trailing_result = RecoveryCapsuleCodec::importCapsule(trailing, fixedPassword());
    QVERIFY(!trailing_result.has_value());
    QCOMPARE(trailing_result.error().code, RecoveryCapsuleErrorCode::MalformedCapsule);

    const auto expect = [&](std::vector<unsigned char> modified,
                            RecoveryCapsuleErrorCode expected_code) {
        const auto result = RecoveryCapsuleCodec::importCapsule(modified, fixedPassword());
        QVERIFY(!result.has_value());
        QCOMPARE(result.error().code, expected_code);
    };
    auto magic = capsule;
    magic[0] ^= 0x01U;
    expect(std::move(magic), RecoveryCapsuleErrorCode::MalformedCapsule);
    auto version = capsule;
    version[5] = 2U;
    expect(std::move(version), RecoveryCapsuleErrorCode::UnsupportedFormat);
    auto kdf = capsule;
    kdf[9] = 2U;
    expect(std::move(kdf), RecoveryCapsuleErrorCode::UnsupportedFormat);
    auto aead = capsule;
    aead[11] = 2U;
    expect(std::move(aead), RecoveryCapsuleErrorCode::UnsupportedFormat);
    auto authenticated_ops = capsule;
    authenticated_ops[19] = 2U;
    expect(std::move(authenticated_ops), RecoveryCapsuleErrorCode::AuthenticationFailed);
    for (const auto index : {std::size_t{27}, std::size_t{28}, std::size_t{44}, std::size_t{72},
                             capsule.size() - 1U}) {
        auto tampered = capsule;
        tampered[index] ^= 0x01U;
        expect(std::move(tampered), RecoveryCapsuleErrorCode::AuthenticationFailed);
    }
    auto wrong_length = capsule;
    writeBigEndian32(std::span<unsigned char>{wrong_length}.subspan(68U, 4U), 187U);
    expect(std::move(wrong_length), RecoveryCapsuleErrorCode::MalformedCapsule);
}

void RecoveryCapsuleTest::enforcesPasswordKdfAndCapsuleBoundsBeforeWork() {
    auto keyring = VaultKeyring::create();
    QVERIFY(keyring.has_value());
    const auto frozen = frozenRecoveryCapsule();

    QVERIFY(!RecoveryCapsuleCodec::exportCapsule(*keyring, {}, fastParameters()).has_value());
    auto long_password = SecretBytes::allocate(1025U);
    QVERIFY(long_password.has_value());
    std::ranges::fill(long_password->mutableBytes(), static_cast<unsigned char>('p'));
    const auto too_long = RecoveryCapsuleCodec::importCapsule(frozen, long_password->bytes());
    QVERIFY(!too_long.has_value());
    QCOMPARE(too_long.error().code, RecoveryCapsuleErrorCode::PasswordOutOfBounds);

    for (const auto parameters :
         {RecoveryKdfParameters{0U, 8192U}, RecoveryKdfParameters{5U, 8192U},
          RecoveryKdfParameters{1U, 8191U},
          RecoveryKdfParameters{1U, 64ULL * 1024ULL * 1024ULL + 1ULL}}) {
        const auto result =
            RecoveryCapsuleCodec::exportCapsule(*keyring, fixedPassword(), parameters);
        QVERIFY(!result.has_value());
        QCOMPARE(result.error().code, RecoveryCapsuleErrorCode::ParametersOutOfBounds);
    }

    RecoveryCapsuleLimits small_capsule;
    small_capsule.maximum_capsule_bytes = 100U;
    const auto too_large = RecoveryCapsuleCodec::exportCapsule(*keyring, fixedPassword(),
                                                               fastParameters(), small_capsule);
    QVERIFY(!too_large.has_value());
    QCOMPARE(too_large.error().code, RecoveryCapsuleErrorCode::CapsuleTooLarge);

    RecoveryCapsuleLimits invalid_limits;
    invalid_limits.maximum_password_bytes = 0U;
    const auto invalid =
        RecoveryCapsuleCodec::importCapsule(frozen, fixedPassword(), invalid_limits);
    QVERIFY(!invalid.has_value());
    QCOMPARE(invalid.error().code, RecoveryCapsuleErrorCode::InvalidArgument);

    auto excessive_ops = frozen;
    writeBigEndian64(std::span<unsigned char>{excessive_ops}.subspan(12U, 8U), 5U);
    const auto rejected_ops = RecoveryCapsuleCodec::importCapsule(excessive_ops, fixedPassword());
    QVERIFY(!rejected_ops.has_value());
    QCOMPARE(rejected_ops.error().code, RecoveryCapsuleErrorCode::ParametersOutOfBounds);
    auto gigabyte_claim = frozen;
    writeBigEndian64(std::span<unsigned char>{gigabyte_claim}.subspan(20U, 8U),
                     1024ULL * 1024ULL * 1024ULL);
    const auto rejected_memory =
        RecoveryCapsuleCodec::importCapsule(gigabyte_claim, fixedPassword());
    QVERIFY(!rejected_memory.has_value());
    QCOMPARE(rejected_memory.error().code, RecoveryCapsuleErrorCode::ParametersOutOfBounds);

    RecoveryCapsuleLimits undersized_input;
    undersized_input.maximum_capsule_bytes = frozen.size() - 1U;
    const auto bounded =
        RecoveryCapsuleCodec::importCapsule(frozen, fixedPassword(), undersized_input);
    QVERIFY(!bounded.has_value());
    QCOMPARE(bounded.error().code, RecoveryCapsuleErrorCode::CapsuleTooLarge);
}

void RecoveryCapsuleTest::rejectsAuthenticatedButInvalidKeyring() {
    auto malformed_plaintext = frozenKeyringBytes();
    duplicateFirstSlotId(malformed_plaintext);
    std::array<unsigned char, crypto_pwhash_SALTBYTES> salt{};
    std::array<unsigned char, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES> nonce{};
    std::ranges::fill(salt, 0x31U);
    std::ranges::fill(nonce, 0x62U);
    const auto capsule = referenceCapsule(malformed_plaintext, fixedPassword(), salt, nonce);
    const auto rejected = RecoveryCapsuleCodec::importCapsule(capsule, fixedPassword());
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, RecoveryCapsuleErrorCode::InvalidKeyring);
}

void RecoveryCapsuleTest::secretStoreRoundTripsAndFailsClosedWithoutAnAdapter() {
    auto keyring = VaultKeyring::create();
    QVERIFY(keyring.has_value());
    const QString reference = QStringLiteral("vault:keyring:test-01");

    InMemorySecretStore fake;
    VaultKeyringStore store(&fake);
    QVERIFY(store.save(reference, *keyring).has_value());
    QCOMPARE(fake.write_count, std::size_t{1});
    auto loaded = store.load(reference);
    QVERIFY(loaded.has_value());
    QVERIFY(*loaded == *keyring);
    QCOMPARE(fake.read_count, std::size_t{1});

    const auto missing = store.load(QStringLiteral("vault:missing"));
    QVERIFY(!missing.has_value());
    QCOMPARE(missing.error().code, VaultKeyringStoreErrorCode::SecretNotFound);
    const auto invalid_reference = store.save(QStringLiteral("contains spaces"), *keyring);
    QVERIFY(!invalid_reference.has_value());
    QCOMPARE(invalid_reference.error().code, VaultKeyringStoreErrorCode::InvalidReference);
    for (const auto& invalid_value :
         {QStringLiteral("/leading"), QStringLiteral("vault/path"), QStringLiteral("vault..escape"),
          QStringLiteral("vault::escape"), QStringLiteral("vault_-escape"),
          QStringLiteral("-leading")}) {
        const auto rejected = store.save(invalid_value, *keyring);
        QVERIFY(!rejected.has_value());
        QCOMPARE(rejected.error().code, VaultKeyringStoreErrorCode::InvalidReference);
    }

    const std::array<unsigned char, 3> malformed{1U, 2U, 3U};
    fake.putRaw(QStringLiteral("vault:corrupt"), malformed);
    const auto corrupt = store.load(QStringLiteral("vault:corrupt"));
    QVERIFY(!corrupt.has_value());
    QCOMPARE(corrupt.error().code, VaultKeyringStoreErrorCode::InvalidStoredKeyring);

    VaultKeyringStore unavailable;
    const auto closed_save = unavailable.save(reference, *keyring);
    QVERIFY(!closed_save.has_value());
    QCOMPARE(closed_save.error().code, VaultKeyringStoreErrorCode::SecretStoreUnavailable);
    const auto closed_load = unavailable.load(reference);
    QVERIFY(!closed_load.has_value());
    QCOMPARE(closed_load.error().code, VaultKeyringStoreErrorCode::SecretStoreUnavailable);
    const auto closed_restore = unavailable.restoreFromRecovery(reference, {}, {});
    QVERIFY(!closed_restore.has_value());
    QCOMPARE(closed_restore.error().code, VaultKeyringStoreErrorCode::SecretStoreUnavailable);
}

void RecoveryCapsuleTest::recoveryRestoreDoesNotMutateTheStoreBeforeValidation() {
    auto original = VaultKeyring::create();
    auto replacement = VaultKeyring::create();
    QVERIFY(original.has_value());
    QVERIFY(replacement.has_value());
    const auto capsule =
        RecoveryCapsuleCodec::exportCapsule(*replacement, fixedPassword(), fastParameters());
    QVERIFY(capsule.has_value());
    const QString reference = QStringLiteral("vault:recovery-preserve");
    InMemorySecretStore fake;
    VaultKeyringStore store(&fake);
    QVERIFY(store.save(reference, *original).has_value());
    const auto baseline_writes = fake.write_count;
    auto baseline = SecretBytes::copyOf(fake.peek(reference));
    QVERIFY(baseline.has_value());

    constexpr char wrong_text[] = "definitely wrong";
    const std::span<const unsigned char> wrong{reinterpret_cast<const unsigned char*>(wrong_text),
                                               sizeof(wrong_text) - 1U};
    const auto wrong_password = store.restoreFromRecovery(reference, *capsule, wrong);
    QVERIFY(!wrong_password.has_value());
    QCOMPARE(wrong_password.error().code, VaultKeyringStoreErrorCode::RecoveryRejected);
    QCOMPARE(fake.write_count, baseline_writes);
    QVERIFY(std::ranges::equal(fake.peek(reference), baseline->bytes()));

    auto tampered = *capsule;
    tampered.back() ^= 0x01U;
    QVERIFY(!store.restoreFromRecovery(reference, tampered, fixedPassword()).has_value());
    QCOMPARE(fake.write_count, baseline_writes);
    QVERIFY(std::ranges::equal(fake.peek(reference), baseline->bytes()));

    const auto truncated = std::span<const unsigned char>{capsule->data(), capsule->size() - 1U};
    QVERIFY(!store.restoreFromRecovery(reference, truncated, fixedPassword()).has_value());
    QCOMPARE(fake.write_count, baseline_writes);
    QVERIFY(std::ranges::equal(fake.peek(reference), baseline->bytes()));

    auto gigabyte_claim = *capsule;
    writeBigEndian64(std::span<unsigned char>{gigabyte_claim}.subspan(20U, 8U),
                     1024ULL * 1024ULL * 1024ULL);
    QVERIFY(!store.restoreFromRecovery(reference, gigabyte_claim, fixedPassword()).has_value());
    QCOMPARE(fake.write_count, baseline_writes);
    QVERIFY(std::ranges::equal(fake.peek(reference), baseline->bytes()));

    auto duplicate_plaintext = frozenKeyringBytes();
    duplicateFirstSlotId(duplicate_plaintext);
    std::array<unsigned char, crypto_pwhash_SALTBYTES> duplicate_salt{};
    std::array<unsigned char, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES> duplicate_nonce{};
    std::ranges::fill(duplicate_salt, 0x73U);
    std::ranges::fill(duplicate_nonce, 0x84U);
    const auto duplicate_capsule =
        referenceCapsule(duplicate_plaintext, fixedPassword(), duplicate_salt, duplicate_nonce);
    QVERIFY(!store.restoreFromRecovery(reference, duplicate_capsule, fixedPassword()).has_value());
    QCOMPARE(fake.write_count, baseline_writes);
    QVERIFY(std::ranges::equal(fake.peek(reference), baseline->bytes()));

    fake.next_write_error =
        SecretStoreError{SecretStoreErrorCode::WriteFailed,
                         QStringLiteral("injected all-or-nothing replacement failure")};
    const auto failed_write = store.restoreFromRecovery(reference, *capsule, fixedPassword());
    QVERIFY(!failed_write.has_value());
    QCOMPARE(failed_write.error().code, VaultKeyringStoreErrorCode::SecretStoreWriteFailed);
    QCOMPARE(fake.write_count, baseline_writes + 1U);
    QVERIFY(std::ranges::equal(fake.peek(reference), baseline->bytes()));

    QVERIFY(store.restoreFromRecovery(reference, *capsule, fixedPassword()).has_value());
    QCOMPARE(fake.write_count, baseline_writes + 2U);
    auto loaded = store.load(reference);
    QVERIFY(loaded.has_value());
    QVERIFY(*loaded == *replacement);
}

void RecoveryCapsuleTest::diagnosticsAndLogsContainNoSecretMaterial() {
    auto keyring = VaultKeyring::create();
    QVERIFY(keyring.has_value());
    const QString reference = QStringLiteral("vault:REFERENCE-CANARY-7391");
    const auto object_hex =
        QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(keyring->objectIdKey().data()),
                                       static_cast<qsizetype>(keyring->objectIdKey().size()))
                                .toHex());
    const auto dek_hex = QString::fromLatin1(
        QByteArray(reinterpret_cast<const char*>(keyring->encryptionKey(0).data()),
                   static_cast<qsizetype>(keyring->encryptionKey(0).size()))
            .toHex());
    constexpr char password_canary_text[] = "PASSWORD-CANARY-8264";
    const std::span<const unsigned char> password_canary{
        reinterpret_cast<const unsigned char*>(password_canary_text),
        sizeof(password_canary_text) - 1U};

    InMemorySecretStore fake;
    fake.next_write_error = SecretStoreError{SecretStoreErrorCode::WriteFailed,
                                             reference + u' ' + object_hex + u' ' + dek_hex + u' ' +
                                                 QString::fromLatin1(password_canary_text)};
    VaultKeyringStore store(&fake);
    QStringList messages;
    QString returned_diagnostics;
    {
        MessageCapture capture(messages);
        const auto write_failure = store.save(reference, *keyring);
        QVERIFY(!write_failure.has_value());
        returned_diagnostics += write_failure.error().message;
        const auto recovery_failure =
            RecoveryCapsuleCodec::importCapsule(frozenRecoveryCapsule(), password_canary);
        QVERIFY(!recovery_failure.has_value());
        returned_diagnostics += recovery_failure.error().message;
    }
    const auto all_output = messages.join(u'\n') + returned_diagnostics;
    QVERIFY(!all_output.contains(reference));
    QVERIFY(!all_output.contains(object_hex));
    QVERIFY(!all_output.contains(dek_hex));
    QVERIFY(!all_output.contains(QString::fromLatin1(password_canary_text)));
}

} // namespace

QTEST_GUILESS_MAIN(RecoveryCapsuleTest)

#include "tst_recovery_capsule.moc"
