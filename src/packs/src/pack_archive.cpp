#include "appellate/packs/pack_archive.hpp"
#include "appellate/packs/schema_validator.hpp"
#include "pack_archive_p.hpp"

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
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

#if defined(Q_OS_LINUX)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>
#endif

namespace appellate::packs {

namespace detail {

#if defined(Q_OS_LINUX)
class ScratchDescriptor final {
  public:
    explicit ScratchDescriptor(int value = -1) : value_(value) {}
    ScratchDescriptor(const ScratchDescriptor&) = delete;
    ScratchDescriptor& operator=(const ScratchDescriptor&) = delete;
    ScratchDescriptor(ScratchDescriptor&& other) noexcept
        : value_(std::exchange(other.value_, -1)) {}
    ScratchDescriptor& operator=(ScratchDescriptor&& other) noexcept {
        if (this != &other) {
            if (value_ >= 0) {
                static_cast<void>(::close(value_));
            }
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }
    ~ScratchDescriptor() {
        if (value_ >= 0) {
            static_cast<void>(::close(value_));
        }
    }
    [[nodiscard]] int get() const { return value_; }

  private:
    int value_;
};

struct ScratchController final {
    QString absolute_path;
    QByteArray component;
    ScratchDescriptor descriptor;
    struct stat identity{};
};
#endif

struct SecureScratchContext::Impl final {
    QString absolute_parent;
    QByteArray encoded_parent;
#if defined(Q_OS_LINUX)
    std::vector<ScratchController> controllers;
#endif
};

SecureScratchContext::SecureScratchContext(std::unique_ptr<Impl> state) : impl_(std::move(state)) {}
SecureScratchContext::SecureScratchContext(SecureScratchContext&&) noexcept = default;
SecureScratchContext& SecureScratchContext::operator=(SecureScratchContext&&) noexcept = default;
SecureScratchContext::~SecureScratchContext() = default;
bool SecureScratchContext::isValid() const { return impl_ != nullptr; }

} // namespace detail

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

[[nodiscard]] detail::SecureScratchInjectedAction
scratchAction(const detail::SecureScratchHooks& hooks,
              const detail::SecureScratchObservation& observation) {
    return hooks.inject ? hooks.inject(observation) : detail::SecureScratchInjectedAction::Continue;
}

[[nodiscard]] bool scratchFailsBefore(detail::SecureScratchInjectedAction action) {
    return action == detail::SecureScratchInjectedAction::FailBefore;
}

[[nodiscard]] bool scratchFinishes(const detail::SecureScratchHooks& hooks,
                                   const detail::SecureScratchObservation& observation,
                                   detail::SecureScratchInjectedAction action) {
    if (hooks.observe) {
        hooks.observe(observation);
    }
    return action != detail::SecureScratchInjectedAction::FailAfter;
}

[[nodiscard]] QString scratchSystemError(const QString& action) {
    return QStringLiteral("%1: %2").arg(action, QString::fromLocal8Bit(std::strerror(errno)));
}

[[nodiscard]] bool hasValidNativeSpelling(const QString& value, qsizetype maximum_bytes) {
    if (value.contains(QChar::Null) || value.contains(QChar::ReplacementCharacter)) {
        return false;
    }
    for (qsizetype index = 0; index < value.size(); ++index) {
        const auto unit = value.at(index).unicode();
        if (unit >= 0xD800U && unit <= 0xDBFFU) {
            if (++index >= value.size()) {
                return false;
            }
            const auto low = value.at(index).unicode();
            if (low < 0xDC00U || low > 0xDFFFU) {
                return false;
            }
        } else if (unit >= 0xDC00U && unit <= 0xDFFFU) {
            return false;
        }
    }
    const auto encoded = QFile::encodeName(value);
    return !encoded.contains('\0') && encoded.size() <= maximum_bytes &&
           QFile::decodeName(encoded) == value;
}

[[nodiscard]] std::optional<QStringList> scratchAbsoluteComponents(const QString& path) {
    if (!path.startsWith(u'/') || !hasValidNativeSpelling(path, 4'095)) {
        return std::nullopt;
    }
    if (path == QStringLiteral("/")) {
        return QStringList{};
    }
    const auto components = path.sliced(1).split(u'/', Qt::KeepEmptyParts);
    if (components.isEmpty() || components.size() > 128 ||
        std::ranges::any_of(components, [](const QString& component) {
            return component.isEmpty() || component == QStringLiteral(".") ||
                   component == QStringLiteral("..") || !hasValidNativeSpelling(component, 255);
        })) {
        return std::nullopt;
    }
    return components;
}

#if defined(Q_OS_LINUX)
[[nodiscard]] bool sameScratchIdentity(const struct stat& left, const struct stat& right,
                                       bool regular_file) {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
           (left.st_mode & S_IFMT) == (right.st_mode & S_IFMT) && left.st_uid == right.st_uid &&
           (left.st_mode & 07777) == (right.st_mode & 07777) &&
           (!regular_file || (left.st_nlink == 1 && right.st_nlink == 1));
}

[[nodiscard]] bool scratchAclAbsent(int descriptor, const char* attribute) {
    while (true) {
        errno = 0;
        const auto result = ::fgetxattr(descriptor, attribute, nullptr, 0);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return result < 0 && errno == ENODATA;
    }
}

[[nodiscard]] bool scratchControllerPolicy(const struct stat& status) {
    const auto owner = status.st_uid;
    const auto mode = status.st_mode;
    return S_ISDIR(mode) && (owner == ::geteuid() || owner == 0) &&
           (((mode & 0022) == 0) || (mode & S_ISVTX) != 0);
}
#endif

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
        // ZIP's legacy timestamp is encoded through the process-local timezone. Leaving mtime
        // unset makes libarchive clamp the zero value to the ZIP epoch without also emitting a
        // timezone-sensitive DOS value or a host-dependent extended timestamp.
        archive_entry_unset_mtime(entry.get());
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

namespace detail {

bool isValidSecureScratchName(QByteArrayView name) {
    if (name.size() < static_cast<qsizetype>(secure_scratch_nonce_hex_characters + 2) ||
        name.size() > 64 || name == QByteArrayView(".") || name == QByteArrayView("..")) {
        return false;
    }
    const auto separator =
        name.size() - static_cast<qsizetype>(secure_scratch_nonce_hex_characters) - 1;
    if (separator <= 0 || name.at(separator) != '-') {
        return false;
    }
    for (qsizetype index = 0; index < name.size(); ++index) {
        const auto character = name.at(index);
        if (!((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
              character == '.' || character == '_' || character == '-')) {
            return false;
        }
        if (index > separator &&
            !((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

std::expected<SecureScratchContext, SecureScratchFailure>
acquireSecureScratchContext(const SecureScratchHooks& hooks) {
#if !defined(Q_OS_LINUX)
    static_cast<void>(hooks);
    return std::unexpected(SecureScratchFailure{
        SecureScratchFailureCode::EnvironmentInfeasible,
        QStringLiteral("Secure archive scratch requires supported Linux descriptor semantics")});
#else
    const auto path = hooks.temp_path_provider ? hooks.temp_path_provider() : QDir::tempPath();
    if (hooks.report != nullptr) {
        *hooks.report = SecureScratchReport{};
    }
    SecureScratchObservation captured{SecureScratchEvent::TempPathCaptured, path};
    const auto capture_action = scratchAction(hooks, captured);
    if (scratchFailsBefore(capture_action)) {
        return std::unexpected(SecureScratchFailure{SecureScratchFailureCode::OperationalFailure,
                                                    QStringLiteral("Scratch capture failed")});
    }
    if (hooks.report != nullptr) {
        hooks.report->captured_temp_path = path;
    }
    if (!scratchFinishes(hooks, captured, capture_action)) {
        return std::unexpected(SecureScratchFailure{SecureScratchFailureCode::OperationalFailure,
                                                    QStringLiteral("Scratch capture failed")});
    }
    const auto components = scratchAbsoluteComponents(path);
    const auto encoded = QFile::encodeName(path);
    const auto worst_control_payload =
        encoded.size() + (path == QStringLiteral("/") ? 64 : 65) + 7 * 65;
    if (!components || components->size() + 8 > 128 || worst_control_payload > 4'095) {
        return std::unexpected(SecureScratchFailure{
            SecureScratchFailureCode::EnvironmentInfeasible,
            QStringLiteral("Scratch parent cannot contain the bounded private workspace")});
    }

    auto state = std::make_unique<SecureScratchContext::Impl>();
    state->absolute_parent = path;
    state->encoded_parent = encoded;
    state->controllers.reserve(static_cast<std::size_t>(components->size()) + 1U);

    ScratchDescriptor root(::open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat root_status{};
    if (root.get() < 0 || ::fstat(root.get(), &root_status) != 0 ||
        !scratchControllerPolicy(root_status)) {
        return std::unexpected(SecureScratchFailure{
            SecureScratchFailureCode::EnvironmentInfeasible,
            scratchSystemError(QStringLiteral("Cannot retain scratch controller root"))});
    }
    SecureScratchObservation root_opened{SecureScratchEvent::ControllerOpened, QStringLiteral("/")};
    root_opened.mode_after = static_cast<unsigned int>(root_status.st_mode & 07777);
    const auto root_open_action = scratchAction(hooks, root_opened);
    if (scratchFailsBefore(root_open_action) ||
        !scratchFinishes(hooks, root_opened, root_open_action)) {
        return std::unexpected(
            SecureScratchFailure{SecureScratchFailureCode::OperationalFailure,
                                 QStringLiteral("Injected scratch root-controller failure")});
    }
    for (const auto& acl :
         {std::pair{SecureScratchEvent::AccessAclProbe, "system.posix_acl_access"},
          std::pair{SecureScratchEvent::DefaultAclProbe, "system.posix_acl_default"}}) {
        SecureScratchObservation probed{acl.first, QStringLiteral("/")};
        const auto action = scratchAction(hooks, probed);
        if (scratchFailsBefore(action) || !scratchAclAbsent(root.get(), acl.second) ||
            !scratchFinishes(hooks, probed, action)) {
            return std::unexpected(SecureScratchFailure{
                SecureScratchFailureCode::EnvironmentInfeasible,
                QStringLiteral("Scratch root-controller ACL is present or unprovable")});
        }
    }
    struct stat root_rebound{};
    SecureScratchObservation root_rebound_event{SecureScratchEvent::ControllerRebound,
                                                QStringLiteral("/")};
    const auto root_rebound_action = scratchAction(hooks, root_rebound_event);
    if (scratchFailsBefore(root_rebound_action) || ::fstat(root.get(), &root_rebound) != 0 ||
        !sameScratchIdentity(root_rebound, root_status, false) ||
        !scratchControllerPolicy(root_rebound) ||
        !scratchFinishes(hooks, root_rebound_event, root_rebound_action)) {
        return std::unexpected(
            SecureScratchFailure{SecureScratchFailureCode::EnvironmentInfeasible,
                                 QStringLiteral("Scratch root-controller binding changed")});
    }
    state->controllers.push_back(
        ScratchController{QStringLiteral("/"), {}, std::move(root), root_status});

    QString current_path;
    for (const auto& component_text : *components) {
        const auto component = QFile::encodeName(component_text);
        auto& parent = state->controllers.back();
        current_path += u'/' + component_text;
        SecureScratchObservation opened{SecureScratchEvent::ControllerOpened, current_path,
                                        component};
        const auto open_action = scratchAction(hooks, opened);
        if (scratchFailsBefore(open_action)) {
            return std::unexpected(
                SecureScratchFailure{SecureScratchFailureCode::OperationalFailure,
                                     QStringLiteral("Injected scratch controller open failure")});
        }
        ScratchDescriptor descriptor(::openat(parent.descriptor.get(), component.constData(),
                                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        struct stat status{};
        struct stat named{};
        if (descriptor.get() < 0 || ::fstat(descriptor.get(), &status) != 0 ||
            ::fstatat(parent.descriptor.get(), component.constData(), &named,
                      AT_SYMLINK_NOFOLLOW) != 0 ||
            status.st_dev != named.st_dev || status.st_ino != named.st_ino ||
            !scratchControllerPolicy(status) ||
            (((parent.identity.st_mode & S_ISVTX) != 0) && status.st_uid != ::geteuid() &&
             status.st_uid != 0)) {
            return std::unexpected(SecureScratchFailure{
                SecureScratchFailureCode::EnvironmentInfeasible,
                QStringLiteral("Scratch controller chain is unsafe or unstable")});
        }
        opened.mode_after = static_cast<unsigned int>(status.st_mode & 07777);
        if (!scratchFinishes(hooks, opened, open_action)) {
            return std::unexpected(
                SecureScratchFailure{SecureScratchFailureCode::OperationalFailure,
                                     QStringLiteral("Injected scratch controller open failure")});
        }
        for (const auto& acl :
             {std::pair{SecureScratchEvent::AccessAclProbe, "system.posix_acl_access"},
              std::pair{SecureScratchEvent::DefaultAclProbe, "system.posix_acl_default"}}) {
            SecureScratchObservation probed{acl.first, current_path, component};
            const auto acl_action = scratchAction(hooks, probed);
            if (scratchFailsBefore(acl_action) || !scratchAclAbsent(descriptor.get(), acl.second) ||
                !scratchFinishes(hooks, probed, acl_action)) {
                return std::unexpected(SecureScratchFailure{
                    SecureScratchFailureCode::EnvironmentInfeasible,
                    QStringLiteral("Scratch controller ACL is present or unprovable")});
            }
        }
        struct stat rebound{};
        struct stat held_rebound{};
        struct stat parent_rebound{};
        SecureScratchObservation rebound_event{SecureScratchEvent::ControllerRebound, current_path,
                                               component};
        const auto rebound_action = scratchAction(hooks, rebound_event);
        if (scratchFailsBefore(rebound_action) || ::fstat(descriptor.get(), &held_rebound) != 0 ||
            ::fstat(parent.descriptor.get(), &parent_rebound) != 0 ||
            ::fstatat(parent.descriptor.get(), component.constData(), &rebound,
                      AT_SYMLINK_NOFOLLOW) != 0 ||
            !sameScratchIdentity(held_rebound, status, false) ||
            !sameScratchIdentity(rebound, status, false) ||
            !sameScratchIdentity(parent_rebound, parent.identity, false) ||
            !scratchControllerPolicy(held_rebound) ||
            (((parent_rebound.st_mode & S_ISVTX) != 0) && held_rebound.st_uid != ::geteuid() &&
             held_rebound.st_uid != 0) ||
            !scratchFinishes(hooks, rebound_event, rebound_action)) {
            return std::unexpected(
                SecureScratchFailure{SecureScratchFailureCode::EnvironmentInfeasible,
                                     QStringLiteral("Scratch controller binding changed")});
        }
        state->controllers.push_back(
            ScratchController{current_path, component, std::move(descriptor), status});
    }
    return SecureScratchContext(std::move(state));
#endif
}

#if defined(Q_OS_LINUX)
struct SecureScratchDirectoryPlan final {
    QString relative_path;
    std::size_t parent_plan_index{};
    QByteArray component;
    std::vector<QByteArray> expected_children;
    std::vector<QByteArray> live_children;
};

struct SecureScratchPlan final {
    std::vector<SecureScratchDirectoryPlan> directories;
    std::size_t ledger_capacity{};
};

struct ScratchLinuxDirent64 final {
    std::uint64_t inode;
    std::int64_t offset;
    unsigned short record_length;
    unsigned char type;
    char name[1];
};

class SecureScratchWorkspace final {
  public:
    struct Entry final {
        QString relative_path;
        QString absolute_path;
        QByteArray component;
        std::size_t parent_index{};
        std::size_t plan_index{};
        ScratchDescriptor descriptor;
        struct stat identity{};
        bool directory{};
        bool provisional_mode_zero{};
        bool removed{};
    };

    SecureScratchWorkspace(SecureScratchContext context, SecureScratchHooks hooks)
        : context_(std::move(context)), hooks_(std::move(hooks)) {}
    SecureScratchWorkspace(const SecureScratchWorkspace&) = delete;
    SecureScratchWorkspace& operator=(const SecureScratchWorkspace&) = delete;
    ~SecureScratchWorkspace() {
        if (!entries_.empty() && !cleaned_) {
            static_cast<void>(cleanup());
        }
    }

    [[nodiscard]] static std::expected<std::unique_ptr<SecureScratchWorkspace>, Error>
    create(SecureScratchContext context, const ZipInspection& inspection,
           const SecureScratchHooks& hooks) {
        if (!context.isValid()) {
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Secure scratch context was consumed"));
        }
        std::unique_ptr<SecureScratchWorkspace> workspace;
        try {
            workspace.reset(new SecureScratchWorkspace(std::move(context), hooks));
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Cannot allocate secure scratch state"));
        }
        for (std::size_t attempt = 0; attempt < 128; ++attempt) {
            auto name = hooks.name_source ? hooks.name_source(attempt) : productionName();
            const auto candidate_path =
                workspace->context_.impl_->absolute_parent == QStringLiteral("/")
                    ? QStringLiteral("/") + QString::fromLatin1(name)
                    : workspace->context_.impl_->absolute_parent + u'/' + QString::fromLatin1(name);
            SecureScratchObservation candidate{SecureScratchEvent::NameCandidate, candidate_path,
                                               name, attempt};
            const auto candidate_action = scratchAction(hooks, candidate);
            if (scratchFailsBefore(candidate_action)) {
                if (hooks.report != nullptr) {
                    hooks.report->failure_code = SecureScratchFailureCode::OperationalFailure;
                }
                return fail(ErrorCode::CannotRead,
                            QStringLiteral("Injected secure scratch name-source failure"));
            }
            if (!isValidSecureScratchName(name)) {
                if (hooks.report != nullptr) {
                    hooks.report->failure_code = SecureScratchFailureCode::InvalidConfiguration;
                }
                return fail(ErrorCode::CannotRead,
                            QStringLiteral("Secure scratch name source is invalid"));
            }
            if (!scratchFinishes(hooks, candidate, candidate_action)) {
                if (hooks.report != nullptr) {
                    hooks.report->failure_code = SecureScratchFailureCode::OperationalFailure;
                }
                return fail(ErrorCode::CannotRead,
                            QStringLiteral("Injected secure scratch name-source failure"));
            }
            if (!workspace->membersFeasible(candidate_path, inspection)) {
                return fail(ErrorCode::UnsafePath,
                            QStringLiteral("Archive member cannot fit the secure scratch path"));
            }
            if (workspace->plan_.directories.empty()) {
                auto plan = buildPlan(inspection);
                if (!plan) {
                    return std::unexpected(plan.error());
                }
                try {
                    workspace->plan_ = std::move(*plan);
                    workspace->entries_.reserve(workspace->plan_.ledger_capacity);
                    workspace->directories_.reserve(
                        static_cast<qsizetype>(workspace->plan_.directories.size()));
                    workspace->plan_indices_.reserve(
                        static_cast<qsizetype>(workspace->plan_.directories.size()));
                    for (std::size_t index = 0; index < workspace->plan_.directories.size();
                         ++index) {
                        const auto& directory = workspace->plan_.directories.at(index);
                        workspace->directories_.insert(directory.relative_path, noParentIndex());
                        workspace->plan_indices_.insert(directory.relative_path, index);
                    }
                } catch (const std::bad_alloc&) {
                    return fail(ErrorCode::CannotRead,
                                QStringLiteral("Cannot allocate the secure scratch ledger"));
                } catch (const std::length_error&) {
                    return fail(ErrorCode::CannotRead,
                                QStringLiteral("Secure scratch ledger capacity is infeasible"));
                }
            }
            const auto parent = workspace->scratchParentDescriptor();
            SecureScratchObservation created{SecureScratchEvent::DirectoryCreate, candidate_path,
                                             name, attempt};
            const auto create_action = scratchAction(hooks, created);
            if (scratchFailsBefore(create_action)) {
                return fail(ErrorCode::CannotRead,
                            QStringLiteral("Injected secure scratch creation failure"));
            }
            if (::mkdirat(parent, name.constData(), 0000) != 0) {
                if (errno == EEXIST) {
                    SecureScratchObservation collision{SecureScratchEvent::NameCollision,
                                                       candidate_path, name, attempt};
                    const auto collision_action = scratchAction(hooks, collision);
                    if (scratchFailsBefore(collision_action) ||
                        !scratchFinishes(hooks, collision, collision_action)) {
                        return fail(ErrorCode::CannotRead,
                                    QStringLiteral("Injected secure scratch collision failure"));
                    }
                    continue;
                }
                return fail(ErrorCode::CannotRead,
                            scratchSystemError(QStringLiteral("Cannot create secure scratch")));
            }
            const auto retained = workspace->retainCreatedDirectory(
                name, QString{}, candidate_path, noParentIndex(), 0, create_action, created);
            if (!retained) {
                return std::unexpected(retained.error());
            }
            workspace->root_name_ = name;
            workspace->root_path_ = candidate_path;
            const auto root_directory = workspace->directories_.find(QString{});
            if (root_directory == workspace->directories_.end()) {
                return fail(ErrorCode::CannotRead,
                            QStringLiteral("Secure scratch root plan is incomplete"));
            }
            root_directory.value() = 0U;
            if (hooks.report != nullptr) {
                hooks.report->workspace_path = candidate_path;
            }
            return workspace;
        }
        if (hooks.report != nullptr) {
            hooks.report->failure_code = SecureScratchFailureCode::OperationalFailure;
        }
        return fail(ErrorCode::CannotRead,
                    QStringLiteral("Cannot create secure scratch after 128 name collisions"));
    }

    [[nodiscard]] const QString& absolutePath() const { return root_path_; }

    [[nodiscard]] std::expected<void, Error> readerBoundary(SecureScratchEvent event) {
        SecureScratchObservation observation{event, root_path_, root_name_};
        const auto action = scratchAction(hooks_, observation);
        if (scratchFailsBefore(action) || !scratchFinishes(hooks_, observation, action)) {
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Injected secure scratch reader-boundary failure"));
        }
        return {};
    }

    [[nodiscard]] std::expected<std::size_t, Error> createFile(const QString& relative_path) {
        auto components = relative_path.split(u'/');
        const auto filename_text = components.takeLast();
        QString directory_path;
        std::size_t parent_index = 0;
        for (const auto& component_text : components) {
            directory_path =
                directory_path.isEmpty() ? component_text : directory_path + u'/' + component_text;
            const auto found = directories_.constFind(directory_path);
            if (found == directories_.constEnd()) {
                return fail(ErrorCode::CannotRead,
                            QStringLiteral("Secure scratch directory plan is incomplete"));
            }
            if (*found != noParentIndex()) {
                parent_index = *found;
                continue;
            }
            const auto plan_index = planIndex(directory_path);
            if (!plan_index) {
                return fail(ErrorCode::CannotRead,
                            QStringLiteral("Secure scratch directory plan is incomplete"));
            }
            const auto component = component_text.toLatin1();
            const auto absolute = root_path_ + u'/' + directory_path;
            SecureScratchObservation created{SecureScratchEvent::DirectoryCreate, absolute,
                                             component};
            const auto action = scratchAction(hooks_, created);
            if (scratchFailsBefore(action) || ::mkdirat(entries_.at(parent_index).descriptor.get(),
                                                        component.constData(), 0000) != 0) {
                return fail(ErrorCode::CannotRead,
                            QStringLiteral("Cannot create secure extraction directory"));
            }
            const auto retained = retainCreatedDirectory(
                component, directory_path, absolute, parent_index, *plan_index, action, created);
            if (!retained) {
                return std::unexpected(retained.error());
            }
            parent_index = *retained;
            const auto planned_directory = directories_.find(directory_path);
            if (planned_directory == directories_.end()) {
                return fail(ErrorCode::CannotRead,
                            QStringLiteral("Secure scratch directory plan is incomplete"));
            }
            planned_directory.value() = parent_index;
        }

        const auto component = filename_text.toLatin1();
        const auto absolute = root_path_ + u'/' + relative_path;
        SecureScratchObservation created{SecureScratchEvent::FileCreate, absolute, component};
        const auto create_action = scratchAction(hooks_, created);
        if (scratchFailsBefore(create_action)) {
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Injected secure extraction file creation failure"));
        }
        ScratchDescriptor descriptor(
            ::openat(entries_.at(parent_index).descriptor.get(), component.constData(),
                     O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (descriptor.get() < 0) {
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Cannot create secure extraction file"));
        }
        entries_.push_back(Entry{relative_path,
                                 absolute,
                                 component,
                                 parent_index,
                                 noParentIndex(),
                                 std::move(descriptor),
                                 {},
                                 false,
                                 false,
                                 false});
        const auto index = entries_.size() - 1U;
        if (!addLiveChild(parent_index, component)) {
            markCleanupBlocked();
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Secure scratch live inventory is inconsistent"));
        }
        SecureScratchObservation retain_rebind{SecureScratchEvent::FileRetainRebind, absolute,
                                               component};
        const auto retain_rebind_action = scratchAction(hooks_, retain_rebind);
        if (scratchFailsBefore(retain_rebind_action)) {
            markCleanupBlocked();
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Injected secure file retain-rebind failure"));
        }
        struct stat status{};
        struct stat named{};
        if (::fstat(entries_.at(index).descriptor.get(), &status) != 0 ||
            ::fstatat(entries_.at(parent_index).descriptor.get(), component.constData(), &named,
                      AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(status.st_mode) || status.st_uid != ::geteuid() || status.st_nlink != 1 ||
            !sameScratchIdentity(status, named, true)) {
            markCleanupBlocked();
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Cannot retain secure extraction file"));
        }
        entries_.at(index).identity = status;
        if (!scratchFinishes(hooks_, retain_rebind, retain_rebind_action)) {
            markCleanupBlocked();
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Injected secure file retain-rebind failure"));
        }
        if (!scratchFinishes(hooks_, created, create_action)) {
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Injected secure extraction file creation failure"));
        }
        if (!fileTransition(index)) {
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Cannot normalize secure extraction file"));
        }
        return index;
    }

    [[nodiscard]] std::expected<void, Error> beginWrite(std::size_t index) {
        auto& entry = entries_.at(index);
        SecureScratchObservation written{SecureScratchEvent::FileWrite, entry.absolute_path,
                                         entry.component};
        pending_write_action_ = scratchAction(hooks_, written);
        pending_write_observation_ = written;
        if (scratchFailsBefore(pending_write_action_)) {
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Injected secure extraction write failure"));
        }
        return {};
    }

    [[nodiscard]] std::expected<void, Error> write(std::size_t index, QByteArrayView bytes) {
        auto& entry = entries_.at(index);
        qsizetype offset = 0;
        while (offset < bytes.size()) {
            const auto count = ::write(entry.descriptor.get(), bytes.data() + offset,
                                       static_cast<std::size_t>(bytes.size() - offset));
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                return fail(ErrorCode::CannotRead,
                            QStringLiteral("Cannot write secure extraction file"));
            }
            offset += static_cast<qsizetype>(count);
        }
        return {};
    }

    [[nodiscard]] std::expected<void, Error> finishWrite(std::size_t index) {
        auto& entry = entries_.at(index);
        if (!scratchFinishes(hooks_, pending_write_observation_, pending_write_action_)) {
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Injected secure extraction write failure"));
        }
        SecureScratchObservation synced{SecureScratchEvent::FileSync, entry.absolute_path,
                                        entry.component};
        const auto action = scratchAction(hooks_, synced);
        if (scratchFailsBefore(action) || ::fsync(entry.descriptor.get()) != 0 ||
            !scratchFinishes(hooks_, synced, action)) {
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Cannot synchronize secure extraction file"));
        }
        return syncDirectory(entry.parent_index);
    }

    [[nodiscard]] std::expected<void, Error> validateInventory() {
        if (std::ranges::any_of(plan_.directories, [](const auto& directory) {
                return directory.live_children != directory.expected_children;
            })) {
            markCleanupBlocked();
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Secure scratch ledger is incomplete"));
        }
        for (const auto& entry : entries_) {
            SecureScratchObservation rebound{SecureScratchEvent::InventoryRebind,
                                             entry.absolute_path, entry.component};
            const auto action = scratchAction(hooks_, rebound);
            if (scratchFailsBefore(action)) {
                markCleanupBlocked();
                return fail(ErrorCode::CannotRead,
                            QStringLiteral("Injected secure scratch inventory failure"));
            }
            if (entry.removed || !validateEntry(entry) ||
                !scratchAclAbsent(entry.descriptor.get(), "system.posix_acl_access") ||
                (entry.directory &&
                 (!scratchAclAbsent(entry.descriptor.get(), "system.posix_acl_default") ||
                  !directoryInventoryMatches(
                      entry, plan_.directories.at(entry.plan_index).live_children))) ||
                !validateEntry(entry)) {
                markCleanupBlocked();
                return fail(ErrorCode::CannotRead,
                            QStringLiteral("Secure scratch inventory binding changed"));
            }
            if (!scratchFinishes(hooks_, rebound, action)) {
                markCleanupBlocked();
                return fail(ErrorCode::CannotRead,
                            QStringLiteral("Injected secure scratch inventory failure"));
            }
        }
        if (std::ranges::any_of(plan_.directories,
                                [](const auto& directory) {
                                    return directory.live_children != directory.expected_children;
                                }) ||
            !exactLedgerInventory(false) || !validateControllers() || entries_.empty() ||
            !validateEntry(entries_.front())) {
            markCleanupBlocked();
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Secure scratch final binding changed"));
        }
        return {};
    }

    [[nodiscard]] std::expected<void, Error> cleanup() {
        if (cleaned_) {
            return {};
        }
        if (!validateControllers()) {
            return preserveCleanup(
                QStringLiteral("Secure scratch controller changed during cleanup"));
        }
        if (!exactLedgerInventory()) {
            return preserveCleanup(
                QStringLiteral("Secure scratch inventory changed during cleanup (%1)")
                    .arg(inventory_diagnostic_));
        }
        for (std::size_t offset = entries_.size(); offset > 0; --offset) {
            auto& entry = entries_.at(offset - 1U);
            if (entry.removed) {
                continue;
            }
            SecureScratchObservation inspected{SecureScratchEvent::CleanupInspect,
                                               entry.absolute_path, entry.component};
            const auto inspect_action = scratchAction(hooks_, inspected);
            if (scratchFailsBefore(inspect_action) || !cleanupEntryProof(entry, true) ||
                !scratchFinishes(hooks_, inspected, inspect_action)) {
                return preserveCleanup(
                    QStringLiteral("Secure scratch cleanup identity is ambiguous"));
            }
            SecureScratchObservation removed{SecureScratchEvent::CleanupRemove, entry.absolute_path,
                                             entry.component};
            const auto remove_action = scratchAction(hooks_, removed);
            if (scratchFailsBefore(remove_action) || !validateControllers() ||
                !cleanupEntryProof(entry, false)) {
                return preserveCleanup(QStringLiteral("Injected secure scratch cleanup failure"));
            }
            const auto parent = parentDescriptor(entry);
            if (::unlinkat(parent, entry.component.constData(),
                           entry.directory ? AT_REMOVEDIR : 0) != 0) {
                return preserveCleanup(QStringLiteral("Cannot remove secure scratch entry"));
            }
            entry.removed = true;
            removeLiveChild(entry);
            if (!scratchFinishes(hooks_, removed, remove_action)) {
                if (!entryNameAbsent(entry)) {
                    entry.removed = false;
                    static_cast<void>(addLiveChild(entry.parent_index, entry.component));
                }
                return preserveCleanup(QStringLiteral("Injected secure scratch cleanup failure"));
            }
            if (!entryNameAbsent(entry)) {
                entry.removed = false;
                static_cast<void>(addLiveChild(entry.parent_index, entry.component));
                return preserveCleanup(
                    QStringLiteral("Secure scratch name was replaced during cleanup"));
            }
            SecureScratchObservation synced{SecureScratchEvent::CleanupSync, entry.absolute_path,
                                            entry.component};
            const auto sync_action = scratchAction(hooks_, synced);
            if (scratchFailsBefore(sync_action) || ::fsync(parent) != 0) {
                return preserveCleanup(QStringLiteral("Cannot synchronize secure scratch cleanup"));
            }
            if (!scratchFinishes(hooks_, synced, sync_action)) {
                if (!entryNameAbsent(entry)) {
                    entry.removed = false;
                    static_cast<void>(addLiveChild(entry.parent_index, entry.component));
                }
                return preserveCleanup(QStringLiteral("Cannot synchronize secure scratch cleanup"));
            }
            if (!entryNameAbsent(entry)) {
                entry.removed = false;
                static_cast<void>(addLiveChild(entry.parent_index, entry.component));
                return preserveCleanup(
                    QStringLiteral("Secure scratch name was replaced after cleanup sync"));
            }
        }
        for (auto& entry : entries_) {
            if (!entryNameAbsent(entry)) {
                entry.removed = false;
                static_cast<void>(addLiveChild(entry.parent_index, entry.component));
                return preserveCleanup(
                    QStringLiteral("Secure scratch name was replaced at cleanup boundary"));
            }
        }
        cleaned_ = true;
        if (hooks_.report != nullptr) {
            hooks_.report->remaining_ledger_paths.clear();
            if (!cleanup_failure_reported_) {
                hooks_.report->cleanup = SecureScratchCleanupOutcome::Removed;
                hooks_.report->residue_identity_ambiguous = false;
            }
        }
        return {};
    }

  private:
    static constexpr std::size_t noParentIndex() { return std::numeric_limits<std::size_t>::max(); }

    [[nodiscard]] static std::expected<SecureScratchPlan, Error>
    buildPlan(const ZipInspection& inspection) {
        try {
            SecureScratchPlan plan;
            plan.directories.push_back(SecureScratchDirectoryPlan{QString{}, 0, {}, {}, {}});
            QHash<QString, std::size_t> indices;
            indices.insert(QString{}, 0);
            for (const auto& member : inspection.members) {
                auto components = member.path.split(u'/');
                const auto filename = components.takeLast().toLatin1();
                QString relative;
                std::size_t parent_plan_index = 0;
                for (const auto& component_text : components) {
                    relative =
                        relative.isEmpty() ? component_text : relative + u'/' + component_text;
                    const auto found = indices.constFind(relative);
                    if (found != indices.constEnd()) {
                        parent_plan_index = *found;
                        continue;
                    }
                    if (plan.directories.size() == std::numeric_limits<std::size_t>::max()) {
                        return fail(ErrorCode::CannotRead,
                                    QStringLiteral("Secure scratch directory plan is too large"));
                    }
                    const auto component = component_text.toLatin1();
                    const auto plan_index = plan.directories.size();
                    plan.directories.push_back(
                        SecureScratchDirectoryPlan{relative, parent_plan_index, component, {}, {}});
                    plan.directories.at(parent_plan_index).expected_children.push_back(component);
                    indices.insert(relative, plan_index);
                    parent_plan_index = plan_index;
                }
                plan.directories.at(parent_plan_index).expected_children.push_back(filename);
            }
            for (auto& directory : plan.directories) {
                std::ranges::sort(directory.expected_children);
                if (std::ranges::adjacent_find(directory.expected_children) !=
                    directory.expected_children.end()) {
                    return fail(
                        ErrorCode::UnsafePath,
                        QStringLiteral("Archive member paths conflict as files and directories"));
                }
                directory.live_children.reserve(directory.expected_children.size());
            }
            if (inspection.members.size() >
                std::numeric_limits<std::size_t>::max() - plan.directories.size()) {
                return fail(ErrorCode::CannotRead,
                            QStringLiteral("Secure scratch ledger capacity overflows"));
            }
            plan.ledger_capacity = plan.directories.size() + inspection.members.size();
            if (plan.directories.size() >
                    static_cast<std::size_t>(std::numeric_limits<qsizetype>::max()) ||
                plan.ledger_capacity > std::vector<Entry>().max_size()) {
                return fail(ErrorCode::CannotRead,
                            QStringLiteral("Secure scratch plan capacity is infeasible"));
            }
            return plan;
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Cannot allocate the secure scratch plan"));
        } catch (const std::length_error&) {
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Secure scratch plan capacity is infeasible"));
        }
    }

    [[nodiscard]] std::optional<std::size_t> planIndex(const QString& relative_path) const {
        const auto found = plan_indices_.constFind(relative_path);
        if (found == plan_indices_.constEnd()) {
            return std::nullopt;
        }
        return *found;
    }

    [[nodiscard]] bool addLiveChild(std::size_t parent_entry_index, const QByteArray& component) {
        if (parent_entry_index == noParentIndex()) {
            return true;
        }
        auto& directory = plan_.directories.at(entries_.at(parent_entry_index).plan_index);
        auto& children = directory.live_children;
        const auto expected = std::ranges::lower_bound(directory.expected_children, component);
        const auto insertion = std::ranges::lower_bound(children, component);
        if (expected == directory.expected_children.end() || *expected != component ||
            (insertion != children.end() && *insertion == component) ||
            children.size() == children.capacity()) {
            inventory_diagnostic_ = QStringLiteral("live inventory update failed");
            return false;
        }
        children.insert(insertion, component);
        return true;
    }

    void removeLiveChild(const Entry& entry) {
        if (entry.parent_index == noParentIndex()) {
            return;
        }
        auto& children =
            plan_.directories.at(entries_.at(entry.parent_index).plan_index).live_children;
        const auto found = std::ranges::lower_bound(children, entry.component);
        if (found != children.end() && *found == entry.component) {
            children.erase(found);
        }
    }

    [[nodiscard]] static QByteArray productionName() {
        const auto first = QRandomGenerator::system()->generate64();
        const auto second = QRandomGenerator::system()->generate64();
        return QStringLiteral("appellate-awpack-%1%2")
            .arg(first, 16, 16, QLatin1Char('0'))
            .arg(second, 16, 16, QLatin1Char('0'))
            .toLatin1();
    }

    [[nodiscard]] int scratchParentDescriptor() const {
        return context_.impl_->controllers.back().descriptor.get();
    }

    [[nodiscard]] bool membersFeasible(const QString& candidate,
                                       const ZipInspection& inspection) const {
        const auto root_components = scratchAbsoluteComponents(candidate);
        if (!root_components) {
            return false;
        }
        for (const auto& member : inspection.members) {
            const auto absolute = candidate + u'/' + member.path;
            SecureScratchObservation checked{SecureScratchEvent::MemberFeasibility, absolute,
                                             member.path.toLatin1()};
            const auto action = scratchAction(hooks_, checked);
            if (scratchFailsBefore(action) || !hasValidNativeSpelling(absolute, 4'095) ||
                root_components->size() + member.path.count(u'/') + 1 > 128 ||
                !scratchFinishes(hooks_, checked, action)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::expected<std::size_t, Error>
    retainCreatedDirectory(const QByteArray& component, const QString& relative,
                           const QString& absolute, std::size_t parent_index,
                           std::size_t plan_index, SecureScratchInjectedAction create_action,
                           const SecureScratchObservation& create_observation) {
        const auto parent = parent_index == noParentIndex()
                                ? scratchParentDescriptor()
                                : entries_.at(parent_index).descriptor.get();
        entries_.push_back(Entry{relative,
                                 absolute,
                                 component,
                                 parent_index,
                                 plan_index,
                                 ScratchDescriptor{},
                                 {},
                                 true,
                                 true,
                                 false});
        const auto index = entries_.size() - 1U;
        if (!addLiveChild(parent_index, component)) {
            markCleanupBlocked();
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Secure scratch live inventory is inconsistent"));
        }
        struct stat provisional{};
        if (::fstatat(parent, component.constData(), &provisional, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISDIR(provisional.st_mode) || provisional.st_uid != ::geteuid() ||
            (provisional.st_mode & 07777) != 0000) {
            markCleanupBlocked();
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Cannot bind newly created secure directory name"));
        }
        entries_.at(index).identity = provisional;
        SecureScratchObservation retain_open{SecureScratchEvent::DirectoryRetainOpen, absolute,
                                             component};
        const auto retain_open_action = scratchAction(hooks_, retain_open);
        if (scratchFailsBefore(retain_open_action)) {
            markCleanupBlocked();
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Injected secure directory retain-open failure"));
        }
        ScratchDescriptor path_descriptor(
            ::openat(parent, component.constData(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        struct stat actual{};
        if (path_descriptor.get() < 0 || ::fstat(path_descriptor.get(), &actual) != 0 ||
            !sameScratchIdentity(actual, provisional, false)) {
            markCleanupBlocked();
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Cannot retain newly created secure directory"));
        }
        entries_.at(index).descriptor = std::move(path_descriptor);
        entries_.at(index).identity = actual;
        if (!scratchFinishes(hooks_, retain_open, retain_open_action)) {
            markCleanupBlocked();
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Injected secure directory retain-open failure"));
        }
        if (!scratchFinishes(hooks_, create_observation, create_action)) {
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Injected secure directory creation failure"));
        }
        SecureScratchObservation retained{SecureScratchEvent::DirectoryRetained, absolute,
                                          component, 0,
                                          static_cast<unsigned int>(actual.st_mode & 07777)};
        const auto retained_action = scratchAction(hooks_, retained);
        if (scratchFailsBefore(retained_action) ||
            !scratchFinishes(hooks_, retained, retained_action)) {
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Injected secure directory retention failure"));
        }
        SecureScratchObservation normalized{SecureScratchEvent::DirectoryNormalize,
                                            absolute,
                                            component,
                                            0,
                                            static_cast<unsigned int>(actual.st_mode & 07777),
                                            0700};
        const auto normalize_action = scratchAction(hooks_, normalized);
        if (scratchFailsBefore(normalize_action) ||
            ::syscall(SYS_fchmodat2, entries_.at(index).descriptor.get(), "", 0700,
                      AT_EMPTY_PATH) != 0) {
            markCleanupBlocked();
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Cannot normalize secure directory mode"));
        }
        struct stat normalized_status{};
        struct stat named{};
        if (::fstat(entries_.at(index).descriptor.get(), &normalized_status) != 0 ||
            ::fstatat(parent, component.constData(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
            normalized_status.st_dev != actual.st_dev ||
            normalized_status.st_ino != actual.st_ino || named.st_dev != actual.st_dev ||
            named.st_ino != actual.st_ino || (normalized_status.st_mode & 07777) != 0700 ||
            normalized_status.st_uid != ::geteuid()) {
            markCleanupBlocked();
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Secure directory mode transition lost identity"));
        }
        entries_.at(index).identity = normalized_status;
        entries_.at(index).provisional_mode_zero = false;
        ScratchDescriptor real_descriptor(::openat(
            parent, component.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        struct stat real_status{};
        if (real_descriptor.get() < 0 || ::fstat(real_descriptor.get(), &real_status) != 0 ||
            !sameScratchIdentity(real_status, normalized_status, false)) {
            markCleanupBlocked();
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Cannot retain normalized secure directory"));
        }
        entries_.at(index).descriptor = std::move(real_descriptor);
        entries_.at(index).identity = real_status;
        if (!scratchFinishes(hooks_, normalized, normalize_action)) {
            markCleanupBlocked();
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Injected secure directory normalization failure"));
        }
        if (!probeEntryAcls(entries_.at(index), true)) {
            markCleanupBlocked();
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Secure directory ACL is present or unprovable"));
        }
        SecureScratchObservation rebound{SecureScratchEvent::DirectoryRebound, absolute, component};
        const auto rebound_action = scratchAction(hooks_, rebound);
        if (scratchFailsBefore(rebound_action) || !validateEntry(entries_.at(index)) ||
            !scratchFinishes(hooks_, rebound, rebound_action)) {
            markCleanupBlocked();
            return fail(ErrorCode::CannotRead, QStringLiteral("Secure directory binding changed"));
        }
        if (const auto synced = syncDirectory(index); !synced) {
            return std::unexpected(synced.error());
        }
        if (const auto synced =
                syncDirectory(parent_index == noParentIndex() ? noParentIndex() : parent_index);
            !synced) {
            return std::unexpected(synced.error());
        }
        return index;
    }

    [[nodiscard]] bool fileTransition(std::size_t index) {
        auto& entry = entries_.at(index);
        SecureScratchObservation retained{
            SecureScratchEvent::FileRetained, entry.absolute_path, entry.component, 0,
            static_cast<unsigned int>(entry.identity.st_mode & 07777)};
        const auto retained_action = scratchAction(hooks_, retained);
        if (scratchFailsBefore(retained_action) ||
            !scratchFinishes(hooks_, retained, retained_action)) {
            return false;
        }
        SecureScratchObservation normalized{
            SecureScratchEvent::FileNormalize,
            entry.absolute_path,
            entry.component,
            0,
            static_cast<unsigned int>(entry.identity.st_mode & 07777),
            0600};
        const auto normalize_action = scratchAction(hooks_, normalized);
        struct stat after{};
        struct stat named{};
        if (scratchFailsBefore(normalize_action) || ::fchmod(entry.descriptor.get(), 0600) != 0 ||
            ::fstat(entry.descriptor.get(), &after) != 0 ||
            ::fstatat(entries_.at(entry.parent_index).descriptor.get(), entry.component.constData(),
                      &named, AT_SYMLINK_NOFOLLOW) != 0 ||
            after.st_dev != entry.identity.st_dev || after.st_ino != entry.identity.st_ino ||
            named.st_dev != after.st_dev || named.st_ino != after.st_ino ||
            (after.st_mode & 07777) != 0600 || after.st_uid != ::geteuid() || after.st_nlink != 1) {
            markCleanupBlocked();
            return false;
        }
        entry.identity = after;
        if (!scratchFinishes(hooks_, normalized, normalize_action)) {
            markCleanupBlocked();
            return false;
        }
        if (!probeEntryAcls(entry, false)) {
            markCleanupBlocked();
            return false;
        }
        SecureScratchObservation rebound{SecureScratchEvent::FileRebound, entry.absolute_path,
                                         entry.component};
        const auto rebound_action = scratchAction(hooks_, rebound);
        if (scratchFailsBefore(rebound_action) || !validateEntry(entry) ||
            !scratchFinishes(hooks_, rebound, rebound_action)) {
            markCleanupBlocked();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool probeEntryAcls(const Entry& entry, bool directory) const {
        for (const auto& acl :
             {std::pair{SecureScratchEvent::AccessAclProbe, "system.posix_acl_access"},
              std::pair{SecureScratchEvent::DefaultAclProbe, "system.posix_acl_default"}}) {
            if (!directory && acl.first == SecureScratchEvent::DefaultAclProbe) {
                continue;
            }
            SecureScratchObservation probed{acl.first, entry.absolute_path, entry.component};
            const auto action = scratchAction(hooks_, probed);
            if (scratchFailsBefore(action) ||
                !scratchAclAbsent(entry.descriptor.get(), acl.second) ||
                !scratchFinishes(hooks_, probed, action)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::expected<void, Error> syncDirectory(std::size_t index) {
        const auto descriptor = index == noParentIndex() ? scratchParentDescriptor()
                                                         : entries_.at(index).descriptor.get();
        const auto absolute = index == noParentIndex() ? context_.impl_->absolute_parent
                                                       : entries_.at(index).absolute_path;
        SecureScratchObservation synced{SecureScratchEvent::DirectorySync, absolute};
        const auto action = scratchAction(hooks_, synced);
        if (scratchFailsBefore(action) || ::fsync(descriptor) != 0 ||
            !scratchFinishes(hooks_, synced, action)) {
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Cannot synchronize secure scratch directory"));
        }
        return {};
    }

    [[nodiscard]] int parentDescriptor(const Entry& entry) const {
        return entry.parent_index == noParentIndex()
                   ? scratchParentDescriptor()
                   : entries_.at(entry.parent_index).descriptor.get();
    }

    [[nodiscard]] bool validateEntry(const Entry& entry) const {
        struct stat held{};
        struct stat named{};
        return ::fstat(entry.descriptor.get(), &held) == 0 &&
               ::fstatat(parentDescriptor(entry), entry.component.constData(), &named,
                         AT_SYMLINK_NOFOLLOW) == 0 &&
               sameScratchIdentity(held, entry.identity, !entry.directory) &&
               sameScratchIdentity(named, entry.identity, !entry.directory);
    }

    [[nodiscard]] bool validateProvisionalDirectory(const Entry& entry) const {
        if (!entry.directory || !entry.provisional_mode_zero || entry.identity.st_ino == 0 ||
            !S_ISDIR(entry.identity.st_mode) || entry.identity.st_uid != ::geteuid() ||
            (entry.identity.st_mode & 07777) != 0000) {
            return false;
        }
        struct stat named{};
        if (::fstatat(parentDescriptor(entry), entry.component.constData(), &named,
                      AT_SYMLINK_NOFOLLOW) != 0 ||
            !sameScratchIdentity(named, entry.identity, false)) {
            return false;
        }
        if (entry.descriptor.get() < 0) {
            return true;
        }
        struct stat held{};
        return ::fstat(entry.descriptor.get(), &held) == 0 &&
               sameScratchIdentity(held, entry.identity, false);
    }

    [[nodiscard]] bool establishProvisionalFileIdentity(Entry& entry) const {
        if (entry.directory || entry.identity.st_ino != 0 || entry.descriptor.get() < 0) {
            return !entry.directory && entry.identity.st_ino != 0;
        }
        struct stat held{};
        struct stat named{};
        if (::fstat(entry.descriptor.get(), &held) != 0 ||
            ::fstatat(parentDescriptor(entry), entry.component.constData(), &named,
                      AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(held.st_mode) || held.st_uid != ::geteuid() || held.st_nlink != 1 ||
            (held.st_mode & 07177) != 0 || !sameScratchIdentity(held, named, true)) {
            return false;
        }
        entry.identity = held;
        return true;
    }

    [[nodiscard]] bool cleanupEntryProof(Entry& entry, bool invoke_inventory_hooks) {
        if (entry.provisional_mode_zero) {
            return plan_.directories.at(entry.plan_index).live_children.empty() &&
                   validateProvisionalDirectory(entry);
        }
        if (!entry.directory && entry.identity.st_ino == 0 &&
            !establishProvisionalFileIdentity(entry)) {
            return false;
        }
        static const std::vector<QByteArray> empty_inventory;
        return validateEntry(entry) &&
               scratchAclAbsent(entry.descriptor.get(), "system.posix_acl_access") &&
               (!entry.directory ||
                (scratchAclAbsent(entry.descriptor.get(), "system.posix_acl_default") &&
                 directoryInventoryMatches(entry, empty_inventory, invoke_inventory_hooks))) &&
               validateEntry(entry);
    }

    void recordUnexpectedRawPath(const Entry& directory, const QByteArray& name) {
        if (hooks_.report == nullptr || !hooks_.report->unexpected_raw_paths.isEmpty()) {
            return;
        }
        auto path = directory.relative_path.toLatin1();
        if (!path.isEmpty()) {
            path += '/';
        }
        path += name;
        hooks_.report->unexpected_raw_paths.push_back(std::move(path));
    }

    [[nodiscard]] bool directoryInventoryMatches(const Entry& directory,
                                                 const std::vector<QByteArray>& expected_children,
                                                 bool invoke_hooks = true) {
        SecureScratchObservation opened_event{SecureScratchEvent::DirectoryEnumerateOpen,
                                              directory.absolute_path, directory.component};
        const auto opened_action = invoke_hooks ? scratchAction(hooks_, opened_event)
                                                : SecureScratchInjectedAction::Continue;
        if (scratchFailsBefore(opened_action)) {
            return false;
        }
        ScratchDescriptor enumeration(::openat(directory.descriptor.get(), ".",
                                               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        struct stat opened{};
        if (enumeration.get() < 0 || ::fstat(enumeration.get(), &opened) != 0 ||
            !sameScratchIdentity(opened, directory.identity, false) ||
            (invoke_hooks && !scratchFinishes(hooks_, opened_event, opened_action))) {
            return false;
        }
        try {
            std::vector<unsigned char> seen(expected_children.size(), 0);
            std::array<char, 64 * 1024> buffer{};
            SecureScratchObservation read_event{SecureScratchEvent::DirectoryEnumerateRead,
                                                directory.absolute_path, directory.component};
            const auto read_action = invoke_hooks ? scratchAction(hooks_, read_event)
                                                  : SecureScratchInjectedAction::Continue;
            if (scratchFailsBefore(read_action)) {
                return false;
            }
            while (true) {
                const auto count =
                    ::syscall(SYS_getdents64, enumeration.get(), buffer.data(), buffer.size());
                if (count < 0 && errno == EINTR) {
                    continue;
                }
                if (count < 0) {
                    return false;
                }
                if (count == 0) {
                    break;
                }
                std::size_t offset = 0;
                while (offset < static_cast<std::size_t>(count)) {
                    constexpr auto name_offset = offsetof(ScratchLinuxDirent64, name);
                    ScratchLinuxDirent64 record{};
                    if (static_cast<std::size_t>(count) - offset < name_offset) {
                        return false;
                    }
                    std::memcpy(&record, buffer.data() + offset, name_offset);
                    if (record.record_length < name_offset + 1U ||
                        record.record_length > static_cast<std::size_t>(count) - offset) {
                        return false;
                    }
                    const auto maximum_name = record.record_length - name_offset;
                    const auto* raw_name_data = buffer.data() + offset + name_offset;
                    const auto* terminator =
                        static_cast<const char*>(std::memchr(raw_name_data, '\0', maximum_name));
                    if (terminator == nullptr) {
                        return false;
                    }
                    const QByteArray raw_name(raw_name_data,
                                              static_cast<qsizetype>(terminator - raw_name_data));
                    offset += record.record_length;
                    if (raw_name == QByteArrayView(".") || raw_name == QByteArrayView("..")) {
                        continue;
                    }
                    const auto found = std::ranges::lower_bound(expected_children, raw_name);
                    if (found == expected_children.end() || *found != raw_name) {
                        recordUnexpectedRawPath(directory, raw_name);
                        read_event.component = raw_name;
                        if (invoke_hooks) {
                            static_cast<void>(scratchFinishes(hooks_, read_event, read_action));
                        }
                        if (inventory_diagnostic_.isEmpty()) {
                            inventory_diagnostic_ =
                                QStringLiteral("unexpected raw directory entry");
                        }
                        return false;
                    }
                    const auto index = static_cast<std::size_t>(found - expected_children.begin());
                    if (seen.at(index) != 0) {
                        return false;
                    }
                    seen.at(index) = 1;
                }
            }
            if (!std::ranges::all_of(seen, [](unsigned char value) { return value != 0; })) {
                if (invoke_hooks) {
                    static_cast<void>(scratchFinishes(hooks_, read_event, read_action));
                }
                inventory_diagnostic_ = QStringLiteral("missing tracked directory entry");
                return false;
            }
            if (invoke_hooks && !scratchFinishes(hooks_, read_event, read_action)) {
                return false;
            }
            SecureScratchObservation rebound_event{SecureScratchEvent::DirectoryEnumerateRebind,
                                                   directory.absolute_path, directory.component};
            const auto rebound_action = invoke_hooks ? scratchAction(hooks_, rebound_event)
                                                     : SecureScratchInjectedAction::Continue;
            if (scratchFailsBefore(rebound_action)) {
                return false;
            }
            struct stat rebound{};
            const auto valid = ::fstat(enumeration.get(), &rebound) == 0 &&
                               sameScratchIdentity(rebound, directory.identity, false) &&
                               validateEntry(directory);
            return valid &&
                   (!invoke_hooks || scratchFinishes(hooks_, rebound_event, rebound_action));
        } catch (const std::bad_alloc&) {
            return false;
        }
    }

    [[nodiscard]] bool exactLedgerInventory(bool invoke_inventory_hooks = true) {
        for (auto& entry : entries_) {
            if (entry.removed) {
                continue;
            }
            if (entry.provisional_mode_zero) {
                const auto& children = plan_.directories.at(entry.plan_index).live_children;
                if (!children.empty() || !validateProvisionalDirectory(entry)) {
                    inventory_diagnostic_ = QStringLiteral("provisional directory changed");
                    return false;
                }
                continue;
            }
            if (!entry.directory && entry.identity.st_ino == 0 &&
                !establishProvisionalFileIdentity(entry)) {
                inventory_diagnostic_ = QStringLiteral("provisional file changed");
                return false;
            }
            if (!validateEntry(entry)) {
                inventory_diagnostic_ = QStringLiteral("entry changed before inventory proof");
                return false;
            }
            if (!scratchAclAbsent(entry.descriptor.get(), "system.posix_acl_access")) {
                inventory_diagnostic_ = QStringLiteral("entry access ACL changed");
                return false;
            }
            if (entry.directory &&
                !scratchAclAbsent(entry.descriptor.get(), "system.posix_acl_default")) {
                inventory_diagnostic_ = QStringLiteral("directory default ACL changed");
                return false;
            }
            if (entry.directory && !directoryInventoryMatches(
                                       entry, plan_.directories.at(entry.plan_index).live_children,
                                       invoke_inventory_hooks)) {
                if (inventory_diagnostic_.isEmpty()) {
                    inventory_diagnostic_ = QStringLiteral("directory contents changed");
                }
                return false;
            }
            if (!validateEntry(entry)) {
                inventory_diagnostic_ = QStringLiteral("entry changed after inventory proof");
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool entryNameAbsent(const Entry& entry) const {
        struct stat named{};
        while (true) {
            errno = 0;
            if (::fstatat(parentDescriptor(entry), entry.component.constData(), &named,
                          AT_SYMLINK_NOFOLLOW) == 0) {
                return false;
            }
            if (errno != EINTR) {
                return errno == ENOENT;
            }
        }
    }

    [[nodiscard]] bool validateControllers() const {
        const auto& controllers = context_.impl_->controllers;
        for (std::size_t index = 0; index < controllers.size(); ++index) {
            const auto& controller = controllers.at(index);
            struct stat held{};
            if (::fstat(controller.descriptor.get(), &held) != 0 ||
                held.st_dev != controller.identity.st_dev ||
                held.st_ino != controller.identity.st_ino ||
                held.st_uid != controller.identity.st_uid ||
                (held.st_mode & 07777) != (controller.identity.st_mode & 07777) ||
                !scratchControllerPolicy(held)) {
                return false;
            }
            if (index != 0) {
                struct stat named{};
                if (::fstatat(controllers.at(index - 1U).descriptor.get(),
                              controller.component.constData(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
                    named.st_dev != held.st_dev || named.st_ino != held.st_ino) {
                    return false;
                }
            }
            if (!scratchAclAbsent(controller.descriptor.get(), "system.posix_acl_access") ||
                !scratchAclAbsent(controller.descriptor.get(), "system.posix_acl_default")) {
                return false;
            }
            struct stat rebound{};
            if (::fstat(controller.descriptor.get(), &rebound) != 0 ||
                !sameScratchIdentity(rebound, controller.identity, false) ||
                !scratchControllerPolicy(rebound)) {
                return false;
            }
            if (index != 0) {
                struct stat named{};
                if (::fstatat(controllers.at(index - 1U).descriptor.get(),
                              controller.component.constData(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
                    !sameScratchIdentity(named, controller.identity, false)) {
                    return false;
                }
            }
        }
        return true;
    }

    void markCleanupBlocked() {
        if (hooks_.report == nullptr) {
            return;
        }
        hooks_.report->cleanup = SecureScratchCleanupOutcome::Preserved;
        hooks_.report->residue_identity_ambiguous = true;
        hooks_.report->remaining_ledger_paths.clear();
        for (const auto& entry : entries_) {
            if (!entry.removed) {
                hooks_.report->remaining_ledger_paths.push_back(entry.absolute_path);
            }
        }
        if (hooks_.report->workspace_path.isEmpty() && !entries_.empty()) {
            hooks_.report->workspace_path = entries_.front().absolute_path;
        }
    }

    [[nodiscard]] std::expected<void, Error> preserveCleanup(QString message) {
        cleanup_failure_reported_ = true;
        markCleanupBlocked();
        return fail(ErrorCode::CannotRead, std::move(message));
    }

    SecureScratchContext context_;
    SecureScratchHooks hooks_;
    SecureScratchPlan plan_;
    std::vector<Entry> entries_;
    QHash<QString, std::size_t> directories_;
    QHash<QString, std::size_t> plan_indices_;
    QByteArray root_name_;
    QString root_path_;
    SecureScratchInjectedAction pending_write_action_{SecureScratchInjectedAction::Continue};
    SecureScratchObservation pending_write_observation_;
    bool cleaned_{};
    bool cleanup_failure_reported_{};
    QString inventory_diagnostic_;
};

[[nodiscard]] std::expected<void, Error> extractArchiveSecure(const QString& archive_path,
                                                              const ZipInspection& inspection,
                                                              SecureScratchWorkspace& workspace) {
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
        const auto file = workspace.createFile(path);
        if (!file) {
            return std::unexpected(file.error());
        }
        if (const auto begun = workspace.beginWrite(*file); !begun) {
            return begun;
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
                written > static_cast<std::uint64_t>(declared_size) - chunk_size) {
                return fail(ErrorCode::InvalidManifest,
                            QStringLiteral("ZIP member exceeds its declared size"));
            }
            if (const auto output = workspace.write(
                    *file, QByteArrayView(buffer.data(), static_cast<qsizetype>(read_size)));
                !output) {
                return output;
            }
            written += chunk_size;
        }
        if (written != static_cast<std::uint64_t>(declared_size)) {
            return fail(ErrorCode::InvalidManifest,
                        QStringLiteral("ZIP member size or staging write is invalid"));
        }
        if (const auto finished = workspace.finishWrite(*file); !finished) {
            return finished;
        }
        seen.insert(path);
    }
    if (seen != expected || archive_read_has_encrypted_entries(reader.get()) > 0) {
        return fail(ErrorCode::InvalidManifest,
                    QStringLiteral("ZIP member set or encryption state is invalid"));
    }
    return {};
}
#endif

std::expected<LoadedPack, Error>
importArchiveThroughSecureScratch(const QString& archive_path, PackArchiveLimits limits,
                                  PackValidationScope scope, const SecureScratchHooks& hooks) {
    const auto inspection = inspectZip(archive_path, limits);
    if (!inspection) {
        return std::unexpected(inspection.error());
    }
    const auto member_set = validateManifestMemberSet(archive_path, *inspection);
    if (!member_set) {
        return std::unexpected(member_set.error());
    }
    auto context = acquireSecureScratchContext(hooks);
    if (!context) {
        return fail(ErrorCode::CannotRead, context.error().message);
    }
#if !defined(Q_OS_LINUX)
    static_cast<void>(scope);
    return fail(ErrorCode::CannotRead,
                QStringLiteral("Secure archive scratch is unavailable on this platform"));
#else
    auto workspace = SecureScratchWorkspace::create(std::move(*context), *inspection, hooks);
    if (!workspace) {
        return std::unexpected(workspace.error());
    }
    const auto cleanFailure =
        [&workspace](const Error& original) -> std::expected<LoadedPack, Error> {
        const auto cleaned = (*workspace)->cleanup();
        return cleaned ? std::unexpected(original) : std::unexpected(cleaned.error());
    };
    if (const auto extracted = extractArchiveSecure(archive_path, *inspection, **workspace);
        !extracted) {
        return cleanFailure(extracted.error());
    }
    if (const auto boundary = (*workspace)->readerBoundary(SecureScratchEvent::BeforeReader);
        !boundary) {
        return cleanFailure(boundary.error());
    }
    if (const auto rebound = (*workspace)->validateInventory(); !rebound) {
        return cleanFailure(rebound.error());
    }
    auto loaded = PackReader::readDirectory((*workspace)->absolutePath(), scope);
    if (const auto boundary = (*workspace)->readerBoundary(SecureScratchEvent::AfterReader);
        !boundary) {
        return cleanFailure(boundary.error());
    }
    if (const auto rebound = (*workspace)->validateInventory(); !rebound) {
        return cleanFailure(rebound.error());
    }
    const auto cleaned = (*workspace)->cleanup();
    if (!cleaned) {
        return std::unexpected(cleaned.error());
    }
    return loaded;
#endif
}

} // namespace detail

std::expected<LoadedPack, Error> PackArchive::importArchive(const QString& archive_path,
                                                            PackArchiveLimits limits,
                                                            PackValidationScope scope) {
    return detail::importArchiveThroughSecureScratch(archive_path, limits, scope, {});
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
