#include "appellate/sync/local_folder_provider.hpp"
#include "appellate/sync/object_transport.hpp"
#include "appellate/sync/protocol_codec.hpp"

#include <QBuffer>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QIODevice>
#include <QMap>
#include <QString>
#include <QStringView>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QtTest>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <memory>
#include <utility>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

namespace {

using appellate::sync::FetchedSyncObject;
using appellate::sync::LocalFolderProvider;
using appellate::sync::ObjectProvider;
using appellate::sync::ObjectTransport;
using appellate::sync::ObjectTransportError;
using appellate::sync::ObjectTransportLocalError;
using appellate::sync::ObjectTransportLocalErrorCode;
using appellate::sync::ObjectTransportRetryPolicy;
using appellate::sync::ObjectTransportStage;
using appellate::sync::ProtocolCodec;
using appellate::sync::ProtocolError;
using appellate::sync::ProtocolErrorCode;
using appellate::sync::ProtocolKeySet;
using appellate::sync::ProtocolKeySlot;
using appellate::sync::ProviderCreateResult;
using appellate::sync::ProviderError;
using appellate::sync::ProviderErrorCode;
using appellate::sync::ProviderErrorRetryability;
using appellate::sync::ProviderListPage;
using appellate::sync::ProviderObjectMetadata;
using appellate::sync::PublishedSyncObject;
using appellate::sync::SyncObjectKind;

[[nodiscard]] ProtocolKeySet deterministicKeys(unsigned char offset = 0U) {
    ProtocolKeySet keys;
    for (std::size_t index = 0; index < keys.object_id_key.size(); ++index) {
        keys.object_id_key[index] = static_cast<unsigned char>(offset + index);
    }
    ProtocolKeySlot slot;
    for (std::size_t index = 0; index < slot.id.size(); ++index) {
        slot.id[index] = static_cast<unsigned char>(0xa0U + index);
    }
    for (std::size_t index = 0; index < slot.encryption_key.size(); ++index) {
        slot.encryption_key[index] = static_cast<unsigned char>(offset + 0x40U + index);
    }
    keys.key_slots.push_back(slot);
    return keys;
}

[[nodiscard]] auto openBuffer(QByteArray& bytes, QIODevice::OpenMode mode)
    -> std::unique_ptr<QBuffer> {
    auto buffer = std::make_unique<QBuffer>(&bytes);
    static_cast<void>(buffer->open(mode));
    return buffer;
}

[[nodiscard]] bool writeAll(QIODevice& destination, QByteArrayView bytes) {
    qsizetype offset{};
    while (offset < bytes.size()) {
        const auto written = destination.write(bytes.data() + offset, bytes.size() - offset);
        if (written <= 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

[[nodiscard]] QString canonicalStage(QTemporaryDir& temporary) {
    const auto path = temporary.filePath(QStringLiteral("stage"));
    if (!QDir{}.mkpath(path)) {
        return {};
    }
    return QFileInfo(path).canonicalFilePath();
}

[[nodiscard]] QStringList stagingEntries(const QString& directory) {
    return QDir(directory).entryList(QDir::AllEntries | QDir::Hidden | QDir::System |
                                     QDir::NoDotAndDotDot);
}

enum class CreateFault {
    None,
    FailBeforeWriteOnce,
    FailAfterPublishOnce,
    PartialThenFailOnce,
    AlwaysTransient,
    PermanentSourceFailure,
};

enum class DownloadFault {
    None,
    PartialThenRetryOnce,
    PartialSuccess,
    Corrupt,
    Trailing,
    WrongMetadata,
    OversizedAttempt,
    AlwaysTransient,
    PermanentDestinationFailure,
};

class FaultProvider final : public ObjectProvider {
  public:
    [[nodiscard]] auto list(QStringView, std::size_t) const
        -> std::expected<ProviderListPage, ProviderError> override {
        return ProviderListPage{};
    }

    [[nodiscard]] auto stat(QStringView remote_object_id) const
        -> std::expected<ProviderObjectMetadata, ProviderError> override {
        const auto found = objects.constFind(remote_object_id.toString());
        if (found == objects.cend()) {
            return std::unexpected(
                ProviderError{ProviderErrorCode::NotFound, QStringLiteral("missing")});
        }
        return ProviderObjectMetadata{remote_object_id.toString(),
                                      static_cast<std::uint64_t>(found->size())};
    }

    [[nodiscard]] auto createIfAbsent(QStringView remote_object_id, QIODevice& ciphertext,
                                      std::uint64_t ciphertext_bytes)
        -> std::expected<ProviderCreateResult, ProviderError> override {
        ++create_calls;
        if (create_fault == CreateFault::FailBeforeWriteOnce && create_calls == 1) {
            return transient(ProviderErrorCode::PublicationFailed);
        }
        if (create_fault == CreateFault::AlwaysTransient) {
            return transient(ProviderErrorCode::PublicationFailed);
        }
        if (create_fault == CreateFault::PermanentSourceFailure) {
            return std::unexpected(
                ProviderError{ProviderErrorCode::SourceReadFailed, QStringLiteral("source")});
        }
        if (create_fault == CreateFault::PartialThenFailOnce && create_calls == 1) {
            partial_upload = ciphertext.read(11);
            return transient(ProviderErrorCode::PublicationFailed);
        }

        const auto uploaded = ciphertext.readAll();
        upload_attempts.push_back(uploaded);
        const auto permissions = [&ciphertext] {
            const auto* file = dynamic_cast<QFileDevice*>(&ciphertext);
            if (file == nullptr) {
                return false;
            }
#ifdef Q_OS_UNIX
            struct stat status{};
            constexpr auto private_mode = static_cast<mode_t>(S_IRUSR | S_IWUSR);
            return file->handle() >= 0 && ::fstat(file->handle(), &status) == 0 &&
                   S_ISREG(status.st_mode) &&
                   (status.st_mode & static_cast<mode_t>(0777)) == private_mode &&
                   status.st_nlink == 0;
#else
            return false;
#endif
        }();
        every_upload_was_private = every_upload_was_private && permissions;
        if (static_cast<std::uint64_t>(uploaded.size()) != ciphertext_bytes) {
            return std::unexpected(
                ProviderError{ProviderErrorCode::SourceReadFailed, QStringLiteral("short")});
        }

        const auto key = remote_object_id.toString();
        if (objects.contains(key)) {
            return ProviderCreateResult::AlreadyPresent;
        }
        objects.insert(key, uploaded);
        if (create_fault == CreateFault::FailAfterPublishOnce && create_calls == 1) {
            return transient(ProviderErrorCode::PublicationFailed);
        }
        return ProviderCreateResult::Created;
    }

    [[nodiscard]] auto download(QStringView remote_object_id, QIODevice& destination) const
        -> std::expected<ProviderObjectMetadata, ProviderError> override {
        ++download_calls;
        const auto found = objects.constFind(remote_object_id.toString());
        if (found == objects.cend()) {
            return std::unexpected(
                ProviderError{ProviderErrorCode::NotFound, QStringLiteral("missing")});
        }
        if (download_fault == DownloadFault::AlwaysTransient) {
            return transient(ProviderErrorCode::CannotReadNamespace);
        }
        if (download_fault == DownloadFault::PermanentDestinationFailure) {
            return std::unexpected(ProviderError{ProviderErrorCode::DestinationWriteFailed,
                                                 QStringLiteral("destination")});
        }
        if (download_fault == DownloadFault::OversizedAttempt) {
            constexpr char byte = 'x';
            static_cast<void>(destination.write(
                &byte,
                static_cast<qint64>(ObjectTransport::maximum_download_ciphertext_bytes + 1U)));
            return ProviderObjectMetadata{remote_object_id.toString(), 0};
        }

        auto bytes = *found;
        if (download_fault == DownloadFault::PartialThenRetryOnce && download_calls == 1) {
            const auto partial = QByteArrayView(bytes).first(bytes.size() / 2);
            if (!writeAll(destination, partial)) {
                return std::unexpected(ProviderError{ProviderErrorCode::DestinationWriteFailed,
                                                     QStringLiteral("destination")});
            }
            return transient(ProviderErrorCode::CannotReadNamespace);
        }
        if (download_fault == DownloadFault::PartialSuccess) {
            const auto partial = QByteArrayView(bytes).first(bytes.size() / 2);
            if (!writeAll(destination, partial)) {
                return std::unexpected(ProviderError{ProviderErrorCode::DestinationWriteFailed,
                                                     QStringLiteral("destination")});
            }
            return ProviderObjectMetadata{remote_object_id.toString(),
                                          static_cast<std::uint64_t>(bytes.size())};
        }
        if (download_fault == DownloadFault::Corrupt && !bytes.isEmpty()) {
            bytes.back() = static_cast<char>(bytes.back() ^ 0x01);
        } else if (download_fault == DownloadFault::Trailing) {
            bytes.append('\0');
        }
        if (!writeAll(destination, bytes)) {
            return std::unexpected(ProviderError{ProviderErrorCode::DestinationWriteFailed,
                                                 QStringLiteral("destination")});
        }
        if (download_fault == DownloadFault::WrongMetadata) {
            return ProviderObjectMetadata{QString(64, QLatin1Char('0')),
                                          static_cast<std::uint64_t>(bytes.size())};
        }
        return ProviderObjectMetadata{remote_object_id.toString(),
                                      static_cast<std::uint64_t>(bytes.size())};
    }

    void store(const QString& remote_object_id, QByteArray bytes) {
        objects.insert(remote_object_id, std::move(bytes));
    }

    [[nodiscard]] const QByteArray& stored(const QString& remote_object_id) const {
        const auto found = objects.constFind(remote_object_id);
        Q_ASSERT(found != objects.cend());
        return found.value();
    }

    CreateFault create_fault{CreateFault::None};
    DownloadFault download_fault{DownloadFault::None};
    int create_calls{};
    mutable int download_calls{};
    bool every_upload_was_private{true};
    QByteArray partial_upload;
    QList<QByteArray> upload_attempts;
    QMap<QString, QByteArray> objects;

  private:
    [[nodiscard]] static auto transient(ProviderErrorCode code) -> std::unexpected<ProviderError> {
        return std::unexpected(
            ProviderError{code, QStringLiteral("transient"), ProviderErrorRetryability::Transient});
    }
};

struct CipherFixture final {
    QString remote_object_id;
    QByteArray bytes;
};

[[nodiscard]] auto encryptedFixture(QByteArray payload, const ProtocolKeySet& keys)
    -> std::expected<CipherFixture, ProtocolError> {
    auto source = openBuffer(payload, QIODevice::ReadOnly);
    QByteArray bytes;
    auto destination = openBuffer(bytes, QIODevice::WriteOnly);
    const auto encrypted =
        ProtocolCodec::encrypt(SyncObjectKind::SessionEventSegment, 1, *source,
                               static_cast<std::uint64_t>(payload.size()), keys, *destination);
    if (!encrypted) {
        return std::unexpected(encrypted.error());
    }
    return CipherFixture{encrypted->identity.remote_object_id, std::move(bytes)};
}

class FailingPayload final : public QIODevice {
  public:
    explicit FailingPayload(QByteArray bytes, qint64 failure_offset)
        : bytes_(std::move(bytes)), failure_offset_(failure_offset) {
        static_cast<void>(open(QIODevice::ReadOnly));
    }

    [[nodiscard]] bool isSequential() const override { return false; }
    [[nodiscard]] qint64 size() const override { return bytes_.size(); }
    [[nodiscard]] qint64 pos() const override { return position_; }

    bool seek(qint64 position) override {
        if (position < 0 || position > bytes_.size() || !QIODevice::seek(position)) {
            return false;
        }
        position_ = position;
        return true;
    }

  protected:
    [[nodiscard]] qint64 readData(char* destination, qint64 maximum_size) override {
        if (position_ >= failure_offset_) {
            return -1;
        }
        const auto count =
            std::min({maximum_size, failure_offset_ - position_, bytes_.size() - position_});
        if (count <= 0) {
            return 0;
        }
        std::memcpy(destination, bytes_.constData() + position_, static_cast<std::size_t>(count));
        position_ += count;
        return count;
    }

    [[nodiscard]] qint64 writeData(const char*, qint64) override { return -1; }

  private:
    QByteArray bytes_;
    qint64 failure_offset_{};
    qint64 position_{};
};

class RestoreFailingPayload final : public QIODevice {
  public:
    explicit RestoreFailingPayload(QByteArray bytes) : bytes_(std::move(bytes)) {
        static_cast<void>(open(QIODevice::ReadOnly));
    }

    [[nodiscard]] bool isSequential() const override { return false; }
    [[nodiscard]] qint64 size() const override { return bytes_.size(); }
    [[nodiscard]] qint64 pos() const override { return position_; }

    bool seek(qint64 position) override {
        ++seek_calls_;
        if (seek_calls_ > 1 || position < 0 || position > bytes_.size() ||
            !QIODevice::seek(position)) {
            return false;
        }
        position_ = position;
        return true;
    }

  protected:
    [[nodiscard]] qint64 readData(char* destination, qint64 maximum_size) override {
        const auto count = std::min(maximum_size, bytes_.size() - position_);
        if (count <= 0) {
            return 0;
        }
        std::memcpy(destination, bytes_.constData() + position_, static_cast<std::size_t>(count));
        position_ += count;
        return count;
    }

    [[nodiscard]] qint64 writeData(const char*, qint64) override { return -1; }

  private:
    QByteArray bytes_;
    qint64 position_{};
    int seek_calls_{};
};

class NoSeekPayload final : public QIODevice {
  public:
    explicit NoSeekPayload(QByteArray bytes) : bytes_(std::move(bytes)) {
        static_cast<void>(open(QIODevice::ReadOnly));
    }

    [[nodiscard]] bool isSequential() const override { return false; }
    [[nodiscard]] qint64 size() const override { return bytes_.size(); }
    [[nodiscard]] qint64 pos() const override { return position_; }
    [[nodiscard]] int readCalls() const noexcept { return read_calls_; }
    [[nodiscard]] int seekCalls() const noexcept { return seek_calls_; }

    bool seek(qint64) override {
        ++seek_calls_;
        return false;
    }

  protected:
    [[nodiscard]] qint64 readData(char* destination, qint64 maximum_size) override {
        ++read_calls_;
        const auto count = std::min(maximum_size, bytes_.size() - position_);
        if (count <= 0) {
            return 0;
        }
        std::memcpy(destination, bytes_.constData() + position_, static_cast<std::size_t>(count));
        position_ += count;
        return count;
    }

    [[nodiscard]] qint64 writeData(const char*, qint64) override { return -1; }

  private:
    QByteArray bytes_;
    qint64 position_{};
    int read_calls_{};
    int seek_calls_{};
};

class SequentialPayload final : public QIODevice {
  public:
    explicit SequentialPayload(QByteArray bytes) : bytes_(std::move(bytes)) {
        static_cast<void>(open(QIODevice::ReadOnly));
    }

    [[nodiscard]] bool isSequential() const override { return true; }
    [[nodiscard]] int readCalls() const noexcept { return read_calls_; }

  protected:
    [[nodiscard]] qint64 readData(char* destination, qint64 maximum_size) override {
        ++read_calls_;
        const auto count = std::min(maximum_size, bytes_.size() - position_);
        if (count <= 0) {
            return 0;
        }
        std::memcpy(destination, bytes_.constData() + position_, static_cast<std::size_t>(count));
        position_ += count;
        return count;
    }

    [[nodiscard]] qint64 writeData(const char*, qint64) override { return -1; }

  private:
    QByteArray bytes_;
    qint64 position_{};
    int read_calls_{};
};

template <typename Result> [[nodiscard]] const ProviderError& providerError(const Result& result) {
    return std::get<ProviderError>(result.error().failure);
}

template <typename Result> [[nodiscard]] const ProtocolError& protocolError(const Result& result) {
    return std::get<ProtocolError>(result.error().failure);
}

template <typename Result>
[[nodiscard]] const ObjectTransportLocalError& localError(const Result& result) {
    return std::get<ObjectTransportLocalError>(result.error().failure);
}

} // namespace

class ObjectTransportTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase();
    void validatesConfigurationAndLocalProviderConformance();
    void retainsDirectoryCapabilityAcrossAncestorSubstitution();
    void publishesCiphertextAndRestoresCallerPosition();
    void roundTripsAndDeduplicatesWithLocalFolderProvider();
    void acceptsOldCiphertextAfterActiveSlotRotation();
    void convergesAmbiguousAndPartialPublicationRetries();
    void boundsRetriesAndNeverRetriesPermanentErrors();
    void retriesPartialDownloadFromCleanStaging();
    void rejectsPartialCorruptTrailingChangedWrongKeyAndWrongId();
    void rejectsAttackerBytesOnAlreadyPresent();
    void preservesSourceFailuresAndLeavesNoPlaintext();
    void preservesProtocolErrorsForUnconsumedSources();
};

#include "provider_conformance.hpp"

void ObjectTransportTest::initTestCase() {
#ifndef Q_OS_UNIX
    QSKIP("ObjectTransport fails closed until this platform has an equivalent directory handle");
#endif
}

void ObjectTransportTest::validatesConfigurationAndLocalProviderConformance() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    FaultProvider fault_provider;
    const auto relative = ObjectTransport::open(fault_provider, QStringLiteral("stage"));
    QVERIFY(!relative);
    QCOMPARE(relative.error().stage, ObjectTransportStage::Configuration);
    QCOMPARE(relative.error().attempt_count, std::size_t{0});
    QCOMPARE(localError(relative).code, ObjectTransportLocalErrorCode::InvalidConfiguration);

    const auto stage = canonicalStage(temporary);
    QVERIFY(!stage.isEmpty());
    const auto zero_attempts =
        ObjectTransport::open(fault_provider, stage, ObjectTransportRetryPolicy{0});
    QVERIFY(!zero_attempts);
    const auto excessive_attempts = ObjectTransport::open(
        fault_provider, stage,
        ObjectTransportRetryPolicy{ObjectTransportRetryPolicy::hard_maximum_attempts + 1U});
    QVERIFY(!excessive_attempts);

    const auto real_parent = temporary.filePath(QStringLiteral("real-parent"));
    const auto real_child = QDir(real_parent).filePath(QStringLiteral("child"));
    QVERIFY(QDir{}.mkpath(real_child));
    const auto linked_parent = temporary.filePath(QStringLiteral("linked-parent"));
    QVERIFY(QFile::link(real_parent, linked_parent));
    const auto through_link = QDir(linked_parent).filePath(QStringLiteral("child"));
    const auto symlinked = ObjectTransport::open(fault_provider, through_link);
    QVERIFY(!symlinked);
    QCOMPARE(localError(symlinked).code, ObjectTransportLocalErrorCode::InvalidConfiguration);

    const auto provider_root = temporary.filePath(QStringLiteral("provider"));
    auto local = LocalFolderProvider::open(provider_root, 8);
    QVERIFY(local);
    appellate::sync::test::runProviderConformance(
        *local, 8, [&local](QStringView remote_object_id) {
            const auto path = QDir(local->rootDirectory())
                                  .filePath(QStringLiteral("objects/%1/%2.awobj")
                                                .arg(remote_object_id.first(2), remote_object_id));
            return QDir{}.mkpath(path);
        });
}

void ObjectTransportTest::retainsDirectoryCapabilityAcrossAncestorSubstitution() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto parent_root = temporary.filePath(QStringLiteral("parents"));
    const auto original_parent = QDir(parent_root).filePath(QStringLiteral("original"));
    const auto original_stage = QDir(original_parent).filePath(QStringLiteral("stage"));
    const auto redirect_parent = QDir(parent_root).filePath(QStringLiteral("redirect"));
    const auto redirect_stage = QDir(redirect_parent).filePath(QStringLiteral("stage"));
    QVERIFY(QDir{}.mkpath(original_stage));
    QVERIFY(QDir{}.mkpath(redirect_parent));
    QFile redirect_blocker(redirect_stage);
    QVERIFY(redirect_blocker.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(redirect_blocker.write("not a directory"), qint64{15});
    redirect_blocker.close();

    FaultProvider provider;
    auto transport = ObjectTransport::open(provider, QFileInfo(original_stage).canonicalFilePath());
    QVERIFY(transport);

    QDir parents(parent_root);
    QVERIFY(parents.rename(QStringLiteral("original"), QStringLiteral("moved")));
    const auto moved_stage =
        QDir(QDir(parent_root).filePath(QStringLiteral("moved"))).filePath(QStringLiteral("stage"));
    QVERIFY(QFile::link(redirect_parent, original_parent));
    QVERIFY(QFileInfo(original_parent).isSymLink());
    QVERIFY(QFileInfo(redirect_stage).isFile());
    QCOMPARE(QFileInfo(original_stage).canonicalFilePath(),
             QFileInfo(redirect_stage).canonicalFilePath());

    const auto keys = deterministicKeys();
    const QByteArray plaintext("retained directory handles defeat ancestor path substitution");
    QByteArray payload_bytes = plaintext;
    auto payload = openBuffer(payload_bytes, QIODevice::ReadOnly);
    QString remote_object_id;
    {
        auto published = transport->publish(SyncObjectKind::SessionEventSegment, 1, *payload,
                                            static_cast<std::uint64_t>(plaintext.size()), keys);
        QVERIFY(published);
        remote_object_id = published->encrypted.identity.remote_object_id;
        QCOMPARE(published->verified.quarantined_payload->readAll(), plaintext);
        QVERIFY(published->verified.quarantined_payload->fileName().isEmpty());
        QVERIFY(stagingEntries(redirect_stage).isEmpty());
        QVERIFY(stagingEntries(moved_stage).isEmpty());
    }

    provider.download_fault = DownloadFault::Corrupt;
    const auto rejected = transport->fetch(remote_object_id, keys);
    QVERIFY(!rejected);
    QCOMPARE(protocolError(rejected).code, ProtocolErrorCode::AuthenticationFailed);
    QVERIFY(stagingEntries(redirect_stage).isEmpty());
    QVERIFY(stagingEntries(moved_stage).isEmpty());
}

void ObjectTransportTest::publishesCiphertextAndRestoresCallerPosition() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto stage = canonicalStage(temporary);
    QVERIFY(!stage.isEmpty());
    FaultProvider provider;
    auto transport = ObjectTransport::open(provider, stage);
    QVERIFY(transport);
    QCOMPARE(transport->retryPolicy().maximum_attempts, std::size_t{3});

    const QByteArray plaintext(
        "private appellate payload that must never be handed to an object provider");
    QByteArray caller_bytes("prefix:");
    caller_bytes.append(plaintext);
    auto payload = openBuffer(caller_bytes, QIODevice::ReadOnly);
    QVERIFY(payload->seek(7));
    const auto original_position = payload->pos();
    const auto keys = deterministicKeys();

    {
        auto published = transport->publish(SyncObjectKind::SessionEventSegment, 1, *payload,
                                            static_cast<std::uint64_t>(plaintext.size()), keys);
        QVERIFY2(published, published ? "" : "publication failed");
        QCOMPARE(published->disposition, ProviderCreateResult::Created);
        QCOMPARE(published->publication_attempts, std::size_t{1});
        QCOMPARE(published->verification_attempts, std::size_t{1});
        QCOMPARE(payload->pos(), original_position);
        QCOMPARE(provider.create_calls, 1);
        QCOMPARE(provider.download_calls, 1);
        QCOMPARE(provider.upload_attempts.size(), qsizetype{1});
        QVERIFY(provider.every_upload_was_private);
        QVERIFY(!provider.upload_attempts.front().contains(plaintext));
        QVERIFY(provider.upload_attempts.front().startsWith("AWSO"));
        QCOMPARE(published->verified.kind, SyncObjectKind::SessionEventSegment);
        QCOMPARE(published->verified.schema_version, std::uint16_t{1});
        QCOMPARE(published->verified.quarantined_payload->pos(), qint64{0});
        QCOMPARE(published->verified.quarantined_payload->readAll(), plaintext);
    }
    QVERIFY(stagingEntries(stage).isEmpty());
}

void ObjectTransportTest::roundTripsAndDeduplicatesWithLocalFolderProvider() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto stage = canonicalStage(temporary);
    auto provider =
        LocalFolderProvider::open(temporary.filePath(QStringLiteral("transport-provider")));
    QVERIFY(provider);
    auto transport = ObjectTransport::open(*provider, stage);
    QVERIFY(transport);
    const auto keys = deterministicKeys();
    const QByteArray plaintext("real local provider transport round trip payload");
    QByteArray first_payload_bytes = plaintext;
    auto first_payload = openBuffer(first_payload_bytes, QIODevice::ReadOnly);

    QString remote_object_id;
    QByteArray first_stored_ciphertext;
    {
        auto first = transport->publish(SyncObjectKind::SessionEventSegment, 1, *first_payload,
                                        static_cast<std::uint64_t>(plaintext.size()), keys);
        QVERIFY(first);
        QCOMPARE(first->disposition, ProviderCreateResult::Created);
        QCOMPARE(first->verified.quarantined_payload->readAll(), plaintext);
        remote_object_id = first->encrypted.identity.remote_object_id;
        const auto stored_path =
            QDir(provider->rootDirectory())
                .filePath(QStringLiteral("objects/%1/%2.awobj")
                              .arg(remote_object_id.first(2), remote_object_id));
        QFile stored(stored_path);
        QVERIFY(stored.open(QIODevice::ReadOnly));
        first_stored_ciphertext = stored.readAll();
        QVERIFY(!first_stored_ciphertext.isEmpty());
        QVERIFY(!first_stored_ciphertext.contains(plaintext));
    }

    QByteArray duplicate_payload_bytes = plaintext;
    auto duplicate_payload = openBuffer(duplicate_payload_bytes, QIODevice::ReadOnly);
    {
        auto duplicate =
            transport->publish(SyncObjectKind::SessionEventSegment, 1, *duplicate_payload,
                               static_cast<std::uint64_t>(duplicate_payload_bytes.size()), keys);
        QVERIFY(duplicate);
        QCOMPARE(duplicate->disposition, ProviderCreateResult::AlreadyPresent);
        QCOMPARE(duplicate->encrypted.identity.remote_object_id, remote_object_id);
        QCOMPARE(duplicate->verified.quarantined_payload->readAll(), plaintext);
    }
    {
        auto fetched = transport->fetch(remote_object_id, keys);
        QVERIFY(fetched);
        QCOMPARE(fetched->verified.quarantined_payload->readAll(), plaintext);
    }

    const auto stored_path = QDir(provider->rootDirectory())
                                 .filePath(QStringLiteral("objects/%1/%2.awobj")
                                               .arg(remote_object_id.first(2), remote_object_id));
    QFile stored(stored_path);
    QVERIFY(stored.open(QIODevice::ReadOnly));
    QCOMPARE(stored.readAll(), first_stored_ciphertext);
    QVERIFY(stagingEntries(stage).isEmpty());
}

void ObjectTransportTest::acceptsOldCiphertextAfterActiveSlotRotation() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto stage = canonicalStage(temporary);
    FaultProvider provider;
    auto transport = ObjectTransport::open(provider, stage);
    QVERIFY(transport);

    const auto old_keys = deterministicKeys();
    auto rotated_keys = old_keys;
    ProtocolKeySlot new_slot;
    for (std::size_t index = 0; index < new_slot.id.size(); ++index) {
        new_slot.id[index] = static_cast<unsigned char>(0xc0U + index);
    }
    for (std::size_t index = 0; index < new_slot.encryption_key.size(); ++index) {
        new_slot.encryption_key[index] = static_cast<unsigned char>(0x20U + index);
    }
    rotated_keys.key_slots.push_back(new_slot);
    rotated_keys.active_slot = 1;
    const QByteArray plaintext("same canonical payload across active encryption slot rotation");

    QByteArray old_payload_bytes = plaintext;
    auto old_payload = openBuffer(old_payload_bytes, QIODevice::ReadOnly);
    QString remote_object_id;
    appellate::sync::SyncKeySlotId old_slot_id{};
    QByteArray stored_ciphertext;
    {
        auto first = transport->publish(SyncObjectKind::Checkpoint, 1, *old_payload,
                                        static_cast<std::uint64_t>(plaintext.size()), old_keys);
        QVERIFY(first);
        QCOMPARE(first->disposition, ProviderCreateResult::Created);
        remote_object_id = first->encrypted.identity.remote_object_id;
        old_slot_id = first->encrypted.key_slot_id;
        stored_ciphertext = provider.stored(remote_object_id);
    }

    QByteArray rotated_payload_bytes = plaintext;
    auto rotated_payload = openBuffer(rotated_payload_bytes, QIODevice::ReadOnly);
    auto duplicate =
        transport->publish(SyncObjectKind::Checkpoint, 1, *rotated_payload,
                           static_cast<std::uint64_t>(rotated_payload_bytes.size()), rotated_keys);
    QVERIFY(duplicate);
    QCOMPARE(duplicate->disposition, ProviderCreateResult::AlreadyPresent);
    QCOMPARE(duplicate->encrypted.identity.remote_object_id, remote_object_id);
    QCOMPARE(duplicate->encrypted.key_slot_id, new_slot.id);
    QCOMPARE(duplicate->verified.key_slot_id, old_slot_id);
    QCOMPARE(duplicate->verified.quarantined_payload->readAll(), plaintext);
    QCOMPARE(provider.stored(remote_object_id), stored_ciphertext);
}

void ObjectTransportTest::convergesAmbiguousAndPartialPublicationRetries() {
    const auto keys = deterministicKeys();
    const QByteArray plaintext("one encryption must serve every bounded retry attempt");

    for (const auto fault : {CreateFault::FailBeforeWriteOnce, CreateFault::FailAfterPublishOnce,
                             CreateFault::PartialThenFailOnce}) {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto stage = canonicalStage(temporary);
        FaultProvider provider;
        provider.create_fault = fault;
        auto transport = ObjectTransport::open(provider, stage);
        QVERIFY(transport);
        QByteArray caller_bytes = plaintext;
        auto payload = openBuffer(caller_bytes, QIODevice::ReadOnly);
        auto published = transport->publish(SyncObjectKind::SessionEventSegment, 1, *payload,
                                            static_cast<std::uint64_t>(plaintext.size()), keys);
        QVERIFY(published);
        QCOMPARE(published->publication_attempts, std::size_t{2});
        QCOMPARE(provider.create_calls, 2);
        QCOMPARE(payload->pos(), qint64{0});
        if (fault == CreateFault::FailAfterPublishOnce) {
            QCOMPARE(published->disposition, ProviderCreateResult::AlreadyPresent);
            QCOMPARE(provider.upload_attempts.size(), qsizetype{2});
            QCOMPARE(provider.upload_attempts.at(0), provider.upload_attempts.at(1));
        } else {
            QCOMPARE(published->disposition, ProviderCreateResult::Created);
        }
        if (fault == CreateFault::PartialThenFailOnce) {
            QCOMPARE(provider.partial_upload.size(), qsizetype{11});
        }
    }
}

void ObjectTransportTest::boundsRetriesAndNeverRetriesPermanentErrors() {
    const auto keys = deterministicKeys();
    const QByteArray plaintext("bounded publication retry payload");

    {
        QTemporaryDir temporary;
        const auto stage = canonicalStage(temporary);
        FaultProvider provider;
        provider.create_fault = CreateFault::AlwaysTransient;
        auto transport = ObjectTransport::open(provider, stage);
        QVERIFY(transport);
        QByteArray bytes = plaintext;
        auto payload = openBuffer(bytes, QIODevice::ReadOnly);
        const auto result = transport->publish(SyncObjectKind::SessionEventSegment, 1, *payload,
                                               static_cast<std::uint64_t>(bytes.size()), keys);
        QVERIFY(!result);
        QCOMPARE(result.error().stage, ObjectTransportStage::Publication);
        QCOMPARE(result.error().attempt_count, std::size_t{3});
        QCOMPARE(providerError(result).retryability, ProviderErrorRetryability::Transient);
        QCOMPARE(provider.create_calls, 3);
        QCOMPARE(payload->pos(), qint64{0});
        QVERIFY(stagingEntries(stage).isEmpty());
    }
    {
        QTemporaryDir temporary;
        const auto stage = canonicalStage(temporary);
        FaultProvider provider;
        provider.create_fault = CreateFault::PermanentSourceFailure;
        auto transport = ObjectTransport::open(provider, stage);
        QVERIFY(transport);
        QByteArray bytes = plaintext;
        auto payload = openBuffer(bytes, QIODevice::ReadOnly);
        const auto result = transport->publish(SyncObjectKind::SessionEventSegment, 1, *payload,
                                               static_cast<std::uint64_t>(bytes.size()), keys);
        QVERIFY(!result);
        QCOMPARE(result.error().attempt_count, std::size_t{1});
        QCOMPARE(providerError(result).code, ProviderErrorCode::SourceReadFailed);
        QCOMPARE(provider.create_calls, 1);
    }
    {
        QTemporaryDir temporary;
        const auto stage = canonicalStage(temporary);
        FaultProvider provider;
        provider.download_fault = DownloadFault::AlwaysTransient;
        const auto fixture = encryptedFixture(plaintext, keys);
        QVERIFY(fixture);
        provider.store(fixture->remote_object_id, fixture->bytes);
        auto transport = ObjectTransport::open(provider, stage);
        QVERIFY(transport);
        const auto result = transport->fetch(fixture->remote_object_id, keys);
        QVERIFY(!result);
        QCOMPARE(result.error().stage, ObjectTransportStage::Download);
        QCOMPARE(result.error().attempt_count, std::size_t{3});
        QCOMPARE(provider.download_calls, 3);
        QVERIFY(stagingEntries(stage).isEmpty());
    }
}

void ObjectTransportTest::retriesPartialDownloadFromCleanStaging() {
    QTemporaryDir temporary;
    const auto stage = canonicalStage(temporary);
    FaultProvider provider;
    provider.download_fault = DownloadFault::PartialThenRetryOnce;
    const auto keys = deterministicKeys();
    const QByteArray plaintext("partial first download must be truncated before retry");
    const auto fixture = encryptedFixture(plaintext, keys);
    QVERIFY(fixture);
    provider.store(fixture->remote_object_id, fixture->bytes);
    auto transport = ObjectTransport::open(provider, stage);
    QVERIFY(transport);

    {
        auto fetched = transport->fetch(fixture->remote_object_id, keys);
        QVERIFY(fetched);
        QCOMPARE(fetched->attempts, std::size_t{2});
        QCOMPARE(provider.download_calls, 2);
        QCOMPARE(fetched->verified.quarantined_payload->readAll(), plaintext);
    }
    QVERIFY(stagingEntries(stage).isEmpty());
}

void ObjectTransportTest::rejectsPartialCorruptTrailingChangedWrongKeyAndWrongId() {
    const auto keys = deterministicKeys();
    const QByteArray plaintext("authenticated fetch fault matrix payload");
    const auto fixture = encryptedFixture(plaintext, keys);
    QVERIFY(fixture);

    for (const auto fault :
         {DownloadFault::PartialSuccess, DownloadFault::Corrupt, DownloadFault::Trailing,
          DownloadFault::WrongMetadata, DownloadFault::OversizedAttempt,
          DownloadFault::PermanentDestinationFailure}) {
        QTemporaryDir temporary;
        const auto stage = canonicalStage(temporary);
        FaultProvider provider;
        provider.download_fault = fault;
        provider.store(fixture->remote_object_id, fixture->bytes);
        auto transport = ObjectTransport::open(provider, stage);
        QVERIFY(transport);
        const auto result = transport->fetch(fixture->remote_object_id, keys);
        QVERIFY(!result);
        QCOMPARE(provider.download_calls, 1);
        if (fault == DownloadFault::PartialSuccess || fault == DownloadFault::WrongMetadata ||
            fault == DownloadFault::OversizedAttempt) {
            QCOMPARE(result.error().stage, ObjectTransportStage::Download);
            QCOMPARE(localError(result).code,
                     ObjectTransportLocalErrorCode::InvalidProviderResponse);
        } else if (fault == DownloadFault::PermanentDestinationFailure) {
            QCOMPARE(providerError(result).code, ProviderErrorCode::DestinationWriteFailed);
        } else {
            QCOMPARE(result.error().stage, ObjectTransportStage::Verification);
            QCOMPARE(protocolError(result).code, fault == DownloadFault::Trailing
                                                     ? ProtocolErrorCode::TrailingCiphertext
                                                     : ProtocolErrorCode::AuthenticationFailed);
        }
        QVERIFY(stagingEntries(stage).isEmpty());
    }

    {
        QTemporaryDir temporary;
        const auto stage = canonicalStage(temporary);
        FaultProvider provider;
        provider.store(fixture->remote_object_id, fixture->bytes);
        auto transport = ObjectTransport::open(provider, stage);
        QVERIFY(transport);
        auto wrong_keys = keys;
        wrong_keys.key_slots.front().encryption_key.front() ^= 0x01U;
        const auto result = transport->fetch(fixture->remote_object_id, wrong_keys);
        QVERIFY(!result);
        QCOMPARE(protocolError(result).code, ProtocolErrorCode::AuthenticationFailed);
        QCOMPARE(provider.download_calls, 1);
        QVERIFY(stagingEntries(stage).isEmpty());
    }
    {
        QTemporaryDir temporary;
        const auto stage = canonicalStage(temporary);
        FaultProvider provider;
        const auto wrong_id = QString(64, QLatin1Char('a'));
        provider.store(wrong_id, fixture->bytes);
        auto transport = ObjectTransport::open(provider, stage);
        QVERIFY(transport);
        const auto result = transport->fetch(wrong_id, keys);
        QVERIFY(!result);
        QCOMPARE(protocolError(result).code, ProtocolErrorCode::AuthenticationFailed);
        QCOMPARE(provider.download_calls, 1);
        QVERIFY(stagingEntries(stage).isEmpty());
    }
    {
        QTemporaryDir temporary;
        const auto stage = canonicalStage(temporary);
        FaultProvider provider;
        auto transport = ObjectTransport::open(provider, stage);
        QVERIFY(transport);
        const auto result = transport->fetch(QString(64, QLatin1Char('b')), keys);
        QVERIFY(!result);
        QCOMPARE(providerError(result).code, ProviderErrorCode::NotFound);
        QCOMPARE(provider.download_calls, 1);
    }
}

void ObjectTransportTest::rejectsAttackerBytesOnAlreadyPresent() {
    QTemporaryDir temporary;
    const auto stage = canonicalStage(temporary);
    FaultProvider provider;
    auto transport = ObjectTransport::open(provider, stage);
    QVERIFY(transport);
    const auto keys = deterministicKeys();
    QByteArray plaintext("canonical caller payload for an occupied remote object name");
    const auto identity = ProtocolCodec::canonicalIdentity(SyncObjectKind::SessionEventSegment, 1,
                                                           plaintext, keys.object_id_key);
    QVERIFY(identity);
    const QByteArray attacker_bytes("attacker controlled non-envelope bytes");
    provider.store(identity->remote_object_id, attacker_bytes);
    auto payload = openBuffer(plaintext, QIODevice::ReadOnly);

    const auto result = transport->publish(SyncObjectKind::SessionEventSegment, 1, *payload,
                                           static_cast<std::uint64_t>(plaintext.size()), keys);
    QVERIFY(!result);
    QCOMPARE(result.error().stage, ObjectTransportStage::Verification);
    QCOMPARE(protocolError(result).code, ProtocolErrorCode::MalformedEnvelope);
    QCOMPARE(provider.stored(identity->remote_object_id), attacker_bytes);
    QCOMPARE(provider.create_calls, 1);
    QCOMPARE(provider.download_calls, 1);
    QCOMPARE(payload->pos(), qint64{0});
    QVERIFY(stagingEntries(stage).isEmpty());
}

void ObjectTransportTest::preservesSourceFailuresAndLeavesNoPlaintext() {
    QTemporaryDir temporary;
    const auto stage = canonicalStage(temporary);
    FaultProvider provider;
    auto transport = ObjectTransport::open(provider, stage);
    QVERIFY(transport);
    const auto keys = deterministicKeys();
    FailingPayload payload(QByteArray("source fails after these first bytes"), 7);
    QVERIFY(payload.seek(2));
    const auto original_position = payload.pos();

    const auto result =
        transport->publish(SyncObjectKind::SessionEventSegment, 1, payload, 34, keys);
    QVERIFY(!result);
    QCOMPARE(result.error().stage, ObjectTransportStage::Encryption);
    QCOMPARE(protocolError(result).code, ProtocolErrorCode::SourceReadFailed);
    QCOMPARE(payload.pos(), original_position);
    QCOMPARE(provider.create_calls, 0);
    QCOMPARE(provider.download_calls, 0);
    QVERIFY(stagingEntries(stage).isEmpty());

    RestoreFailingPayload restore_failure(
        QByteArray("encryption succeeds but caller-position restoration fails"));
    const auto restoration =
        transport->publish(SyncObjectKind::SessionEventSegment, 1, restore_failure,
                           static_cast<std::uint64_t>(restore_failure.size()), keys);
    QVERIFY(!restoration);
    QCOMPARE(restoration.error().stage, ObjectTransportStage::SourceRestoration);
    QCOMPARE(localError(restoration).code,
             ObjectTransportLocalErrorCode::CannotRestoreSourcePosition);
    QCOMPARE(provider.create_calls, 0);
    QCOMPARE(provider.download_calls, 0);
    QVERIFY(stagingEntries(stage).isEmpty());
}

void ObjectTransportTest::preservesProtocolErrorsForUnconsumedSources() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto stage = canonicalStage(temporary);
    FaultProvider provider;
    auto transport = ObjectTransport::open(provider, stage);
    QVERIFY(transport);
    const auto keys = deterministicKeys();

    NoSeekPayload invalid_kind_payload(QByteArray("invalid kind remains unread"));
    const auto invalid_kind =
        transport->publish(static_cast<SyncObjectKind>(0), 1, invalid_kind_payload,
                           static_cast<std::uint64_t>(invalid_kind_payload.size()), keys);
    QVERIFY(!invalid_kind);
    QCOMPARE(invalid_kind.error().stage, ObjectTransportStage::Encryption);
    QCOMPARE(protocolError(invalid_kind).code, ProtocolErrorCode::InvalidArgument);
    QCOMPARE(invalid_kind_payload.pos(), qint64{0});
    QCOMPARE(invalid_kind_payload.readCalls(), 0);
    QCOMPARE(invalid_kind_payload.seekCalls(), 0);

    SequentialPayload sequential_payload(QByteArray("sequential payload remains unread"));
    const auto sequential =
        transport->publish(SyncObjectKind::SessionEventSegment, 1, sequential_payload, 33, keys);
    QVERIFY(!sequential);
    QCOMPARE(sequential.error().stage, ObjectTransportStage::Encryption);
    QCOMPARE(protocolError(sequential).code, ProtocolErrorCode::SourceNotSeekable);
    QCOMPARE(sequential_payload.readCalls(), 0);
    QCOMPARE(provider.create_calls, 0);
    QCOMPARE(provider.download_calls, 0);
    QVERIFY(stagingEntries(stage).isEmpty());
}

QTEST_APPLESS_MAIN(ObjectTransportTest)

#include "tst_object_transport.moc"
