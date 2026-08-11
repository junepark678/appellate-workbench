#include "appellate/storage/workflow_codec.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSet>
#include <QStringView>

#include <algorithm>
#include <cctype>
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

constexpr auto legacy_schema_version = 1;
constexpr auto provenance_schema_version = 2;
constexpr auto structured_schema_version = 3;
constexpr auto deadline_snapshot_schema_version = 4;
constexpr qsizetype maximum_payload_bytes = 1024 * 1024;
constexpr qsizetype maximum_id_characters = 160;
constexpr qsizetype maximum_text_characters = 4096;
constexpr qsizetype maximum_type_characters = 64;
constexpr qsizetype maximum_fields = 256;
constexpr qsizetype maximum_served_actors = 1024;
constexpr qsizetype maximum_requirements = 256;
constexpr qsizetype maximum_supporting_authorities = 32;
constexpr qsizetype maximum_preconditions = 32;
constexpr qsizetype maximum_disposition_components = 32;
constexpr qsizetype maximum_disposition_authorities = 32;
constexpr qsizetype maximum_disposition_record_anchors = 32;
constexpr qsizetype maximum_source_url_characters = 2048;
constexpr qsizetype maximum_canonical_text_code_units = 8192;
constexpr std::uint32_t maximum_events_per_command = 3;
constexpr int maximum_json_depth = 64;

constexpr auto submit_filing_type = "filing.submit";
constexpr auto enter_order_type = "order.enter";
constexpr auto set_sealed_type = "sealed.set";
constexpr auto schedule_argument_type = "argument.schedule";
constexpr auto issue_judgment_type = "judgment.issue";
constexpr auto issue_mandate_type = "mandate.issue";
constexpr auto calculate_deadline_command_type = "deadline.calculate";
constexpr auto advance_stage_command_type = "stage.advance";

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

[[nodiscard]] auto encodeAuthorityType(model::AuthorityType type)
    -> std::expected<QString, WorkflowCodecError> {
    switch (type) {
    case model::AuthorityType::Constitution:
        return QStringLiteral("constitution");
    case model::AuthorityType::Statute:
        return QStringLiteral("statute");
    case model::AuthorityType::Rule:
        return QStringLiteral("rule");
    case model::AuthorityType::Regulation:
        return QStringLiteral("regulation");
    case model::AuthorityType::Case:
        return QStringLiteral("case");
    case model::AuthorityType::Order:
        return QStringLiteral("order");
    case model::AuthorityType::AdministrativeDecision:
        return QStringLiteral("administrative_decision");
    case model::AuthorityType::Other:
        return QStringLiteral("other");
    }
    return fail(WorkflowCodecErrorCode::IncompleteAuthority,
                QStringLiteral("Unknown authority type"));
}

[[nodiscard]] auto decodeAuthorityType(QStringView value) -> std::optional<model::AuthorityType> {
    if (value == u"constitution") {
        return model::AuthorityType::Constitution;
    }
    if (value == u"statute") {
        return model::AuthorityType::Statute;
    }
    if (value == u"rule") {
        return model::AuthorityType::Rule;
    }
    if (value == u"regulation") {
        return model::AuthorityType::Regulation;
    }
    if (value == u"case") {
        return model::AuthorityType::Case;
    }
    if (value == u"order") {
        return model::AuthorityType::Order;
    }
    if (value == u"administrative_decision") {
        return model::AuthorityType::AdministrativeDecision;
    }
    if (value == u"other") {
        return model::AuthorityType::Other;
    }
    return std::nullopt;
}

[[nodiscard]] auto encodePrecedentialStatus(model::PrecedentialStatus status)
    -> std::expected<QString, WorkflowCodecError> {
    switch (status) {
    case model::PrecedentialStatus::NotApplicable:
        return QStringLiteral("not_applicable");
    case model::PrecedentialStatus::Precedential:
        return QStringLiteral("precedential");
    case model::PrecedentialStatus::Nonprecedential:
        return QStringLiteral("nonprecedential");
    }
    return fail(WorkflowCodecErrorCode::IncompleteAuthority,
                QStringLiteral("Unknown precedential status"));
}

[[nodiscard]] auto decodePrecedentialStatus(QStringView value)
    -> std::optional<model::PrecedentialStatus> {
    if (value == u"not_applicable") {
        return model::PrecedentialStatus::NotApplicable;
    }
    if (value == u"precedential") {
        return model::PrecedentialStatus::Precedential;
    }
    if (value == u"nonprecedential") {
        return model::PrecedentialStatus::Nonprecedential;
    }
    return std::nullopt;
}

[[nodiscard]] auto encodeProvenance(const model::AuthorityProvenance& provenance)
    -> std::expected<QJsonObject, WorkflowCodecError> {
    const auto type = encodeAuthorityType(provenance.type);
    const auto status = encodePrecedentialStatus(provenance.precedential_status);
    if (!type || !status || !roundTripsUtf8(provenance.jurisdiction_id) ||
        !roundTripsUtf8(provenance.issuing_body_id) || !roundTripsUtf8(provenance.checked_on) ||
        !roundTripsUtf8(provenance.locator) || !roundTripsUtf8(provenance.source_url)) {
        return fail(WorkflowCodecErrorCode::IncompleteAuthority,
                    QStringLiteral("Authority provenance must be complete valid UTF-8"));
    }
    const auto jurisdiction_id = QString::fromUtf8(provenance.jurisdiction_id);
    const auto issuing_body_id = QString::fromUtf8(provenance.issuing_body_id);
    const auto checked_on = QString::fromUtf8(provenance.checked_on);
    const auto locator = QString::fromUtf8(provenance.locator);
    const auto source_url = QString::fromUtf8(provenance.source_url);
    if (!isNamespacedId(jurisdiction_id) || !isNamespacedId(issuing_body_id) ||
        !parseDateText(checked_on, u"authority.provenance.checked_on") ||
        !model::isCanonicalAuthorityText(provenance.locator, 4096) || source_url.isEmpty() ||
        source_url.toUtf8().size() > maximum_source_url_characters || containsNull(source_url) ||
        !model::isCanonicalAuthoritySourceUrl(provenance.source_url)) {
        return fail(WorkflowCodecErrorCode::IncompleteAuthority,
                    QStringLiteral("Authority provenance is incomplete or noncanonical"));
    }
    return QJsonObject{
        {QStringLiteral("authority_type"), *type},
        {QStringLiteral("checked_on"), checked_on},
        {QStringLiteral("issuing_body_id"), issuing_body_id},
        {QStringLiteral("jurisdiction_id"), jurisdiction_id},
        {QStringLiteral("locator"), locator},
        {QStringLiteral("official_source"), provenance.official_source},
        {QStringLiteral("precedential_status"), *status},
        {QStringLiteral("source_url"), source_url},
    };
}

[[nodiscard]] auto decodeProvenance(const QJsonObject& object, QStringView context)
    -> std::expected<model::AuthorityProvenance, WorkflowCodecError> {
    if (const auto keys =
            exactKeys(object,
                      {u"authority_type", u"checked_on", u"issuing_body_id", u"jurisdiction_id",
                       u"locator", u"official_source", u"precedential_status", u"source_url"},
                      context);
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto type_text = readString(object, u"authority_type", 32, context);
    const auto jurisdiction_id = readString(object, u"jurisdiction_id", 160, context);
    const auto issuing_body_id = readString(object, u"issuing_body_id", 160, context);
    const auto status_text = readString(object, u"precedential_status", 32, context);
    const auto checked_on = readString(object, u"checked_on", 10, context);
    const auto locator = readString(object, u"locator", maximum_canonical_text_code_units, context);
    const auto source_url =
        readString(object, u"source_url", maximum_source_url_characters, context);
    const auto official_source = object.value(u"official_source");
    if (!type_text || !jurisdiction_id || !issuing_body_id || !status_text || !checked_on ||
        !locator || !source_url || !official_source.isBool()) {
        return fail(WorkflowCodecErrorCode::IncompleteAuthority,
                    QStringLiteral("Authority provenance fields must be complete"));
    }
    const auto type = decodeAuthorityType(*type_text);
    const auto status = decodePrecedentialStatus(*status_text);
    const auto locator_bytes = locator->toUtf8().toStdString();
    const auto source_url_bytes = source_url->toUtf8().toStdString();
    if (!type || !status || !isNamespacedId(*jurisdiction_id) ||
        !isNamespacedId(*issuing_body_id) ||
        !parseDateText(*checked_on, QStringLiteral("%1.checked_on").arg(context)) ||
        !model::isCanonicalAuthorityText(locator_bytes, 4096) ||
        !model::isCanonicalAuthoritySourceUrl(source_url_bytes)) {
        return fail(WorkflowCodecErrorCode::IncompleteAuthority,
                    QStringLiteral("Authority provenance is incomplete or noncanonical"));
    }
    return model::AuthorityProvenance{
        *type,
        jurisdiction_id->toUtf8().toStdString(),
        issuing_body_id->toUtf8().toStdString(),
        *status,
        official_source.toBool(),
        checked_on->toUtf8().toStdString(),
        std::move(locator_bytes),
        std::move(source_url_bytes),
    };
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
    const auto has_provenance = authority.provenance.has_value();
    if (citation.isEmpty() || containsNull(citation) || proposition.isEmpty() ||
        containsNull(proposition) ||
        (!has_provenance && (citation.toUtf8().size() > maximum_text_characters ||
                             proposition.toUtf8().size() > maximum_text_characters)) ||
        (has_provenance && (!model::isCanonicalAuthorityText(authority.citation, 4096) ||
                            !model::isCanonicalAuthorityText(authority.proposition, 4096) ||
                            !model::authorityVerificationNotBeforeSource(
                                authority.source_version, authority.provenance->checked_on))) ||
        !parseDateText(source_version, u"authority.source_version")) {
        return fail(WorkflowCodecErrorCode::IncompleteAuthority,
                    QStringLiteral("Authority fields must be complete and bounded"));
    }
    QJsonObject result{
        {QStringLiteral("citation"), citation},
        {QStringLiteral("id"), *id},
        {QStringLiteral("proposition"), proposition},
        {QStringLiteral("source_version"), source_version},
    };
    if (has_provenance) {
        const auto provenance = encodeProvenance(*authority.provenance);
        if (!provenance) {
            return std::unexpected(provenance.error());
        }
        result.insert(QStringLiteral("provenance"), *provenance);
    }
    return result;
}

[[nodiscard]] auto decodeAuthorityRef(const QJsonObject& object, QStringView context)
    -> std::expected<model::AuthorityRef, WorkflowCodecError> {
    const auto has_provenance = object.contains(u"provenance");
    const auto keys =
        has_provenance
            ? exactKeys(object,
                        {u"citation", u"id", u"proposition", u"provenance", u"source_version"},
                        context)
            : exactKeys(object, {u"citation", u"id", u"proposition", u"source_version"}, context);
    if (!keys) {
        return std::unexpected(keys.error());
    }
    const auto id = readId(object, u"id", context);
    const auto citation = readString(
        object, u"citation",
        has_provenance ? maximum_canonical_text_code_units : maximum_text_characters, context);
    const auto source_version = readString(object, u"source_version", 10, context);
    const auto proposition = readString(
        object, u"proposition",
        has_provenance ? maximum_canonical_text_code_units : maximum_text_characters, context);
    if (!id || !citation || !source_version || !proposition) {
        return fail(WorkflowCodecErrorCode::IncompleteAuthority,
                    QStringLiteral("Authority fields must be complete and bounded"));
    }
    if (!parseDateText(*source_version, QStringLiteral("%1.source_version").arg(context))) {
        return fail(WorkflowCodecErrorCode::IncompleteAuthority,
                    QStringLiteral("Authority source_version must be a valid YYYY-MM-DD date"));
    }
    std::optional<model::AuthorityProvenance> provenance;
    if (has_provenance) {
        const auto provenance_value = object.value(u"provenance");
        const auto citation_bytes = citation->toUtf8().toStdString();
        const auto proposition_bytes = proposition->toUtf8().toStdString();
        if (!provenance_value.isObject() ||
            !model::isCanonicalAuthorityText(citation_bytes, 4096) ||
            !model::isCanonicalAuthorityText(proposition_bytes, 4096)) {
            return fail(WorkflowCodecErrorCode::IncompleteAuthority,
                        QStringLiteral("Authority provenance must be an object"));
        }
        auto decoded = decodeProvenance(provenance_value.toObject(),
                                        QStringLiteral("%1.provenance").arg(context));
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        if (!model::authorityVerificationNotBeforeSource(source_version->toUtf8().toStdString(),
                                                         decoded->checked_on)) {
            return fail(WorkflowCodecErrorCode::IncompleteAuthority,
                        QStringLiteral("Authority verification predates its source version"));
        }
        provenance = std::move(*decoded);
    }
    return model::AuthorityRef{model::AuthorityId{*id}, citation->toUtf8().toStdString(),
                               source_version->toUtf8().toStdString(),
                               proposition->toUtf8().toStdString(), std::move(provenance)};
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

[[nodiscard]] auto authoritySchemaVersion(const model::AuthorityBasis& authority)
    -> std::expected<int, WorkflowCodecError> {
    const auto has_provenance = authority.primary.provenance.has_value();
    if (std::ranges::any_of(authority.supporting, [&](const auto& supporting) {
            return supporting.provenance.has_value() != has_provenance;
        })) {
        return fail(WorkflowCodecErrorCode::IncompleteAuthority,
                    QStringLiteral("Authority basis mixes legacy and provenance-bearing refs"));
    }
    return has_provenance ? provenance_schema_version : legacy_schema_version;
}

[[nodiscard]] auto authorityOf(const model::WorkflowEvent& event) -> const model::AuthorityBasis& {
    return std::visit(
        [](const auto& concrete) -> const model::AuthorityBasis& {
            return concrete.header.authority;
        },
        event);
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

[[nodiscard]] auto
encodeDeadlineEventBase(const std::optional<model::WorkflowDeadlineEventBase>& base)
    -> std::expected<QJsonValue, WorkflowCodecError> {
    if (!base.has_value()) {
        return QJsonValue{QJsonValue::Null};
    }
    return std::visit(
        [](const auto& concrete) -> std::expected<QJsonValue, WorkflowCodecError> {
            using Base = std::remove_cvref_t<decltype(concrete)>;
            if constexpr (std::same_as<Base, model::WorkflowJudgmentOccurredDeadlineBase>) {
                return QJsonObject{{QStringLiteral("kind"), QStringLiteral("judgment_occurred")}};
            } else {
                const auto order_id =
                    checkedId(concrete.order_id.value, u"payload.deadline_event_base.order_id");
                const auto operation_id = checkedId(concrete.operation_id.value,
                                                    u"payload.deadline_event_base.operation_id");
                if (!order_id) {
                    return std::unexpected(order_id.error());
                }
                if (!operation_id) {
                    return std::unexpected(operation_id.error());
                }
                return QJsonObject{{QStringLiteral("kind"), QStringLiteral("order_occurred")},
                                   {QStringLiteral("operation_id"), *operation_id},
                                   {QStringLiteral("order_id"), *order_id}};
            }
        },
        *base);
}

[[nodiscard]] auto decodeDeadlineEventBase(const QJsonObject& payload)
    -> std::expected<std::optional<model::WorkflowDeadlineEventBase>, WorkflowCodecError> {
    const auto value = payload.value(u"deadline_event_base");
    if (value.isNull()) {
        return std::nullopt;
    }
    if (!value.isObject()) {
        return fail(WorkflowCodecErrorCode::InvalidField,
                    QStringLiteral("payload.deadline_event_base must be an object or null"));
    }
    const auto object = value.toObject();
    const auto kind = readString(object, u"kind", 32, u"payload.deadline_event_base");
    if (!kind) {
        return std::unexpected(kind.error());
    }
    if (*kind == u"judgment_occurred") {
        if (const auto keys = exactKeys(object, {u"kind"}, u"payload.deadline_event_base"); !keys) {
            return std::unexpected(keys.error());
        }
        return model::WorkflowDeadlineEventBase{model::WorkflowJudgmentOccurredDeadlineBase{}};
    }
    if (*kind == u"order_occurred") {
        if (const auto keys = exactKeys(object, {u"kind", u"operation_id", u"order_id"},
                                        u"payload.deadline_event_base");
            !keys) {
            return std::unexpected(keys.error());
        }
        const auto order_id =
            decodeId<model::WorkflowOrderId>(object, u"order_id", u"payload.deadline_event_base");
        const auto operation_id = decodeId<model::WorkflowOperationId>(
            object, u"operation_id", u"payload.deadline_event_base");
        if (!order_id) {
            return std::unexpected(order_id.error());
        }
        if (!operation_id) {
            return std::unexpected(operation_id.error());
        }
        return model::WorkflowDeadlineEventBase{
            model::WorkflowOrderOccurredDeadlineBase{*order_id, *operation_id}};
    }
    return fail(WorkflowCodecErrorCode::InvalidField,
                QStringLiteral("payload.deadline_event_base.kind is unknown"));
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

[[nodiscard]] auto encodeDeadlineCondition(model::WorkflowDeadlineCondition condition)
    -> std::expected<QString, WorkflowCodecError> {
    switch (condition) {
    case model::WorkflowDeadlineCondition::Open:
        return QStringLiteral("open");
    case model::WorkflowDeadlineCondition::Satisfied:
        return QStringLiteral("satisfied");
    case model::WorkflowDeadlineCondition::Reached:
        return QStringLiteral("reached");
    case model::WorkflowDeadlineCondition::Elapsed:
        return QStringLiteral("elapsed");
    case model::WorkflowDeadlineCondition::NotElapsed:
        return QStringLiteral("not_elapsed");
    }
    return fail(WorkflowCodecErrorCode::InvalidField,
                QStringLiteral("Unknown workflow deadline condition"));
}

[[nodiscard]] auto decodeDeadlineCondition(const QJsonObject& object, QStringView key,
                                           QStringView context)
    -> std::expected<model::WorkflowDeadlineCondition, WorkflowCodecError> {
    const auto value = readString(object, key, 16, context);
    if (!value) {
        return std::unexpected(value.error());
    }
    if (*value == u"open") {
        return model::WorkflowDeadlineCondition::Open;
    }
    if (*value == u"satisfied") {
        return model::WorkflowDeadlineCondition::Satisfied;
    }
    if (*value == u"reached") {
        return model::WorkflowDeadlineCondition::Reached;
    }
    if (*value == u"elapsed") {
        return model::WorkflowDeadlineCondition::Elapsed;
    }
    if (*value == u"not_elapsed") {
        return model::WorkflowDeadlineCondition::NotElapsed;
    }
    return fail(WorkflowCodecErrorCode::InvalidField,
                QStringLiteral("%1.%2 is an unknown deadline condition").arg(context, key));
}

[[nodiscard]] auto preconditionSubject(const model::WorkflowPrecondition& precondition) -> QString {
    return std::visit(
        [](const auto& concrete) {
            using Precondition = std::remove_cvref_t<decltype(concrete)>;
            if constexpr (std::same_as<Precondition, model::WorkflowFilingPrecondition>) {
                return QStringLiteral("filing:") + QString::fromUtf8(concrete.filing_type.value);
            } else if constexpr (std::same_as<Precondition, model::WorkflowOrderPrecondition>) {
                return QStringLiteral("order:") + QString::fromUtf8(concrete.order_id.value);
            } else if constexpr (std::same_as<Precondition, model::WorkflowDeadlinePrecondition>) {
                QString condition;
                switch (concrete.condition) {
                case model::WorkflowDeadlineCondition::Open:
                    condition = QStringLiteral("open");
                    break;
                case model::WorkflowDeadlineCondition::Satisfied:
                    condition = QStringLiteral("satisfied");
                    break;
                case model::WorkflowDeadlineCondition::Elapsed:
                    condition = QStringLiteral("elapsed");
                    break;
                case model::WorkflowDeadlineCondition::NotElapsed:
                    condition = QStringLiteral("not_elapsed");
                    break;
                case model::WorkflowDeadlineCondition::Reached:
                    condition = QStringLiteral("reached");
                    break;
                }
                return QStringLiteral("deadline:") + QString::fromUtf8(concrete.deadline_id.value) +
                       u':' + condition;
            } else if constexpr (std::same_as<Precondition, model::WorkflowArgumentPrecondition>) {
                return QStringLiteral("argument:") +
                       (concrete.scheduled ? QStringLiteral("true") : QStringLiteral("false"));
            } else if constexpr (std::same_as<Precondition,
                                              model::WorkflowArgumentDatePrecondition>) {
                return QStringLiteral("argument-date:reached");
            } else {
                return QStringLiteral("judgment");
            }
        },
        precondition);
}

[[nodiscard]] auto
contradictoryPreconditionSubjects(const model::WorkflowPrecondition& precondition) -> QStringList {
    if (const auto* argument = std::get_if<model::WorkflowArgumentPrecondition>(&precondition)) {
        QStringList result{argument->scheduled ? QStringLiteral("argument:false")
                                               : QStringLiteral("argument:true")};
        if (!argument->scheduled) {
            result.push_back(QStringLiteral("argument-date:reached"));
        }
        return result;
    }
    if (std::holds_alternative<model::WorkflowArgumentDatePrecondition>(precondition)) {
        return {QStringLiteral("argument:false")};
    }
    const auto* deadline = std::get_if<model::WorkflowDeadlinePrecondition>(&precondition);
    if (deadline == nullptr) {
        return {};
    }
    QString opposite;
    switch (deadline->condition) {
    case model::WorkflowDeadlineCondition::Open:
        opposite = QStringLiteral("satisfied");
        break;
    case model::WorkflowDeadlineCondition::Satisfied:
        opposite = QStringLiteral("open");
        break;
    case model::WorkflowDeadlineCondition::Elapsed:
        opposite = QStringLiteral("not_elapsed");
        break;
    case model::WorkflowDeadlineCondition::NotElapsed:
        opposite = QStringLiteral("elapsed");
        break;
    case model::WorkflowDeadlineCondition::Reached:
        return {};
    }
    return {QStringLiteral("deadline:") + QString::fromUtf8(deadline->deadline_id.value) + u':' +
            opposite};
}

[[nodiscard]] auto encodePrecondition(const model::WorkflowPrecondition& precondition)
    -> std::expected<QJsonObject, WorkflowCodecError> {
    return std::visit(
        [](const auto& concrete) -> std::expected<QJsonObject, WorkflowCodecError> {
            using Precondition = std::remove_cvref_t<decltype(concrete)>;
            if constexpr (std::same_as<Precondition, model::WorkflowFilingPrecondition>) {
                const auto filing_type =
                    checkedId(concrete.filing_type.value, u"payload.preconditions.filing_type_id");
                if (!filing_type) {
                    return std::unexpected(filing_type.error());
                }
                return QJsonObject{{QStringLiteral("filing_type_id"), *filing_type},
                                   {QStringLiteral("kind"), QStringLiteral("filing_presence")},
                                   {QStringLiteral("present"), concrete.present}};
            } else if constexpr (std::same_as<Precondition, model::WorkflowOrderPrecondition>) {
                const auto order_id =
                    checkedId(concrete.order_id.value, u"payload.preconditions.order_id");
                const auto disposition = encodeOrderDisposition(concrete.disposition);
                if (!order_id) {
                    return std::unexpected(order_id.error());
                }
                if (!disposition) {
                    return std::unexpected(disposition.error());
                }
                return QJsonObject{{QStringLiteral("disposition"), *disposition},
                                   {QStringLiteral("kind"), QStringLiteral("order_disposition")},
                                   {QStringLiteral("order_id"), *order_id}};
            } else if constexpr (std::same_as<Precondition, model::WorkflowDeadlinePrecondition>) {
                const auto deadline_id =
                    checkedId(concrete.deadline_id.value, u"payload.preconditions.deadline_id");
                const auto condition = encodeDeadlineCondition(concrete.condition);
                if (!deadline_id) {
                    return std::unexpected(deadline_id.error());
                }
                if (!condition) {
                    return std::unexpected(condition.error());
                }
                return QJsonObject{{QStringLiteral("deadline_id"), *deadline_id},
                                   {QStringLiteral("kind"), QStringLiteral("deadline_status")},
                                   {QStringLiteral("status"), *condition}};
            } else if constexpr (std::same_as<Precondition, model::WorkflowArgumentPrecondition>) {
                return QJsonObject{{QStringLiteral("kind"), QStringLiteral("argument_scheduled")},
                                   {QStringLiteral("scheduled"), concrete.scheduled}};
            } else if constexpr (std::same_as<Precondition,
                                              model::WorkflowArgumentDatePrecondition>) {
                if (concrete.condition != model::WorkflowArgumentDateCondition::Reached) {
                    return fail(WorkflowCodecErrorCode::InvalidField,
                                QStringLiteral("Unknown argument-date condition"));
                }
                return QJsonObject{{QStringLiteral("kind"), QStringLiteral("argument_date_status")},
                                   {QStringLiteral("status"), QStringLiteral("reached")}};
            } else {
                return QJsonObject{{QStringLiteral("issued"), concrete.issued},
                                   {QStringLiteral("kind"), QStringLiteral("judgment_issued")}};
            }
        },
        precondition);
}

[[nodiscard]] auto
encodePreconditions(const std::vector<model::WorkflowPrecondition>& preconditions)
    -> std::expected<QJsonArray, WorkflowCodecError> {
    if (preconditions.size() > static_cast<std::size_t>(maximum_preconditions)) {
        return fail(WorkflowCodecErrorCode::OutOfRange,
                    QStringLiteral("payload.preconditions exceeds its bound"));
    }
    QSet<QString> subjects;
    QJsonArray result;
    for (const auto& precondition : preconditions) {
        const auto subject = preconditionSubject(precondition);
        const auto contradictory = contradictoryPreconditionSubjects(precondition);
        if (subjects.contains(subject) ||
            std::ranges::any_of(contradictory,
                                [&](const QString& item) { return subjects.contains(item); })) {
            return fail(
                WorkflowCodecErrorCode::InvalidField,
                QStringLiteral("payload.preconditions has duplicate or conflicting subjects"));
        }
        subjects.insert(subject);
        const auto encoded = encodePrecondition(precondition);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        result.append(*encoded);
    }
    return result;
}

[[nodiscard]] bool
usesExtendedPreconditions(const std::vector<model::WorkflowPrecondition>& preconditions) {
    return std::ranges::any_of(preconditions, [](const auto& precondition) {
        if (std::holds_alternative<model::WorkflowArgumentDatePrecondition>(precondition)) {
            return true;
        }
        const auto* deadline = std::get_if<model::WorkflowDeadlinePrecondition>(&precondition);
        return deadline != nullptr &&
               deadline->condition == model::WorkflowDeadlineCondition::Reached;
    });
}

[[nodiscard]] auto decodePrecondition(const QJsonObject& object, QStringView context,
                                      bool allow_extended)
    -> std::expected<model::WorkflowPrecondition, WorkflowCodecError> {
    const auto kind = readString(object, u"kind", 32, context);
    if (!kind) {
        return std::unexpected(kind.error());
    }
    if (*kind == u"filing_presence") {
        if (const auto keys = exactKeys(object, {u"filing_type_id", u"kind", u"present"}, context);
            !keys) {
            return std::unexpected(keys.error());
        }
        const auto filing_type = decodeId<model::FilingTypeId>(object, u"filing_type_id", context);
        const auto present = object.value(u"present");
        if (!filing_type) {
            return std::unexpected(filing_type.error());
        }
        if (!present.isBool()) {
            return fail(WorkflowCodecErrorCode::InvalidField,
                        QStringLiteral("%1.present must be a boolean").arg(context));
        }
        return model::WorkflowFilingPrecondition{*filing_type, present.toBool()};
    }
    if (*kind == u"order_disposition") {
        if (const auto keys = exactKeys(object, {u"disposition", u"kind", u"order_id"}, context);
            !keys) {
            return std::unexpected(keys.error());
        }
        const auto order_id = decodeId<model::WorkflowOrderId>(object, u"order_id", context);
        const auto disposition = decodeOrderDisposition(object, u"disposition", context);
        if (!order_id) {
            return std::unexpected(order_id.error());
        }
        if (!disposition) {
            return std::unexpected(disposition.error());
        }
        return model::WorkflowOrderPrecondition{*order_id, *disposition};
    }
    if (*kind == u"deadline_status") {
        if (const auto keys = exactKeys(object, {u"deadline_id", u"kind", u"status"}, context);
            !keys) {
            return std::unexpected(keys.error());
        }
        const auto deadline_id =
            decodeId<model::WorkflowDeadlineId>(object, u"deadline_id", context);
        const auto condition = decodeDeadlineCondition(object, u"status", context);
        if (!deadline_id) {
            return std::unexpected(deadline_id.error());
        }
        if (!condition) {
            return std::unexpected(condition.error());
        }
        if (*condition == model::WorkflowDeadlineCondition::Reached && !allow_extended) {
            return fail(WorkflowCodecErrorCode::UnsupportedVersion,
                        QStringLiteral("deadline reached requires workflow event schema 4"));
        }
        return model::WorkflowDeadlinePrecondition{*deadline_id, *condition};
    }
    if (*kind == u"argument_scheduled") {
        if (const auto keys = exactKeys(object, {u"kind", u"scheduled"}, context); !keys) {
            return std::unexpected(keys.error());
        }
        const auto scheduled = object.value(u"scheduled");
        if (!scheduled.isBool()) {
            return fail(WorkflowCodecErrorCode::InvalidField,
                        QStringLiteral("%1.scheduled must be a boolean").arg(context));
        }
        return model::WorkflowArgumentPrecondition{scheduled.toBool()};
    }
    if (*kind == u"argument_date_status") {
        if (!allow_extended) {
            return fail(WorkflowCodecErrorCode::UnsupportedVersion,
                        QStringLiteral("argument-date guards require workflow event schema 4"));
        }
        if (const auto keys = exactKeys(object, {u"kind", u"status"}, context); !keys) {
            return std::unexpected(keys.error());
        }
        const auto status = readString(object, u"status", 16, context);
        if (!status) {
            return std::unexpected(status.error());
        }
        if (*status != u"reached") {
            return fail(WorkflowCodecErrorCode::InvalidField,
                        QStringLiteral("%1.status is unknown").arg(context));
        }
        return model::WorkflowArgumentDatePrecondition{
            model::WorkflowArgumentDateCondition::Reached};
    }
    if (*kind == u"judgment_issued") {
        if (const auto keys = exactKeys(object, {u"issued", u"kind"}, context); !keys) {
            return std::unexpected(keys.error());
        }
        const auto issued = object.value(u"issued");
        if (!issued.isBool()) {
            return fail(WorkflowCodecErrorCode::InvalidField,
                        QStringLiteral("%1.issued must be a boolean").arg(context));
        }
        return model::WorkflowJudgmentPrecondition{issued.toBool()};
    }
    return fail(WorkflowCodecErrorCode::InvalidField,
                QStringLiteral("%1.kind is unknown").arg(context));
}

[[nodiscard]] auto decodePreconditions(const QJsonValue& value, bool allow_extended)
    -> std::expected<std::vector<model::WorkflowPrecondition>, WorkflowCodecError> {
    if (!value.isArray()) {
        return fail(WorkflowCodecErrorCode::InvalidField,
                    QStringLiteral("payload.preconditions must be an array"));
    }
    const auto array = value.toArray();
    if (array.size() > maximum_preconditions) {
        return fail(WorkflowCodecErrorCode::OutOfRange,
                    QStringLiteral("payload.preconditions exceeds its bound"));
    }
    QSet<QString> subjects;
    std::vector<model::WorkflowPrecondition> result;
    result.reserve(static_cast<std::size_t>(array.size()));
    for (qsizetype index = 0; index < array.size(); ++index) {
        if (!array.at(index).isObject()) {
            return fail(WorkflowCodecErrorCode::InvalidField,
                        QStringLiteral("payload.preconditions[%1] must be an object").arg(index));
        }
        const auto decoded = decodePrecondition(
            array.at(index).toObject(), QStringLiteral("payload.preconditions[%1]").arg(index),
            allow_extended);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        const auto subject = preconditionSubject(*decoded);
        const auto contradictory = contradictoryPreconditionSubjects(*decoded);
        if (subjects.contains(subject) ||
            std::ranges::any_of(contradictory,
                                [&](const QString& item) { return subjects.contains(item); })) {
            return fail(
                WorkflowCodecErrorCode::InvalidField,
                QStringLiteral("payload.preconditions has duplicate or conflicting subjects"));
        }
        subjects.insert(subject);
        result.push_back(*decoded);
    }
    return result;
}

[[nodiscard]] auto encodeDispositionScope(model::DispositionScope scope)
    -> std::expected<QString, WorkflowCodecError> {
    switch (scope) {
    case model::DispositionScope::Whole:
        return QStringLiteral("whole");
    case model::DispositionScope::Part:
        return QStringLiteral("part");
    }
    return fail(WorkflowCodecErrorCode::InvalidField, QStringLiteral("Unknown disposition scope"));
}

[[nodiscard]] auto decodeDispositionScope(const QJsonObject& object, QStringView context)
    -> std::expected<model::DispositionScope, WorkflowCodecError> {
    const auto value = readString(object, u"scope", 16, context);
    if (!value) {
        return std::unexpected(value.error());
    }
    if (*value == u"whole") {
        return model::DispositionScope::Whole;
    }
    if (*value == u"part") {
        return model::DispositionScope::Part;
    }
    return fail(WorkflowCodecErrorCode::InvalidField,
                QStringLiteral("%1.scope is unknown").arg(context));
}

[[nodiscard]] auto encodeDispositionAction(model::DispositionAction action)
    -> std::expected<QString, WorkflowCodecError> {
    switch (action) {
    case model::DispositionAction::Affirm:
        return QStringLiteral("affirm");
    case model::DispositionAction::Reverse:
        return QStringLiteral("reverse");
    case model::DispositionAction::Vacate:
        return QStringLiteral("vacate");
    case model::DispositionAction::Dismiss:
        return QStringLiteral("dismiss");
    case model::DispositionAction::Grant:
        return QStringLiteral("grant");
    case model::DispositionAction::Deny:
        return QStringLiteral("deny");
    }
    return fail(WorkflowCodecErrorCode::InvalidField, QStringLiteral("Unknown disposition action"));
}

[[nodiscard]] auto decodeDispositionAction(const QJsonObject& object, QStringView context)
    -> std::expected<model::DispositionAction, WorkflowCodecError> {
    const auto value = readString(object, u"action", 16, context);
    if (!value) {
        return std::unexpected(value.error());
    }
    if (*value == u"affirm") {
        return model::DispositionAction::Affirm;
    }
    if (*value == u"reverse") {
        return model::DispositionAction::Reverse;
    }
    if (*value == u"vacate") {
        return model::DispositionAction::Vacate;
    }
    if (*value == u"dismiss") {
        return model::DispositionAction::Dismiss;
    }
    if (*value == u"grant") {
        return model::DispositionAction::Grant;
    }
    if (*value == u"deny") {
        return model::DispositionAction::Deny;
    }
    return fail(WorkflowCodecErrorCode::InvalidField,
                QStringLiteral("%1.action is unknown").arg(context));
}

[[nodiscard]] bool actionMayRemand(model::DispositionAction action) {
    return action == model::DispositionAction::Reverse ||
           action == model::DispositionAction::Vacate ||
           action == model::DispositionAction::Dismiss || action == model::DispositionAction::Grant;
}

[[nodiscard]] auto encodeDispositionFinality(model::DispositionFinality finality)
    -> std::expected<QString, WorkflowCodecError> {
    switch (finality) {
    case model::DispositionFinality::Final:
        return QStringLiteral("final");
    case model::DispositionFinality::Nonfinal:
        return QStringLiteral("nonfinal");
    }
    return fail(WorkflowCodecErrorCode::InvalidField,
                QStringLiteral("Unknown disposition finality"));
}

[[nodiscard]] auto decodeDispositionFinality(const QJsonObject& object, QStringView context)
    -> std::expected<model::DispositionFinality, WorkflowCodecError> {
    const auto value = readString(object, u"finality", 16, context);
    if (!value) {
        return std::unexpected(value.error());
    }
    if (*value == u"final") {
        return model::DispositionFinality::Final;
    }
    if (*value == u"nonfinal") {
        return model::DispositionFinality::Nonfinal;
    }
    return fail(WorkflowCodecErrorCode::InvalidField,
                QStringLiteral("%1.finality is unknown").arg(context));
}

[[nodiscard]] auto encodeDispositionComponent(const model::DispositionComponent& component)
    -> std::expected<QJsonObject, WorkflowCodecError> {
    const auto issue_id = checkedId(component.issue_id.value, u"disposition.components.issue_id");
    const auto target_id =
        checkedId(component.target_id.value, u"disposition.components.target_id");
    const auto scope = encodeDispositionScope(component.scope);
    const auto action = encodeDispositionAction(component.action);
    const auto authorities = encodeIdArray(component.authority_ids, maximum_disposition_authorities,
                                           true, u"disposition.components.authority_ids");
    const auto anchors =
        encodeIdArray(component.record_anchor_ids, maximum_disposition_record_anchors, true,
                      u"disposition.components.record_anchor_ids");
    if (!issue_id) {
        return std::unexpected(issue_id.error());
    }
    if (!target_id) {
        return std::unexpected(target_id.error());
    }
    if (!scope) {
        return std::unexpected(scope.error());
    }
    if (!action) {
        return std::unexpected(action.error());
    }
    if (!authorities) {
        return std::unexpected(authorities.error());
    }
    if (!anchors) {
        return std::unexpected(anchors.error());
    }
    if (component.remand && !actionMayRemand(component.action)) {
        return fail(WorkflowCodecErrorCode::InvalidField,
                    QStringLiteral("Disposition action cannot carry remand"));
    }
    return QJsonObject{{QStringLiteral("action"), *action},
                       {QStringLiteral("authority_ids"), *authorities},
                       {QStringLiteral("issue_id"), *issue_id},
                       {QStringLiteral("record_anchor_ids"), *anchors},
                       {QStringLiteral("remand"), component.remand},
                       {QStringLiteral("scope"), *scope},
                       {QStringLiteral("target_id"), *target_id}};
}

[[nodiscard]] auto decodeDispositionComponent(const QJsonObject& object, QStringView context)
    -> std::expected<model::DispositionComponent, WorkflowCodecError> {
    if (const auto keys = exactKeys(object,
                                    {u"action", u"authority_ids", u"issue_id", u"record_anchor_ids",
                                     u"remand", u"scope", u"target_id"},
                                    context);
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto issue_id = decodeId<model::CaseIssueId>(object, u"issue_id", context);
    const auto target_id = decodeId<model::DispositionTargetId>(object, u"target_id", context);
    const auto scope = decodeDispositionScope(object, context);
    const auto action = decodeDispositionAction(object, context);
    const auto authorities = decodeIdArray<model::AuthorityId>(
        object.value(u"authority_ids"), maximum_disposition_authorities, true,
        QStringLiteral("%1.authority_ids").arg(context));
    const auto anchors = decodeIdArray<model::RecordAnchorId>(
        object.value(u"record_anchor_ids"), maximum_disposition_record_anchors, true,
        QStringLiteral("%1.record_anchor_ids").arg(context));
    const auto remand = object.value(u"remand");
    if (!issue_id) {
        return std::unexpected(issue_id.error());
    }
    if (!target_id) {
        return std::unexpected(target_id.error());
    }
    if (!scope) {
        return std::unexpected(scope.error());
    }
    if (!action) {
        return std::unexpected(action.error());
    }
    if (!authorities) {
        return std::unexpected(authorities.error());
    }
    if (!anchors) {
        return std::unexpected(anchors.error());
    }
    if (!remand.isBool() || (remand.toBool() && !actionMayRemand(*action))) {
        return fail(WorkflowCodecErrorCode::InvalidField,
                    QStringLiteral("%1.remand is invalid for its action").arg(context));
    }
    return model::DispositionComponent{
        *issue_id,          *target_id, *scope, *action, remand.toBool(), std::move(*authorities),
        std::move(*anchors)};
}

[[nodiscard]] auto encodeDispositionPlan(const model::DispositionPlan& plan)
    -> std::expected<QJsonObject, WorkflowCodecError> {
    const auto plan_id = checkedId(plan.id.value, u"disposition.plan_id");
    const auto finality = encodeDispositionFinality(plan.finality);
    const auto digest = checkedDigest(plan.canonical_sha256, u"disposition.digest");
    if (!plan_id) {
        return std::unexpected(plan_id.error());
    }
    if (!finality) {
        return std::unexpected(finality.error());
    }
    if (!digest) {
        return std::unexpected(digest.error());
    }
    if (plan.components.empty() ||
        plan.components.size() > static_cast<std::size_t>(maximum_disposition_components)) {
        return fail(WorkflowCodecErrorCode::OutOfRange,
                    QStringLiteral("Disposition plan components are empty or exceed their bound"));
    }
    QSet<QString> targets;
    QJsonArray components;
    for (const auto& component : plan.components) {
        const auto target = QString::fromUtf8(component.issue_id.value) + u'\n' +
                            QString::fromUtf8(component.target_id.value);
        if (targets.contains(target)) {
            return fail(WorkflowCodecErrorCode::InvalidField,
                        QStringLiteral("Disposition plan repeats an issue/target pair"));
        }
        targets.insert(target);
        const auto encoded = encodeDispositionComponent(component);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        components.append(*encoded);
    }
    return QJsonObject{{QStringLiteral("components"), components},
                       {QStringLiteral("digest"), *digest},
                       {QStringLiteral("finality"), *finality},
                       {QStringLiteral("plan_id"), *plan_id}};
}

[[nodiscard]] auto decodeDispositionPlan(const QJsonObject& object, QStringView context)
    -> std::expected<model::DispositionPlan, WorkflowCodecError> {
    if (const auto keys =
            exactKeys(object, {u"components", u"digest", u"finality", u"plan_id"}, context);
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto plan_id = decodeId<model::DispositionPlanId>(object, u"plan_id", context);
    const auto finality = decodeDispositionFinality(object, context);
    const auto digest = readDigest(object, u"digest", context);
    const auto components_value = object.value(u"components");
    if (!plan_id) {
        return std::unexpected(plan_id.error());
    }
    if (!finality) {
        return std::unexpected(finality.error());
    }
    if (!digest) {
        return std::unexpected(digest.error());
    }
    if (!components_value.isArray() || components_value.toArray().isEmpty() ||
        components_value.toArray().size() > maximum_disposition_components) {
        return fail(WorkflowCodecErrorCode::OutOfRange,
                    QStringLiteral("%1.components is empty or exceeds its bound").arg(context));
    }
    const auto array = components_value.toArray();
    QSet<QString> targets;
    std::vector<model::DispositionComponent> components;
    components.reserve(static_cast<std::size_t>(array.size()));
    for (qsizetype index = 0; index < array.size(); ++index) {
        if (!array.at(index).isObject()) {
            return fail(
                WorkflowCodecErrorCode::InvalidField,
                QStringLiteral("%1.components[%2] must be an object").arg(context).arg(index));
        }
        const auto decoded =
            decodeDispositionComponent(array.at(index).toObject(),
                                       QStringLiteral("%1.components[%2]").arg(context).arg(index));
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        const auto target = QString::fromUtf8(decoded->issue_id.value) + u'\n' +
                            QString::fromUtf8(decoded->target_id.value);
        if (targets.contains(target)) {
            return fail(WorkflowCodecErrorCode::InvalidField,
                        QStringLiteral("Disposition plan repeats an issue/target pair"));
        }
        targets.insert(target);
        components.push_back(*decoded);
    }
    return model::DispositionPlan{*plan_id, *finality, *digest, std::move(components)};
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

[[nodiscard]] auto encodeCommandPayload(const model::CalculateWorkflowDeadline& command)
    -> std::expected<QJsonObject, WorkflowCodecError> {
    auto payload = encodeCommandHeader(command.header);
    const auto operation_id = checkedId(command.operation_id.value, u"payload.operation_id");
    const auto deadline_id = checkedId(command.deadline_id.value, u"payload.deadline_id");
    if (!payload) {
        return std::unexpected(payload.error());
    }
    if (!operation_id) {
        return std::unexpected(operation_id.error());
    }
    if (!deadline_id) {
        return std::unexpected(deadline_id.error());
    }
    payload->insert(QStringLiteral("deadline_id"), *deadline_id);
    payload->insert(QStringLiteral("operation_id"), *operation_id);
    return payload;
}

[[nodiscard]] auto encodeCommandPayload(const model::AdvanceWorkflowStage& command)
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
    QJsonValue disposition;
    if (const auto* legacy = std::get_if<std::string>(&command.disposition)) {
        if (!roundTripsUtf8(*legacy)) {
            return fail(WorkflowCodecErrorCode::InvalidField,
                        QStringLiteral("payload.disposition is not valid UTF-8"));
        }
        const auto text = QString::fromUtf8(*legacy);
        if (text.isEmpty() || text.toUtf8().size() > maximum_text_characters ||
            containsNull(text)) {
            return fail(WorkflowCodecErrorCode::OutOfRange,
                        QStringLiteral("payload.disposition is empty or exceeds its bound"));
        }
        disposition = text;
    } else {
        const auto plan_id =
            checkedId(std::get<model::DispositionPlanId>(command.disposition).value,
                      u"payload.disposition.plan_id");
        if (!plan_id) {
            return std::unexpected(plan_id.error());
        }
        disposition = QJsonObject{{QStringLiteral("plan_id"), *plan_id}};
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

[[nodiscard]] auto decodeCalculateDeadline(const QJsonObject& payload)
    -> std::expected<model::WorkflowCommand, WorkflowCodecError> {
    if (const auto keys = exactKeys(payload,
                                    {u"actor_id", u"command_id", u"deadline_id", u"occurred_at",
                                     u"operation_id", u"session_id"},
                                    u"payload");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto header = decodeCommandHeader(payload);
    const auto operation_id =
        decodeId<model::WorkflowOperationId>(payload, u"operation_id", u"payload");
    const auto deadline_id =
        decodeId<model::WorkflowDeadlineId>(payload, u"deadline_id", u"payload");
    if (!header) {
        return std::unexpected(header.error());
    }
    if (!operation_id) {
        return std::unexpected(operation_id.error());
    }
    if (!deadline_id) {
        return std::unexpected(deadline_id.error());
    }
    return model::CalculateWorkflowDeadline{*header, *operation_id, *deadline_id};
}

[[nodiscard]] auto decodeAdvanceStage(const QJsonObject& payload)
    -> std::expected<model::WorkflowCommand, WorkflowCodecError> {
    if (const auto keys = exactKeys(
            payload, {u"actor_id", u"command_id", u"occurred_at", u"operation_id", u"session_id"},
            u"payload");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto header = decodeCommandHeader(payload);
    const auto operation_id =
        decodeId<model::WorkflowOperationId>(payload, u"operation_id", u"payload");
    if (!header) {
        return std::unexpected(header.error());
    }
    if (!operation_id) {
        return std::unexpected(operation_id.error());
    }
    return model::AdvanceWorkflowStage{*header, *operation_id};
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
    if (!header) {
        return std::unexpected(header.error());
    }
    if (!operation_id) {
        return std::unexpected(operation_id.error());
    }
    if (!digest) {
        return std::unexpected(digest.error());
    }
    model::WorkflowJudgmentSelection disposition;
    const auto disposition_value = payload.value(u"disposition");
    if (disposition_value.isString()) {
        const auto legacy =
            readString(payload, u"disposition", maximum_text_characters, u"payload");
        if (!legacy) {
            return std::unexpected(legacy.error());
        }
        disposition = legacy->toUtf8().toStdString();
    } else if (disposition_value.isObject()) {
        const auto object = disposition_value.toObject();
        if (const auto keys = exactKeys(object, {u"plan_id"}, u"payload.disposition"); !keys) {
            return std::unexpected(keys.error());
        }
        const auto plan_id =
            decodeId<model::DispositionPlanId>(object, u"plan_id", u"payload.disposition");
        if (!plan_id) {
            return std::unexpected(plan_id.error());
        }
        disposition = *plan_id;
    } else {
        return fail(WorkflowCodecErrorCode::InvalidField,
                    QStringLiteral("payload.disposition must be text or a plan selection"));
    }
    return model::IssueWorkflowJudgment{*header, *operation_id, *digest, std::move(disposition)};
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
    const auto deadline_base_id =
        encodeOptionalId(event.deadline_base_id, u"payload.deadline_base_id");
    const auto produced_deadline_id =
        encodeOptionalId(event.produced_deadline_id, u"payload.produced_deadline_id");
    const auto deadline_event_base = encodeDeadlineEventBase(event.deadline_event_base);
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
    if (!deadline_base_id) {
        return std::unexpected(deadline_base_id.error());
    }
    if (!produced_deadline_id) {
        return std::unexpected(produced_deadline_id.error());
    }
    if (!deadline_event_base) {
        return std::unexpected(deadline_event_base.error());
    }
    const auto uses_snapshot = event.deadline_base_id.has_value() ||
                               event.produced_deadline_id.has_value() ||
                               event.deadline_event_base.has_value();
    if (uses_snapshot &&
        (!event.produced_deadline_id.has_value() ||
         event.deadline_id != *event.produced_deadline_id ||
         (event.deadline_base_id.has_value() && event.deadline_event_base.has_value()))) {
        return fail(WorkflowCodecErrorCode::InvalidField,
                    QStringLiteral("A deadline snapshot requires its exact named output"));
    }
    payload->insert(QStringLiteral("base_date"), *base_date);
    payload->insert(QStringLiteral("deadline_id"), *deadline_id);
    payload->insert(QStringLiteral("due_date"), *due_date);
    payload->insert(QStringLiteral("purpose"), *purpose);
    if (uses_snapshot) {
        payload->insert(QStringLiteral("deadline_base_id"), *deadline_base_id);
        payload->insert(QStringLiteral("deadline_event_base"), *deadline_event_base);
        payload->insert(QStringLiteral("produced_deadline_id"), *produced_deadline_id);
    }
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
    QJsonValue disposition;
    if (const auto* legacy = std::get_if<std::string>(&event.disposition)) {
        if (!roundTripsUtf8(*legacy)) {
            return fail(WorkflowCodecErrorCode::InvalidField,
                        QStringLiteral("payload.disposition is not valid UTF-8"));
        }
        const auto text = QString::fromUtf8(*legacy);
        if (text.isEmpty() || text.toUtf8().size() > maximum_text_characters ||
            containsNull(text)) {
            return fail(WorkflowCodecErrorCode::OutOfRange,
                        QStringLiteral("payload.disposition is empty or exceeds its bound"));
        }
        disposition = text;
    } else {
        const auto plan =
            encodeDispositionPlan(std::get<model::DispositionPlan>(event.disposition));
        if (!plan) {
            return std::unexpected(plan.error());
        }
        disposition = *plan;
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

[[nodiscard]] auto decodeDeadlineCalculated(const QJsonObject& payload, int schema_version)
    -> std::expected<model::WorkflowEvent, WorkflowCodecError> {
    const auto snapshot = schema_version == deadline_snapshot_schema_version;
    const auto keys =
        snapshot
            ? exactKeys(payload,
                        {u"authority", u"base_date", u"command_event_count", u"command_event_index",
                         u"command_id", u"deadline_base_id", u"deadline_event_base", u"deadline_id",
                         u"due_date", u"occurred_at", u"operation_id", u"produced_deadline_id",
                         u"purpose", u"sequence", u"session_id", u"workflow_id"},
                        u"payload")
            : exactKeys(payload,
                        {u"authority", u"base_date", u"command_event_count", u"command_event_index",
                         u"command_id", u"deadline_id", u"due_date", u"occurred_at",
                         u"operation_id", u"purpose", u"sequence", u"session_id", u"workflow_id"},
                        u"payload");
    if (!keys) {
        return std::unexpected(keys.error());
    }
    const auto header = decodeEventHeader(payload);
    const auto deadline_id =
        decodeId<model::WorkflowDeadlineId>(payload, u"deadline_id", u"payload");
    const auto purpose = decodeDeadlinePurpose(payload);
    const auto base_date = parseDate(payload, u"base_date", u"payload");
    const auto due_date = parseDate(payload, u"due_date", u"payload");
    const auto deadline_base_id =
        snapshot
            ? decodeOptionalId<model::WorkflowDeadlineId>(payload, u"deadline_base_id", u"payload")
            : std::expected<std::optional<model::WorkflowDeadlineId>, WorkflowCodecError>{
                  std::nullopt};
    const auto produced_deadline_id =
        snapshot ? decodeId<model::WorkflowDeadlineId>(payload, u"produced_deadline_id", u"payload")
                 : std::expected<model::WorkflowDeadlineId, WorkflowCodecError>{
                       model::WorkflowDeadlineId{}};
    const auto deadline_event_base =
        snapshot
            ? decodeDeadlineEventBase(payload)
            : std::expected<std::optional<model::WorkflowDeadlineEventBase>, WorkflowCodecError>{
                  std::nullopt};
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
    if (!deadline_base_id) {
        return std::unexpected(deadline_base_id.error());
    }
    if (!produced_deadline_id) {
        return std::unexpected(produced_deadline_id.error());
    }
    if (!deadline_event_base) {
        return std::unexpected(deadline_event_base.error());
    }
    if (snapshot && (*deadline_id != *produced_deadline_id ||
                     (deadline_base_id->has_value() && deadline_event_base->has_value()))) {
        return fail(WorkflowCodecErrorCode::InvalidField,
                    QStringLiteral("A deadline snapshot must match its named output"));
    }
    return model::WorkflowDeadlineCalculated{*header,
                                             *deadline_id,
                                             *purpose,
                                             *base_date,
                                             *due_date,
                                             *deadline_base_id,
                                             snapshot ? std::optional{*produced_deadline_id}
                                                      : std::nullopt,
                                             *deadline_event_base};
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
    model::WorkflowJudgmentDisposition disposition;
    const auto disposition_value = payload.value(u"disposition");
    if (disposition_value.isString()) {
        const auto legacy =
            readString(payload, u"disposition", maximum_text_characters, u"payload");
        if (!legacy) {
            return std::unexpected(legacy.error());
        }
        disposition = legacy->toUtf8().toStdString();
    } else if (disposition_value.isObject()) {
        const auto plan =
            decodeDispositionPlan(disposition_value.toObject(), u"payload.disposition");
        if (!plan) {
            return std::unexpected(plan.error());
        }
        disposition = std::move(*plan);
    } else {
        return fail(WorkflowCodecErrorCode::InvalidField,
                    QStringLiteral("payload.disposition must be text or a disposition plan"));
    }
    return model::WorkflowJudgmentIssued{*header, *digest, std::move(disposition), *next};
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

struct DecodedEnvelope final {
    QString type;
    QJsonObject payload;
    int version{};
};

[[nodiscard]] auto decodeEnvelope(QByteArrayView encoded, QStringView type_key,
                                  QStringView envelope_context, bool allow_provenance,
                                  bool allow_structured = false,
                                  bool allow_deadline_snapshot = false)
    -> std::expected<DecodedEnvelope, WorkflowCodecError> {
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
    if (!version.isDouble() ||
        (version.toDouble() != legacy_schema_version &&
         (!allow_provenance || version.toDouble() != provenance_schema_version) &&
         (!allow_structured || version.toDouble() != structured_schema_version) &&
         (!allow_deadline_snapshot || version.toDouble() != deadline_snapshot_schema_version))) {
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
    return DecodedEnvelope{*type, envelope.value(u"payload").toObject(), version.toInt()};
}

[[nodiscard]] auto encodeEnvelope(QString type, const QJsonObject& payload, QStringView type_key,
                                  int version = legacy_schema_version)
    -> std::expected<QByteArray, WorkflowCodecError> {
    const QJsonObject envelope{
        {type_key.toString(), std::move(type)},
        {QStringLiteral("payload"), payload},
        {QStringLiteral("schema_version"), version},
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
    const auto structured = std::visit(
        [](const auto& concrete) {
            using Command = std::remove_cvref_t<decltype(concrete)>;
            if constexpr (std::same_as<Command, model::IssueWorkflowJudgment>) {
                return std::holds_alternative<model::DispositionPlanId>(concrete.disposition);
            }
            return false;
        },
        command);
    return encodeEnvelope(workflowCommandType(command), *payload, u"command_type",
                          structured ? structured_schema_version : legacy_schema_version);
}

std::expected<model::WorkflowCommand, WorkflowCodecError>
decodeWorkflowCommand(QByteArrayView encoded) {
    const auto envelope = decodeEnvelope(encoded, u"command_type", u"command", false, true);
    if (!envelope) {
        return std::unexpected(envelope.error());
    }
    const auto& type = envelope->type;
    const auto& payload = envelope->payload;
    auto decoded = [&]() -> std::expected<model::WorkflowCommand, WorkflowCodecError> {
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
        if (type == QLatin1StringView(calculate_deadline_command_type)) {
            return decodeCalculateDeadline(payload);
        }
        if (type == QLatin1StringView(advance_stage_command_type)) {
            return decodeAdvanceStage(payload);
        }
        return fail(WorkflowCodecErrorCode::UnknownCommandType,
                    QStringLiteral("Unknown workflow command type %1").arg(type));
    }();
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    const auto structured = std::visit(
        [](const auto& concrete) {
            using Command = std::remove_cvref_t<decltype(concrete)>;
            if constexpr (std::same_as<Command, model::IssueWorkflowJudgment>) {
                return std::holds_alternative<model::DispositionPlanId>(concrete.disposition);
            }
            return false;
        },
        *decoded);
    const auto expected_version = structured ? structured_schema_version : legacy_schema_version;
    if (envelope->version != expected_version) {
        return fail(WorkflowCodecErrorCode::UnsupportedVersion,
                    QStringLiteral("Command schema version does not match its disposition form"));
    }
    return decoded;
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
            } else if constexpr (std::same_as<Command, model::IssueWorkflowMandate>) {
                return QString::fromLatin1(issue_mandate_type);
            } else if constexpr (std::same_as<Command, model::CalculateWorkflowDeadline>) {
                return QString::fromLatin1(calculate_deadline_command_type);
            } else {
                return QString::fromLatin1(advance_stage_command_type);
            }
        },
        command);
}

std::expected<QByteArray, WorkflowCodecError>
encodeWorkflowEvent(const model::WorkflowEvent& event) {
    const auto authority_version = authoritySchemaVersion(authorityOf(event));
    if (!authority_version) {
        return std::unexpected(authority_version.error());
    }
    const auto payload = std::visit(
        [](const auto& concrete) -> std::expected<QJsonObject, WorkflowCodecError> {
            return encodeEventPayload(concrete);
        },
        event);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    const auto structured_disposition = std::visit(
        [](const auto& concrete) {
            using Event = std::remove_cvref_t<decltype(concrete)>;
            if constexpr (std::same_as<Event, model::WorkflowJudgmentIssued>) {
                return std::holds_alternative<model::DispositionPlan>(concrete.disposition);
            }
            return false;
        },
        event);
    const auto deadline_snapshot = std::visit(
        [](const auto& concrete) {
            using Event = std::remove_cvref_t<decltype(concrete)>;
            if constexpr (std::same_as<Event, model::WorkflowDeadlineCalculated>) {
                return concrete.deadline_base_id.has_value() ||
                       concrete.produced_deadline_id.has_value() ||
                       concrete.deadline_event_base.has_value();
            }
            return false;
        },
        event);
    const auto& preconditions = std::visit(
        [](const auto& concrete) -> const std::vector<model::WorkflowPrecondition>& {
            return concrete.header.preconditions;
        },
        event);
    const auto structured = structured_disposition || !preconditions.empty();
    const auto extended_preconditions = usesExtendedPreconditions(preconditions);
    const auto extended = deadline_snapshot || extended_preconditions;
    if (extended_preconditions && !deadline_snapshot &&
        std::holds_alternative<model::WorkflowDeadlineCalculated>(event)) {
        return fail(WorkflowCodecErrorCode::InvalidField,
                    QStringLiteral("Schema-4 deadline events require exact named bindings"));
    }
    auto encoded_payload = *payload;
    if (structured || extended) {
        if (*authority_version != provenance_schema_version) {
            return fail(WorkflowCodecErrorCode::IncompleteAuthority,
                        QStringLiteral("Structured workflow events require canonical authorities"));
        }
        const auto encoded_preconditions = encodePreconditions(preconditions);
        if (!encoded_preconditions) {
            return std::unexpected(encoded_preconditions.error());
        }
        encoded_payload.insert(QStringLiteral("preconditions"), *encoded_preconditions);
    }
    return encodeEnvelope(workflowEventType(event), encoded_payload, u"event_type",
                          extended     ? deadline_snapshot_schema_version
                          : structured ? structured_schema_version
                                       : *authority_version);
}

std::expected<model::WorkflowEvent, WorkflowCodecError>
decodeWorkflowEvent(QByteArrayView encoded) {
    const auto envelope = decodeEnvelope(encoded, u"event_type", u"event", true, true, true);
    if (!envelope) {
        return std::unexpected(envelope.error());
    }
    const auto& type = envelope->type;
    auto payload = envelope->payload;
    std::vector<model::WorkflowPrecondition> preconditions;
    if (envelope->version == structured_schema_version ||
        envelope->version == deadline_snapshot_schema_version) {
        if (!payload.contains(u"preconditions")) {
            return fail(WorkflowCodecErrorCode::MissingField,
                        QStringLiteral("Missing payload.preconditions"));
        }
        const auto decoded_preconditions = decodePreconditions(
            payload.value(u"preconditions"), envelope->version == deadline_snapshot_schema_version);
        if (!decoded_preconditions) {
            return std::unexpected(decoded_preconditions.error());
        }
        preconditions = std::move(*decoded_preconditions);
        payload.remove(u"preconditions");
    }
    auto decoded = [&]() -> std::expected<model::WorkflowEvent, WorkflowCodecError> {
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
            return decodeDeadlineCalculated(payload, envelope->version);
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
    }();
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    std::visit([&](auto& concrete) { concrete.header.preconditions = std::move(preconditions); },
               *decoded);
    const auto authority_version = authoritySchemaVersion(authorityOf(*decoded));
    if (!authority_version) {
        return std::unexpected(authority_version.error());
    }
    const auto structured_disposition = std::visit(
        [](const auto& concrete) {
            using Event = std::remove_cvref_t<decltype(concrete)>;
            if constexpr (std::same_as<Event, model::WorkflowJudgmentIssued>) {
                return std::holds_alternative<model::DispositionPlan>(concrete.disposition);
            }
            return false;
        },
        *decoded);
    const auto structured =
        structured_disposition ||
        std::visit([](const auto& concrete) { return !concrete.header.preconditions.empty(); },
                   *decoded);
    const auto extended_preconditions = std::visit(
        [](const auto& concrete) {
            return usesExtendedPreconditions(concrete.header.preconditions);
        },
        *decoded);
    const auto deadline_snapshot = std::visit(
        [](const auto& concrete) {
            using Event = std::remove_cvref_t<decltype(concrete)>;
            if constexpr (std::same_as<Event, model::WorkflowDeadlineCalculated>) {
                return concrete.deadline_base_id.has_value() ||
                       concrete.produced_deadline_id.has_value() ||
                       concrete.deadline_event_base.has_value();
            }
            return false;
        },
        *decoded);
    const auto extended = deadline_snapshot || extended_preconditions;
    if ((extended && (envelope->version != deadline_snapshot_schema_version ||
                      *authority_version != provenance_schema_version)) ||
        (!extended && structured &&
         (envelope->version != structured_schema_version ||
          *authority_version != provenance_schema_version)) ||
        (!extended && !structured && *authority_version != envelope->version)) {
        return fail(
            WorkflowCodecErrorCode::IncompleteAuthority,
            QStringLiteral("Event schema version does not match its authority or feature form"));
    }
    return decoded;
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
