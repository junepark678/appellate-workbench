#include "appellate/storage/oral_argument_codec.hpp"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSet>
#include <QStringView>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace appellate::storage {
namespace {

constexpr auto schema_version = "1";
constexpr auto configuration_type = "oral_argument.configuration";
constexpr auto counsel_answer_type = "oral_argument.counsel_answer";
constexpr auto opening_command_type = "oral_argument.opening_command";
constexpr auto counsel_command_type = "oral_argument.counsel_command";
constexpr auto event_type = "oral_argument.event";
constexpr qsizetype maximum_payload_bytes = 1024 * 1024;
constexpr qsizetype maximum_id_bytes = 160;
constexpr qsizetype maximum_answer_bytes = 16 * 1024;
constexpr qsizetype maximum_prompt_bytes = 640;
constexpr qsizetype maximum_utterance_bytes = 32 * 1024;
constexpr qsizetype maximum_probability_bytes = 1'100;
constexpr qsizetype maximum_citations = 32;
constexpr qsizetype maximum_grounding_refs = 64;
constexpr std::uint64_t maximum_event_sequence = 4'096;
constexpr std::uint64_t maximum_argument_seconds = 24 * 60 * 60;
constexpr std::uint64_t maximum_follow_up_depth = 16;
constexpr int maximum_json_depth = 64;

[[nodiscard]] auto fail(OralArgumentCodecErrorCode code, QString message)
    -> std::unexpected<OralArgumentCodecError> {
    return std::unexpected(OralArgumentCodecError{code, std::move(message)});
}

class RawJsonScanner final {
  public:
    explicit RawJsonScanner(QStringView input) : input_(input) {}

    [[nodiscard]] auto scan() -> std::expected<void, OralArgumentCodecError> {
        skipWhitespace();
        if (const auto parsed = parseValue(0); !parsed) {
            return parsed;
        }
        skipWhitespace();
        if (position_ != input_.size()) {
            return invalid(QStringLiteral("Trailing content after JSON value"));
        }
        return {};
    }

  private:
    [[nodiscard]] auto invalid(QString message) const -> std::unexpected<OralArgumentCodecError> {
        return fail(OralArgumentCodecErrorCode::InvalidJson, std::move(message));
    }

    void skipWhitespace() {
        while (position_ < input_.size()) {
            const auto character = input_.at(position_);
            if (character != u' ' && character != u'\t' && character != u'\r' &&
                character != u'\n') {
                break;
            }
            ++position_;
        }
    }

    [[nodiscard]] bool consume(QChar expected) {
        if (position_ >= input_.size() || input_.at(position_) != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    [[nodiscard]] bool consumeLiteral(QStringView literal) {
        if (!input_.sliced(position_).startsWith(literal)) {
            return false;
        }
        position_ += literal.size();
        return true;
    }

    [[nodiscard]] static auto hexValue(QChar character) -> std::optional<unsigned> {
        if (character >= u'0' && character <= u'9') {
            return static_cast<unsigned>(character.unicode() - u'0');
        }
        if (character >= u'a' && character <= u'f') {
            return static_cast<unsigned>(character.unicode() - u'a' + 10U);
        }
        if (character >= u'A' && character <= u'F') {
            return static_cast<unsigned>(character.unicode() - u'A' + 10U);
        }
        return std::nullopt;
    }

    [[nodiscard]] auto parseString() -> std::expected<QString, OralArgumentCodecError> {
        if (!consume(u'"')) {
            return invalid(QStringLiteral("Expected JSON string"));
        }
        QString result;
        while (position_ < input_.size()) {
            const auto character = input_.at(position_++);
            if (character == u'"') {
                return result;
            }
            if (character.unicode() < 0x20U) {
                return invalid(QStringLiteral("Unescaped control character in JSON string"));
            }
            if (character != u'\\') {
                result.append(character);
                continue;
            }
            if (position_ >= input_.size()) {
                return invalid(QStringLiteral("Incomplete JSON escape"));
            }
            const auto escaped = input_.at(position_++);
            switch (escaped.unicode()) {
            case u'"':
            case u'\\':
            case u'/':
                result.append(escaped);
                break;
            case u'b':
                result.append(u'\b');
                break;
            case u'f':
                result.append(u'\f');
                break;
            case u'n':
                result.append(u'\n');
                break;
            case u'r':
                result.append(u'\r');
                break;
            case u't':
                result.append(u'\t');
                break;
            case u'u': {
                if (input_.size() - position_ < 4) {
                    return invalid(QStringLiteral("Incomplete JSON Unicode escape"));
                }
                unsigned code_unit = 0;
                for (int index = 0; index < 4; ++index) {
                    const auto digit = hexValue(input_.at(position_++));
                    if (!digit) {
                        return invalid(QStringLiteral("Invalid JSON Unicode escape"));
                    }
                    code_unit = code_unit * 16U + *digit;
                }
                result.append(QChar{static_cast<char16_t>(code_unit)});
                break;
            }
            default:
                return invalid(QStringLiteral("Unknown JSON escape"));
            }
        }
        return invalid(QStringLiteral("Unterminated JSON string"));
    }

    [[nodiscard]] auto parseObject(int depth) -> std::expected<void, OralArgumentCodecError> {
        if (!consume(u'{')) {
            return invalid(QStringLiteral("Expected JSON object"));
        }
        skipWhitespace();
        if (consume(u'}')) {
            return {};
        }
        QSet<QString> members;
        while (true) {
            const auto key = parseString();
            if (!key) {
                return std::unexpected(key.error());
            }
            if (members.contains(*key)) {
                return fail(OralArgumentCodecErrorCode::DuplicateMember,
                            QStringLiteral("Duplicate JSON member %1").arg(*key));
            }
            members.insert(*key);
            skipWhitespace();
            if (!consume(u':')) {
                return invalid(QStringLiteral("Expected colon after JSON object member"));
            }
            skipWhitespace();
            if (const auto value = parseValue(depth + 1); !value) {
                return value;
            }
            skipWhitespace();
            if (consume(u'}')) {
                return {};
            }
            if (!consume(u',')) {
                return invalid(QStringLiteral("Expected comma in JSON object"));
            }
            skipWhitespace();
        }
    }

    [[nodiscard]] auto parseArray(int depth) -> std::expected<void, OralArgumentCodecError> {
        if (!consume(u'[')) {
            return invalid(QStringLiteral("Expected JSON array"));
        }
        skipWhitespace();
        if (consume(u']')) {
            return {};
        }
        while (true) {
            if (const auto value = parseValue(depth + 1); !value) {
                return value;
            }
            skipWhitespace();
            if (consume(u']')) {
                return {};
            }
            if (!consume(u',')) {
                return invalid(QStringLiteral("Expected comma in JSON array"));
            }
            skipWhitespace();
        }
    }

    [[nodiscard]] auto parseNumber() -> std::expected<void, OralArgumentCodecError> {
        static_cast<void>(consume(u'-'));
        if (consume(u'0')) {
            if (position_ < input_.size() && input_.at(position_).isDigit()) {
                return invalid(QStringLiteral("Leading zero in JSON number"));
            }
        } else {
            if (position_ >= input_.size() || input_.at(position_) < u'1' ||
                input_.at(position_) > u'9') {
                return invalid(QStringLiteral("Invalid JSON number"));
            }
            while (position_ < input_.size() && input_.at(position_).isDigit()) {
                ++position_;
            }
        }
        if (consume(u'.')) {
            const auto fraction_start = position_;
            while (position_ < input_.size() && input_.at(position_).isDigit()) {
                ++position_;
            }
            if (fraction_start == position_) {
                return invalid(QStringLiteral("Invalid JSON fraction"));
            }
        }
        if (position_ < input_.size() &&
            (input_.at(position_) == u'e' || input_.at(position_) == u'E')) {
            ++position_;
            if (position_ < input_.size() &&
                (input_.at(position_) == u'+' || input_.at(position_) == u'-')) {
                ++position_;
            }
            const auto exponent_start = position_;
            while (position_ < input_.size() && input_.at(position_).isDigit()) {
                ++position_;
            }
            if (exponent_start == position_) {
                return invalid(QStringLiteral("Invalid JSON exponent"));
            }
        }
        return {};
    }

    [[nodiscard]] auto parseValue(int depth) -> std::expected<void, OralArgumentCodecError> {
        if (depth > maximum_json_depth || position_ >= input_.size()) {
            return invalid(QStringLiteral("JSON is empty or too deeply nested"));
        }
        switch (input_.at(position_).unicode()) {
        case u'{':
            return parseObject(depth);
        case u'[':
            return parseArray(depth);
        case u'"': {
            const auto string = parseString();
            if (!string) {
                return std::unexpected(string.error());
            }
            return {};
        }
        case u't':
            if (consumeLiteral(u"true")) {
                return {};
            }
            break;
        case u'f':
            if (consumeLiteral(u"false")) {
                return {};
            }
            break;
        case u'n':
            if (consumeLiteral(u"null")) {
                return {};
            }
            break;
        default:
            if (input_.at(position_) == u'-' || input_.at(position_).isDigit()) {
                return parseNumber();
            }
            break;
        }
        return invalid(QStringLiteral("Invalid JSON value"));
    }

    QStringView input_;
    qsizetype position_{};
};

[[nodiscard]] auto rejectDuplicateMembers(QByteArrayView encoded)
    -> std::expected<void, OralArgumentCodecError> {
    const auto text = QString::fromUtf8(encoded.data(), encoded.size());
    if (text.toUtf8() != encoded) {
        return fail(OralArgumentCodecErrorCode::InvalidJson,
                    QStringLiteral("Oral-argument JSON is not valid UTF-8"));
    }
    return RawJsonScanner{text}.scan();
}

[[nodiscard]] auto exactKeys(const QJsonObject& object, std::initializer_list<QStringView> keys,
                             QStringView context) -> std::expected<void, OralArgumentCodecError> {
    QSet<QString> expected;
    for (const auto key : keys) {
        expected.insert(key.toString());
        if (!object.contains(key)) {
            return fail(OralArgumentCodecErrorCode::MissingField,
                        QStringLiteral("Missing %1.%2").arg(context, key));
        }
    }
    for (auto iterator = object.constBegin(); iterator != object.constEnd(); ++iterator) {
        if (!expected.contains(iterator.key())) {
            return fail(OralArgumentCodecErrorCode::UnexpectedField,
                        QStringLiteral("Unexpected %1.%2").arg(context, iterator.key()));
        }
    }
    return {};
}

[[nodiscard]] bool containsNull(QStringView value) {
    return std::ranges::find(value, QChar::Null) != value.end();
}

[[nodiscard]] bool roundTripsUtf8(std::string_view value) {
    const auto decoded = QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
    return decoded.toUtf8() == QByteArrayView(value.data(), static_cast<qsizetype>(value.size()));
}

[[nodiscard]] auto checkedText(std::string_view value, qsizetype maximum, QStringView context,
                               bool allow_empty = false)
    -> std::expected<QString, OralArgumentCodecError> {
    if (!roundTripsUtf8(value)) {
        return fail(OralArgumentCodecErrorCode::InvalidField,
                    QStringLiteral("%1 is not valid UTF-8").arg(context));
    }
    const auto text = QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
    if ((!allow_empty && text.isEmpty()) || text.size() > maximum ||
        text.toUtf8().size() > maximum || containsNull(text)) {
        return fail(OralArgumentCodecErrorCode::OutOfRange,
                    QStringLiteral("%1 is empty or exceeds its bound").arg(context));
    }
    return text;
}

[[nodiscard]] auto readString(const QJsonObject& object, QStringView key, qsizetype maximum,
                              QStringView context, bool allow_empty = false)
    -> std::expected<QString, OralArgumentCodecError> {
    const auto value = object.value(key);
    if (!value.isString()) {
        return fail(OralArgumentCodecErrorCode::InvalidField,
                    QStringLiteral("%1.%2 must be a string").arg(context, key));
    }
    const auto text = value.toString();
    if ((!allow_empty && text.isEmpty()) || text.size() > maximum ||
        text.toUtf8().size() > maximum || containsNull(text)) {
        return fail(OralArgumentCodecErrorCode::OutOfRange,
                    QStringLiteral("%1.%2 is empty or exceeds its bound").arg(context, key));
    }
    return text;
}

[[nodiscard]] bool isCanonicalId(QStringView value) {
    if (value.size() < 3 || value.size() > maximum_id_bytes) {
        return false;
    }
    bool has_separator = false;
    bool previous_separator = true;
    for (const auto character : value) {
        const auto code = character.unicode();
        const auto alphanumeric = (code >= u'a' && code <= u'z') || (code >= u'0' && code <= u'9');
        const auto separator = code == u'.' || code == u'-';
        if ((!alphanumeric && !separator) || (separator && previous_separator)) {
            return false;
        }
        has_separator = has_separator || separator;
        previous_separator = separator;
    }
    return has_separator && !previous_separator;
}

[[nodiscard]] bool isCanonicalUtc(QStringView value) {
    if (value.size() != 20 || !value.endsWith(u'Z')) {
        return false;
    }
    const auto parsed = QDateTime::fromString(value, Qt::ISODate);
    return parsed.isValid() && parsed.offsetFromUtc() == 0 &&
           parsed.toUTC().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'")) == value;
}

[[nodiscard]] auto checkedCanonicalId(const QString& value, QStringView context)
    -> std::expected<QString, OralArgumentCodecError> {
    if (!isCanonicalId(value)) {
        return fail(OralArgumentCodecErrorCode::InvalidField,
                    QStringLiteral("%1 is not a canonical namespaced ID").arg(context));
    }
    return value;
}

[[nodiscard]] auto checkedCanonicalUtc(const QString& value, QStringView context)
    -> std::expected<QString, OralArgumentCodecError> {
    if (!isCanonicalUtc(value)) {
        return fail(OralArgumentCodecErrorCode::InvalidField,
                    QStringLiteral("%1 must use canonical UTC YYYY-MM-DDThh:mm:ssZ").arg(context));
    }
    return value;
}

[[nodiscard]] auto readCanonicalId(const QJsonObject& object, QStringView key, QStringView context)
    -> std::expected<QString, OralArgumentCodecError> {
    const auto value = readString(object, key, maximum_id_bytes, context);
    if (!value) {
        return std::unexpected(value.error());
    }
    return checkedCanonicalId(*value, QStringLiteral("%1.%2").arg(context, key));
}

[[nodiscard]] auto readCanonicalUtc(const QJsonObject& object, QStringView key, QStringView context)
    -> std::expected<QString, OralArgumentCodecError> {
    const auto value = readString(object, key, 20, context);
    if (!value) {
        return std::unexpected(value.error());
    }
    return checkedCanonicalUtc(*value, QStringLiteral("%1.%2").arg(context, key));
}

[[nodiscard]] auto checkedId(std::string_view value, QStringView context)
    -> std::expected<QString, OralArgumentCodecError> {
    const auto text = checkedText(value, maximum_id_bytes, context);
    if (!text) {
        return std::unexpected(text.error());
    }
    if (!isCanonicalId(*text)) {
        return fail(OralArgumentCodecErrorCode::InvalidField,
                    QStringLiteral("%1 is not a canonical namespaced ID").arg(context));
    }
    return text;
}

[[nodiscard]] auto readId(const QJsonObject& object, QStringView key, QStringView context)
    -> std::expected<std::string, OralArgumentCodecError> {
    const auto text = readString(object, key, maximum_id_bytes, context);
    if (!text) {
        return std::unexpected(text.error());
    }
    if (!isCanonicalId(*text)) {
        return fail(OralArgumentCodecErrorCode::InvalidField,
                    QStringLiteral("%1.%2 is not a canonical namespaced ID").arg(context, key));
    }
    return text->toUtf8().toStdString();
}

[[nodiscard]] bool isLowercaseDigest(QStringView value) {
    return value.size() == 64 && std::ranges::all_of(value, [](QChar character) {
               return (character >= u'0' && character <= u'9') ||
                      (character >= u'a' && character <= u'f');
           });
}

[[nodiscard]] auto checkedDigest(std::string_view value, QStringView context)
    -> std::expected<QString, OralArgumentCodecError> {
    const auto text = checkedText(value, 64, context);
    if (!text) {
        return std::unexpected(text.error());
    }
    if (!isLowercaseDigest(*text)) {
        return fail(OralArgumentCodecErrorCode::InvalidField,
                    QStringLiteral("%1 must be a lowercase SHA-256 digest").arg(context));
    }
    return text;
}

[[nodiscard]] auto readDigest(const QJsonObject& object, QStringView key, QStringView context)
    -> std::expected<std::string, OralArgumentCodecError> {
    const auto text = readString(object, key, 64, context);
    if (!text) {
        return std::unexpected(text.error());
    }
    if (!isLowercaseDigest(*text)) {
        return fail(OralArgumentCodecErrorCode::InvalidField,
                    QStringLiteral("%1.%2 must be a lowercase SHA-256 digest").arg(context, key));
    }
    return text->toLatin1().toStdString();
}

[[nodiscard]] bool isCanonicalUnsignedInteger(std::string_view value) {
    return value == "0" || (!value.empty() && value.front() >= '1' && value.front() <= '9' &&
                            std::ranges::all_of(value, [](char character) {
                                return character >= '0' && character <= '9';
                            }));
}

template <typename Integer>
[[nodiscard]] auto parseUnsigned(const QJsonObject& object, QStringView key, Integer maximum,
                                 QStringView context)
    -> std::expected<Integer, OralArgumentCodecError> {
    const auto encoded = readString(object, key, 32, context);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    const auto bytes = encoded->toLatin1();
    const std::string_view view(bytes.constData(), static_cast<std::size_t>(bytes.size()));
    if (!isCanonicalUnsignedInteger(view)) {
        return fail(
            OralArgumentCodecErrorCode::InvalidField,
            QStringLiteral("%1.%2 must be a canonical decimal integer string").arg(context, key));
    }
    Integer result{};
    const auto parsed = std::from_chars(view.data(), view.data() + view.size(), result);
    if (parsed.ec == std::errc::result_out_of_range || result > maximum) {
        return fail(OralArgumentCodecErrorCode::OutOfRange,
                    QStringLiteral("%1.%2 is outside its allowed range").arg(context, key));
    }
    if (parsed.ec != std::errc{} || parsed.ptr != view.data() + view.size()) {
        return fail(OralArgumentCodecErrorCode::InvalidField,
                    QStringLiteral("%1.%2 is not an integer").arg(context, key));
    }
    return result;
}

template <typename Integer> [[nodiscard]] QString decimalString(Integer value) {
    return QString::fromStdString(std::to_string(value));
}

[[nodiscard]] auto expandScientific(std::string_view encoded)
    -> std::expected<std::string, OralArgumentCodecError> {
    const auto exponent_at = encoded.find_first_of("eE");
    if (exponent_at == std::string_view::npos) {
        return std::string(encoded);
    }
    const auto mantissa = encoded.substr(0, exponent_at);
    const auto exponent_text = encoded.substr(exponent_at + 1);
    int exponent{};
    const auto parsed = std::from_chars(exponent_text.data(),
                                        exponent_text.data() + exponent_text.size(), exponent);
    if (parsed.ec != std::errc{} || parsed.ptr != exponent_text.data() + exponent_text.size() ||
        mantissa.empty() || mantissa.front() == '-') {
        return fail(OralArgumentCodecErrorCode::InvalidField,
                    QStringLiteral("Probability cannot be represented canonically"));
    }
    std::string digits;
    digits.reserve(mantissa.size());
    std::size_t integer_digits = mantissa.size();
    if (const auto point = mantissa.find('.'); point != std::string_view::npos) {
        integer_digits = point;
        digits.append(mantissa.substr(0, point));
        digits.append(mantissa.substr(point + 1));
    } else {
        digits.assign(mantissa);
    }
    const auto decimal_position = static_cast<long long>(integer_digits) + exponent;
    std::string result;
    if (decimal_position <= 0) {
        result = "0.";
        result.append(static_cast<std::size_t>(-decimal_position), '0');
        result.append(digits);
    } else if (decimal_position >= static_cast<long long>(digits.size())) {
        result = digits;
        result.append(
            static_cast<std::size_t>(decimal_position - static_cast<long long>(digits.size())),
            '0');
    } else {
        const auto point = static_cast<std::size_t>(decimal_position);
        result.assign(digits.substr(0, point));
        result.push_back('.');
        result.append(digits.substr(point));
    }
    return result;
}

[[nodiscard]] auto canonicalProbability(double value)
    -> std::expected<QString, OralArgumentCodecError> {
    if (!std::isfinite(value) || std::signbit(value) || value < 0.0 || value > 1.0) {
        return fail(OralArgumentCodecErrorCode::OutOfRange,
                    QStringLiteral("Probability must be a finite value from zero through one"));
    }
    char buffer[64]{};
    const auto encoded =
        std::to_chars(std::begin(buffer), std::end(buffer), value, std::chars_format::general);
    if (encoded.ec != std::errc{}) {
        return fail(OralArgumentCodecErrorCode::InvalidField,
                    QStringLiteral("Probability cannot be encoded"));
    }
    const auto expanded =
        expandScientific(std::string_view(buffer, static_cast<std::size_t>(encoded.ptr - buffer)));
    if (!expanded || expanded->size() > static_cast<std::size_t>(maximum_probability_bytes)) {
        return expanded ? fail(OralArgumentCodecErrorCode::OutOfRange,
                               QStringLiteral("Canonical probability exceeds its bound"))
                        : std::unexpected(expanded.error());
    }
    return QString::fromLatin1(expanded->data(), static_cast<qsizetype>(expanded->size()));
}

[[nodiscard]] auto parseProbability(const QJsonObject& object, QStringView key, QStringView context)
    -> std::expected<double, OralArgumentCodecError> {
    const auto encoded = readString(object, key, maximum_probability_bytes, context);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    const auto bytes = encoded->toLatin1();
    const std::string_view view(bytes.constData(), static_cast<std::size_t>(bytes.size()));
    if (view.empty() || view.front() == '-' || view.front() == '+' ||
        view.find_first_of("eE") != std::string_view::npos || std::ranges::count(view, '.') > 1 ||
        !std::ranges::all_of(view, [](char character) {
            return (character >= '0' && character <= '9') || character == '.';
        })) {
        return fail(OralArgumentCodecErrorCode::InvalidField,
                    QStringLiteral("%1.%2 must be a canonical decimal probability string")
                        .arg(context, key));
    }
    double result{};
    const auto parsed =
        std::from_chars(view.data(), view.data() + view.size(), result, std::chars_format::fixed);
    if (parsed.ec != std::errc{} || parsed.ptr != view.data() + view.size() ||
        !std::isfinite(result) || result < 0.0 || result > 1.0) {
        return fail(OralArgumentCodecErrorCode::OutOfRange,
                    QStringLiteral("%1.%2 is outside the probability range").arg(context, key));
    }
    const auto canonical = canonicalProbability(result);
    if (!canonical || *canonical != *encoded) {
        return fail(OralArgumentCodecErrorCode::InvalidField,
                    QStringLiteral("%1.%2 is not in canonical decimal form").arg(context, key));
    }
    return result;
}

[[nodiscard]] auto parseRoot(QByteArrayView encoded, const QString& expected_type)
    -> std::expected<QJsonObject, OralArgumentCodecError> {
    if (encoded.isEmpty() || encoded.size() > maximum_payload_bytes) {
        return fail(OralArgumentCodecErrorCode::OutOfRange,
                    QStringLiteral("Oral-argument payload is empty or too large"));
    }
    if (const auto unique = rejectDuplicateMembers(encoded); !unique) {
        return std::unexpected(unique.error());
    }
    QJsonParseError parse_error;
    const auto document =
        QJsonDocument::fromJson(QByteArray(encoded.data(), encoded.size()), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        return fail(OralArgumentCodecErrorCode::InvalidJson,
                    QStringLiteral("Oral-argument payload must be one JSON object"));
    }
    const auto root = document.object();
    if (const auto keys = exactKeys(root, {u"payload", u"schema_version", u"type"}, u"root");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto version = readString(root, u"schema_version", 8, u"root");
    const auto type = readString(root, u"type", 64, u"root");
    if (!version || !type) {
        return fail(OralArgumentCodecErrorCode::InvalidField,
                    QStringLiteral("Oral-argument envelope is invalid"));
    }
    if (*version != QLatin1StringView(schema_version)) {
        return fail(OralArgumentCodecErrorCode::InvalidField,
                    QStringLiteral("Unsupported oral-argument schema version"));
    }
    if (*type != expected_type) {
        return fail(OralArgumentCodecErrorCode::InvalidField,
                    QStringLiteral("Unexpected oral-argument payload type"));
    }
    if (!root.value(u"payload").isObject()) {
        return fail(OralArgumentCodecErrorCode::InvalidField,
                    QStringLiteral("Oral-argument payload member must be an object"));
    }
    return root.value(u"payload").toObject();
}

[[nodiscard]] QByteArray envelope(QString type, QJsonObject payload) {
    return QJsonDocument{QJsonObject{
                             {QStringLiteral("payload"), std::move(payload)},
                             {QStringLiteral("schema_version"), QStringLiteral("1")},
                             {QStringLiteral("type"), std::move(type)},
                         }}
        .toJson(QJsonDocument::Compact);
}

[[nodiscard]] auto counselKindName(model::CounselActKind kind)
    -> std::expected<QString, OralArgumentCodecError> {
    switch (kind) {
    case model::CounselActKind::Answer:
        return QStringLiteral("answer");
    case model::CounselActKind::Concession:
        return QStringLiteral("concession");
    case model::CounselActKind::RecordClaim:
        return QStringLiteral("record_claim");
    }
    return fail(OralArgumentCodecErrorCode::InvalidField,
                QStringLiteral("Unknown counsel act kind"));
}

[[nodiscard]] auto parseCounselKind(QStringView name)
    -> std::expected<model::CounselActKind, OralArgumentCodecError> {
    if (name == u"answer") {
        return model::CounselActKind::Answer;
    }
    if (name == u"concession") {
        return model::CounselActKind::Concession;
    }
    if (name == u"record_claim") {
        return model::CounselActKind::RecordClaim;
    }
    return fail(OralArgumentCodecErrorCode::InvalidField,
                QStringLiteral("Unknown counsel act kind"));
}

[[nodiscard]] auto benchKindName(model::BenchActKind kind)
    -> std::expected<QString, OralArgumentCodecError> {
    switch (kind) {
    case model::BenchActKind::Question:
        return QStringLiteral("question");
    case model::BenchActKind::Interruption:
        return QStringLiteral("interruption");
    case model::BenchActKind::FollowUp:
        return QStringLiteral("follow_up");
    case model::BenchActKind::Hypothetical:
        return QStringLiteral("hypothetical");
    case model::BenchActKind::RecordPinDemand:
        return QStringLiteral("record_pin_demand");
    case model::BenchActKind::ClarificationRequest:
        return QStringLiteral("clarification_request");
    case model::BenchActKind::TimeExpired:
        return QStringLiteral("time_expired");
    }
    return fail(OralArgumentCodecErrorCode::InvalidField, QStringLiteral("Unknown bench act kind"));
}

[[nodiscard]] auto parseBenchKind(QStringView name)
    -> std::expected<model::BenchActKind, OralArgumentCodecError> {
    if (name == u"question") {
        return model::BenchActKind::Question;
    }
    if (name == u"interruption") {
        return model::BenchActKind::Interruption;
    }
    if (name == u"follow_up") {
        return model::BenchActKind::FollowUp;
    }
    if (name == u"hypothetical") {
        return model::BenchActKind::Hypothetical;
    }
    if (name == u"record_pin_demand") {
        return model::BenchActKind::RecordPinDemand;
    }
    if (name == u"clarification_request") {
        return model::BenchActKind::ClarificationRequest;
    }
    if (name == u"time_expired") {
        return model::BenchActKind::TimeExpired;
    }
    return fail(OralArgumentCodecErrorCode::InvalidField, QStringLiteral("Unknown bench act kind"));
}

[[nodiscard]] bool isQuestionKind(model::BenchActKind kind) {
    return kind != model::BenchActKind::TimeExpired;
}

[[nodiscard]] auto groundingKindName(model::GroundingKind kind)
    -> std::expected<QString, OralArgumentCodecError> {
    switch (kind) {
    case model::GroundingKind::Authority:
        return QStringLiteral("authority");
    case model::GroundingKind::BriefPassage:
        return QStringLiteral("brief_passage");
    case model::GroundingKind::RecordPage:
        return QStringLiteral("record_page");
    }
    return fail(OralArgumentCodecErrorCode::InvalidField, QStringLiteral("Unknown grounding kind"));
}

[[nodiscard]] auto parseGroundingKind(QStringView name)
    -> std::expected<model::GroundingKind, OralArgumentCodecError> {
    if (name == u"authority") {
        return model::GroundingKind::Authority;
    }
    if (name == u"brief_passage") {
        return model::GroundingKind::BriefPassage;
    }
    if (name == u"record_page") {
        return model::GroundingKind::RecordPage;
    }
    return fail(OralArgumentCodecErrorCode::InvalidField, QStringLiteral("Unknown grounding kind"));
}

[[nodiscard]] auto encodeCounselPayload(const model::CounselAnswer& answer)
    -> std::expected<QJsonObject, OralArgumentCodecError> {
    const auto kind = counselKindName(answer.kind);
    const auto text = checkedText(answer.text, maximum_answer_bytes, u"answer.text");
    const auto issue_id = checkedId(answer.issue_id, u"answer.issue_id");
    const auto confidence = canonicalProbability(answer.classification_confidence);
    const auto elapsed = answer.elapsed.count();
    if (!kind || !text || !issue_id || !confidence) {
        if (!kind) {
            return std::unexpected(kind.error());
        }
        if (!text) {
            return std::unexpected(text.error());
        }
        if (!issue_id) {
            return std::unexpected(issue_id.error());
        }
        return std::unexpected(confidence.error());
    }
    if (elapsed <= 0 || elapsed > static_cast<std::int64_t>(maximum_argument_seconds) ||
        answer.cited_grounding_ids.size() > static_cast<std::size_t>(maximum_citations)) {
        return fail(OralArgumentCodecErrorCode::OutOfRange,
                    QStringLiteral("Counsel answer time or citation count is out of range"));
    }
    QJsonArray citations;
    QSet<QString> seen;
    for (const auto& citation : answer.cited_grounding_ids) {
        const auto id = checkedId(citation, u"answer.cited_grounding_ids[]");
        if (!id) {
            return std::unexpected(id.error());
        }
        if (seen.contains(*id)) {
            return fail(OralArgumentCodecErrorCode::InvalidField,
                        QStringLiteral("Counsel grounding citations must be unique"));
        }
        seen.insert(*id);
        citations.append(*id);
    }
    return QJsonObject{
        {QStringLiteral("cited_grounding_ids"), citations},
        {QStringLiteral("classification_confidence"), *confidence},
        {QStringLiteral("elapsed_seconds"), decimalString(elapsed)},
        {QStringLiteral("issue_id"), *issue_id},
        {QStringLiteral("kind"), *kind},
        {QStringLiteral("text"), *text},
    };
}

[[nodiscard]] auto decodeCounselPayload(const QJsonObject& object, QStringView context)
    -> std::expected<model::CounselAnswer, OralArgumentCodecError> {
    if (const auto keys = exactKeys(object,
                                    {u"cited_grounding_ids", u"classification_confidence",
                                     u"elapsed_seconds", u"issue_id", u"kind", u"text"},
                                    context);
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto kind_text = readString(object, u"kind", 32, context);
    const auto text = readString(object, u"text", maximum_answer_bytes, context);
    const auto issue_id = readId(object, u"issue_id", context);
    const auto confidence = parseProbability(object, u"classification_confidence", context);
    const auto elapsed =
        parseUnsigned<std::uint64_t>(object, u"elapsed_seconds", maximum_argument_seconds, context);
    if (!kind_text || !text || !issue_id || !confidence || !elapsed) {
        if (!kind_text) {
            return std::unexpected(kind_text.error());
        }
        if (!text) {
            return std::unexpected(text.error());
        }
        if (!issue_id) {
            return std::unexpected(issue_id.error());
        }
        if (!confidence) {
            return std::unexpected(confidence.error());
        }
        return std::unexpected(elapsed.error());
    }
    const auto kind = parseCounselKind(*kind_text);
    if (!kind) {
        return std::unexpected(kind.error());
    }
    if (*elapsed == 0) {
        return fail(OralArgumentCodecErrorCode::OutOfRange,
                    QStringLiteral("Counsel answer elapsed time must be positive"));
    }
    const auto citations_value = object.value(u"cited_grounding_ids");
    if (!citations_value.isArray()) {
        return fail(OralArgumentCodecErrorCode::InvalidField,
                    QStringLiteral("%1.cited_grounding_ids must be an array").arg(context));
    }
    const auto citations_array = citations_value.toArray();
    if (citations_array.size() > maximum_citations) {
        return fail(OralArgumentCodecErrorCode::OutOfRange,
                    QStringLiteral("Too many counsel grounding citations"));
    }
    std::vector<std::string> citations;
    citations.reserve(static_cast<std::size_t>(citations_array.size()));
    QSet<QString> seen;
    for (const auto& value : citations_array) {
        if (!value.isString() || !isCanonicalId(value.toString()) ||
            seen.contains(value.toString())) {
            return fail(OralArgumentCodecErrorCode::InvalidField,
                        QStringLiteral("Counsel grounding citations must be unique canonical IDs"));
        }
        seen.insert(value.toString());
        citations.push_back(value.toString().toUtf8().toStdString());
    }
    return model::CounselAnswer{
        *kind,       text->toUtf8().toStdString(),
        *issue_id,   std::move(citations),
        *confidence, std::chrono::seconds{static_cast<std::int64_t>(*elapsed)}};
}

[[nodiscard]] auto encodeGroundingRef(const model::ArgumentGroundingRef& reference)
    -> std::expected<QJsonObject, OralArgumentCodecError> {
    const auto kind = groundingKindName(reference.kind);
    const auto id = checkedId(reference.id, u"event.bench.question.grounding[].id");
    if (!kind || !id) {
        return !kind ? std::unexpected(kind.error()) : std::unexpected(id.error());
    }
    if ((reference.kind == model::GroundingKind::RecordPage &&
         (!reference.page_number.has_value() || *reference.page_number == 0)) ||
        (reference.page_number.has_value() && *reference.page_number == 0)) {
        return fail(OralArgumentCodecErrorCode::InvalidField,
                    QStringLiteral("Grounding page number is invalid"));
    }
    return QJsonObject{
        {QStringLiteral("id"), *id},
        {QStringLiteral("kind"), *kind},
        {QStringLiteral("page_number"), reference.page_number.has_value()
                                            ? QJsonValue{decimalString(*reference.page_number)}
                                            : QJsonValue{QJsonValue::Null}},
    };
}

[[nodiscard]] auto decodeGroundingRef(const QJsonObject& object)
    -> std::expected<model::ArgumentGroundingRef, OralArgumentCodecError> {
    constexpr auto context = u"event.bench.question.grounding[]";
    if (const auto keys = exactKeys(object, {u"id", u"kind", u"page_number"}, context); !keys) {
        return std::unexpected(keys.error());
    }
    const auto id = readId(object, u"id", context);
    const auto kind_text = readString(object, u"kind", 32, context);
    if (!id || !kind_text) {
        return !id ? std::unexpected(id.error()) : std::unexpected(kind_text.error());
    }
    const auto kind = parseGroundingKind(*kind_text);
    if (!kind) {
        return std::unexpected(kind.error());
    }
    std::optional<std::uint32_t> page_number;
    const auto page = object.value(u"page_number");
    if (!page.isNull()) {
        const auto parsed = parseUnsigned<std::uint32_t>(
            object, u"page_number", std::numeric_limits<std::uint32_t>::max(), context);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        if (*parsed == 0) {
            return fail(OralArgumentCodecErrorCode::OutOfRange,
                        QStringLiteral("Grounding page number must be positive"));
        }
        page_number = *parsed;
    }
    if (*kind == model::GroundingKind::RecordPage && !page_number.has_value()) {
        return fail(OralArgumentCodecErrorCode::MissingField,
                    QStringLiteral("Record-page grounding requires a page number"));
    }
    return model::ArgumentGroundingRef{*kind, *id, page_number};
}

[[nodiscard]] auto encodeQuestion(const model::GroundedQuestion& question, model::BenchActKind kind,
                                  std::uint64_t event_sequence)
    -> std::expected<QJsonObject, OralArgumentCodecError> {
    const auto issue_id = checkedId(question.issue_id, u"event.bench.question.issue_id");
    const auto prompt =
        checkedText(question.prompt, maximum_prompt_bytes, u"event.bench.question.prompt");
    if (!issue_id || !prompt) {
        return !issue_id ? std::unexpected(issue_id.error()) : std::unexpected(prompt.error());
    }
    if (question.grounding.empty() ||
        question.grounding.size() > static_cast<std::size_t>(maximum_grounding_refs) ||
        (question.parent_act_sequence.has_value() &&
         (*question.parent_act_sequence == 0 || *question.parent_act_sequence >= event_sequence)) ||
        (question.recalls_concession && kind != model::BenchActKind::FollowUp)) {
        return fail(OralArgumentCodecErrorCode::InvalidField,
                    QStringLiteral("Grounded question shape is invalid"));
    }
    QJsonArray grounding;
    QSet<QString> seen;
    bool has_record_page = false;
    for (const auto& reference : question.grounding) {
        const auto encoded = encodeGroundingRef(reference);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        const auto id = QString::fromUtf8(reference.id);
        if (seen.contains(id)) {
            return fail(OralArgumentCodecErrorCode::InvalidField,
                        QStringLiteral("Question grounding IDs must be unique"));
        }
        seen.insert(id);
        has_record_page = has_record_page || reference.kind == model::GroundingKind::RecordPage;
        grounding.append(*encoded);
    }
    if (kind == model::BenchActKind::RecordPinDemand && !has_record_page) {
        return fail(OralArgumentCodecErrorCode::InvalidField,
                    QStringLiteral("Record-pin demand requires record-page grounding"));
    }
    return QJsonObject{
        {QStringLiteral("grounding"), grounding},
        {QStringLiteral("issue_id"), *issue_id},
        {QStringLiteral("parent_act_sequence"),
         question.parent_act_sequence.has_value()
             ? QJsonValue{decimalString(*question.parent_act_sequence)}
             : QJsonValue{QJsonValue::Null}},
        {QStringLiteral("prompt"), *prompt},
        {QStringLiteral("recalls_concession"), question.recalls_concession},
    };
}

[[nodiscard]] auto decodeQuestion(const QJsonObject& object, model::BenchActKind kind,
                                  std::uint64_t event_sequence)
    -> std::expected<model::GroundedQuestion, OralArgumentCodecError> {
    constexpr auto context = u"event.bench.question";
    if (const auto keys = exactKeys(
            object,
            {u"grounding", u"issue_id", u"parent_act_sequence", u"prompt", u"recalls_concession"},
            context);
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto issue_id = readId(object, u"issue_id", context);
    const auto prompt = readString(object, u"prompt", maximum_prompt_bytes, context);
    if (!issue_id || !prompt) {
        return !issue_id ? std::unexpected(issue_id.error()) : std::unexpected(prompt.error());
    }
    const auto recall_value = object.value(u"recalls_concession");
    if (!recall_value.isBool()) {
        return fail(OralArgumentCodecErrorCode::InvalidField,
                    QStringLiteral("event.bench.question.recalls_concession must be Boolean"));
    }
    const auto recalls = recall_value.toBool();
    if (recalls && kind != model::BenchActKind::FollowUp) {
        return fail(OralArgumentCodecErrorCode::InvalidField,
                    QStringLiteral("Only follow-ups may recall concessions"));
    }
    std::optional<std::uint64_t> parent;
    if (!object.value(u"parent_act_sequence").isNull()) {
        const auto parsed = parseUnsigned<std::uint64_t>(object, u"parent_act_sequence",
                                                         maximum_event_sequence, context);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        if (*parsed == 0 || *parsed >= event_sequence) {
            return fail(OralArgumentCodecErrorCode::OutOfRange,
                        QStringLiteral("Question parent sequence must precede its event"));
        }
        parent = *parsed;
    }
    const auto grounding_value = object.value(u"grounding");
    if (!grounding_value.isArray()) {
        return fail(OralArgumentCodecErrorCode::InvalidField,
                    QStringLiteral("event.bench.question.grounding must be an array"));
    }
    const auto array = grounding_value.toArray();
    if (array.isEmpty() || array.size() > maximum_grounding_refs) {
        return fail(OralArgumentCodecErrorCode::OutOfRange,
                    QStringLiteral("Question grounding is empty or too large"));
    }
    std::vector<model::ArgumentGroundingRef> grounding;
    grounding.reserve(static_cast<std::size_t>(array.size()));
    QSet<QString> seen;
    bool has_record_page = false;
    for (const auto& value : array) {
        if (!value.isObject()) {
            return fail(OralArgumentCodecErrorCode::InvalidField,
                        QStringLiteral("Question grounding entries must be objects"));
        }
        const auto reference = decodeGroundingRef(value.toObject());
        if (!reference) {
            return std::unexpected(reference.error());
        }
        const auto id = QString::fromUtf8(reference->id);
        if (seen.contains(id)) {
            return fail(OralArgumentCodecErrorCode::InvalidField,
                        QStringLiteral("Question grounding IDs must be unique"));
        }
        seen.insert(id);
        has_record_page = has_record_page || reference->kind == model::GroundingKind::RecordPage;
        grounding.push_back(*reference);
    }
    if (kind == model::BenchActKind::RecordPinDemand && !has_record_page) {
        return fail(OralArgumentCodecErrorCode::InvalidField,
                    QStringLiteral("Record-pin demand requires record-page grounding"));
    }
    return model::GroundedQuestion{*issue_id, prompt->toUtf8().toStdString(), std::move(grounding),
                                   parent, recalls};
}

[[nodiscard]] auto encodeEventPayload(const model::OralArgumentEvent& event)
    -> std::expected<QJsonObject, OralArgumentCodecError> {
    if (event.sequence == 0 || event.sequence > maximum_event_sequence) {
        return fail(OralArgumentCodecErrorCode::OutOfRange,
                    QStringLiteral("Oral-argument event sequence is out of range"));
    }
    const auto kind = benchKindName(event.bench.kind);
    const auto seat_id = checkedId(event.bench.seat_id, u"event.bench.seat_id");
    const auto utterance = checkedText(event.bench.rendered_utterance, maximum_utterance_bytes,
                                       u"event.bench.rendered_utterance");
    if (!kind || !seat_id || !utterance) {
        if (!kind) {
            return std::unexpected(kind.error());
        }
        if (!seat_id) {
            return std::unexpected(seat_id.error());
        }
        return std::unexpected(utterance.error());
    }
    QJsonValue counsel{QJsonValue::Null};
    if (event.counsel.has_value()) {
        const auto encoded = encodeCounselPayload(*event.counsel);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        counsel = *encoded;
    }
    QJsonValue question{QJsonValue::Null};
    if (event.bench.question.has_value()) {
        if (!isQuestionKind(event.bench.kind)) {
            return fail(OralArgumentCodecErrorCode::InvalidField,
                        QStringLiteral("Non-question bench act cannot carry grounding"));
        }
        const auto encoded =
            encodeQuestion(*event.bench.question, event.bench.kind, event.sequence);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        question = *encoded;
    } else if (isQuestionKind(event.bench.kind)) {
        return fail(OralArgumentCodecErrorCode::MissingField,
                    QStringLiteral("Question bench act requires grounded question data"));
    }
    return QJsonObject{
        {QStringLiteral("bench"),
         QJsonObject{
             {QStringLiteral("kind"), *kind},
             {QStringLiteral("question"), question},
             {QStringLiteral("rendered_utterance"), *utterance},
             {QStringLiteral("seat_id"), *seat_id},
         }},
        {QStringLiteral("counsel"), counsel},
        {QStringLiteral("sequence"), decimalString(event.sequence)},
    };
}

[[nodiscard]] auto decodeEventPayload(const QJsonObject& object)
    -> std::expected<model::OralArgumentEvent, OralArgumentCodecError> {
    if (const auto keys = exactKeys(object, {u"bench", u"counsel", u"sequence"}, u"event"); !keys) {
        return std::unexpected(keys.error());
    }
    const auto sequence =
        parseUnsigned<std::uint64_t>(object, u"sequence", maximum_event_sequence, u"event");
    if (!sequence) {
        return std::unexpected(sequence.error());
    }
    if (*sequence == 0) {
        return fail(OralArgumentCodecErrorCode::OutOfRange,
                    QStringLiteral("Oral-argument event sequence must be positive"));
    }
    std::optional<model::CounselAnswer> counsel;
    const auto counsel_value = object.value(u"counsel");
    if (!counsel_value.isNull()) {
        if (!counsel_value.isObject()) {
            return fail(OralArgumentCodecErrorCode::InvalidField,
                        QStringLiteral("event.counsel must be an object or null"));
        }
        const auto decoded = decodeCounselPayload(counsel_value.toObject(), u"event.counsel");
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        counsel = *decoded;
    }
    const auto bench_value = object.value(u"bench");
    if (!bench_value.isObject()) {
        return fail(OralArgumentCodecErrorCode::InvalidField,
                    QStringLiteral("event.bench must be an object"));
    }
    const auto bench = bench_value.toObject();
    if (const auto keys = exactKeys(
            bench, {u"kind", u"question", u"rendered_utterance", u"seat_id"}, u"event.bench");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto kind_text = readString(bench, u"kind", 32, u"event.bench");
    const auto seat_id = readId(bench, u"seat_id", u"event.bench");
    const auto utterance =
        readString(bench, u"rendered_utterance", maximum_utterance_bytes, u"event.bench");
    if (!kind_text || !seat_id || !utterance) {
        if (!kind_text) {
            return std::unexpected(kind_text.error());
        }
        if (!seat_id) {
            return std::unexpected(seat_id.error());
        }
        return std::unexpected(utterance.error());
    }
    const auto kind = parseBenchKind(*kind_text);
    if (!kind) {
        return std::unexpected(kind.error());
    }
    std::optional<model::GroundedQuestion> question;
    const auto question_value = bench.value(u"question");
    if (!question_value.isNull()) {
        if (!question_value.isObject() || !isQuestionKind(*kind)) {
            return fail(OralArgumentCodecErrorCode::InvalidField,
                        QStringLiteral("event.bench.question has an invalid shape"));
        }
        const auto decoded = decodeQuestion(question_value.toObject(), *kind, *sequence);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        question = *decoded;
    } else if (isQuestionKind(*kind)) {
        return fail(OralArgumentCodecErrorCode::MissingField,
                    QStringLiteral("Question bench act requires grounded question data"));
    }
    return model::OralArgumentEvent{
        *sequence,
        counsel,
        model::BenchAct{*kind, *seat_id, question, utterance->toUtf8().toStdString()},
    };
}

} // namespace

std::expected<QByteArray, OralArgumentCodecError>
encodeOralArgumentConfiguration(const model::OralArgumentConfiguration& configuration) {
    const auto threshold = canonicalProbability(configuration.classification_confidence_threshold);
    const auto behavior = checkedDigest(configuration.behavior_definition_digest,
                                        u"configuration.behavior_definition_digest");
    const auto grounding =
        checkedDigest(configuration.grounding_digest, u"configuration.grounding_digest");
    const auto legal =
        checkedDigest(configuration.legal_state_digest, u"configuration.legal_state_digest");
    const auto disposition =
        checkedId(configuration.authored_disposition_id, u"configuration.authored_disposition_id");
    if (!threshold || !behavior || !grounding || !legal || !disposition) {
        if (!threshold) {
            return std::unexpected(threshold.error());
        }
        if (!behavior) {
            return std::unexpected(behavior.error());
        }
        if (!grounding) {
            return std::unexpected(grounding.error());
        }
        if (!legal) {
            return std::unexpected(legal.error());
        }
        return std::unexpected(disposition.error());
    }
    const auto principal = configuration.principal_time.count();
    const auto rebuttal = configuration.rebuttal_time.count();
    if (principal <= 0 || principal > static_cast<std::int64_t>(maximum_argument_seconds) ||
        rebuttal < 0 || rebuttal > static_cast<std::int64_t>(maximum_argument_seconds) ||
        configuration.maximum_follow_up_depth > maximum_follow_up_depth) {
        return fail(OralArgumentCodecErrorCode::OutOfRange,
                    QStringLiteral("Oral-argument configuration is outside its bounds"));
    }
    return envelope(QString::fromLatin1(configuration_type),
                    QJsonObject{
                        {QStringLiteral("authored_disposition_id"), *disposition},
                        {QStringLiteral("behavior_definition_digest"), *behavior},
                        {QStringLiteral("classification_confidence_threshold"), *threshold},
                        {QStringLiteral("grounding_digest"), *grounding},
                        {QStringLiteral("legal_state_digest"), *legal},
                        {QStringLiteral("maximum_follow_up_depth"),
                         decimalString(configuration.maximum_follow_up_depth)},
                        {QStringLiteral("principal_seconds"), decimalString(principal)},
                        {QStringLiteral("rebuttal_seconds"), decimalString(rebuttal)},
                    });
}

std::expected<model::OralArgumentConfiguration, OralArgumentCodecError>
decodeOralArgumentConfiguration(QByteArrayView encoded) {
    const auto payload = parseRoot(encoded, QString::fromLatin1(configuration_type));
    if (!payload) {
        return std::unexpected(payload.error());
    }
    if (const auto keys = exactKeys(*payload,
                                    {u"authored_disposition_id", u"behavior_definition_digest",
                                     u"classification_confidence_threshold", u"grounding_digest",
                                     u"legal_state_digest", u"maximum_follow_up_depth",
                                     u"principal_seconds", u"rebuttal_seconds"},
                                    u"configuration");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto principal = parseUnsigned<std::uint64_t>(*payload, u"principal_seconds",
                                                        maximum_argument_seconds, u"configuration");
    const auto rebuttal = parseUnsigned<std::uint64_t>(*payload, u"rebuttal_seconds",
                                                       maximum_argument_seconds, u"configuration");
    const auto threshold =
        parseProbability(*payload, u"classification_confidence_threshold", u"configuration");
    const auto follow_up = parseUnsigned<std::uint32_t>(
        *payload, u"maximum_follow_up_depth", static_cast<std::uint32_t>(maximum_follow_up_depth),
        u"configuration");
    const auto behavior = readDigest(*payload, u"behavior_definition_digest", u"configuration");
    const auto grounding = readDigest(*payload, u"grounding_digest", u"configuration");
    const auto legal = readDigest(*payload, u"legal_state_digest", u"configuration");
    const auto disposition = readId(*payload, u"authored_disposition_id", u"configuration");
    if (!principal || !rebuttal || !threshold || !follow_up || !behavior || !grounding || !legal ||
        !disposition) {
        if (!principal) {
            return std::unexpected(principal.error());
        }
        if (!rebuttal) {
            return std::unexpected(rebuttal.error());
        }
        if (!threshold) {
            return std::unexpected(threshold.error());
        }
        if (!follow_up) {
            return std::unexpected(follow_up.error());
        }
        if (!behavior) {
            return std::unexpected(behavior.error());
        }
        if (!grounding) {
            return std::unexpected(grounding.error());
        }
        if (!legal) {
            return std::unexpected(legal.error());
        }
        return std::unexpected(disposition.error());
    }
    if (*principal == 0) {
        return fail(OralArgumentCodecErrorCode::OutOfRange,
                    QStringLiteral("Principal argument time must be positive"));
    }
    return model::OralArgumentConfiguration{
        std::chrono::seconds{static_cast<std::chrono::seconds::rep>(*principal)},
        std::chrono::seconds{static_cast<std::chrono::seconds::rep>(*rebuttal)},
        *threshold,
        *follow_up,
        *behavior,
        *grounding,
        *legal,
        *disposition,
    };
}

std::expected<QByteArray, OralArgumentCodecError>
encodeCounselAnswer(const model::CounselAnswer& answer) {
    const auto payload = encodeCounselPayload(answer);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    return envelope(QString::fromLatin1(counsel_answer_type), *payload);
}

std::expected<model::CounselAnswer, OralArgumentCodecError>
decodeCounselAnswer(QByteArrayView encoded) {
    const auto payload = parseRoot(encoded, QString::fromLatin1(counsel_answer_type));
    if (!payload) {
        return std::unexpected(payload.error());
    }
    return decodeCounselPayload(*payload, u"answer");
}

std::expected<QByteArray, OralArgumentCodecError>
encodeOralArgumentOpeningCommand(const OralArgumentOpeningCommand& command) {
    const auto session_id = checkedCanonicalId(command.session_id, u"opening_command.session_id");
    const auto command_id = checkedCanonicalId(command.command_id, u"opening_command.command_id");
    const auto engine_revision =
        checkedCanonicalId(command.engine_revision, u"opening_command.engine_revision");
    const auto recorded_at =
        checkedCanonicalUtc(command.recorded_at_utc, u"opening_command.recorded_at_utc");
    if (!session_id || !command_id || !engine_revision || !recorded_at) {
        if (!session_id) {
            return std::unexpected(session_id.error());
        }
        if (!command_id) {
            return std::unexpected(command_id.error());
        }
        if (!engine_revision) {
            return std::unexpected(engine_revision.error());
        }
        return std::unexpected(recorded_at.error());
    }
    const auto configuration = encodeOralArgumentConfiguration(command.configuration);
    if (!configuration) {
        return std::unexpected(configuration.error());
    }
    const auto configuration_root = QJsonDocument::fromJson(*configuration).object();
    return envelope(QString::fromLatin1(opening_command_type),
                    QJsonObject{
                        {QStringLiteral("command_id"), *command_id},
                        {QStringLiteral("configuration"), configuration_root.value(u"payload")},
                        {QStringLiteral("engine_revision"), *engine_revision},
                        {QStringLiteral("recorded_at_utc"), *recorded_at},
                        {QStringLiteral("session_id"), *session_id},
                    });
}

std::expected<OralArgumentOpeningCommand, OralArgumentCodecError>
decodeOralArgumentOpeningCommand(QByteArrayView encoded) {
    const auto payload = parseRoot(encoded, QString::fromLatin1(opening_command_type));
    if (!payload) {
        return std::unexpected(payload.error());
    }
    if (const auto keys = exactKeys(*payload,
                                    {u"command_id", u"configuration", u"engine_revision",
                                     u"recorded_at_utc", u"session_id"},
                                    u"opening_command");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto session_id = readCanonicalId(*payload, u"session_id", u"opening_command");
    const auto command_id = readCanonicalId(*payload, u"command_id", u"opening_command");
    const auto engine_revision = readCanonicalId(*payload, u"engine_revision", u"opening_command");
    const auto recorded_at = readCanonicalUtc(*payload, u"recorded_at_utc", u"opening_command");
    const auto configuration_value = payload->value(u"configuration");
    if (!session_id || !command_id || !engine_revision || !recorded_at ||
        !configuration_value.isObject()) {
        if (!session_id) {
            return std::unexpected(session_id.error());
        }
        if (!command_id) {
            return std::unexpected(command_id.error());
        }
        if (!engine_revision) {
            return std::unexpected(engine_revision.error());
        }
        if (!recorded_at) {
            return std::unexpected(recorded_at.error());
        }
        return fail(OralArgumentCodecErrorCode::InvalidField,
                    QStringLiteral("opening_command.configuration must be an object"));
    }
    const auto configuration = decodeOralArgumentConfiguration(
        envelope(QString::fromLatin1(configuration_type), configuration_value.toObject()));
    if (!configuration) {
        return std::unexpected(configuration.error());
    }
    return OralArgumentOpeningCommand{*session_id, *command_id, *engine_revision, *recorded_at,
                                      *configuration};
}

std::expected<QByteArray, OralArgumentCodecError>
encodeOralArgumentCounselCommand(const OralArgumentCounselCommand& command) {
    const auto session_id = checkedCanonicalId(command.session_id, u"counsel_command.session_id");
    const auto command_id = checkedCanonicalId(command.command_id, u"counsel_command.command_id");
    const auto recorded_at =
        checkedCanonicalUtc(command.recorded_at_utc, u"counsel_command.recorded_at_utc");
    if (!session_id || !command_id || !recorded_at) {
        if (!session_id) {
            return std::unexpected(session_id.error());
        }
        if (!command_id) {
            return std::unexpected(command_id.error());
        }
        return std::unexpected(recorded_at.error());
    }
    const auto answer = encodeCounselPayload(command.answer);
    if (!answer) {
        return std::unexpected(answer.error());
    }
    return envelope(QString::fromLatin1(counsel_command_type),
                    QJsonObject{
                        {QStringLiteral("answer"), *answer},
                        {QStringLiteral("command_id"), *command_id},
                        {QStringLiteral("recorded_at_utc"), *recorded_at},
                        {QStringLiteral("session_id"), *session_id},
                    });
}

std::expected<OralArgumentCounselCommand, OralArgumentCodecError>
decodeOralArgumentCounselCommand(QByteArrayView encoded) {
    const auto payload = parseRoot(encoded, QString::fromLatin1(counsel_command_type));
    if (!payload) {
        return std::unexpected(payload.error());
    }
    if (const auto keys =
            exactKeys(*payload, {u"answer", u"command_id", u"recorded_at_utc", u"session_id"},
                      u"counsel_command");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto session_id = readCanonicalId(*payload, u"session_id", u"counsel_command");
    const auto command_id = readCanonicalId(*payload, u"command_id", u"counsel_command");
    const auto recorded_at = readCanonicalUtc(*payload, u"recorded_at_utc", u"counsel_command");
    const auto answer_value = payload->value(u"answer");
    if (!session_id || !command_id || !recorded_at || !answer_value.isObject()) {
        if (!session_id) {
            return std::unexpected(session_id.error());
        }
        if (!command_id) {
            return std::unexpected(command_id.error());
        }
        if (!recorded_at) {
            return std::unexpected(recorded_at.error());
        }
        return fail(OralArgumentCodecErrorCode::InvalidField,
                    QStringLiteral("counsel_command.answer must be an object"));
    }
    const auto answer = decodeCounselPayload(answer_value.toObject(), u"counsel_command.answer");
    if (!answer) {
        return std::unexpected(answer.error());
    }
    return OralArgumentCounselCommand{*session_id, *command_id, *recorded_at, *answer};
}

std::expected<QByteArray, OralArgumentCodecError>
encodeOralArgumentEvent(const model::OralArgumentEvent& event) {
    const auto payload = encodeEventPayload(event);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    return envelope(QString::fromLatin1(event_type), *payload);
}

std::expected<model::OralArgumentEvent, OralArgumentCodecError>
decodeOralArgumentEvent(QByteArrayView encoded) {
    const auto payload = parseRoot(encoded, QString::fromLatin1(event_type));
    if (!payload) {
        return std::unexpected(payload.error());
    }
    return decodeEventPayload(*payload);
}

} // namespace appellate::storage
