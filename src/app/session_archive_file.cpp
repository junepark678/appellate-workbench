#include "session_archive_file.hpp"

#include "appellate/storage/session_archive.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <expected>
#include <limits>
#include <utility>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef Q_OS_LINUX
#include <sys/xattr.h>
#endif

namespace appellate::ui {
namespace {

static_assert(SessionArchiveFile::maximum_bytes ==
              storage::SessionArchiveLimits::default_maximum_archive_bytes);

[[nodiscard]] auto fail(QString message) -> std::unexpected<QString> {
    return std::unexpected(std::move(message));
}

[[nodiscard]] bool invalidPath(QStringView path) {
    return path.isEmpty() || path.contains(QChar::Null);
}

#ifdef Q_OS_UNIX

class ScopedFileDescriptor final {
  public:
    explicit ScopedFileDescriptor(int descriptor = -1) : descriptor_(descriptor) {}
    ~ScopedFileDescriptor() {
        if (descriptor_ >= 0) {
            static_cast<void>(::close(descriptor_));
        }
    }

    ScopedFileDescriptor(const ScopedFileDescriptor&) = delete;
    ScopedFileDescriptor& operator=(const ScopedFileDescriptor&) = delete;

    ScopedFileDescriptor(ScopedFileDescriptor&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)) {}
    ScopedFileDescriptor& operator=(ScopedFileDescriptor&& other) noexcept {
        if (this != &other) {
            if (descriptor_ >= 0) {
                static_cast<void>(::close(descriptor_));
            }
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return descriptor_; }

  private:
    int descriptor_;
};

[[nodiscard]] QString systemError(QStringView operation, int error_number = errno) {
    return QStringLiteral("%1: %2").arg(operation,
                                        QString::fromLocal8Bit(std::strerror(error_number)));
}

[[nodiscard]] auto statDescriptor(int descriptor, QStringView description)
    -> std::expected<struct stat, QString> {
    struct stat metadata{};
    while (::fstat(descriptor, &metadata) != 0) {
        if (errno == EINTR) {
            continue;
        }
        return fail(systemError(QStringLiteral("Cannot inspect %1").arg(description)));
    }
    return metadata;
}

[[nodiscard]] bool stableInputMetadata(const struct stat& before, const struct stat& after) {
    const auto stable = before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
                        before.st_mode == after.st_mode && before.st_nlink == after.st_nlink &&
                        before.st_size == after.st_size;
#ifdef Q_OS_LINUX
    return stable && before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
           before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
           before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
           before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
#else
    return stable;
#endif
}

#ifdef Q_OS_LINUX
[[nodiscard]] auto descriptorHasAcl(int descriptor, const char* attribute)
    -> std::expected<bool, QString> {
    while (true) {
        const auto size = ::fgetxattr(descriptor, attribute, nullptr, 0);
        if (size >= 0) {
            return true;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == ENODATA) {
            return false;
        }
        return fail(systemError(QStringLiteral("Cannot prove archive path ACL safety")));
    }
}

[[nodiscard]] auto requireNoAcl(int descriptor, bool directory) -> std::expected<void, QString> {
    const auto access = descriptorHasAcl(descriptor, "system.posix_acl_access");
    if (!access) {
        return std::unexpected(access.error());
    }
    if (*access) {
        return fail(QStringLiteral("Archive path must not carry a POSIX access ACL"));
    }
    if (directory) {
        const auto inherited = descriptorHasAcl(descriptor, "system.posix_acl_default");
        if (!inherited) {
            return std::unexpected(inherited.error());
        }
        if (*inherited) {
            return fail(QStringLiteral("Archive parent must not carry a default POSIX ACL"));
        }
    }
    return {};
}
#else
[[nodiscard]] auto requireNoAcl(int, bool) -> std::expected<void, QString> { return {}; }
#endif

[[nodiscard]] auto requireSafeParent(int descriptor) -> std::expected<void, QString> {
    const auto metadata = statDescriptor(descriptor, u"archive parent directory");
    if (!metadata) {
        return std::unexpected(metadata.error());
    }
    if (!S_ISDIR(metadata->st_mode)) {
        return fail(QStringLiteral("Archive parent must be a real directory"));
    }
    const auto effective_user = ::geteuid();
    if (metadata->st_uid != effective_user && metadata->st_uid != 0) {
        return fail(QStringLiteral("Archive parent must be owned by the current user or root"));
    }
    const auto writable_by_others = metadata->st_mode & (S_IWGRP | S_IWOTH);
    if (writable_by_others != 0 && (metadata->st_mode & S_ISVTX) == 0) {
        return fail(
            QStringLiteral("Archive parent must not be group/world-writable unless it is sticky"));
    }
    return requireNoAcl(descriptor, true);
}

[[nodiscard]] auto writeAll(int descriptor, QByteArrayView bytes) -> std::expected<void, QString> {
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const auto remaining = bytes.size() - offset;
        const auto count = static_cast<std::size_t>(
            std::min<qsizetype>(remaining, static_cast<qsizetype>(1U << 20U)));
        const auto written = ::write(descriptor, bytes.data() + offset, count);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fail(systemError(u"Cannot write session archive staging file"));
        }
        if (written == 0) {
            return fail(QStringLiteral("Cannot make progress writing session archive"));
        }
        offset += static_cast<qsizetype>(written);
    }
    return {};
}

[[nodiscard]] auto syncDescriptor(int descriptor, QStringView description)
    -> std::expected<void, QString> {
    while (::fsync(descriptor) != 0) {
        if (errno == EINTR) {
            continue;
        }
        return fail(systemError(QStringLiteral("Cannot durably sync %1").arg(description)));
    }
    return {};
}

#endif

} // namespace

auto SessionArchiveFile::read(QStringView path) -> std::expected<QByteArray, QString> {
    if (invalidPath(path)) {
        return fail(QStringLiteral("Session archive path is empty or contains a null character"));
    }
#ifndef Q_OS_UNIX
    return fail(QStringLiteral("Secure session archive reads are unsupported on this platform"));
#else
    const auto absolute_path = QFileInfo(path.toString()).absoluteFilePath();
    const auto encoded_path = QFile::encodeName(absolute_path);
    int descriptor = -1;
    while (descriptor < 0) {
        descriptor =
            ::open(encoded_path.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (descriptor < 0 && errno == EINTR) {
            continue;
        }
        if (descriptor < 0) {
            return fail(systemError(u"Cannot open a regular non-symbolic-link session archive"));
        }
    }
    ScopedFileDescriptor input(descriptor);
    const auto metadata = statDescriptor(input.get(), u"session archive input");
    if (!metadata) {
        return std::unexpected(metadata.error());
    }
    if (!S_ISREG(metadata->st_mode)) {
        return fail(QStringLiteral("Session archive input must be a regular file"));
    }
    if (metadata->st_size < 0 || metadata->st_size > maximum_bytes) {
        return fail(QStringLiteral("Session archive exceeds the 512 MiB limit"));
    }

    const auto expected_size = static_cast<qint64>(metadata->st_size);
    QByteArray contents;
    contents.reserve(static_cast<qsizetype>(expected_size));
    constexpr std::size_t chunk_size = 1U << 20U;
    QByteArray chunk(static_cast<qsizetype>(chunk_size), Qt::Uninitialized);
    while (contents.size() < expected_size) {
        const auto remaining = expected_size - contents.size();
        const auto requested =
            static_cast<std::size_t>(std::min<qint64>(remaining, static_cast<qint64>(chunk_size)));
        const auto count = ::read(input.get(), chunk.data(), requested);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fail(systemError(u"Cannot read session archive"));
        }
        if (count == 0) {
            return fail(QStringLiteral("Session archive changed while it was being read"));
        }
        contents.append(chunk.constData(), static_cast<qsizetype>(count));
    }
    char growth_probe{};
    while (true) {
        const auto count = ::read(input.get(), &growth_probe, 1);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            return fail(systemError(u"Cannot finish reading session archive"));
        }
        if (count != 0) {
            return fail(QStringLiteral("Session archive changed or exceeds its declared size"));
        }
        break;
    }
    const auto final_metadata = statDescriptor(input.get(), u"session archive input");
    if (!final_metadata) {
        return std::unexpected(final_metadata.error());
    }
    if (!stableInputMetadata(*metadata, *final_metadata)) {
        return fail(QStringLiteral("Session archive changed while it was being read"));
    }
    return contents;
#endif
}

auto SessionArchiveFile::publish(QByteArrayView archive, QStringView path)
    -> std::expected<void, QString> {
    if (archive.isEmpty()) {
        return fail(QStringLiteral("An empty session archive cannot be published"));
    }
    if (archive.size() > maximum_bytes) {
        return fail(QStringLiteral("Session archive exceeds the 512 MiB limit"));
    }
    if (invalidPath(path)) {
        return fail(QStringLiteral("Session archive path is empty or contains a null character"));
    }
#ifndef Q_OS_UNIX
    return fail(
        QStringLiteral("Secure session archive publication is unsupported on this platform"));
#else
    const QFileInfo requested(path.toString());
    const auto absolute_path = requested.absoluteFilePath();
    const QFileInfo absolute(absolute_path);
    const auto leaf = QFile::encodeName(absolute.fileName());
    if (leaf.isEmpty() || leaf == "." || leaf == ".." || leaf.contains('/')) {
        return fail(QStringLiteral("Session archive destination name is invalid"));
    }

    const auto parent_path = absolute.absolutePath();
    const QFileInfo parent_info(parent_path);
    if (!parent_info.exists() || !parent_info.isDir() || parent_info.isSymbolicLink() ||
        parent_info.canonicalFilePath().isEmpty() ||
        QDir::cleanPath(parent_info.canonicalFilePath()) != QDir::cleanPath(parent_path)) {
        return fail(QStringLiteral("Session archive parent must be a canonical real directory"));
    }
    const auto encoded_parent = QFile::encodeName(parent_path);
    int parent_descriptor = -1;
    while (parent_descriptor < 0) {
        parent_descriptor =
            ::open(encoded_parent.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (parent_descriptor < 0 && errno == EINTR) {
            continue;
        }
        if (parent_descriptor < 0) {
            return fail(systemError(u"Cannot open session archive parent"));
        }
    }
    ScopedFileDescriptor parent(parent_descriptor);
    if (const auto safe = requireSafeParent(parent.get()); !safe) {
        return std::unexpected(safe.error());
    }

    struct stat destination_metadata{};
    if (::fstatat(parent.get(), leaf.constData(), &destination_metadata, AT_SYMLINK_NOFOLLOW) ==
        0) {
        return fail(QStringLiteral("Session archive export never overwrites an existing path"));
    }
    if (errno != ENOENT) {
        return fail(systemError(u"Cannot safely inspect session archive destination"));
    }

    QByteArray staging_name;
    ScopedFileDescriptor staging;
    for (int attempt = 0; attempt < 16; ++attempt) {
        staging_name = QStringLiteral(".awsessions-%1.tmp")
                           .arg(QUuid::createUuid().toString(QUuid::Id128))
                           .toLatin1();
        const auto candidate = ::openat(parent.get(), staging_name.constData(),
                                        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0000);
        if (candidate >= 0) {
            staging = ScopedFileDescriptor(candidate);
            break;
        }
        if (errno != EEXIST && errno != EINTR) {
            return fail(systemError(u"Cannot create private session archive staging file"));
        }
        if (errno == EINTR) {
            --attempt;
        }
    }
    if (staging.get() < 0) {
        return fail(QStringLiteral("Cannot allocate a unique session archive staging file"));
    }
    bool staging_exists = true;
    const auto remove_staging = [&] {
        if (staging_exists) {
            static_cast<void>(::unlinkat(parent.get(), staging_name.constData(), 0));
            staging_exists = false;
        }
    };

    if (::fchmod(staging.get(), S_IRUSR | S_IWUSR) != 0) {
        const auto error = systemError(u"Cannot make session archive staging file private");
        remove_staging();
        return fail(error);
    }
    const auto staging_metadata = statDescriptor(staging.get(), u"session archive staging file");
    if (!staging_metadata) {
        remove_staging();
        return std::unexpected(staging_metadata.error());
    }
    if (!S_ISREG(staging_metadata->st_mode) || staging_metadata->st_uid != ::geteuid() ||
        staging_metadata->st_nlink != static_cast<nlink_t>(1) ||
        (staging_metadata->st_mode & 0777) != 0600) {
        remove_staging();
        return fail(QStringLiteral("Session archive staging file is not a private owned file"));
    }
    if (const auto no_acl = requireNoAcl(staging.get(), false); !no_acl) {
        remove_staging();
        return std::unexpected(no_acl.error());
    }
    if (const auto written = writeAll(staging.get(), archive); !written) {
        remove_staging();
        return std::unexpected(written.error());
    }
    if (const auto synced = syncDescriptor(staging.get(), u"session archive contents"); !synced) {
        remove_staging();
        return std::unexpected(synced.error());
    }

    if (::linkat(parent.get(), staging_name.constData(), parent.get(), leaf.constData(), 0) != 0) {
        const auto link_error = errno;
        remove_staging();
        if (link_error == EEXIST) {
            return fail(QStringLiteral("Session archive export never overwrites an existing path"));
        }
        return fail(systemError(u"Cannot atomically publish session archive", link_error));
    }
    if (::unlinkat(parent.get(), staging_name.constData(), 0) != 0) {
        const auto unlink_error = errno;
        static_cast<void>(::unlinkat(parent.get(), leaf.constData(), 0));
        remove_staging();
        return fail(systemError(u"Cannot remove session archive staging link", unlink_error));
    }
    staging_exists = false;
    if (const auto synced = syncDescriptor(parent.get(), u"session archive parent directory");
        !synced) {
        static_cast<void>(::unlinkat(parent.get(), leaf.constData(), 0));
        static_cast<void>(syncDescriptor(parent.get(), u"session archive rollback"));
        return std::unexpected(synced.error());
    }
    return {};
#endif
}

} // namespace appellate::ui
