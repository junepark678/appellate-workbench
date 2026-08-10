#include "appellate/packs/schema_validator.hpp"

#include <QDate>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QRegularExpression>
#include <QResource>
#include <QSet>
#include <QStringList>
#include <QUrl>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <utility>

static void initializeAppellateSchemaResources() {
    static const bool initialized = [] {
        Q_INIT_RESOURCE(schemas);
        return true;
    }();
    static_cast<void>(initialized);
}

namespace appellate::packs {
namespace {

constexpr qsizetype maximum_schema_bytes = 1024 * 1024;
constexpr qsizetype maximum_schema_recursion = 128;

// Keep each generation's registry independent even while the current file
// names happen to match. A future v2-only kind must never make the v1 loader
// require or expose its schema.
constexpr std::array<const char*, 14> v1_schema_files{
    "argument-config.schema.json",
    "authority-set.schema.json",
    "bench-configuration.schema.json",
    "case.schema.json",
    "common.schema.json",
    "court.schema.json",
    "filing-catalog.schema.json",
    "form.schema.json",
    "judge-profile.schema.json",
    "manifest.schema.json",
    "procedure-profile.schema.json",
    "realism-review.schema.json",
    "record.schema.json",
    "workflow.schema.json",
};

constexpr std::array<const char*, 14> v2_schema_files{
    "argument-config.schema.json",
    "authority-set.schema.json",
    "bench-configuration.schema.json",
    "case.schema.json",
    "common.schema.json",
    "court.schema.json",
    "filing-catalog.schema.json",
    "form.schema.json",
    "judge-profile.schema.json",
    "manifest.schema.json",
    "procedure-profile.schema.json",
    "realism-review.schema.json",
    "record.schema.json",
    "workflow.schema.json",
};

[[nodiscard]] std::span<const char* const> schemaFiles(std::uint32_t schema_version) {
    if (schema_version == 1) {
        return v1_schema_files;
    }
    if (schema_version == 2) {
        return v2_schema_files;
    }
    return {};
}

[[nodiscard]] auto fail(ErrorCode code, QString message) -> std::unexpected<Error> {
    return std::unexpected(Error{code, std::move(message)});
}

class JsonLexicalScanner final {
  public:
    JsonLexicalScanner(QByteArrayView bytes, QStringView source_name, JsonLimits limits)
        : bytes_(bytes), source_name_(source_name.toString()), limits_(limits) {}

    [[nodiscard]] std::expected<void, Error> scan() {
        skipWhitespace();
        if (!scanValue(0)) {
            return std::unexpected(error_);
        }
        skipWhitespace();
        if (position_ != bytes_.size()) {
            return invalid(QStringLiteral("Trailing data"));
        }
        return {};
    }

  private:
    [[nodiscard]] std::expected<void, Error> invalid(QString detail) {
        return fail(ErrorCode::InvalidJson, QStringLiteral("Invalid JSON in %1 at byte %2: %3")
                                                .arg(source_name_)
                                                .arg(position_)
                                                .arg(std::move(detail)));
    }

    bool setInvalid(QString detail) {
        error_ = Error{ErrorCode::InvalidJson, QStringLiteral("Invalid JSON in %1 at byte %2: %3")
                                                   .arg(source_name_)
                                                   .arg(position_)
                                                   .arg(std::move(detail))};
        return false;
    }

    [[nodiscard]] bool setDuplicate(const QString& key) {
        error_ = Error{ErrorCode::DuplicateJsonKey,
                       QStringLiteral("Duplicate JSON key '%1' in %2 at byte %3")
                           .arg(key, source_name_)
                           .arg(position_)};
        return false;
    }

    void skipWhitespace() {
        while (position_ < bytes_.size()) {
            const auto character = bytes_.at(position_);
            if (character != ' ' && character != '\t' && character != '\r' && character != '\n') {
                return;
            }
            ++position_;
        }
    }

    [[nodiscard]] bool scanValue(qsizetype depth) {
        if (depth > limits_.maximum_depth) {
            return setInvalid(QStringLiteral("Maximum nesting depth exceeded"));
        }
        ++values_;
        if (values_ > limits_.maximum_values) {
            return setInvalid(QStringLiteral("Maximum JSON value count exceeded"));
        }
        skipWhitespace();
        if (position_ >= bytes_.size()) {
            return setInvalid(QStringLiteral("Unexpected end of input"));
        }
        switch (bytes_.at(position_)) {
        case '{':
            return scanObject(depth + 1);
        case '[':
            return scanArray(depth + 1);
        case '"': {
            const auto token = scanStringToken();
            return token.has_value();
        }
        case 't':
            return scanLiteral(QByteArrayView("true"));
        case 'f':
            return scanLiteral(QByteArrayView("false"));
        case 'n':
            return scanLiteral(QByteArrayView("null"));
        default:
            return scanNumber();
        }
    }

    [[nodiscard]] bool scanObject(qsizetype depth) {
        ++position_;
        skipWhitespace();
        if (consume('}')) {
            return true;
        }

        QSet<QString> keys;
        while (position_ < bytes_.size()) {
            if (bytes_.at(position_) != '"') {
                return setInvalid(QStringLiteral("Object key must be a string"));
            }
            const auto token = scanStringToken();
            if (!token) {
                return false;
            }
            const QByteArray wrapper = QByteArray("[") + token->toByteArray() + QByteArray("]");
            QJsonParseError parse_error;
            const auto decoded = QJsonDocument::fromJson(wrapper, &parse_error);
            if (parse_error.error != QJsonParseError::NoError || !decoded.isArray() ||
                decoded.array().size() != 1 || !decoded.array().at(0).isString()) {
                return setInvalid(QStringLiteral("Invalid object key encoding"));
            }
            const auto key = decoded.array().at(0).toString();
            if (keys.contains(key)) {
                return setDuplicate(key);
            }
            keys.insert(key);

            skipWhitespace();
            if (!consume(':')) {
                return setInvalid(QStringLiteral("Expected ':' after object key"));
            }
            if (!scanValue(depth)) {
                return false;
            }
            skipWhitespace();
            if (consume('}')) {
                return true;
            }
            if (!consume(',')) {
                return setInvalid(QStringLiteral("Expected ',' or '}'"));
            }
            skipWhitespace();
        }
        return setInvalid(QStringLiteral("Unterminated object"));
    }

    [[nodiscard]] bool scanArray(qsizetype depth) {
        ++position_;
        skipWhitespace();
        if (consume(']')) {
            return true;
        }
        while (position_ < bytes_.size()) {
            if (!scanValue(depth)) {
                return false;
            }
            skipWhitespace();
            if (consume(']')) {
                return true;
            }
            if (!consume(',')) {
                return setInvalid(QStringLiteral("Expected ',' or ']'"));
            }
            skipWhitespace();
        }
        return setInvalid(QStringLiteral("Unterminated array"));
    }

    [[nodiscard]] std::optional<QByteArrayView> scanStringToken() {
        const auto begin = position_;
        ++position_;
        while (position_ < bytes_.size()) {
            const auto character = static_cast<unsigned char>(bytes_.at(position_++));
            if (character == '"') {
                return bytes_.sliced(begin, position_ - begin);
            }
            if (character < 0x20U) {
                setInvalid(QStringLiteral("Control character in string"));
                return std::nullopt;
            }
            if (character != '\\') {
                continue;
            }
            if (position_ >= bytes_.size()) {
                setInvalid(QStringLiteral("Unterminated string escape"));
                return std::nullopt;
            }
            const auto escaped = bytes_.at(position_++);
            if (escaped == '"' || escaped == '\\' || escaped == '/' || escaped == 'b' ||
                escaped == 'f' || escaped == 'n' || escaped == 'r' || escaped == 't') {
                continue;
            }
            if (escaped != 'u' || position_ + 4 > bytes_.size()) {
                setInvalid(QStringLiteral("Invalid string escape"));
                return std::nullopt;
            }
            for (qsizetype index = 0; index < 4; ++index) {
                const auto digit = bytes_.at(position_ + index);
                const auto hexadecimal = (digit >= '0' && digit <= '9') ||
                                         (digit >= 'a' && digit <= 'f') ||
                                         (digit >= 'A' && digit <= 'F');
                if (!hexadecimal) {
                    setInvalid(QStringLiteral("Invalid Unicode escape"));
                    return std::nullopt;
                }
            }
            position_ += 4;
        }
        setInvalid(QStringLiteral("Unterminated string"));
        return std::nullopt;
    }

    [[nodiscard]] bool scanLiteral(QByteArrayView literal) {
        if (position_ + literal.size() > bytes_.size() ||
            bytes_.sliced(position_, literal.size()) != literal) {
            return setInvalid(QStringLiteral("Invalid literal"));
        }
        position_ += literal.size();
        return true;
    }

    [[nodiscard]] bool scanNumber() {
        const auto begin = position_;
        consume('-');
        if (position_ >= bytes_.size()) {
            return setInvalid(QStringLiteral("Invalid number"));
        }
        if (consume('0')) {
            if (position_ < bytes_.size() && bytes_.at(position_) >= '0' &&
                bytes_.at(position_) <= '9') {
                return setInvalid(QStringLiteral("Leading zero in number"));
            }
        } else {
            if (bytes_.at(position_) < '1' || bytes_.at(position_) > '9') {
                return setInvalid(QStringLiteral("Invalid number"));
            }
            while (position_ < bytes_.size() && bytes_.at(position_) >= '0' &&
                   bytes_.at(position_) <= '9') {
                ++position_;
            }
        }
        if (consume('.')) {
            if (position_ >= bytes_.size() || bytes_.at(position_) < '0' ||
                bytes_.at(position_) > '9') {
                return setInvalid(QStringLiteral("Invalid fraction"));
            }
            while (position_ < bytes_.size() && bytes_.at(position_) >= '0' &&
                   bytes_.at(position_) <= '9') {
                ++position_;
            }
        }
        if (position_ < bytes_.size() &&
            (bytes_.at(position_) == 'e' || bytes_.at(position_) == 'E')) {
            ++position_;
            if (position_ < bytes_.size() &&
                (bytes_.at(position_) == '+' || bytes_.at(position_) == '-')) {
                ++position_;
            }
            if (position_ >= bytes_.size() || bytes_.at(position_) < '0' ||
                bytes_.at(position_) > '9') {
                return setInvalid(QStringLiteral("Invalid exponent"));
            }
            while (position_ < bytes_.size() && bytes_.at(position_) >= '0' &&
                   bytes_.at(position_) <= '9') {
                ++position_;
            }
        }
        return position_ > begin;
    }

    bool consume(char expected) {
        if (position_ >= bytes_.size() || bytes_.at(position_) != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    QByteArrayView bytes_;
    QString source_name_;
    JsonLimits limits_;
    qsizetype position_{};
    qsizetype values_{};
    Error error_{ErrorCode::InvalidJson, QStringLiteral("Invalid JSON")};
};

struct ResolvedSchema final {
    QJsonObject schema;
    QString file;
};

[[nodiscard]] QString decodePointerToken(QString token) {
    return token.replace(QStringLiteral("~1"), QStringLiteral("/"))
        .replace(QStringLiteral("~0"), QStringLiteral("~"));
}

[[nodiscard]] auto resolveReference(const QHash<QString, QJsonObject>& schemas,
                                    const QString& current_file, const QString& reference)
    -> std::expected<ResolvedSchema, Error> {
    const auto hash_index = reference.indexOf(u'#');
    const auto file = hash_index < 0 ? reference : reference.left(hash_index);
    const auto fragment = hash_index < 0 ? QString{} : reference.mid(hash_index + 1);
    const auto target_file = file.isEmpty() ? current_file : file;
    if (target_file.isEmpty() || target_file.contains(u'/') || target_file.contains(u'\\') ||
        target_file.contains(u':') || target_file == QStringLiteral(".") ||
        target_file == QStringLiteral("..") || !schemas.contains(target_file)) {
        return fail(ErrorCode::UnsupportedSchema,
                    QStringLiteral("Non-local or unknown schema reference '%1'").arg(reference));
    }

    const auto& root = schemas[target_file];
    QJsonValue current(root);
    if (!fragment.isEmpty()) {
        if (!fragment.startsWith(u'/')) {
            return fail(ErrorCode::UnsupportedSchema,
                        QStringLiteral("Unsupported schema fragment in '%1'").arg(reference));
        }
        const auto tokens = fragment.mid(1).split(u'/');
        for (const auto& encoded_token : tokens) {
            if (!current.isObject()) {
                return fail(ErrorCode::UnsupportedSchema,
                            QStringLiteral("Schema reference does not name an object: '%1'")
                                .arg(reference));
            }
            const auto token = decodePointerToken(encoded_token);
            const auto object = current.toObject();
            if (!object.contains(token)) {
                return fail(ErrorCode::UnsupportedSchema,
                            QStringLiteral("Unresolved schema reference '%1'").arg(reference));
            }
            current = object.value(token);
        }
    }
    if (!current.isObject()) {
        return fail(
            ErrorCode::UnsupportedSchema,
            QStringLiteral("Schema reference does not resolve to an object: '%1'").arg(reference));
    }

    return ResolvedSchema{current.toObject(), target_file};
}

[[nodiscard]] bool isSchemaKeyword(const QString& key) {
    static const QSet<QString> keywords{
        QStringLiteral("$schema"),
        QStringLiteral("$id"),
        QStringLiteral("$ref"),
        QStringLiteral("$defs"),
        QStringLiteral("title"),
        QStringLiteral("type"),
        QStringLiteral("const"),
        QStringLiteral("enum"),
        QStringLiteral("additionalProperties"),
        QStringLiteral("required"),
        QStringLiteral("properties"),
        QStringLiteral("minLength"),
        QStringLiteral("maxLength"),
        QStringLiteral("pattern"),
        QStringLiteral("format"),
        QStringLiteral("minItems"),
        QStringLiteral("maxItems"),
        QStringLiteral("uniqueItems"),
        QStringLiteral("items"),
        QStringLiteral("minimum"),
        QStringLiteral("maximum"),
    };
    return keywords.contains(key);
}

[[nodiscard]] auto verifySchemaShape(const QHash<QString, QJsonObject>& schemas,
                                     const QString& file, const QJsonObject& schema,
                                     qsizetype depth) -> std::expected<void, Error> {
    if (depth > maximum_schema_recursion) {
        return fail(ErrorCode::UnsupportedSchema,
                    QStringLiteral("Schema nesting is too deep in %1").arg(file));
    }
    for (auto iterator = schema.constBegin(); iterator != schema.constEnd(); ++iterator) {
        if (!isSchemaKeyword(iterator.key())) {
            return fail(ErrorCode::UnsupportedSchema,
                        QStringLiteral("Unsupported JSON Schema keyword '%1' in %2")
                            .arg(iterator.key(), file));
        }
    }
    if (schema.contains(QStringLiteral("$ref"))) {
        if (!schema.value(QStringLiteral("$ref")).isString()) {
            return fail(ErrorCode::UnsupportedSchema,
                        QStringLiteral("$ref must be a string in %1").arg(file));
        }
        const auto resolved =
            resolveReference(schemas, file, schema.value(QStringLiteral("$ref")).toString());
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
    }
    for (const auto& container_name : {QStringLiteral("$defs"), QStringLiteral("properties")}) {
        if (!schema.contains(container_name)) {
            continue;
        }
        if (!schema.value(container_name).isObject()) {
            return fail(ErrorCode::UnsupportedSchema,
                        QStringLiteral("%1 must be an object in %2").arg(container_name, file));
        }
        const auto children = schema.value(container_name).toObject();
        for (auto child = children.constBegin(); child != children.constEnd(); ++child) {
            if (!child.value().isObject()) {
                return fail(ErrorCode::UnsupportedSchema,
                            QStringLiteral("Boolean schemas are not supported in %1").arg(file));
            }
            const auto verified =
                verifySchemaShape(schemas, file, child.value().toObject(), depth + 1);
            if (!verified) {
                return verified;
            }
        }
    }
    if (schema.contains(QStringLiteral("items"))) {
        if (!schema.value(QStringLiteral("items")).isObject()) {
            return fail(
                ErrorCode::UnsupportedSchema,
                QStringLiteral("Tuple and boolean schemas are not supported in %1").arg(file));
        }
        return verifySchemaShape(schemas, file, schema.value(QStringLiteral("items")).toObject(),
                                 depth + 1);
    }
    return {};
}

[[nodiscard]] QString childPointer(const QString& parent, const QString& token) {
    auto escaped = token;
    escaped.replace(QStringLiteral("~"), QStringLiteral("~0"));
    escaped.replace(QStringLiteral("/"), QStringLiteral("~1"));
    return parent + u'/' + escaped;
}

[[nodiscard]] auto violation(const QString& schema_file, const QString& instance_path,
                             QString detail) -> std::unexpected<Error> {
    return fail(ErrorCode::SchemaViolation,
                QStringLiteral("%1 violates %2 at %3: %4")
                    .arg(QStringLiteral("Resource"), schema_file,
                         instance_path.isEmpty() ? QStringLiteral("/") : instance_path,
                         std::move(detail)));
}

[[nodiscard]] bool isExactInteger(const QJsonValue& value) {
    if (!value.isDouble()) {
        return false;
    }
    const auto number = value.toDouble();
    return std::isfinite(number) && std::floor(number) == number;
}

[[nodiscard]] std::optional<qsizetype> unicodeScalarCount(QStringView text) {
    qsizetype count = 0;
    for (qsizetype index = 0; index < text.size(); ++index) {
        const auto unit = text.at(index).unicode();
        if (unit >= 0xD800U && unit <= 0xDBFFU) {
            if (index + 1 >= text.size()) {
                return std::nullopt;
            }
            const auto low = text.at(index + 1).unicode();
            if (low < 0xDC00U || low > 0xDFFFU) {
                return std::nullopt;
            }
            ++index;
        } else if (unit >= 0xDC00U && unit <= 0xDFFFU) {
            return std::nullopt;
        }
        ++count;
    }
    return count;
}

[[nodiscard]] auto validateNode(const QHash<QString, QJsonObject>& schemas,
                                const QString& schema_file, const QJsonObject& schema,
                                const QJsonValue& instance, const QString& path, qsizetype depth)
    -> std::expected<void, Error> {
    if (depth > maximum_schema_recursion) {
        return fail(
            ErrorCode::UnsupportedSchema,
            QStringLiteral("Schema reference recursion is too deep in %1").arg(schema_file));
    }

    if (schema.contains(QStringLiteral("$ref"))) {
        const auto resolved =
            resolveReference(schemas, schema_file, schema.value(QStringLiteral("$ref")).toString());
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        const auto result =
            validateNode(schemas, resolved->file, resolved->schema, instance, path, depth + 1);
        if (!result) {
            return result;
        }
    }

    if (schema.contains(QStringLiteral("type"))) {
        const auto type_value = schema.value(QStringLiteral("type"));
        if (!type_value.isString()) {
            return fail(ErrorCode::UnsupportedSchema,
                        QStringLiteral("Only single-string type declarations are supported in %1")
                            .arg(schema_file));
        }
        const auto type = type_value.toString();
        const auto matches = (type == QStringLiteral("object") && instance.isObject()) ||
                             (type == QStringLiteral("array") && instance.isArray()) ||
                             (type == QStringLiteral("string") && instance.isString()) ||
                             (type == QStringLiteral("boolean") && instance.isBool()) ||
                             (type == QStringLiteral("number") && instance.isDouble()) ||
                             (type == QStringLiteral("integer") && isExactInteger(instance)) ||
                             (type == QStringLiteral("null") && instance.isNull());
        static const QSet<QString> supported_types{
            QStringLiteral("object"),  QStringLiteral("array"),  QStringLiteral("string"),
            QStringLiteral("boolean"), QStringLiteral("number"), QStringLiteral("integer"),
            QStringLiteral("null"),
        };
        if (!supported_types.contains(type)) {
            return fail(ErrorCode::UnsupportedSchema,
                        QStringLiteral("Unsupported JSON type '%1' in %2").arg(type, schema_file));
        }
        if (!matches) {
            return violation(schema_file, path, QStringLiteral("expected type %1").arg(type));
        }
    }

    if (schema.contains(QStringLiteral("const")) &&
        instance != schema.value(QStringLiteral("const"))) {
        return violation(schema_file, path, QStringLiteral("value does not match const"));
    }
    if (schema.contains(QStringLiteral("enum"))) {
        const auto enum_value = schema.value(QStringLiteral("enum"));
        if (!enum_value.isArray()) {
            return fail(ErrorCode::UnsupportedSchema,
                        QStringLiteral("enum must be an array in %1").arg(schema_file));
        }
        const auto choices = enum_value.toArray();
        if (std::ranges::none_of(
                choices, [&instance](const QJsonValue& choice) { return choice == instance; })) {
            return violation(schema_file, path, QStringLiteral("value is not in enum"));
        }
    }

    if (instance.isObject()) {
        const auto object = instance.toObject();
        if (schema.contains(QStringLiteral("required"))) {
            const auto required_value = schema.value(QStringLiteral("required"));
            if (!required_value.isArray()) {
                return fail(ErrorCode::UnsupportedSchema,
                            QStringLiteral("required must be an array in %1").arg(schema_file));
            }
            QSet<QString> required_names;
            for (const auto& name_value : required_value.toArray()) {
                if (!name_value.isString() || required_names.contains(name_value.toString())) {
                    return fail(ErrorCode::UnsupportedSchema,
                                QStringLiteral("required names must be unique strings in %1")
                                    .arg(schema_file));
                }
                required_names.insert(name_value.toString());
                if (!object.contains(name_value.toString())) {
                    return violation(schema_file, path,
                                     QStringLiteral("missing required property '%1'")
                                         .arg(name_value.toString()));
                }
            }
        }
        QJsonObject properties;
        if (schema.contains(QStringLiteral("properties"))) {
            properties = schema.value(QStringLiteral("properties")).toObject();
        }
        if (schema.contains(QStringLiteral("additionalProperties"))) {
            const auto additional = schema.value(QStringLiteral("additionalProperties"));
            if (!additional.isBool()) {
                return fail(ErrorCode::UnsupportedSchema,
                            QStringLiteral("Only boolean additionalProperties is supported in %1")
                                .arg(schema_file));
            }
            if (!additional.toBool()) {
                for (auto item = object.constBegin(); item != object.constEnd(); ++item) {
                    if (!properties.contains(item.key())) {
                        return violation(schema_file, childPointer(path, item.key()),
                                         QStringLiteral("unknown property"));
                    }
                }
            }
        }
        for (auto property = properties.constBegin(); property != properties.constEnd();
             ++property) {
            if (!object.contains(property.key())) {
                continue;
            }
            const auto result = validateNode(schemas, schema_file, property.value().toObject(),
                                             object.value(property.key()),
                                             childPointer(path, property.key()), depth + 1);
            if (!result) {
                return result;
            }
        }
    }

    if (instance.isArray()) {
        const auto array = instance.toArray();
        for (const auto& keyword : {QStringLiteral("minItems"), QStringLiteral("maxItems")}) {
            if (!schema.contains(keyword)) {
                continue;
            }
            const auto bound = schema.value(keyword);
            if (!isExactInteger(bound) || bound.toDouble() < 0) {
                return fail(ErrorCode::UnsupportedSchema,
                            QStringLiteral("%1 must be a non-negative integer in %2")
                                .arg(keyword, schema_file));
            }
            const auto size = static_cast<double>(array.size());
            if ((keyword == QStringLiteral("minItems") && size < bound.toDouble()) ||
                (keyword == QStringLiteral("maxItems") && size > bound.toDouble())) {
                return violation(schema_file, path,
                                 QStringLiteral("array violates %1").arg(keyword));
            }
        }
        if (schema.contains(QStringLiteral("uniqueItems"))) {
            const auto unique = schema.value(QStringLiteral("uniqueItems"));
            if (!unique.isBool()) {
                return fail(ErrorCode::UnsupportedSchema,
                            QStringLiteral("uniqueItems must be boolean in %1").arg(schema_file));
            }
            if (unique.toBool()) {
                QSet<QByteArray> seen;
                for (const auto& value : array) {
                    const auto canonical =
                        QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
                    if (seen.contains(canonical)) {
                        return violation(schema_file, path,
                                         QStringLiteral("array items must be unique"));
                    }
                    seen.insert(canonical);
                }
            }
        }
        if (schema.contains(QStringLiteral("items"))) {
            const auto item_schema = schema.value(QStringLiteral("items")).toObject();
            for (qsizetype index = 0; index < array.size(); ++index) {
                const auto result =
                    validateNode(schemas, schema_file, item_schema, array.at(index),
                                 childPointer(path, QString::number(index)), depth + 1);
                if (!result) {
                    return result;
                }
            }
        }
    }

    if (instance.isString()) {
        const auto string = instance.toString();
        const auto scalar_count = unicodeScalarCount(string);
        if (!scalar_count.has_value()) {
            return violation(schema_file, path,
                             QStringLiteral("string is not valid Unicode scalar text"));
        }
        for (const auto& keyword : {QStringLiteral("minLength"), QStringLiteral("maxLength")}) {
            if (!schema.contains(keyword)) {
                continue;
            }
            const auto bound = schema.value(keyword);
            if (!isExactInteger(bound) || bound.toDouble() < 0) {
                return fail(ErrorCode::UnsupportedSchema,
                            QStringLiteral("%1 must be a non-negative integer in %2")
                                .arg(keyword, schema_file));
            }
            const auto length = static_cast<double>(*scalar_count);
            if ((keyword == QStringLiteral("minLength") && length < bound.toDouble()) ||
                (keyword == QStringLiteral("maxLength") && length > bound.toDouble())) {
                return violation(schema_file, path,
                                 QStringLiteral("string violates %1").arg(keyword));
            }
        }
        if (schema.contains(QStringLiteral("pattern"))) {
            const auto pattern_value = schema.value(QStringLiteral("pattern"));
            if (!pattern_value.isString()) {
                return fail(ErrorCode::UnsupportedSchema,
                            QStringLiteral("pattern must be a string in %1").arg(schema_file));
            }
            const QRegularExpression pattern(pattern_value.toString());
            if (!pattern.isValid()) {
                return fail(ErrorCode::UnsupportedSchema,
                            QStringLiteral("Invalid regular expression in %1").arg(schema_file));
            }
            if (!pattern.match(string).hasMatch()) {
                return violation(schema_file, path,
                                 QStringLiteral("string does not match pattern"));
            }
        }
        if (schema.contains(QStringLiteral("format"))) {
            const auto format_value = schema.value(QStringLiteral("format"));
            if (!format_value.isString()) {
                return fail(ErrorCode::UnsupportedSchema,
                            QStringLiteral("format must be a string in %1").arg(schema_file));
            }
            const auto format = format_value.toString();
            if (format == QStringLiteral("date")) {
                const auto parsed = QDate::fromString(string, QStringLiteral("yyyy-MM-dd"));
                if (!parsed.isValid() || parsed.toString(QStringLiteral("yyyy-MM-dd")) != string) {
                    return violation(schema_file, path, QStringLiteral("invalid date"));
                }
            } else if (format == QStringLiteral("uri")) {
                const QUrl parsed(string, QUrl::StrictMode);
                if (!parsed.isValid() || parsed.scheme().isEmpty()) {
                    return violation(schema_file, path, QStringLiteral("invalid absolute URI"));
                }
            } else {
                return fail(
                    ErrorCode::UnsupportedSchema,
                    QStringLiteral("Unsupported format '%1' in %2").arg(format, schema_file));
            }
        }
    }

    if (instance.isDouble()) {
        const auto number = instance.toDouble();
        if (!std::isfinite(number)) {
            return violation(schema_file, path, QStringLiteral("number must be finite"));
        }
        for (const auto& keyword : {QStringLiteral("minimum"), QStringLiteral("maximum")}) {
            if (!schema.contains(keyword)) {
                continue;
            }
            const auto bound = schema.value(keyword);
            if (!bound.isDouble() || !std::isfinite(bound.toDouble())) {
                return fail(
                    ErrorCode::UnsupportedSchema,
                    QStringLiteral("%1 must be a finite number in %2").arg(keyword, schema_file));
            }
            if ((keyword == QStringLiteral("minimum") && number < bound.toDouble()) ||
                (keyword == QStringLiteral("maximum") && number > bound.toDouble())) {
                return violation(schema_file, path,
                                 QStringLiteral("number violates %1").arg(keyword));
            }
        }
    }
    return {};
}

} // namespace

std::expected<SchemaValidator, Error>
SchemaValidator::fromBundledSchemas(std::uint32_t schema_version) {
    initializeAppellateSchemaResources();
    const auto generation_files = schemaFiles(schema_version);
    if (generation_files.empty()) {
        return fail(ErrorCode::UnsupportedSchema,
                    QStringLiteral("Unsupported bundled schema version: %1").arg(schema_version));
    }
    SchemaValidator validator;
    const auto resource_prefix = QStringLiteral(":/appellate/schemas/v%1/").arg(schema_version);
    for (const auto* file_name : generation_files) {
        const auto name = QString::fromLatin1(file_name);
        QFile file(resource_prefix + name);
        if (!file.open(QIODevice::ReadOnly) || file.size() < 0 ||
            file.size() > maximum_schema_bytes) {
            return fail(ErrorCode::UnsupportedSchema,
                        QStringLiteral("Bundled schema is missing or oversized: %1").arg(name));
        }
        const auto bytes = file.read(maximum_schema_bytes + 1);
        if (bytes.size() > maximum_schema_bytes || !file.atEnd()) {
            return fail(ErrorCode::UnsupportedSchema,
                        QStringLiteral("Bundled schema is oversized: %1").arg(name));
        }
        auto parsed = parseObject(bytes, name);
        if (!parsed) {
            auto error = parsed.error();
            error.code = ErrorCode::UnsupportedSchema;
            return std::unexpected(std::move(error));
        }
        validator.schemas_.insert(name, std::move(*parsed));
    }

    for (auto schema = validator.schemas_.constBegin(); schema != validator.schemas_.constEnd();
         ++schema) {
        const auto declaration = schema.value().value(QStringLiteral("$schema"));
        if (!declaration.isString() ||
            declaration.toString() !=
                QStringLiteral("https://json-schema.org/draft/2020-12/schema")) {
            return fail(ErrorCode::UnsupportedSchema,
                        QStringLiteral("Schema %1 is not declared as JSON Schema 2020-12")
                            .arg(schema.key()));
        }
        const auto verified =
            verifySchemaShape(validator.schemas_, schema.key(), schema.value(), 0);
        if (!verified) {
            return std::unexpected(verified.error());
        }
    }
    return validator;
}

std::expected<QJsonObject, Error>
SchemaValidator::parseObject(QByteArrayView bytes, QStringView source_name, JsonLimits limits) {
    JsonLexicalScanner scanner(bytes, source_name, limits);
    const auto scanned = scanner.scan();
    if (!scanned) {
        return std::unexpected(scanned.error());
    }
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(bytes.toByteArray(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        return fail(ErrorCode::InvalidJson,
                    QStringLiteral("Invalid JSON object in %1: %2")
                        .arg(source_name.toString(), parse_error.errorString()));
    }
    return document.object();
}

std::expected<void, Error> SchemaValidator::validate(QStringView schema_file,
                                                     const QJsonObject& instance) const {
    const auto name = schema_file.toString();
    const auto schema = schemas_.constFind(name);
    if (schema == schemas_.constEnd()) {
        return fail(ErrorCode::UnsupportedSchema,
                    QStringLiteral("Unknown bundled schema: %1").arg(name));
    }
    return validateNode(schemas_, name, schema.value(), instance, QString{}, 0);
}

} // namespace appellate::packs
