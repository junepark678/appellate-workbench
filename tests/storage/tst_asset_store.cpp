#include "appellate/storage/asset_store.hpp"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <utility>

#if defined(Q_OS_UNIX)
#include <unistd.h>
#endif

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
    void rejectsObjectsDirectorySymlink();
    void retainedDirectoryHandlesDefeatPathSwap();
    void lockMoveAssignmentTransfersOwnershipSafely();
    void rejectsHardLinkedFinalObjectWithoutChangingAlias();
    void rejectsHardLinkedPublicationLockWithoutChangingAlias();
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

void AssetStoreTest::rejectsObjectsDirectorySymlink() {
#if !defined(Q_OS_UNIX)
    QSKIP("No-follow symlink boundary is Unix-only");
#else
    QTemporaryDir directory;
    QTemporaryDir outside;
    QVERIFY(directory.isValid());
    QVERIFY(outside.isValid());
    QVERIFY(QFile::link(outside.path(),
                       QDir(directory.path()).filePath(QStringLiteral("objects"))));

    AssetStore store(directory.path(), 1024);
    const auto result = store.put(QByteArrayLiteral("must-not-escape"));
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, AssetStoreErrorCode::InvalidConfiguration);
    QVERIFY(QDir(outside.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
#endif
}

void AssetStoreTest::retainedDirectoryHandlesDefeatPathSwap() {
#if !defined(Q_OS_UNIX)
    QSKIP("Descriptor-retention swap test is Unix-only");
#else
    QTemporaryDir parent;
    QTemporaryDir outside;
    QVERIFY(parent.isValid());
    QVERIFY(outside.isValid());
    const auto root = QDir(parent.path()).filePath(QStringLiteral("cas"));
    const auto retained_root = QDir(parent.path()).filePath(QStringLiteral("cas-retained"));
    QVERIFY(QDir{}.mkpath(root));
    AssetStore store(root, 1024);
    const auto first = store.put(QByteArrayLiteral("first"));
    QVERIFY(first.has_value());

    QVERIFY(QDir{}.rename(root, retained_root));
    QVERIFY(QDir{}.mkpath(root));
    QVERIFY(QFile::link(outside.path(), QDir(root).filePath(QStringLiteral("objects"))));

    const auto second = store.put(QByteArrayLiteral("second"));
    QVERIFY(second.has_value());
    const auto loaded = store.read(second->sha256);
    QVERIFY(loaded.has_value());
    QCOMPARE(*loaded, QByteArrayLiteral("second"));
    QVERIFY(QFileInfo::exists(
        QDir(QDir(retained_root).filePath(QStringLiteral("objects"))).filePath(second->sha256)));
    QVERIFY(QDir(outside.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
#endif
}

void AssetStoreTest::lockMoveAssignmentTransfersOwnershipSafely() {
    QTemporaryDir first_directory;
    QTemporaryDir second_directory;
    QVERIFY(first_directory.isValid());
    QVERIFY(second_directory.isValid());
    AssetStore first(first_directory.path(), 1024);
    AssetStore second(second_directory.path(), 1024);
    auto first_lock = first.acquireLock();
    auto second_lock = second.acquireLock();
    QVERIFY(first_lock.has_value());
    QVERIFY(second_lock.has_value());

    *first_lock = std::move(*second_lock);
    auto staged = second.stage(QByteArrayLiteral("move-assigned-lock"));
    QVERIFY(staged.has_value());
    const auto finalized = second.finalize(*staged, *first_lock);
    QVERIFY(finalized.has_value());
    const auto loaded = second.read(finalized->sha256);
    QVERIFY(loaded.has_value());
    QCOMPARE(*loaded, QByteArrayLiteral("move-assigned-lock"));

    // The replaced ownership was released, so the first store can be locked again.
    const auto reacquired = first.acquireLock();
    QVERIFY(reacquired.has_value());
}

void AssetStoreTest::rejectsHardLinkedFinalObjectWithoutChangingAlias() {
#if !defined(Q_OS_UNIX)
    QSKIP("Hard-link object boundary is Unix-only");
#else
    QTemporaryDir directory;
    QTemporaryDir outside;
    QVERIFY(directory.isValid());
    QVERIFY(outside.isValid());
    AssetStore store(directory.path(), 1024);
    const auto stored = store.put(QByteArrayLiteral("immutable-object"));
    QVERIFY(stored.has_value());
    const auto object_path = QDir(store.objectsDirectory()).filePath(stored->sha256);
    const auto alias_path = outside.filePath(QStringLiteral("outside-alias"));
    QCOMPARE(::link(QFile::encodeName(object_path).constData(),
                    QFile::encodeName(alias_path).constData()),
             0);
    QFile alias(alias_path);
    QVERIFY(alias.open(QIODevice::ReadOnly));
    const auto before = alias.readAll();
    alias.close();

    const auto rejected_read = store.read(stored->sha256);
    QVERIFY(!rejected_read.has_value());
    QCOMPARE(rejected_read.error().code, AssetStoreErrorCode::CorruptObject);
    const auto rejected_put = store.put(QByteArrayLiteral("immutable-object"));
    QVERIFY(!rejected_put.has_value());
    QCOMPARE(rejected_put.error().code, AssetStoreErrorCode::CorruptObject);

    QVERIFY(alias.open(QIODevice::ReadOnly));
    QCOMPARE(alias.readAll(), before);
    alias.close();
    QVERIFY(QFileInfo::exists(object_path));
#endif
}

void AssetStoreTest::rejectsHardLinkedPublicationLockWithoutChangingAlias() {
#if !defined(Q_OS_UNIX)
    QSKIP("Hard-link lock boundary is Unix-only");
#else
    QTemporaryDir directory;
    QTemporaryDir outside;
    QVERIFY(directory.isValid());
    QVERIFY(outside.isValid());
    AssetStore store(directory.path(), 1024);
    QVERIFY(store.put(QByteArrayLiteral("publish-lock-fixture")).has_value());
    const auto lock_path = directory.filePath(QStringLiteral(".cas.lock"));
    const auto alias_path = outside.filePath(QStringLiteral("outside-lock-alias"));
    QCOMPARE(::link(QFile::encodeName(lock_path).constData(),
                    QFile::encodeName(alias_path).constData()),
             0);
    QFile alias(alias_path);
    QVERIFY(alias.open(QIODevice::ReadOnly));
    const auto before = alias.readAll();
    alias.close();

    const auto rejected = store.acquireLock();
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, AssetStoreErrorCode::InvalidConfiguration);
    QVERIFY(alias.open(QIODevice::ReadOnly));
    QCOMPARE(alias.readAll(), before);
    alias.close();
#endif
}

} // namespace

QTEST_GUILESS_MAIN(AssetStoreTest)

#include "tst_asset_store.moc"
