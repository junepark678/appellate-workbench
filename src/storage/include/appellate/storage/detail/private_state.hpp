#pragma once

#include <QByteArray>
#include <QString>
#include <QStringView>

#include <expected>

namespace appellate::storage::detail {

// Every successful directory opener transfers ownership of the returned descriptor to the caller.
// These helpers deliberately fail closed outside the Linux release platform because exact POSIX ACL
// absence cannot otherwise be established by this checkpoint.
[[nodiscard]] auto openPrivateStateController(QStringView absolute_path)
    -> std::expected<int, QString>;

[[nodiscard]] auto openPrivateStateDirectory(QStringView absolute_path)
    -> std::expected<int, QString>;

[[nodiscard]] auto ensurePrivateStateDirectory(QStringView absolute_path,
                                               QStringView private_boundary)
    -> std::expected<int, QString>;

[[nodiscard]] auto validatePrivateStateDirectoryDescriptor(int descriptor)
    -> std::expected<void, QString>;

[[nodiscard]] auto validatePrivateStateControllerDescriptor(int descriptor)
    -> std::expected<void, QString>;

[[nodiscard]] auto normalizeNewPrivateStateFile(int descriptor, int expected_link_count)
    -> std::expected<void, QString>;

[[nodiscard]] auto validatePrivateStateFileDescriptor(int descriptor, int expected_link_count)
    -> std::expected<void, QString>;

[[nodiscard]] auto validatePrivateStateFileBinding(int descriptor, int parent_descriptor,
                                                   const QByteArray& name,
                                                   int expected_link_count = 1)
    -> std::expected<void, QString>;

} // namespace appellate::storage::detail
