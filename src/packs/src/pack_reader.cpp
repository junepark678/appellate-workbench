#include "appellate/packs/pack_reader.hpp"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace appellate::packs {
namespace {

constexpr auto supported_schema_version = 1;
constexpr QByteArrayView hash_separator{"\0", 1};

[[nodiscard]] auto fail(ErrorCode code, QString message) -> std::unexpected<Error> {
    return std::unexpected(Error{code, std::move(message)});
}

[[nodiscard]] auto readFile(const QString& path) -> std::expected<QByteArray, Error> {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(ErrorCode::CannotRead, QStringLiteral("Cannot read %1").arg(path));
    }
    return file.readAll();
}

[[nodiscard]] auto parseObject(const QByteArray& bytes, const QString& name)
    -> std::expected<QJsonObject, Error> {
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(bytes, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        return fail(ErrorCode::InvalidJson,
                    QStringLiteral("Invalid JSON in %1: %2").arg(name, parse_error.errorString()));
    }
    return document.object();
}

[[nodiscard]] bool isSafeRelativePath(const QString& path) {
    if (path.isEmpty() || QDir::isAbsolutePath(path) || path.contains(u'\\')) {
        return false;
    }

    const auto clean = QDir::cleanPath(path);
    return clean == path && clean != QStringLiteral("..") &&
           !clean.startsWith(QStringLiteral("../"));
}

[[nodiscard]] bool isNamespacedId(const QString& value) {
    static const QRegularExpression pattern(
        QStringLiteral(R"(^[a-z0-9]+(?:[.-][a-z0-9]+)+(?:[-.][a-z0-9]+)*$)"));
    return pattern.match(value).hasMatch();
}

[[nodiscard]] bool isUnitInterval(double value) { return value >= 0.0 && value <= 1.0; }

[[nodiscard]] auto parseRole(const QString& value) -> std::optional<model::CourtRole> {
    if (value == QStringLiteral("appellate")) {
        return model::CourtRole::Appellate;
    }
    if (value == QStringLiteral("district")) {
        return model::CourtRole::District;
    }
    return std::nullopt;
}

[[nodiscard]] auto parseJudge(const QByteArray& bytes, const QString& name)
    -> std::expected<model::JudgeProfile, Error> {
    const auto parsed = parseObject(bytes, name);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }

    const auto object = *parsed;
    if (object.value(QStringLiteral("schema_version")).toInt(-1) != supported_schema_version) {
        return fail(ErrorCode::InvalidJudgeProfile,
                    QStringLiteral("Unsupported judge profile schema in %1").arg(name));
    }

    const auto id = object.value(QStringLiteral("profile_id")).toString();
    const auto display_name = object.value(QStringLiteral("display_name")).toString();
    const auto role = parseRole(object.value(QStringLiteral("court_role")).toString());
    const auto interaction = object.value(QStringLiteral("interaction")).toObject();

    constexpr std::array fields{
        "directness",
        "formality",
        "interruption_frequency",
        "follow_up_depth",
        "hypothetical_frequency",
        "time_strictness",
    };
    if (!isNamespacedId(id) || display_name.isEmpty() || !role.has_value() ||
        interaction.size() != static_cast<qsizetype>(fields.size())) {
        return fail(ErrorCode::InvalidJudgeProfile,
                    QStringLiteral("Invalid judge profile fields in %1").arg(name));
    }

    for (const auto* field : fields) {
        const auto value = interaction.value(QLatin1StringView(field));
        if (!value.isDouble() || !isUnitInterval(value.toDouble())) {
            return fail(ErrorCode::InvalidJudgeProfile,
                        QStringLiteral("%1 must be between 0 and 1 in %2")
                            .arg(QLatin1StringView(field), name));
        }
    }

    return model::JudgeProfile{
        id.toStdString(),
        display_name.toStdString(),
        *role,
        model::InteractionStyle{
            interaction.value(QStringLiteral("directness")).toDouble(),
            interaction.value(QStringLiteral("formality")).toDouble(),
            interaction.value(QStringLiteral("interruption_frequency")).toDouble(),
            interaction.value(QStringLiteral("follow_up_depth")).toDouble(),
            interaction.value(QStringLiteral("hypothetical_frequency")).toDouble(),
            interaction.value(QStringLiteral("time_strictness")).toDouble(),
        },
    };
}

} // namespace

std::expected<LoadedPack, Error> PackReader::readDirectory(const QString& directory) {
    const QDir root(directory);
    const auto manifest_path = root.filePath(QStringLiteral("manifest.json"));
    const auto manifest_bytes = readFile(manifest_path);
    if (!manifest_bytes) {
        return std::unexpected(manifest_bytes.error());
    }

    const auto parsed = parseObject(*manifest_bytes, manifest_path);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    const auto manifest = *parsed;

    const auto schema_version = manifest.value(QStringLiteral("schema_version")).toInt(-1);
    if (schema_version != supported_schema_version) {
        return fail(ErrorCode::UnsupportedSchema,
                    QStringLiteral("Unsupported manifest schema version %1").arg(schema_version));
    }

    const auto pack_id = manifest.value(QStringLiteral("pack_id")).toString();
    const auto version = manifest.value(QStringLiteral("version")).toString();
    const auto contents = manifest.value(QStringLiteral("contents")).toArray();
    if (!isNamespacedId(pack_id) || version.isEmpty() || contents.isEmpty()) {
        return fail(
            ErrorCode::InvalidManifest,
            QStringLiteral("Manifest requires a namespaced pack_id, version, and contents"));
    }

    QSet<QString> content_ids;
    std::vector<model::JudgeProfile> judges;
    QCryptographicHash pack_hash(QCryptographicHash::Sha256);
    pack_hash.addData(pack_id.toUtf8());
    pack_hash.addData(hash_separator);
    pack_hash.addData(version.toUtf8());

    for (const auto& entry_value : contents) {
        if (!entry_value.isObject()) {
            return fail(ErrorCode::InvalidManifest,
                        QStringLiteral("Every content entry must be an object"));
        }
        const auto entry = entry_value.toObject();
        const auto content_id = entry.value(QStringLiteral("id")).toString();
        const auto kind = entry.value(QStringLiteral("kind")).toString();
        const auto relative_path = entry.value(QStringLiteral("path")).toString();
        const auto declared_digest = entry.value(QStringLiteral("sha256")).toString().toLower();

        if (!isNamespacedId(content_id) || kind != QStringLiteral("judge_profile") ||
            !isSafeRelativePath(relative_path) || declared_digest.size() != 64) {
            const auto code = isSafeRelativePath(relative_path) ? ErrorCode::InvalidManifest
                                                                : ErrorCode::UnsafePath;
            return fail(code, QStringLiteral("Invalid content entry %1").arg(content_id));
        }
        if (content_ids.contains(content_id)) {
            return fail(ErrorCode::DuplicateContentId,
                        QStringLiteral("Duplicate content id %1").arg(content_id));
        }
        content_ids.insert(content_id);

        const auto absolute_path = root.filePath(relative_path);
        const QFileInfo file_info(absolute_path);
        if (!file_info.isFile() || file_info.isSymLink()) {
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Content must be a regular file: %1").arg(relative_path));
        }

        const auto bytes = readFile(absolute_path);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        const auto actual_digest = QString::fromLatin1(
            QCryptographicHash::hash(*bytes, QCryptographicHash::Sha256).toHex());
        if (actual_digest != declared_digest) {
            return fail(ErrorCode::DigestMismatch,
                        QStringLiteral("Digest mismatch for %1").arg(relative_path));
        }

        const auto judge = parseJudge(*bytes, relative_path);
        if (!judge) {
            return std::unexpected(judge.error());
        }
        judges.push_back(*judge);

        pack_hash.addData(hash_separator);
        pack_hash.addData(content_id.toUtf8());
        pack_hash.addData(hash_separator);
        pack_hash.addData(actual_digest.toLatin1());
    }

    return LoadedPack{
        model::PackRevision{
            model::PackId{pack_id.toStdString()},
            version.toStdString(),
            QString::fromLatin1(pack_hash.result().toHex()).toStdString(),
        },
        std::move(judges),
    };
}

} // namespace appellate::packs
