#include "appellate/storage/asset_store.hpp"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

namespace {

class AssetStoreTest final : public QObject {
    Q_OBJECT

  private slots:
    void storesAndVerifiesByLowercaseDigest();
    void deduplicatesWithoutReplacingTheObject();
    void enforcesMaximumSizeAndCleansTemporaryFile();
    void rejectsCorruptObjectOnReadAndPut();
    void interruptedTemporaryFileIsNotAddressable();
    void rejectsNonCanonicalDigest();
};

using appellate::storage::AssetStore;
using appellate::storage::AssetStoreErrorCode;

[[nodiscard]] QString digestOf(QByteArrayView bytes) {
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes.toByteArray(), QCryptographicHash::Sha256).toHex());
}

void AssetStoreTest::storesAndVerifiesByLowercaseDigest() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AssetStore store(directory.path(), 1024);

    const QByteArray contents("hello");
    const auto stored = store.put(contents);

    if (!stored) {
        QFAIL(qPrintable(stored.error().message));
    }
    QCOMPARE(stored->sha256,
             QStringLiteral("2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"));
    QCOMPARE(stored->size, qint64{5});
    QVERIFY(!stored->deduplicated);

    const auto loaded = store.read(stored->sha256);
    if (!loaded) {
        QFAIL(qPrintable(loaded.error().message));
    }
    QCOMPARE(*loaded, contents);
}

void AssetStoreTest::deduplicatesWithoutReplacingTheObject() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AssetStore store(directory.path(), 1024);

    const QByteArray contents("same immutable bytes");
    const auto first = store.put(contents);
    QVERIFY(first.has_value());
    const QFileInfo first_file(QDir(store.objectsDirectory()).filePath(first->sha256));
    const auto first_modified = first_file.lastModified();

    const auto second = store.put(contents);
    QVERIFY(second.has_value());
    QCOMPARE(second->sha256, first->sha256);
    QVERIFY(second->deduplicated);
    QCOMPARE(QFileInfo(first_file.filePath()).lastModified(), first_modified);

    const auto object_files =
        QDir(store.objectsDirectory()).entryList(QDir::Files | QDir::NoDotAndDotDot);
    QCOMPARE(object_files, QStringList{first->sha256});
}

void AssetStoreTest::enforcesMaximumSizeAndCleansTemporaryFile() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AssetStore store(directory.path(), 4);

    QByteArray oversized("hello");
    QBuffer source(&oversized);
    QVERIFY(source.open(QIODevice::ReadOnly));
    const auto result = store.put(source);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, AssetStoreErrorCode::AssetTooLarge);
    QVERIFY(QDir(store.objectsDirectory())
                .entryList(QDir::AllEntries | QDir::NoDotAndDotDot)
                .isEmpty());
}

void AssetStoreTest::rejectsCorruptObjectOnReadAndPut() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AssetStore store(directory.path(), 1024);

    const QByteArray original("hello");
    const auto stored = store.put(original);
    QVERIFY(stored.has_value());

    QFile object(QDir(store.objectsDirectory()).filePath(stored->sha256));
    QVERIFY(object.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(object.write("HELLO"), qint64{5});
    object.close();

    const auto loaded = store.read(stored->sha256);
    QVERIFY(!loaded.has_value());
    QCOMPARE(loaded.error().code, AssetStoreErrorCode::CorruptObject);

    const auto duplicate = store.put(original);
    QVERIFY(!duplicate.has_value());
    QCOMPARE(duplicate.error().code, AssetStoreErrorCode::CorruptObject);

    QVERIFY(object.open(QIODevice::ReadOnly));
    QCOMPARE(object.readAll(), QByteArray("HELLO"));
}

void AssetStoreTest::interruptedTemporaryFileIsNotAddressable() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AssetStore store(directory.path(), 1024);
    QVERIFY(QDir().mkpath(store.objectsDirectory()));

    const QByteArray contents("interrupted");
    QFile temporary(
        QDir(store.objectsDirectory()).filePath(QStringLiteral(".asset-interrupted.tmp")));
    QVERIFY(temporary.open(QIODevice::WriteOnly));
    QCOMPARE(temporary.write(contents), static_cast<qint64>(contents.size()));
    temporary.close();

    const auto before_put = store.read(digestOf(contents));
    QVERIFY(!before_put.has_value());
    QCOMPARE(before_put.error().code, AssetStoreErrorCode::NotFound);

    const auto stored = store.put(contents);
    QVERIFY(stored.has_value());
    QVERIFY(!stored->deduplicated);
    QVERIFY(temporary.exists());
}

void AssetStoreTest::rejectsNonCanonicalDigest() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AssetStore store(directory.path(), 1024);

    const auto result = store.read(
        QStringLiteral("2CF24DBA5FB0A30E26E83B2AC5B9E29E1B161E5C1FA7425E73043362938B9824"));
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, AssetStoreErrorCode::InvalidDigest);
}

} // namespace

QTEST_GUILESS_MAIN(AssetStoreTest)

#include "tst_asset_store.moc"
