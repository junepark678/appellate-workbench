#include "appellate/storage/asset_store.hpp"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <cerrno>
#include <utility>

#if defined(Q_OS_UNIX)
#include <unistd.h>
#endif

#if defined(Q_OS_LINUX)
#include <sys/stat.h>
#include <sys/wait.h>
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
    void firstLockPublicationSurvivesCreatorCrash();
    void preservesUnsafeLegacyLockCrashResidue();
    void createsExactPrivateLayoutUnderHostileUmask_data();
    void createsExactPrivateLayoutUnderHostileUmask();
    void rejectsUnsafePreexistingModesWithoutRepair();
    void rejectsFifoObjectWithoutBlocking();
};

using appellate::storage::AssetStore;
using appellate::storage::AssetStoreErrorCode;
using appellate::storage::detail::AssetStoreLockHooks;

#if defined(Q_OS_LINUX)
class ScopedUmask final {
  public:
    explicit ScopedUmask(mode_t value) noexcept : previous_(::umask(value)) {}
    ~ScopedUmask() { static_cast<void>(::umask(previous_)); }

  private:
    mode_t previous_{};
};

[[nodiscard]] mode_t exactMode(const QString& path) {
    struct stat status{};
    return ::lstat(QFile::encodeName(path).constData(), &status) == 0
               ? static_cast<mode_t>(status.st_mode & 07777)
               : static_cast<mode_t>(~0U);
}

[[nodiscard]] bool chmodExact(const QString& path, mode_t mode) {
    return ::chmod(QFile::encodeName(path).constData(), mode) == 0;
}
#endif

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
    QVERIFY(QFile::setPermissions(store.objectsDirectory(), QFileDevice::ReadOwner |
                                                                QFileDevice::WriteOwner |
                                                                QFileDevice::ExeOwner));

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
    QVERIFY(QFile::setPermissions(root, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                            QFileDevice::ExeOwner));
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

void AssetStoreTest::firstLockPublicationSurvivesCreatorCrash() {
#if !defined(Q_OS_LINUX)
    QSKIP("Atomic CAS lock publication is Linux-only");
#else
    QTemporaryDir controller;
    QVERIFY(controller.isValid());
    const auto root = QDir(controller.path()).filePath(QStringLiteral("assets"));
    const auto lock_path = QDir(root).filePath(QStringLiteral(".cas.lock"));

    const auto prepublish_child = ::fork();
    QVERIFY(prepublish_child >= 0);
    if (prepublish_child == 0) {
        static_cast<void>(::umask(0777));
        AssetStore store(root, 1024);
        AssetStoreLockHooks hooks;
        hooks.after_anonymous_create = [] { ::_exit(0); };
        static_cast<void>(store.acquireLock(hooks));
        ::_exit(10);
    }

    int child_status{};
    pid_t waited{};
    do {
        waited = ::waitpid(prepublish_child, &child_status, 0);
    } while (waited < 0 && errno == EINTR);
    QCOMPARE(waited, prepublish_child);
    QVERIFY(WIFEXITED(child_status));
    QCOMPARE(WEXITSTATUS(child_status), 0);
    QVERIFY(!QFileInfo::exists(lock_path));

    const auto child = ::fork();
    QVERIFY(child >= 0);
    if (child == 0) {
        static_cast<void>(::umask(0777));
        AssetStore store(root, 1024);
        const auto lock = store.acquireLock();
        if (!lock) {
            ::_exit(10);
        }
        struct stat published{};
        if (::lstat(QFile::encodeName(lock_path).constData(), &published) != 0) {
            ::_exit(11);
        }
        if (!S_ISREG(published.st_mode) || (published.st_mode & 07777) != 0600 ||
            published.st_nlink != 1 || published.st_size != 0) {
            ::_exit(12);
        }
        // Deliberately bypass every C++ destructor. The kernel releases flock ownership, while the
        // exact published name must remain as the durable cooperative-lock rendezvous point.
        ::_exit(0);
    }

    child_status = 0;
    waited = 0;
    do {
        waited = ::waitpid(child, &child_status, 0);
    } while (waited < 0 && errno == EINTR);
    QCOMPARE(waited, child);
    QVERIFY(WIFEXITED(child_status));
    QCOMPARE(WEXITSTATUS(child_status), 0);

    struct stat before{};
    QCOMPARE(::lstat(QFile::encodeName(lock_path).constData(), &before), 0);
    QVERIFY(S_ISREG(before.st_mode));
    QCOMPARE(static_cast<mode_t>(before.st_mode & 07777), static_cast<mode_t>(0600));
    QCOMPARE(before.st_nlink, static_cast<nlink_t>(1));
    QCOMPARE(before.st_size, static_cast<off_t>(0));

    AssetStore reopened(root, 1024);
    const auto reacquired = reopened.acquireLock();
    if (!reacquired) {
        QFAIL(qPrintable(reacquired.error().message));
    }
    struct stat after{};
    QCOMPARE(::lstat(QFile::encodeName(lock_path).constData(), &after), 0);
    QCOMPARE(after.st_dev, before.st_dev);
    QCOMPARE(after.st_ino, before.st_ino);
    QCOMPARE(static_cast<mode_t>(after.st_mode & 07777), static_cast<mode_t>(0600));
    QCOMPARE(after.st_nlink, static_cast<nlink_t>(1));
#endif
}

void AssetStoreTest::preservesUnsafeLegacyLockCrashResidue() {
#if !defined(Q_OS_LINUX)
    QSKIP("Exact CAS lock validation is Linux-only");
#else
    QTemporaryDir controller;
    QVERIFY(controller.isValid());
    const auto root = QDir(controller.path()).filePath(QStringLiteral("assets"));
    AssetStore store(root, 1024);
    {
        const auto staged = store.stage(QByteArrayLiteral("initialize-without-lock"));
        QVERIFY(staged.has_value());
    }

    const auto lock_path = QDir(root).filePath(QStringLiteral(".cas.lock"));
    QFile residue(lock_path);
    QVERIFY(residue.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    residue.close();
    QVERIFY(chmodExact(lock_path, 0000));
    struct stat before{};
    QCOMPARE(::lstat(QFile::encodeName(lock_path).constData(), &before), 0);

    const auto rejected = store.acquireLock();
    QVERIFY(!rejected.has_value());
    struct stat after{};
    QCOMPARE(::lstat(QFile::encodeName(lock_path).constData(), &after), 0);
    QCOMPARE(after.st_dev, before.st_dev);
    QCOMPARE(after.st_ino, before.st_ino);
    QCOMPARE(static_cast<mode_t>(after.st_mode & 07777), static_cast<mode_t>(0000));
    QCOMPARE(after.st_nlink, static_cast<nlink_t>(1));
#endif
}

void AssetStoreTest::createsExactPrivateLayoutUnderHostileUmask_data() {
    QTest::addColumn<unsigned int>("mask");
    QTest::newRow("fully-permissive") << 0000U;
    QTest::newRow("fully-restrictive") << 0777U;
}

void AssetStoreTest::createsExactPrivateLayoutUnderHostileUmask() {
#if !defined(Q_OS_LINUX)
    QSKIP("Exact CAS permission enforcement is Linux-only");
#else
    QFETCH(unsigned int, mask);
    QTemporaryDir controller;
    QVERIFY(controller.isValid());
    const auto root = QDir(controller.path()).filePath(QStringLiteral("assets"));
    const QByteArray contents("private-cas-object");
    {
        const ScopedUmask hostile(static_cast<mode_t>(mask));
        AssetStore store(root, 1024);
        const auto stored = store.put(contents);
        if (!stored) {
            QFAIL(qPrintable(stored.error().message));
        }
        auto lock = store.acquireLock();
        QVERIFY(lock.has_value());

        QCOMPARE(exactMode(root), static_cast<mode_t>(0700));
        QCOMPARE(exactMode(store.objectsDirectory()), static_cast<mode_t>(0700));
        QCOMPARE(exactMode(QDir(root).filePath(QStringLiteral(".cas.lock"))),
                 static_cast<mode_t>(0600));
        QCOMPARE(exactMode(QDir(store.objectsDirectory()).filePath(stored->sha256)),
                 static_cast<mode_t>(0600));
    }
#endif
}

void AssetStoreTest::rejectsUnsafePreexistingModesWithoutRepair() {
#if !defined(Q_OS_LINUX)
    QSKIP("Exact CAS permission enforcement is Linux-only");
#else
    {
        QTemporaryDir controller;
        QVERIFY(controller.isValid());
        const auto root = QDir(controller.path()).filePath(QStringLiteral("assets"));
        QVERIFY(QDir{}.mkpath(root));
        QVERIFY(chmodExact(root, 0755));
        AssetStore store(root, 1024);
        const auto result = store.put(QByteArrayLiteral("must-not-write"));
        QVERIFY(!result.has_value());
        QCOMPARE(exactMode(root), static_cast<mode_t>(0755));
        QVERIFY(QDir(root).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    }
    {
        QTemporaryDir controller;
        QVERIFY(controller.isValid());
        AssetStore store(QDir(controller.path()).filePath(QStringLiteral("assets")), 1024);
        const QByteArray bytes("permission-bound-object");
        const auto stored = store.put(bytes);
        QVERIFY(stored.has_value());
        const auto object_path = QDir(store.objectsDirectory()).filePath(stored->sha256);
        QVERIFY(chmodExact(object_path, 0644));
        const auto before = [&] {
            QFile object(object_path);
            return object.open(QIODevice::ReadOnly) ? object.readAll() : QByteArray{};
        }();
        const auto loaded = store.read(stored->sha256);
        QVERIFY(!loaded.has_value());
        QCOMPARE(loaded.error().code, AssetStoreErrorCode::CorruptObject);
        QCOMPARE(exactMode(object_path), static_cast<mode_t>(0644));
        QFile after(object_path);
        QVERIFY(after.open(QIODevice::ReadOnly));
        QCOMPARE(after.readAll(), before);
    }
    {
        QTemporaryDir controller;
        QVERIFY(controller.isValid());
        const auto root = QDir(controller.path()).filePath(QStringLiteral("assets"));
        AssetStore store(root, 1024);
        {
            auto first = store.acquireLock();
            QVERIFY(first.has_value());
        }
        const auto lock_path = QDir(root).filePath(QStringLiteral(".cas.lock"));
        QVERIFY(chmodExact(lock_path, 0644));
        const auto second = store.acquireLock();
        QVERIFY(!second.has_value());
        QCOMPARE(exactMode(lock_path), static_cast<mode_t>(0644));
    }
    {
        QTemporaryDir controller;
        QVERIFY(controller.isValid());
        const auto root = QDir(controller.path()).filePath(QStringLiteral("assets"));
        AssetStore store(root, 1024);
        const auto stored = store.put(QByteArrayLiteral("retained-private-object"));
        QVERIFY(stored.has_value());
        const auto before_entries =
            QDir(store.objectsDirectory()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
        QVERIFY(chmodExact(store.objectsDirectory(), 0755));
        const auto rejected_objects = store.put(QByteArrayLiteral("must-not-publish"));
        QVERIFY(!rejected_objects.has_value());
        QCOMPARE(rejected_objects.error().code, AssetStoreErrorCode::InvalidConfiguration);
        QCOMPARE(exactMode(store.objectsDirectory()), static_cast<mode_t>(0755));
        QCOMPARE(QDir(store.objectsDirectory()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot),
                 before_entries);
        QVERIFY(chmodExact(store.objectsDirectory(), 0700));
        QVERIFY(chmodExact(root, 0755));
        const auto rejected_root = store.read(stored->sha256);
        QVERIFY(!rejected_root.has_value());
        QCOMPARE(rejected_root.error().code, AssetStoreErrorCode::InvalidConfiguration);
        QCOMPARE(exactMode(root), static_cast<mode_t>(0755));
        QVERIFY(chmodExact(root, 0700));
    }
#endif
}

void AssetStoreTest::rejectsFifoObjectWithoutBlocking() {
#if !defined(Q_OS_LINUX)
    QSKIP("FIFO rejection is Linux-only");
#else
    QTemporaryDir controller;
    QVERIFY(controller.isValid());
    const auto root = QDir(controller.path()).filePath(QStringLiteral("assets"));
    AssetStore store(root, 1024);
    QVERIFY(store.put(QByteArrayLiteral("seed-object")).has_value());
    const auto fifo_digest = QString(64, QLatin1Char('a'));
    const auto fifo_path = QDir(store.objectsDirectory()).filePath(fifo_digest);
    QVERIFY(::mkfifo(QFile::encodeName(fifo_path).constData(), 0600) == 0);
    QVERIFY(chmodExact(fifo_path, 0600));

    QElapsedTimer elapsed;
    elapsed.start();
    const auto rejected = store.read(fifo_digest);
    QVERIFY(!rejected.has_value());
    QVERIFY2(elapsed.elapsed() < 1'000, "FIFO validation blocked unexpectedly");
    QCOMPARE(rejected.error().code, AssetStoreErrorCode::CorruptObject);
    QCOMPARE(exactMode(fifo_path), static_cast<mode_t>(0600));
#endif
}

} // namespace

QTEST_GUILESS_MAIN(AssetStoreTest)

#include "tst_asset_store.moc"
