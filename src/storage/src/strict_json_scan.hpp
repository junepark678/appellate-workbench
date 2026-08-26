#pragma once

#include <QByteArrayView>
#include <QChar>
#include <QSet>
#include <QString>
#include <QStringView>

#include <expected>
#include <optional>
#include <utility>

namespace appellate::storage::detail {

enum class StrictJsonErrorCode {
    InvalidJson,
    DuplicateMember,
};

struct StrictJsonError final {
    StrictJsonErrorCode code;
    QString message;
};

class StrictJsonScanner final {
  public:
    explicit StrictJsonScanner(QStringView input) : input_(input) {}

    [[nodiscard]] auto scan() -> std::expected<void, StrictJsonError> {
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
    static constexpr int maximum_depth = 64;

    [[nodiscard]] static auto fail(StrictJsonErrorCode code, QString message)
        -> std::unexpected<StrictJsonError> {
        return std::unexpected(StrictJsonError{code, std::move(message)});
    }

    [[nodiscard]] static auto invalid(QString message) -> std::unexpected<StrictJsonError> {
        return fail(StrictJsonErrorCode::InvalidJson, std::move(message));
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

    [[nodiscard]] auto parseString() -> std::expected<QString, StrictJsonError> {
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

    [[nodiscard]] auto parseObject(int depth) -> std::expected<void, StrictJsonError> {
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
                return fail(StrictJsonErrorCode::DuplicateMember,
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

    [[nodiscard]] auto parseArray(int depth) -> std::expected<void, StrictJsonError> {
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

    [[nodiscard]] auto parseNumber() -> std::expected<void, StrictJsonError> {
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

    [[nodiscard]] auto parseValue(int depth) -> std::expected<void, StrictJsonError> {
        if (depth > maximum_depth || position_ >= input_.size()) {
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

[[nodiscard]] inline auto scanStrictJson(QByteArrayView encoded)
    -> std::expected<void, StrictJsonError> {
    const auto text = QString::fromUtf8(encoded.data(), encoded.size());
    if (text.toUtf8() != encoded) {
        return std::unexpected(StrictJsonError{StrictJsonErrorCode::InvalidJson,
                                               QStringLiteral("JSON is not valid UTF-8")});
    }
    return StrictJsonScanner{text}.scan();
}

} // namespace appellate::storage::detail
