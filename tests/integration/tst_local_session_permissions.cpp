#include "local_session_provider.hpp"

#include "appellate/storage/asset_store.hpp"
#include "appellate/storage/detail/private_state.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <cstdint>

#if defined(Q_OS_LINUX)
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#endif

namespace {

#if defined(Q_OS_LINUX)
class ScopedUmask final {
  public:
    explicit ScopedUmask(mode_t value) noexcept : previous_(::umask(value)) {}
    ScopedUmask(const ScopedUmask&) = delete;
    ScopedUmask& operator=(const ScopedUmask&) = delete;
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

void appendLittleEndian16(QByteArray& bytes, std::uint16_t value) {
    bytes.append(static_cast<char>(value & 0xffU));
    bytes.append(static_cast<char>((value >> 8U) & 0xffU));
}

void appendLittleEndian32(QByteArray& bytes, std::uint32_t value) {
    for (unsigned int shift = 0; shift < 32U; shift += 8U) {
        bytes.append(static_cast<char>((value >> shift) & 0xffU));
    }
}

[[nodiscard]] QByteArray namedUserAcl(uid_t user_id) {
    constexpr std::uint32_t undefined_id = 0xffffffffU;
    QByteArray bytes;
    appendLittleEndian32(bytes, 0x0002U);
    const auto append_entry = [&](std::uint16_t tag, std::uint16_t permissions, std::uint32_t id) {
        appendLittleEndian16(bytes, tag);
        appendLittleEndian16(bytes, permissions);
        appendLittleEndian32(bytes, id);
    };
    append_entry(0x01, 7, undefined_id);
    append_entry(0x02, 4, static_cast<std::uint32_t>(user_id));
    append_entry(0x04, 0, undefined_id);
    append_entry(0x10, 4, undefined_id);
    append_entry(0x20, 0, undefined_id);
    return bytes;
}
#endif

class LocalSessionPermissionsTest final : public QObject {
    Q_OBJECT

  private slots:
    void hostileUmaskCreatesExactPrivateTree_data();
    void hostileUmaskCreatesExactPrivateTree();
    void rejectsUnsafeExistingDirectoriesWithoutRepair();
    void rejectsUnsafeExistingDatabaseWithoutArtifacts();
    void rejectsPreexistingAclWithoutMutation();
    void failedNestedCreationDetachesIdentityProvenResidue();
    void interruptedDirectoryStageDoesNotBrickRetry();
    void interruptedAssetObjectsStageDoesNotBrickRetry();
};

void LocalSessionPermissionsTest::hostileUmaskCreatesExactPrivateTree_data() {
    QTest::addColumn<unsigned int>("mask");
    QTest::newRow("fully-permissive") << 0000U;
    QTest::newRow("fully-restrictive") << 0777U;
}

void LocalSessionPermissionsTest::hostileUmaskCreatesExactPrivateTree() {
#if !defined(Q_OS_LINUX)
    QSKIP("Exact local-session permission enforcement is Linux-only");
#else
    QFETCH(unsigned int, mask);
    QTemporaryDir controller;
    QVERIFY(controller.isValid());
    const auto state = QDir(controller.path()).filePath(QStringLiteral("app-state"));
    const auto sessions = QDir(state).filePath(QStringLiteral("sessions"));
    const auto database = QDir(sessions).filePath(QStringLiteral("sessions.sqlite"));
    const auto assets = QDir(sessions).filePath(QStringLiteral("assets"));
    {
        const ScopedUmask hostile(static_cast<mode_t>(mask));
        auto provider = appellate::ui::LocalSessionProvider::create({database, assets});
        if (!provider) {
            QFAIL(qPrintable(provider.error()));
        }

        QCOMPARE(exactMode(state), static_cast<mode_t>(0700));
        QCOMPARE(exactMode(sessions), static_cast<mode_t>(0700));
        QCOMPARE(exactMode(assets), static_cast<mode_t>(0700));
        QCOMPARE(exactMode(QDir(assets).filePath(QStringLiteral("objects"))),
                 static_cast<mode_t>(0700));
        QCOMPARE(exactMode(database), static_cast<mode_t>(0600));
        QCOMPARE(exactMode(QDir(sessions).filePath(
                     QStringLiteral(".sessions.sqlite.appellate-open.lock"))),
                 static_cast<mode_t>(0600));
        QCOMPARE(exactMode(QDir(assets).filePath(QStringLiteral(".cas.lock"))),
                 static_cast<mode_t>(0600));
        QCOMPARE(exactMode(QDir(assets).filePath(QStringLiteral(".appellate-store-id"))),
                 static_cast<mode_t>(0600));

        const auto wal = database + QStringLiteral("-wal");
        const auto shm = database + QStringLiteral("-shm");
        QVERIFY(QFileInfo::exists(wal));
        QVERIFY(QFileInfo::exists(shm));
        QCOMPARE(exactMode(wal), static_cast<mode_t>(0600));
        QCOMPARE(exactMode(shm), static_cast<mode_t>(0600));
    }
#endif
}

void LocalSessionPermissionsTest::rejectsUnsafeExistingDirectoriesWithoutRepair() {
#if !defined(Q_OS_LINUX)
    QSKIP("Exact local-session permission enforcement is Linux-only");
#else
    QTemporaryDir controller;
    QVERIFY(controller.isValid());
    const auto sessions = QDir(controller.path()).filePath(QStringLiteral("sessions"));
    const auto assets = QDir(sessions).filePath(QStringLiteral("assets"));
    const auto database = QDir(sessions).filePath(QStringLiteral("sessions.sqlite"));
    QVERIFY(QDir{}.mkpath(assets));
    QVERIFY(chmodExact(sessions, 0700));
    QVERIFY(chmodExact(assets, 0755));

    const auto before_entries = QDir(sessions).entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
    const auto opened = appellate::ui::LocalSessionProvider::create({database, assets});
    QVERIFY(!opened.has_value());
    QCOMPARE(exactMode(assets), static_cast<mode_t>(0755));
    QCOMPARE(QDir(sessions).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), before_entries);
    QVERIFY(!QFileInfo::exists(database));

    QVERIFY(chmodExact(assets, 0700));
    QVERIFY(chmodExact(sessions, 0777));
    const auto second = appellate::ui::LocalSessionProvider::create({database, assets});
    QVERIFY(!second.has_value());
    QCOMPARE(exactMode(sessions), static_cast<mode_t>(0777));
    QVERIFY(!QFileInfo::exists(database));
    QVERIFY(chmodExact(sessions, 0700));

    const auto missing_state = QDir(controller.path()).filePath(QStringLiteral("missing-state"));
    const auto disjoint_database =
        QDir(missing_state).filePath(QStringLiteral("sessions/sessions.sqlite"));
    const auto unsafe_assets = QDir(controller.path()).filePath(QStringLiteral("unsafe-assets"));
    QVERIFY(QDir{}.mkpath(unsafe_assets));
    QVERIFY(chmodExact(unsafe_assets, 0755));
    const auto disjoint =
        appellate::ui::LocalSessionProvider::create({disjoint_database, unsafe_assets});
    QVERIFY(!disjoint.has_value());
    QVERIFY(!QFileInfo::exists(missing_state));
    QCOMPARE(exactMode(unsafe_assets), static_cast<mode_t>(0755));
#endif
}

void LocalSessionPermissionsTest::rejectsUnsafeExistingDatabaseWithoutArtifacts() {
#if !defined(Q_OS_LINUX)
    QSKIP("Exact local-session permission enforcement is Linux-only");
#else
    QTemporaryDir controller;
    QVERIFY(controller.isValid());
    const auto sessions = QDir(controller.path()).filePath(QStringLiteral("sessions"));
    const auto assets = QDir(sessions).filePath(QStringLiteral("assets"));
    const auto database = QDir(sessions).filePath(QStringLiteral("sessions.sqlite"));
    QVERIFY(QDir{}.mkpath(assets));
    QVERIFY(chmodExact(sessions, 0700));
    QVERIFY(chmodExact(assets, 0700));
    QFile hostile(database);
    QVERIFY(hostile.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(hostile.write("permission-sentinel"), qint64{19});
    hostile.close();
    QVERIFY(chmodExact(database, 0644));
    const auto before = [&] {
        QFile file(database);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    }();

    const auto opened = appellate::ui::LocalSessionProvider::create({database, assets});
    QVERIFY(!opened.has_value());
    QCOMPARE(exactMode(database), static_cast<mode_t>(0644));
    QFile after(database);
    QVERIFY(after.open(QIODevice::ReadOnly));
    QCOMPARE(after.readAll(), before);
    QVERIFY(!QFileInfo::exists(
        QDir(sessions).filePath(QStringLiteral(".sessions.sqlite.appellate-open.lock"))));
#endif
}

void LocalSessionPermissionsTest::rejectsPreexistingAclWithoutMutation() {
#if !defined(Q_OS_LINUX)
    QSKIP("Exact local-session ACL enforcement is Linux-only");
#else
    QTemporaryDir controller;
    QVERIFY(controller.isValid());
    const auto sessions = QDir(controller.path()).filePath(QStringLiteral("sessions"));
    const auto assets = QDir(sessions).filePath(QStringLiteral("assets"));
    const auto database = QDir(sessions).filePath(QStringLiteral("sessions.sqlite"));
    QVERIFY(QDir{}.mkpath(sessions));
    QVERIFY(chmodExact(sessions, 0700));
    const auto encoded_sessions = QFile::encodeName(sessions);
    const auto acl = namedUserAcl(::geteuid() == 65534 ? 65533 : 65534);
    QVERIFY(::setxattr(encoded_sessions.constData(), "system.posix_acl_default", acl.constData(),
                       static_cast<std::size_t>(acl.size()), 0) == 0);
    const auto before_entries = QDir(sessions).entryList(QDir::AllEntries | QDir::NoDotAndDotDot);

    const auto opened = appellate::ui::LocalSessionProvider::create({database, assets});
    QVERIFY(!opened.has_value());
    QCOMPARE(exactMode(sessions), static_cast<mode_t>(0700));
    QCOMPARE(QDir(sessions).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), before_entries);
    QVERIFY(!QFileInfo::exists(database));
    QVERIFY(!QFileInfo::exists(assets));
    QVERIFY(::removexattr(encoded_sessions.constData(), "system.posix_acl_default") == 0);
#endif
}

void LocalSessionPermissionsTest::failedNestedCreationDetachesIdentityProvenResidue() {
#if !defined(Q_OS_LINUX)
    QSKIP("Exact local-session rollback enforcement is Linux-only");
#else
    QTemporaryDir controller;
    QVERIFY(controller.isValid());
    const auto fresh = QDir(controller.path()).filePath(QStringLiteral("fresh-private"));
    const auto impossible = QDir(fresh).filePath(QString(1024, QLatin1Char('x')));
    const auto created = appellate::storage::detail::ensurePrivateStateDirectory(impossible, fresh);
    QVERIFY(!created.has_value());
    QVERIFY(!QFileInfo::exists(fresh));
    const auto quarantines = QDir(controller.path())
                                 .entryList({QStringLiteral(".appellate-quarantine-*.tmp")},
                                            QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    QCOMPARE(quarantines.size(), 1);
    QCOMPARE(exactMode(QDir(controller.path()).filePath(quarantines.front())),
             static_cast<mode_t>(0700));
    const auto retried = appellate::storage::detail::ensurePrivateStateDirectory(fresh, fresh);
    QVERIFY(retried.has_value());
    static_cast<void>(::close(*retried));
    QCOMPARE(exactMode(fresh), static_cast<mode_t>(0700));
#endif
}

void LocalSessionPermissionsTest::interruptedDirectoryStageDoesNotBrickRetry() {
#if !defined(Q_OS_LINUX)
    QSKIP("Crash-safe private directory publication is Linux-only");
#else
    QTemporaryDir controller;
    QVERIFY(controller.isValid());
    const auto staged = QDir(controller.path())
                            .filePath(QStringLiteral(".appellate-directory-stage-"
                                                     "0123456789abcdef0123456789abcdef.tmp"));
    const auto requested = QDir(controller.path()).filePath(QStringLiteral("requested-private"));
    QVERIFY(::mkdir(QFile::encodeName(staged).constData(), 0000) == 0);

    const auto retried =
        appellate::storage::detail::ensurePrivateStateDirectory(requested, requested);
    QVERIFY(retried.has_value());
    static_cast<void>(::close(*retried));
    QCOMPARE(exactMode(requested), static_cast<mode_t>(0700));
    QCOMPARE(exactMode(staged), static_cast<mode_t>(0000));

    const auto unknown = QDir(controller.path()).filePath(QStringLiteral("unknown-preexisting"));
    QVERIFY(::mkdir(QFile::encodeName(unknown).constData(), 0000) == 0);
    const auto rejected = appellate::storage::detail::ensurePrivateStateDirectory(unknown, unknown);
    QVERIFY(!rejected.has_value());
    QCOMPARE(exactMode(unknown), static_cast<mode_t>(0000));
    QVERIFY(chmodExact(staged, 0700));
    QVERIFY(chmodExact(unknown, 0700));
#endif
}

void LocalSessionPermissionsTest::interruptedAssetObjectsStageDoesNotBrickRetry() {
#if !defined(Q_OS_LINUX)
    QSKIP("Crash-safe asset directory publication is Linux-only");
#else
    QTemporaryDir controller;
    QVERIFY(controller.isValid());
    const auto root = QDir(controller.path()).filePath(QStringLiteral("interrupted-assets"));
    QVERIFY(QDir{}.mkdir(root));
    QVERIFY(chmodExact(root, 0700));
    const auto staged = QDir(root).filePath(QStringLiteral(".appellate-directory-stage-"
                                                           "fedcba9876543210fedcba9876543210.tmp"));
    QVERIFY(::mkdir(QFile::encodeName(staged).constData(), 0000) == 0);

    appellate::storage::AssetStore assets(root, 1024);
    const auto stored = assets.put(QByteArrayLiteral("retry-after-crash"));
    if (!stored) {
        QFAIL(qPrintable(stored.error().message));
    }
    QCOMPARE(exactMode(QDir(root).filePath(QStringLiteral("objects"))), static_cast<mode_t>(0700));
    QCOMPARE(exactMode(staged), static_cast<mode_t>(0000));
    QVERIFY(chmodExact(staged, 0700));
#endif
}

} // namespace

QTEST_GUILESS_MAIN(LocalSessionPermissionsTest)

#include "tst_local_session_permissions.moc"
