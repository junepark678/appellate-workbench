#include "appellate/packs/pack_archive.hpp"
#include "appellate/packs/schema_validator.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace appellate::packs {
namespace {

constexpr std::uint32_t local_header_signature = 0x04034b50U;
constexpr std::uint32_t central_header_signature = 0x02014b50U;
constexpr std::uint32_t end_of_central_directory_signature = 0x06054b50U;
constexpr std::uint32_t data_descriptor_signature = 0x08074b50U;
constexpr std::uint16_t encrypted_flag_mask = 0x2041U;
constexpr std::uint16_t data_descriptor_flag = 0x0008U;
constexpr std::uint16_t stored_compression_method = 0;
constexpr std::uint64_t maximum_central_directory_bytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t maximum_extra_field_bytes = 4ULL * 1024ULL;
constexpr std::uint64_t maximum_standard_zip_value =
    static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
constexpr std::size_t maximum_standard_zip_members =
    static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max());
constexpr auto fixed_zip_timestamp = static_cast<time_t>(315'532'800);
constexpr std::uint64_t maximum_manifest_bytes = 1024ULL * 1024ULL;
constexpr qsizetype maximum_declared_payloads = 10'000;
constexpr qsizetype pdf_tail_bytes = 1024;

struct ArchiveReadDeleter final {
    void operator()(archive* value) const noexcept {
        if (value != nullptr) {
            archive_read_free(value);
        }
    }
};

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

using ArchiveRead = std::unique_ptr<archive, ArchiveReadDeleter>;
using ArchiveWrite = std::unique_ptr<archive, ArchiveWriteDeleter>;
using ArchiveEntry = std::unique_ptr<archive_entry, ArchiveEntryDeleter>;

struct ZipMember final {
    QString path;
    std::uint16_t flags{};
    std::uint32_t crc32{};
    std::uint32_t compressed_size{};
    std::uint32_t uncompressed_size{};
    std::uint32_t local_header_offset{};
};

struct ZipInspection final {
    std::vector<ZipMember> members;
    std::uint64_t total_uncompressed_bytes{};
};

struct ExportMember final {
    QString path;
    QString absolute_path;
    std::uint64_t size{};
};

[[nodiscard]] auto fail(ErrorCode code, QString message) -> std::unexpected<Error> {
    return std::unexpected(Error{code, std::move(message)});
}

[[nodiscard]] QString libarchiveMessage(archive* value, const QString& operation) {
    const auto* detail = archive_error_string(value);
    return detail == nullptr
               ? operation
               : QStringLiteral("%1: %2").arg(operation, QString::fromLocal8Bit(detail));
}

[[nodiscard]] bool isReservedPathSegment(const QString& segment) {
    static const QSet<QString> reserved{
        QStringLiteral("con"),  QStringLiteral("prn"),  QStringLiteral("aux"),
        QStringLiteral("nul"),  QStringLiteral("com1"), QStringLiteral("com2"),
        QStringLiteral("com3"), QStringLiteral("com4"), QStringLiteral("com5"),
        QStringLiteral("com6"), QStringLiteral("com7"), QStringLiteral("com8"),
        QStringLiteral("com9"), QStringLiteral("lpt1"), QStringLiteral("lpt2"),
        QStringLiteral("lpt3"), QStringLiteral("lpt4"), QStringLiteral("lpt5"),
        QStringLiteral("lpt6"), QStringLiteral("lpt7"), QStringLiteral("lpt8"),
        QStringLiteral("lpt9"),
    };
    return reserved.contains(segment.section(u'.', 0, 0));
}

[[nodiscard]] bool isPortablePath(const QString& path) {
    static const QRegularExpression pattern(QStringLiteral(
        R"(^[a-z0-9](?:[a-z0-9._-]*[a-z0-9_-])?(?:/[a-z0-9](?:[a-z0-9._-]*[a-z0-9_-])?)*$)"));
    if (path.isEmpty() || path.size() > 240 || QDir::isAbsolutePath(path) ||
        QDir::cleanPath(path) != path || !pattern.match(path).hasMatch()) {
        return false;
    }
    return std::ranges::none_of(
        path.split(u'/'), [](const QString& segment) { return isReservedPathSegment(segment); });
}

[[nodiscard]] bool isAsciiPath(QByteArrayView value) {
    return std::ranges::all_of(value, [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte >= 0x20U && byte <= 0x7eU;
    });
}

[[nodiscard]] bool validLimits(const PackArchiveLimits& limits) {
    return limits.maximum_members > 0 && limits.maximum_members <= maximum_standard_zip_members &&
           limits.maximum_file_bytes > 0 &&
           limits.maximum_file_bytes <= maximum_standard_zip_value &&
           limits.maximum_total_bytes > 0 &&
           limits.maximum_total_bytes <= maximum_standard_zip_value;
}

[[nodiscard]] std::uint16_t little16(QByteArrayView value, qsizetype offset) {
    const auto first = static_cast<std::uint16_t>(static_cast<unsigned char>(value.at(offset)));
    const auto second =
        static_cast<std::uint16_t>(static_cast<unsigned char>(value.at(offset + 1)));
    return static_cast<std::uint16_t>(first | static_cast<std::uint16_t>(second << 8U));
}

[[nodiscard]] std::uint32_t little32(QByteArrayView value, qsizetype offset) {
    std::uint32_t result = 0;
    for (qsizetype index = 0; index < 4; ++index) {
        const auto byte =
            static_cast<std::uint32_t>(static_cast<unsigned char>(value.at(offset + index)));
        result |= byte << static_cast<unsigned>(index * 8);
    }
    return result;
}

[[nodiscard]] bool rangeFits(std::uint64_t offset, std::uint64_t length, std::uint64_t boundary) {
    return offset <= boundary && length <= boundary - offset;
}

[[nodiscard]] auto readExactly(QFile& file, std::uint64_t offset, std::uint64_t length)
    -> std::expected<QByteArray, Error> {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<qint64>::max()) ||
        length > static_cast<std::uint64_t>(std::numeric_limits<qint64>::max()) ||
        !file.seek(static_cast<qint64>(offset))) {
        return fail(ErrorCode::CannotRead, QStringLiteral("Cannot seek within archive"));
    }
    const auto bytes = file.read(static_cast<qint64>(length));
    if (bytes.size() != static_cast<qsizetype>(length)) {
        return fail(ErrorCode::CannotRead, QStringLiteral("Truncated archive"));
    }
    return bytes;
}

[[nodiscard]] auto inspectZip(const QString& archive_path, const PackArchiveLimits& limits)
    -> std::expected<ZipInspection, Error> {
    if (!validLimits(limits)) {
        return fail(ErrorCode::ResourceTooLarge, QStringLiteral("Invalid archive limits"));
    }
    const QFileInfo archive_info(archive_path);
    if (archive_info.isSymLink()) {
        return fail(ErrorCode::UnsafePath,
                    QStringLiteral("Archive path cannot be a symbolic link"));
    }
    if (!archive_info.isFile() || archive_info.size() < 22) {
        return fail(ErrorCode::CannotRead, QStringLiteral("Archive is missing or truncated"));
    }
    const auto archive_size = static_cast<std::uint64_t>(archive_info.size());
    const auto physical_limit =
        limits.maximum_total_bytes + maximum_central_directory_bytes + 16ULL * 1024ULL * 1024ULL;
    if (archive_size > physical_limit) {
        return fail(ErrorCode::ResourceTooLarge, QStringLiteral("Archive exceeds its size limit"));
    }

    QFile file(archive_path);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(ErrorCode::CannotRead, QStringLiteral("Cannot open archive"));
    }
    const auto tail_size = std::min<std::uint64_t>(archive_size, 65'557ULL);
    const auto tail_result = readExactly(file, archive_size - tail_size, tail_size);
    if (!tail_result) {
        return std::unexpected(tail_result.error());
    }
    const auto& tail = *tail_result;
    std::optional<qsizetype> end_offset;
    for (auto offset = tail.size() - 22; offset >= 0; --offset) {
        if (little32(QByteArrayView(tail), offset) == end_of_central_directory_signature &&
            offset + 22 + little16(QByteArrayView(tail), offset + 20) == tail.size()) {
            end_offset = offset;
            break;
        }
        if (offset == 0) {
            break;
        }
    }
    if (!end_offset) {
        return fail(ErrorCode::InvalidManifest,
                    QStringLiteral("Archive has no valid end-of-central-directory record"));
    }
    const auto end = QByteArrayView(tail).sliced(*end_offset, 22);
    if (little16(end, 4) != 0 || little16(end, 6) != 0 || little16(end, 20) != 0 ||
        little16(end, 8) != little16(end, 10)) {
        return fail(ErrorCode::InvalidManifest,
                    QStringLiteral("Multi-disk and commented ZIP files are not supported"));
    }
    const auto member_count = static_cast<std::size_t>(little16(end, 10));
    const auto central_size = static_cast<std::uint64_t>(little32(end, 12));
    const auto central_offset = static_cast<std::uint64_t>(little32(end, 16));
    const auto absolute_end_offset =
        archive_size - tail_size + static_cast<std::uint64_t>(*end_offset);
    if (member_count == 0 || member_count > limits.maximum_members ||
        central_size > maximum_central_directory_bytes ||
        !rangeFits(central_offset, central_size, absolute_end_offset) ||
        central_offset + central_size != absolute_end_offset) {
        return fail(member_count > limits.maximum_members ? ErrorCode::ResourceTooLarge
                                                          : ErrorCode::InvalidManifest,
                    QStringLiteral("Invalid or oversized ZIP central directory"));
    }

    const auto central_result = readExactly(file, central_offset, central_size);
    if (!central_result) {
        return std::unexpected(central_result.error());
    }
    const auto& central_bytes = *central_result;
    const auto central = QByteArrayView(central_bytes);
    qsizetype cursor = 0;
    ZipInspection inspection;
    inspection.members.reserve(member_count);
    QSet<QString> paths;
    for (std::size_t index = 0; index < member_count; ++index) {
        if (cursor < 0 || cursor + 46 > central.size() ||
            little32(central, cursor) != central_header_signature) {
            return fail(ErrorCode::InvalidManifest,
                        QStringLiteral("Malformed ZIP central-directory entry"));
        }
        const auto version_made_by = little16(central, cursor + 4);
        const auto flags = little16(central, cursor + 8);
        const auto method = little16(central, cursor + 10);
        const auto crc32 = little32(central, cursor + 16);
        const auto compressed_size = little32(central, cursor + 20);
        const auto uncompressed_size = little32(central, cursor + 24);
        const auto name_size = static_cast<qsizetype>(little16(central, cursor + 28));
        const auto extra_size = static_cast<qsizetype>(little16(central, cursor + 30));
        const auto comment_size = static_cast<qsizetype>(little16(central, cursor + 32));
        const auto disk_start = little16(central, cursor + 34);
        const auto external_attributes = little32(central, cursor + 38);
        const auto local_offset = little32(central, cursor + 42);
        const auto variable_size = name_size + extra_size + comment_size;
        if (name_size <= 0 || name_size > 240 ||
            static_cast<std::uint64_t>(extra_size) > maximum_extra_field_bytes ||
            comment_size != 0 || cursor + 46 + variable_size > central.size() || disk_start != 0 ||
            (flags & encrypted_flag_mask) != 0 || method != stored_compression_method ||
            compressed_size != uncompressed_size ||
            compressed_size == std::numeric_limits<std::uint32_t>::max() ||
            local_offset == std::numeric_limits<std::uint32_t>::max()) {
            const auto code = compressed_size > limits.maximum_file_bytes
                                  ? ErrorCode::ResourceTooLarge
                                  : ErrorCode::InvalidManifest;
            return fail(code, QStringLiteral("Unsupported ZIP member metadata"));
        }
        const auto name_bytes = central.sliced(cursor + 46, name_size);
        if (!isAsciiPath(name_bytes)) {
            return fail(ErrorCode::UnsafePath, QStringLiteral("ZIP member path is not portable"));
        }
        const auto path = QString::fromLatin1(name_bytes.data(), name_bytes.size());
        if (!isPortablePath(path)) {
            return fail(ErrorCode::UnsafePath,
                        QStringLiteral("Unsafe ZIP member path: %1").arg(path));
        }
        if (paths.contains(path)) {
            return fail(ErrorCode::DuplicateContentPath,
                        QStringLiteral("Duplicate ZIP member path: %1").arg(path));
        }
        const auto creator_system = static_cast<std::uint16_t>(version_made_by >> 8U);
        const auto unix_mode = static_cast<std::uint16_t>(external_attributes >> 16U);
        if (creator_system == 3U && (unix_mode & AE_IFMT) != 0U &&
            (unix_mode & AE_IFMT) != AE_IFREG) {
            return fail(
                ErrorCode::UnsafePath,
                QStringLiteral("ZIP links and special entries are forbidden: %1").arg(path));
        }
        if (uncompressed_size > limits.maximum_file_bytes ||
            uncompressed_size > limits.maximum_total_bytes ||
            inspection.total_uncompressed_bytes > limits.maximum_total_bytes - uncompressed_size) {
            return fail(ErrorCode::ResourceTooLarge, QStringLiteral("ZIP member limits exceeded"));
        }
        paths.insert(path);
        inspection.total_uncompressed_bytes += uncompressed_size;
        inspection.members.push_back(
            ZipMember{path, flags, crc32, compressed_size, uncompressed_size, local_offset});
        cursor += 46 + variable_size;
    }
    if (cursor != central.size()) {
        return fail(ErrorCode::InvalidManifest,
                    QStringLiteral("Unexpected data in ZIP central directory"));
    }

    struct LocalRange final {
        std::uint64_t begin{};
        std::uint64_t end{};
    };
    std::vector<LocalRange> ranges;
    ranges.reserve(inspection.members.size());
    for (const auto& member : inspection.members) {
        const auto header_result = readExactly(file, member.local_header_offset, 30);
        if (!header_result) {
            return std::unexpected(header_result.error());
        }
        const auto header = QByteArrayView(*header_result);
        const auto flags = little16(header, 6);
        const auto method = little16(header, 8);
        const auto local_crc32 = little32(header, 14);
        const auto local_compressed_size = little32(header, 18);
        const auto local_uncompressed_size = little32(header, 22);
        const auto name_size = static_cast<std::uint64_t>(little16(header, 26));
        const auto extra_size = static_cast<std::uint64_t>(little16(header, 28));
        if (little32(header, 0) != local_header_signature || flags != member.flags ||
            method != stored_compression_method || (flags & encrypted_flag_mask) != 0 ||
            name_size == 0 || name_size > 240 || extra_size > maximum_extra_field_bytes) {
            return fail(ErrorCode::InvalidManifest, QStringLiteral("Malformed ZIP local header"));
        }
        const auto variable_result =
            readExactly(file, static_cast<std::uint64_t>(member.local_header_offset) + 30,
                        name_size + extra_size);
        if (!variable_result) {
            return std::unexpected(variable_result.error());
        }
        const auto local_name =
            QByteArrayView(*variable_result).first(static_cast<qsizetype>(name_size));
        if (QString::fromLatin1(local_name.data(), local_name.size()) != member.path) {
            return fail(ErrorCode::InvalidManifest,
                        QStringLiteral("ZIP local and central paths differ"));
        }
        const auto data_begin =
            static_cast<std::uint64_t>(member.local_header_offset) + 30 + name_size + extra_size;
        if (!rangeFits(data_begin, member.compressed_size, central_offset)) {
            return fail(ErrorCode::InvalidManifest,
                        QStringLiteral("ZIP member data overlaps metadata"));
        }
        auto record_end = data_begin + member.compressed_size;
        if ((flags & data_descriptor_flag) == 0U) {
            if (local_crc32 != member.crc32 || local_compressed_size != member.compressed_size ||
                local_uncompressed_size != member.uncompressed_size) {
                return fail(ErrorCode::InvalidManifest,
                            QStringLiteral("ZIP local and central sizes differ"));
            }
        } else {
            const auto descriptor_result = readExactly(file, record_end, 16);
            if (!descriptor_result) {
                return std::unexpected(descriptor_result.error());
            }
            const auto descriptor = QByteArrayView(*descriptor_result);
            qsizetype descriptor_offset = 0;
            std::uint64_t descriptor_size = 12;
            if (little32(descriptor, 0) == data_descriptor_signature) {
                descriptor_offset = 4;
                descriptor_size = 16;
            }
            if (little32(descriptor, descriptor_offset) != member.crc32 ||
                little32(descriptor, descriptor_offset + 4) != member.compressed_size ||
                little32(descriptor, descriptor_offset + 8) != member.uncompressed_size) {
                return fail(ErrorCode::InvalidManifest,
                            QStringLiteral("Invalid ZIP data descriptor"));
            }
            record_end += descriptor_size;
        }
        if (record_end > central_offset) {
            return fail(ErrorCode::InvalidManifest, QStringLiteral("ZIP member crosses directory"));
        }
        ranges.push_back(LocalRange{member.local_header_offset, record_end});
    }
    std::ranges::sort(ranges, {}, &LocalRange::begin);
    if (ranges.front().begin != 0 || ranges.back().end != central_offset) {
        return fail(ErrorCode::InvalidManifest,
                    QStringLiteral("ZIP contains hidden data outside its members"));
    }
    for (std::size_t index = 1; index < ranges.size(); ++index) {
        if (ranges.at(index - 1).end != ranges.at(index).begin) {
            return fail(ErrorCode::InvalidManifest,
                        QStringLiteral("ZIP members overlap or contain hidden gaps"));
        }
    }
    return inspection;
}

[[nodiscard]] auto openArchiveReader(const QString& archive_path)
    -> std::expected<ArchiveRead, Error> {
    ArchiveRead reader(archive_read_new());
    if (!reader) {
        return fail(ErrorCode::CannotRead, QStringLiteral("Cannot allocate ZIP reader"));
    }
    if (archive_read_support_filter_none(reader.get()) != ARCHIVE_OK ||
        archive_read_support_format_zip_seekable(reader.get()) != ARCHIVE_OK) {
        return fail(ErrorCode::CannotRead,
                    libarchiveMessage(reader.get(), QStringLiteral("Cannot configure ZIP reader")));
    }
#ifdef Q_OS_WIN
    const auto open_result = archive_read_open_filename_w(
        reader.get(), reinterpret_cast<const wchar_t*>(archive_path.utf16()), 64 * 1024);
#else
    const auto encoded_path = QFile::encodeName(archive_path);
    const auto open_result =
        archive_read_open_filename(reader.get(), encoded_path.constData(), 64 * 1024);
#endif
    if (open_result != ARCHIVE_OK) {
        return fail(ErrorCode::CannotRead,
                    libarchiveMessage(reader.get(), QStringLiteral("Cannot open ZIP archive")));
    }
    return reader;
}

[[nodiscard]] auto validateManifestMemberSet(const QString& archive_path,
                                             const ZipInspection& inspection)
    -> std::expected<QJsonObject, Error> {
    const auto manifest_member =
        std::ranges::find(inspection.members, QStringLiteral("manifest.json"), &ZipMember::path);
    if (manifest_member == inspection.members.end()) {
        return fail(ErrorCode::InvalidManifest,
                    QStringLiteral("Archive does not contain manifest.json"));
    }
    if (manifest_member->uncompressed_size > maximum_manifest_bytes) {
        return fail(ErrorCode::ResourceTooLarge,
                    QStringLiteral("Archive manifest exceeds its size limit"));
    }

    auto reader_result = openArchiveReader(archive_path);
    if (!reader_result) {
        return std::unexpected(reader_result.error());
    }
    auto reader = std::move(*reader_result);
    QHash<QString, std::uint32_t> inspected_sizes;
    inspected_sizes.reserve(static_cast<qsizetype>(inspection.members.size()));
    for (const auto& member : inspection.members) {
        inspected_sizes.insert(member.path, member.uncompressed_size);
    }
    QByteArray manifest_bytes;
    manifest_bytes.reserve(static_cast<qsizetype>(manifest_member->uncompressed_size));
    std::array<char, 64 * 1024> buffer{};
    bool found_manifest = false;
    archive_entry* raw_entry = nullptr;
    while (!found_manifest) {
        const auto header_result = archive_read_next_header(reader.get(), &raw_entry);
        if (header_result == ARCHIVE_EOF) {
            break;
        }
        if (header_result != ARCHIVE_OK || raw_entry == nullptr) {
            return fail(ErrorCode::InvalidManifest,
                        libarchiveMessage(reader.get(), QStringLiteral("Cannot read ZIP member")));
        }
        const auto* raw_path = archive_entry_pathname(raw_entry);
        const auto path = raw_path == nullptr ? QString{} : QString::fromLatin1(raw_path);
        const auto declared_size = archive_entry_size(raw_entry);
        if (!inspected_sizes.contains(path)) {
            return fail(ErrorCode::UnsafePath,
                        QStringLiteral("Unexpected ZIP member path: %1").arg(path));
        }
        if (declared_size < 0 ||
            static_cast<std::uint64_t>(declared_size) != inspected_sizes.value(path)) {
            return fail(ErrorCode::InvalidManifest,
                        QStringLiteral("ZIP member size changed after inspection"));
        }
        if (archive_entry_filetype(raw_entry) != AE_IFREG ||
            archive_entry_symlink(raw_entry) != nullptr ||
            archive_entry_hardlink(raw_entry) != nullptr ||
            archive_entry_is_encrypted(raw_entry) > 0 ||
            archive_entry_size_is_set(raw_entry) == 0) {
            return fail(ErrorCode::UnsafePath,
                        QStringLiteral("ZIP member is not a plain regular file: %1").arg(path));
        }
        if (path != QStringLiteral("manifest.json")) {
            if (archive_read_data_skip(reader.get()) != ARCHIVE_OK) {
                return fail(
                    ErrorCode::InvalidManifest,
                    libarchiveMessage(reader.get(), QStringLiteral("Cannot skip ZIP member")));
            }
            continue;
        }
        std::uint64_t total = 0;
        while (true) {
            const auto read_size = archive_read_data(reader.get(), buffer.data(), buffer.size());
            if (read_size < 0) {
                return fail(
                    ErrorCode::InvalidManifest,
                    libarchiveMessage(reader.get(), QStringLiteral("Cannot read ZIP manifest")));
            }
            if (read_size == 0) {
                break;
            }
            const auto chunk_size = static_cast<std::uint64_t>(read_size);
            if (chunk_size > manifest_member->uncompressed_size ||
                total > manifest_member->uncompressed_size - chunk_size) {
                return fail(ErrorCode::InvalidManifest,
                            QStringLiteral("ZIP manifest exceeds its declared size"));
            }
            manifest_bytes.append(buffer.data(), static_cast<qsizetype>(read_size));
            total += chunk_size;
        }
        if (total != manifest_member->uncompressed_size) {
            return fail(ErrorCode::InvalidManifest, QStringLiteral("ZIP manifest is truncated"));
        }
        found_manifest = true;
    }
    if (!found_manifest) {
        return fail(ErrorCode::InvalidManifest,
                    QStringLiteral("Archive manifest could not be read"));
    }

    const auto parsed =
        SchemaValidator::parseObject(manifest_bytes, QStringLiteral("manifest.json"));
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    const auto& manifest = *parsed;
    if (manifest.size() != 7 || !manifest.contains(QStringLiteral("schema_version")) ||
        !manifest.contains(QStringLiteral("pack_id")) ||
        !manifest.contains(QStringLiteral("version")) ||
        !manifest.contains(QStringLiteral("required_capabilities")) ||
        !manifest.contains(QStringLiteral("dependencies")) ||
        !manifest.value(QStringLiteral("contents")).isArray() ||
        !manifest.value(QStringLiteral("blobs")).isArray()) {
        return fail(ErrorCode::InvalidManifest,
                    QStringLiteral("Archive manifest cannot safely declare its member set"));
    }
    const auto schema_version = manifest.value(QStringLiteral("schema_version"));
    if (!schema_version.isDouble() ||
        (schema_version.toDouble() != 1.0 && schema_version.toDouble() != 2.0)) {
        return fail(ErrorCode::UnsupportedSchema,
                    QStringLiteral("Unsupported manifest schema version"));
    }
    const auto contents = manifest.value(QStringLiteral("contents")).toArray();
    const auto blobs = manifest.value(QStringLiteral("blobs")).toArray();
    if (contents.isEmpty() || contents.size() > maximum_declared_payloads ||
        blobs.size() > maximum_declared_payloads - contents.size()) {
        return fail(ErrorCode::InvalidManifest,
                    QStringLiteral("Archive manifest member count is invalid"));
    }

    QSet<QString> declared{QStringLiteral("manifest.json")};
    std::vector<QString> payload_paths;
    payload_paths.reserve(static_cast<std::size_t>(contents.size() + blobs.size()));
    const auto addPaths = [&declared,
                           &payload_paths](const QJsonArray& values) -> std::expected<void, Error> {
        for (const auto& value : values) {
            if (!value.isObject()) {
                return fail(ErrorCode::InvalidManifest,
                            QStringLiteral("Archive descriptor is not an object"));
            }
            const auto path_value = value.toObject().value(QStringLiteral("path"));
            const auto path = path_value.toString();
            if (!path_value.isString() || !isPortablePath(path) ||
                path == QStringLiteral("manifest.json")) {
                return fail(ErrorCode::UnsafePath,
                            QStringLiteral("Unsafe archive descriptor path: %1").arg(path));
            }
            if (declared.contains(path)) {
                return fail(ErrorCode::DuplicateContentPath,
                            QStringLiteral("Duplicate archive path: %1").arg(path));
            }
            payload_paths.push_back(path);
            declared.insert(path);
        }
        return {};
    };
    if (const auto content_paths = addPaths(contents); !content_paths) {
        return std::unexpected(content_paths.error());
    }
    if (const auto blob_paths = addPaths(blobs); !blob_paths) {
        return std::unexpected(blob_paths.error());
    }
    for (const auto& path : payload_paths) {
        auto separator = path.indexOf(u'/');
        while (separator >= 0) {
            if (declared.contains(path.first(separator))) {
                return fail(ErrorCode::DuplicateContentPath,
                            QStringLiteral("Overlapping archive path: %1").arg(path));
            }
            separator = path.indexOf(u'/', separator + 1);
        }
    }

    QSet<QString> archived;
    for (const auto& member : inspection.members) {
        archived.insert(member.path);
    }
    if (archived != declared) {
        const auto has_undeclared = std::ranges::any_of(
            archived, [&declared](const QString& path) { return !declared.contains(path); });
        return fail(has_undeclared ? ErrorCode::UndeclaredFile : ErrorCode::CannotRead,
                    QStringLiteral("Archive member set does not exactly match its manifest"));
    }
    return manifest;
}

[[nodiscard]] bool isPdfWhitespace(char value) {
    switch (static_cast<unsigned char>(value)) {
    case 0x00:
    case 0x09:
    case 0x0a:
    case 0x0c:
    case 0x0d:
    case 0x20:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool hasPdfSignature(QByteArrayView header) {
    return header.size() >= 8 && header.first(5) == QByteArrayView("%PDF-") &&
           (header.at(5) == '1' || header.at(5) == '2') && header.at(6) == '.' &&
           header.at(7) >= '0' && header.at(7) <= '9';
}

[[nodiscard]] bool hasPdfTrailer(QByteArrayView tail) {
    auto end = tail.size();
    while (end > 0 && isPdfWhitespace(tail.at(end - 1))) {
        --end;
    }
    return end >= 5 && tail.sliced(end - 5, 5) == QByteArrayView("%%EOF");
}

[[nodiscard]] bool isLowercaseDigest(const std::string& value) {
    return value.size() == 64 && std::ranges::all_of(value, [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

[[nodiscard]] auto extractArchive(const QString& archive_path, const ZipInspection& inspection,
                                  const QString& staging_path) -> std::expected<void, Error> {
    auto reader_result = openArchiveReader(archive_path);
    if (!reader_result) {
        return std::unexpected(reader_result.error());
    }
    auto reader = std::move(*reader_result);
    QSet<QString> expected;
    QHash<QString, std::uint32_t> expected_sizes;
    for (const auto& member : inspection.members) {
        expected.insert(member.path);
        expected_sizes.insert(member.path, member.uncompressed_size);
    }

    QSet<QString> seen;
    std::array<char, 64 * 1024> buffer{};
    archive_entry* raw_entry = nullptr;
    while (true) {
        const auto header_result = archive_read_next_header(reader.get(), &raw_entry);
        if (header_result == ARCHIVE_EOF) {
            break;
        }
        if (header_result != ARCHIVE_OK || raw_entry == nullptr) {
            return fail(ErrorCode::InvalidManifest,
                        libarchiveMessage(reader.get(), QStringLiteral("Cannot read ZIP member")));
        }
        if ((archive_format(reader.get()) & ARCHIVE_FORMAT_BASE_MASK) != ARCHIVE_FORMAT_ZIP ||
            archive_filter_code(reader.get(), 0) != ARCHIVE_FILTER_NONE) {
            return fail(ErrorCode::InvalidManifest,
                        QStringLiteral("Only an unwrapped ZIP archive is supported"));
        }
        const auto* raw_path = archive_entry_pathname(raw_entry);
        const auto path = raw_path == nullptr ? QString{} : QString::fromLatin1(raw_path);
        if (!expected.contains(path) || seen.contains(path) || !isPortablePath(path)) {
            return fail(ErrorCode::UnsafePath,
                        QStringLiteral("Unexpected ZIP member path: %1").arg(path));
        }
        if (archive_entry_filetype(raw_entry) != AE_IFREG ||
            archive_entry_symlink(raw_entry) != nullptr ||
            archive_entry_hardlink(raw_entry) != nullptr ||
            archive_entry_is_encrypted(raw_entry) > 0 ||
            archive_entry_size_is_set(raw_entry) == 0) {
            return fail(ErrorCode::UnsafePath,
                        QStringLiteral("ZIP member is not a plain regular file: %1").arg(path));
        }
        const auto declared_size = archive_entry_size(raw_entry);
        if (declared_size < 0 ||
            static_cast<std::uint64_t>(declared_size) != expected_sizes.value(path)) {
            return fail(ErrorCode::InvalidManifest,
                        QStringLiteral("ZIP member size changed after inspection"));
        }
        const auto absolute_path = QDir(staging_path).filePath(path);
        if (!QDir{}.mkpath(QFileInfo(absolute_path).path())) {
            return fail(ErrorCode::CannotRead, QStringLiteral("Cannot create staging directory"));
        }
        QFile output(absolute_path);
        if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            return fail(ErrorCode::CannotRead, QStringLiteral("Cannot create staged ZIP member"));
        }
        std::uint64_t written = 0;
        while (true) {
            const auto read_size = archive_read_data(reader.get(), buffer.data(), buffer.size());
            if (read_size == 0) {
                break;
            }
            if (read_size < 0) {
                return fail(
                    ErrorCode::InvalidManifest,
                    libarchiveMessage(reader.get(), QStringLiteral("Cannot extract ZIP member")));
            }
            const auto chunk_size = static_cast<std::uint64_t>(read_size);
            if (chunk_size > static_cast<std::uint64_t>(declared_size) ||
                written > static_cast<std::uint64_t>(declared_size) - chunk_size ||
                output.write(buffer.data(), static_cast<qint64>(read_size)) != read_size) {
                return fail(ErrorCode::InvalidManifest,
                            QStringLiteral("ZIP member exceeds its declared size"));
            }
            written += chunk_size;
        }
        if (written != static_cast<std::uint64_t>(declared_size) || !output.flush()) {
            return fail(ErrorCode::InvalidManifest,
                        QStringLiteral("ZIP member size or staging write is invalid"));
        }
        seen.insert(path);
    }
    if (seen != expected || archive_read_has_encrypted_entries(reader.get()) > 0) {
        return fail(ErrorCode::InvalidManifest,
                    QStringLiteral("ZIP member set or encryption state is invalid"));
    }
    return {};
}

[[nodiscard]] auto collectExportMembers(const QString& directory, const PackArchiveLimits& limits)
    -> std::expected<std::vector<ExportMember>, Error> {
    const QFileInfo root_info(directory);
    if (!root_info.isDir() || root_info.isSymLink()) {
        return fail(ErrorCode::UnsafePath,
                    QStringLiteral("Pack export root must be a real directory"));
    }
    const QDir root(root_info.absoluteFilePath());
    std::vector<ExportMember> members;
    std::uint64_t total_size = 0;
    QDirIterator iterator(root.absolutePath(),
                          QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        const auto info = iterator.fileInfo();
        if (info.isSymLink()) {
            return fail(ErrorCode::UnsafePath, QStringLiteral("Cannot export symbolic links"));
        }
        if (info.isDir()) {
            continue;
        }
        const auto path = QDir::fromNativeSeparators(root.relativeFilePath(info.filePath()));
        if (!info.isFile() || !isPortablePath(path) || info.size() < 0) {
            return fail(ErrorCode::UnsafePath,
                        QStringLiteral("Cannot export non-portable member: %1").arg(path));
        }
        const auto size = static_cast<std::uint64_t>(info.size());
        if (members.size() >= limits.maximum_members || size > limits.maximum_file_bytes ||
            size > limits.maximum_total_bytes || total_size > limits.maximum_total_bytes - size) {
            return fail(ErrorCode::ResourceTooLarge,
                        QStringLiteral("Pack export exceeds archive limits"));
        }
        total_size += size;
        members.push_back(ExportMember{path, info.absoluteFilePath(), size});
    }
    std::ranges::sort(members, {}, &ExportMember::path);
    return members;
}

la_ssize_t writeArchiveData(archive* value, void* client_data, const void* buffer,
                            std::size_t length) {
    auto* output = static_cast<QSaveFile*>(client_data);
    if (length > static_cast<std::size_t>(std::numeric_limits<qint64>::max())) {
        archive_set_error(value, EFBIG, "Archive output block is too large");
        return -1;
    }
    const auto written =
        output->write(static_cast<const char*>(buffer), static_cast<qint64>(length));
    if (written < 0) {
        archive_set_error(value, EIO, "Cannot write archive output");
    }
    return static_cast<la_ssize_t>(written);
}

int closeArchiveOutput(archive* value, void* client_data) {
    auto* output = static_cast<QSaveFile*>(client_data);
    if (!output->flush()) {
        archive_set_error(value, EIO, "Cannot flush archive output");
        return ARCHIVE_FATAL;
    }
    return ARCHIVE_OK;
}

[[nodiscard]] auto writeStoredZip(const std::vector<ExportMember>& members,
                                  const QString& archive_path) -> std::expected<void, Error> {
    QSaveFile output(archive_path);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) {
        return fail(ErrorCode::CannotRead, QStringLiteral("Cannot create archive output"));
    }
    ArchiveWrite writer(archive_write_new());
    if (!writer) {
        return fail(ErrorCode::CannotRead, QStringLiteral("Cannot allocate ZIP writer"));
    }
    if (archive_write_set_format_zip(writer.get()) != ARCHIVE_OK ||
        archive_write_zip_set_compression_store(writer.get()) != ARCHIVE_OK ||
        archive_write_set_options(writer.get(), "zip:compression=store,zip:!zip64") != ARCHIVE_OK ||
        archive_write_set_bytes_per_block(writer.get(), 0) != ARCHIVE_OK ||
        archive_write_set_bytes_in_last_block(writer.get(), 1) != ARCHIVE_OK ||
        archive_write_open(writer.get(), &output, nullptr, writeArchiveData, closeArchiveOutput) !=
            ARCHIVE_OK) {
        return fail(ErrorCode::CannotRead,
                    libarchiveMessage(writer.get(), QStringLiteral("Cannot configure ZIP writer")));
    }

    std::array<char, 64 * 1024> buffer{};
    for (const auto& member : members) {
        ArchiveEntry entry(archive_entry_new());
        if (!entry) {
            return fail(ErrorCode::CannotRead, QStringLiteral("Cannot allocate ZIP entry"));
        }
        const auto encoded_path = member.path.toLatin1();
        archive_entry_copy_pathname(entry.get(), encoded_path.constData());
        archive_entry_set_filetype(entry.get(), AE_IFREG);
        archive_entry_set_perm(entry.get(), 0644);
        archive_entry_set_uid(entry.get(), 0);
        archive_entry_set_gid(entry.get(), 0);
        archive_entry_set_size(entry.get(), static_cast<la_int64_t>(member.size));
        archive_entry_set_mtime(entry.get(), fixed_zip_timestamp, 0);
        archive_entry_unset_atime(entry.get());
        archive_entry_unset_ctime(entry.get());
        archive_entry_unset_birthtime(entry.get());
        if (archive_write_header(writer.get(), entry.get()) != ARCHIVE_OK) {
            return fail(ErrorCode::CannotRead,
                        libarchiveMessage(writer.get(), QStringLiteral("Cannot write ZIP header")));
        }

        QFile input(member.absolute_path);
        if (!input.open(QIODevice::ReadOnly)) {
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Cannot read pack member during export"));
        }
        std::uint64_t copied = 0;
        while (true) {
            const auto read_size = input.read(buffer.data(), static_cast<qint64>(buffer.size()));
            if (read_size < 0) {
                return fail(ErrorCode::CannotRead,
                            QStringLiteral("Cannot read pack member during export"));
            }
            if (read_size == 0) {
                break;
            }
            qsizetype offset = 0;
            while (offset < read_size) {
                const auto written =
                    archive_write_data(writer.get(), buffer.data() + offset,
                                       static_cast<std::size_t>(read_size - offset));
                if (written <= 0) {
                    return fail(
                        ErrorCode::CannotRead,
                        libarchiveMessage(writer.get(), QStringLiteral("Cannot write ZIP data")));
                }
                offset += static_cast<qsizetype>(written);
            }
            copied += static_cast<std::uint64_t>(read_size);
        }
        if (copied != member.size || archive_write_finish_entry(writer.get()) != ARCHIVE_OK) {
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Pack member changed during archive export"));
        }
    }
    if (archive_write_close(writer.get()) != ARCHIVE_OK) {
        return fail(ErrorCode::CannotRead,
                    libarchiveMessage(writer.get(), QStringLiteral("Cannot finish ZIP archive")));
    }
    if (!output.commit()) {
        return fail(ErrorCode::CannotRead, QStringLiteral("Cannot commit archive output"));
    }
    return {};
}

} // namespace

std::expected<LoadedPack, Error> PackArchive::importArchive(const QString& archive_path,
                                                            PackArchiveLimits limits,
                                                            PackValidationScope scope) {
    const auto inspection = inspectZip(archive_path, limits);
    if (!inspection) {
        return std::unexpected(inspection.error());
    }
    const auto member_set = validateManifestMemberSet(archive_path, *inspection);
    if (!member_set) {
        return std::unexpected(member_set.error());
    }
    QTemporaryDir staging(QDir::tempPath() + QStringLiteral("/appellate-awpack-XXXXXX"));
    if (!staging.isValid() ||
        !QFile::setPermissions(staging.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                   QFileDevice::ExeOwner)) {
        return fail(ErrorCode::CannotRead, QStringLiteral("Cannot create private pack staging"));
    }
    const auto extracted = extractArchive(archive_path, *inspection, staging.path());
    if (!extracted) {
        return std::unexpected(extracted.error());
    }
    return PackReader::readDirectory(staging.path(), scope);
}

std::expected<model::BlobDescriptor, Error>
PackArchive::declaredBlob(const QString& archive_path, const model::PackRevision& exact_revision,
                          const std::string& blob_path, PackArchiveLimits limits) {
    if (!validLimits(limits) || !isLowercaseDigest(exact_revision.digest)) {
        return fail(ErrorCode::InvalidManifest,
                    QStringLiteral("Invalid exact-revision blob lookup"));
    }
    const auto verified = importArchive(archive_path, limits, PackValidationScope::ResolvedClosure);
    if (!verified || verified->revision != exact_revision) {
        return fail(ErrorCode::InvalidManifest,
                    QStringLiteral("Archive differs from the complete exact pack revision"));
    }
    const auto descriptor =
        std::ranges::find(verified->blobs, blob_path, &model::BlobDescriptor::path);
    if (descriptor == verified->blobs.end()) {
        return fail(ErrorCode::InvalidManifest,
                    QStringLiteral("Blob path is not declared by the exact pack revision"));
    }
    return *descriptor;
}

std::expected<void, Error> PackArchive::streamValidatedBlob(
    const QString& archive_path, const model::PackRevision& exact_revision,
    const model::BlobDescriptor& descriptor, QIODevice& destination, PackArchiveLimits limits) {
    const auto descriptor_path = QString::fromStdString(descriptor.path);
    if (!validLimits(limits) || !isPortablePath(descriptor_path) ||
        descriptor.media_type != "application/pdf" ||
        descriptor.byte_size > limits.maximum_file_bytes ||
        descriptor.byte_size > limits.maximum_total_bytes ||
        !isLowercaseDigest(descriptor.sha256) || !destination.isWritable() ||
        destination.isSequential() || destination.pos() != 0 || destination.size() != 0) {
        return fail(ErrorCode::InvalidManifest,
                    QStringLiteral("Invalid validated-blob materialization request"));
    }
    if (!isLowercaseDigest(exact_revision.digest)) {
        return fail(ErrorCode::InvalidManifest,
                    QStringLiteral("Exact pack revision has an invalid digest"));
    }

    const auto verified = importArchive(archive_path, limits, PackValidationScope::ResolvedClosure);
    if (!verified || verified->revision != exact_revision) {
        return fail(ErrorCode::InvalidManifest,
                    QStringLiteral("Archive differs from the complete exact pack revision"));
    }
    const auto declared =
        std::ranges::find(verified->blobs, descriptor.path, &model::BlobDescriptor::path);
    if (declared == verified->blobs.end() || *declared != descriptor) {
        return fail(
            ErrorCode::InvalidManifest,
            QStringLiteral("Blob descriptor differs from the verified exact pack revision"));
    }

    const auto inspection = inspectZip(archive_path, limits);
    if (!inspection) {
        return std::unexpected(inspection.error());
    }
    const auto selected = std::ranges::find(inspection->members, descriptor_path, &ZipMember::path);
    if (selected == inspection->members.end() ||
        selected->uncompressed_size != descriptor.byte_size) {
        return fail(ErrorCode::DigestMismatch,
                    QStringLiteral("Archive blob size differs from its validated descriptor"));
    }

    auto reader_result = openArchiveReader(archive_path);
    if (!reader_result) {
        return std::unexpected(reader_result.error());
    }
    auto reader = std::move(*reader_result);
    QHash<QString, std::uint32_t> expected_sizes;
    expected_sizes.reserve(static_cast<qsizetype>(inspection->members.size()));
    for (const auto& member : inspection->members) {
        expected_sizes.insert(member.path, member.uncompressed_size);
    }

    QSet<QString> seen;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray header;
    QByteArray tail;
    header.reserve(8);
    tail.reserve(pdf_tail_bytes);
    std::array<char, 64 * 1024> buffer{};
    std::uint64_t written = 0;
    bool found = false;
    archive_entry* raw_entry = nullptr;
    while (true) {
        const auto header_result = archive_read_next_header(reader.get(), &raw_entry);
        if (header_result == ARCHIVE_EOF) {
            break;
        }
        if (header_result != ARCHIVE_OK || raw_entry == nullptr) {
            return fail(ErrorCode::InvalidManifest,
                        libarchiveMessage(reader.get(), QStringLiteral("Cannot read ZIP member")));
        }
        if ((archive_format(reader.get()) & ARCHIVE_FORMAT_BASE_MASK) != ARCHIVE_FORMAT_ZIP ||
            archive_filter_code(reader.get(), 0) != ARCHIVE_FILTER_NONE) {
            return fail(ErrorCode::InvalidManifest,
                        QStringLiteral("Only an unwrapped ZIP archive is supported"));
        }
        const auto* raw_path = archive_entry_pathname(raw_entry);
        const auto path = raw_path == nullptr ? QString{} : QString::fromLatin1(raw_path);
        const auto declared_size = archive_entry_size(raw_entry);
        if (!expected_sizes.contains(path) || seen.contains(path) || !isPortablePath(path) ||
            declared_size < 0 ||
            static_cast<std::uint64_t>(declared_size) != expected_sizes.value(path) ||
            archive_entry_filetype(raw_entry) != AE_IFREG ||
            archive_entry_symlink(raw_entry) != nullptr ||
            archive_entry_hardlink(raw_entry) != nullptr ||
            archive_entry_is_encrypted(raw_entry) > 0 ||
            archive_entry_size_is_set(raw_entry) == 0) {
            return fail(ErrorCode::UnsafePath,
                        QStringLiteral("ZIP member changed after validation: %1").arg(path));
        }
        seen.insert(path);
        if (path != descriptor_path) {
            if (archive_read_data_skip(reader.get()) != ARCHIVE_OK) {
                return fail(
                    ErrorCode::InvalidManifest,
                    libarchiveMessage(reader.get(), QStringLiteral("Cannot skip ZIP member")));
            }
            continue;
        }

        found = true;
        while (true) {
            const auto read_size = archive_read_data(reader.get(), buffer.data(), buffer.size());
            if (read_size < 0) {
                return fail(ErrorCode::InvalidManifest,
                            libarchiveMessage(reader.get(),
                                              QStringLiteral("Cannot stream validated blob")));
            }
            if (read_size == 0) {
                break;
            }
            const auto chunk_size = static_cast<std::uint64_t>(read_size);
            if (chunk_size > descriptor.byte_size || written > descriptor.byte_size - chunk_size ||
                destination.write(buffer.data(), static_cast<qint64>(read_size)) != read_size) {
                return fail(ErrorCode::CannotRead,
                            QStringLiteral("Cannot write the complete validated blob"));
            }
            const QByteArrayView chunk(buffer.data(), static_cast<qsizetype>(read_size));
            if (header.size() < 8) {
                const auto needed = 8 - header.size();
                header.append(chunk.first(std::min(needed, chunk.size())));
            }
            tail.append(chunk);
            if (tail.size() > pdf_tail_bytes) {
                tail.remove(0, tail.size() - pdf_tail_bytes);
            }
            hash.addData(chunk);
            written += chunk_size;
        }
    }
    if (seen.size() != expected_sizes.size() || !found ||
        archive_read_has_encrypted_entries(reader.get()) > 0) {
        return fail(ErrorCode::InvalidManifest,
                    QStringLiteral("ZIP member set or encryption state changed"));
    }
    if (written != descriptor.byte_size ||
        QString::fromLatin1(hash.result().toHex()).toStdString() != descriptor.sha256) {
        return fail(ErrorCode::DigestMismatch,
                    QStringLiteral("Streamed blob does not match its validated descriptor"));
    }
    if (!hasPdfSignature(QByteArrayView(header)) || !hasPdfTrailer(QByteArrayView(tail))) {
        return fail(ErrorCode::InvalidManifest,
                    QStringLiteral("Streamed blob is not a structurally recognizable PDF"));
    }
    return {};
}

std::expected<model::PackRevision, Error> PackArchive::exportDirectory(const QString& directory,
                                                                       const QString& archive_path,
                                                                       PackArchiveLimits limits,
                                                                       PackValidationScope scope) {
    if (!validLimits(limits)) {
        return fail(ErrorCode::ResourceTooLarge, QStringLiteral("Invalid archive limits"));
    }
    const auto loaded = PackReader::readDirectory(directory, scope);
    if (!loaded) {
        return std::unexpected(loaded.error());
    }
    const auto source_root = QDir(QFileInfo(directory).absoluteFilePath()).absolutePath();
    const auto output_info = QFileInfo(archive_path);
    const auto output_path = output_info.absoluteFilePath();
    if (output_info.isSymLink() || output_path == source_root ||
        output_path.startsWith(source_root + u'/')) {
        return fail(ErrorCode::UnsafePath,
                    QStringLiteral("Archive output must be outside the pack source tree"));
    }
    const auto members = collectExportMembers(directory, limits);
    if (!members) {
        return std::unexpected(members.error());
    }
    const auto written = writeStoredZip(*members, archive_path);
    if (!written) {
        return std::unexpected(written.error());
    }
    const auto imported = importArchive(archive_path, limits, scope);
    if (!imported) {
        return std::unexpected(imported.error());
    }
    if (imported->revision != loaded->revision) {
        return fail(ErrorCode::DigestMismatch,
                    QStringLiteral("Exported archive does not match its validated source"));
    }
    return imported->revision;
}

} // namespace appellate::packs
