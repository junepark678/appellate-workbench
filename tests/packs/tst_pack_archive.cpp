#include "appellate/packs/pack_archive.hpp"
#include "pack_archive_p.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <QByteArray>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <type_traits>
#if defined(Q_OS_LINUX)
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>
#endif
#include <utility>
#include <vector>

namespace {

static_assert(!std::is_copy_constructible_v<appellate::packs::detail::SecureScratchContext>);
static_assert(!std::is_copy_assignable_v<appellate::packs::detail::SecureScratchContext>);
static_assert(std::is_move_constructible_v<appellate::packs::detail::SecureScratchContext>);
static_assert(std::is_move_assignable_v<appellate::packs::detail::SecureScratchContext>);

struct ArchiveWriteDeleter final {
    void operator()(archive* value) const noexcept {
        if (value != nullptr) {
            archive_write_free(value);
        }
    }
};

struct ArchiveEntryDeleter final {
    void operator()(archive_entry* value) const noexcept {
        if (value != nullptr) {
            archive_entry_free(value);
        }
    }
};

using ArchiveWrite = std::unique_ptr<archive, ArchiveWriteDeleter>;
using ArchiveEntry = std::unique_ptr<archive_entry, ArchiveEntryDeleter>;

struct TestEntry final {
    QString path;
    QByteArray data;
    unsigned int file_type{};
    QString link_target;

    TestEntry(QString entry_path, QByteArray entry_data,
              unsigned int entry_file_type = static_cast<unsigned int>(AE_IFREG),
              QString entry_link_target = {})
        : path(std::move(entry_path)), data(std::move(entry_data)), file_type(entry_file_type),
          link_target(std::move(entry_link_target)) {}
};

enum class TestCompression {
    Store,
    Deflate,
};

void refreshProcessTimeZone() {
#if defined(Q_OS_WIN)
    _tzset();
#else
    tzset();
#endif
}

class ScopedTimeZone final {
  public:
    ScopedTimeZone() : was_set_(qEnvironmentVariableIsSet("TZ")), original_(qgetenv("TZ")) {}

    ~ScopedTimeZone() {
        if (was_set_) {
            qputenv("TZ", original_);
        } else {
            qunsetenv("TZ");
        }
        refreshProcessTimeZone();
    }

    void set(const QByteArray& value) {
        qputenv("TZ", value);
        refreshProcessTimeZone();
    }

  private:
    bool was_set_{};
    QByteArray original_;
};

#if defined(Q_OS_LINUX)
class ScopedAmbientState final {
  public:
    ScopedAmbientState()
        : current_directory_(QDir::currentPath()),
          variables_{
              Variable{QByteArrayLiteral("TMPDIR"), qgetenv("TMPDIR"), false},
              Variable{QByteArrayLiteral("TMP"), qgetenv("TMP"), false},
              Variable{QByteArrayLiteral("TEMP"), qgetenv("TEMP"), false},
              Variable{QByteArrayLiteral("SQLITE_TMPDIR"), qgetenv("SQLITE_TMPDIR"), false}} {
        for (auto& variable : variables_) {
            variable.was_set = qEnvironmentVariableIsSet(variable.name.constData());
        }
    }

    ~ScopedAmbientState() { restore(); }

    void restore() {
        static_cast<void>(QDir::setCurrent(current_directory_));
        for (const auto& variable : variables_) {
            if (variable.was_set) {
                qputenv(variable.name.constData(), variable.value);
            } else {
                qunsetenv(variable.name.constData());
            }
        }
    }

    void redirect(const QString& current_directory, const QByteArray& value) {
        static_cast<void>(QDir::setCurrent(current_directory));
        for (const auto& variable : variables_) {
            qputenv(variable.name.constData(), value);
        }
    }

  private:
    struct Variable final {
        QByteArray name;
        QByteArray value;
        bool was_set{};
    };

    QString current_directory_;
    std::array<Variable, 4> variables_;
};

class ScopedUmask final {
  public:
    explicit ScopedUmask(mode_t mask) : original_(::umask(mask)) {}
    ~ScopedUmask() { ::umask(original_); }

  private:
    mode_t original_{};
};

[[nodiscard]] QByteArray scratchName(std::size_t attempt, qsizetype family_size = 4) {
    const auto nonce =
        QByteArray::number(static_cast<qulonglong>(attempt), 16)
            .rightJustified(static_cast<qsizetype>(
                                appellate::packs::detail::secure_scratch_nonce_hex_characters),
                            '0');
    return QByteArray(family_size, 'a') + '-' + nonce;
}

[[nodiscard]] QSet<QString> directoryEntries(const QString& path) {
    const auto entries =
        QDir(path).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    return QSet<QString>(entries.begin(), entries.end());
}

[[nodiscard]] QStringList recursivePaths(const QString& root) {
    if (!QFileInfo::exists(root)) {
        return {};
    }
    QStringList paths{QDir::cleanPath(root)};
    QDirIterator iterator(root,
                          QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        paths.push_back(QDir::cleanPath(iterator.next()));
    }
    paths.sort();
    return paths;
}

[[nodiscard]] int absoluteComponentCount(const QString& path) {
    return static_cast<int>(QDir::cleanPath(path).split(u'/', Qt::SkipEmptyParts).size());
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
    constexpr std::uint16_t user_object_tag = 0x01;
    constexpr std::uint16_t named_user_tag = 0x02;
    constexpr std::uint16_t group_object_tag = 0x04;
    constexpr std::uint16_t mask_tag = 0x10;
    constexpr std::uint16_t other_tag = 0x20;
    constexpr std::uint32_t undefined_id = 0xffffffffU;
    QByteArray bytes;
    appendLittleEndian32(bytes, 0x0002U);
    const auto append_entry = [&](std::uint16_t tag, std::uint16_t permissions, std::uint32_t id) {
        appendLittleEndian16(bytes, tag);
        appendLittleEndian16(bytes, permissions);
        appendLittleEndian32(bytes, id);
    };
    append_entry(user_object_tag, 7, undefined_id);
    append_entry(named_user_tag, 4, static_cast<std::uint32_t>(user_id));
    append_entry(group_object_tag, 0, undefined_id);
    append_entry(mask_tag, 4, undefined_id);
    append_entry(other_tag, 0, undefined_id);
    return bytes;
}

[[nodiscard]] std::optional<uid_t> subordinateId(const QString& path) {
    const auto* account = ::getpwuid(::geteuid());
    if (account == nullptr) {
        return std::nullopt;
    }
    const auto account_name = QString::fromLocal8Bit(account->pw_name);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const auto lines = QString::fromUtf8(file.readAll()).split(u'\n', Qt::SkipEmptyParts);
    for (const auto& line : lines) {
        const auto fields = line.split(u':');
        if (fields.size() != 3 || fields.at(0) != account_name) {
            continue;
        }
        bool start_ok = false;
        bool count_ok = false;
        const auto start = fields.at(1).toULongLong(&start_ok);
        const auto count = fields.at(2).toULongLong(&count_ok);
        if (start_ok && count_ok && count > 0 && start <= std::numeric_limits<uid_t>::max()) {
            return static_cast<uid_t>(start);
        }
    }
    return std::nullopt;
}
#endif

class PackArchiveTest final : public QObject {
    Q_OBJECT

  private slots:
    void roundTripPreservesRevision();
    void exportIsByteDeterministic();
    void rejectsDeflatedMember();
    void rejectsEncryptedMember();
    void rejectsUnsafeAndDuplicatePaths();
    void rejectsUndeclaredPortableMember();
    void rejectsDeclaredButMissingBlob();
    void rejectsLinksAndDirectories();
    void enforcesMemberAndSizeLimits();
    void reusesStrictPackValidationAfterExtraction();
    void importsArchivesThroughSecureScratch();
};

[[nodiscard]] QString fixture(const QString& name) {
    return QStringLiteral(APPELLATE_TEST_FIXTURES) + u'/' + name;
}

[[nodiscard]] QByteArray readAll(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

[[nodiscard]] bool writeAll(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(bytes) == static_cast<qint64>(bytes.size());
}

[[nodiscard]] bool writeTestZip(const QString& path, const std::vector<TestEntry>& entries,
                                TestCompression compression = TestCompression::Store) {
    ArchiveWrite writer(archive_write_new());
    if (!writer || archive_write_set_format_zip(writer.get()) != ARCHIVE_OK) {
        return false;
    }
    const auto compression_result = compression == TestCompression::Store
                                        ? archive_write_zip_set_compression_store(writer.get())
                                        : archive_write_zip_set_compression_deflate(writer.get());
    if (compression_result != ARCHIVE_OK ||
        archive_write_set_options(writer.get(), "zip:!zip64") != ARCHIVE_OK) {
        return false;
    }
    const auto encoded_path = QFile::encodeName(path);
    if (archive_write_open_filename(writer.get(), encoded_path.constData()) != ARCHIVE_OK) {
        return false;
    }
    for (const auto& item : entries) {
        ArchiveEntry entry(archive_entry_new());
        if (!entry) {
            return false;
        }
        const auto member_path = item.path.toLatin1();
        archive_entry_copy_pathname(entry.get(), member_path.constData());
        archive_entry_set_filetype(entry.get(), item.file_type);
        archive_entry_set_perm(entry.get(), item.file_type == AE_IFDIR ? 0755 : 0644);
        archive_entry_set_mtime(entry.get(), 315'532'800, 0);
        if (item.file_type == AE_IFLNK) {
            const auto target = item.link_target.toLatin1();
            archive_entry_set_symlink(entry.get(), target.constData());
            archive_entry_set_size(entry.get(), 0);
        } else {
            archive_entry_set_size(entry.get(), static_cast<la_int64_t>(item.data.size()));
        }
        if (archive_write_header(writer.get(), entry.get()) != ARCHIVE_OK) {
            return false;
        }
        if (item.file_type == AE_IFREG && !item.data.isEmpty() &&
            archive_write_data(writer.get(), item.data.constData(),
                               static_cast<std::size_t>(item.data.size())) != item.data.size()) {
            return false;
        }
        if (archive_write_finish_entry(writer.get()) != ARCHIVE_OK) {
            return false;
        }
    }
    return archive_write_close(writer.get()) == ARCHIVE_OK;
}

[[nodiscard]] bool markFirstMemberEncrypted(const QString& path) {
    auto bytes = readAll(path);
    const QByteArray local_signature("PK\x03\x04", 4);
    const QByteArray central_signature("PK\x01\x02", 4);
    const auto local = bytes.indexOf(local_signature);
    const auto central = bytes.indexOf(central_signature);
    if (local < 0 || central < 0 || local + 7 >= bytes.size() || central + 9 >= bytes.size()) {
        return false;
    }
    bytes[local + 6] = static_cast<char>(static_cast<unsigned char>(bytes.at(local + 6)) | 0x01U);
    bytes[central + 8] =
        static_cast<char>(static_cast<unsigned char>(bytes.at(central + 8)) | 0x01U);
    return writeAll(path, bytes);
}

void PackArchiveTest::roundTripPreservesRevision() {
    QTemporaryDir output;
    QVERIFY(output.isValid());
    const auto source = fixture(QStringLiteral("full-resource-pack"));
    const auto archive_path = QDir(output.path()).filePath(QStringLiteral("full.awpack"));
    const auto source_pack = appellate::packs::PackReader::readDirectory(source);
    QVERIFY(source_pack.has_value());

    const auto exported = appellate::packs::PackArchive::exportDirectory(source, archive_path);
    if (!exported.has_value()) {
        QFAIL(qPrintable(exported.error().message));
    }
    const auto imported = appellate::packs::PackArchive::importArchive(archive_path);
    if (!imported.has_value()) {
        QFAIL(qPrintable(imported.error().message));
    }
    QCOMPARE(*exported, source_pack->revision);
    QCOMPARE(imported->revision, source_pack->revision);
    QCOMPARE(imported->blobs, source_pack->blobs);
    QCOMPARE(imported->judge_profiles, source_pack->judge_profiles);
}

void PackArchiveTest::exportIsByteDeterministic() {
    QTemporaryDir output;
    QVERIFY(output.isValid());
    const auto source = fixture(QStringLiteral("minimal-pack"));
    const auto first_path = QDir(output.path()).filePath(QStringLiteral("first.awpack"));
    const auto second_path = QDir(output.path()).filePath(QStringLiteral("second.awpack"));
    ScopedTimeZone time_zone;
    time_zone.set(QByteArrayLiteral("UTC0"));
    QVERIFY(appellate::packs::PackArchive::exportDirectory(source, first_path).has_value());
    time_zone.set(QByteArrayLiteral("KST-9"));
    QVERIFY(appellate::packs::PackArchive::exportDirectory(source, second_path).has_value());
    QCOMPARE(readAll(first_path), readAll(second_path));
}

void PackArchiveTest::rejectsDeflatedMember() {
    QTemporaryDir output;
    QVERIFY(output.isValid());
    const auto path = QDir(output.path()).filePath(QStringLiteral("deflated.awpack"));
    QVERIFY(writeTestZip(path, {{QStringLiteral("manifest.json"), QByteArray(4096, 'a')}},
                         TestCompression::Deflate));

    const auto result = appellate::packs::PackArchive::importArchive(path);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::InvalidManifest);
}

void PackArchiveTest::rejectsEncryptedMember() {
    QTemporaryDir output;
    QVERIFY(output.isValid());
    const auto path = QDir(output.path()).filePath(QStringLiteral("encrypted.awpack"));
    QVERIFY(writeTestZip(path, {{QStringLiteral("manifest.json"), QByteArray("{}")}}));
    QVERIFY(markFirstMemberEncrypted(path));

    const auto result = appellate::packs::PackArchive::importArchive(path);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::InvalidManifest);
}

void PackArchiveTest::rejectsUnsafeAndDuplicatePaths() {
    QTemporaryDir output;
    QVERIFY(output.isValid());
    const auto unsafe_path = QDir(output.path()).filePath(QStringLiteral("unsafe.awpack"));
    QVERIFY(writeTestZip(unsafe_path, {{QStringLiteral("../manifest.json"), QByteArray("{}")}}));
    auto result = appellate::packs::PackArchive::importArchive(unsafe_path);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::UnsafePath);

    const auto duplicate_path = QDir(output.path()).filePath(QStringLiteral("duplicate.awpack"));
    QVERIFY(writeTestZip(duplicate_path, {
                                             {QStringLiteral("manifest.json"), QByteArray("{}")},
                                             {QStringLiteral("manifest.json"), QByteArray("{}")},
                                         }));
    result = appellate::packs::PackArchive::importArchive(duplicate_path);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::DuplicateContentPath);
}

void PackArchiveTest::rejectsUndeclaredPortableMember() {
    QTemporaryDir output;
    QVERIFY(output.isValid());
    const auto source = fixture(QStringLiteral("minimal-pack"));
    const auto manifest = readAll(QDir(source).filePath(QStringLiteral("manifest.json")));
    const auto profile = readAll(QDir(source).filePath(QStringLiteral("judges/measured.json")));
    QVERIFY(!manifest.isEmpty());
    QVERIFY(!profile.isEmpty());
    const auto path = QDir(output.path()).filePath(QStringLiteral("undeclared.awpack"));
    QVERIFY(writeTestZip(
        path, {
                  {QStringLiteral("manifest.json"), manifest},
                  {QStringLiteral("judges/measured.json"), profile},
                  {QStringLiteral("objects/unlisted.pdf"), QByteArray("%PDF-1.7\n%%EOF\n")},
              }));

    const auto result = appellate::packs::PackArchive::importArchive(path);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::UndeclaredFile);
}

void PackArchiveTest::rejectsDeclaredButMissingBlob() {
    QTemporaryDir output;
    QVERIFY(output.isValid());
    const auto source = fixture(QStringLiteral("minimal-pack"));
    auto manifest =
        QJsonDocument::fromJson(readAll(QDir(source).filePath(QStringLiteral("manifest.json"))))
            .object();
    const auto profile = readAll(QDir(source).filePath(QStringLiteral("judges/measured.json")));
    QVERIFY(!manifest.isEmpty());
    QVERIFY(!profile.isEmpty());
    manifest.insert(QStringLiteral("blobs"),
                    QJsonArray{QJsonObject{
                        {QStringLiteral("path"), QStringLiteral("objects/missing.pdf")},
                        {QStringLiteral("media_type"), QStringLiteral("application/pdf")},
                        {QStringLiteral("byte_size"), 17},
                        {QStringLiteral("sha256"), QString(64, u'0')},
                    }});
    const auto path = QDir(output.path()).filePath(QStringLiteral("missing-blob.awpack"));
    QVERIFY(writeTestZip(path, {{QStringLiteral("manifest.json"),
                                 QJsonDocument(manifest).toJson(QJsonDocument::Compact)},
                                {QStringLiteral("judges/measured.json"), profile}}));

    const auto result = appellate::packs::PackArchive::importArchive(path);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::CannotRead);
}

void PackArchiveTest::rejectsLinksAndDirectories() {
    QTemporaryDir output;
    QVERIFY(output.isValid());
    const auto link_path = QDir(output.path()).filePath(QStringLiteral("link.awpack"));
    QVERIFY(writeTestZip(
        link_path,
        {{QStringLiteral("manifest.json"), {}, AE_IFLNK, QStringLiteral("elsewhere.json")}}));
    auto result = appellate::packs::PackArchive::importArchive(link_path);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::UnsafePath);

    const auto directory_path = QDir(output.path()).filePath(QStringLiteral("directory.awpack"));
    QVERIFY(writeTestZip(directory_path, {{QStringLiteral("manifest.json"), {}, AE_IFDIR, {}}}));
    result = appellate::packs::PackArchive::importArchive(directory_path);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::UnsafePath);
}

void PackArchiveTest::enforcesMemberAndSizeLimits() {
    QTemporaryDir output;
    QVERIFY(output.isValid());
    const auto path = QDir(output.path()).filePath(QStringLiteral("limits.awpack"));
    QVERIFY(writeTestZip(path, {
                                   {QStringLiteral("manifest.json"), QByteArray("{}")},
                                   {QStringLiteral("second.json"), QByteArray("1234")},
                               }));

    auto limits = appellate::packs::PackArchiveLimits{};
    limits.maximum_members = 1;
    auto result = appellate::packs::PackArchive::importArchive(path, limits);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::ResourceTooLarge);

    limits = {};
    limits.maximum_file_bytes = 3;
    result = appellate::packs::PackArchive::importArchive(path, limits);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::ResourceTooLarge);

    limits = {};
    limits.maximum_total_bytes = 5;
    result = appellate::packs::PackArchive::importArchive(path, limits);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::ResourceTooLarge);
}

void PackArchiveTest::reusesStrictPackValidationAfterExtraction() {
    QTemporaryDir output;
    QVERIFY(output.isValid());
    const auto source = fixture(QStringLiteral("minimal-pack"));
    const auto manifest = readAll(QDir(source).filePath(QStringLiteral("manifest.json")));
    auto profile = readAll(QDir(source).filePath(QStringLiteral("judges/measured.json")));
    QVERIFY(!manifest.isEmpty());
    QVERIFY(!profile.isEmpty());
    profile.append(' ');
    const auto path = QDir(output.path()).filePath(QStringLiteral("tampered.awpack"));
    QVERIFY(writeTestZip(path, {
                                   {QStringLiteral("manifest.json"), manifest},
                                   {QStringLiteral("judges/measured.json"), profile},
                               }));

    const auto result = appellate::packs::PackArchive::importArchive(path);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::DigestMismatch);
}

void PackArchiveTest::importsArchivesThroughSecureScratch() {
    using appellate::packs::ErrorCode;
    using appellate::packs::PackArchive;
    using appellate::packs::PackArchiveLimits;
    using appellate::packs::PackValidationScope;
    using appellate::packs::detail::acquireSecureScratchContext;
    using appellate::packs::detail::importArchiveThroughRetainedSecureScratch;
    using appellate::packs::detail::importArchiveThroughSecureScratch;
    using appellate::packs::detail::isValidSecureScratchName;
    using appellate::packs::detail::SecureScratchCleanupOutcome;
    using appellate::packs::detail::SecureScratchEvent;
    using appellate::packs::detail::SecureScratchFailureCode;
    using appellate::packs::detail::SecureScratchHooks;
    using appellate::packs::detail::SecureScratchInjectedAction;
    using appellate::packs::detail::SecureScratchObservation;
    using appellate::packs::detail::SecureScratchReport;

#if !defined(Q_OS_LINUX)
    QTemporaryDir archive_root;
    QVERIFY(archive_root.isValid());
    const auto archive_path = QDir(archive_root.path()).filePath(QStringLiteral("minimal.awpack"));
    QVERIFY(PackArchive::exportDirectory(fixture(QStringLiteral("minimal-pack")), archive_path)
                .has_value());
    const auto public_result = PackArchive::importArchive(archive_path);
    QVERIFY(!public_result.has_value());
    QCOMPARE(public_result.error().code, ErrorCode::CannotRead);
    const auto private_result = importArchiveThroughSecureScratch(
        archive_path, PackArchiveLimits{}, PackValidationScope::Standalone, SecureScratchHooks{});
    QVERIFY(!private_result.has_value());
    QCOMPARE(private_result.error().code, ErrorCode::CannotRead);
#else
    QTemporaryDir archive_root;
    QTemporaryDir scratch_parent;
    QTemporaryDir cwd_decoy;
    QTemporaryDir environment_decoy;
    QVERIFY(archive_root.isValid());
    QVERIFY(scratch_parent.isValid());
    QVERIFY(cwd_decoy.isValid());
    QVERIFY(environment_decoy.isValid());
    const auto archive_path = QDir(archive_root.path()).filePath(QStringLiteral("minimal.awpack"));
    const auto source_path = fixture(QStringLiteral("minimal-pack"));
    const auto source_pack = appellate::packs::PackReader::readDirectory(source_path);
    QVERIFY(source_pack.has_value());
    QVERIFY(PackArchive::exportDirectory(source_path, archive_path).has_value());
    const auto archive_bytes = readAll(archive_path);
    QVERIFY(!archive_bytes.isEmpty());
    const auto packs_spec = readAll(
        QDir(QStringLiteral(APPELLATE_SOURCE_DIR)).filePath(QStringLiteral("docs/spec/PACKS.md")));
    QVERIFY(packs_spec.contains("`QDir::tempPath()` exactly once"));
    QVERIFY(packs_spec.contains("owned by the current effective UID or UID 0"));
    QVERIFY(packs_spec.contains("every group- or other-writable controller is sticky"));
    QVERIFY(packs_spec.contains("Access\nACLs must be absent from every retained inode and default "
                                "ACLs must be absent from every directory"));
    QVERIFY(packs_spec.contains("each is 1–64\nnative ASCII bytes, uses only `[a-z0-9._-]`"));
    QVERIFY(packs_spec.contains("exact mode 0700 or 0600"));
    QVERIFY(packs_spec.contains("127 collisions followed by a unique name succeeds"));
    QVERIFY(packs_spec.contains("`ErrorCode::CannotRead`"));
    QVERIFY(packs_spec.contains("fall\nback to the current working directory, or try another "
                                "unretained temporary location"));
    QVERIFY(packs_spec.contains("preserved for manual diagnosis"));
    QVERIFY(packs_spec.contains("Normal success and\nordinary failure leave no scratch residue."));

    const auto scratch_baseline = directoryEntries(scratch_parent.path());
    const auto cwd_baseline = directoryEntries(cwd_decoy.path());
    const auto environment_baseline = directoryEntries(environment_decoy.path());
    ScopedAmbientState ambient;
    int temp_path_captures = 0;
    bool redirected = false;
    std::vector<SecureScratchObservation> observations;
    std::vector<SecureScratchObservation> action_observations;
    SecureScratchReport report;
    SecureScratchHooks hooks;
    hooks.temp_path_provider = [&] {
        ++temp_path_captures;
        return scratch_parent.path();
    };
    hooks.name_source = [](std::size_t attempt) { return scratchName(attempt); };
    hooks.inject = [&](const SecureScratchObservation& observation) {
        action_observations.push_back(observation);
        return SecureScratchInjectedAction::Continue;
    };
    hooks.observe = [&](const SecureScratchObservation& observation) {
        observations.push_back(observation);
        if (observation.event == SecureScratchEvent::TempPathCaptured && !redirected) {
            redirected = true;
            ambient.redirect(cwd_decoy.path(), environment_decoy.path().toUtf8());
        }
    };
    hooks.report = &report;
    const auto imported = importArchiveThroughSecureScratch(archive_path, PackArchiveLimits{},
                                                            PackValidationScope::Standalone, hooks);
    QVERIFY2(imported.has_value(), imported ? "" : qPrintable(imported.error().message));
    QCOMPARE(temp_path_captures, 1);
    QCOMPARE(report.captured_temp_path, QDir::cleanPath(scratch_parent.path()));
    QVERIFY(!report.workspace_path.isEmpty());
    QCOMPARE(report.cleanup, SecureScratchCleanupOutcome::Removed);
    QVERIFY(!QFileInfo::exists(report.workspace_path));
    QCOMPARE(directoryEntries(scratch_parent.path()), scratch_baseline);
    QCOMPARE(directoryEntries(cwd_decoy.path()), cwd_baseline);
    QCOMPARE(directoryEntries(environment_decoy.path()), environment_baseline);
    QCOMPARE(readAll(archive_path), archive_bytes);
    ambient.restore();
    const auto compare_loaded_pack = [](const appellate::packs::LoadedPack& actual,
                                        const appellate::packs::LoadedPack& expected) {
        QCOMPARE(actual.manifest_schema_version, expected.manifest_schema_version);
        QCOMPARE(actual.revision, expected.revision);
        QCOMPARE(actual.required_capabilities, expected.required_capabilities);
        QCOMPARE(actual.dependencies, expected.dependencies);
        QCOMPARE(actual.resources.size(), expected.resources.size());
        for (std::size_t index = 0; index < actual.resources.size(); ++index) {
            QCOMPARE(actual.resources.at(index).descriptor,
                     expected.resources.at(index).descriptor);
            QCOMPARE(actual.resources.at(index).document, expected.resources.at(index).document);
        }
        QCOMPARE(actual.blobs, expected.blobs);
        QCOMPARE(actual.judge_profiles, expected.judge_profiles);
        QCOMPARE(actual.graph_state, expected.graph_state);
    };
    compare_loaded_pack(*imported, *source_pack);

    QTemporaryDir retained_parent;
    QVERIFY(retained_parent.isValid());
    const auto retained_baseline = directoryEntries(retained_parent.path());
    int retained_temp_path_calls = 0;
    std::vector<SecureScratchObservation> retained_observations;
    SecureScratchHooks retained_hooks;
    retained_hooks.temp_path_provider = [&] {
        ++retained_temp_path_calls;
        return retained_parent.path();
    };
    retained_hooks.name_source = [](std::size_t attempt) { return scratchName(attempt, 6); };
    retained_hooks.observe = [&](const auto& observation) {
        retained_observations.push_back(observation);
    };
    auto retained_context = acquireSecureScratchContext(retained_hooks);
    QVERIFY2(retained_context.has_value(),
             retained_context ? "" : qPrintable(retained_context.error().message));
    QCOMPARE(retained_temp_path_calls, 1);
    QCOMPARE(retained_context->absoluteParent(), QDir::cleanPath(retained_parent.path()));
    const auto retained_parent_descriptor = retained_context->parentDescriptor();
    QVERIFY(retained_parent_descriptor.has_value());
    struct stat retained_parent_before{};
    QVERIFY(::fstat(*retained_parent_descriptor, &retained_parent_before) == 0);
    QVERIFY(retained_context->validateRetainedControllers().has_value());
    const auto retained_controller_opens =
        std::ranges::count(retained_observations, SecureScratchEvent::ControllerOpened,
                           &SecureScratchObservation::event);

    const auto retained_first = importArchiveThroughRetainedSecureScratch(
        archive_path, PackArchiveLimits{}, PackValidationScope::Standalone, *retained_context,
        retained_hooks);
    QVERIFY2(retained_first.has_value(),
             retained_first ? "" : qPrintable(retained_first.error().message));
    compare_loaded_pack(*retained_first, *source_pack);
    QVERIFY(retained_context->isValid());
    QVERIFY(retained_context->validateRetainedControllers().has_value());

    const auto retained_second = importArchiveThroughRetainedSecureScratch(
        archive_path, PackArchiveLimits{}, PackValidationScope::Standalone, *retained_context,
        retained_hooks);
    QVERIFY2(retained_second.has_value(),
             retained_second ? "" : qPrintable(retained_second.error().message));
    compare_loaded_pack(*retained_second, *source_pack);
    QCOMPARE(retained_temp_path_calls, 1);
    QCOMPARE(std::ranges::count(retained_observations, SecureScratchEvent::TempPathCaptured,
                                &SecureScratchObservation::event),
             1);
    QCOMPARE(std::ranges::count(retained_observations, SecureScratchEvent::ControllerOpened,
                                &SecureScratchObservation::event),
             retained_controller_opens);
    const auto retained_parent_after = retained_context->parentDescriptor();
    QVERIFY(retained_parent_after.has_value());
    QCOMPARE(*retained_parent_after, *retained_parent_descriptor);
    struct stat retained_parent_rebound{};
    QVERIFY(::fstat(*retained_parent_after, &retained_parent_rebound) == 0);
    QCOMPARE(retained_parent_rebound.st_dev, retained_parent_before.st_dev);
    QCOMPARE(retained_parent_rebound.st_ino, retained_parent_before.st_ino);
    QCOMPARE(directoryEntries(retained_parent.path()), retained_baseline);

    QTemporaryDir moved_parent;
    QVERIFY(moved_parent.isValid());
    int moved_temp_path_calls = 0;
    std::vector<SecureScratchObservation> moved_observations;
    SecureScratchHooks moved_hooks;
    moved_hooks.temp_path_provider = [&] {
        ++moved_temp_path_calls;
        return moved_parent.path();
    };
    moved_hooks.name_source = [](std::size_t attempt) { return scratchName(attempt, 7); };
    moved_hooks.observe = [&](const auto& observation) {
        moved_observations.push_back(observation);
    };
    auto moved_context = acquireSecureScratchContext(moved_hooks);
    QVERIFY2(moved_context.has_value(),
             moved_context ? "" : qPrintable(moved_context.error().message));
    auto retained_owner = std::move(*moved_context);
    QVERIFY(retained_owner.isValid());
    QVERIFY(!moved_context->isValid());
    QVERIFY(moved_context->absoluteParent().isEmpty());
    QVERIFY(!moved_context->parentDescriptor().has_value());
    QVERIFY(!moved_context->validateRetainedControllers().has_value());
    moved_observations.clear();
    const auto moved_temp_path_calls_before_import = moved_temp_path_calls;
    const auto missing_archive =
        QDir(archive_root.path()).filePath(QStringLiteral("must-not-be-opened.awpack"));
    QVERIFY(!QFileInfo::exists(missing_archive));
    const auto moved_from_import = importArchiveThroughRetainedSecureScratch(
        missing_archive, PackArchiveLimits{}, PackValidationScope::Standalone, *moved_context,
        moved_hooks);
    QVERIFY(!moved_from_import.has_value());
    QCOMPARE(moved_from_import.error().code, ErrorCode::CannotRead);
    QVERIFY(moved_from_import.error().message.contains(QStringLiteral("consumed")));
    QCOMPARE(moved_temp_path_calls, moved_temp_path_calls_before_import);
    QVERIFY(moved_observations.empty());
    QCOMPARE(directoryEntries(moved_parent.path()), QSet<QString>{});

    int captured_events = 0;
    int reader_events = 0;
    bool saw_directory_0700 = false;
    bool saw_file_0600 = false;
    for (const auto& observation : observations) {
        captured_events += observation.event == SecureScratchEvent::TempPathCaptured ? 1 : 0;
        reader_events += observation.event == SecureScratchEvent::BeforeReader ? 1 : 0;
        saw_directory_0700 =
            saw_directory_0700 || (observation.event == SecureScratchEvent::DirectoryNormalize &&
                                   observation.mode_after == 0700U);
        saw_file_0600 = saw_file_0600 || (observation.event == SecureScratchEvent::FileNormalize &&
                                          observation.mode_after == 0600U);
    }
    QCOMPARE(captured_events, 1);
    QCOMPARE(reader_events, 1);
    QVERIFY(saw_directory_0700);
    QVERIFY(saw_file_0600);

    using ObservedStep = std::pair<SecureScratchEvent, QString>;
    const auto observed_in_order = [&](const std::vector<ObservedStep>& expected) {
        auto cursor = action_observations.cbegin();
        for (const auto& [event, path] : expected) {
            cursor = std::find_if(cursor, action_observations.cend(), [&](const auto& observation) {
                return observation.event == event && observation.absolute_path == path;
            });
            if (cursor == action_observations.cend()) {
                return false;
            }
            ++cursor;
        }
        return true;
    };
    const auto workspace_root = report.workspace_path;
    const auto nested_directory = QDir(workspace_root).filePath(QStringLiteral("judges"));
    const auto manifest_file = QDir(workspace_root).filePath(QStringLiteral("manifest.json"));
    const auto nested_file = QDir(nested_directory).filePath(QStringLiteral("measured.json"));
    QVERIFY(observed_in_order(
        {{SecureScratchEvent::DirectoryCreate, workspace_root},
         {SecureScratchEvent::DirectoryRetainOpen, workspace_root},
         {SecureScratchEvent::DirectoryRetained, workspace_root},
         {SecureScratchEvent::DirectoryNormalize, workspace_root},
         {SecureScratchEvent::AccessAclProbe, workspace_root},
         {SecureScratchEvent::DefaultAclProbe, workspace_root},
         {SecureScratchEvent::DirectoryRebound, workspace_root},
         {SecureScratchEvent::DirectorySync, workspace_root},
         {SecureScratchEvent::DirectorySync, QDir::cleanPath(scratch_parent.path())}}));
    QVERIFY(observed_in_order({{SecureScratchEvent::DirectoryCreate, nested_directory},
                               {SecureScratchEvent::DirectoryRetainOpen, nested_directory},
                               {SecureScratchEvent::DirectoryRetained, nested_directory},
                               {SecureScratchEvent::DirectoryNormalize, nested_directory},
                               {SecureScratchEvent::AccessAclProbe, nested_directory},
                               {SecureScratchEvent::DefaultAclProbe, nested_directory},
                               {SecureScratchEvent::DirectoryRebound, nested_directory},
                               {SecureScratchEvent::DirectorySync, nested_directory},
                               {SecureScratchEvent::DirectorySync, workspace_root}}));
    QVERIFY(observed_in_order({{SecureScratchEvent::FileCreate, manifest_file},
                               {SecureScratchEvent::FileRetainRebind, manifest_file},
                               {SecureScratchEvent::FileRetained, manifest_file},
                               {SecureScratchEvent::FileNormalize, manifest_file},
                               {SecureScratchEvent::AccessAclProbe, manifest_file},
                               {SecureScratchEvent::FileRebound, manifest_file},
                               {SecureScratchEvent::FileWrite, manifest_file},
                               {SecureScratchEvent::FileSync, manifest_file},
                               {SecureScratchEvent::DirectorySync, workspace_root}}));
    QVERIFY(observed_in_order({{SecureScratchEvent::FileCreate, nested_file},
                               {SecureScratchEvent::FileRetainRebind, nested_file},
                               {SecureScratchEvent::FileRetained, nested_file},
                               {SecureScratchEvent::FileNormalize, nested_file},
                               {SecureScratchEvent::AccessAclProbe, nested_file},
                               {SecureScratchEvent::FileRebound, nested_file},
                               {SecureScratchEvent::FileWrite, nested_file},
                               {SecureScratchEvent::FileSync, nested_file},
                               {SecureScratchEvent::DirectorySync, nested_directory}}));
    const auto before_reader = std::find_if(
        action_observations.cbegin(), action_observations.cend(), [](const auto& observation) {
            return observation.event == SecureScratchEvent::BeforeReader;
        });
    QVERIFY(before_reader != action_observations.cend());
    const auto inventory_between = std::find_if(
        std::next(before_reader), action_observations.cend(), [](const auto& observation) {
            return observation.event == SecureScratchEvent::InventoryRebind;
        });
    QVERIFY(inventory_between != action_observations.cend());
    const auto after_reader = std::find_if(
        std::next(inventory_between), action_observations.cend(), [](const auto& observation) {
            return observation.event == SecureScratchEvent::AfterReader;
        });
    QVERIFY(after_reader != action_observations.cend());
    const auto first_cleanup_observation = std::find_if(
        std::next(after_reader), action_observations.cend(), [](const auto& observation) {
            return observation.event == SecureScratchEvent::CleanupInspect;
        });
    QVERIFY(first_cleanup_observation != action_observations.cend());
    const auto inventory_paths = [](auto first, auto last) {
        QSet<QString> paths;
        for (; first != last; ++first) {
            if (first->event == SecureScratchEvent::InventoryRebind) {
                paths.insert(QDir::cleanPath(first->absolute_path));
            }
        }
        return paths;
    };
    const QSet<QString> expected_inventory_paths{workspace_root, nested_directory, manifest_file,
                                                 nested_file};
    const auto first_inventory_rebind_count = static_cast<std::size_t>(std::count_if(
        std::next(before_reader), after_reader, [](const SecureScratchObservation& observation) {
            return observation.event == SecureScratchEvent::InventoryRebind;
        }));
    QCOMPARE(first_inventory_rebind_count,
             static_cast<std::size_t>(expected_inventory_paths.size()));
    QCOMPARE(inventory_paths(std::next(before_reader), after_reader), expected_inventory_paths);
    QCOMPARE(inventory_paths(std::next(after_reader), first_cleanup_observation),
             expected_inventory_paths);

    const auto public_import = PackArchive::importArchive(archive_path);
    QVERIFY(public_import.has_value());
    compare_loaded_pack(*public_import, *imported);

    const auto invalid_archive =
        QDir(archive_root.path()).filePath(QStringLiteral("invalid.awpack"));
    QVERIFY(writeTestZip(invalid_archive,
                         {{QStringLiteral("manifest.json"), QByteArrayLiteral("{}")}}));
    const auto invalid_archive_bytes = readAll(invalid_archive);
    QVERIFY(!invalid_archive_bytes.isEmpty());
    const auto public_invalid = PackArchive::importArchive(invalid_archive);
    QVERIFY(!public_invalid.has_value());
    SecureScratchReport invalid_report;
    auto invalid_hooks = hooks;
    invalid_hooks.observe = {};
    invalid_hooks.report = &invalid_report;
    const auto private_invalid = importArchiveThroughSecureScratch(
        invalid_archive, PackArchiveLimits{}, PackValidationScope::Standalone, invalid_hooks);
    QVERIFY(!private_invalid.has_value());
    QCOMPARE(private_invalid.error().code, public_invalid.error().code);
    QCOMPARE(private_invalid.error().message, public_invalid.error().message);
    QCOMPARE(invalid_report.cleanup, SecureScratchCleanupOutcome::NotAttempted);
    QVERIFY(invalid_report.workspace_path.isEmpty());
    QCOMPARE(readAll(invalid_archive), invalid_archive_bytes);

    for (const auto mask : {mode_t{0000}, mode_t{0777}}) {
        QTemporaryDir masked_parent;
        QVERIFY(masked_parent.isValid());
        SecureScratchReport masked_report;
        std::vector<SecureScratchObservation> masked_observations;
        QSet<QString> rebound_directories;
        QSet<QString> rebound_files;
        SecureScratchHooks masked_hooks;
        masked_hooks.temp_path_provider = [&] { return masked_parent.path(); };
        masked_hooks.name_source = [](std::size_t attempt) { return scratchName(attempt); };
        masked_hooks.observe = [&](const auto& observation) {
            masked_observations.push_back(observation);
            if (observation.event == SecureScratchEvent::DirectoryRebound ||
                observation.event == SecureScratchEvent::FileRebound) {
                struct stat status{};
                QVERIFY(::stat(QFile::encodeName(observation.absolute_path).constData(), &status) ==
                        0);
                const auto directory = observation.event == SecureScratchEvent::DirectoryRebound;
                QCOMPARE(static_cast<unsigned int>(status.st_mode & 07777),
                         directory ? 0700U : 0600U);
                (directory ? rebound_directories : rebound_files)
                    .insert(QDir::cleanPath(observation.absolute_path));
            }
        };
        masked_hooks.report = &masked_report;
        std::optional<ScopedUmask> scoped_umask;
        scoped_umask.emplace(mask);
        const auto result = importArchiveThroughSecureScratch(
            archive_path, PackArchiveLimits{}, PackValidationScope::Standalone, masked_hooks);
        QVERIFY2(result.has_value(), result ? "" : qPrintable(result.error().message));
        QSet<QString> normalized_directories;
        QSet<QString> normalized_files;
        for (const auto& observation : masked_observations) {
            if (observation.event == SecureScratchEvent::DirectoryNormalize) {
                QCOMPARE(observation.mode_after, 0700U);
                normalized_directories.insert(QDir::cleanPath(observation.absolute_path));
            }
            if (observation.event == SecureScratchEvent::FileNormalize) {
                QCOMPARE(observation.mode_after, 0600U);
                normalized_files.insert(QDir::cleanPath(observation.absolute_path));
            }
        }
        const auto masked_workspace = QDir::cleanPath(masked_report.workspace_path);
        const QSet<QString> expected_normalized_directories{
            masked_workspace, QDir(masked_workspace).filePath(QStringLiteral("judges"))};
        const QSet<QString> expected_normalized_files{
            QDir(masked_workspace).filePath(QStringLiteral("manifest.json")),
            QDir(masked_workspace).filePath(QStringLiteral("judges/measured.json"))};
        QCOMPARE(normalized_directories, expected_normalized_directories);
        QCOMPARE(normalized_files, expected_normalized_files);
        QCOMPARE(rebound_directories, normalized_directories);
        QCOMPARE(rebound_files, normalized_files);
        QCOMPARE(masked_report.cleanup, SecureScratchCleanupOutcome::Removed);
        QCOMPARE(directoryEntries(masked_parent.path()), QSet<QString>{});
    }

    QTemporaryDir writable_controller;
    QVERIFY(writable_controller.isValid());
    const auto writable_controller_name = QFile::encodeName(writable_controller.path());
    SecureScratchHooks writable_hooks;
    writable_hooks.temp_path_provider = [&] { return writable_controller.path(); };
    writable_hooks.name_source = [](std::size_t attempt) { return scratchName(attempt); };
    QVERIFY(::chmod(writable_controller_name.constData(), 01777) == 0);
    const auto sticky_result = importArchiveThroughSecureScratch(
        archive_path, PackArchiveLimits{}, PackValidationScope::Standalone, writable_hooks);
    QVERIFY2(sticky_result.has_value(),
             sticky_result ? "" : qPrintable(sticky_result.error().message));
    QVERIFY(::chmod(writable_controller_name.constData(), 0777) == 0);
    const auto unsafe_result = importArchiveThroughSecureScratch(
        archive_path, PackArchiveLimits{}, PackValidationScope::Standalone, writable_hooks);
    QVERIFY(!unsafe_result.has_value());
    QCOMPARE(unsafe_result.error().code, ErrorCode::CannotRead);
    const auto unsafe_acquire = acquireSecureScratchContext(writable_hooks);
    QVERIFY(!unsafe_acquire.has_value());
    QCOMPARE(unsafe_acquire.error().code, SecureScratchFailureCode::EnvironmentInfeasible);
    QVERIFY(::chmod(writable_controller_name.constData(), 0700) == 0);

    SecureScratchHooks root_sticky_hooks;
    root_sticky_hooks.temp_path_provider = [] { return QStringLiteral("/tmp"); };
    root_sticky_hooks.name_source = [](std::size_t attempt) { return scratchName(attempt, 5); };
    const auto root_sticky_result = importArchiveThroughSecureScratch(
        archive_path, PackArchiveLimits{}, PackValidationScope::Standalone, root_sticky_hooks);
    QVERIFY2(root_sticky_result.has_value(),
             root_sticky_result ? "" : qPrintable(root_sticky_result.error().message));

    if (::geteuid() == 0) {
        bool rename_attempted = false;
        bool rename_denied = false;
        SecureScratchReport second_uid_report;
        SecureScratchHooks second_uid_hooks;
        second_uid_hooks.temp_path_provider = [] { return QStringLiteral("/tmp"); };
        second_uid_hooks.name_source = [](std::size_t attempt) { return scratchName(attempt, 6); };
        second_uid_hooks.observe = [&](const SecureScratchObservation& observation) {
            if (rename_attempted || observation.event != SecureScratchEvent::DirectoryRebound) {
                return;
            }
            rename_attempted = true;
            const auto source = QFile::encodeName(observation.absolute_path);
            const auto destination = source + QByteArrayLiteral(".second-uid");
            const auto child = ::fork();
            if (child == 0) {
                if (::setgroups(0, nullptr) != 0 || ::setgid(65534) != 0 || ::setuid(65534) != 0) {
                    ::_exit(3);
                }
                errno = 0;
                const auto renamed = ::rename(source.constData(), destination.constData());
                ::_exit(renamed != 0 && (errno == EACCES || errno == EPERM) ? 0 : 1);
            }
            int status = 0;
            rename_denied = child > 0 && ::waitpid(child, &status, 0) == child &&
                            WIFEXITED(status) && WEXITSTATUS(status) == 0;
            if (!rename_denied && QFileInfo::exists(QString::fromLocal8Bit(destination))) {
                static_cast<void>(::rename(destination.constData(), source.constData()));
            }
        };
        second_uid_hooks.report = &second_uid_report;
        const auto second_uid_result = importArchiveThroughSecureScratch(
            archive_path, PackArchiveLimits{}, PackValidationScope::Standalone, second_uid_hooks);
        QVERIFY(rename_attempted);
        QVERIFY(rename_denied);
        QVERIFY2(second_uid_result.has_value(),
                 second_uid_result ? "" : qPrintable(second_uid_result.error().message));
        QCOMPARE(second_uid_report.cleanup, SecureScratchCleanupOutcome::Removed);
        QVERIFY(!QFileInfo::exists(second_uid_report.workspace_path));
    } else {
        const auto unshare = QStandardPaths::findExecutable(QStringLiteral("unshare"));
        const auto subordinate_uid = subordinateId(QStringLiteral("/etc/subuid"));
        const auto subordinate_gid = subordinateId(QStringLiteral("/etc/subgid"));
        if (unshare.isEmpty() || !subordinate_uid.has_value() || !subordinate_gid.has_value()) {
            QWARN("Second-UID sticky-controller rename row skipped explicitly: subordinate user "
                  "namespace mappings are unavailable");
        } else {
            bool rename_attempted = false;
            bool rename_denied = false;
            SecureScratchReport second_uid_report;
            SecureScratchHooks second_uid_hooks;
            second_uid_hooks.temp_path_provider = [] { return QStringLiteral("/tmp"); };
            second_uid_hooks.name_source = [](std::size_t attempt) {
                return scratchName(attempt, 13);
            };
            second_uid_hooks.observe = [&](const SecureScratchObservation& observation) {
                if (rename_attempted || observation.event != SecureScratchEvent::DirectoryRebound) {
                    return;
                }
                rename_attempted = true;
                const auto destination = observation.absolute_path + QStringLiteral(".second-uid");
                QProcess process;
                process.start(
                    unshare,
                    {QStringLiteral("--user"), QStringLiteral("--map-users"),
                     QStringLiteral("0:%1:1").arg(::geteuid()), QStringLiteral("--map-users"),
                     QStringLiteral("1:%1:1").arg(*subordinate_uid), QStringLiteral("--map-groups"),
                     QStringLiteral("0:%1:1").arg(::getegid()), QStringLiteral("--map-groups"),
                     QStringLiteral("1:%1:1").arg(*subordinate_gid), QStringLiteral("--setgid"),
                     QStringLiteral("1"), QStringLiteral("--setuid"), QStringLiteral("1"),
                     QStringLiteral("/bin/sh"), QStringLiteral("-c"),
                     QStringLiteral("probe=\"${1}.probe\"; : > \"$probe\" || exit 3; "
                                    "rm -f -- \"$probe\" || exit 4; "
                                    "if mv -- \"$1\" \"$2\" 2>/dev/null; then "
                                    "mv -- \"$2\" \"$1\"; exit 5; fi; "
                                    "test -e \"$1\" && test ! -e \"$2\""),
                     QStringLiteral("secure-scratch-second-uid"), observation.absolute_path,
                     destination});
                rename_denied = process.waitForFinished() &&
                                process.exitStatus() == QProcess::NormalExit &&
                                process.exitCode() == 0;
            };
            second_uid_hooks.report = &second_uid_report;
            const auto second_uid_result = importArchiveThroughSecureScratch(
                archive_path, PackArchiveLimits{}, PackValidationScope::Standalone,
                second_uid_hooks);
            QVERIFY(rename_attempted);
            QVERIFY(rename_denied);
            QVERIFY2(second_uid_result.has_value(),
                     second_uid_result ? "" : qPrintable(second_uid_result.error().message));
            QCOMPARE(second_uid_report.cleanup, SecureScratchCleanupOutcome::Removed);
            QVERIFY(!QFileInfo::exists(second_uid_report.workspace_path));
        }
    }

    if (::geteuid() == 0) {
        QTemporaryDir wrong_owner;
        QVERIFY(wrong_owner.isValid());
        const auto encoded = QFile::encodeName(wrong_owner.path());
        QVERIFY(::chown(encoded.constData(), 65534, 65534) == 0);
        SecureScratchHooks wrong_owner_hooks;
        wrong_owner_hooks.temp_path_provider = [&] { return wrong_owner.path(); };
        wrong_owner_hooks.name_source = [](std::size_t attempt) { return scratchName(attempt); };
        const auto wrong_owner_result = importArchiveThroughSecureScratch(
            archive_path, PackArchiveLimits{}, PackValidationScope::Standalone, wrong_owner_hooks);
        QVERIFY(!wrong_owner_result.has_value());
        QCOMPARE(wrong_owner_result.error().code, ErrorCode::CannotRead);
        const auto wrong_owner_acquire = acquireSecureScratchContext(wrong_owner_hooks);
        QVERIFY(!wrong_owner_acquire.has_value());
        QCOMPARE(wrong_owner_acquire.error().code, SecureScratchFailureCode::EnvironmentInfeasible);
        QVERIFY(::chown(encoded.constData(), 0, 0) == 0);
        QVERIFY(::chmod(encoded.constData(), 0700) == 0);
    } else {
        const auto unshare = QStandardPaths::findExecutable(QStringLiteral("unshare"));
        const auto subordinate_uid = subordinateId(QStringLiteral("/etc/subuid"));
        const auto subordinate_gid = subordinateId(QStringLiteral("/etc/subgid"));
        if (unshare.isEmpty() || !subordinate_uid.has_value() || !subordinate_gid.has_value()) {
            QWARN("Wrong-owner controller row skipped explicitly: subordinate user namespace "
                  "mappings are unavailable");
        } else {
            QTemporaryDir wrong_owner_controller;
            QVERIFY(wrong_owner_controller.isValid());
            const auto encoded_controller = QFile::encodeName(wrong_owner_controller.path());
            QVERIFY(::chmod(encoded_controller.constData(), 01777) == 0);
            const auto wrong_owner_path =
                QDir(wrong_owner_controller.path()).filePath(QStringLiteral("wrong-owner"));
            QProcess process;
            process.start(
                unshare,
                {QStringLiteral("--user"), QStringLiteral("--map-users"),
                 QStringLiteral("0:%1:1").arg(::geteuid()), QStringLiteral("--map-users"),
                 QStringLiteral("1:%1:1").arg(*subordinate_uid), QStringLiteral("--map-groups"),
                 QStringLiteral("0:%1:1").arg(::getegid()), QStringLiteral("--map-groups"),
                 QStringLiteral("1:%1:1").arg(*subordinate_gid), QStringLiteral("--setgid"),
                 QStringLiteral("1"), QStringLiteral("--setuid"), QStringLiteral("1"),
                 QStringLiteral("/bin/mkdir"), QStringLiteral("-m"), QStringLiteral("0700"),
                 wrong_owner_path});
            QVERIFY(process.waitForFinished());
            QCOMPARE(process.exitStatus(), QProcess::NormalExit);
            QVERIFY2(process.exitCode() == 0, process.readAllStandardError().constData());
            SecureScratchHooks wrong_owner_hooks;
            wrong_owner_hooks.temp_path_provider = [&] { return wrong_owner_path; };
            wrong_owner_hooks.name_source = [](std::size_t attempt) {
                return scratchName(attempt);
            };
            const auto wrong_owner_acquire = acquireSecureScratchContext(wrong_owner_hooks);
            QVERIFY(!wrong_owner_acquire.has_value());
            QCOMPARE(wrong_owner_acquire.error().code,
                     SecureScratchFailureCode::EnvironmentInfeasible);
            const auto wrong_owner_result = importArchiveThroughSecureScratch(
                archive_path, PackArchiveLimits{}, PackValidationScope::Standalone,
                wrong_owner_hooks);
            QVERIFY(!wrong_owner_result.has_value());
            QCOMPARE(wrong_owner_result.error().code, ErrorCode::CannotRead);
            QVERIFY(QDir(wrong_owner_controller.path()).rmdir(QStringLiteral("wrong-owner")));
            QVERIFY(::chmod(encoded_controller.constData(), 0700) == 0);
        }
    }

    for (const auto* acl_name : {"system.posix_acl_access", "system.posix_acl_default"}) {
        QTemporaryDir acl_parent;
        QVERIFY(acl_parent.isValid());
        const auto encoded_parent = QFile::encodeName(acl_parent.path());
        const auto acl_bytes = namedUserAcl(::geteuid() == 65534 ? 65533 : 65534);
        QVERIFY2(::setxattr(encoded_parent.constData(), acl_name, acl_bytes.constData(),
                            static_cast<std::size_t>(acl_bytes.size()), 0) == 0,
                 qPrintable(QStringLiteral("setxattr(%1) failed: %2")
                                .arg(QString::fromLatin1(acl_name),
                                     QString::fromLocal8Bit(std::strerror(errno)))));
        SecureScratchHooks acl_hooks;
        acl_hooks.temp_path_provider = [&] { return acl_parent.path(); };
        acl_hooks.name_source = [](std::size_t attempt) { return scratchName(attempt); };
        const auto acl_result = importArchiveThroughSecureScratch(
            archive_path, PackArchiveLimits{}, PackValidationScope::Standalone, acl_hooks);
        QVERIFY(!acl_result.has_value());
        QCOMPARE(acl_result.error().code, ErrorCode::CannotRead);
        const auto acl_acquire = acquireSecureScratchContext(acl_hooks);
        QVERIFY(!acl_acquire.has_value());
        QCOMPARE(acl_acquire.error().code, SecureScratchFailureCode::EnvironmentInfeasible);
        QVERIFY(::removexattr(encoded_parent.constData(), acl_name) == 0);
    }

    struct EntryAclCase final {
        SecureScratchEvent transition;
        const char* acl_name;
    };
    const std::array entry_acl_cases{
        EntryAclCase{SecureScratchEvent::DirectoryNormalize, "system.posix_acl_access"},
        EntryAclCase{SecureScratchEvent::DirectoryNormalize, "system.posix_acl_default"},
        EntryAclCase{SecureScratchEvent::FileNormalize, "system.posix_acl_access"},
    };
    for (const auto& entry_acl_case : entry_acl_cases) {
        QTemporaryDir entry_acl_parent;
        QVERIFY(entry_acl_parent.isValid());
        const auto baseline = directoryEntries(entry_acl_parent.path());
        bool acl_installed = false;
        SecureScratchReport entry_acl_report;
        SecureScratchHooks entry_acl_hooks;
        entry_acl_hooks.temp_path_provider = [&] { return entry_acl_parent.path(); };
        entry_acl_hooks.name_source = [](std::size_t attempt) { return scratchName(attempt, 14); };
        entry_acl_hooks.observe = [&](const SecureScratchObservation& observation) {
            if (acl_installed || observation.event != entry_acl_case.transition) {
                return;
            }
            const auto encoded = QFile::encodeName(observation.absolute_path);
            const auto acl_bytes = namedUserAcl(::geteuid() == 65534 ? 65533 : 65534);
            acl_installed =
                ::setxattr(encoded.constData(), entry_acl_case.acl_name, acl_bytes.constData(),
                           static_cast<std::size_t>(acl_bytes.size()), 0) == 0;
        };
        entry_acl_hooks.report = &entry_acl_report;
        const auto entry_acl_result = importArchiveThroughSecureScratch(
            archive_path, PackArchiveLimits{}, PackValidationScope::Standalone, entry_acl_hooks);
        QVERIFY(acl_installed);
        QVERIFY(!entry_acl_result.has_value());
        QCOMPARE(entry_acl_result.error().code, ErrorCode::CannotRead);
        QCOMPARE(entry_acl_report.cleanup, SecureScratchCleanupOutcome::Preserved);
        QVERIFY(entry_acl_report.residue_identity_ambiguous);
        QVERIFY(QFileInfo::exists(entry_acl_report.workspace_path));
        QVERIFY(QDir(entry_acl_report.workspace_path).removeRecursively());
        QCOMPARE(directoryEntries(entry_acl_parent.path()), baseline);
    }

    const QByteArray maximum_name = QByteArray(31, 'a') + '-' + QByteArray(32, '0');
    QCOMPARE(maximum_name.size(), 64);
    QVERIFY(isValidSecureScratchName(maximum_name));
    for (const auto& invalid_name :
         {QByteArray{}, QByteArray(32, 'a') + '-' + QByteArray(32, '0'),
          QByteArrayLiteral("bad@-") + QByteArray(32, '0'),
          QByteArrayLiteral("bad/-") + QByteArray(32, '0'), QByteArrayLiteral("."),
          QByteArrayLiteral(".."), QByteArrayLiteral("a-") + QByteArray(31, '0'),
          QByteArrayLiteral("a-") + QByteArray(32, 'A')}) {
        QVERIFY(!isValidSecureScratchName(invalid_name));
        QTemporaryDir invalid_name_parent;
        QVERIFY(invalid_name_parent.isValid());
        SecureScratchReport name_report;
        SecureScratchHooks name_hooks;
        name_hooks.temp_path_provider = [&] { return invalid_name_parent.path(); };
        name_hooks.name_source = [invalid_name](std::size_t) { return invalid_name; };
        name_hooks.report = &name_report;
        const auto name_result = importArchiveThroughSecureScratch(
            archive_path, PackArchiveLimits{}, PackValidationScope::Standalone, name_hooks);
        QVERIFY(!name_result.has_value());
        QCOMPARE(name_result.error().code, ErrorCode::CannotRead);
        QVERIFY(name_report.failure_code.has_value());
        QCOMPARE(*name_report.failure_code, SecureScratchFailureCode::InvalidConfiguration);
        QCOMPARE(directoryEntries(invalid_name_parent.path()), QSet<QString>{});
    }
    {
        QTemporaryDir maximum_name_parent;
        QVERIFY(maximum_name_parent.isValid());
        SecureScratchHooks maximum_name_hooks;
        maximum_name_hooks.temp_path_provider = [&] { return maximum_name_parent.path(); };
        maximum_name_hooks.name_source = [maximum_name](std::size_t) { return maximum_name; };
        const auto maximum_name_result = importArchiveThroughSecureScratch(
            archive_path, PackArchiveLimits{}, PackValidationScope::Standalone, maximum_name_hooks);
        QVERIFY2(maximum_name_result.has_value(),
                 maximum_name_result ? "" : qPrintable(maximum_name_result.error().message));
        QCOMPARE(directoryEntries(maximum_name_parent.path()), QSet<QString>{});
    }

    const auto exercise_collisions = [&](std::size_t collision_count, bool expect_success) {
        QTemporaryDir collision_parent;
        QVERIFY(collision_parent.isValid());
        for (std::size_t attempt = 0; attempt < collision_count; ++attempt) {
            QVERIFY(QDir(collision_parent.path()).mkdir(QString::fromLatin1(scratchName(attempt))));
        }
        const auto baseline = directoryEntries(collision_parent.path());
        SecureScratchReport collision_report;
        SecureScratchHooks collision_hooks;
        collision_hooks.temp_path_provider = [&] { return collision_parent.path(); };
        collision_hooks.name_source = [](std::size_t attempt) { return scratchName(attempt); };
        collision_hooks.report = &collision_report;
        const auto result = importArchiveThroughSecureScratch(
            archive_path, PackArchiveLimits{}, PackValidationScope::Standalone, collision_hooks);
        QCOMPARE(result.has_value(), expect_success);
        if (!expect_success) {
            QCOMPARE(result.error().code, ErrorCode::CannotRead);
            QVERIFY(collision_report.failure_code.has_value());
            QCOMPARE(*collision_report.failure_code, SecureScratchFailureCode::OperationalFailure);
        }
        QCOMPARE(directoryEntries(collision_parent.path()), baseline);
    };
    exercise_collisions(127, true);
    exercise_collisions(128, false);

    for (std::size_t ordinal = 0; ordinal < 3; ++ordinal) {
        for (const auto action :
             {SecureScratchInjectedAction::FailBefore, SecureScratchInjectedAction::FailAfter}) {
            QTemporaryDir collision_fault_parent;
            QVERIFY(collision_fault_parent.isValid());
            for (std::size_t attempt = 0; attempt < 3; ++attempt) {
                QVERIFY(QDir(collision_fault_parent.path())
                            .mkdir(QString::fromLatin1(scratchName(attempt))));
            }
            const auto baseline = directoryEntries(collision_fault_parent.path());
            std::size_t seen = 0;
            bool injected = false;
            SecureScratchReport collision_fault_report;
            SecureScratchHooks collision_fault_hooks;
            collision_fault_hooks.temp_path_provider = [&] {
                return collision_fault_parent.path();
            };
            collision_fault_hooks.name_source = [](std::size_t attempt) {
                return scratchName(attempt);
            };
            collision_fault_hooks.inject = [&](const SecureScratchObservation& observation) {
                if (observation.event == SecureScratchEvent::NameCollision && seen++ == ordinal) {
                    injected = true;
                    return action;
                }
                return SecureScratchInjectedAction::Continue;
            };
            collision_fault_hooks.report = &collision_fault_report;
            const auto result = importArchiveThroughSecureScratch(archive_path, PackArchiveLimits{},
                                                                  PackValidationScope::Standalone,
                                                                  collision_fault_hooks);
            QVERIFY(injected);
            QVERIFY(!result.has_value());
            QCOMPARE(result.error().code, ErrorCode::CannotRead);
            QCOMPARE(collision_fault_report.cleanup, SecureScratchCleanupOutcome::NotAttempted);
            QVERIFY(collision_fault_report.workspace_path.isEmpty());
            QCOMPARE(directoryEntries(collision_fault_parent.path()), baseline);
        }
    }

    const std::array injected_events{
        SecureScratchEvent::TempPathCaptured,
        SecureScratchEvent::ControllerOpened,
        SecureScratchEvent::ControllerRebound,
        SecureScratchEvent::AccessAclProbe,
        SecureScratchEvent::DefaultAclProbe,
        SecureScratchEvent::NameCandidate,
        SecureScratchEvent::MemberFeasibility,
        SecureScratchEvent::DirectoryCreate,
        SecureScratchEvent::DirectoryRetainOpen,
        SecureScratchEvent::DirectoryRetained,
        SecureScratchEvent::DirectoryNormalize,
        SecureScratchEvent::DirectoryRebound,
        SecureScratchEvent::DirectorySync,
        SecureScratchEvent::FileCreate,
        SecureScratchEvent::FileRetainRebind,
        SecureScratchEvent::FileRetained,
        SecureScratchEvent::FileNormalize,
        SecureScratchEvent::FileRebound,
        SecureScratchEvent::FileWrite,
        SecureScratchEvent::FileSync,
        SecureScratchEvent::BeforeReader,
        SecureScratchEvent::AfterReader,
        SecureScratchEvent::InventoryRebind,
        SecureScratchEvent::DirectoryEnumerateOpen,
        SecureScratchEvent::DirectoryEnumerateRead,
        SecureScratchEvent::DirectoryEnumerateRebind,
        SecureScratchEvent::CleanupInspect,
        SecureScratchEvent::CleanupRemove,
        SecureScratchEvent::CleanupSync,
    };
    const auto first_workspace_create = std::find_if(
        action_observations.cbegin(), action_observations.cend(), [&](const auto& observation) {
            return observation.event == SecureScratchEvent::DirectoryCreate &&
                   observation.absolute_path == report.workspace_path;
        });
    const auto last_inventory = std::find_if(
        action_observations.crbegin(), action_observations.crend(), [](const auto& observation) {
            return observation.event == SecureScratchEvent::InventoryRebind;
        });
    const auto cleanup_phase_start = last_inventory.base();
    QVERIFY(first_workspace_create != action_observations.cend());
    QVERIFY(last_inventory != action_observations.crend());
    QVERIFY(cleanup_phase_start != action_observations.cend());
    for (const auto event : injected_events) {
        const auto occurrences = static_cast<std::size_t>(
            std::ranges::count(action_observations, event, &SecureScratchObservation::event));
        QVERIFY2(
            occurrences > 0,
            qPrintable(
                QStringLiteral("successful trace omitted event %1").arg(static_cast<int>(event))));
        for (std::size_t ordinal = 0; ordinal < occurrences; ++ordinal) {
            auto target = action_observations.cbegin();
            for (std::size_t seen = 0; seen <= ordinal; ++target) {
                QVERIFY(target != action_observations.cend());
                if (target->event == event) {
                    ++seen;
                }
            }
            --target;
            const auto before_workspace = target < first_workspace_create;
            const auto root_create_before = target == first_workspace_create;
            const auto cleanup_phase = target >= cleanup_phase_start;
            for (const auto action : {SecureScratchInjectedAction::FailBefore,
                                      SecureScratchInjectedAction::FailAfter}) {
                QTemporaryDir fault_parent;
                QVERIFY(fault_parent.isValid());
                const auto baseline = directoryEntries(fault_parent.path());
                std::size_t seen = 0;
                bool injected = false;
                SecureScratchReport fault_report;
                SecureScratchHooks fault_hooks;
                fault_hooks.temp_path_provider = [&] { return fault_parent.path(); };
                fault_hooks.name_source = [](std::size_t attempt) { return scratchName(attempt); };
                fault_hooks.inject = [&](const auto& observation) {
                    if (observation.event == event && seen++ == ordinal) {
                        injected = true;
                        return action;
                    }
                    return SecureScratchInjectedAction::Continue;
                };
                fault_hooks.report = &fault_report;
                const auto result =
                    importArchiveThroughSecureScratch(archive_path, PackArchiveLimits{},
                                                      PackValidationScope::Standalone, fault_hooks);
                QVERIFY(injected);
                QVERIFY(!result.has_value());
                const auto expected_code = event == SecureScratchEvent::MemberFeasibility
                                               ? ErrorCode::UnsafePath
                                               : ErrorCode::CannotRead;
                QVERIFY2(
                    result.error().code == expected_code,
                    qPrintable(QStringLiteral("event %1 ordinal %2 action %3 returned code %4: "
                                              "%5")
                                   .arg(static_cast<int>(event))
                                   .arg(ordinal)
                                   .arg(static_cast<int>(action))
                                   .arg(static_cast<int>(result.error().code))
                                   .arg(result.error().message)));
                const auto expected_cleanup =
                    cleanup_phase
                        ? SecureScratchCleanupOutcome::Preserved
                        : (before_workspace || (root_create_before &&
                                                action == SecureScratchInjectedAction::FailBefore)
                               ? SecureScratchCleanupOutcome::NotAttempted
                               : SecureScratchCleanupOutcome::Removed);
                QCOMPARE(fault_report.cleanup, expected_cleanup);
                if (expected_cleanup == SecureScratchCleanupOutcome::Preserved) {
                    QVERIFY(fault_report.residue_identity_ambiguous);
                } else {
                    QVERIFY(!fault_report.residue_identity_ambiguous);
                }
                auto reported_paths = fault_report.remaining_ledger_paths;
                for (auto& path : reported_paths) {
                    path = QDir::cleanPath(path);
                }
                reported_paths.sort();
                QCOMPARE(reported_paths, recursivePaths(fault_report.workspace_path));
                QCOMPARE(directoryEntries(fault_parent.path()), baseline);
            }
        }
    }

    QTemporaryDir cleanup_swap_parent;
    QVERIFY(cleanup_swap_parent.isValid());
    const auto cleanup_swap_baseline = directoryEntries(cleanup_swap_parent.path());
    bool cleanup_swap_performed = false;
    QString cleanup_swap_original;
    QString cleanup_swap_replacement;
    SecureScratchReport cleanup_swap_report;
    SecureScratchHooks cleanup_swap_hooks;
    cleanup_swap_hooks.temp_path_provider = [&] { return cleanup_swap_parent.path(); };
    cleanup_swap_hooks.name_source = [](std::size_t attempt) { return scratchName(attempt, 9); };
    cleanup_swap_hooks.inject = [&](const SecureScratchObservation& observation) {
        if (!cleanup_swap_performed && observation.event == SecureScratchEvent::CleanupRemove &&
            observation.absolute_path == cleanup_swap_report.workspace_path) {
            cleanup_swap_original = observation.absolute_path + QStringLiteral(".retained");
            cleanup_swap_replacement = observation.absolute_path;
            const auto original = QFile::encodeName(observation.absolute_path);
            const auto retained = QFile::encodeName(cleanup_swap_original);
            cleanup_swap_performed = ::rename(original.constData(), retained.constData()) == 0 &&
                                     ::mkdir(original.constData(), 0700) == 0;
        }
        return SecureScratchInjectedAction::Continue;
    };
    cleanup_swap_hooks.report = &cleanup_swap_report;
    const auto cleanup_swap_result = importArchiveThroughSecureScratch(
        archive_path, PackArchiveLimits{}, PackValidationScope::Standalone, cleanup_swap_hooks);
    QVERIFY(cleanup_swap_performed);
    QVERIFY(!cleanup_swap_result.has_value());
    QCOMPARE(cleanup_swap_result.error().code, ErrorCode::CannotRead);
    QCOMPARE(cleanup_swap_report.cleanup, SecureScratchCleanupOutcome::Preserved);
    QVERIFY(cleanup_swap_report.residue_identity_ambiguous);
    QVERIFY(QFileInfo(cleanup_swap_original).isDir());
    QVERIFY(QFileInfo(cleanup_swap_replacement).isDir());
    QVERIFY(QDir(cleanup_swap_replacement).removeRecursively());
    QVERIFY(QDir(cleanup_swap_original).removeRecursively());
    QCOMPARE(directoryEntries(cleanup_swap_parent.path()), cleanup_swap_baseline);

    QTemporaryDir normalize_swap_parent;
    QVERIFY(normalize_swap_parent.isValid());
    const auto normalize_swap_baseline = directoryEntries(normalize_swap_parent.path());
    bool normalize_swap_performed = false;
    QString normalize_swap_original;
    QString normalize_swap_replacement;
    SecureScratchReport normalize_swap_report;
    SecureScratchHooks normalize_swap_hooks;
    normalize_swap_hooks.temp_path_provider = [&] { return normalize_swap_parent.path(); };
    normalize_swap_hooks.name_source = [](std::size_t attempt) { return scratchName(attempt, 10); };
    normalize_swap_hooks.inject = [&](const SecureScratchObservation& observation) {
        if (!normalize_swap_performed &&
            observation.event == SecureScratchEvent::DirectoryNormalize &&
            observation.absolute_path.endsWith(QStringLiteral("/judges"))) {
            normalize_swap_replacement = observation.absolute_path;
            normalize_swap_original = observation.absolute_path + QStringLiteral(".retained");
            const auto replacement = QFile::encodeName(normalize_swap_replacement);
            const auto retained = QFile::encodeName(normalize_swap_original);
            normalize_swap_performed =
                ::rename(replacement.constData(), retained.constData()) == 0 &&
                ::mkdir(replacement.constData(), 0755) == 0;
        }
        return SecureScratchInjectedAction::Continue;
    };
    normalize_swap_hooks.report = &normalize_swap_report;
    const auto normalize_swap_result = importArchiveThroughSecureScratch(
        archive_path, PackArchiveLimits{}, PackValidationScope::Standalone, normalize_swap_hooks);
    QVERIFY(normalize_swap_performed);
    QVERIFY(!normalize_swap_result.has_value());
    QCOMPARE(normalize_swap_result.error().code, ErrorCode::CannotRead);
    QCOMPARE(normalize_swap_report.cleanup, SecureScratchCleanupOutcome::Preserved);
    QVERIFY(normalize_swap_report.residue_identity_ambiguous);
    struct stat normalize_replacement_status{};
    QVERIFY(::stat(QFile::encodeName(normalize_swap_replacement).constData(),
                   &normalize_replacement_status) == 0);
    QCOMPARE(static_cast<unsigned int>(normalize_replacement_status.st_mode & 07777), 0755U);
    QVERIFY(QFileInfo(normalize_swap_original).isDir());
    QVERIFY(QFileInfo(normalize_swap_replacement).isDir());
    QVERIFY(QDir(normalize_swap_report.workspace_path).removeRecursively());
    QCOMPARE(directoryEntries(normalize_swap_parent.path()), normalize_swap_baseline);

    QTemporaryDir replacement_parent;
    QVERIFY(replacement_parent.isValid());
    const auto replacement_baseline = directoryEntries(replacement_parent.path());
    bool replacement_created = false;
    QByteArray replacement_component;
    SecureScratchReport replacement_report;
    SecureScratchHooks replacement_hooks;
    replacement_hooks.temp_path_provider = [&] { return replacement_parent.path(); };
    replacement_hooks.name_source = [](std::size_t attempt) { return scratchName(attempt, 7); };
    replacement_hooks.observe = [&](const SecureScratchObservation& observation) {
        if (!replacement_created && observation.event == SecureScratchEvent::CleanupRemove &&
            observation.absolute_path == replacement_report.workspace_path) {
            replacement_component = observation.component;
            replacement_created =
                QDir(replacement_parent.path()).mkdir(QString::fromLatin1(replacement_component));
        }
    };
    replacement_hooks.report = &replacement_report;
    const auto replacement_result = importArchiveThroughSecureScratch(
        archive_path, PackArchiveLimits{}, PackValidationScope::Standalone, replacement_hooks);
    QVERIFY(replacement_created);
    QVERIFY(!replacement_result.has_value());
    QCOMPARE(replacement_result.error().code, ErrorCode::CannotRead);
    QCOMPARE(replacement_report.cleanup, SecureScratchCleanupOutcome::Preserved);
    QVERIFY(replacement_report.residue_identity_ambiguous);
    const auto replacement_path =
        QDir(replacement_parent.path()).filePath(QString::fromLatin1(replacement_component));
    QVERIFY(QFileInfo(replacement_path).isDir());
    QCOMPARE(replacement_report.remaining_ledger_paths,
             QStringList{QDir::cleanPath(replacement_path)});
    QVERIFY(QDir(replacement_parent.path()).rmdir(QString::fromLatin1(replacement_component)));
    QCOMPARE(directoryEntries(replacement_parent.path()), replacement_baseline);

    QTemporaryDir unexpected_parent;
    QVERIFY(unexpected_parent.isValid());
    const auto unexpected_baseline = directoryEntries(unexpected_parent.path());
    const auto unexpected_component = QByteArrayLiteral("audit-unexpected-entry");
    bool unexpected_created = false;
    SecureScratchReport unexpected_report;
    SecureScratchHooks unexpected_hooks;
    unexpected_hooks.temp_path_provider = [&] { return unexpected_parent.path(); };
    unexpected_hooks.name_source = [](std::size_t attempt) { return scratchName(attempt, 8); };
    unexpected_hooks.observe = [&](const SecureScratchObservation& observation) {
        if (!unexpected_created && observation.event == SecureScratchEvent::BeforeReader) {
            unexpected_created =
                QDir(observation.absolute_path).mkdir(QString::fromLatin1(unexpected_component));
        }
    };
    unexpected_hooks.report = &unexpected_report;
    const auto unexpected_result = importArchiveThroughSecureScratch(
        archive_path, PackArchiveLimits{}, PackValidationScope::Standalone, unexpected_hooks);
    QVERIFY(unexpected_created);
    QVERIFY(!unexpected_result.has_value());
    QCOMPARE(unexpected_result.error().code, ErrorCode::CannotRead);
    QCOMPARE(unexpected_report.cleanup, SecureScratchCleanupOutcome::Preserved);
    QVERIFY(unexpected_report.residue_identity_ambiguous);
    QCOMPARE(unexpected_report.unexpected_raw_paths, QList<QByteArray>{unexpected_component});
    const auto unexpected_path =
        QDir(unexpected_report.workspace_path).filePath(QString::fromLatin1(unexpected_component));
    QVERIFY(QFileInfo(unexpected_path).isDir());
    QVERIFY(QDir(unexpected_report.workspace_path).removeRecursively());
    QCOMPARE(directoryEntries(unexpected_parent.path()), unexpected_baseline);

    QTemporaryDir raw_unexpected_parent;
    QVERIFY(raw_unexpected_parent.isValid());
    const auto raw_unexpected_baseline = directoryEntries(raw_unexpected_parent.path());
    const QByteArray raw_unexpected_component("audit-\xff", 7);
    bool raw_unexpected_created = false;
    SecureScratchReport raw_unexpected_report;
    SecureScratchHooks raw_unexpected_hooks;
    raw_unexpected_hooks.temp_path_provider = [&] { return raw_unexpected_parent.path(); };
    raw_unexpected_hooks.name_source = [](std::size_t attempt) { return scratchName(attempt, 11); };
    raw_unexpected_hooks.observe = [&](const SecureScratchObservation& observation) {
        if (raw_unexpected_created || observation.event != SecureScratchEvent::BeforeReader) {
            return;
        }
        const auto workspace = QFile::encodeName(observation.absolute_path);
        const auto directory =
            ::open(workspace.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (directory < 0) {
            return;
        }
        const auto file = ::openat(directory, raw_unexpected_component.constData(),
                                   O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        raw_unexpected_created = file >= 0;
        if (file >= 0) {
            ::close(file);
        }
        ::close(directory);
    };
    raw_unexpected_hooks.report = &raw_unexpected_report;
    const auto raw_unexpected_result = importArchiveThroughSecureScratch(
        archive_path, PackArchiveLimits{}, PackValidationScope::Standalone, raw_unexpected_hooks);
    QVERIFY(raw_unexpected_created);
    QVERIFY(!raw_unexpected_result.has_value());
    QCOMPARE(raw_unexpected_result.error().code, ErrorCode::CannotRead);
    QCOMPARE(raw_unexpected_report.cleanup, SecureScratchCleanupOutcome::Preserved);
    QVERIFY(raw_unexpected_report.residue_identity_ambiguous);
    QCOMPARE(raw_unexpected_report.unexpected_raw_paths,
             QList<QByteArray>{raw_unexpected_component});
    const auto raw_workspace = QFile::encodeName(raw_unexpected_report.workspace_path);
    const auto raw_directory =
        ::open(raw_workspace.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    QVERIFY(raw_directory >= 0);
    struct stat raw_status{};
    QVERIFY(::fstatat(raw_directory, raw_unexpected_component.constData(), &raw_status,
                      AT_SYMLINK_NOFOLLOW) == 0);
    QVERIFY(S_ISREG(raw_status.st_mode));
    QVERIFY(::unlinkat(raw_directory, raw_unexpected_component.constData(), 0) == 0);
    ::close(raw_directory);
    QVERIFY(QDir(raw_unexpected_report.workspace_path).removeRecursively());
    QCOMPARE(directoryEntries(raw_unexpected_parent.path()), raw_unexpected_baseline);

    QTemporaryDir inventory_mutation_parent;
    QVERIFY(inventory_mutation_parent.isValid());
    const auto inventory_mutation_baseline = directoryEntries(inventory_mutation_parent.path());
    bool inventory_mutated = false;
    bool inventory_after_reader_observed = false;
    std::size_t inventory_rebind_callbacks = 0;
    const auto inventory_unexpected_component = QByteArrayLiteral("final-pass-extra");
    QString inventory_unexpected_path;
    SecureScratchReport inventory_mutation_report;
    SecureScratchHooks inventory_mutation_hooks;
    inventory_mutation_hooks.temp_path_provider = [&] { return inventory_mutation_parent.path(); };
    inventory_mutation_hooks.name_source = [](std::size_t attempt) {
        return scratchName(attempt, 12);
    };
    inventory_mutation_hooks.observe = [&](const SecureScratchObservation& observation) {
        inventory_after_reader_observed =
            inventory_after_reader_observed || observation.event == SecureScratchEvent::AfterReader;
        if (observation.event != SecureScratchEvent::InventoryRebind) {
            return;
        }
        ++inventory_rebind_callbacks;
        if (inventory_mutated || inventory_rebind_callbacks != first_inventory_rebind_count) {
            return;
        }
        inventory_unexpected_path =
            QDir(inventory_mutation_report.workspace_path)
                .filePath(QString::fromLatin1(inventory_unexpected_component));
        const auto unexpected = QFile::encodeName(inventory_unexpected_path);
        const auto file = ::open(unexpected.constData(),
                                 O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (file < 0) {
            return;
        }
        const QByteArray marker = QByteArrayLiteral("unexpected");
        inventory_mutated = ::write(file, marker.constData(),
                                    static_cast<std::size_t>(marker.size())) == marker.size();
        ::close(file);
    };
    inventory_mutation_hooks.report = &inventory_mutation_report;
    const auto inventory_mutation_result = importArchiveThroughSecureScratch(
        archive_path, PackArchiveLimits{}, PackValidationScope::Standalone,
        inventory_mutation_hooks);
    QVERIFY(inventory_mutated);
    QCOMPARE(inventory_rebind_callbacks, first_inventory_rebind_count);
    QVERIFY(!inventory_mutation_result.has_value());
    QCOMPARE(inventory_mutation_result.error().code, ErrorCode::CannotRead);
    QCOMPARE(inventory_mutation_report.cleanup, SecureScratchCleanupOutcome::Preserved);
    QVERIFY(inventory_mutation_report.residue_identity_ambiguous);
    QVERIFY(!inventory_after_reader_observed);
    QCOMPARE(inventory_mutation_report.unexpected_raw_paths,
             QList<QByteArray>{inventory_unexpected_component});
    QCOMPARE(readAll(inventory_unexpected_path), QByteArrayLiteral("unexpected"));
    QVERIFY(QDir(inventory_mutation_report.workspace_path).removeRecursively());
    QCOMPARE(directoryEntries(inventory_mutation_parent.path()), inventory_mutation_baseline);

    const auto segmentedPath = [](int segments) {
        QStringList values;
        values.reserve(segments);
        for (int index = 0; index < segments; ++index) {
            values.push_back(index + 1 == segments ? QStringLiteral("z") : QStringLiteral("a"));
        }
        return values.join(u'/');
    };
    const auto path120 = segmentedPath(120);
    const auto path121 = segmentedPath(121);
    QCOMPARE(path120.toUtf8().size(), 239);
    QCOMPARE(path121.toUtf8().size(), 241);
    const auto relocated_profile =
        readAll(QDir(source_path).filePath(QStringLiteral("judges/measured.json")));
    const auto source_manifest =
        QJsonDocument::fromJson(
            readAll(QDir(source_path).filePath(QStringLiteral("manifest.json"))))
            .object();
    QVERIFY(!relocated_profile.isEmpty());
    QVERIFY(!source_manifest.isEmpty());
    const auto write_relocated_archive = [&](const QString& path, const QString& member) {
        auto manifest = source_manifest;
        auto contents = manifest.value(QStringLiteral("contents")).toArray();
        auto descriptor = contents.at(0).toObject();
        descriptor.insert(QStringLiteral("path"), member);
        contents.replace(0, descriptor);
        manifest.insert(QStringLiteral("contents"), contents);
        return writeTestZip(path, {{QStringLiteral("manifest.json"),
                                    QJsonDocument(manifest).toJson(QJsonDocument::Compact)},
                                   {member, relocated_profile}});
    };
    const auto exercise_member_path = [&](const QString& member, bool expect_create) {
        QTemporaryDir member_archive_root;
        QVERIFY(member_archive_root.isValid());
        const auto path =
            QDir(member_archive_root.path()).filePath(QStringLiteral("member.awpack"));
        QVERIFY(write_relocated_archive(path, member));
        std::vector<SecureScratchObservation> member_observations;
        SecureScratchReport member_report;
        SecureScratchHooks member_hooks;
        member_hooks.temp_path_provider = [] { return QStringLiteral("/tmp"); };
        member_hooks.name_source = [](std::size_t attempt) { return scratchName(attempt); };
        member_hooks.observe = [&](const auto& observation) {
            member_observations.push_back(observation);
        };
        member_hooks.report = &member_report;
        const auto result = importArchiveThroughSecureScratch(
            path, PackArchiveLimits{}, PackValidationScope::Standalone, member_hooks);
        bool created = false;
        bool any_mutation = false;
        for (const auto& observation : member_observations) {
            created = created || (observation.event == SecureScratchEvent::FileCreate &&
                                  observation.absolute_path.endsWith(member));
            any_mutation = any_mutation ||
                           observation.event == SecureScratchEvent::DirectoryCreate ||
                           observation.event == SecureScratchEvent::FileCreate;
        }
        QCOMPARE(created, expect_create);
        if (expect_create) {
            QVERIFY2(result.has_value(), result ? "" : qPrintable(result.error().message));
        } else {
            QVERIFY(!result.has_value());
            QCOMPARE(result.error().code, member.toUtf8().size() > 240 ? ErrorCode::InvalidManifest
                                                                       : ErrorCode::UnsafePath);
            QVERIFY(!any_mutation);
        }
        QCOMPARE(member_report.cleanup, expect_create ? SecureScratchCleanupOutcome::Removed
                                                      : SecureScratchCleanupOutcome::NotAttempted);
        if (!expect_create) {
            QVERIFY(member_report.workspace_path.isEmpty());
        }
    };
    exercise_member_path(path120, true);
    exercise_member_path(path121, false);

    QTemporaryDir depth_root;
    QVERIFY(depth_root.isValid());
    auto deep_parent = depth_root.path();
    for (int index = absoluteComponentCount(deep_parent); index < 118; ++index) {
        deep_parent = QDir(deep_parent).filePath(QStringLiteral("d"));
        QVERIFY(QDir().mkdir(deep_parent));
    }
    QCOMPARE(absoluteComponentCount(deep_parent), 118);
    const auto exercise_absolute_depth = [&](int member_segments, bool expect_create) {
        QTemporaryDir depth_archive_root;
        QVERIFY(depth_archive_root.isValid());
        const auto member = segmentedPath(member_segments);
        const auto path = QDir(depth_archive_root.path()).filePath(QStringLiteral("depth.awpack"));
        QVERIFY(write_relocated_archive(path, member));
        std::vector<SecureScratchObservation> depth_observations;
        SecureScratchReport depth_report;
        SecureScratchHooks depth_hooks;
        depth_hooks.temp_path_provider = [&] { return deep_parent; };
        depth_hooks.name_source = [](std::size_t attempt) { return scratchName(attempt); };
        depth_hooks.observe = [&](const auto& observation) {
            depth_observations.push_back(observation);
        };
        depth_hooks.report = &depth_report;
        const auto result = importArchiveThroughSecureScratch(
            path, PackArchiveLimits{}, PackValidationScope::Standalone, depth_hooks);
        bool created = false;
        bool controller_opened = false;
        bool any_mutation = false;
        for (const auto& observation : depth_observations) {
            created = created || (observation.event == SecureScratchEvent::FileCreate &&
                                  observation.absolute_path.endsWith(member));
            controller_opened =
                controller_opened || observation.event == SecureScratchEvent::ControllerOpened;
            any_mutation = any_mutation ||
                           observation.event == SecureScratchEvent::DirectoryCreate ||
                           observation.event == SecureScratchEvent::FileCreate;
        }
        QCOMPARE(created, expect_create);
        if (expect_create) {
            QVERIFY2(result.has_value(), result ? "" : qPrintable(result.error().message));
        } else {
            QVERIFY(!result.has_value());
            QCOMPARE(result.error().code, ErrorCode::UnsafePath);
            QVERIFY(controller_opened);
            QVERIFY(!any_mutation);
        }
        QCOMPARE(depth_report.cleanup, expect_create ? SecureScratchCleanupOutcome::Removed
                                                     : SecureScratchCleanupOutcome::NotAttempted);
        if (!expect_create) {
            QVERIFY(depth_report.workspace_path.isEmpty());
        }
    };
    exercise_absolute_depth(9, true);
    exercise_absolute_depth(10, false);

    QTemporaryDir spelling_root;
    QVERIFY(spelling_root.isValid());
    QString nul_spelling = spelling_root.path() + QStringLiteral("/nul");
    nul_spelling.append(QChar::Null);
    QString replacement_spelling = spelling_root.path() + u'/';
    replacement_spelling.append(QChar(static_cast<char16_t>(0xfffd)));
    QString high_surrogate_spelling = spelling_root.path() + u'/';
    high_surrogate_spelling.append(QChar(static_cast<char16_t>(0xd800)));
    QString low_surrogate_spelling = spelling_root.path() + u'/';
    low_surrogate_spelling.append(QChar(static_cast<char16_t>(0xdc00)));
    const std::array invalid_parent_spellings{
        QString{},
        QStringLiteral("relative/scratch"),
        spelling_root.path() + QStringLiteral("/./child"),
        spelling_root.path() + QStringLiteral("/../child"),
        spelling_root.path() + QStringLiteral("//child"),
        nul_spelling,
        replacement_spelling,
        high_surrogate_spelling,
        low_surrogate_spelling,
    };
    const auto spelling_baseline = directoryEntries(spelling_root.path());
    for (const auto& parent_spelling : invalid_parent_spellings) {
        bool created = false;
        bool controller_opened = false;
        SecureScratchReport spelling_report;
        SecureScratchHooks spelling_hooks;
        spelling_hooks.temp_path_provider = [&] { return parent_spelling; };
        spelling_hooks.name_source = [](std::size_t attempt) { return scratchName(attempt); };
        spelling_hooks.observe = [&](const SecureScratchObservation& observation) {
            created = created || observation.event == SecureScratchEvent::DirectoryCreate ||
                      observation.event == SecureScratchEvent::FileCreate;
            controller_opened =
                controller_opened || observation.event == SecureScratchEvent::ControllerOpened;
        };
        spelling_hooks.report = &spelling_report;
        const auto spelling_acquire = acquireSecureScratchContext(spelling_hooks);
        QVERIFY(!spelling_acquire.has_value());
        QCOMPARE(spelling_acquire.error().code, SecureScratchFailureCode::EnvironmentInfeasible);
        QVERIFY(!controller_opened);
        const auto spelling_result = importArchiveThroughSecureScratch(
            archive_path, PackArchiveLimits{}, PackValidationScope::Standalone, spelling_hooks);
        QVERIFY(!spelling_result.has_value());
        QCOMPARE(spelling_result.error().code, ErrorCode::CannotRead);
        QVERIFY(!created);
        QVERIFY(!controller_opened);
        QCOMPARE(spelling_report.cleanup, SecureScratchCleanupOutcome::NotAttempted);
        QVERIFY(spelling_report.workspace_path.isEmpty());
        QCOMPARE(directoryEntries(spelling_root.path()), spelling_baseline);
    }

    QTemporaryDir count_root;
    QVERIFY(count_root.isValid());
    auto count_parent = count_root.path();
    while (absoluteComponentCount(count_parent) < 120) {
        count_parent = QDir(count_parent).filePath(QStringLiteral("q"));
        QVERIFY(QDir().mkdir(count_parent));
    }
    const auto parent_at_120 = count_parent;
    count_parent = QDir(count_parent).filePath(QStringLiteral("q"));
    QVERIFY(QDir().mkdir(count_parent));
    const auto parent_at_121 = count_parent;
    QCOMPARE(absoluteComponentCount(parent_at_120) + 8, 128);
    QCOMPARE(absoluteComponentCount(parent_at_121) + 8, 129);
    SecureScratchReport count_128_report;
    SecureScratchHooks count_128_hooks;
    count_128_hooks.temp_path_provider = [&] { return parent_at_120; };
    count_128_hooks.name_source = [](std::size_t attempt) { return scratchName(attempt); };
    count_128_hooks.report = &count_128_report;
    const auto count_128_acquire = acquireSecureScratchContext(count_128_hooks);
    QVERIFY(count_128_acquire.has_value());
    const auto count_128_result = importArchiveThroughSecureScratch(
        archive_path, PackArchiveLimits{}, PackValidationScope::Standalone, count_128_hooks);
    QVERIFY2(count_128_result.has_value(),
             count_128_result ? "" : qPrintable(count_128_result.error().message));
    QCOMPARE(count_128_report.cleanup, SecureScratchCleanupOutcome::Removed);
    QCOMPARE(directoryEntries(parent_at_120), QSet<QString>{QStringLiteral("q")});
    SecureScratchReport count_129_report;
    SecureScratchHooks count_129_hooks;
    count_129_hooks.temp_path_provider = [&] { return parent_at_121; };
    count_129_hooks.name_source = [](std::size_t attempt) { return scratchName(attempt); };
    bool count_129_controller_opened = false;
    bool count_129_mutated = false;
    count_129_hooks.observe = [&](const SecureScratchObservation& observation) {
        count_129_controller_opened = count_129_controller_opened ||
                                      observation.event == SecureScratchEvent::ControllerOpened;
        count_129_mutated = count_129_mutated ||
                            observation.event == SecureScratchEvent::DirectoryCreate ||
                            observation.event == SecureScratchEvent::FileCreate;
    };
    count_129_hooks.report = &count_129_report;
    const auto count_129_acquire = acquireSecureScratchContext(count_129_hooks);
    QVERIFY(!count_129_acquire.has_value());
    QCOMPARE(count_129_acquire.error().code, SecureScratchFailureCode::EnvironmentInfeasible);
    QVERIFY(!count_129_controller_opened);
    const auto count_129_result = importArchiveThroughSecureScratch(
        archive_path, PackArchiveLimits{}, PackValidationScope::Standalone, count_129_hooks);
    QVERIFY(!count_129_result.has_value());
    QCOMPARE(count_129_result.error().code, ErrorCode::CannotRead);
    QCOMPARE(count_129_report.cleanup, SecureScratchCleanupOutcome::NotAttempted);
    QVERIFY(count_129_report.workspace_path.isEmpty());
    QVERIFY(!count_129_controller_opened);
    QVERIFY(!count_129_mutated);
    QCOMPARE(directoryEntries(parent_at_121), QSet<QString>{});

    QTemporaryDir component_root;
    QVERIFY(component_root.isValid());
    const auto maximum_controller_component = QString(255, u'c');
    QVERIFY(QDir(component_root.path()).mkdir(maximum_controller_component));
    const auto maximum_component_parent =
        QDir(component_root.path()).filePath(maximum_controller_component);
    SecureScratchHooks maximum_component_hooks;
    maximum_component_hooks.temp_path_provider = [&] { return maximum_component_parent; };
    maximum_component_hooks.name_source = [](std::size_t attempt) { return scratchName(attempt); };
    const auto maximum_component_acquire = acquireSecureScratchContext(maximum_component_hooks);
    QVERIFY(maximum_component_acquire.has_value());
    const auto maximum_component_result =
        importArchiveThroughSecureScratch(archive_path, PackArchiveLimits{},
                                          PackValidationScope::Standalone, maximum_component_hooks);
    QVERIFY2(maximum_component_result.has_value(),
             maximum_component_result ? "" : qPrintable(maximum_component_result.error().message));
    QCOMPARE(directoryEntries(maximum_component_parent), QSet<QString>{});

    SecureScratchReport oversized_component_report;
    auto oversized_component_hooks = maximum_component_hooks;
    oversized_component_hooks.temp_path_provider = [&] {
        return QDir(component_root.path()).filePath(QString(256, u'c'));
    };
    bool oversized_component_controller_opened = false;
    bool oversized_component_mutated = false;
    oversized_component_hooks.observe = [&](const SecureScratchObservation& observation) {
        oversized_component_controller_opened =
            oversized_component_controller_opened ||
            observation.event == SecureScratchEvent::ControllerOpened;
        oversized_component_mutated = oversized_component_mutated ||
                                      observation.event == SecureScratchEvent::DirectoryCreate ||
                                      observation.event == SecureScratchEvent::FileCreate;
    };
    oversized_component_hooks.report = &oversized_component_report;
    const auto oversized_component_acquire = acquireSecureScratchContext(oversized_component_hooks);
    QVERIFY(!oversized_component_acquire.has_value());
    QCOMPARE(oversized_component_acquire.error().code,
             SecureScratchFailureCode::EnvironmentInfeasible);
    QVERIFY(!oversized_component_controller_opened);
    const auto oversized_component_result = importArchiveThroughSecureScratch(
        archive_path, PackArchiveLimits{}, PackValidationScope::Standalone,
        oversized_component_hooks);
    QVERIFY(!oversized_component_result.has_value());
    QCOMPARE(oversized_component_result.error().code, ErrorCode::CannotRead);
    QCOMPARE(oversized_component_report.cleanup, SecureScratchCleanupOutcome::NotAttempted);
    QVERIFY(oversized_component_report.workspace_path.isEmpty());
    QVERIFY(!oversized_component_controller_opened);
    QVERIFY(!oversized_component_mutated);

    const auto create_parent_with_payload = [](const QString& base,
                                               qsizetype target_bytes) -> std::optional<QString> {
        auto path = base;
        while (QFile::encodeName(path).size() < target_bytes) {
            const auto remaining = target_bytes - QFile::encodeName(path).size();
            auto addition = std::min<qsizetype>(256, remaining);
            if (remaining - addition == 1) {
                --addition;
            }
            if (addition < 2) {
                return std::nullopt;
            }
            path = QDir(path).filePath(QString(addition - 1, u'p'));
            if (!QDir().mkdir(path)) {
                return std::nullopt;
            }
        }
        return QFile::encodeName(path).size() == target_bytes ? std::optional<QString>{path}
                                                              : std::nullopt;
    };
    for (const auto target_bytes : {qsizetype{3'575}, qsizetype{3'576}}) {
        QTemporaryDir payload_root;
        QVERIFY(payload_root.isValid());
        const auto payload_parent = create_parent_with_payload(payload_root.path(), target_bytes);
        QVERIFY(payload_parent.has_value());
        QCOMPARE(QFile::encodeName(*payload_parent).size(), target_bytes);
        SecureScratchReport payload_report;
        SecureScratchHooks payload_hooks;
        payload_hooks.temp_path_provider = [&] { return *payload_parent; };
        payload_hooks.name_source = [](std::size_t attempt) { return scratchName(attempt); };
        bool payload_controller_opened = false;
        bool payload_mutated = false;
        payload_hooks.observe = [&](const SecureScratchObservation& observation) {
            payload_controller_opened = payload_controller_opened ||
                                        observation.event == SecureScratchEvent::ControllerOpened;
            payload_mutated = payload_mutated ||
                              observation.event == SecureScratchEvent::DirectoryCreate ||
                              observation.event == SecureScratchEvent::FileCreate;
        };
        payload_hooks.report = &payload_report;
        const auto payload_acquire = acquireSecureScratchContext(payload_hooks);
        QCOMPARE(payload_acquire.has_value(), target_bytes == 3'575);
        if (!payload_acquire.has_value()) {
            QCOMPARE(payload_acquire.error().code, SecureScratchFailureCode::EnvironmentInfeasible);
            QVERIFY(!payload_controller_opened);
        }
        const auto payload_result = importArchiveThroughSecureScratch(
            archive_path, PackArchiveLimits{}, PackValidationScope::Standalone, payload_hooks);
        if (target_bytes == 3'575) {
            QVERIFY2(payload_result.has_value(),
                     payload_result ? "" : qPrintable(payload_result.error().message));
            QCOMPARE(payload_report.cleanup, SecureScratchCleanupOutcome::Removed);
            QCOMPARE(directoryEntries(*payload_parent), QSet<QString>{});
        } else {
            QVERIFY(!payload_result.has_value());
            QCOMPARE(payload_result.error().code, ErrorCode::CannotRead);
            QCOMPARE(payload_report.cleanup, SecureScratchCleanupOutcome::NotAttempted);
            QVERIFY(payload_report.workspace_path.isEmpty());
            QVERIFY(!payload_controller_opened);
            QVERIFY(!payload_mutated);
        }
    }
#endif
}

} // namespace

QTEST_GUILESS_MAIN(PackArchiveTest)

#include "tst_pack_archive.moc"
