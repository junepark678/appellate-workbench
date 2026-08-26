#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QStringView>

#include <expected>

namespace appellate::ui {

// Filesystem boundary for portable Workflow/Oral session archives. Reads are bounded and reject
// symbolic-link inputs. Publications are create-only, durable, and private to the current user.
class SessionArchiveFile final {
  public:
    static constexpr qint64 maximum_bytes = 512LL * 1024LL * 1024LL;

    [[nodiscard]] static auto read(QStringView path) -> std::expected<QByteArray, QString>;
    [[nodiscard]] static auto publish(QByteArrayView archive, QStringView path)
        -> std::expected<void, QString>;
};

} // namespace appellate::ui
