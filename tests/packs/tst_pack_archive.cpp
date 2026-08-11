#include "appellate/packs/pack_archive.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QtGlobal>

#include <cstdint>
#include <ctime>
#include <memory>
#include <utility>
#include <vector>

namespace {

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

} // namespace

QTEST_GUILESS_MAIN(PackArchiveTest)

#include "tst_pack_archive.moc"
