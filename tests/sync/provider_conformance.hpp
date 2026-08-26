#pragma once

#include "appellate/sync/object_provider.hpp"

#include <QBuffer>
#include <QByteArray>
#include <QIODevice>
#include <QString>
#include <QStringView>
#include <QtTest>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace appellate::sync::test {

[[nodiscard]] inline QString conformanceRemoteId(char marker) {
    return QString(64, QChar::fromLatin1(marker));
}

[[nodiscard]] inline auto conformanceBuffer(QByteArray& bytes, QIODevice::OpenMode mode)
    -> std::unique_ptr<QBuffer> {
    auto buffer = std::make_unique<QBuffer>(&bytes);
    static_cast<void>(buffer->open(mode));
    return buffer;
}

// The fixture hook deliberately sits outside ObjectProvider: creating a nonregular backing
// object is provider-specific, while all resulting assertions exercise the common API.
template <typename InstallNonregular>
void runProviderConformance(ObjectProvider& provider, std::uint64_t maximum_ciphertext_bytes,
                            InstallNonregular install_nonregular) {
    const auto invalid_id = QString(64, QLatin1Char('A'));
    const auto missing_id = conformanceRemoteId('f');

    const auto invalid_stat = provider.stat(invalid_id);
    QVERIFY(!invalid_stat);
    QCOMPARE(invalid_stat.error().code, ProviderErrorCode::InvalidArgument);
    const auto invalid_list = provider.list(QStringLiteral("invalid"), 1);
    QVERIFY(!invalid_list);
    QCOMPARE(invalid_list.error().code, ProviderErrorCode::InvalidArgument);
    const auto invalid_page_size = provider.list({}, 0);
    QVERIFY(!invalid_page_size);
    QCOMPARE(invalid_page_size.error().code, ProviderErrorCode::InvalidArgument);

    QByteArray invalid_upload_bytes("ciphertext");
    auto invalid_upload = conformanceBuffer(invalid_upload_bytes, QIODevice::ReadOnly);
    const auto invalid_create = provider.createIfAbsent(
        invalid_id, *invalid_upload, static_cast<std::uint64_t>(invalid_upload_bytes.size()));
    QVERIFY(!invalid_create);
    QCOMPARE(invalid_create.error().code, ProviderErrorCode::InvalidArgument);

    QByteArray invalid_destination_bytes;
    auto invalid_destination = conformanceBuffer(invalid_destination_bytes, QIODevice::WriteOnly);
    const auto invalid_download = provider.download(invalid_id, *invalid_destination);
    QVERIFY(!invalid_download);
    QCOMPARE(invalid_download.error().code, ProviderErrorCode::InvalidArgument);

    const auto missing_stat = provider.stat(missing_id);
    QVERIFY(!missing_stat);
    QCOMPARE(missing_stat.error().code, ProviderErrorCode::NotFound);
    const auto missing_download = provider.download(missing_id, *invalid_destination);
    QVERIFY(!missing_download);
    QCOMPARE(missing_download.error().code, ProviderErrorCode::NotFound);

    for (const auto marker : {'3', '1', '2'}) {
        QByteArray stored_bytes;
        stored_bytes.append('x');
        stored_bytes.append(marker);
        stored_bytes.append(marker);
        auto source = conformanceBuffer(stored_bytes, QIODevice::ReadOnly);
        QVERIFY(source->seek(1));
        const auto created = provider.createIfAbsent(conformanceRemoteId(marker), *source, 2);
        QVERIFY(created);
        QCOMPARE(*created, ProviderCreateResult::Created);
        QCOMPARE(source->pos(), qint64{3});
    }

    const auto first_page = provider.list({}, 2);
    QVERIFY(first_page);
    QCOMPARE(first_page->objects.size(), std::size_t{2});
    QCOMPARE(first_page->objects.at(0), (ProviderObjectMetadata{conformanceRemoteId('1'), 2}));
    QCOMPARE(first_page->objects.at(1), (ProviderObjectMetadata{conformanceRemoteId('2'), 2}));
    QCOMPARE(first_page->continuation_token, conformanceRemoteId('2'));
    const auto second_page = provider.list(first_page->continuation_token, 2);
    QVERIFY(second_page);
    QCOMPARE(second_page->objects.size(), std::size_t{1});
    QCOMPARE(second_page->objects.front(), (ProviderObjectMetadata{conformanceRemoteId('3'), 2}));
    QVERIFY(second_page->continuation_token.isEmpty());

    for (const auto& metadata : first_page->objects) {
        const auto stated = provider.stat(metadata.remote_object_id);
        QVERIFY(stated);
        QCOMPARE(*stated, metadata);
    }
    const auto last_stat = provider.stat(conformanceRemoteId('3'));
    QVERIFY(last_stat);
    QCOMPARE(*last_stat, second_page->objects.front());

    QByteArray replacement("zz");
    auto replacement_source = conformanceBuffer(replacement, QIODevice::ReadOnly);
    const auto duplicate = provider.createIfAbsent(conformanceRemoteId('2'), *replacement_source,
                                                   static_cast<std::uint64_t>(replacement.size()));
    QVERIFY(duplicate);
    QCOMPARE(*duplicate, ProviderCreateResult::AlreadyPresent);
    QCOMPARE(replacement_source->pos(), qint64{0});

    QByteArray downloaded_bytes;
    auto destination = conformanceBuffer(downloaded_bytes, QIODevice::WriteOnly);
    const auto downloaded = provider.download(conformanceRemoteId('2'), *destination);
    QVERIFY(downloaded);
    QCOMPARE(*downloaded, (ProviderObjectMetadata{conformanceRemoteId('2'), 2}));
    QCOMPARE(downloaded_bytes, QByteArray("22"));

    QByteArray oversized(static_cast<qsizetype>(maximum_ciphertext_bytes + 1U), 'z');
    auto oversized_source = conformanceBuffer(oversized, QIODevice::ReadOnly);
    const auto oversized_result = provider.createIfAbsent(
        conformanceRemoteId('4'), *oversized_source, static_cast<std::uint64_t>(oversized.size()));
    QVERIFY(!oversized_result);
    QCOMPARE(oversized_result.error().code, ProviderErrorCode::ObjectTooLarge);
    QCOMPARE(oversized_source->pos(), qint64{0});
    const auto absent_oversized = provider.stat(conformanceRemoteId('4'));
    QVERIFY(!absent_oversized);
    QCOMPARE(absent_oversized.error().code, ProviderErrorCode::NotFound);

    const auto nonregular_id = conformanceRemoteId('5');
    QVERIFY(install_nonregular(nonregular_id));
    const auto nonregular_stat = provider.stat(nonregular_id);
    QVERIFY(!nonregular_stat);
    QCOMPARE(nonregular_stat.error().code, ProviderErrorCode::InvalidObject);
    QByteArray nonregular_bytes;
    auto nonregular_destination = conformanceBuffer(nonregular_bytes, QIODevice::WriteOnly);
    const auto nonregular_download = provider.download(nonregular_id, *nonregular_destination);
    QVERIFY(!nonregular_download);
    QCOMPARE(nonregular_download.error().code, ProviderErrorCode::InvalidObject);
    QVERIFY(nonregular_bytes.isEmpty());
}

} // namespace appellate::sync::test
