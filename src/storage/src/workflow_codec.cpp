#include "appellate/storage/workflow_codec.hpp"

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
#include <concepts>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace appellate::storage {
namespace {

constexpr auto schema_version = 1;
constexpr qsizetype maximum_payload_bytes = 1024 * 1024;
constexpr qsizetype maximum_id_characters = 160;
constexpr qsizetype maximum_text_characters = 4096;
constexpr qsizetype maximum_type_characters = 64;
constexpr qsizetype maximum_fields = 256;
constexpr qsizetype maximum_served_actors = 1024;
constexpr qsizetype maximum_requirements = 256;
constexpr qsizetype maximum_supporting_authorities = 32;
constexpr std::uint32_t maximum_events_per_command = 3;
constexpr int maximum_json_depth = 64;

constexpr auto submit_filing_type = "filing.submit";
constexpr auto enter_order_type = "order.enter";
constexpr auto set_sealed_type = "sealed.set";
constexpr auto schedule_argument_type = "argument.schedule";
constexpr auto issue_judgment_type = "judgment.issue";
constexpr auto issue_mandate_type = "mandate.issue";

constexpr auto filing_accepted_type = "filing.accepted";
constexpr auto filing_rejected_type = "filing.rejected";
constexpr auto deficiency_issued_type = "filing.deficiency_issued";
constexpr auto deadline_calculated_type = "deadline.calculated";
constexpr auto order_entered_type = "order.entered";
constexpr auto stage_advanced_type = "stage.advanced";
constexpr auto sealed_set_type = "sealed.set";
constexpr auto argument_scheduled_type = "argument.scheduled";
constexpr auto judgment_issued_type = "judgment.issued";
constexpr auto mandate_issued_type = "mandate.issued";

[[nodiscard]] auto fail(WorkflowCodecErrorCode code, QString message)
    -> std::unexpected<WorkflowCodecError> {
    return std::unexpected(WorkflowCodecError{code, std::move(message)});
}

class RawJsonScanner final {
  public:
    explicit RawJsonScanner(QStringView input) : input_(input) {}

    [[nodiscard]] auto scan() -> std::expected<void, WorkflowCodecError> {
        skipWhitespace();
        if (const auto value = parseValue(0); !value) {
            return value;
        }
        skipWhitespace();
        if (position_ != input_.size()) {
            return invalid(QStringLiteral("Trailing content after JSON value"));
        }
        return {};
    }

  private:
    [[nodiscard]] auto invalid(QString message) const -> std::unexpected<WorkflowCodecError> {
        return fail(WorkflowCodecErrorCode::InvalidJson, std::move(message));
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
        if (input_.sliced(position_).startsWith(literal)) {
            position_ += literal.size();
            return true;
        }
        return false;
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

    [[nodiscard]] auto parseString() -> std::expected<QString, WorkflowCodecError> {
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

    [[nodiscard]] auto parseObject(int depth) -> std::expected<void, WorkflowCodecError> {
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
                return fail(WorkflowCodecErrorCode::DuplicateMember,
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

    [[nodiscard]] auto parseArray(int depth) -> std::expected<void, WorkflowCodecError> {
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

    [[nodiscard]] auto parseNumber() -> std::expected<void, WorkflowCodecError> {
        const auto start = position_;
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
        if (position_ == start) {
            return invalid(QStringLiteral("Invalid JSON number"));
        }
        return {};
    }

    [[nodiscard]] auto parseValue(int depth) -> std::expected<void, WorkflowCodecError> {
        if (depth > maximum_json_depth || position_ >= input_.size()) {
            return invalid(QStringLiteral("JSON is empty or too deeply nested"));
        }
        switch (input_.at(position_).unicode()) {
        case u'{':
            return parseObject(depth);
        case u'[':
            return parseArray(depth);
        case u'"': {
            const auto value = parseString();
            if (!value) {
                return std::unexpected(value.error());
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
    -> std::expected<void, WorkflowCodecError> {
    const auto text = QString::fromUtf8(encoded.data(), encoded.size());
    if (text.toUtf8() != encoded) {
        return fail(WorkflowCodecErrorCode::InvalidJson,
                    QStringLiteral("Workflow JSON is not valid UTF-8"));
    }
    return RawJsonScanner{text}.scan();
}

[[nodiscard]] auto exactKeys(const QJsonObject& object, std::initializer_list<QStringView> keys,
                             QStringView context) -> std::expected<void, WorkflowCodecError> {
    QSet<QString> expected;
    for (const auto key : keys) {
        expected.insert(key.toString());
        if (!object.contains(key)) {
            return fail(WorkflowCodecErrorCode::MissingField,
                        QStringLiteral("Missing %1.%2").arg(context, key));
        }
    }
    for (auto iterator = object.constBegin(); iterator != object.constEnd(); ++iterator) {
        if (!expected.contains(iterator.key())) {
            return fail(WorkflowCodecErrorCode::UnexpectedField,
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

[[nodiscard]] auto readString(const QJsonObject& object, QStringView key, qsizetype maximum,
                              QStringView context, bool allow_empty = false)
    -> std::expected<QString, WorkflowCodecError> {
    const auto value = object.value(key);
    if (!value.isString()) {
        return fail(WorkflowCodecErrorCode::InvalidField,
                    QStringLiteral("%1.%2 must be a string").arg(context, key));
    }
    const auto result = value.toString();
    if ((!allow_empty && result.isEmpty()) || result.size() > maximum ||
        result.toUtf8().size() > maximum || containsNull(result)) {
        return fail(WorkflowCodecErrorCode::OutOfRange,
                    QStringLiteral("%1.%2 is empty or exceeds its bound").arg(context, key));
    }
    return result;
}

[[nodiscard]] bool isNamespacedId(QStringView value) {
    if (value.size() < 3 || value.size() > maximum_id_characters) {
        return false;
    }
    bool has_separator = false;
    bool previous_was_separator = true;
    for (const auto character : value) {
        const auto code = character.unicode();
        const auto alphanumeric = (code >= u'0' && code <= u'9') || (code >= u'a' && code <= u'z');
        const auto separator = code == u'.' || code == u'-';
        if ((!alphanumeric && !separator) || (separator && previous_was_separator)) {
            return false;
        }
        has_separator = has_separator || separator;
        previous_was_separator = separator;
    }
    return has_separator && !previous_was_separator;
}

[[nodiscard]] auto checkedId(std::string_view value, QStringView context)
    -> std::expected<QString, WorkflowCodecError> {
    if (!roundTripsUtf8(value)) {
        return fail(WorkflowCodecErrorCode::InvalidField,
                    QStringLiteral("%1 is not valid UTF-8").arg(context));
    }
    const auto result = QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
    if (!isNamespacedId(result)) {
        return fail(WorkflowCodecErrorCode::InvalidField,
                    QStringLiteral("%1 is not a canonical namespaced ID").arg(context));
    }
    return result;
}

[[nodiscard]] auto readId(const QJsonObject& object, QStringView key, QStringView context)
    -> std::expected<std::string, WorkflowCodecError> {
    const auto value = readString(object, key, maximum_id_characters, context);
    if (!value) {
        return std::unexpected(value.error());
    }
    if (!isNamespacedId(*value)) {
        return fail(WorkflowCodecErrorCode::InvalidField,
                    QStringLiteral("%1.%2 is not a canonical namespaced ID").arg(context, key));
    }
    return value->toUtf8().toStdString();
}

[[nodiscard]] bool isLowercaseDigest(QStringView value) {
    return value.size() == 64 && std::ranges::all_of(value, [](QChar character) {
               return (character >= u'0' && character <= u'9') ||
                      (character >= u'a' && character <= u'f');
           });
}

[[nodiscard]] auto checkedDigest(std::string_view value, QStringView context)
    -> std::expected<QString, WorkflowCodecError> {
    const auto digest = QString::fromLatin1(value.data(), static_cast<qsizetype>(value.size()));
    if (!isLowercaseDigest(digest)) {
        return fail(WorkflowCodecErrorCode::InvalidField,
                    QStringLiteral("%1 must be lowercase SHA-256").arg(context));
    }
    return digest;
}

[[nodiscard]] auto readDigest(const QJsonObject& object, QStringView key, QStringView context)
    -> std::expected<std::string, WorkflowCodecError> {
    const auto digest = readString(object, key, 64, context);
    if (!digest) {
        return std::unexpected(digest.error());
    }
    if (!isLowercaseDigest(*digest)) {
        return fail(WorkflowCodecErrorCode::InvalidField,
                    QStringLiteral("%1.%2 must be lowercase SHA-256").arg(context, key));
    }
    return digest->toLatin1().toStdString();
}

[[nodiscard]] auto formatDate(model::LegalDate date) -> std::expected<QString, WorkflowCodecError> {
    if (!date.value.ok()) {
        return fail(WorkflowCodecErrorCode::InvalidField, QStringLiteral("Invalid legal date"));
    }
    const auto year = static_cast<int>(date.value.year());
    if (year < 1 || year > 9999) {
        return fail(WorkflowCodecErrorCode::OutOfRange,
                    QStringLiteral("Legal date year must be between 0001 and 9999"));
    }
    return QStringLiteral("%1-%2-%3")
        .arg(year, 4, 10, QLatin1Char('0'))
        .arg(static_cast<unsigned>(date.value.month()), 2, 10, QLatin1Char('0'))
        .arg(static_cast<unsigned>(date.value.day()), 2, 10, QLatin1Char('0'));
}

[[nodiscard]] auto parseDateText(QStringView encoded, QStringView context)
    -> std::expected<model::LegalDate, WorkflowCodecError> {
    if (encoded.size() != 10 || encoded.at(4) != u'-' || encoded.at(7) != u'-') {
        return fail(WorkflowCodecErrorCode::InvalidField,
                    QStringLiteral("%1 must use YYYY-MM-DD").arg(context));
    }
    for (const auto index : {0, 1, 2, 3, 5, 6, 8, 9}) {
        if (encoded.at(index) < u'0' || encoded.at(index) > u'9') {
            return fail(WorkflowCodecErrorCode::InvalidField,
                        QStringLiteral("%1 must use YYYY-MM-DD").arg(context));
        }
    }
    bool year_ok = false;
    bool month_ok = false;
    bool day_ok = false;
    const auto year = encoded.first(4).toInt(&year_ok);
    const auto month = encoded.sliced(5, 2).toUInt(&month_ok);
    const auto day = encoded.last(2).toUInt(&day_ok);
    const auto result = model::LegalDate{std::chrono::year{year} / std::chrono::month{month} /
                                         std::chrono::day{day}};
    if (!year_ok || !month_ok || !day_ok || !result.value.ok() || year == 0) {
        return fail(WorkflowCodecErrorCode::InvalidField,
                    QStringLiteral("%1 is not a valid date").arg(context));
    }
    return result;
}

[[nodiscard]] auto parseDate(const QJsonObject& object, QStringView key, QStringView context)
    -> std::expected<model::LegalDate, WorkflowCodecError> {
    const auto encoded = readString(object, key, 10, context);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return parseDateText(*encoded, QStringLiteral("%1.%2").arg(context, key));
}

[[nodiscard]] bool isCanonicalSignedInteger(std::string_view value) {
    if (value == "0") {
        return true;
    }
    if (value.empty()) {
        return false;
    }
    auto digits = value;
    if (digits.front() == '-') {
        digits.remove_prefix(1);
    }
    return !digits.empty() && digits.front() >= '1' && digits.front() <= '9' &&
           std::ranges::all_of(digits,
                               [](char character) { return character >= '0' && character <= '9'; });
}

[[nodiscard]] bool isCanonicalUnsignedInteger(std::string_view value) {
    return value == "0" || (!value.empty() && value.front() >= '1' && value.front() <= '9' &&
                            std::ranges::all_of(value, [](char character) {
                                return character >= '0' && character <= '9';
                            }));
}

template <typename Integer>
[[nodiscard]] auto parseInteger(const QJsonObject& object, QStringView key, QStringView context,
                                bool allow_negative) -> std::expected<Integer, WorkflowCodecError> {
    const auto encoded = readString(object, key, 32, context);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    const auto bytes = encoded->toLatin1();
    const std::string_view view(bytes.constData(), static_cast<std::size_t>(bytes.size()));
    if ((allow_negative && !isCanonicalSignedInteger(view)) ||
        (!allow_negative && !isCanonicalUnsignedInteger(view))) {
        return fail(WorkflowCodecErrorCode::InvalidField,
                    QStringLiteral("%1.%2 must be a canonical decimal string").arg(context, key));
    }
    Integer result{};
    const auto parsed = std::from_chars(view.data(), view.data() + view.size(), result);
    if (parsed.ec == std::errc::result_out_of_range) {
        return fail(WorkflowCodecErrorCode::OutOfRange,
                    QStringLiteral("%1.%2 is outside its integer range").arg(context, key));
    }
    if (parsed.ec != std::errc{} || parsed.ptr != view.data() + view.size()) {
        return fail(WorkflowCodecErrorCode::InvalidField,
                    QStringLiteral("%1.%2 is not an integer").arg(context, key));
    }
    return result;
}

[[nodiscard]] auto encodeTime(const model::LegalTime& time)
    -> std::expected<QJsonObject, WorkflowCodecError> {
    const auto court_date = formatDate(time.court_date);
    if (!court_date) {
        return std::unexpected(court_date.error());
    }
    return QJsonObject{
        {QStringLiteral("court_date"), *court_date},
        {QStringLiteral("instant_unix_seconds"),
         QString::fromStdString(std::to_string(time.instant.time_since_epoch().count()))},
    };
}

[[nodiscard]] auto decodeTime(const QJsonObject& object, QStringView context)
    -> std::expected<model::LegalTime, WorkflowCodecError> {
    if (const auto keys = exactKeys(object, {u"court_date", u"instant_unix_seconds"}, context);
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto date = parseDate(object, u"court_date", context);
    const auto seconds = parseInteger<std::int64_t>(object, u"instant_unix_seconds", context, true);
    if (!date) {
        return std::unexpected(date.error());
    }
    if (!seconds) {
        return std::unexpected(seconds.error());
    }
    return model::LegalTime{std::chrono::sys_seconds{std::chrono::seconds{*seconds}}, *date};
}

[[nodiscard]] auto encodeAuthorityRef(const model::AuthorityRef& authority)
    -> std::expected<QJsonObject, WorkflowCodecError> {
    const auto id = checkedId(authority.id.value, u"authority.id");
    if (!id || !roundTripsUtf8(authority.citation) || !roundTripsUtf8(authority.source_version) ||
        !roundTripsUtf8(authority.proposition)) {
        return fail(WorkflowCodecErrorCode::IncompleteAuthority,
                    QStringLiteral("Authority fields must be complete valid UTF-8"));
    }
    const auto citation = QString::fromUtf8(authority.citation);
    const auto source_version = QString::fromUtf8(authority.source_version);
    const auto proposition = QString::fromUtf8(authority.proposition);
    if (citation.isEmpty() || citation.toUtf8().size() > maximum_text_characters ||
        containsNull(citation) || proposition.isEmpty() ||
        proposition.toUtf8().size() > maximum_text_characters || containsNull(proposition) ||
        !parseDateText(source_version, u"authority.source_version")) {
        return fail(WorkflowCodecErrorCode::IncompleteAuthority,
                    QStringLiteral("Authority fields must be complete and bounded"));
    }
    return QJsonObject{
        {QStringLiteral("citation"), citation},
        {QStringLiteral("id"), *id},
        {QStringLiteral("proposition"), proposition},
        {QStringLiteral("source_version"), source_version},
    };
}

[[nodiscard]] auto decodeAuthorityRef(const QJsonObject& object, QStringView context)
    -> std::expected<model::AuthorityRef, WorkflowCodecError> {
    if (const auto keys =
            exactKeys(object, {u"citation", u"id", u"proposition", u"source_version"}, context);
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto id = readId(object, u"id", context);
    const auto citation = readString(object, u"citation", maximum_text_characters, context);
    const auto source_version = readString(object, u"source_version", 10, context);
    const auto proposition = readString(object, u"proposition", maximum_text_characters, context);
    if (!id || !citation || !source_version || !proposition) {
        return fail(WorkflowCodecErrorCode::IncompleteAuthority,
                    QStringLiteral("Authority fields must be complete and bounded"));
    }
    if (!parseDateText(*source_version, QStringLiteral("%1.source_version").arg(context))) {
        return fail(WorkflowCodecErrorCode::IncompleteAuthority,
                    QStringLiteral("Authority source_version must be a valid YYYY-MM-DD date"));
    }
    return model::AuthorityRef{model::AuthorityId{*id}, citation->toUtf8().toStdString(),
                               source_version->toUtf8().toStdString(),
                               proposition->toUtf8().toStdString()};
}

[[nodiscard]] auto encodeAuthority(const model::AuthorityBasis& authority)
    -> std::expected<QJsonObject, WorkflowCodecError> {
    if (authority.supporting.size() > static_cast<std::size_t>(maximum_supporting_authorities)) {
        return fail(WorkflowCodecErrorCode::OutOfRange,
                    QStringLiteral("Too many supporting authorities"));
    }
    const auto primary = encodeAuthorityRef(authority.primary);
    if (!primary) {
        return std::unexpected(primary.error());
    }
    QJsonArray supporting;
    QSet<QString> identifiers{QString::fromUtf8(authority.primary.id.value)};
    for (const auto& item : authority.supporting) {
        const auto encoded = encodeAuthorityRef(item);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        const auto id = QString::fromUtf8(item.id.value);
        if (identifiers.contains(id)) {
            return fail(WorkflowCodecErrorCode::InvalidField,
                        QStringLiteral("Authority IDs must be unique"));
        }
        identifiers.insert(id);
        supporting.append(*encoded);
    }
    return QJsonObject{
        {QStringLiteral("primary"), *primary},
        {QStringLiteral("supporting"), supporting},
    };
}

[[nodiscard]] auto decodeAuthority(const QJsonObject& object, QStringView context)
    -> std::expected<model::AuthorityBasis, WorkflowCodecError> {
    if (const auto keys = exactKeys(object, {u"primary", u"supporting"}, context); !keys) {
        return std::unexpected(keys.error());
    }
    const auto primary_value = object.value(u"primary");
    const auto supporting_value = object.value(u"supporting");
    if (!primary_value.isObject() || !supporting_value.isArray()) {
        return fail(WorkflowCodecErrorCode::IncompleteAuthority,
                    QStringLiteral("Authority primary/supporting have invalid types"));
    }
    const auto primary =
        decodeAuthorityRef(primary_value.toObject(), QStringLiteral("%1.primary").arg(context));
    if (!primary) {
        return std::unexpected(primary.error());
    }
    const auto array = supporting_value.toArray();
    if (array.size() > maximum_supporting_authorities) {
        return fail(WorkflowCodecErrorCode::OutOfRange,
                    QStringLiteral("Too many supporting authorities"));
    }
    std::vector<model::AuthorityRef> supporting;
    supporting.reserve(static_cast<std::size_t>(array.size()));
    QSet<QString> identifiers{QString::fromUtf8(primary->id.value)};
    for (const auto& value : array) {
        if (!value.isObject()) {
            return fail(WorkflowCodecErrorCode::IncompleteAuthority,
                        QStringLiteral("Supporting authority must be an object"));
        }
        auto decoded =
            decodeAuthorityRef(value.toObject(), QStringLiteral("%1.supporting").arg(context));
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        const auto id = QString::fromUtf8(decoded->id.value);
        if (identifiers.contains(id)) {
            return fail(WorkflowCodecErrorCode::InvalidField,
                        QStringLiteral("Authority IDs must be unique"));
        }
        identifiers.insert(id);
        supporting.push_back(std::move(*decoded));
    }
    return model::AuthorityBasis{*primary, std::move(supporting)};
}

template <typename Id>
[[nodiscard]] auto decodeId(const QJsonObject& object, QStringView key, QStringView context)
    -> std::expected<Id, WorkflowCodecError> {
    const auto decoded = readId(object, key, context);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    return Id{*decoded};
}

template <typename Id>
[[nodiscard]] auto encodeOptionalId(const std::optional<Id>& id, QStringView context)
    -> std::expected<QJsonValue, WorkflowCodecError> {
    if (!id) {
        return QJsonValue{QJsonValue::Null};
    }
    const auto encoded = checkedId(id->value, context);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return QJsonValue{*encoded};
}

template <typename Id>
[[nodiscard]] auto decodeOptionalId(const QJsonObject& object, QStringView key, QStringView context)
    -> std::expected<std::optional<Id>, WorkflowCodecError> {
    const auto value = object.value(key);
    if (value.isNull()) {
        return std::optional<Id>{};
    }
    if (!value.isString() || !isNamespacedId(value.toString())) {
        return fail(
            WorkflowCodecErrorCode::InvalidField,
            QStringLiteral("%1.%2 must be null or a canonical namespaced ID").arg(context, key));
    }
    return std::optional<Id>{Id{value.toString().toUtf8().toStdString()}};
}

template <typename Id>
[[nodiscard]] auto encodeIdArray(const std::vector<Id>& values, qsizetype maximum,
                                 bool require_non_empty, QStringView context)
    -> std::expected<QJsonArray, WorkflowCodecError> {
    if ((require_non_empty && values.empty()) ||
        values.size() > static_cast<std::size_t>(maximum)) {
        return fail(WorkflowCodecErrorCode::OutOfRange,
                    QStringLiteral("%1 has an invalid size").arg(context));
    }
    QJsonArray result;
    QSet<QString> identifiers;
    for (const auto& value : values) {
        const auto encoded = checkedId(value.value, context);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        if (identifiers.contains(*encoded)) {
            return fail(WorkflowCodecErrorCode::InvalidField,
                        QStringLiteral("%1 IDs must be unique").arg(context));
        }
        identifiers.insert(*encoded);
        result.append(*encoded);
    }
    return result;
}

template <typename Id>
[[nodiscard]] auto decodeIdArray(const QJsonValue& value, qsizetype maximum, bool require_non_empty,
                                 QStringView context)
    -> std::expected<std::vector<Id>, WorkflowCodecError> {
    if (!value.isArray()) {
        return fail(WorkflowCodecErrorCode::InvalidField,
                    QStringLiteral("%1 must be an array").arg(context));
    }
    const auto array = value.toArray();
    if ((require_non_empty && array.isEmpty()) || array.size() > maximum) {
        return fail(WorkflowCodecErrorCode::OutOfRange,
                    QStringLiteral("%1 has an invalid size").arg(context));
    }
    std::vector<Id> result;
    result.reserve(static_cast<std::size_t>(array.size()));
    QSet<QString> identifiers;
    for (const auto& item : array) {
        if (!item.isString() || !isNamespacedId(item.toString()) ||
            identifiers.contains(item.toString())) {
            return fail(WorkflowCodecErrorCode::InvalidField,
                        QStringLiteral("%1 IDs must be unique canonical strings").arg(context));
        }
        identifiers.insert(item.toString());
        result.push_back(Id{item.toString().toUtf8().toStdString()});
    }
    return result;
}

[[nodiscard]] auto encodeFieldValues(const std::vector<model::WorkflowFieldValue>& fields)
    -> std::expected<QJsonArray, WorkflowCodecError> {
    if (fields.size() > static_cast<std::size_t>(maximum_fields)) {
        return fail(WorkflowCodecErrorCode::OutOfRange, QStringLiteral("Too many filing fields"));
    }
    QJsonArray result;
    QSet<QString> identifiers;
    for (const auto& field : fields) {
        const auto id = checkedId(field.id.value, u"payload.fields.id");
        if (!id || !roundTripsUtf8(field.value)) {
            return fail(WorkflowCodecErrorCode::InvalidField,
                        QStringLiteral("Filing field is not valid UTF-8"));
        }
        const auto value = QString::fromUtf8(field.value);
        if (value.toUtf8().size() > maximum_text_characters || containsNull(value) ||
            identifiers.contains(*id)) {
            return fail(WorkflowCodecErrorCode::InvalidField,
                        QStringLiteral("Filing fields must be unique and bounded"));
        }
        identifiers.insert(*id);
        result.append(QJsonObject{{QStringLiteral("id"), *id}, {QStringLiteral("value"), value}});
    }
    return result;
}

[[nodiscard]] auto decodeFieldValues(const QJsonValue& value)
    -> std::expected<std::vector<model::WorkflowFieldValue>, WorkflowCodecError> {
    if (!value.isArray() || value.toArray().size() > maximum_fields) {
        return fail(WorkflowCodecErrorCode::OutOfRange,
                    QStringLiteral("payload.fields must be a bounded array"));
    }
    std::vector<model::WorkflowFieldValue> result;
    const auto array = value.toArray();
    result.reserve(static_cast<std::size_t>(array.size()));
    QSet<QString> identifiers;
    for (const auto& item : array) {
        if (!item.isObject()) {
            return fail(WorkflowCodecErrorCode::InvalidField,
                        QStringLiteral("payload.fields entries must be objects"));
        }
        const auto object = item.toObject();
        if (const auto keys = exactKeys(object, {u"id", u"value"}, u"payload.fields"); !keys) {
            return std::unexpected(keys.error());
        }
        const auto id = readId(object, u"id", u"payload.fields");
        const auto field_value =
            readString(object, u"value", maximum_text_characters, u"payload.fields", true);
        if (!id) {
            return std::unexpected(id.error());
        }
        if (!field_value) {
            return std::unexpected(field_value.error());
        }
        const auto id_string = QString::fromUtf8(*id);
        if (identifiers.contains(id_string)) {
            return fail(WorkflowCodecErrorCode::InvalidField,
                        QStringLiteral("payload.fields IDs must be unique"));
        }
        identifiers.insert(id_string);
        result.push_back(model::WorkflowFieldValue{model::FilingFieldId{*id},
                                                   field_value->toUtf8().toStdString()});
    }
    return result;
}

[[nodiscard]] auto encodeCommandHeader(const model::WorkflowCommandHeader& header)
    -> std::expected<QJsonObject, WorkflowCodecError> {
    const auto session_id = checkedId(header.session_id, u"payload.session_id");
    const auto command_id = checkedId(header.command_id.value, u"payload.command_id");
    const auto actor_id = checkedId(header.actor_id.value, u"payload.actor_id");
    const auto occurred_at = encodeTime(header.occurred_at);
    if (!session_id) {
        return std::unexpected(session_id.error());
    }
    if (!command_id) {
        return std::unexpected(command_id.error());
    }
    if (!actor_id) {
        return std::unexpected(actor_id.error());
    }
    if (!occurred_at) {
        return std::unexpected(occurred_at.error());
    }
    return QJsonObject{
        {QStringLiteral("actor_id"), *actor_id},
        {QStringLiteral("command_id"), *command_id},
        {QStringLiteral("occurred_at"), *occurred_at},
        {QStringLiteral("session_id"), *session_id},
    };
}

[[nodiscard]] auto decodeCommandHeader(const QJsonObject& payload)
    -> std::expected<model::WorkflowCommandHeader, WorkflowCodecError> {
    const auto session_id = readId(payload, u"session_id", u"payload");
    const auto command_id = decodeId<model::WorkflowCommandId>(payload, u"command_id", u"payload");
    const auto actor_id = decodeId<model::ActorId>(payload, u"actor_id", u"payload");
    const auto occurred_value = payload.value(u"occurred_at");
    if (!session_id) {
        return std::unexpected(session_id.error());
    }
    if (!command_id) {
        return std::unexpected(command_id.error());
    }
    if (!actor_id) {
        return std::unexpected(actor_id.error());
    }
    if (!occurred_value.isObject()) {
        return fail(WorkflowCodecErrorCode::InvalidField,
                    QStringLiteral("payload.occurred_at must be an object"));
    }
    const auto occurred_at = decodeTime(occurred_value.toObject(), u"payload.occurred_at");
    if (!occurred_at) {
        return std::unexpected(occurred_at.error());
    }
    return model::WorkflowCommandHeader{*session_id, *command_id, *actor_id, *occurred_at};
}

[[nodiscard]] auto encodeOrderDisposition(model::WorkflowOrderDisposition disposition)
    -> std::expected<QString, WorkflowCodecError> {
    switch (disposition) {
    case model::WorkflowOrderDisposition::Granted:
        return QStringLiteral("granted");
    case model::WorkflowOrderDisposition::Denied:
        return QStringLiteral("denied");
    case model::WorkflowOrderDisposition::Other:
        return QStringLiteral("other");
    }
    return fail(WorkflowCodecErrorCode::InvalidField,
                QStringLiteral("Unknown workflow order disposition"));
}

[[nodiscard]] auto decodeOrderDisposition(const QJsonObject& object, QStringView key,
                                          QStringView context)
    -> std::expected<model::WorkflowOrderDisposition, WorkflowCodecError> {
    const auto value = readString(object, key, 16, context);
    if (!value) {
        return std::unexpected(value.error());
    }
    if (*value == u"granted") {
        return model::WorkflowOrderDisposition::Granted;
    }
    if (*value == u"denied") {
        return model::WorkflowOrderDisposition::Denied;
    }
    if (*value == u"other") {
        return model::WorkflowOrderDisposition::Other;
    }
    return fail(WorkflowCodecErrorCode::InvalidField,
                QStringLiteral("%1.%2 is an unknown order disposition").arg(context, key));
}

[[nodiscard]] auto encodeCommandPayload(const model::SubmitWorkflowFiling& command)
    -> std::expected<QJsonObject, WorkflowCodecError> {
    auto payload = encodeCommandHeader(command.header);
    const auto filing_id = checkedId(command.filing_id.value, u"payload.filing_id");
    const auto filing_type = checkedId(command.filing_type.value, u"payload.filing_type");
    const auto digest = checkedDigest(command.document_sha256, u"payload.document_sha256");
    const auto fields = encodeFieldValues(command.fields);
    const auto served = encodeIdArray(command.served_actors, maximum_served_actors, false,
                                      u"payload.served_actors");
    const auto cures =
        encodeOptionalId(command.cures_deficiency_id, u"payload.cures_deficiency_id");
    if (!payload) {
        return std::unexpected(payload.error());
    }
    if (!filing_id) {
        return std::unexpected(filing_id.error());
    }
    if (!filing_type) {
        return std::unexpected(filing_type.error());
    }
    if (!digest) {
        return std::unexpected(digest.error());
    }
    if (!fields) {
        return std::unexpected(fields.error());
    }
    if (!served) {
        return std::unexpected(served.error());
    }
    if (!cures) {
        return std::unexpected(cures.error());
    }
    if (std::ranges::find(command.served_actors, command.header.actor_id) !=
        command.served_actors.end()) {
        return fail(WorkflowCodecErrorCode::InvalidField,
                    QStringLiteral("Filer cannot appear in served_actors"));
    }
    payload->insert(QStringLiteral("cures_deficiency_id"), *cures);
    payload->insert(QStringLiteral("document_sha256"), *digest);
    payload->insert(QStringLiteral("fields"), *fields);
    payload->insert(QStringLiteral("filing_id"), *filing_id);
    payload->insert(QStringLiteral("filing_type"), *filing_type);
    payload->insert(QStringLiteral("served_actors"), *served);
    return payload;
}

[[nodiscard]] auto encodeCommandPayload(const model::EnterWorkflowOrder& command)
    -> std::expected<QJsonObject, WorkflowCodecError> {
    auto payload = encodeCommandHeader(command.header);
    const auto operation_id = checkedId(command.operation_id.value, u"payload.operation_id");
    const auto order_id = checkedId(command.order_id.value, u"payload.order_id");
    const auto disposition = encodeOrderDisposition(command.disposition);
    const auto digest = checkedDigest(command.document_sha256, u"payload.document_sha256");
    const auto extension =
        encodeOptionalId(command.extension_deadline_id, u"payload.extension_deadline_id");
    if (!payload) {
        return std::unexpected(payload.error());
    }
    if (!operation_id) {
        return std::unexpected(operation_id.error());
    }
    if (!order_id) {
        return std::unexpected(order_id.error());
    }
    if (!disposition) {
        return std::unexpected(disposition.error());
    }
    if (!digest) {
        return std::unexpected(digest.error());
    }
    if (!extension) {
        return std::unexpected(extension.error());
    }
    payload->insert(QStringLiteral("disposition"), *disposition);
    payload->insert(QStringLiteral("document_sha256"), *digest);
    payload->insert(QStringLiteral("extension_deadline_id"), *extension);
    payload->insert(QStringLiteral("operation_id"), *operation_id);
    payload->insert(QStringLiteral("order_id"), *order_id);
    return payload;
}

[[nodiscard]] auto encodeCommandPayload(const model::SetWorkflowSealed& command)
    -> std::expected<QJsonObject, WorkflowCodecError> {
    auto payload = encodeCommandHeader(command.header);
    const auto operation_id = checkedId(command.operation_id.value, u"payload.operation_id");
    if (!payload) {
        return std::unexpected(payload.error());
    }
    if (!operation_id) {
        return std::unexpected(operation_id.error());
    }
    payload->insert(QStringLiteral("operation_id"), *operation_id);
    payload->insert(QStringLiteral("sealed"), command.sealed);
    return payload;
}

[[nodiscard]] auto encodeCommandPayload(const model::ScheduleWorkflowArgument& command)
    -> std::expected<QJsonObject, WorkflowCodecError> {
    auto payload = encodeCommandHeader(command.header);
    const auto operation_id = checkedId(command.operation_id.value, u"payload.operation_id");
    const auto argument_date = formatDate(command.argument_date);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    if (!operation_id) {
        return std::unexpected(operation_id.error());
    }
    if (!argument_date) {
        return std::unexpected(argument_date.error());
    }
    payload->insert(QStringLiteral("argument_date"), *argument_date);
    payload->insert(QStringLiteral("operation_id"), *operation_id);
    return payload;
}

[[nodiscard]] auto encodeCommandPayload(const model::IssueWorkflowJudgment& command)
    -> std::expected<QJsonObject, WorkflowCodecError> {
    auto payload = encodeCommandHeader(command.header);
    const auto operation_id = checkedId(command.operation_id.value, u"payload.operation_id");
    const auto digest = checkedDigest(command.document_sha256, u"payload.document_sha256");
    if (!payload) {
        return std::unexpected(payload.error());
    }
    if (!operation_id) {
        return std::unexpected(operation_id.error());
    }
    if (!digest) {
        return std::unexpected(digest.error());
    }
    if (!roundTripsUtf8(command.disposition)) {
        return fail(WorkflowCodecErrorCode::InvalidField,
                    QStringLiteral("payload.disposition is not valid UTF-8"));
    }
    const auto disposition = QString::fromUtf8(command.disposition);
    if (disposition.isEmpty() || disposition.toUtf8().size() > maximum_text_characters ||
        containsNull(disposition)) {
        return fail(WorkflowCodecErrorCode::OutOfRange,
                    QStringLiteral("payload.disposition is empty or exceeds its bound"));
    }
    payload->insert(QStringLiteral("disposition"), disposition);
    payload->insert(QStringLiteral("document_sha256"), *digest);
    payload->insert(QStringLiteral("operation_id"), *operation_id);
    return payload;
}

[[nodiscard]] auto encodeCommandPayload(const model::IssueWorkflowMandate& command)
    -> std::expected<QJsonObject, WorkflowCodecError> {
    auto payload = encodeCommandHeader(command.header);
    const auto operation_id = checkedId(command.operation_id.value, u"payload.operation_id");
    const auto digest = checkedDigest(command.document_sha256, u"payload.document_sha256");
    if (!payload) {
        return std::unexpected(payload.error());
    }
    if (!operation_id) {
        return std::unexpected(operation_id.error());
    }
    if (!digest) {
        return std::unexpected(digest.error());
    }
    payload->insert(QStringLiteral("document_sha256"), *digest);
    payload->insert(QStringLiteral("operation_id"), *operation_id);
    return payload;
}

[[nodiscard]] auto decodeSubmitFiling(const QJsonObject& payload)
    -> std::expected<model::WorkflowCommand, WorkflowCodecError> {
    if (const auto keys = exactKeys(payload,
                                    {u"actor_id", u"command_id", u"cures_deficiency_id",
                                     u"document_sha256", u"fields", u"filing_id", u"filing_type",
                                     u"occurred_at", u"served_actors", u"session_id"},
                                    u"payload");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto header = decodeCommandHeader(payload);
    const auto filing_id = decodeId<model::WorkflowFilingId>(payload, u"filing_id", u"payload");
    const auto filing_type = decodeId<model::FilingTypeId>(payload, u"filing_type", u"payload");
    const auto digest = readDigest(payload, u"document_sha256", u"payload");
    const auto fields = decodeFieldValues(payload.value(u"fields"));
    const auto served = decodeIdArray<model::ActorId>(
        payload.value(u"served_actors"), maximum_served_actors, false, u"payload.served_actors");
    const auto cures =
        decodeOptionalId<model::WorkflowDeficiencyId>(payload, u"cures_deficiency_id", u"payload");
    if (!header) {
        return std::unexpected(header.error());
    }
    if (!filing_id) {
        return std::unexpected(filing_id.error());
    }
    if (!filing_type) {
        return std::unexpected(filing_type.error());
    }
    if (!digest) {
        return std::unexpected(digest.error());
    }
    if (!fields) {
        return std::unexpected(fields.error());
    }
    if (!served) {
        return std::unexpected(served.error());
    }
    if (!cures) {
        return std::unexpected(cures.error());
    }
    if (std::ranges::find(*served, header->actor_id) != served->end()) {
        return fail(WorkflowCodecErrorCode::InvalidField,
                    QStringLiteral("Filer cannot appear in served_actors"));
    }
    return model::SubmitWorkflowFiling{
        *header, *filing_id, *filing_type, *digest, std::move(*fields), std::move(*served), *cures};
}

[[nodiscard]] auto decodeEnterOrder(const QJsonObject& payload)
    -> std::expected<model::WorkflowCommand, WorkflowCodecError> {
    if (const auto keys = exactKeys(payload,
                                    {u"actor_id", u"command_id", u"disposition", u"document_sha256",
                                     u"extension_deadline_id", u"occurred_at", u"operation_id",
                                     u"order_id", u"session_id"},
                                    u"payload");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto header = decodeCommandHeader(payload);
    const auto operation_id =
        decodeId<model::WorkflowOperationId>(payload, u"operation_id", u"payload");
    const auto order_id = decodeId<model::WorkflowOrderId>(payload, u"order_id", u"payload");
    const auto disposition = decodeOrderDisposition(payload, u"disposition", u"payload");
    const auto digest = readDigest(payload, u"document_sha256", u"payload");
    const auto extension =
        decodeOptionalId<model::WorkflowDeadlineId>(payload, u"extension_deadline_id", u"payload");
    if (!header) {
        return std::unexpected(header.error());
    }
    if (!operation_id) {
        return std::unexpected(operation_id.error());
    }
    if (!order_id) {
        return std::unexpected(order_id.error());
    }
    if (!disposition) {
        return std::unexpected(disposition.error());
    }
    if (!digest) {
        return std::unexpected(digest.error());
    }
    if (!extension) {
        return std::unexpected(extension.error());
    }
    return model::EnterWorkflowOrder{*header,      *operation_id, *order_id,
                                     *disposition, *digest,       *extension};
}

[[nodiscard]] auto decodeSetSealed(const QJsonObject& payload)
    -> std::expected<model::WorkflowCommand, WorkflowCodecError> {
    if (const auto keys = exactKeys(
            payload,
            {u"actor_id", u"command_id", u"occurred_at", u"operation_id", u"sealed", u"session_id"},
            u"payload");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto header = decodeCommandHeader(payload);
    const auto operation_id =
        decodeId<model::WorkflowOperationId>(payload, u"operation_id", u"payload");
    const auto sealed = payload.value(u"sealed");
    if (!header) {
        return std::unexpected(header.error());
    }
    if (!operation_id) {
        return std::unexpected(operation_id.error());
    }
    if (!sealed.isBool()) {
        return fail(WorkflowCodecErrorCode::InvalidField,
                    QStringLiteral("payload.sealed must be a boolean"));
    }
    return model::SetWorkflowSealed{*header, *operation_id, sealed.toBool()};
}

[[nodiscard]] auto decodeScheduleArgument(const QJsonObject& payload)
    -> std::expected<model::WorkflowCommand, WorkflowCodecError> {
    if (const auto keys = exactKeys(payload,
                                    {u"actor_id", u"argument_date", u"command_id", u"occurred_at",
                                     u"operation_id", u"session_id"},
                                    u"payload");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto header = decodeCommandHeader(payload);
    const auto operation_id =
        decodeId<model::WorkflowOperationId>(payload, u"operation_id", u"payload");
    const auto argument_date = parseDate(payload, u"argument_date", u"payload");
    if (!header) {
        return std::unexpected(header.error());
    }
    if (!operation_id) {
        return std::unexpected(operation_id.error());
    }
    if (!argument_date) {
        return std::unexpected(argument_date.error());
    }
    return model::ScheduleWorkflowArgument{*header, *operation_id, *argument_date};
}

[[nodiscard]] auto decodeIssueJudgment(const QJsonObject& payload)
    -> std::expected<model::WorkflowCommand, WorkflowCodecError> {
    if (const auto keys = exactKeys(payload,
                                    {u"actor_id", u"command_id", u"disposition", u"document_sha256",
                                     u"occurred_at", u"operation_id", u"session_id"},
                                    u"payload");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto header = decodeCommandHeader(payload);
    const auto operation_id =
        decodeId<model::WorkflowOperationId>(payload, u"operation_id", u"payload");
    const auto digest = readDigest(payload, u"document_sha256", u"payload");
    const auto disposition =
        readString(payload, u"disposition", maximum_text_characters, u"payload");
    if (!header) {
        return std::unexpected(header.error());
    }
    if (!operation_id) {
        return std::unexpected(operation_id.error());
    }
    if (!digest) {
        return std::unexpected(digest.error());
    }
    if (!disposition) {
        return std::unexpected(disposition.error());
    }
    return model::IssueWorkflowJudgment{*header, *operation_id, *digest,
                                        disposition->toUtf8().toStdString()};
}

[[nodiscard]] auto decodeIssueMandate(const QJsonObject& payload)
    -> std::expected<model::WorkflowCommand, WorkflowCodecError> {
    if (const auto keys = exactKeys(payload,
                                    {u"actor_id", u"command_id", u"document_sha256", u"occurred_at",
                                     u"operation_id", u"session_id"},
                                    u"payload");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto header = decodeCommandHeader(payload);
    const auto operation_id =
        decodeId<model::WorkflowOperationId>(payload, u"operation_id", u"payload");
    const auto digest = readDigest(payload, u"document_sha256", u"payload");
    if (!header) {
        return std::unexpected(header.error());
    }
    if (!operation_id) {
        return std::unexpected(operation_id.error());
    }
    if (!digest) {
        return std::unexpected(digest.error());
    }
    return model::IssueWorkflowMandate{*header, *operation_id, *digest};
}

[[nodiscard]] auto encodeEventHeader(const model::WorkflowEventHeader& header)
    -> std::expected<QJsonObject, WorkflowCodecError> {
    const auto session_id = checkedId(header.session_id, u"payload.session_id");
    const auto workflow_id = checkedId(header.workflow_id.value, u"payload.workflow_id");
    const auto command_id = checkedId(header.command_id.value, u"payload.command_id");
    const auto operation_id = checkedId(header.operation_id.value, u"payload.operation_id");
    const auto occurred_at = encodeTime(header.occurred_at);
    const auto authority = encodeAuthority(header.authority);
    if (!session_id) {
        return std::unexpected(session_id.error());
    }
    if (!workflow_id) {
        return std::unexpected(workflow_id.error());
    }
    if (!command_id) {
        return std::unexpected(command_id.error());
    }
    if (!operation_id) {
        return std::unexpected(operation_id.error());
    }
    if (header.sequence == 0 || header.sequence == std::numeric_limits<std::uint64_t>::max()) {
        return fail(WorkflowCodecErrorCode::OutOfRange,
                    QStringLiteral("payload.sequence must permit a following sequence"));
    }
    if (header.command_event_count == 0 ||
        header.command_event_count > maximum_events_per_command ||
        header.command_event_index >= header.command_event_count) {
        return fail(WorkflowCodecErrorCode::OutOfRange,
                    QStringLiteral("Invalid workflow command event grouping"));
    }
    if (!occurred_at) {
        return std::unexpected(occurred_at.error());
    }
    if (!authority) {
        return std::unexpected(authority.error());
    }
    return QJsonObject{
        {QStringLiteral("authority"), *authority},
        {QStringLiteral("command_event_count"),
         QString::fromStdString(std::to_string(header.command_event_count))},
        {QStringLiteral("command_event_index"),
         QString::fromStdString(std::to_string(header.command_event_index))},
        {QStringLiteral("command_id"), *command_id},
        {QStringLiteral("occurred_at"), *occurred_at},
        {QStringLiteral("operation_id"), *operation_id},
        {QStringLiteral("sequence"), QString::fromStdString(std::to_string(header.sequence))},
        {QStringLiteral("session_id"), *session_id},
        {QStringLiteral("workflow_id"), *workflow_id},
    };
}

[[nodiscard]] auto decodeEventHeader(const QJsonObject& payload)
    -> std::expected<model::WorkflowEventHeader, WorkflowCodecError> {
    const auto session_id = readId(payload, u"session_id", u"payload");
    const auto workflow_id = decodeId<model::WorkflowId>(payload, u"workflow_id", u"payload");
    const auto command_id = decodeId<model::WorkflowCommandId>(payload, u"command_id", u"payload");
    const auto operation_id =
        decodeId<model::WorkflowOperationId>(payload, u"operation_id", u"payload");
    const auto sequence = parseInteger<std::uint64_t>(payload, u"sequence", u"payload", false);
    const auto index =
        parseInteger<std::uint32_t>(payload, u"command_event_index", u"payload", false);
    const auto count =
        parseInteger<std::uint32_t>(payload, u"command_event_count", u"payload", false);
    const auto occurred_value = payload.value(u"occurred_at");
    const auto authority_value = payload.value(u"authority");
    if (!session_id) {
        return std::unexpected(session_id.error());
    }
    if (!workflow_id) {
        return std::unexpected(workflow_id.error());
    }
    if (!command_id) {
        return std::unexpected(command_id.error());
    }
    if (!operation_id) {
        return std::unexpected(operation_id.error());
    }
    if (!sequence) {
        return std::unexpected(sequence.error());
    }
    if (!index) {
        return std::unexpected(index.error());
    }
    if (!count) {
        return std::unexpected(count.error());
    }
    if (*sequence == 0 || *sequence == std::numeric_limits<std::uint64_t>::max() || *count == 0 ||
        *count > maximum_events_per_command || *index >= *count) {
        return fail(WorkflowCodecErrorCode::OutOfRange,
                    QStringLiteral("Invalid workflow command event grouping or sequence"));
    }
    if (!occurred_value.isObject()) {
        return fail(WorkflowCodecErrorCode::InvalidField,
                    QStringLiteral("payload.occurred_at must be an object"));
    }
    if (!authority_value.isObject()) {
        return fail(WorkflowCodecErrorCode::IncompleteAuthority,
                    QStringLiteral("payload.authority must be an object"));
    }
    const auto occurred_at = decodeTime(occurred_value.toObject(), u"payload.occurred_at");
    const auto authority = decodeAuthority(authority_value.toObject(), u"payload.authority");
    if (!occurred_at) {
        return std::unexpected(occurred_at.error());
    }
    if (!authority) {
        return std::unexpected(authority.error());
    }
    return model::WorkflowEventHeader{*session_id,   *workflow_id, *command_id,
                                      *operation_id, *sequence,    *index,
                                      *count,        *occurred_at, *authority};
}

[[nodiscard]] auto encodeRejectionReason(model::WorkflowFilingRejectionReason reason)
    -> std::expected<QString, WorkflowCodecError> {
    switch (reason) {
    case model::WorkflowFilingRejectionReason::UnauthorizedActor:
        return QStringLiteral("unauthorized_actor");
    case model::WorkflowFilingRejectionReason::IneligibleFiling:
        return QStringLiteral("ineligible_filing");
    case model::WorkflowFilingRejectionReason::NonconformingFiling:
        return QStringLiteral("nonconforming_filing");
    case model::WorkflowFilingRejectionReason::DeadlineExpired:
        return QStringLiteral("deadline_expired");
    case model::WorkflowFilingRejectionReason::UnknownDeficiency:
        return QStringLiteral("unknown_deficiency");
    }
    return fail(WorkflowCodecErrorCode::InvalidField,
                QStringLiteral("Unknown workflow filing rejection reason"));
}

[[nodiscard]] auto decodeRejectionReason(const QJsonObject& object)
    -> std::expected<model::WorkflowFilingRejectionReason, WorkflowCodecError> {
    const auto value = readString(object, u"reason", 32, u"payload");
    if (!value) {
        return std::unexpected(value.error());
    }
    if (*value == u"unauthorized_actor") {
        return model::WorkflowFilingRejectionReason::UnauthorizedActor;
    }
    if (*value == u"ineligible_filing") {
        return model::WorkflowFilingRejectionReason::IneligibleFiling;
    }
    if (*value == u"nonconforming_filing") {
        return model::WorkflowFilingRejectionReason::NonconformingFiling;
    }
    if (*value == u"deadline_expired") {
        return model::WorkflowFilingRejectionReason::DeadlineExpired;
    }
    if (*value == u"unknown_deficiency") {
        return model::WorkflowFilingRejectionReason::UnknownDeficiency;
    }
    return fail(WorkflowCodecErrorCode::InvalidField, QStringLiteral("payload.reason is unknown"));
}

[[nodiscard]] auto encodeDeadlinePurpose(model::WorkflowDeadlinePurpose purpose)
    -> std::expected<QString, WorkflowCodecError> {
    switch (purpose) {
    case model::WorkflowDeadlinePurpose::Filing:
        return QStringLiteral("filing");
    case model::WorkflowDeadlinePurpose::DeficiencyCure:
        return QStringLiteral("deficiency_cure");
    }
    return fail(WorkflowCodecErrorCode::InvalidField,
                QStringLiteral("Unknown workflow deadline purpose"));
}

[[nodiscard]] auto decodeDeadlinePurpose(const QJsonObject& object)
    -> std::expected<model::WorkflowDeadlinePurpose, WorkflowCodecError> {
    const auto value = readString(object, u"purpose", 32, u"payload");
    if (!value) {
        return std::unexpected(value.error());
    }
    if (*value == u"filing") {
        return model::WorkflowDeadlinePurpose::Filing;
    }
    if (*value == u"deficiency_cure") {
        return model::WorkflowDeadlinePurpose::DeficiencyCure;
    }
    return fail(WorkflowCodecErrorCode::InvalidField, QStringLiteral("payload.purpose is unknown"));
}

[[nodiscard]] auto encodeDeadlineExtension(const model::WorkflowDeadlineExtension& extension)
    -> std::expected<QJsonObject, WorkflowCodecError> {
    const auto deadline_id =
        checkedId(extension.deadline_id.value, u"payload.extension.deadline_id");
    const auto previous = formatDate(extension.previous_due_date);
    const auto extended = formatDate(extension.extended_due_date);
    if (!deadline_id) {
        return std::unexpected(deadline_id.error());
    }
    if (!previous) {
        return std::unexpected(previous.error());
    }
    if (!extended) {
        return std::unexpected(extended.error());
    }
    if (std::chrono::sys_days{extension.extended_due_date.value} <=
        std::chrono::sys_days{extension.previous_due_date.value}) {
        return fail(WorkflowCodecErrorCode::InvalidField,
                    QStringLiteral("Deadline extension must move the due date forward"));
    }
    return QJsonObject{
        {QStringLiteral("deadline_id"), *deadline_id},
        {QStringLiteral("extended_due_date"), *extended},
        {QStringLiteral("previous_due_date"), *previous},
    };
}

[[nodiscard]] auto decodeDeadlineExtension(const QJsonObject& object)
    -> std::expected<model::WorkflowDeadlineExtension, WorkflowCodecError> {
    if (const auto keys =
            exactKeys(object, {u"deadline_id", u"extended_due_date", u"previous_due_date"},
                      u"payload.extension");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto deadline_id =
        decodeId<model::WorkflowDeadlineId>(object, u"deadline_id", u"payload.extension");
    const auto previous = parseDate(object, u"previous_due_date", u"payload.extension");
    const auto extended = parseDate(object, u"extended_due_date", u"payload.extension");
    if (!deadline_id) {
        return std::unexpected(deadline_id.error());
    }
    if (!previous) {
        return std::unexpected(previous.error());
    }
    if (!extended) {
        return std::unexpected(extended.error());
    }
    if (std::chrono::sys_days{extended->value} <= std::chrono::sys_days{previous->value}) {
        return fail(WorkflowCodecErrorCode::InvalidField,
                    QStringLiteral("Deadline extension must move the due date forward"));
    }
    return model::WorkflowDeadlineExtension{*deadline_id, *previous, *extended};
}

[[nodiscard]] auto encodeEventPayload(const model::WorkflowFilingAccepted& event)
    -> std::expected<QJsonObject, WorkflowCodecError> {
    auto payload = encodeEventHeader(event.header);
    const auto filing_id = checkedId(event.filing_id.value, u"payload.filing_id");
    const auto filing_type = checkedId(event.filing_type.value, u"payload.filing_type");
    const auto actor_id = checkedId(event.actor_id.value, u"payload.actor_id");
    const auto digest = checkedDigest(event.document_sha256, u"payload.document_sha256");
    const auto served =
        encodeIdArray(event.served_actors, maximum_served_actors, false, u"payload.served_actors");
    const auto cured = encodeOptionalId(event.cured_deficiency_id, u"payload.cured_deficiency_id");
    const auto satisfied =
        encodeOptionalId(event.satisfied_deadline_id, u"payload.satisfied_deadline_id");
    if (!payload) {
        return std::unexpected(payload.error());
    }
    if (!filing_id) {
        return std::unexpected(filing_id.error());
    }
    if (!filing_type) {
        return std::unexpected(filing_type.error());
    }
    if (!actor_id) {
        return std::unexpected(actor_id.error());
    }
    if (!digest) {
        return std::unexpected(digest.error());
    }
    if (!served) {
        return std::unexpected(served.error());
    }
    if (!cured) {
        return std::unexpected(cured.error());
    }
    if (!satisfied) {
        return std::unexpected(satisfied.error());
    }
    payload->insert(QStringLiteral("actor_id"), *actor_id);
    payload->insert(QStringLiteral("cured_deficiency_id"), *cured);
    payload->insert(QStringLiteral("document_sha256"), *digest);
    payload->insert(QStringLiteral("filing_id"), *filing_id);
    payload->insert(QStringLiteral("filing_type"), *filing_type);
    payload->insert(QStringLiteral("satisfied_deadline_id"), *satisfied);
    payload->insert(QStringLiteral("served_actors"), *served);
    return payload;
}

[[nodiscard]] auto encodeEventPayload(const model::WorkflowFilingRejected& event)
    -> std::expected<QJsonObject, WorkflowCodecError> {
    auto payload = encodeEventHeader(event.header);
    const auto filing_id = checkedId(event.filing_id.value, u"payload.filing_id");
    const auto filing_type = checkedId(event.filing_type.value, u"payload.filing_type");
    const auto actor_id = checkedId(event.actor_id.value, u"payload.actor_id");
    const auto reason = encodeRejectionReason(event.reason);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    if (!filing_id) {
        return std::unexpected(filing_id.error());
    }
    if (!filing_type) {
        return std::unexpected(filing_type.error());
    }
    if (!actor_id) {
        return std::unexpected(actor_id.error());
    }
    if (!reason) {
        return std::unexpected(reason.error());
    }
    payload->insert(QStringLiteral("actor_id"), *actor_id);
    payload->insert(QStringLiteral("filing_id"), *filing_id);
    payload->insert(QStringLiteral("filing_type"), *filing_type);
    payload->insert(QStringLiteral("reason"), *reason);
    return payload;
}

[[nodiscard]] auto encodeEventPayload(const model::WorkflowDeficiencyIssued& event)
    -> std::expected<QJsonObject, WorkflowCodecError> {
    auto payload = encodeEventHeader(event.header);
    const auto deficiency_id = checkedId(event.deficiency_id.value, u"payload.deficiency_id");
    const auto filing_id = checkedId(event.filing_id.value, u"payload.filing_id");
    const auto filing_type = checkedId(event.filing_type.value, u"payload.filing_type");
    const auto actor_id = checkedId(event.actor_id.value, u"payload.actor_id");
    const auto requirements = encodeIdArray(event.missing_requirements, maximum_requirements, true,
                                            u"payload.missing_requirements");
    const auto deadline = encodeOptionalId(event.cure_deadline_id, u"payload.cure_deadline_id");
    if (!payload) {
        return std::unexpected(payload.error());
    }
    if (!deficiency_id) {
        return std::unexpected(deficiency_id.error());
    }
    if (!filing_id) {
        return std::unexpected(filing_id.error());
    }
    if (!filing_type) {
        return std::unexpected(filing_type.error());
    }
    if (!actor_id) {
        return std::unexpected(actor_id.error());
    }
    if (!requirements) {
        return std::unexpected(requirements.error());
    }
    if (!deadline) {
        return std::unexpected(deadline.error());
    }
    payload->insert(QStringLiteral("actor_id"), *actor_id);
    payload->insert(QStringLiteral("cure_deadline_id"), *deadline);
    payload->insert(QStringLiteral("deficiency_id"), *deficiency_id);
    payload->insert(QStringLiteral("filing_id"), *filing_id);
    payload->insert(QStringLiteral("filing_type"), *filing_type);
    payload->insert(QStringLiteral("missing_requirements"), *requirements);
    return payload;
}

[[nodiscard]] auto encodeEventPayload(const model::WorkflowDeadlineCalculated& event)
    -> std::expected<QJsonObject, WorkflowCodecError> {
    auto payload = encodeEventHeader(event.header);
    const auto deadline_id = checkedId(event.deadline_id.value, u"payload.deadline_id");
    const auto purpose = encodeDeadlinePurpose(event.purpose);
    const auto base_date = formatDate(event.base_date);
    const auto due_date = formatDate(event.due_date);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    if (!deadline_id) {
        return std::unexpected(deadline_id.error());
    }
    if (!purpose) {
        return std::unexpected(purpose.error());
    }
    if (!base_date) {
        return std::unexpected(base_date.error());
    }
    if (!due_date) {
        return std::unexpected(due_date.error());
    }
    payload->insert(QStringLiteral("base_date"), *base_date);
    payload->insert(QStringLiteral("deadline_id"), *deadline_id);
    payload->insert(QStringLiteral("due_date"), *due_date);
    payload->insert(QStringLiteral("purpose"), *purpose);
    return payload;
}

[[nodiscard]] auto encodeEventPayload(const model::WorkflowOrderEntered& event)
    -> std::expected<QJsonObject, WorkflowCodecError> {
    auto payload = encodeEventHeader(event.header);
    const auto order_id = checkedId(event.order_id.value, u"payload.order_id");
    const auto disposition = encodeOrderDisposition(event.disposition);
    const auto digest = checkedDigest(event.document_sha256, u"payload.document_sha256");
    if (!payload) {
        return std::unexpected(payload.error());
    }
    if (!order_id) {
        return std::unexpected(order_id.error());
    }
    if (!disposition) {
        return std::unexpected(disposition.error());
    }
    if (!digest) {
        return std::unexpected(digest.error());
    }
    QJsonValue extension{QJsonValue::Null};
    if (event.extension) {
        const auto encoded = encodeDeadlineExtension(*event.extension);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        extension = *encoded;
    }
    payload->insert(QStringLiteral("disposition"), *disposition);
    payload->insert(QStringLiteral("document_sha256"), *digest);
    payload->insert(QStringLiteral("extension"), extension);
    payload->insert(QStringLiteral("order_id"), *order_id);
    return payload;
}

[[nodiscard]] auto encodeEventPayload(const model::WorkflowStageAdvanced& event)
    -> std::expected<QJsonObject, WorkflowCodecError> {
    auto payload = encodeEventHeader(event.header);
    const auto previous = checkedId(event.previous_stage_id.value, u"payload.previous_stage_id");
    const auto next = checkedId(event.next_stage_id.value, u"payload.next_stage_id");
    if (!payload) {
        return std::unexpected(payload.error());
    }
    if (!previous) {
        return std::unexpected(previous.error());
    }
    if (!next) {
        return std::unexpected(next.error());
    }
    payload->insert(QStringLiteral("next_stage_id"), *next);
    payload->insert(QStringLiteral("previous_stage_id"), *previous);
    return payload;
}

[[nodiscard]] auto encodeEventPayload(const model::WorkflowSealedSet& event)
    -> std::expected<QJsonObject, WorkflowCodecError> {
    auto payload = encodeEventHeader(event.header);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    payload->insert(QStringLiteral("sealed"), event.sealed);
    return payload;
}

[[nodiscard]] auto encodeEventPayload(const model::WorkflowArgumentScheduled& event)
    -> std::expected<QJsonObject, WorkflowCodecError> {
    auto payload = encodeEventHeader(event.header);
    const auto argument_date = formatDate(event.argument_date);
    const auto next = encodeOptionalId(event.next_stage_id, u"payload.next_stage_id");
    if (!payload) {
        return std::unexpected(payload.error());
    }
    if (!argument_date) {
        return std::unexpected(argument_date.error());
    }
    if (!next) {
        return std::unexpected(next.error());
    }
    payload->insert(QStringLiteral("argument_date"), *argument_date);
    payload->insert(QStringLiteral("next_stage_id"), *next);
    return payload;
}

[[nodiscard]] auto encodeEventPayload(const model::WorkflowJudgmentIssued& event)
    -> std::expected<QJsonObject, WorkflowCodecError> {
    auto payload = encodeEventHeader(event.header);
    const auto digest = checkedDigest(event.document_sha256, u"payload.document_sha256");
    const auto next = encodeOptionalId(event.next_stage_id, u"payload.next_stage_id");
    if (!payload) {
        return std::unexpected(payload.error());
    }
    if (!digest) {
        return std::unexpected(digest.error());
    }
    if (!next) {
        return std::unexpected(next.error());
    }
    if (!roundTripsUtf8(event.disposition)) {
        return fail(WorkflowCodecErrorCode::InvalidField,
                    QStringLiteral("payload.disposition is not valid UTF-8"));
    }
    const auto disposition = QString::fromUtf8(event.disposition);
    if (disposition.isEmpty() || disposition.toUtf8().size() > maximum_text_characters ||
        containsNull(disposition)) {
        return fail(WorkflowCodecErrorCode::OutOfRange,
                    QStringLiteral("payload.disposition is empty or exceeds its bound"));
    }
    payload->insert(QStringLiteral("disposition"), disposition);
    payload->insert(QStringLiteral("document_sha256"), *digest);
    payload->insert(QStringLiteral("next_stage_id"), *next);
    return payload;
}

[[nodiscard]] auto encodeEventPayload(const model::WorkflowMandateIssued& event)
    -> std::expected<QJsonObject, WorkflowCodecError> {
    auto payload = encodeEventHeader(event.header);
    const auto digest = checkedDigest(event.document_sha256, u"payload.document_sha256");
    const auto next = encodeOptionalId(event.next_stage_id, u"payload.next_stage_id");
    if (!payload) {
        return std::unexpected(payload.error());
    }
    if (!digest) {
        return std::unexpected(digest.error());
    }
    if (!next) {
        return std::unexpected(next.error());
    }
    payload->insert(QStringLiteral("document_sha256"), *digest);
    payload->insert(QStringLiteral("next_stage_id"), *next);
    return payload;
}

[[nodiscard]] auto decodeFilingAccepted(const QJsonObject& payload)
    -> std::expected<model::WorkflowEvent, WorkflowCodecError> {
    if (const auto keys =
            exactKeys(payload,
                      {u"actor_id", u"authority", u"command_event_count", u"command_event_index",
                       u"command_id", u"cured_deficiency_id", u"document_sha256", u"filing_id",
                       u"filing_type", u"occurred_at", u"operation_id", u"satisfied_deadline_id",
                       u"sequence", u"served_actors", u"session_id", u"workflow_id"},
                      u"payload");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto header = decodeEventHeader(payload);
    const auto filing_id = decodeId<model::WorkflowFilingId>(payload, u"filing_id", u"payload");
    const auto filing_type = decodeId<model::FilingTypeId>(payload, u"filing_type", u"payload");
    const auto actor_id = decodeId<model::ActorId>(payload, u"actor_id", u"payload");
    const auto digest = readDigest(payload, u"document_sha256", u"payload");
    const auto served = decodeIdArray<model::ActorId>(
        payload.value(u"served_actors"), maximum_served_actors, false, u"payload.served_actors");
    const auto cured =
        decodeOptionalId<model::WorkflowDeficiencyId>(payload, u"cured_deficiency_id", u"payload");
    const auto satisfied =
        decodeOptionalId<model::WorkflowDeadlineId>(payload, u"satisfied_deadline_id", u"payload");
    if (!header) {
        return std::unexpected(header.error());
    }
    if (!filing_id) {
        return std::unexpected(filing_id.error());
    }
    if (!filing_type) {
        return std::unexpected(filing_type.error());
    }
    if (!actor_id) {
        return std::unexpected(actor_id.error());
    }
    if (!digest) {
        return std::unexpected(digest.error());
    }
    if (!served) {
        return std::unexpected(served.error());
    }
    if (!cured) {
        return std::unexpected(cured.error());
    }
    if (!satisfied) {
        return std::unexpected(satisfied.error());
    }
    if (std::ranges::find(*served, *actor_id) != served->end()) {
        return fail(WorkflowCodecErrorCode::InvalidField,
                    QStringLiteral("Filer cannot appear in served_actors"));
    }
    return model::WorkflowFilingAccepted{*header, *filing_id,         *filing_type, *actor_id,
                                         *digest, std::move(*served), *cured,       *satisfied};
}

[[nodiscard]] auto decodeFilingRejected(const QJsonObject& payload)
    -> std::expected<model::WorkflowEvent, WorkflowCodecError> {
    if (const auto keys =
            exactKeys(payload,
                      {u"actor_id", u"authority", u"command_event_count", u"command_event_index",
                       u"command_id", u"filing_id", u"filing_type", u"occurred_at", u"operation_id",
                       u"reason", u"sequence", u"session_id", u"workflow_id"},
                      u"payload");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto header = decodeEventHeader(payload);
    const auto filing_id = decodeId<model::WorkflowFilingId>(payload, u"filing_id", u"payload");
    const auto filing_type = decodeId<model::FilingTypeId>(payload, u"filing_type", u"payload");
    const auto actor_id = decodeId<model::ActorId>(payload, u"actor_id", u"payload");
    const auto reason = decodeRejectionReason(payload);
    if (!header) {
        return std::unexpected(header.error());
    }
    if (!filing_id) {
        return std::unexpected(filing_id.error());
    }
    if (!filing_type) {
        return std::unexpected(filing_type.error());
    }
    if (!actor_id) {
        return std::unexpected(actor_id.error());
    }
    if (!reason) {
        return std::unexpected(reason.error());
    }
    return model::WorkflowFilingRejected{*header, *filing_id, *filing_type, *actor_id, *reason};
}

[[nodiscard]] auto decodeDeficiencyIssued(const QJsonObject& payload)
    -> std::expected<model::WorkflowEvent, WorkflowCodecError> {
    if (const auto keys =
            exactKeys(payload,
                      {u"actor_id", u"authority", u"command_event_count", u"command_event_index",
                       u"command_id", u"cure_deadline_id", u"deficiency_id", u"filing_id",
                       u"filing_type", u"missing_requirements", u"occurred_at", u"operation_id",
                       u"sequence", u"session_id", u"workflow_id"},
                      u"payload");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto header = decodeEventHeader(payload);
    const auto deficiency_id =
        decodeId<model::WorkflowDeficiencyId>(payload, u"deficiency_id", u"payload");
    const auto filing_id = decodeId<model::WorkflowFilingId>(payload, u"filing_id", u"payload");
    const auto filing_type = decodeId<model::FilingTypeId>(payload, u"filing_type", u"payload");
    const auto actor_id = decodeId<model::ActorId>(payload, u"actor_id", u"payload");
    const auto requirements = decodeIdArray<model::WorkflowRequirementId>(
        payload.value(u"missing_requirements"), maximum_requirements, true,
        u"payload.missing_requirements");
    const auto deadline_id =
        decodeOptionalId<model::WorkflowDeadlineId>(payload, u"cure_deadline_id", u"payload");
    if (!header) {
        return std::unexpected(header.error());
    }
    if (!deficiency_id) {
        return std::unexpected(deficiency_id.error());
    }
    if (!filing_id) {
        return std::unexpected(filing_id.error());
    }
    if (!filing_type) {
        return std::unexpected(filing_type.error());
    }
    if (!actor_id) {
        return std::unexpected(actor_id.error());
    }
    if (!requirements) {
        return std::unexpected(requirements.error());
    }
    if (!deadline_id) {
        return std::unexpected(deadline_id.error());
    }
    return model::WorkflowDeficiencyIssued{*header,      *deficiency_id, *filing_id,
                                           *filing_type, *actor_id,      std::move(*requirements),
                                           *deadline_id};
}

[[nodiscard]] auto decodeDeadlineCalculated(const QJsonObject& payload)
    -> std::expected<model::WorkflowEvent, WorkflowCodecError> {
    if (const auto keys =
            exactKeys(payload,
                      {u"authority", u"base_date", u"command_event_count", u"command_event_index",
                       u"command_id", u"deadline_id", u"due_date", u"occurred_at", u"operation_id",
                       u"purpose", u"sequence", u"session_id", u"workflow_id"},
                      u"payload");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto header = decodeEventHeader(payload);
    const auto deadline_id =
        decodeId<model::WorkflowDeadlineId>(payload, u"deadline_id", u"payload");
    const auto purpose = decodeDeadlinePurpose(payload);
    const auto base_date = parseDate(payload, u"base_date", u"payload");
    const auto due_date = parseDate(payload, u"due_date", u"payload");
    if (!header) {
        return std::unexpected(header.error());
    }
    if (!deadline_id) {
        return std::unexpected(deadline_id.error());
    }
    if (!purpose) {
        return std::unexpected(purpose.error());
    }
    if (!base_date) {
        return std::unexpected(base_date.error());
    }
    if (!due_date) {
        return std::unexpected(due_date.error());
    }
    return model::WorkflowDeadlineCalculated{*header, *deadline_id, *purpose, *base_date,
                                             *due_date};
}

[[nodiscard]] auto decodeOrderEntered(const QJsonObject& payload)
    -> std::expected<model::WorkflowEvent, WorkflowCodecError> {
    if (const auto keys =
            exactKeys(payload,
                      {u"authority", u"command_event_count", u"command_event_index", u"command_id",
                       u"disposition", u"document_sha256", u"extension", u"occurred_at",
                       u"operation_id", u"order_id", u"sequence", u"session_id", u"workflow_id"},
                      u"payload");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto header = decodeEventHeader(payload);
    const auto order_id = decodeId<model::WorkflowOrderId>(payload, u"order_id", u"payload");
    const auto disposition = decodeOrderDisposition(payload, u"disposition", u"payload");
    const auto digest = readDigest(payload, u"document_sha256", u"payload");
    if (!header) {
        return std::unexpected(header.error());
    }
    if (!order_id) {
        return std::unexpected(order_id.error());
    }
    if (!disposition) {
        return std::unexpected(disposition.error());
    }
    if (!digest) {
        return std::unexpected(digest.error());
    }
    std::optional<model::WorkflowDeadlineExtension> extension;
    const auto extension_value = payload.value(u"extension");
    if (!extension_value.isNull()) {
        if (!extension_value.isObject()) {
            return fail(WorkflowCodecErrorCode::InvalidField,
                        QStringLiteral("payload.extension must be null or an object"));
        }
        auto decoded = decodeDeadlineExtension(extension_value.toObject());
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        extension = std::move(*decoded);
    }
    return model::WorkflowOrderEntered{*header, *order_id, *disposition, *digest, extension};
}

[[nodiscard]] auto decodeStageAdvanced(const QJsonObject& payload)
    -> std::expected<model::WorkflowEvent, WorkflowCodecError> {
    if (const auto keys =
            exactKeys(payload,
                      {u"authority", u"command_event_count", u"command_event_index", u"command_id",
                       u"next_stage_id", u"occurred_at", u"operation_id", u"previous_stage_id",
                       u"sequence", u"session_id", u"workflow_id"},
                      u"payload");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto header = decodeEventHeader(payload);
    const auto previous =
        decodeId<model::WorkflowStageId>(payload, u"previous_stage_id", u"payload");
    const auto next = decodeId<model::WorkflowStageId>(payload, u"next_stage_id", u"payload");
    if (!header) {
        return std::unexpected(header.error());
    }
    if (!previous) {
        return std::unexpected(previous.error());
    }
    if (!next) {
        return std::unexpected(next.error());
    }
    return model::WorkflowStageAdvanced{*header, *previous, *next};
}

[[nodiscard]] auto decodeSealedSet(const QJsonObject& payload)
    -> std::expected<model::WorkflowEvent, WorkflowCodecError> {
    if (const auto keys = exactKeys(payload,
                                    {u"authority", u"command_event_count", u"command_event_index",
                                     u"command_id", u"occurred_at", u"operation_id", u"sealed",
                                     u"sequence", u"session_id", u"workflow_id"},
                                    u"payload");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto header = decodeEventHeader(payload);
    const auto sealed = payload.value(u"sealed");
    if (!header) {
        return std::unexpected(header.error());
    }
    if (!sealed.isBool()) {
        return fail(WorkflowCodecErrorCode::InvalidField,
                    QStringLiteral("payload.sealed must be a boolean"));
    }
    return model::WorkflowSealedSet{*header, sealed.toBool()};
}

[[nodiscard]] auto decodeArgumentScheduled(const QJsonObject& payload)
    -> std::expected<model::WorkflowEvent, WorkflowCodecError> {
    if (const auto keys =
            exactKeys(payload,
                      {u"argument_date", u"authority", u"command_event_count",
                       u"command_event_index", u"command_id", u"next_stage_id", u"occurred_at",
                       u"operation_id", u"sequence", u"session_id", u"workflow_id"},
                      u"payload");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto header = decodeEventHeader(payload);
    const auto argument_date = parseDate(payload, u"argument_date", u"payload");
    const auto next =
        decodeOptionalId<model::WorkflowStageId>(payload, u"next_stage_id", u"payload");
    if (!header) {
        return std::unexpected(header.error());
    }
    if (!argument_date) {
        return std::unexpected(argument_date.error());
    }
    if (!next) {
        return std::unexpected(next.error());
    }
    return model::WorkflowArgumentScheduled{*header, *argument_date, *next};
}

[[nodiscard]] auto decodeJudgmentIssued(const QJsonObject& payload)
    -> std::expected<model::WorkflowEvent, WorkflowCodecError> {
    if (const auto keys =
            exactKeys(payload,
                      {u"authority", u"command_event_count", u"command_event_index", u"command_id",
                       u"disposition", u"document_sha256", u"next_stage_id", u"occurred_at",
                       u"operation_id", u"sequence", u"session_id", u"workflow_id"},
                      u"payload");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto header = decodeEventHeader(payload);
    const auto digest = readDigest(payload, u"document_sha256", u"payload");
    const auto disposition =
        readString(payload, u"disposition", maximum_text_characters, u"payload");
    const auto next =
        decodeOptionalId<model::WorkflowStageId>(payload, u"next_stage_id", u"payload");
    if (!header) {
        return std::unexpected(header.error());
    }
    if (!digest) {
        return std::unexpected(digest.error());
    }
    if (!disposition) {
        return std::unexpected(disposition.error());
    }
    if (!next) {
        return std::unexpected(next.error());
    }
    return model::WorkflowJudgmentIssued{*header, *digest, disposition->toUtf8().toStdString(),
                                         *next};
}

[[nodiscard]] auto decodeMandateIssued(const QJsonObject& payload)
    -> std::expected<model::WorkflowEvent, WorkflowCodecError> {
    if (const auto keys =
            exactKeys(payload,
                      {u"authority", u"command_event_count", u"command_event_index", u"command_id",
                       u"document_sha256", u"next_stage_id", u"occurred_at", u"operation_id",
                       u"sequence", u"session_id", u"workflow_id"},
                      u"payload");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto header = decodeEventHeader(payload);
    const auto digest = readDigest(payload, u"document_sha256", u"payload");
    const auto next =
        decodeOptionalId<model::WorkflowStageId>(payload, u"next_stage_id", u"payload");
    if (!header) {
        return std::unexpected(header.error());
    }
    if (!digest) {
        return std::unexpected(digest.error());
    }
    if (!next) {
        return std::unexpected(next.error());
    }
    return model::WorkflowMandateIssued{*header, *digest, *next};
}

[[nodiscard]] auto decodeEnvelope(QByteArrayView encoded, QStringView type_key,
                                  QStringView envelope_context)
    -> std::expected<std::pair<QString, QJsonObject>, WorkflowCodecError> {
    if (encoded.isEmpty() || encoded.size() > maximum_payload_bytes) {
        return fail(WorkflowCodecErrorCode::PayloadTooLarge,
                    QStringLiteral("Workflow payload is empty or exceeds the size limit"));
    }
    if (const auto duplicate_check = rejectDuplicateMembers(encoded); !duplicate_check) {
        return std::unexpected(duplicate_check.error());
    }
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(encoded.toByteArray(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        return fail(WorkflowCodecErrorCode::InvalidJson,
                    QStringLiteral("Invalid workflow JSON: %1").arg(parse_error.errorString()));
    }
    const auto envelope = document.object();
    if (const auto keys =
            exactKeys(envelope, {type_key, u"payload", u"schema_version"}, envelope_context);
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto version = envelope.value(u"schema_version");
    if (!version.isDouble() || version.toDouble() != schema_version) {
        return fail(WorkflowCodecErrorCode::UnsupportedVersion,
                    QStringLiteral("Unsupported workflow schema version"));
    }
    const auto type = readString(envelope, type_key, maximum_type_characters, envelope_context);
    if (!type) {
        return std::unexpected(type.error());
    }
    if (!envelope.value(u"payload").isObject()) {
        return fail(WorkflowCodecErrorCode::InvalidField,
                    QStringLiteral("%1.payload must be an object").arg(envelope_context));
    }
    return std::pair{*type, envelope.value(u"payload").toObject()};
}

[[nodiscard]] auto encodeEnvelope(QString type, const QJsonObject& payload, QStringView type_key)
    -> std::expected<QByteArray, WorkflowCodecError> {
    const QJsonObject envelope{
        {type_key.toString(), std::move(type)},
        {QStringLiteral("payload"), payload},
        {QStringLiteral("schema_version"), schema_version},
    };
    const auto result = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    if (result.size() > maximum_payload_bytes) {
        return fail(WorkflowCodecErrorCode::PayloadTooLarge,
                    QStringLiteral("Encoded workflow payload exceeds the size limit"));
    }
    return result;
}

} // namespace

std::expected<QByteArray, WorkflowCodecError>
encodeWorkflowCommand(const model::WorkflowCommand& command) {
    const auto payload = std::visit(
        [](const auto& concrete) -> std::expected<QJsonObject, WorkflowCodecError> {
            return encodeCommandPayload(concrete);
        },
        command);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    return encodeEnvelope(workflowCommandType(command), *payload, u"command_type");
}

std::expected<model::WorkflowCommand, WorkflowCodecError>
decodeWorkflowCommand(QByteArrayView encoded) {
    const auto envelope = decodeEnvelope(encoded, u"command_type", u"command");
    if (!envelope) {
        return std::unexpected(envelope.error());
    }
    const auto& [type, payload] = *envelope;
    if (type == QLatin1StringView(submit_filing_type)) {
        return decodeSubmitFiling(payload);
    }
    if (type == QLatin1StringView(enter_order_type)) {
        return decodeEnterOrder(payload);
    }
    if (type == QLatin1StringView(set_sealed_type)) {
        return decodeSetSealed(payload);
    }
    if (type == QLatin1StringView(schedule_argument_type)) {
        return decodeScheduleArgument(payload);
    }
    if (type == QLatin1StringView(issue_judgment_type)) {
        return decodeIssueJudgment(payload);
    }
    if (type == QLatin1StringView(issue_mandate_type)) {
        return decodeIssueMandate(payload);
    }
    return fail(WorkflowCodecErrorCode::UnknownCommandType,
                QStringLiteral("Unknown workflow command type %1").arg(type));
}

QString workflowCommandType(const model::WorkflowCommand& command) {
    return std::visit(
        [](const auto& concrete) {
            using Command = std::remove_cvref_t<decltype(concrete)>;
            if constexpr (std::same_as<Command, model::SubmitWorkflowFiling>) {
                return QString::fromLatin1(submit_filing_type);
            } else if constexpr (std::same_as<Command, model::EnterWorkflowOrder>) {
                return QString::fromLatin1(enter_order_type);
            } else if constexpr (std::same_as<Command, model::SetWorkflowSealed>) {
                return QString::fromLatin1(set_sealed_type);
            } else if constexpr (std::same_as<Command, model::ScheduleWorkflowArgument>) {
                return QString::fromLatin1(schedule_argument_type);
            } else if constexpr (std::same_as<Command, model::IssueWorkflowJudgment>) {
                return QString::fromLatin1(issue_judgment_type);
            } else {
                return QString::fromLatin1(issue_mandate_type);
            }
        },
        command);
}

std::expected<QByteArray, WorkflowCodecError>
encodeWorkflowEvent(const model::WorkflowEvent& event) {
    const auto payload = std::visit(
        [](const auto& concrete) -> std::expected<QJsonObject, WorkflowCodecError> {
            return encodeEventPayload(concrete);
        },
        event);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    return encodeEnvelope(workflowEventType(event), *payload, u"event_type");
}

std::expected<model::WorkflowEvent, WorkflowCodecError>
decodeWorkflowEvent(QByteArrayView encoded) {
    const auto envelope = decodeEnvelope(encoded, u"event_type", u"event");
    if (!envelope) {
        return std::unexpected(envelope.error());
    }
    const auto& [type, payload] = *envelope;
    if (type == QLatin1StringView(filing_accepted_type)) {
        return decodeFilingAccepted(payload);
    }
    if (type == QLatin1StringView(filing_rejected_type)) {
        return decodeFilingRejected(payload);
    }
    if (type == QLatin1StringView(deficiency_issued_type)) {
        return decodeDeficiencyIssued(payload);
    }
    if (type == QLatin1StringView(deadline_calculated_type)) {
        return decodeDeadlineCalculated(payload);
    }
    if (type == QLatin1StringView(order_entered_type)) {
        return decodeOrderEntered(payload);
    }
    if (type == QLatin1StringView(stage_advanced_type)) {
        return decodeStageAdvanced(payload);
    }
    if (type == QLatin1StringView(sealed_set_type)) {
        return decodeSealedSet(payload);
    }
    if (type == QLatin1StringView(argument_scheduled_type)) {
        return decodeArgumentScheduled(payload);
    }
    if (type == QLatin1StringView(judgment_issued_type)) {
        return decodeJudgmentIssued(payload);
    }
    if (type == QLatin1StringView(mandate_issued_type)) {
        return decodeMandateIssued(payload);
    }
    return fail(WorkflowCodecErrorCode::UnknownEventType,
                QStringLiteral("Unknown workflow event type %1").arg(type));
}

QString workflowEventType(const model::WorkflowEvent& event) {
    return std::visit(
        [](const auto& concrete) {
            using Event = std::remove_cvref_t<decltype(concrete)>;
            if constexpr (std::same_as<Event, model::WorkflowFilingAccepted>) {
                return QString::fromLatin1(filing_accepted_type);
            } else if constexpr (std::same_as<Event, model::WorkflowFilingRejected>) {
                return QString::fromLatin1(filing_rejected_type);
            } else if constexpr (std::same_as<Event, model::WorkflowDeficiencyIssued>) {
                return QString::fromLatin1(deficiency_issued_type);
            } else if constexpr (std::same_as<Event, model::WorkflowDeadlineCalculated>) {
                return QString::fromLatin1(deadline_calculated_type);
            } else if constexpr (std::same_as<Event, model::WorkflowOrderEntered>) {
                return QString::fromLatin1(order_entered_type);
            } else if constexpr (std::same_as<Event, model::WorkflowStageAdvanced>) {
                return QString::fromLatin1(stage_advanced_type);
            } else if constexpr (std::same_as<Event, model::WorkflowSealedSet>) {
                return QString::fromLatin1(sealed_set_type);
            } else if constexpr (std::same_as<Event, model::WorkflowArgumentScheduled>) {
                return QString::fromLatin1(argument_scheduled_type);
            } else if constexpr (std::same_as<Event, model::WorkflowJudgmentIssued>) {
                return QString::fromLatin1(judgment_issued_type);
            } else {
                return QString::fromLatin1(mandate_issued_type);
            }
        },
        event);
}

QString workflowPrimaryAuthorityId(const model::WorkflowEvent& event) {
    return std::visit(
        [](const auto& concrete) {
            return QString::fromUtf8(concrete.header.authority.primary.id.value);
        },
        event);
}

} // namespace appellate::storage
