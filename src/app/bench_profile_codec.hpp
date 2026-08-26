#pragma once

#include "appellate/model/judge_profile.hpp"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QStringView>

#include <expected>

namespace appellate::ui {

enum class BenchProfileErrorCode {
    CannotRead,
    CannotWrite,
    UnsafePath,
    AlreadyExists,
    InputTooLarge,
    InvalidJson,
    DuplicateJsonKey,
    UnsupportedSchema,
    UnexpectedField,
    MissingField,
    InvalidField,
    OutOfRange,
    IncompatibleProfile,
};

struct BenchProfileError final {
    BenchProfileErrorCode code;
    QString message;

    friend bool operator==(const BenchProfileError&, const BenchProfileError&) = default;
};

class BenchProfileCodec final {
  public:
    [[nodiscard]] static auto validate(const model::JudgeProfile& profile)
        -> std::expected<void, BenchProfileError>;
    [[nodiscard]] static auto encode(const model::JudgeProfile& profile)
        -> std::expected<QByteArray, BenchProfileError>;
    [[nodiscard]] static auto decode(QByteArrayView document)
        -> std::expected<model::JudgeProfile, BenchProfileError>;
    [[nodiscard]] static auto importFile(QStringView path)
        -> std::expected<model::JudgeProfile, BenchProfileError>;
    [[nodiscard]] static auto exportFile(const model::JudgeProfile& profile, QStringView path)
        -> std::expected<void, BenchProfileError>;
};

} // namespace appellate::ui
