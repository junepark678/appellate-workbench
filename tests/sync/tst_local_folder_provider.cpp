#include "appellate/sync/local_folder_provider.hpp"
#include "appellate/sync/protocol_codec.hpp"

#include <QBuffer>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

#include <array>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <thread>

namespace {

using appellate::sync::LocalFolderProvider;
using appellate::sync::ProtocolCodec;
using appellate::sync::ProtocolKeySet;
using appellate::sync::ProtocolKeySlot;
using appellate::sync::ProviderCreateResult;
using appellate::sync::ProviderErrorCode;
using appellate::sync::SyncObjectKind;

[[nodiscard]] QString remoteId(char character) { return QString(64, QChar::fromLatin1(character)); }

[[nodiscard]] ProtocolKeySet deterministicKeys() {
    ProtocolKeySet keys;
    for (std::size_t index = 0; index < keys.object_id_key.size(); ++index) {
        keys.object_id_key[index] = static_cast<unsigned char>(index);
    }
    ProtocolKeySlot slot;
    for (std::size_t index = 0; index < slot.id.size(); ++index) {
        slot.id[index] = static_cast<unsigned char>(0xa0U + index);
    }
    for (std::size_t index = 0; index < slot.encryption_key.size(); ++index) {
        slot.encryption_key[index] = static_cast<unsigned char>(0x40U + index);
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

[[nodiscard]] QString storedPath(const LocalFolderProvider& provider, const QString& remote_id) {
    return QDir(provider.rootDirectory())
        .filePath(QStringLiteral("objects/%1/%2.awobj").arg(remote_id.first(2), remote_id));
}

} // namespace

class LocalFolderProviderTest final : public QObject {
    Q_OBJECT

  private slots:
    void rejectsInvalidConfigurationAndObjectIds();
    void publishesCreateOnlyObjects();
    void concurrentPublicationNeverOverwrites();
    void listsStablePagesAndDetectsNamespaceCorruption();
    void roundTripsEncryptedObject();
    void rejectsNonregularAndOversizedObjects();
};

void LocalFolderProviderTest::rejectsInvalidConfigurationAndObjectIds() {
    const auto relative = LocalFolderProvider::open(QStringLiteral("relative/provider"));
    QVERIFY(!relative);
    QCOMPARE(relative.error().code, ProviderErrorCode::InvalidArgument);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto real_root = temporary.filePath(QStringLiteral("real-root"));
    const auto linked_root = temporary.filePath(QStringLiteral("linked-root"));
    QVERIFY(QDir{}.mkpath(real_root));
    QVERIFY(QFile::link(real_root, linked_root));
    const auto symlinked = LocalFolderProvider::open(linked_root);
    QVERIFY(!symlinked);
    QCOMPARE(symlinked.error().code, ProviderErrorCode::CannotCreateNamespace);

    const auto provider = LocalFolderProvider::open(temporary.filePath(QStringLiteral("provider")));
    QVERIFY(provider.has_value());
    QCOMPARE(provider->rootDirectory(),
             QFileInfo(temporary.filePath(QStringLiteral("provider"))).canonicalFilePath());

    const auto bad_stat = provider->stat(QString(64, QLatin1Char('A')));
    QVERIFY(!bad_stat);
    QCOMPARE(bad_stat.error().code, ProviderErrorCode::InvalidArgument);
    const auto bad_page = provider->list(QStringLiteral("bad"), 1);
    QVERIFY(!bad_page);
    QCOMPARE(bad_page.error().code, ProviderErrorCode::InvalidArgument);
    const auto zero_page = provider->list({}, 0);
    QVERIFY(!zero_page);
    QCOMPARE(zero_page.error().code, ProviderErrorCode::InvalidArgument);
    const auto huge_page = provider->list({}, LocalFolderProvider::maximum_page_size + 1);
    QVERIFY(!huge_page);
    QCOMPARE(huge_page.error().code, ProviderErrorCode::InvalidArgument);

    const auto missing = provider->stat(remoteId('a'));
    QVERIFY(!missing);
    QCOMPARE(missing.error().code, ProviderErrorCode::NotFound);
}

void LocalFolderProviderTest::publishesCreateOnlyObjects() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    auto provider = LocalFolderProvider::open(temporary.filePath(QStringLiteral("provider")));
    QVERIFY(provider.has_value());

    const auto remote_id = remoteId('a');
    QByteArray first_bytes("first-ciphertext");
    auto first_source = openBuffer(first_bytes, QIODevice::ReadOnly);
    const auto first = provider->createIfAbsent(remote_id, *first_source,
                                                static_cast<std::uint64_t>(first_bytes.size()));
    QVERIFY(first.has_value());
    QCOMPARE(*first, ProviderCreateResult::Created);

    const auto path = storedPath(*provider, remote_id);
    QVERIFY(QFileInfo::exists(path));
    const auto permissions = QFileInfo(path).permissions();
    QVERIFY(permissions.testFlag(QFileDevice::ReadOwner));
    QVERIFY(permissions.testFlag(QFileDevice::WriteOwner));
    QVERIFY(!(permissions &
              (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
               QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther)));

    QByteArray replacement("other-ciphertext");
    auto replacement_source = openBuffer(replacement, QIODevice::ReadOnly);
    const auto duplicate = provider->createIfAbsent(remote_id, *replacement_source,
                                                    static_cast<std::uint64_t>(replacement.size()));
    QVERIFY(duplicate.has_value());
    QCOMPARE(*duplicate, ProviderCreateResult::AlreadyPresent);
    QCOMPARE(replacement_source->pos(), qint64{0});

    QByteArray downloaded;
    auto destination = openBuffer(downloaded, QIODevice::WriteOnly);
    const auto metadata = provider->download(remote_id, *destination);
    QVERIFY(metadata.has_value());
    QCOMPARE(metadata->remote_object_id, remote_id);
    QCOMPARE(metadata->ciphertext_bytes, static_cast<std::uint64_t>(first_bytes.size()));
    QCOMPARE(downloaded, first_bytes);

    const QDir prefix(QFileInfo(path).absolutePath());
    const auto staging_files =
        prefix.entryList({QStringLiteral(".upload-*")}, QDir::Files | QDir::Hidden | QDir::System);
    QVERIFY(staging_files.isEmpty());

    QFile stale(prefix.filePath(QStringLiteral(".upload-stale")));
    QVERIFY(stale.open(QIODevice::WriteOnly));
    QCOMPARE(stale.write("ciphertext"), qint64{10});
    stale.close();
    const auto with_stale_upload = provider->list({}, 10);
    QVERIFY(with_stale_upload.has_value());
    QCOMPARE(with_stale_upload->objects.size(), std::size_t{1});
}

void LocalFolderProviderTest::concurrentPublicationNeverOverwrites() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = temporary.filePath(QStringLiteral("provider"));
    auto first_provider = LocalFolderProvider::open(root);
    auto second_provider = LocalFolderProvider::open(root);
    QVERIFY(first_provider.has_value());
    QVERIFY(second_provider.has_value());

    const auto remote_id = remoteId('9');
    QByteArray first_bytes("first-device-object");
    QByteArray second_bytes("other-device-object");
    QCOMPARE(first_bytes.size(), second_bytes.size());
    std::barrier start(3);
    std::optional<std::expected<ProviderCreateResult, appellate::sync::ProviderError>> first_result;
    std::optional<std::expected<ProviderCreateResult, appellate::sync::ProviderError>>
        second_result;
    std::jthread first_thread([&] {
        auto source = openBuffer(first_bytes, QIODevice::ReadOnly);
        start.arrive_and_wait();
        first_result = first_provider->createIfAbsent(
            remote_id, *source, static_cast<std::uint64_t>(first_bytes.size()));
    });
    std::jthread second_thread([&] {
        auto source = openBuffer(second_bytes, QIODevice::ReadOnly);
        start.arrive_and_wait();
        second_result = second_provider->createIfAbsent(
            remote_id, *source, static_cast<std::uint64_t>(second_bytes.size()));
    });
    start.arrive_and_wait();
    first_thread.join();
    second_thread.join();

    QVERIFY(first_result.has_value());
    QVERIFY(second_result.has_value());
    QVERIFY(first_result->has_value());
    QVERIFY(second_result->has_value());
    const auto created_count = static_cast<int>(**first_result == ProviderCreateResult::Created) +
                               static_cast<int>(**second_result == ProviderCreateResult::Created);
    const auto present_count =
        static_cast<int>(**first_result == ProviderCreateResult::AlreadyPresent) +
        static_cast<int>(**second_result == ProviderCreateResult::AlreadyPresent);
    QCOMPARE(created_count, 1);
    QCOMPARE(present_count, 1);

    QByteArray downloaded;
    auto destination = openBuffer(downloaded, QIODevice::WriteOnly);
    QVERIFY(first_provider->download(remote_id, *destination).has_value());
    QVERIFY(downloaded == first_bytes || downloaded == second_bytes);
}

void LocalFolderProviderTest::listsStablePagesAndDetectsNamespaceCorruption() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    auto provider = LocalFolderProvider::open(temporary.filePath(QStringLiteral("provider")));
    QVERIFY(provider.has_value());

    for (const auto marker : {'c', 'a', 'b'}) {
        QByteArray bytes(1, marker);
        auto source = openBuffer(bytes, QIODevice::ReadOnly);
        const auto created = provider->createIfAbsent(remoteId(marker), *source,
                                                      static_cast<std::uint64_t>(bytes.size()));
        QVERIFY(created.has_value());
        QCOMPARE(*created, ProviderCreateResult::Created);
    }

    const auto first = provider->list({}, 2);
    QVERIFY(first.has_value());
    QCOMPARE(first->objects.size(), std::size_t{2});
    QCOMPARE(first->objects.at(0).remote_object_id, remoteId('a'));
    QCOMPARE(first->objects.at(1).remote_object_id, remoteId('b'));
    QCOMPARE(first->continuation_token, remoteId('b'));
    const auto second = provider->list(first->continuation_token, 2);
    QVERIFY(second.has_value());
    QCOMPARE(second->objects.size(), std::size_t{1});
    QCOMPARE(second->objects.front().remote_object_id, remoteId('c'));
    QVERIFY(second->continuation_token.isEmpty());

    const auto malformed = QDir(provider->rootDirectory()).filePath(QStringLiteral("objects/zz"));
    QVERIFY(QDir{}.mkpath(malformed));
    const auto rejected = provider->list({}, 10);
    QVERIFY(!rejected);
    QCOMPARE(rejected.error().code, ProviderErrorCode::CannotReadNamespace);

    QVERIFY(QDir(malformed).removeRecursively());
    const auto wrong_prefix =
        QDir(provider->rootDirectory())
            .filePath(QStringLiteral("objects/aa/%1.awobj").arg(remoteId('b')));
    QVERIFY(QDir{}.mkpath(QFileInfo(wrong_prefix).absolutePath()));
    QFile misplaced(wrong_prefix);
    QVERIFY(misplaced.open(QIODevice::WriteOnly));
    QCOMPARE(misplaced.write("ciphertext"), qint64{10});
    misplaced.close();
    const auto misplaced_result = provider->list({}, 10);
    QVERIFY(!misplaced_result);
    QCOMPARE(misplaced_result.error().code, ProviderErrorCode::CannotReadNamespace);
}

void LocalFolderProviderTest::roundTripsEncryptedObject() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    auto provider = LocalFolderProvider::open(temporary.filePath(QStringLiteral("provider")));
    QVERIFY(provider.has_value());

    const auto keys = deterministicKeys();
    QByteArray plaintext("private appellate session segment");
    auto plaintext_source = openBuffer(plaintext, QIODevice::ReadOnly);
    QByteArray ciphertext;
    auto ciphertext_destination = openBuffer(ciphertext, QIODevice::WriteOnly);
    const auto encrypted = ProtocolCodec::encrypt(
        SyncObjectKind::SessionEventSegment, 1, *plaintext_source,
        static_cast<std::uint64_t>(plaintext.size()), keys, *ciphertext_destination);
    QVERIFY(encrypted.has_value());

    auto upload_source = openBuffer(ciphertext, QIODevice::ReadOnly);
    const auto published =
        provider->createIfAbsent(encrypted->identity.remote_object_id, *upload_source,
                                 static_cast<std::uint64_t>(ciphertext.size()));
    QVERIFY(published.has_value());
    QCOMPARE(*published, ProviderCreateResult::Created);

    QByteArray received_ciphertext;
    auto received_destination = openBuffer(received_ciphertext, QIODevice::WriteOnly);
    const auto received =
        provider->download(encrypted->identity.remote_object_id, *received_destination);
    QVERIFY(received.has_value());
    QCOMPARE(received_ciphertext, ciphertext);

    auto decrypt_source = openBuffer(received_ciphertext, QIODevice::ReadOnly);
    const auto quarantine = temporary.filePath(QStringLiteral("quarantine"));
    QVERIFY(QDir{}.mkpath(quarantine));
    const auto verified = ProtocolCodec::decrypt(
        *decrypt_source, encrypted->identity.remote_object_id, keys, quarantine);
    QVERIFY2(verified.has_value(), verified ? "" : verified.error().message.toUtf8().constData());
    QCOMPARE(verified->kind, SyncObjectKind::SessionEventSegment);
    QCOMPARE(verified->identity, encrypted->identity);
    QCOMPARE(verified->quarantined_payload->readAll(), plaintext);
}

void LocalFolderProviderTest::rejectsNonregularAndOversizedObjects() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    auto provider = LocalFolderProvider::open(temporary.filePath(QStringLiteral("provider")), 8);
    QVERIFY(provider.has_value());

    QByteArray oversized("123456789");
    auto oversized_source = openBuffer(oversized, QIODevice::ReadOnly);
    const auto rejected = provider->createIfAbsent(remoteId('d'), *oversized_source,
                                                   static_cast<std::uint64_t>(oversized.size()));
    QVERIFY(!rejected);
    QCOMPARE(rejected.error().code, ProviderErrorCode::ObjectTooLarge);
    QVERIFY(!QFileInfo::exists(storedPath(*provider, remoteId('d'))));

    const auto invalid_path = storedPath(*provider, remoteId('e'));
    QVERIFY(QDir{}.mkpath(invalid_path));
    const auto invalid = provider->stat(remoteId('e'));
    QVERIFY(!invalid);
    QCOMPARE(invalid.error().code, ProviderErrorCode::InvalidObject);

    const auto symlink_id = remoteId('f');
    const auto symlink_path = storedPath(*provider, symlink_id);
    QVERIFY(QDir{}.mkpath(QFileInfo(symlink_path).absolutePath()));
    const auto symlink_target = temporary.filePath(QStringLiteral("outside-ciphertext"));
    QFile target(symlink_target);
    QVERIFY(target.open(QIODevice::WriteOnly));
    QCOMPARE(target.write("1234"), qint64{4});
    target.close();
    QVERIFY(QFile::link(symlink_target, symlink_path));
    const auto symlink = provider->stat(symlink_id);
    QVERIFY(!symlink);
    QCOMPARE(symlink.error().code, ProviderErrorCode::InvalidObject);

    QByteArray bytes("short");
    auto source = openBuffer(bytes, QIODevice::ReadOnly);
    const auto wrong_length = provider->createIfAbsent(remoteId('0'), *source, 4);
    QVERIFY(!wrong_length);
    QCOMPARE(wrong_length.error().code, ProviderErrorCode::InvalidArgument);
    QVERIFY(!QFileInfo::exists(storedPath(*provider, remoteId('0'))));
}

QTEST_APPLESS_MAIN(LocalFolderProviderTest)

#include "tst_local_folder_provider.moc"
