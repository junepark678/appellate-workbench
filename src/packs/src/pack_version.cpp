#include "appellate/packs/pack_version.hpp"

#include <QDate>
#include <QRegularExpression>

namespace appellate::packs {
namespace {

[[nodiscard]] bool isSemanticVersion(const QString& value) {
    static const QRegularExpression pattern(QStringLiteral(
        R"(^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)(?:\.(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*))*)?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$)"));
    return value.size() >= 5 && value.size() <= 128 && pattern.match(value).hasMatch();
}

[[nodiscard]] bool isCalendarVersion(const QString& value) {
    static const QRegularExpression shape(QStringLiteral(R"(^[2-9][0-9]{3}\.[0-9]{2}\.[0-9]{2}$)"));
    if (!shape.match(value).hasMatch()) {
        return false;
    }

    const auto year = value.first(4).toInt();
    const auto month = value.sliced(5, 2).toInt();
    const auto day = value.last(2).toInt();
    return QDate(year, month, day).isValid();
}

} // namespace

bool isValidPackVersion(const QString& value, std::uint32_t manifest_schema_version) {
    if (isSemanticVersion(value)) {
        return manifest_schema_version == 1 || manifest_schema_version == 2;
    }
    return manifest_schema_version == 2 && isCalendarVersion(value);
}

} // namespace appellate::packs
