#include "bench_profile_codec.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>
#include <QStringConverter>
#include <QTemporaryFile>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace appellate::ui {
namespace {

constexpr auto supported_schema_version = 1;
constexpr qsizetype maximum_document_bytes = 1024 * 1024;
constexpr qsizetype maximum_id_characters = 160;
constexpr qsizetype maximum_display_name_characters = 128;
constexpr qsizetype maximum_jurisdictions = 64;
constexpr qsizetype maximum_issue_focus_items = 32;
constexpr int maximum_json_depth = 32;

[[nodiscard]] auto fail(BenchProfileErrorCode code, QString message)
    -> std::unexpected<BenchProfileError> {
    return std::unexpected(BenchProfileError{code, std::move(message)});
}

class RawJsonScanner final {
  public:
    explicit RawJsonScanner(QStringView input) : input_(input) {}

    [[nodiscard]] auto scan() -> std::expected<void, BenchProfileError> {
        skipWhitespace();
        if (const auto value = parseValue(0); !value) {
            return value;
        }
        skipWhitespace();
        if (position_ != input_.size()) {
            return invalid(QStringLiteral("Trailing content after JSON document"));
        }
        return {};
    }

  private:
    [[nodiscard]] auto invalid(QString message) const -> std::unexpected<BenchProfileError> {
        return fail(BenchProfileErrorCode::InvalidJson, std::move(message));
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

    [[nodiscard]] auto parseString() -> std::expected<QString, BenchProfileError> {
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
                return invalid(QStringLiteral("Unescaped JSON control character"));
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

    [[nodiscard]] auto parseObject(int depth) -> std::expected<void, BenchProfileError> {
        if (!consume(u'{')) {
            return invalid(QStringLiteral("Expected JSON object"));
        }
        skipWhitespace();
        if (consume(u'}')) {
            return {};
        }
        QSet<QString> keys;
        while (true) {
            const auto key = parseString();
            if (!key) {
                return std::unexpected(key.error());
            }
            if (keys.contains(*key)) {
                return fail(BenchProfileErrorCode::DuplicateJsonKey,
                            QStringLiteral("Duplicate JSON key '%1'").arg(*key));
            }
            keys.insert(*key);
            skipWhitespace();
            if (!consume(u':')) {
                return invalid(QStringLiteral("Expected colon after JSON key"));
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

    [[nodiscard]] auto parseArray(int depth) -> std::expected<void, BenchProfileError> {
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

    [[nodiscard]] auto parseNumber() -> std::expected<void, BenchProfileError> {
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

    [[nodiscard]] auto parseValue(int depth) -> std::expected<void, BenchProfileError> {
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

[[nodiscard]] auto exactKeys(const QJsonObject& object, std::initializer_list<QStringView> expected,
                             QStringView context) -> std::expected<void, BenchProfileError> {
    QSet<QString> expected_set;
    for (const auto key : expected) {
        expected_set.insert(key.toString());
        if (!object.contains(key)) {
            return fail(BenchProfileErrorCode::MissingField,
                        QStringLiteral("Missing %1.%2").arg(context, key));
        }
    }
    for (auto iterator = object.constBegin(); iterator != object.constEnd(); ++iterator) {
        if (!expected_set.contains(iterator.key())) {
            return fail(BenchProfileErrorCode::UnexpectedField,
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

[[nodiscard]] bool isNamespacedId(QStringView value) {
    static const QRegularExpression pattern(
        QStringLiteral(R"(^[a-z0-9]+(?:[.-][a-z0-9]+)+(?:[-.][a-z0-9]+)*$)"));
    return value.size() >= 3 && value.size() <= maximum_id_characters && !containsNull(value) &&
           pattern.matchView(value).hasMatch();
}

[[nodiscard]] bool isDisplayName(QStringView value) {
    return !value.trimmed().isEmpty() && value.size() <= maximum_display_name_characters &&
           !containsNull(value) && std::ranges::all_of(value, [](QChar character) {
               return character.isPrint() || character == u' ';
           });
}

[[nodiscard]] bool isUnitInterval(double value) {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

[[nodiscard]] auto readString(const QJsonObject& object, QStringView key, qsizetype maximum,
                              QStringView context) -> std::expected<QString, BenchProfileError> {
    const auto value = object.value(key);
    if (!value.isString()) {
        return fail(BenchProfileErrorCode::InvalidField,
                    QStringLiteral("%1.%2 must be a string").arg(context, key));
    }
    const auto text = value.toString();
    if (text.isEmpty() || text.size() > maximum || containsNull(text)) {
        return fail(BenchProfileErrorCode::OutOfRange,
                    QStringLiteral("%1.%2 is empty or exceeds its bound").arg(context, key));
    }
    return text;
}

[[nodiscard]] auto readId(const QJsonObject& object, QStringView key, QStringView context)
    -> std::expected<std::string, BenchProfileError> {
    const auto value = readString(object, key, maximum_id_characters, context);
    if (!value) {
        return std::unexpected(value.error());
    }
    if (!isNamespacedId(*value)) {
        return fail(BenchProfileErrorCode::InvalidField,
                    QStringLiteral("%1.%2 must be a namespaced ID").arg(context, key));
    }
    return value->toUtf8().toStdString();
}

[[nodiscard]] auto readUnit(const QJsonObject& object, QStringView key, QStringView context)
    -> std::expected<double, BenchProfileError> {
    const auto value = object.value(key);
    if (!value.isDouble() || !isUnitInterval(value.toDouble())) {
        return fail(BenchProfileErrorCode::OutOfRange,
                    QStringLiteral("%1.%2 must be between 0 and 1").arg(context, key));
    }
    return value.toDouble();
}

[[nodiscard]] auto parseRole(QStringView value) -> std::optional<model::CourtRole> {
    if (value == u"district") {
        return model::CourtRole::District;
    }
    if (value == u"appellate") {
        return model::CourtRole::Appellate;
    }
    return std::nullopt;
}

[[nodiscard]] auto encodeRole(model::CourtRole role) -> std::optional<QString> {
    switch (role) {
    case model::CourtRole::District:
        return QStringLiteral("district");
    case model::CourtRole::Appellate:
        return QStringLiteral("appellate");
    }
    return std::nullopt;
}

[[nodiscard]] auto parseRegister(QStringView value) -> std::optional<model::VoiceRegister> {
    if (value == u"plain") {
        return model::VoiceRegister::Plain;
    }
    if (value == u"formal") {
        return model::VoiceRegister::Formal;
    }
    if (value == u"technical") {
        return model::VoiceRegister::Technical;
    }
    return std::nullopt;
}

[[nodiscard]] auto encodeRegister(model::VoiceRegister value) -> std::optional<QString> {
    switch (value) {
    case model::VoiceRegister::Plain:
        return QStringLiteral("plain");
    case model::VoiceRegister::Formal:
        return QStringLiteral("formal");
    case model::VoiceRegister::Technical:
        return QStringLiteral("technical");
    }
    return std::nullopt;
}

[[nodiscard]] auto parseCadence(QStringView value) -> std::optional<model::VoiceCadence> {
    if (value == u"clipped") {
        return model::VoiceCadence::Clipped;
    }
    if (value == u"measured") {
        return model::VoiceCadence::Measured;
    }
    if (value == u"expansive") {
        return model::VoiceCadence::Expansive;
    }
    return std::nullopt;
}

[[nodiscard]] auto encodeCadence(model::VoiceCadence value) -> std::optional<QString> {
    switch (value) {
    case model::VoiceCadence::Clipped:
        return QStringLiteral("clipped");
    case model::VoiceCadence::Measured:
        return QStringLiteral("measured");
    case model::VoiceCadence::Expansive:
        return QStringLiteral("expansive");
    }
    return std::nullopt;
}

[[nodiscard]] auto filesystemPath(const QString& path) -> std::filesystem::path {
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(QFile::encodeName(path).toStdString());
#endif
}

} // namespace

auto BenchProfileCodec::validate(const model::JudgeProfile& profile)
    -> std::expected<void, BenchProfileError> {
    if (profile.profile_class != model::ProfileClass::FictionalComposite) {
        return fail(BenchProfileErrorCode::IncompatibleProfile,
                    QStringLiteral("Only fictional/composite profiles can be edited"));
    }
    if (!roundTripsUtf8(profile.id) ||
        !isNamespacedId(
            QString::fromUtf8(profile.id.data(), static_cast<qsizetype>(profile.id.size())))) {
        return fail(BenchProfileErrorCode::InvalidField,
                    QStringLiteral("Profile ID must be a namespaced ID"));
    }
    if (!roundTripsUtf8(profile.display_name) ||
        !isDisplayName(QString::fromUtf8(profile.display_name.data(),
                                         static_cast<qsizetype>(profile.display_name.size())))) {
        return fail(BenchProfileErrorCode::InvalidField,
                    QStringLiteral("Display name must be printable and at most 128 characters"));
    }

    const auto& roles = profile.compatibility.court_roles;
    if (roles.empty() || roles.size() > 2) {
        return fail(BenchProfileErrorCode::IncompatibleProfile,
                    QStringLiteral("Profile must support one or two court roles"));
    }
    QSet<int> role_set;
    for (const auto role : roles) {
        if (!encodeRole(role) || role_set.contains(static_cast<int>(role))) {
            return fail(BenchProfileErrorCode::IncompatibleProfile,
                        QStringLiteral("Court roles must be recognized and unique"));
        }
        role_set.insert(static_cast<int>(role));
    }

    const auto& jurisdictions = profile.compatibility.jurisdiction_ids;
    if (jurisdictions.empty() || jurisdictions.size() > maximum_jurisdictions) {
        return fail(BenchProfileErrorCode::IncompatibleProfile,
                    QStringLiteral("Profile must name 1 to 64 compatible jurisdictions"));
    }
    QSet<QString> jurisdiction_set;
    for (const auto& jurisdiction : jurisdictions) {
        if (!roundTripsUtf8(jurisdiction)) {
            return fail(BenchProfileErrorCode::InvalidField,
                        QStringLiteral("Jurisdiction ID is not valid UTF-8"));
        }
        const auto id =
            QString::fromUtf8(jurisdiction.data(), static_cast<qsizetype>(jurisdiction.size()));
        if (!isNamespacedId(id) || jurisdiction_set.contains(id)) {
            return fail(BenchProfileErrorCode::IncompatibleProfile,
                        QStringLiteral("Jurisdiction IDs must be namespaced and unique"));
        }
        jurisdiction_set.insert(id);
    }

    const auto& interaction = profile.interaction;
    const std::array values{
        interaction.directness,        interaction.formality,
        interaction.question_length,   interaction.interruption_frequency,
        interaction.follow_up_depth,   interaction.hypothetical_frequency,
        interaction.concession_recall, interaction.time_strictness,
        profile.voice.verbosity,       profile.voice.sentence_complexity,
    };
    if (!std::ranges::all_of(values, isUnitInterval)) {
        return fail(BenchProfileErrorCode::OutOfRange,
                    QStringLiteral("Interaction and voice values must be between 0 and 1"));
    }
    if (!encodeRegister(profile.voice.register_style) || !encodeCadence(profile.voice.cadence)) {
        return fail(BenchProfileErrorCode::IncompatibleProfile,
                    QStringLiteral("Voice register and cadence must use schema-v1 values"));
    }

    const auto& focus = interaction.issue_focus;
    if (focus.empty() || focus.size() > maximum_issue_focus_items) {
        return fail(BenchProfileErrorCode::IncompatibleProfile,
                    QStringLiteral("Profile must contain 1 to 32 issue-focus entries"));
    }
    QSet<QString> topic_set;
    for (const auto& item : focus) {
        if (!roundTripsUtf8(item.topic_id)) {
            return fail(BenchProfileErrorCode::InvalidField,
                        QStringLiteral("Issue topic ID is not valid UTF-8"));
        }
        const auto topic =
            QString::fromUtf8(item.topic_id.data(), static_cast<qsizetype>(item.topic_id.size()));
        if (!isNamespacedId(topic) || !isUnitInterval(item.weight) || topic_set.contains(topic)) {
            return fail(
                BenchProfileErrorCode::IncompatibleProfile,
                QStringLiteral("Issue topics must be unique namespaced IDs with bounded weights"));
        }
        topic_set.insert(topic);
    }
    return {};
}

auto BenchProfileCodec::encode(const model::JudgeProfile& profile)
    -> std::expected<QByteArray, BenchProfileError> {
    if (const auto valid = validate(profile); !valid) {
        return std::unexpected(valid.error());
    }

    QJsonArray roles;
    for (const auto role : profile.compatibility.court_roles) {
        roles.append(*encodeRole(role));
    }
    QJsonArray jurisdictions;
    for (const auto& jurisdiction : profile.compatibility.jurisdiction_ids) {
        jurisdictions.append(QString::fromUtf8(jurisdiction));
    }
    QJsonArray focus;
    for (const auto& item : profile.interaction.issue_focus) {
        focus.append(QJsonObject{
            {QStringLiteral("topic_id"), QString::fromUtf8(item.topic_id)},
            {QStringLiteral("weight"), item.weight},
        });
    }

    const auto& interaction = profile.interaction;
    const QJsonObject root{
        {QStringLiteral("compatibility"),
         QJsonObject{
             {QStringLiteral("court_roles"), roles},
             {QStringLiteral("jurisdiction_ids"), jurisdictions},
         }},
        {QStringLiteral("display_name"), QString::fromUtf8(profile.display_name)},
        {QStringLiteral("interaction"),
         QJsonObject{
             {QStringLiteral("concession_recall"), interaction.concession_recall},
             {QStringLiteral("directness"), interaction.directness},
             {QStringLiteral("follow_up_depth"), interaction.follow_up_depth},
             {QStringLiteral("formality"), interaction.formality},
             {QStringLiteral("hypothetical_frequency"), interaction.hypothetical_frequency},
             {QStringLiteral("interruption_frequency"), interaction.interruption_frequency},
             {QStringLiteral("issue_focus"), focus},
             {QStringLiteral("question_length"), interaction.question_length},
             {QStringLiteral("time_strictness"), interaction.time_strictness},
         }},
        {QStringLiteral("profile_class"), QStringLiteral("fictional_composite")},
        {QStringLiteral("resource_id"), QString::fromUtf8(profile.id)},
        {QStringLiteral("resource_kind"), QStringLiteral("judge_profile")},
        {QStringLiteral("schema_version"), supported_schema_version},
        {QStringLiteral("voice"),
         QJsonObject{
             {QStringLiteral("cadence"), *encodeCadence(profile.voice.cadence)},
             {QStringLiteral("register"), *encodeRegister(profile.voice.register_style)},
             {QStringLiteral("sentence_complexity"), profile.voice.sentence_complexity},
             {QStringLiteral("verbosity"), profile.voice.verbosity},
         }},
    };
    auto document = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (document.size() > maximum_document_bytes) {
        return fail(BenchProfileErrorCode::InputTooLarge,
                    QStringLiteral("Encoded profile exceeds the 1 MiB limit"));
    }
    return document;
}

auto BenchProfileCodec::decode(QByteArrayView document)
    -> std::expected<model::JudgeProfile, BenchProfileError> {
    if (document.empty()) {
        return fail(BenchProfileErrorCode::InvalidJson,
                    QStringLiteral("Profile document is empty"));
    }
    if (document.size() > maximum_document_bytes) {
        return fail(BenchProfileErrorCode::InputTooLarge,
                    QStringLiteral("Profile document exceeds the 1 MiB limit"));
    }

    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString raw_text = decoder.decode(document);
    if (decoder.hasError()) {
        return fail(BenchProfileErrorCode::InvalidJson,
                    QStringLiteral("Profile document is not valid UTF-8"));
    }
    if (const auto scanned = RawJsonScanner(raw_text).scan(); !scanned) {
        return std::unexpected(scanned.error());
    }

    QJsonParseError parse_error;
    const auto parsed =
        QJsonDocument::fromJson(QByteArray(document.data(), document.size()), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !parsed.isObject()) {
        return fail(
            BenchProfileErrorCode::InvalidJson,
            QStringLiteral("Profile must be one JSON object: %1").arg(parse_error.errorString()));
    }
    const auto root = parsed.object();
    if (const auto keys =
            exactKeys(root,
                      {u"schema_version", u"resource_kind", u"resource_id", u"display_name",
                       u"profile_class", u"compatibility", u"interaction", u"voice"},
                      u"profile");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto schema_value = root.value(u"schema_version");
    if (!schema_value.isDouble() || schema_value.toDouble() != supported_schema_version) {
        return fail(BenchProfileErrorCode::UnsupportedSchema,
                    QStringLiteral("Only judge-profile schema version 1 is supported"));
    }
    if (root.value(u"resource_kind").toString() != u"judge_profile") {
        return fail(BenchProfileErrorCode::InvalidField,
                    QStringLiteral("profile.resource_kind must be judge_profile"));
    }
    if (root.value(u"profile_class").toString() != u"fictional_composite") {
        return fail(BenchProfileErrorCode::IncompatibleProfile,
                    QStringLiteral("Only fictional/composite profiles can be imported"));
    }

    const auto id = readId(root, u"resource_id", u"profile");
    const auto display_name =
        readString(root, u"display_name", maximum_display_name_characters, u"profile");
    if (!id || !display_name) {
        return id ? std::unexpected(display_name.error()) : std::unexpected(id.error());
    }
    if (!isDisplayName(*display_name)) {
        return fail(BenchProfileErrorCode::InvalidField,
                    QStringLiteral("profile.display_name must be printable"));
    }

    if (!root.value(u"compatibility").isObject()) {
        return fail(BenchProfileErrorCode::InvalidField,
                    QStringLiteral("profile.compatibility must be an object"));
    }
    const auto compatibility = root.value(u"compatibility").toObject();
    if (const auto keys = exactKeys(compatibility, {u"court_roles", u"jurisdiction_ids"},
                                    u"profile.compatibility");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto role_values = compatibility.value(u"court_roles");
    const auto jurisdiction_values = compatibility.value(u"jurisdiction_ids");
    if (!role_values.isArray() || !jurisdiction_values.isArray()) {
        return fail(BenchProfileErrorCode::InvalidField,
                    QStringLiteral("Compatibility lists must be arrays"));
    }
    const auto role_array = role_values.toArray();
    const auto jurisdiction_array = jurisdiction_values.toArray();
    if (role_array.isEmpty() || role_array.size() > 2 || jurisdiction_array.isEmpty() ||
        jurisdiction_array.size() > maximum_jurisdictions) {
        return fail(BenchProfileErrorCode::OutOfRange,
                    QStringLiteral("Compatibility arrays exceed schema-v1 bounds"));
    }
    std::vector<model::CourtRole> roles;
    QSet<QString> role_set;
    roles.reserve(static_cast<std::size_t>(role_array.size()));
    for (const auto& value : role_array) {
        if (!value.isString()) {
            return fail(BenchProfileErrorCode::InvalidField,
                        QStringLiteral("Court role must be a string"));
        }
        const auto text = value.toString();
        const auto role = parseRole(text);
        if (!role || role_set.contains(text)) {
            return fail(BenchProfileErrorCode::IncompatibleProfile,
                        QStringLiteral("Court roles must be recognized and unique"));
        }
        role_set.insert(text);
        roles.push_back(*role);
    }
    std::vector<std::string> jurisdictions;
    QSet<QString> jurisdiction_set;
    jurisdictions.reserve(static_cast<std::size_t>(jurisdiction_array.size()));
    for (const auto& value : jurisdiction_array) {
        if (!value.isString() || !isNamespacedId(value.toString()) ||
            jurisdiction_set.contains(value.toString())) {
            return fail(BenchProfileErrorCode::IncompatibleProfile,
                        QStringLiteral("Jurisdiction IDs must be namespaced and unique"));
        }
        jurisdiction_set.insert(value.toString());
        jurisdictions.push_back(value.toString().toUtf8().toStdString());
    }

    if (!root.value(u"interaction").isObject()) {
        return fail(BenchProfileErrorCode::InvalidField,
                    QStringLiteral("profile.interaction must be an object"));
    }
    const auto interaction = root.value(u"interaction").toObject();
    if (const auto keys =
            exactKeys(interaction,
                      {u"directness", u"formality", u"question_length", u"interruption_frequency",
                       u"follow_up_depth", u"hypothetical_frequency", u"concession_recall",
                       u"time_strictness", u"issue_focus"},
                      u"profile.interaction");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto directness = readUnit(interaction, u"directness", u"profile.interaction");
    const auto formality = readUnit(interaction, u"formality", u"profile.interaction");
    const auto question_length = readUnit(interaction, u"question_length", u"profile.interaction");
    const auto interruption =
        readUnit(interaction, u"interruption_frequency", u"profile.interaction");
    const auto follow_up = readUnit(interaction, u"follow_up_depth", u"profile.interaction");
    const auto hypothetical =
        readUnit(interaction, u"hypothetical_frequency", u"profile.interaction");
    const auto concession = readUnit(interaction, u"concession_recall", u"profile.interaction");
    const auto time_strictness = readUnit(interaction, u"time_strictness", u"profile.interaction");
    if (!directness || !formality || !question_length || !interruption || !follow_up ||
        !hypothetical || !concession || !time_strictness) {
        const auto message = QStringLiteral("All interaction controls must be between 0 and 1");
        return fail(BenchProfileErrorCode::OutOfRange, message);
    }
    const auto focus_value = interaction.value(u"issue_focus");
    if (!focus_value.isArray()) {
        return fail(BenchProfileErrorCode::InvalidField,
                    QStringLiteral("profile.interaction.issue_focus must be an array"));
    }
    const auto focus_array = focus_value.toArray();
    if (focus_array.isEmpty() || focus_array.size() > maximum_issue_focus_items) {
        return fail(BenchProfileErrorCode::OutOfRange,
                    QStringLiteral("Issue focus must contain 1 to 32 entries"));
    }
    std::vector<model::IssueFocus> focus;
    QSet<QString> focus_set;
    focus.reserve(static_cast<std::size_t>(focus_array.size()));
    for (const auto& value : focus_array) {
        if (!value.isObject()) {
            return fail(BenchProfileErrorCode::InvalidField,
                        QStringLiteral("Issue focus entries must be objects"));
        }
        const auto item = value.toObject();
        if (const auto keys =
                exactKeys(item, {u"topic_id", u"weight"}, u"profile.interaction.issue_focus[]");
            !keys) {
            return std::unexpected(keys.error());
        }
        const auto topic_id = readId(item, u"topic_id", u"profile.interaction.issue_focus[]");
        const auto weight = readUnit(item, u"weight", u"profile.interaction.issue_focus[]");
        if (!topic_id || !weight) {
            return topic_id ? std::unexpected(weight.error()) : std::unexpected(topic_id.error());
        }
        const auto topic_text = QString::fromUtf8(*topic_id);
        if (focus_set.contains(topic_text)) {
            return fail(BenchProfileErrorCode::IncompatibleProfile,
                        QStringLiteral("Issue-focus topic IDs must be unique"));
        }
        focus_set.insert(topic_text);
        focus.push_back(model::IssueFocus{*topic_id, *weight});
    }

    if (!root.value(u"voice").isObject()) {
        return fail(BenchProfileErrorCode::InvalidField,
                    QStringLiteral("profile.voice must be an object"));
    }
    const auto voice = root.value(u"voice").toObject();
    if (const auto keys =
            exactKeys(voice, {u"register", u"cadence", u"verbosity", u"sentence_complexity"},
                      u"profile.voice");
        !keys) {
        return std::unexpected(keys.error());
    }
    if (!voice.value(u"register").isString() || !voice.value(u"cadence").isString()) {
        return fail(BenchProfileErrorCode::InvalidField,
                    QStringLiteral("Voice register and cadence must be strings"));
    }
    const auto register_style = parseRegister(voice.value(u"register").toString());
    const auto cadence = parseCadence(voice.value(u"cadence").toString());
    const auto verbosity = readUnit(voice, u"verbosity", u"profile.voice");
    const auto sentence_complexity = readUnit(voice, u"sentence_complexity", u"profile.voice");
    if (!register_style || !cadence || !verbosity || !sentence_complexity) {
        return fail(BenchProfileErrorCode::IncompatibleProfile,
                    QStringLiteral("Voice controls must use schema-v1 values"));
    }

    model::JudgeProfile result{
        *id,
        display_name->toUtf8().toStdString(),
        model::ProfileClass::FictionalComposite,
        model::ProfileCompatibility{std::move(roles), std::move(jurisdictions)},
        model::InteractionStyle{
            *directness,
            *formality,
            *question_length,
            *interruption,
            *follow_up,
            *hypothetical,
            *concession,
            *time_strictness,
            std::move(focus),
        },
        model::VoiceStyle{*register_style, *cadence, *verbosity, *sentence_complexity},
    };
    if (const auto valid = validate(result); !valid) {
        return std::unexpected(valid.error());
    }
    return result;
}

auto BenchProfileCodec::importFile(QStringView path)
    -> std::expected<model::JudgeProfile, BenchProfileError> {
    const auto file_path = path.toString();
    if (file_path.isEmpty() || containsNull(file_path)) {
        return fail(BenchProfileErrorCode::UnsafePath,
                    QStringLiteral("Profile path is empty or contains a null character"));
    }
    const QFileInfo file_info(file_path);
    if (file_info.isSymLink() || !file_info.isFile()) {
        return fail(BenchProfileErrorCode::UnsafePath,
                    QStringLiteral("Profile input must be a regular, non-symbolic-link file"));
    }
    if (file_info.size() < 0 || file_info.size() > maximum_document_bytes) {
        return fail(BenchProfileErrorCode::InputTooLarge,
                    QStringLiteral("Profile document exceeds the 1 MiB limit"));
    }
    QFile input(file_path);
    if (!input.open(QIODevice::ReadOnly)) {
        return fail(BenchProfileErrorCode::CannotRead,
                    QStringLiteral("Cannot open profile input: %1").arg(input.errorString()));
    }
    const auto contents = input.read(maximum_document_bytes + 1);
    if (input.error() != QFileDevice::NoError) {
        return fail(BenchProfileErrorCode::CannotRead,
                    QStringLiteral("Cannot read profile input: %1").arg(input.errorString()));
    }
    if (contents.size() > maximum_document_bytes || !input.atEnd()) {
        return fail(BenchProfileErrorCode::InputTooLarge,
                    QStringLiteral("Profile document exceeds the 1 MiB limit"));
    }
    return decode(contents);
}

auto BenchProfileCodec::exportFile(const model::JudgeProfile& profile, QStringView path)
    -> std::expected<void, BenchProfileError> {
    const auto encoded = encode(profile);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    const auto requested_path = path.toString();
    if (requested_path.isEmpty() || containsNull(requested_path)) {
        return fail(BenchProfileErrorCode::UnsafePath,
                    QStringLiteral("Profile path is empty or contains a null character"));
    }

    const QFileInfo requested_info(requested_path);
    if (requested_info.exists() || requested_info.isSymLink()) {
        return fail(BenchProfileErrorCode::AlreadyExists,
                    QStringLiteral("Profile export never overwrites an existing path"));
    }
    const auto absolute_path = requested_info.absoluteFilePath();
    const QFileInfo parent_info(requested_info.absolutePath());
    if (!parent_info.isDir() || parent_info.isSymLink()) {
        return fail(BenchProfileErrorCode::UnsafePath,
                    QStringLiteral("Profile export parent must be a real directory"));
    }

    QTemporaryFile staging(
        QDir(parent_info.absoluteFilePath()).filePath(QStringLiteral(".profile-XXXXXX.tmp")));
    if (!staging.open()) {
        return fail(BenchProfileErrorCode::CannotWrite,
                    QStringLiteral("Cannot create atomic profile staging file: %1")
                        .arg(staging.errorString()));
    }
    if (staging.write(*encoded) != encoded->size() || !staging.flush()) {
        return fail(
            BenchProfileErrorCode::CannotWrite,
            QStringLiteral("Cannot write profile staging file: %1").arg(staging.errorString()));
    }
    staging.close();

    std::error_code link_error;
    std::filesystem::create_hard_link(filesystemPath(staging.fileName()),
                                      filesystemPath(absolute_path), link_error);
    if (link_error) {
        const QFileInfo raced_destination(absolute_path);
        if (raced_destination.exists() || raced_destination.isSymLink()) {
            return fail(BenchProfileErrorCode::AlreadyExists,
                        QStringLiteral("Profile export never overwrites an existing path"));
        }
        return fail(BenchProfileErrorCode::CannotWrite,
                    QStringLiteral("Cannot atomically publish profile: %1")
                        .arg(QString::fromStdString(link_error.message())));
    }
    return {};
}

} // namespace appellate::ui
