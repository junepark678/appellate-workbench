#include "appellate/content/render_cli.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>
#include <QStringDecoder>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <expected>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

namespace appellate::content {
namespace {

constexpr auto plan_schema_version = 1;
constexpr auto inventory_schema_version = 1;
constexpr auto inventory_file_name = "inventory.json";
constexpr auto assembly_contract = "appellate.markdown-assembly.v1";
constexpr int maximum_json_depth = 32;
constexpr qint64 hard_maximum_plan_bytes = 16LL * 1024LL * 1024LL;
constexpr qsizetype hard_maximum_entries = 4'096;
constexpr qsizetype hard_maximum_segments_per_entry = 2'048;
constexpr qsizetype hard_maximum_string_bytes = 1024 * 1024;
constexpr qint64 hard_maximum_total_bytes = 16LL * 1024LL * 1024LL * 1024LL;
constexpr qint64 hard_maximum_document_input_bytes = 16LL * 1024LL * 1024LL;
constexpr qint64 hard_maximum_document_output_bytes = 512LL * 1024LL * 1024LL;
constexpr int hard_maximum_document_pages = 2'048;

struct Failure final {
    RenderCliExitCode exit_code;
    QString code;
    QString message;
};

template <typename Value> using Result = std::expected<Value, Failure>;

template <typename Value = void>
[[nodiscard]] auto fail(RenderCliExitCode exit_code, QString code, QString message)
    -> std::unexpected<Failure> {
    return std::unexpected(Failure{exit_code, std::move(code), std::move(message)});
}

[[nodiscard]] QByteArray encoded(QJsonObject object) {
    object.insert(QStringLiteral("schema_version"), inventory_schema_version);
    return QJsonDocument(std::move(object)).toJson(QJsonDocument::Compact) + '\n';
}

[[nodiscard]] RenderCliResult failureResult(const Failure& failure) {
    return RenderCliResult{
        static_cast<int>(failure.exit_code),
        {},
        encoded(QJsonObject{
            {QStringLiteral("code"), failure.code},
            {QStringLiteral("message"), failure.message},
            {QStringLiteral("status"), QStringLiteral("error")},
        }),
    };
}

[[nodiscard]] RenderCliResult successResult(const QString& output_directory,
                                            qsizetype entry_count) {
    return RenderCliResult{
        static_cast<int>(RenderCliExitCode::Success),
        encoded(QJsonObject{
            {QStringLiteral("entry_count"), static_cast<qint64>(entry_count)},
            {QStringLiteral("inventory_path"),
             QDir(output_directory).filePath(QString::fromLatin1(inventory_file_name))},
            {QStringLiteral("output_directory"), output_directory},
            {QStringLiteral("pdf_byte_deterministic"),
             MarkdownPdfRenderer::byteOutputIsDeterministic()},
            {QStringLiteral("renderer_contract"),
             MarkdownPdfRenderer::rendererContractVersion().toString()},
            {QStringLiteral("status"), QStringLiteral("ok")},
        }),
        {},
    };
}

class StrictJsonScanner final {
  public:
    explicit StrictJsonScanner(QStringView input) : input_(input) {}

    [[nodiscard]] Result<void> scan() {
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
    [[nodiscard]] auto invalid(QString message) const -> std::unexpected<Failure> {
        return fail(RenderCliExitCode::InvalidPlan, QStringLiteral("invalid_json"),
                    QStringLiteral("Invalid render plan JSON at character %1: %2")
                        .arg(position_)
                        .arg(std::move(message)));
    }

    void skipWhitespace() {
        while (position_ < input_.size()) {
            const auto character = input_.at(position_);
            if (character != u' ' && character != u'\t' && character != u'\r' &&
                character != u'\n') {
                return;
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

    [[nodiscard]] static std::optional<unsigned> hexValue(QChar character) {
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

    [[nodiscard]] Result<unsigned> parseEscapedCodeUnit() {
        if (input_.size() - position_ < 4) {
            return fail<unsigned>(RenderCliExitCode::InvalidPlan, QStringLiteral("invalid_json"),
                                  QStringLiteral("Incomplete JSON Unicode escape"));
        }
        unsigned code_unit{};
        for (int index = 0; index < 4; ++index) {
            const auto digit = hexValue(input_.at(position_++));
            if (!digit) {
                return fail<unsigned>(RenderCliExitCode::InvalidPlan,
                                      QStringLiteral("invalid_json"),
                                      QStringLiteral("Invalid JSON Unicode escape"));
            }
            code_unit = code_unit * 16U + *digit;
        }
        return code_unit;
    }

    [[nodiscard]] Result<QString> parseString() {
        if (!consume(u'"')) {
            return fail<QString>(RenderCliExitCode::InvalidPlan, QStringLiteral("invalid_json"),
                                 QStringLiteral("Expected JSON string"));
        }
        QString result;
        while (position_ < input_.size()) {
            const auto character = input_.at(position_++);
            if (character == u'"') {
                return result;
            }
            if (character.unicode() < 0x20U) {
                return fail<QString>(RenderCliExitCode::InvalidPlan, QStringLiteral("invalid_json"),
                                     QStringLiteral("Unescaped JSON control character"));
            }
            if (character != u'\\') {
                if (character.isHighSurrogate()) {
                    if (position_ >= input_.size() || !input_.at(position_).isLowSurrogate()) {
                        return fail<QString>(RenderCliExitCode::InvalidPlan,
                                             QStringLiteral("invalid_json"),
                                             QStringLiteral("Unpaired Unicode surrogate"));
                    }
                    result.append(character);
                    result.append(input_.at(position_++));
                } else if (character.isLowSurrogate()) {
                    return fail<QString>(RenderCliExitCode::InvalidPlan,
                                         QStringLiteral("invalid_json"),
                                         QStringLiteral("Unpaired Unicode surrogate"));
                } else {
                    result.append(character);
                }
                continue;
            }
            if (position_ >= input_.size()) {
                return fail<QString>(RenderCliExitCode::InvalidPlan, QStringLiteral("invalid_json"),
                                     QStringLiteral("Incomplete JSON escape"));
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
                const auto first = parseEscapedCodeUnit();
                if (!first) {
                    return std::unexpected(first.error());
                }
                if (*first >= 0xD800U && *first <= 0xDBFFU) {
                    if (input_.size() - position_ < 6 || input_.at(position_) != u'\\' ||
                        input_.at(position_ + 1) != u'u') {
                        return fail<QString>(RenderCliExitCode::InvalidPlan,
                                             QStringLiteral("invalid_json"),
                                             QStringLiteral("Unpaired Unicode surrogate escape"));
                    }
                    position_ += 2;
                    const auto second = parseEscapedCodeUnit();
                    if (!second) {
                        return std::unexpected(second.error());
                    }
                    if (*second < 0xDC00U || *second > 0xDFFFU) {
                        return fail<QString>(RenderCliExitCode::InvalidPlan,
                                             QStringLiteral("invalid_json"),
                                             QStringLiteral("Unpaired Unicode surrogate escape"));
                    }
                    result.append(QChar{static_cast<char16_t>(*first)});
                    result.append(QChar{static_cast<char16_t>(*second)});
                } else if (*first >= 0xDC00U && *first <= 0xDFFFU) {
                    return fail<QString>(RenderCliExitCode::InvalidPlan,
                                         QStringLiteral("invalid_json"),
                                         QStringLiteral("Unpaired Unicode surrogate escape"));
                } else {
                    result.append(QChar{static_cast<char16_t>(*first)});
                }
                break;
            }
            default:
                return fail<QString>(RenderCliExitCode::InvalidPlan, QStringLiteral("invalid_json"),
                                     QStringLiteral("Unknown JSON escape"));
            }
        }
        return fail<QString>(RenderCliExitCode::InvalidPlan, QStringLiteral("invalid_json"),
                             QStringLiteral("Unterminated JSON string"));
    }

    [[nodiscard]] Result<void> parseObject(int depth) {
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
                return fail(RenderCliExitCode::InvalidPlan, QStringLiteral("duplicate_json_key"),
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

    [[nodiscard]] Result<void> parseArray(int depth) {
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

    [[nodiscard]] Result<void> parseNumber() {
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
            const auto begin = position_;
            while (position_ < input_.size() && input_.at(position_).isDigit()) {
                ++position_;
            }
            if (begin == position_) {
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
            const auto begin = position_;
            while (position_ < input_.size() && input_.at(position_).isDigit()) {
                ++position_;
            }
            if (begin == position_) {
                return invalid(QStringLiteral("Invalid JSON exponent"));
            }
        }
        return {};
    }

    [[nodiscard]] Result<void> parseValue(int depth) {
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

[[nodiscard]] Result<void> exactKeys(const QJsonObject& object,
                                     std::initializer_list<QStringView> required,
                                     std::initializer_list<QStringView> optional,
                                     QStringView context) {
    QSet<QString> allowed;
    for (const auto key : required) {
        allowed.insert(key.toString());
        if (!object.contains(key)) {
            return fail(RenderCliExitCode::InvalidPlan, QStringLiteral("missing_field"),
                        QStringLiteral("Missing %1.%2").arg(context, key));
        }
    }
    for (const auto key : optional) {
        allowed.insert(key.toString());
    }
    for (auto iterator = object.constBegin(); iterator != object.constEnd(); ++iterator) {
        if (!allowed.contains(iterator.key())) {
            return fail(RenderCliExitCode::InvalidPlan, QStringLiteral("unknown_field"),
                        QStringLiteral("Unknown %1.%2").arg(context, iterator.key()));
        }
    }
    return {};
}

[[nodiscard]] bool hasForbiddenControl(QStringView value, bool allow_layout_whitespace) {
    for (const auto character : value) {
        const auto code = character.unicode();
        if (code == 0x7FU ||
            (code < 0x20U && !(allow_layout_whitespace &&
                               (character == u'\n' || character == u'\r' || character == u'\t')))) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool isCleanAbsolutePath(const QString& path) {
    if (path.isEmpty() || path.contains(u'\\') || hasForbiddenControl(path, false) ||
        !QDir::isAbsolutePath(path)) {
        return false;
    }
    const auto normalized = QDir::fromNativeSeparators(path);
    if ((normalized.endsWith(u'/') && normalized != QStringLiteral("/")) ||
        QDir::cleanPath(normalized) != normalized) {
        return false;
    }
    return QFileInfo(normalized).absoluteFilePath() == normalized;
}

[[nodiscard]] bool pathHasSymlinkComponent(const QString& absolute_path) {
    auto current = absolute_path;
    while (true) {
        const QFileInfo information(current);
        if (information.isSymLink()) {
            return true;
        }
        const auto parent = information.dir().absolutePath();
        if (parent == current) {
            return false;
        }
        current = parent;
    }
}

[[nodiscard]] bool isPortableCharacter(QChar character) {
    return (character >= u'a' && character <= u'z') || (character >= u'A' && character <= u'Z') ||
           (character >= u'0' && character <= u'9') || character == u'.' || character == u'_' ||
           character == u'-';
}

[[nodiscard]] bool isReservedPortableSegment(QStringView segment) {
    const auto base = segment.toString().section(u'.', 0, 0).toUpper();
    static const QSet<QString> reserved{
        QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),
        QStringLiteral("NUL"),  QStringLiteral("COM1"), QStringLiteral("COM2"),
        QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
        QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
        QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
        QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
        QStringLiteral("LPT9"),
    };
    return reserved.contains(base);
}

[[nodiscard]] bool isCleanPortableRelativePath(QStringView path, qsizetype maximum_bytes,
                                               QStringView required_suffix) {
    const auto encoded = path.toUtf8();
    if (path.isEmpty() || encoded.size() > maximum_bytes || QDir::isAbsolutePath(path.toString()) ||
        path.contains(u'\\') || path.contains(u':') || path.startsWith(u'/') ||
        path.endsWith(u'/') || QDir::cleanPath(path.toString()) != path ||
        !path.endsWith(required_suffix, Qt::CaseSensitive)) {
        return false;
    }
    const auto segments = path.toString().split(u'/', Qt::KeepEmptyParts);
    for (const auto& segment : segments) {
        if (segment.isEmpty() || segment == QStringLiteral(".") ||
            segment == QStringLiteral("..") || segment.size() > 120 || segment.endsWith(u'.') ||
            isReservedPortableSegment(segment) ||
            !std::ranges::all_of(segment, isPortableCharacter)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] QString normalizedPathKey(QStringView path) { return path.toString().toCaseFolded(); }

[[nodiscard]] bool addWithin(qint64& total, qint64 amount, qint64 maximum) {
    if (amount < 0 || maximum < 0 || total > maximum || amount > maximum - total) {
        return false;
    }
    total += amount;
    return true;
}

[[nodiscard]] QString sha256(QByteArrayView bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

void addDigestFrame(QCryptographicHash& hash, QByteArrayView name, QByteArrayView value) {
    hash.addData(name);
    hash.addData(QByteArrayView("\0", 1));
    const auto size = QByteArray::number(value.size());
    hash.addData(size);
    hash.addData(QByteArrayView("\0", 1));
    hash.addData(value);
    hash.addData(QByteArrayView("\0", 1));
}

[[nodiscard]] QString semanticPlanDigest(QStringView assembly_digest,
                                         QStringView semantic_render_digest) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addDigestFrame(hash, QByteArrayView("assembly-plan-sha256"), assembly_digest.toLatin1());
    addDigestFrame(hash, QByteArrayView("semantic-render-sha256"),
                   semantic_render_digest.toLatin1());
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] Result<void> validateLimits(const RenderBatchLimits& limits) {
    const auto positive_string_limit = [](qsizetype value) {
        return value > 0 && value <= hard_maximum_string_bytes;
    };
    if (limits.maximum_plan_bytes <= 0 || limits.maximum_plan_bytes > hard_maximum_plan_bytes ||
        limits.maximum_entries <= 0 || limits.maximum_entries > hard_maximum_entries ||
        limits.maximum_segments_per_entry <= 0 ||
        limits.maximum_segments_per_entry > hard_maximum_segments_per_entry ||
        !positive_string_limit(limits.maximum_path_bytes) ||
        !positive_string_limit(limits.maximum_title_bytes) ||
        !positive_string_limit(limits.maximum_inline_markdown_bytes) ||
        limits.maximum_total_string_bytes <= 0 ||
        limits.maximum_total_string_bytes > hard_maximum_total_bytes ||
        limits.maximum_total_source_bytes <= 0 ||
        limits.maximum_total_source_bytes > hard_maximum_total_bytes ||
        limits.maximum_total_assembled_bytes <= 0 ||
        limits.maximum_total_assembled_bytes > hard_maximum_total_bytes ||
        limits.maximum_total_output_bytes <= 0 ||
        limits.maximum_total_output_bytes > hard_maximum_total_bytes ||
        limits.document_limits.max_input_bytes <= 0 ||
        limits.document_limits.max_input_bytes > hard_maximum_document_input_bytes ||
        limits.document_limits.max_output_bytes <= 0 ||
        limits.document_limits.max_output_bytes > hard_maximum_document_output_bytes ||
        limits.document_limits.max_pages <= 0 ||
        limits.document_limits.max_pages > hard_maximum_document_pages) {
        return fail(RenderCliExitCode::InvalidArguments, QStringLiteral("invalid_limits"),
                    QStringLiteral("Render batch limits are outside supported bounds"));
    }
    return {};
}

[[nodiscard]] Result<QByteArray> readBoundedFile(const QString& path, qint64 maximum_bytes,
                                                 RenderCliExitCode exit_code, QString code,
                                                 QStringView label) {
    const QFileInfo information(path);
    if (!information.exists()) {
        return fail<QByteArray>(exit_code, std::move(code),
                                QStringLiteral("%1 does not exist: %2").arg(label, path));
    }
    if (!information.isFile() || information.isSymLink()) {
        return fail<QByteArray>(exit_code, std::move(code),
                                QStringLiteral("%1 is not a regular file: %2").arg(label, path));
    }
    if (information.size() < 0 || information.size() > maximum_bytes) {
        return fail<QByteArray>(
            exit_code, QStringLiteral("limit_exceeded"),
            QStringLiteral("%1 exceeds the %2-byte limit").arg(label).arg(maximum_bytes));
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail<QByteArray>(exit_code, std::move(code),
                                QStringLiteral("Cannot open %1: %2").arg(label, path));
    }
    const auto bytes = file.read(maximum_bytes + 1);
    if (bytes.size() > maximum_bytes) {
        return fail<QByteArray>(
            exit_code, QStringLiteral("limit_exceeded"),
            QStringLiteral("%1 exceeds the %2-byte limit").arg(label).arg(maximum_bytes));
    }
    if (!file.atEnd() || file.error() != QFileDevice::NoError) {
        return fail<QByteArray>(exit_code, std::move(code),
                                QStringLiteral("Cannot read complete %1: %2").arg(label, path));
    }
    return bytes;
}

[[nodiscard]] Result<QString> strictUtf8(QByteArrayView bytes, QStringView label) {
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString decoded = decoder.decode(bytes);
    if (decoder.hasError() || QByteArrayView(decoded.toUtf8()) != bytes) {
        return fail<QString>(RenderCliExitCode::InvalidPlan, QStringLiteral("invalid_utf8"),
                             QStringLiteral("%1 is not canonical UTF-8").arg(label));
    }
    return decoded;
}

struct SegmentSpec final {
    QString source_path;
    int first_page{};
    int last_page{};
};

struct EntrySpec final {
    std::optional<QString> source_path;
    std::vector<SegmentSpec> segments;
    std::optional<QString> front_matter_markdown;
    std::optional<MarkdownPdfPageLabels> page_labels;
    QString output_path;
    QString title;
};

struct ParsedPlan final {
    QByteArray bytes;
    std::vector<EntrySpec> entries;
};

[[nodiscard]] Result<int> positiveInteger(const QJsonValue& value, QStringView field, int maximum) {
    if (!value.isDouble()) {
        return fail<int>(RenderCliExitCode::InvalidPlan, QStringLiteral("schema_violation"),
                         QStringLiteral("%1 must be an integer").arg(field));
    }
    const auto number = value.toDouble();
    if (!std::isfinite(number) || std::trunc(number) != number || number < 1.0 ||
        number > static_cast<double>(maximum)) {
        return fail<int>(RenderCliExitCode::InvalidPlan, QStringLiteral("schema_violation"),
                         QStringLiteral("%1 must be between 1 and %2").arg(field).arg(maximum));
    }
    return static_cast<int>(number);
}

[[nodiscard]] Result<void> addPlanString(qint64& total, QStringView value,
                                         const RenderBatchLimits& limits, qsizetype maximum,
                                         QStringView field, bool allow_layout_whitespace) {
    const auto bytes = value.toUtf8().size();
    if (value.isEmpty() || bytes > maximum ||
        (!allow_layout_whitespace && value.trimmed() != value) ||
        (allow_layout_whitespace && value.trimmed().isEmpty()) ||
        hasForbiddenControl(value, allow_layout_whitespace)) {
        return fail(RenderCliExitCode::InvalidPlan, QStringLiteral("invalid_string"),
                    QStringLiteral("%1 is empty, malformed, or exceeds its byte limit").arg(field));
    }
    if (!addWithin(total, bytes, limits.maximum_total_string_bytes)) {
        return fail(RenderCliExitCode::InvalidPlan, QStringLiteral("limit_exceeded"),
                    QStringLiteral("Render plan strings exceed the aggregate byte limit"));
    }
    return {};
}

[[nodiscard]] Result<ParsedPlan> parsePlan(const QString& plan_path,
                                           const RenderBatchLimits& limits) {
    if (!isCleanAbsolutePath(plan_path) || pathHasSymlinkComponent(plan_path)) {
        return fail<ParsedPlan>(
            RenderCliExitCode::InvalidArguments, QStringLiteral("unsafe_plan_path"),
            QStringLiteral("Plan path must be a clean absolute regular path without symlinks"));
    }
    auto bytes =
        readBoundedFile(plan_path, limits.maximum_plan_bytes, RenderCliExitCode::InvalidPlan,
                        QStringLiteral("cannot_read_plan"), u"Render plan");
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    const auto decoded = strictUtf8(*bytes, u"Render plan");
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    if (const auto scan = StrictJsonScanner(*decoded).scan(); !scan) {
        return std::unexpected(scan.error());
    }

    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(*bytes, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        return fail<ParsedPlan>(RenderCliExitCode::InvalidPlan, QStringLiteral("invalid_json"),
                                QStringLiteral("Render plan must be one JSON object"));
    }
    const auto root = document.object();
    if (const auto keys = exactKeys(root, {u"schema_version", u"entries"}, {}, u"plan"); !keys) {
        return std::unexpected(keys.error());
    }
    const auto version = root.value(QStringLiteral("schema_version"));
    if (!version.isDouble() || version.toDouble() != static_cast<double>(plan_schema_version)) {
        return fail<ParsedPlan>(RenderCliExitCode::InvalidPlan,
                                QStringLiteral("unsupported_schema"),
                                QStringLiteral("Render plan schema_version must be 1"));
    }
    const auto entries_json = root.value(QStringLiteral("entries"));
    if (!entries_json.isArray()) {
        return fail<ParsedPlan>(RenderCliExitCode::InvalidPlan, QStringLiteral("schema_violation"),
                                QStringLiteral("plan.entries must be an array"));
    }
    const auto array = entries_json.toArray();
    if (array.isEmpty() || array.size() > limits.maximum_entries) {
        return fail<ParsedPlan>(RenderCliExitCode::InvalidPlan, QStringLiteral("limit_exceeded"),
                                QStringLiteral("plan.entries must contain between 1 and %1 items")
                                    .arg(limits.maximum_entries));
    }

    ParsedPlan parsed{*bytes, {}};
    parsed.entries.reserve(static_cast<std::size_t>(array.size()));
    QSet<QString> output_keys;
    QSet<QString> simple_source_keys;
    qint64 total_string_bytes{};
    for (qsizetype index = 0; index < array.size(); ++index) {
        const auto value = array.at(index);
        if (!value.isObject()) {
            return fail<ParsedPlan>(
                RenderCliExitCode::InvalidPlan, QStringLiteral("schema_violation"),
                QStringLiteral("plan.entries[%1] must be an object").arg(index));
        }
        const auto object = value.toObject();
        const auto context = QStringLiteral("plan.entries[%1]").arg(index);
        const auto has_source = object.contains(QStringLiteral("source_path"));
        const auto has_segments = object.contains(QStringLiteral("segments"));
        const auto has_page_label_prefix = object.contains(QStringLiteral("page_label_prefix"));
        const auto has_page_label_start = object.contains(QStringLiteral("page_label_start"));
        if (has_source == has_segments) {
            return fail<ParsedPlan>(
                RenderCliExitCode::InvalidPlan, QStringLiteral("schema_violation"),
                QStringLiteral("%1 must contain exactly one of source_path or segments")
                    .arg(context));
        }
        if (has_page_label_prefix != has_page_label_start) {
            return fail<ParsedPlan>(
                RenderCliExitCode::InvalidPlan, QStringLiteral("schema_violation"),
                QStringLiteral("%1 must contain page_label_prefix and page_label_start together")
                    .arg(context));
        }
        if (has_source) {
            if (const auto keys = exactKeys(object, {u"source_path", u"output_path", u"title"},
                                            {u"page_label_prefix", u"page_label_start"}, context);
                !keys) {
                return std::unexpected(keys.error());
            }
        } else if (const auto keys = exactKeys(
                       object, {u"segments", u"output_path", u"title"},
                       {u"front_matter_markdown", u"page_label_prefix", u"page_label_start"},
                       context);
                   !keys) {
            return std::unexpected(keys.error());
        }
        if (!object.value(QStringLiteral("output_path")).isString() ||
            !object.value(QStringLiteral("title")).isString()) {
            return fail<ParsedPlan>(
                RenderCliExitCode::InvalidPlan, QStringLiteral("schema_violation"),
                QStringLiteral("%1 output_path and title must be strings").arg(context));
        }

        EntrySpec entry;
        entry.output_path = object.value(QStringLiteral("output_path")).toString();
        entry.title = object.value(QStringLiteral("title")).toString();
        if (has_page_label_prefix) {
            if (!object.value(QStringLiteral("page_label_prefix")).isString()) {
                return fail<ParsedPlan>(
                    RenderCliExitCode::InvalidPlan, QStringLiteral("schema_violation"),
                    QStringLiteral("%1.page_label_prefix must be a string").arg(context));
            }
            const auto prefix = object.value(QStringLiteral("page_label_prefix")).toString();
            const auto uppercase_ascii = std::ranges::all_of(
                prefix, [](QChar character) { return character >= u'A' && character <= u'Z'; });
            if (prefix.isEmpty() ||
                prefix.toLatin1().size() > MarkdownPdfPageLabels::maximum_prefix_bytes ||
                !uppercase_ascii) {
                return fail<ParsedPlan>(
                    RenderCliExitCode::InvalidPlan, QStringLiteral("invalid_page_labels"),
                    QStringLiteral("%1.page_label_prefix must contain 1-16 uppercase ASCII letters")
                        .arg(context));
            }
            if (const auto added = addPlanString(
                    total_string_bytes, prefix, limits, MarkdownPdfPageLabels::maximum_prefix_bytes,
                    context + QStringLiteral(".page_label_prefix"), false);
                !added) {
                return std::unexpected(added.error());
            }
            const auto start = positiveInteger(object.value(QStringLiteral("page_label_start")),
                                               context + QStringLiteral(".page_label_start"),
                                               MarkdownPdfPageLabels::maximum_number);
            if (!start) {
                return std::unexpected(start.error());
            }
            entry.page_labels = MarkdownPdfPageLabels{prefix, *start};
        }
        if (!isCleanPortableRelativePath(entry.output_path, limits.maximum_path_bytes, u".pdf")) {
            return fail<ParsedPlan>(
                RenderCliExitCode::InvalidPlan, QStringLiteral("unsafe_output_path"),
                QStringLiteral("%1.output_path must be a clean portable relative .pdf path")
                    .arg(context));
        }
        if (const auto added = addPlanString(total_string_bytes, entry.output_path, limits,
                                             limits.maximum_path_bytes,
                                             context + QStringLiteral(".output_path"), false);
            !added) {
            return std::unexpected(added.error());
        }
        if (const auto added =
                addPlanString(total_string_bytes, entry.title, limits,
                              std::min(limits.maximum_title_bytes, static_cast<qsizetype>(512)),
                              context + QStringLiteral(".title"), false);
            !added) {
            return std::unexpected(added.error());
        }
        const auto output_key = normalizedPathKey(entry.output_path);
        if (output_keys.contains(output_key)) {
            return fail<ParsedPlan>(
                RenderCliExitCode::InvalidPlan, QStringLiteral("duplicate_output_path"),
                QStringLiteral("Duplicate normalized PDF output path: %1").arg(entry.output_path));
        }
        output_keys.insert(output_key);

        if (has_source) {
            if (!object.value(QStringLiteral("source_path")).isString()) {
                return fail<ParsedPlan>(
                    RenderCliExitCode::InvalidPlan, QStringLiteral("schema_violation"),
                    QStringLiteral("%1.source_path must be a string").arg(context));
            }
            entry.source_path = object.value(QStringLiteral("source_path")).toString();
            if (!isCleanPortableRelativePath(*entry.source_path, limits.maximum_path_bytes,
                                             u".md")) {
                return fail<ParsedPlan>(
                    RenderCliExitCode::InvalidPlan, QStringLiteral("unsafe_source_path"),
                    QStringLiteral("%1.source_path must be a clean portable relative .md path")
                        .arg(context));
            }
            if (const auto added = addPlanString(total_string_bytes, *entry.source_path, limits,
                                                 limits.maximum_path_bytes,
                                                 context + QStringLiteral(".source_path"), false);
                !added) {
                return std::unexpected(added.error());
            }
            const auto source_key = normalizedPathKey(*entry.source_path);
            if (simple_source_keys.contains(source_key)) {
                return fail<ParsedPlan>(
                    RenderCliExitCode::InvalidPlan, QStringLiteral("duplicate_source_path"),
                    QStringLiteral("Duplicate normalized single source path: %1")
                        .arg(*entry.source_path));
            }
            simple_source_keys.insert(source_key);
        } else {
            const auto segments_value = object.value(QStringLiteral("segments"));
            if (!segments_value.isArray()) {
                return fail<ParsedPlan>(
                    RenderCliExitCode::InvalidPlan, QStringLiteral("schema_violation"),
                    QStringLiteral("%1.segments must be an array").arg(context));
            }
            const auto segments = segments_value.toArray();
            if (segments.isEmpty() || segments.size() > limits.maximum_segments_per_entry) {
                return fail<ParsedPlan>(
                    RenderCliExitCode::InvalidPlan, QStringLiteral("limit_exceeded"),
                    QStringLiteral("%1.segments must contain between 1 and %2 items")
                        .arg(context)
                        .arg(limits.maximum_segments_per_entry));
            }
            entry.segments.reserve(static_cast<std::size_t>(segments.size()));
            QSet<QString> selected_ranges;
            for (qsizetype segment_index = 0; segment_index < segments.size(); ++segment_index) {
                const auto segment_value = segments.at(segment_index);
                if (!segment_value.isObject()) {
                    return fail<ParsedPlan>(RenderCliExitCode::InvalidPlan,
                                            QStringLiteral("schema_violation"),
                                            QStringLiteral("%1.segments[%2] must be an object")
                                                .arg(context)
                                                .arg(segment_index));
                }
                const auto segment_object = segment_value.toObject();
                const auto segment_context =
                    QStringLiteral("%1.segments[%2]").arg(context).arg(segment_index);
                if (const auto keys =
                        exactKeys(segment_object, {u"source_path", u"first_page", u"last_page"}, {},
                                  segment_context);
                    !keys) {
                    return std::unexpected(keys.error());
                }
                if (!segment_object.value(QStringLiteral("source_path")).isString()) {
                    return fail<ParsedPlan>(
                        RenderCliExitCode::InvalidPlan, QStringLiteral("schema_violation"),
                        QStringLiteral("%1.source_path must be a string").arg(segment_context));
                }
                SegmentSpec segment;
                segment.source_path =
                    segment_object.value(QStringLiteral("source_path")).toString();
                if (!isCleanPortableRelativePath(segment.source_path, limits.maximum_path_bytes,
                                                 u".md")) {
                    return fail<ParsedPlan>(
                        RenderCliExitCode::InvalidPlan, QStringLiteral("unsafe_source_path"),
                        QStringLiteral("%1.source_path must be a clean portable relative .md path")
                            .arg(segment_context));
                }
                if (const auto added = addPlanString(
                        total_string_bytes, segment.source_path, limits, limits.maximum_path_bytes,
                        segment_context + QStringLiteral(".source_path"), false);
                    !added) {
                    return std::unexpected(added.error());
                }
                const auto first =
                    positiveInteger(segment_object.value(QStringLiteral("first_page")),
                                    segment_context + QStringLiteral(".first_page"),
                                    limits.document_limits.max_pages);
                const auto last = positiveInteger(segment_object.value(QStringLiteral("last_page")),
                                                  segment_context + QStringLiteral(".last_page"),
                                                  limits.document_limits.max_pages);
                if (!first) {
                    return std::unexpected(first.error());
                }
                if (!last) {
                    return std::unexpected(last.error());
                }
                if (*first > *last) {
                    return fail<ParsedPlan>(
                        RenderCliExitCode::InvalidPlan, QStringLiteral("invalid_page_range"),
                        QStringLiteral("%1 first_page exceeds last_page").arg(segment_context));
                }
                segment.first_page = *first;
                segment.last_page = *last;
                const auto range_key = QStringLiteral("%1:%2:%3")
                                           .arg(normalizedPathKey(segment.source_path))
                                           .arg(segment.first_page)
                                           .arg(segment.last_page);
                if (selected_ranges.contains(range_key)) {
                    return fail<ParsedPlan>(
                        RenderCliExitCode::InvalidPlan, QStringLiteral("duplicate_segment"),
                        QStringLiteral("Duplicate normalized segment range in %1").arg(context));
                }
                selected_ranges.insert(range_key);
                entry.segments.push_back(std::move(segment));
            }
            if (object.contains(QStringLiteral("front_matter_markdown"))) {
                if (!object.value(QStringLiteral("front_matter_markdown")).isString()) {
                    return fail<ParsedPlan>(
                        RenderCliExitCode::InvalidPlan, QStringLiteral("schema_violation"),
                        QStringLiteral("%1.front_matter_markdown must be a string").arg(context));
                }
                entry.front_matter_markdown =
                    object.value(QStringLiteral("front_matter_markdown")).toString();
                if (const auto added =
                        addPlanString(total_string_bytes, *entry.front_matter_markdown, limits,
                                      limits.maximum_inline_markdown_bytes,
                                      context + QStringLiteral(".front_matter_markdown"), true);
                    !added) {
                    return std::unexpected(added.error());
                }
            }
        }
        parsed.entries.push_back(std::move(entry));
    }

    std::ranges::sort(parsed.entries, [](const EntrySpec& left, const EntrySpec& right) {
        const auto left_key = normalizedPathKey(left.output_path);
        const auto right_key = normalizedPathKey(right.output_path);
        return left_key == right_key ? left.output_path < right.output_path : left_key < right_key;
    });
    for (std::size_t index = 1; index < parsed.entries.size(); ++index) {
        const auto previous = normalizedPathKey(parsed.entries.at(index - 1).output_path) + u'/';
        const auto current = normalizedPathKey(parsed.entries.at(index).output_path);
        if (current.startsWith(previous)) {
            return fail<ParsedPlan>(
                RenderCliExitCode::InvalidPlan, QStringLiteral("output_path_collision"),
                QStringLiteral("A PDF output path cannot contain another output"));
        }
    }
    return parsed;
}

struct SourceDocument final {
    QString source_path;
    QByteArray bytes;
    QString sha256;
    QStringList logical_pages;
};

struct SourceCache final {
    QHash<QString, SourceDocument> documents;
    qint64 total_bytes{};
};

[[nodiscard]] Result<const SourceDocument*> loadSource(const QString& source_root,
                                                       const QString& relative_path,
                                                       const RenderBatchLimits& limits,
                                                       SourceCache& cache) {
    const auto key = normalizedPathKey(relative_path);
    const auto existing = cache.documents.constFind(key);
    if (existing != cache.documents.constEnd()) {
        if (existing->source_path != relative_path) {
            return fail<const SourceDocument*>(
                RenderCliExitCode::InvalidPlan, QStringLiteral("duplicate_normalized_path"),
                QStringLiteral("Source paths collide on a portable filesystem: %1 and %2")
                    .arg(existing->source_path, relative_path));
        }
        return &existing.value();
    }

    const auto absolute_path = QDir(source_root).filePath(relative_path);
    if (!isCleanAbsolutePath(absolute_path) || pathHasSymlinkComponent(absolute_path)) {
        return fail<const SourceDocument*>(
            RenderCliExitCode::InvalidPlan, QStringLiteral("unsafe_source_path"),
            QStringLiteral("Source has a symbolic-link or unsafe path component: %1")
                .arg(relative_path));
    }
    auto bytes = readBoundedFile(absolute_path, limits.document_limits.max_input_bytes,
                                 RenderCliExitCode::InvalidPlan,
                                 QStringLiteral("cannot_read_source"), u"Markdown source");
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    const auto decoded =
        strictUtf8(*bytes, QStringLiteral("Markdown source %1").arg(relative_path));
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    if (!addWithin(cache.total_bytes, bytes->size(), limits.maximum_total_source_bytes)) {
        return fail<const SourceDocument*>(
            RenderCliExitCode::InvalidPlan, QStringLiteral("limit_exceeded"),
            QStringLiteral("Unique Markdown sources exceed the aggregate byte limit"));
    }
    const auto marker = MarkdownPdfRenderer::pageBreakMarker().toString();
    auto pages = decoded->split(marker, Qt::KeepEmptyParts);
    SourceDocument source{relative_path, std::move(*bytes), {}, std::move(pages)};
    source.sha256 = sha256(source.bytes);
    const auto inserted = cache.documents.insert(key, std::move(source));
    return &inserted.value();
}

struct PreparedEntry final {
    QString output_path;
    QString title;
    std::optional<MarkdownPdfPageLabels> page_labels;
    QByteArray markdown;
    int logical_page_count{};
    bool enforce_one_physical_page_per_logical_page{};
    QJsonObject assembly_provenance;
    QString assembly_plan_sha256;
};

[[nodiscard]] Result<void> appendPage(QByteArray& assembled, QStringView page, int& page_count,
                                      const RenderBatchLimits& limits) {
    if (page.trimmed().isEmpty()) {
        return fail(RenderCliExitCode::InvalidPlan, QStringLiteral("empty_source_page"),
                    QStringLiteral("Selected Markdown pages must contain visible text"));
    }
    if (page_count > 0) {
        assembled.append("\n\n");
        assembled.append(MarkdownPdfRenderer::pageBreakMarker().toUtf8());
        assembled.append("\n\n");
    }
    assembled.append(page.toUtf8());
    ++page_count;
    if (page_count > limits.document_limits.max_pages ||
        assembled.size() > limits.document_limits.max_input_bytes) {
        return fail(
            RenderCliExitCode::InvalidPlan, QStringLiteral("limit_exceeded"),
            QStringLiteral("Assembled Markdown exceeds the per-document page or byte limit"));
    }
    return {};
}

[[nodiscard]] Result<PreparedEntry> prepareEntry(const QString& source_root, const EntrySpec& entry,
                                                 const RenderBatchLimits& limits,
                                                 SourceCache& cache,
                                                 qint64& total_assembled_bytes) {
    PreparedEntry prepared;
    prepared.output_path = entry.output_path;
    prepared.title = entry.title;
    prepared.page_labels = entry.page_labels;
    QJsonObject assembly{
        {QStringLiteral("assembly_contract"), QString::fromLatin1(assembly_contract)},
    };

    if (entry.source_path) {
        const auto source = loadSource(source_root, *entry.source_path, limits, cache);
        if (!source) {
            return std::unexpected(source.error());
        }
        prepared.markdown = (*source)->bytes;
        prepared.logical_page_count = static_cast<int>((*source)->logical_pages.size());
        assembly.insert(QStringLiteral("kind"), QStringLiteral("single_source"));
        assembly.insert(QStringLiteral("logical_page_count"), prepared.logical_page_count);
        assembly.insert(QStringLiteral("source_path"), (*source)->source_path);
        assembly.insert(QStringLiteral("source_sha256"), (*source)->sha256);
    } else {
        prepared.enforce_one_physical_page_per_logical_page = true;
        assembly.insert(QStringLiteral("canonical_page_join"),
                        QStringLiteral("LF-LF-explicit-page-break-marker-LF-LF"));
        assembly.insert(QStringLiteral("kind"), QStringLiteral("composite"));
        if (entry.front_matter_markdown) {
            const auto front_bytes = entry.front_matter_markdown->toUtf8();
            const auto front_pages = entry.front_matter_markdown->split(
                MarkdownPdfRenderer::pageBreakMarker().toString(), Qt::KeepEmptyParts);
            for (const auto& page : front_pages) {
                if (const auto appended =
                        appendPage(prepared.markdown, page, prepared.logical_page_count, limits);
                    !appended) {
                    return std::unexpected(appended.error());
                }
            }
            assembly.insert(QStringLiteral("front_matter"),
                            QJsonObject{
                                {QStringLiteral("logical_page_count"), front_pages.size()},
                                {QStringLiteral("source_sha256"), sha256(front_bytes)},
                            });
        }

        QJsonArray segment_inventory;
        for (const auto& segment : entry.segments) {
            const auto source = loadSource(source_root, segment.source_path, limits, cache);
            if (!source) {
                return std::unexpected(source.error());
            }
            if (segment.last_page > (*source)->logical_pages.size()) {
                return fail<PreparedEntry>(
                    RenderCliExitCode::InvalidPlan, QStringLiteral("invalid_page_range"),
                    QStringLiteral("Range %1-%2 exceeds %3 logical pages in %4")
                        .arg(segment.first_page)
                        .arg(segment.last_page)
                        .arg((*source)->logical_pages.size())
                        .arg(segment.source_path));
            }
            for (int page_index = segment.first_page; page_index <= segment.last_page;
                 ++page_index) {
                const auto& page = (*source)->logical_pages.at(page_index - 1);
                if (const auto appended =
                        appendPage(prepared.markdown, page, prepared.logical_page_count, limits);
                    !appended) {
                    return std::unexpected(appended.error());
                }
            }
            segment_inventory.push_back(QJsonObject{
                {QStringLiteral("first_page"), segment.first_page},
                {QStringLiteral("last_page"), segment.last_page},
                {QStringLiteral("source_logical_page_count"), (*source)->logical_pages.size()},
                {QStringLiteral("source_path"), segment.source_path},
                {QStringLiteral("source_sha256"), (*source)->sha256},
            });
        }
        assembly.insert(QStringLiteral("logical_page_count"), prepared.logical_page_count);
        assembly.insert(QStringLiteral("segments"), segment_inventory);
    }

    if (prepared.markdown.isEmpty() || !addWithin(total_assembled_bytes, prepared.markdown.size(),
                                                  limits.maximum_total_assembled_bytes)) {
        return fail<PreparedEntry>(
            RenderCliExitCode::InvalidPlan, QStringLiteral("limit_exceeded"),
            QStringLiteral("Assembled Markdown exceeds aggregate byte limit"));
    }
    if (prepared.page_labels &&
        static_cast<qint64>(prepared.page_labels->first_number) + prepared.logical_page_count - 1 >
            MarkdownPdfPageLabels::maximum_number) {
        return fail<PreparedEntry>(
            RenderCliExitCode::InvalidPlan, QStringLiteral("page_label_overflow"),
            QStringLiteral("Page label sequence for %1 exceeds the maximum label number %2")
                .arg(prepared.output_path)
                .arg(MarkdownPdfPageLabels::maximum_number));
    }
    prepared.assembly_provenance = std::move(assembly);
    const auto canonical_assembly =
        QJsonDocument(prepared.assembly_provenance).toJson(QJsonDocument::Compact);
    prepared.assembly_plan_sha256 = sha256(canonical_assembly);
    return prepared;
}

[[nodiscard]] QString rendererErrorCode(MarkdownPdfErrorCode code) {
    switch (code) {
    case MarkdownPdfErrorCode::InvalidConfiguration:
        return QStringLiteral("invalid_renderer_configuration");
    case MarkdownPdfErrorCode::InvalidUtf8:
        return QStringLiteral("invalid_markdown_utf8");
    case MarkdownPdfErrorCode::EmptyInput:
        return QStringLiteral("empty_markdown");
    case MarkdownPdfErrorCode::InputTooLarge:
        return QStringLiteral("markdown_too_large");
    case MarkdownPdfErrorCode::UnsafeSourcePath:
        return QStringLiteral("unsafe_source_path");
    case MarkdownPdfErrorCode::SourceNotFound:
        return QStringLiteral("source_not_found");
    case MarkdownPdfErrorCode::SourceNotRegularFile:
        return QStringLiteral("source_not_regular_file");
    case MarkdownPdfErrorCode::CannotReadSource:
        return QStringLiteral("cannot_read_source");
    case MarkdownPdfErrorCode::UnsafeOutputPath:
        return QStringLiteral("unsafe_staging_output");
    case MarkdownPdfErrorCode::OutputAlreadyExists:
        return QStringLiteral("staging_output_exists");
    case MarkdownPdfErrorCode::UnsupportedContent:
        return QStringLiteral("unsupported_markdown_content");
    case MarkdownPdfErrorCode::RequiredFontUnavailable:
        return QStringLiteral("required_font_unavailable");
    case MarkdownPdfErrorCode::PageLimitExceeded:
        return QStringLiteral("page_limit_exceeded");
    case MarkdownPdfErrorCode::CannotCreateTemporaryOutput:
        return QStringLiteral("cannot_create_pdf_staging");
    case MarkdownPdfErrorCode::CannotRender:
        return QStringLiteral("cannot_render_pdf");
    case MarkdownPdfErrorCode::OutputTooLarge:
        return QStringLiteral("pdf_too_large");
    case MarkdownPdfErrorCode::CannotSyncOutput:
        return QStringLiteral("cannot_sync_pdf");
    case MarkdownPdfErrorCode::CannotCommitOutput:
        return QStringLiteral("cannot_commit_pdf");
    }
    return QStringLiteral("render_failed");
}

[[nodiscard]] Result<void> writeInventory(const QString& staging_root, QByteArrayView plan_bytes,
                                          const QJsonArray& entries) {
    const QJsonObject inventory{
        {QStringLiteral("entries"), entries},
        {QStringLiteral("ordering"), QStringLiteral("output_path_casefolded_then_codepoint")},
        {QStringLiteral("pdf_byte_deterministic"),
         MarkdownPdfRenderer::byteOutputIsDeterministic()},
        {QStringLiteral("plan_sha256"), sha256(plan_bytes)},
        {QStringLiteral("renderer_contract"),
         MarkdownPdfRenderer::rendererContractVersion().toString()},
        {QStringLiteral("schema_version"), inventory_schema_version},
    };
    const auto bytes = QJsonDocument(inventory).toJson(QJsonDocument::Compact);
    QSaveFile output(QDir(staging_root).filePath(QString::fromLatin1(inventory_file_name)));
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly) ||
        output.write(bytes) != static_cast<qint64>(bytes.size()) || !output.commit()) {
        return fail(RenderCliExitCode::OperationFailed, QStringLiteral("cannot_write_inventory"),
                    QStringLiteral("Cannot atomically write the render inventory"));
    }
    return {};
}

[[nodiscard]] Result<void> validateRoots(const QString& source_root,
                                         const QString& output_directory) {
    if (!isCleanAbsolutePath(source_root) || pathHasSymlinkComponent(source_root)) {
        return fail(
            RenderCliExitCode::InvalidArguments, QStringLiteral("unsafe_source_root"),
            QStringLiteral("Source root must be a clean absolute directory without symlinks"));
    }
    const QFileInfo source_information(source_root);
    if (!source_information.exists() || !source_information.isDir() ||
        source_information.isSymLink()) {
        return fail(RenderCliExitCode::InvalidArguments, QStringLiteral("unsafe_source_root"),
                    QStringLiteral("Source root must be an existing regular directory"));
    }
    if (!isCleanAbsolutePath(output_directory) || output_directory == QStringLiteral("/") ||
        pathHasSymlinkComponent(output_directory)) {
        return fail(
            RenderCliExitCode::InvalidArguments, QStringLiteral("unsafe_output_directory"),
            QStringLiteral("Output must be a clean new absolute directory without symlinks"));
    }
    const QFileInfo output_information(output_directory);
    if (output_information.exists() || output_information.isSymLink()) {
        return fail(RenderCliExitCode::OperationFailed, QStringLiteral("destination_exists"),
                    QStringLiteral("Refusing to overwrite the output directory"));
    }
    const QFileInfo parent(output_information.absolutePath());
    if (!parent.exists() || !parent.isDir() || parent.isSymLink() ||
        pathHasSymlinkComponent(parent.absoluteFilePath())) {
        return fail(
            RenderCliExitCode::InvalidArguments, QStringLiteral("unsafe_output_directory"),
            QStringLiteral("Output parent must be an existing regular directory without symlinks"));
    }
    return {};
}

[[nodiscard]] Result<void> renderAndPublish(const ParsedPlan& plan, const QString& source_root,
                                            const QString& output_directory,
                                            const RenderBatchLimits& limits) {
    SourceCache cache;
    std::vector<PreparedEntry> prepared_entries;
    prepared_entries.reserve(plan.entries.size());
    qint64 total_assembled_bytes{};
    for (const auto& entry : plan.entries) {
        auto prepared = prepareEntry(source_root, entry, limits, cache, total_assembled_bytes);
        if (!prepared) {
            return std::unexpected(prepared.error());
        }
        prepared_entries.push_back(std::move(*prepared));
    }

    const QFileInfo destination_information(output_directory);
    const auto parent_path = destination_information.absolutePath();
    const auto destination_name = destination_information.fileName();
    const auto staging_pattern =
        QDir(parent_path)
            .filePath(QStringLiteral(".%1.appellate-render-XXXXXX").arg(destination_name));
    QTemporaryDir staging(staging_pattern);
    if (!staging.isValid() ||
        !QFile::setPermissions(staging.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                   QFileDevice::ExeOwner)) {
        return fail(RenderCliExitCode::OperationFailed, QStringLiteral("cannot_create_staging"),
                    QStringLiteral("Cannot create private same-filesystem staging directory"));
    }

    const MarkdownPdfRenderer renderer(limits.document_limits);
    QJsonArray inventory_entries;
    qint64 total_output_bytes{};
    for (const auto& entry : prepared_entries) {
        const auto output_path = QDir(staging.path()).filePath(entry.output_path);
        const auto relative_parent = QFileInfo(entry.output_path).path();
        if (relative_parent != QStringLiteral(".") &&
            !QDir(staging.path()).mkpath(relative_parent)) {
            return fail(
                RenderCliExitCode::OperationFailed,
                QStringLiteral("cannot_create_output_directory"),
                QStringLiteral("Cannot create staged PDF parent for %1").arg(entry.output_path));
        }
        MarkdownPdfMetadata metadata{entry.title};
        metadata.page_labels = entry.page_labels;
        const auto rendered = renderer.render(entry.markdown, output_path, metadata);
        if (!rendered) {
            return fail(RenderCliExitCode::RenderFailed, rendererErrorCode(rendered.error().code),
                        QStringLiteral("Cannot render %1: %2")
                            .arg(entry.output_path, rendered.error().message));
        }
        if (entry.enforce_one_physical_page_per_logical_page &&
            rendered->page_count != entry.logical_page_count) {
            return fail(
                RenderCliExitCode::RenderFailed, QStringLiteral("page_layout_mismatch"),
                QStringLiteral(
                    "Composite %1 rendered %2 physical pages from %3 selected logical pages")
                    .arg(entry.output_path)
                    .arg(rendered->page_count)
                    .arg(entry.logical_page_count));
        }
        if (!addWithin(total_output_bytes, rendered->output_bytes,
                       limits.maximum_total_output_bytes)) {
            return fail(RenderCliExitCode::RenderFailed,
                        QStringLiteral("total_output_limit_exceeded"),
                        QStringLiteral("Rendered PDFs exceed the aggregate output byte limit"));
        }
        QJsonObject inventory_entry{
            {QStringLiteral("assembly_plan_sha256"), entry.assembly_plan_sha256},
            {QStringLiteral("assembly_provenance"), entry.assembly_provenance},
            {QStringLiteral("byte_size"), rendered->output_bytes},
            {QStringLiteral("output_path"), entry.output_path},
            {QStringLiteral("page_count"), rendered->page_count},
            {QStringLiteral("pdf_byte_deterministic"),
             MarkdownPdfRenderer::byteOutputIsDeterministic()},
            {QStringLiteral("pdf_sha256"), rendered->pdf_sha256},
            {QStringLiteral("renderer_contract"),
             MarkdownPdfRenderer::rendererContractVersion().toString()},
            {QStringLiteral("renderer_provenance"), rendered->renderer_provenance},
            {QStringLiteral("semantic_plan_sha256"),
             semanticPlanDigest(entry.assembly_plan_sha256, rendered->semantic_render_sha256)},
            {QStringLiteral("semantic_render_sha256"), rendered->semantic_render_sha256},
            {QStringLiteral("source_sha256"), rendered->source_sha256},
            {QStringLiteral("title"), entry.title},
        };
        if (entry.page_labels) {
            inventory_entry.insert(
                QStringLiteral("page_labels"),
                QJsonObject{
                    {QStringLiteral("first_number"), entry.page_labels->first_number},
                    {QStringLiteral("last_number"),
                     static_cast<qint64>(entry.page_labels->first_number) + rendered->page_count -
                         1},
                    {QStringLiteral("prefix"), entry.page_labels->prefix},
                });
        }
        inventory_entries.push_back(std::move(inventory_entry));
    }
    if (const auto inventory = writeInventory(staging.path(), plan.bytes, inventory_entries);
        !inventory) {
        return std::unexpected(inventory.error());
    }

    QDir parent(parent_path);
    const auto staging_name = QFileInfo(staging.path()).fileName();
    if (QFileInfo::exists(output_directory) || QFileInfo(output_directory).isSymLink()) {
        return fail(
            RenderCliExitCode::OperationFailed, QStringLiteral("destination_exists"),
            QStringLiteral("Output destination appeared during rendering; refusing overwrite"));
    }
    if (!parent.rename(staging_name, destination_name)) {
        return fail(RenderCliExitCode::OperationFailed, QStringLiteral("cannot_publish"),
                    QStringLiteral("Cannot atomically publish the completed output directory"));
    }
    staging.setAutoRemove(false);
    return {};
}

} // namespace

RenderCliResult runRenderCli(const QStringList& arguments, const RenderBatchLimits& limits) {
    if (const auto valid_limits = validateLimits(limits); !valid_limits) {
        return failureResult(valid_limits.error());
    }
    if (arguments.size() != 3) {
        return failureResult(Failure{
            RenderCliExitCode::InvalidArguments,
            QStringLiteral("invalid_arguments"),
            QStringLiteral("usage: appellate-render <absolute-plan.json> <absolute-source-root> "
                           "<new-absolute-output-directory>"),
        });
    }
    const auto& plan_path = arguments.at(0);
    const auto& source_root = arguments.at(1);
    const auto& output_directory = arguments.at(2);
    if (const auto roots = validateRoots(source_root, output_directory); !roots) {
        return failureResult(roots.error());
    }
    const auto plan = parsePlan(plan_path, limits);
    if (!plan) {
        return failureResult(plan.error());
    }
    if (const auto rendered = renderAndPublish(*plan, source_root, output_directory, limits);
        !rendered) {
        return failureResult(rendered.error());
    }
    return successResult(output_directory, static_cast<qsizetype>(plan->entries.size()));
}

} // namespace appellate::content
