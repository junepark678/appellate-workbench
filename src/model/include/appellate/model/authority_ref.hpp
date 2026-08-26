#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace appellate::model {

struct AuthorityId final {
    std::string value;

    friend bool operator==(const AuthorityId&, const AuthorityId&) = default;
};

enum class AuthorityType {
    Constitution,
    Statute,
    Rule,
    Regulation,
    Case,
    Order,
    AdministrativeDecision,
    Other,
};

enum class PrecedentialStatus {
    NotApplicable,
    Precedential,
    Nonprecedential,
};

// Intrinsic source facts captured when an authority is resolved. Whether the authority is
// primary/supporting, and its binding effect in a particular use, remain contextual.
struct AuthorityProvenance final {
    AuthorityType type{};
    std::string jurisdiction_id;
    std::string issuing_body_id;
    PrecedentialStatus precedential_status{};
    bool official_source{};
    std::string checked_on;
    std::string locator;
    std::string source_url;

    friend bool operator==(const AuthorityProvenance&, const AuthorityProvenance&) = default;
};

// Counts Unicode scalar values in strict UTF-8 while rejecting control characters. Canonical
// authority text follows JSON Schema maxLength semantics (scalars, not UTF-8 bytes or UTF-16
// code units), which keeps pack, engine, and persistence limits identical.
[[nodiscard]] inline bool isCanonicalAuthorityText(std::string_view value,
                                                   std::size_t maximum_scalars) {
    if (value.empty()) {
        return false;
    }
    std::size_t scalar_count = 0;
    for (std::size_t index = 0; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index]);
        char32_t scalar = 0;
        std::size_t width = 0;
        if (first <= 0x7fU) {
            scalar = first;
            width = 1;
        } else if (first >= 0xc2U && first <= 0xdfU) {
            scalar = static_cast<char32_t>(first & 0x1fU);
            width = 2;
        } else if (first >= 0xe0U && first <= 0xefU) {
            scalar = static_cast<char32_t>(first & 0x0fU);
            width = 3;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            scalar = static_cast<char32_t>(first & 0x07U);
            width = 4;
        } else {
            return false;
        }
        if (index + width > value.size()) {
            return false;
        }
        for (std::size_t continuation = 1; continuation < width; ++continuation) {
            const auto byte = static_cast<unsigned char>(value[index + continuation]);
            if ((byte & 0xc0U) != 0x80U) {
                return false;
            }
            scalar = (scalar << 6U) | static_cast<char32_t>(byte & 0x3fU);
        }
        if ((width == 3 && first == 0xe0U &&
             static_cast<unsigned char>(value[index + 1]) < 0xa0U) ||
            (width == 3 && first == 0xedU &&
             static_cast<unsigned char>(value[index + 1]) >= 0xa0U) ||
            (width == 4 && first == 0xf0U &&
             static_cast<unsigned char>(value[index + 1]) < 0x90U) ||
            (width == 4 && first == 0xf4U &&
             static_cast<unsigned char>(value[index + 1]) >= 0x90U) ||
            scalar > 0x10ffffU || (scalar >= 0xd800U && scalar <= 0xdfffU)) {
            return false;
        }
        if (scalar <= 0x1fU || scalar == 0x7fU || ++scalar_count > maximum_scalars) {
            return false;
        }
        index += width;
    }
    return true;
}

// Call after both values have passed canonical YYYY-MM-DD validation. Lexical order then equals
// calendar order and avoids boundary-specific timezone/date-library behavior.
[[nodiscard]] inline bool authorityVerificationNotBeforeSource(std::string_view source_version,
                                                               std::string_view checked_on) {
    return source_version.size() == 10 && checked_on.size() == 10 && checked_on >= source_version;
}

// Authority source URLs are durable legal-evidence identifiers, so every boundary uses one
// deliberately narrow canonical form: HTTPS, lowercase DNS host without user info or a port,
// printable ASCII path/query bytes, no fragment, and uppercase percent escapes.
[[nodiscard]] inline bool isCanonicalAuthoritySourceUrl(std::string_view value) {
    constexpr std::string_view prefix = "https://";
    if (!value.starts_with(prefix) || value.size() > 2048) {
        return false;
    }
    const auto host_start = prefix.size();
    const auto host_end = value.find_first_of("/?#", host_start);
    const auto host = value.substr(host_start, host_end - host_start);
    if (host.empty() || host.size() > 253 || host.front() == '.' || host.back() == '.' ||
        host.contains("..") || host.contains('@') || host.contains(':')) {
        return false;
    }
    std::size_t label_length = 0;
    bool label_starts_with_hyphen = false;
    for (std::size_t index = 0; index <= host.size(); ++index) {
        if (index == host.size() || host[index] == '.') {
            if (label_length == 0 || label_length > 63 || label_starts_with_hyphen ||
                host[index - 1] == '-') {
                return false;
            }
            label_length = 0;
            label_starts_with_hyphen = false;
            continue;
        }
        const auto character = static_cast<unsigned char>(host[index]);
        const auto alphanumeric = (character >= static_cast<unsigned char>('0') &&
                                   character <= static_cast<unsigned char>('9')) ||
                                  (character >= static_cast<unsigned char>('a') &&
                                   character <= static_cast<unsigned char>('z'));
        if (!alphanumeric && character != static_cast<unsigned char>('-')) {
            return false;
        }
        label_starts_with_hyphen = label_length == 0 && character == '-';
        ++label_length;
    }
    if (host_end == std::string_view::npos) {
        return true;
    }
    if (value[host_end] == '#') {
        return false;
    }
    for (std::size_t index = host_end; index < value.size(); ++index) {
        const auto character = static_cast<unsigned char>(value[index]);
        if (character < 0x21 || character > 0x7e || character == '"' || character == '<' ||
            character == '>' || character == '\\' || character == '`' || character == '{' ||
            character == '|' || character == '}' || character == '^' || character == '#') {
            return false;
        }
        if (character == '%') {
            if (index + 2 >= value.size()) {
                return false;
            }
            const auto canonical_hex = [](unsigned char digit) {
                return (digit >= static_cast<unsigned char>('0') &&
                        digit <= static_cast<unsigned char>('9')) ||
                       (digit >= static_cast<unsigned char>('A') &&
                        digit <= static_cast<unsigned char>('F'));
            };
            if (!canonical_hex(static_cast<unsigned char>(value[index + 1])) ||
                !canonical_hex(static_cast<unsigned char>(value[index + 2]))) {
                return false;
            }
            index += 2;
        }
    }
    return true;
}

struct AuthorityRef final {
    AuthorityId id;
    std::string citation;
    std::string source_version;
    std::string proposition;
    std::optional<AuthorityProvenance> provenance{};

    friend bool operator==(const AuthorityRef&, const AuthorityRef&) = default;
};

// A primary authority is required by construction. The engine additionally rejects an
// authority whose fields are empty before it emits or applies a rule-driven event.
struct AuthorityBasis final {
    AuthorityRef primary;
    std::vector<AuthorityRef> supporting;

    friend bool operator==(const AuthorityBasis&, const AuthorityBasis&) = default;
};

} // namespace appellate::model
