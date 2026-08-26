#pragma once

#include <QString>

#include <cstdint>

namespace appellate::packs {

// Pack-version syntax is selected by the manifest generation. Schema version 1 is the frozen
// SemVer contract. Schema version 2 additionally accepts an exact Gregorian YYYY.MM.DD release
// date in the inclusive year range 2000-9999. The input is validated, never normalized.
[[nodiscard]] bool isValidPackVersion(const QString& value, std::uint32_t manifest_schema_version);

} // namespace appellate::packs
