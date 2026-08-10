#include "appellate/packs/pack_reader.hpp"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

namespace appellate::packs {
namespace {

constexpr auto supported_schema_version = 1;
constexpr qint64 maximum_manifest_bytes = 1024 * 1024;
constexpr qint64 maximum_resource_json_bytes = 8 * 1024 * 1024;
constexpr qsizetype maximum_capabilities = 128;
constexpr qsizetype maximum_dependencies = 128;
constexpr qsizetype maximum_contents = 10'000;
constexpr qsizetype maximum_directory_entries = 20'000;
constexpr qsizetype maximum_issue_focus_items = 32;
constexpr qsizetype maximum_jurisdictions = 64;

struct ContentDescriptor final {
    QString id;
    QString kind;
    int schema_version{};
    QString path;
    QString digest;
};

[[nodiscard]] auto fail(ErrorCode code, QString message) -> std::unexpected<Error> {
    return std::unexpected(Error{code, std::move(message)});
}

[[nodiscard]] bool hasExactKeys(const QJsonObject& object,
                                std::initializer_list<const char*> expected) {
    if (static_cast<std::size_t>(object.size()) != expected.size()) {
        return false;
    }
    return std::ranges::all_of(
        expected, [&object](const char* key) { return object.contains(QLatin1StringView(key)); });
}

[[nodiscard]] auto readFile(const QString& path, qint64 maximum_bytes)
    -> std::expected<QByteArray, Error> {
    const QFileInfo file_info(path);
    if (file_info.isSymLink()) {
        return fail(ErrorCode::UnsafePath,
                    QStringLiteral("Symbolic links are not allowed: %1").arg(path));
    }
    if (!file_info.isFile()) {
        return fail(ErrorCode::CannotRead,
                    QStringLiteral("Content must be a regular file: %1").arg(path));
    }
    if (file_info.size() < 0 || file_info.size() > maximum_bytes) {
        return fail(ErrorCode::ResourceTooLarge,
                    QStringLiteral("File exceeds its size limit: %1").arg(path));
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(ErrorCode::CannotRead, QStringLiteral("Cannot read %1").arg(path));
    }
    const auto bytes = file.read(maximum_bytes + 1);
    if (file.error() != QFileDevice::NoError) {
        return fail(ErrorCode::CannotRead, QStringLiteral("Cannot read %1").arg(path));
    }
    if (bytes.size() > maximum_bytes || !file.atEnd()) {
        return fail(ErrorCode::ResourceTooLarge,
                    QStringLiteral("File exceeds its size limit: %1").arg(path));
    }
    return bytes;
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

[[nodiscard]] bool isExactInteger(const QJsonValue& value, qint64 minimum, qint64 maximum) {
    if (!value.isDouble()) {
        return false;
    }
    const auto number = value.toDouble();
    return std::isfinite(number) && std::floor(number) == number &&
           number >= static_cast<double>(minimum) && number <= static_cast<double>(maximum);
}

[[nodiscard]] bool isNamespacedId(const QString& value) {
    static const QRegularExpression pattern(
        QStringLiteral(R"(^[a-z0-9]+(?:[.-][a-z0-9]+)+(?:[-.][a-z0-9]+)*$)"));
    return value.size() >= 3 && value.size() <= 128 && pattern.match(value).hasMatch();
}

[[nodiscard]] bool isSemanticVersion(const QString& value) {
    static const QRegularExpression pattern(QStringLiteral(
        R"(^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)(?:\.(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*))*)?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$)"));
    return value.size() >= 5 && value.size() <= 128 && pattern.match(value).hasMatch();
}

[[nodiscard]] bool isSha256(const QString& value) {
    static const QRegularExpression pattern(QStringLiteral(R"(^[a-f0-9]{64}$)"));
    return pattern.match(value).hasMatch();
}

[[nodiscard]] bool isReservedPathSegment(const QString& segment) {
    static const QSet<QString> reserved{
        QStringLiteral("con"),  QStringLiteral("prn"),  QStringLiteral("aux"),
        QStringLiteral("nul"),  QStringLiteral("com1"), QStringLiteral("com2"),
        QStringLiteral("com3"), QStringLiteral("com4"), QStringLiteral("com5"),
        QStringLiteral("com6"), QStringLiteral("com7"), QStringLiteral("com8"),
        QStringLiteral("com9"), QStringLiteral("lpt1"), QStringLiteral("lpt2"),
        QStringLiteral("lpt3"), QStringLiteral("lpt4"), QStringLiteral("lpt5"),
        QStringLiteral("lpt6"), QStringLiteral("lpt7"), QStringLiteral("lpt8"),
        QStringLiteral("lpt9"),
    };
    return reserved.contains(segment.section(u'.', 0, 0));
}

[[nodiscard]] bool isSafeRelativePath(const QString& path) {
    static const QRegularExpression pattern(
        QStringLiteral(R"(^[a-z0-9][a-z0-9._-]*(?:/[a-z0-9][a-z0-9._-]*)*$)"));
    if (path.isEmpty() || path.size() > 240 || QDir::isAbsolutePath(path) ||
        !pattern.match(path).hasMatch() || QDir::cleanPath(path) != path) {
        return false;
    }
    const auto segments = path.split(u'/');
    return std::ranges::none_of(segments, [](const QString& segment) {
        return segment.endsWith(u'.') || isReservedPathSegment(segment);
    });
}

[[nodiscard]] bool isDisplayName(const QString& value) {
    if (value.size() > 128 || value.trimmed().isEmpty()) {
        return false;
    }
    return std::ranges::all_of(
        value, [](QChar character) { return character.isPrint() || character == u' '; });
}

[[nodiscard]] bool isUnitInterval(const QJsonValue& value) {
    if (!value.isDouble()) {
        return false;
    }
    const auto number = value.toDouble();
    return std::isfinite(number) && number >= 0.0 && number <= 1.0;
}

[[nodiscard]] auto parseRole(const QString& value) -> std::optional<model::CourtRole> {
    if (value == QStringLiteral("appellate")) {
        return model::CourtRole::Appellate;
    }
    if (value == QStringLiteral("district")) {
        return model::CourtRole::District;
    }
    return std::nullopt;
}

[[nodiscard]] auto parseRegister(const QString& value) -> std::optional<model::VoiceRegister> {
    if (value == QStringLiteral("plain")) {
        return model::VoiceRegister::Plain;
    }
    if (value == QStringLiteral("formal")) {
        return model::VoiceRegister::Formal;
    }
    if (value == QStringLiteral("technical")) {
        return model::VoiceRegister::Technical;
    }
    return std::nullopt;
}

[[nodiscard]] auto parseCadence(const QString& value) -> std::optional<model::VoiceCadence> {
    if (value == QStringLiteral("clipped")) {
        return model::VoiceCadence::Clipped;
    }
    if (value == QStringLiteral("measured")) {
        return model::VoiceCadence::Measured;
    }
    if (value == QStringLiteral("expansive")) {
        return model::VoiceCadence::Expansive;
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
    if (!hasExactKeys(object, {"schema_version", "profile_id", "display_name", "profile_class",
                               "compatibility", "interaction", "voice"}) ||
        !isExactInteger(object.value(QStringLiteral("schema_version")), supported_schema_version,
                        supported_schema_version)) {
        return fail(
            ErrorCode::InvalidJudgeProfile,
            QStringLiteral("Unknown, missing, or invalid judge profile fields in %1").arg(name));
    }

    const auto id = object.value(QStringLiteral("profile_id")).toString();
    const auto display_name = object.value(QStringLiteral("display_name")).toString();
    if (!isNamespacedId(id) || !isDisplayName(display_name) ||
        object.value(QStringLiteral("profile_class")).toString() !=
            QStringLiteral("fictional_composite") ||
        !object.value(QStringLiteral("compatibility")).isObject() ||
        !object.value(QStringLiteral("interaction")).isObject() ||
        !object.value(QStringLiteral("voice")).isObject()) {
        return fail(ErrorCode::InvalidJudgeProfile,
                    QStringLiteral("Invalid judge profile fields in %1").arg(name));
    }

    const auto compatibility = object.value(QStringLiteral("compatibility")).toObject();
    if (!hasExactKeys(compatibility, {"court_roles", "jurisdiction_ids"}) ||
        !compatibility.value(QStringLiteral("court_roles")).isArray() ||
        !compatibility.value(QStringLiteral("jurisdiction_ids")).isArray()) {
        return fail(ErrorCode::InvalidJudgeProfile,
                    QStringLiteral("Invalid compatibility fields in %1").arg(name));
    }

    const auto role_values = compatibility.value(QStringLiteral("court_roles")).toArray();
    const auto jurisdiction_values =
        compatibility.value(QStringLiteral("jurisdiction_ids")).toArray();
    if (role_values.isEmpty() || role_values.size() > 2 || jurisdiction_values.isEmpty() ||
        jurisdiction_values.size() > maximum_jurisdictions) {
        return fail(ErrorCode::InvalidJudgeProfile,
                    QStringLiteral("Invalid compatibility bounds in %1").arg(name));
    }

    std::vector<model::CourtRole> roles;
    QSet<QString> role_ids;
    for (const auto& value : role_values) {
        const auto role_name = value.toString();
        const auto role = value.isString() ? parseRole(role_name) : std::nullopt;
        if (!role || role_ids.contains(role_name)) {
            return fail(ErrorCode::InvalidJudgeProfile,
                        QStringLiteral("Invalid or duplicate court role in %1").arg(name));
        }
        role_ids.insert(role_name);
        roles.push_back(*role);
    }

    std::vector<std::string> jurisdiction_ids;
    QSet<QString> jurisdiction_set;
    for (const auto& value : jurisdiction_values) {
        const auto jurisdiction_id = value.toString();
        if (!value.isString() || !isNamespacedId(jurisdiction_id) ||
            jurisdiction_set.contains(jurisdiction_id)) {
            return fail(ErrorCode::InvalidJudgeProfile,
                        QStringLiteral("Invalid or duplicate jurisdiction in %1").arg(name));
        }
        jurisdiction_set.insert(jurisdiction_id);
        jurisdiction_ids.push_back(jurisdiction_id.toStdString());
    }

    const auto interaction = object.value(QStringLiteral("interaction")).toObject();
    constexpr std::array scalar_fields{
        "directness",        "formality",
        "question_length",   "interruption_frequency",
        "follow_up_depth",   "hypothetical_frequency",
        "concession_recall", "time_strictness",
    };
    if (!hasExactKeys(interaction,
                      {"directness", "formality", "question_length", "interruption_frequency",
                       "follow_up_depth", "hypothetical_frequency", "concession_recall",
                       "time_strictness", "issue_focus"}) ||
        !interaction.value(QStringLiteral("issue_focus")).isArray()) {
        return fail(ErrorCode::InvalidJudgeProfile,
                    QStringLiteral("Invalid interaction fields in %1").arg(name));
    }
    for (const auto* field : scalar_fields) {
        if (!isUnitInterval(interaction.value(QLatin1StringView(field)))) {
            return fail(ErrorCode::InvalidJudgeProfile,
                        QStringLiteral("%1 must be between 0 and 1 in %2")
                            .arg(QLatin1StringView(field), name));
        }
    }

    const auto focus_values = interaction.value(QStringLiteral("issue_focus")).toArray();
    if (focus_values.isEmpty() || focus_values.size() > maximum_issue_focus_items) {
        return fail(ErrorCode::InvalidJudgeProfile,
                    QStringLiteral("Invalid issue-focus bounds in %1").arg(name));
    }
    std::vector<model::IssueFocus> focus;
    QSet<QString> focus_ids;
    for (const auto& value : focus_values) {
        if (!value.isObject()) {
            return fail(ErrorCode::InvalidJudgeProfile,
                        QStringLiteral("Issue focus must be an object in %1").arg(name));
        }
        const auto item = value.toObject();
        const auto topic_id = item.value(QStringLiteral("topic_id")).toString();
        if (!hasExactKeys(item, {"topic_id", "weight"}) || !isNamespacedId(topic_id) ||
            !isUnitInterval(item.value(QStringLiteral("weight"))) || focus_ids.contains(topic_id)) {
            return fail(ErrorCode::InvalidJudgeProfile,
                        QStringLiteral("Invalid or duplicate issue focus in %1").arg(name));
        }
        focus_ids.insert(topic_id);
        focus.push_back(model::IssueFocus{topic_id.toStdString(),
                                          item.value(QStringLiteral("weight")).toDouble()});
    }

    const auto voice = object.value(QStringLiteral("voice")).toObject();
    const auto register_style = parseRegister(voice.value(QStringLiteral("register")).toString());
    const auto cadence = parseCadence(voice.value(QStringLiteral("cadence")).toString());
    if (!hasExactKeys(voice, {"register", "cadence", "verbosity", "sentence_complexity"}) ||
        !register_style || !cadence || !isUnitInterval(voice.value(QStringLiteral("verbosity"))) ||
        !isUnitInterval(voice.value(QStringLiteral("sentence_complexity")))) {
        return fail(ErrorCode::InvalidJudgeProfile,
                    QStringLiteral("Invalid voice fields in %1").arg(name));
    }

    return model::JudgeProfile{
        id.toStdString(),
        display_name.toStdString(),
        model::ProfileClass::FictionalComposite,
        model::ProfileCompatibility{std::move(roles), std::move(jurisdiction_ids)},
        model::InteractionStyle{
            interaction.value(QStringLiteral("directness")).toDouble(),
            interaction.value(QStringLiteral("formality")).toDouble(),
            interaction.value(QStringLiteral("question_length")).toDouble(),
            interaction.value(QStringLiteral("interruption_frequency")).toDouble(),
            interaction.value(QStringLiteral("follow_up_depth")).toDouble(),
            interaction.value(QStringLiteral("hypothetical_frequency")).toDouble(),
            interaction.value(QStringLiteral("concession_recall")).toDouble(),
            interaction.value(QStringLiteral("time_strictness")).toDouble(),
            std::move(focus),
        },
        model::VoiceStyle{
            *register_style,
            *cadence,
            voice.value(QStringLiteral("verbosity")).toDouble(),
            voice.value(QStringLiteral("sentence_complexity")).toDouble(),
        },
    };
}

[[nodiscard]] auto validateRegularPath(const QDir& root, const QString& relative_path)
    -> std::expected<QString, Error> {
    auto current = root.absolutePath();
    const auto segments = relative_path.split(u'/');
    for (qsizetype index = 0; index < segments.size(); ++index) {
        current = QDir(current).filePath(segments.at(index));
        const QFileInfo info(current);
        if (info.isSymLink()) {
            return fail(ErrorCode::UnsafePath,
                        QStringLiteral("Symbolic links are not allowed: %1").arg(relative_path));
        }
        if (index + 1 < segments.size() && !info.isDir()) {
            return fail(ErrorCode::CannotRead,
                        QStringLiteral("Missing content directory: %1").arg(relative_path));
        }
    }
    const QFileInfo final_info(current);
    if (!final_info.isFile() || final_info.isSymLink()) {
        return fail(ErrorCode::CannotRead,
                    QStringLiteral("Content must be a regular file: %1").arg(relative_path));
    }
    return current;
}

[[nodiscard]] auto validateDeclaredFileSet(const QDir& root, const QSet<QString>& declared)
    -> std::expected<void, Error> {
    qsizetype entry_count = 0;
    QDirIterator iterator(root.absolutePath(),
                          QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        ++entry_count;
        if (entry_count > maximum_directory_entries) {
            return fail(ErrorCode::ResourceTooLarge,
                        QStringLiteral("Pack directory contains too many entries"));
        }
        const auto info = iterator.fileInfo();
        const auto relative_path =
            QDir::fromNativeSeparators(root.relativeFilePath(info.filePath()));
        if (info.isSymLink()) {
            return fail(ErrorCode::UnsafePath,
                        QStringLiteral("Symbolic links are not allowed: %1").arg(relative_path));
        }
        if (info.isDir()) {
            continue;
        }
        if (!info.isFile() || !declared.contains(relative_path)) {
            return fail(ErrorCode::UndeclaredFile,
                        QStringLiteral("Undeclared or unsupported file: %1").arg(relative_path));
        }
    }
    return {};
}

void addUint64(QCryptographicHash& hash, quint64 value) {
    std::array<char, 8> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto shift = static_cast<unsigned>((bytes.size() - index - 1U) * 8U);
        bytes.at(index) = static_cast<char>((value >> shift) & 0xffU);
    }
    hash.addData(QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())));
}

void addFrame(QCryptographicHash& hash, QByteArrayView value) {
    addUint64(hash, static_cast<quint64>(value.size()));
    hash.addData(value);
}

void addFrame(QCryptographicHash& hash, const QString& value) {
    const auto utf8 = value.toUtf8();
    addFrame(hash, QByteArrayView(utf8));
}

[[nodiscard]] auto canonicalDigest(const QString& pack_id, const QString& version,
                                   std::vector<model::RequiredCapability> capabilities,
                                   std::vector<model::PackDependency> dependencies,
                                   std::vector<ContentDescriptor> contents) -> std::string {
    std::ranges::sort(capabilities, {}, &model::RequiredCapability::id);
    std::ranges::sort(dependencies, [](const auto& left, const auto& right) {
        return std::tie(left.revision.id.value, left.revision.version, left.revision.digest) <
               std::tie(right.revision.id.value, right.revision.version, right.revision.digest);
    });
    std::ranges::sort(contents, [](const auto& left, const auto& right) {
        return std::tie(left.id, left.kind, left.path) < std::tie(right.id, right.kind, right.path);
    });

    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, QByteArrayView("appellate-workbench-pack-revision-v1"));
    addUint64(hash, supported_schema_version);
    addFrame(hash, pack_id);
    addFrame(hash, version);

    addUint64(hash, static_cast<quint64>(capabilities.size()));
    for (const auto& capability : capabilities) {
        addFrame(hash, QString::fromStdString(capability.id));
        addUint64(hash, capability.version);
    }
    addUint64(hash, static_cast<quint64>(dependencies.size()));
    for (const auto& dependency : dependencies) {
        addFrame(hash, QString::fromStdString(dependency.revision.id.value));
        addFrame(hash, QString::fromStdString(dependency.revision.version));
        addFrame(hash, QString::fromStdString(dependency.revision.digest));
    }
    addUint64(hash, static_cast<quint64>(contents.size()));
    for (const auto& content : contents) {
        addFrame(hash, content.id);
        addFrame(hash, content.kind);
        addUint64(hash, static_cast<quint64>(content.schema_version));
        addFrame(hash, content.path);
        addFrame(hash, content.digest);
    }
    return QString::fromLatin1(hash.result().toHex()).toStdString();
}

} // namespace

std::expected<LoadedPack, Error> PackReader::readDirectory(const QString& directory) {
    const QFileInfo root_info(directory);
    if (!root_info.isDir() || root_info.isSymLink()) {
        return fail(ErrorCode::UnsafePath,
                    QStringLiteral("Pack root must be a real directory: %1").arg(directory));
    }
    const QDir root(root_info.absoluteFilePath());
    const auto manifest_path = root.filePath(QStringLiteral("manifest.json"));
    const auto manifest_bytes = readFile(manifest_path, maximum_manifest_bytes);
    if (!manifest_bytes) {
        return std::unexpected(manifest_bytes.error());
    }
    const auto parsed = parseObject(*manifest_bytes, manifest_path);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    const auto manifest = *parsed;
    if (!isExactInteger(manifest.value(QStringLiteral("schema_version")), supported_schema_version,
                        supported_schema_version)) {
        return fail(ErrorCode::UnsupportedSchema,
                    QStringLiteral("Unsupported manifest schema version"));
    }
    if (!hasExactKeys(manifest, {"schema_version", "pack_id", "version", "required_capabilities",
                                 "dependencies", "contents"}) ||
        !manifest.value(QStringLiteral("required_capabilities")).isArray() ||
        !manifest.value(QStringLiteral("dependencies")).isArray() ||
        !manifest.value(QStringLiteral("contents")).isArray()) {
        return fail(ErrorCode::InvalidManifest,
                    QStringLiteral("Manifest contains unknown, missing, or invalid fields"));
    }

    const auto pack_id = manifest.value(QStringLiteral("pack_id")).toString();
    const auto version = manifest.value(QStringLiteral("version")).toString();
    const auto capability_values =
        manifest.value(QStringLiteral("required_capabilities")).toArray();
    const auto dependency_values = manifest.value(QStringLiteral("dependencies")).toArray();
    const auto content_values = manifest.value(QStringLiteral("contents")).toArray();
    if (!isNamespacedId(pack_id) || !isSemanticVersion(version) ||
        capability_values.size() > maximum_capabilities ||
        dependency_values.size() > maximum_dependencies || content_values.isEmpty() ||
        content_values.size() > maximum_contents) {
        return fail(ErrorCode::InvalidManifest,
                    QStringLiteral("Manifest identifiers, version, or array bounds are invalid"));
    }

    std::vector<model::RequiredCapability> capabilities;
    QSet<QString> capability_ids;
    for (const auto& value : capability_values) {
        if (!value.isObject()) {
            return fail(ErrorCode::InvalidManifest,
                        QStringLiteral("Every capability must be an object"));
        }
        const auto object = value.toObject();
        const auto id = object.value(QStringLiteral("id")).toString();
        const auto capability_version = object.value(QStringLiteral("version"));
        if (!hasExactKeys(object, {"id", "version"}) || !isNamespacedId(id) ||
            !isExactInteger(capability_version, 1, 65'535) || capability_ids.contains(id)) {
            return fail(ErrorCode::InvalidManifest,
                        QStringLiteral("Invalid or duplicate required capability"));
        }
        capability_ids.insert(id);
        capabilities.push_back(model::RequiredCapability{
            id.toStdString(), static_cast<std::uint32_t>(capability_version.toDouble())});
    }

    std::vector<model::PackDependency> dependencies;
    QSet<QString> dependency_ids;
    for (const auto& value : dependency_values) {
        if (!value.isObject()) {
            return fail(ErrorCode::InvalidManifest,
                        QStringLiteral("Every dependency must be an object"));
        }
        const auto object = value.toObject();
        const auto id = object.value(QStringLiteral("pack_id")).toString();
        const auto dependency_version = object.value(QStringLiteral("version")).toString();
        const auto digest = object.value(QStringLiteral("sha256")).toString();
        if (!hasExactKeys(object, {"pack_id", "version", "sha256"}) || !isNamespacedId(id) ||
            !isSemanticVersion(dependency_version) || !isSha256(digest) || id == pack_id ||
            dependency_ids.contains(id)) {
            return fail(ErrorCode::InvalidManifest,
                        QStringLiteral("Invalid, duplicate, or self-referential dependency"));
        }
        dependency_ids.insert(id);
        dependencies.push_back(model::PackDependency{
            model::PackRevision{model::PackId{id.toStdString()}, dependency_version.toStdString(),
                                digest.toStdString()}});
    }

    std::vector<ContentDescriptor> contents;
    QSet<QString> content_ids;
    QSet<QString> content_paths;
    QSet<QString> declared_files{QStringLiteral("manifest.json")};
    for (const auto& value : content_values) {
        if (!value.isObject()) {
            return fail(ErrorCode::InvalidManifest,
                        QStringLiteral("Every content entry must be an object"));
        }
        const auto object = value.toObject();
        const auto id = object.value(QStringLiteral("id")).toString();
        const auto kind = object.value(QStringLiteral("kind")).toString();
        const auto path = object.value(QStringLiteral("path")).toString();
        const auto digest = object.value(QStringLiteral("sha256")).toString();
        const auto content_schema = object.value(QStringLiteral("schema_version"));
        if (!hasExactKeys(object, {"id", "kind", "schema_version", "path", "sha256"}) ||
            !isNamespacedId(id) || kind != QStringLiteral("judge_profile") ||
            !isExactInteger(content_schema, supported_schema_version, supported_schema_version) ||
            !isSafeRelativePath(path) || path == QStringLiteral("manifest.json") ||
            !isSha256(digest)) {
            const auto code =
                isSafeRelativePath(path) ? ErrorCode::InvalidManifest : ErrorCode::UnsafePath;
            return fail(code, QStringLiteral("Invalid content entry %1").arg(id));
        }
        if (content_ids.contains(id)) {
            return fail(ErrorCode::DuplicateContentId,
                        QStringLiteral("Duplicate content id %1").arg(id));
        }
        if (content_paths.contains(path)) {
            return fail(ErrorCode::DuplicateContentPath,
                        QStringLiteral("Duplicate content path %1").arg(path));
        }
        content_ids.insert(id);
        content_paths.insert(path);
        declared_files.insert(path);
        contents.push_back(ContentDescriptor{id, kind, supported_schema_version, path, digest});
    }

    const auto file_set_result = validateDeclaredFileSet(root, declared_files);
    if (!file_set_result) {
        return std::unexpected(file_set_result.error());
    }

    std::vector<model::JudgeProfile> judges;
    QSet<QString> payload_ids;
    for (const auto& content : contents) {
        const auto absolute_path = validateRegularPath(root, content.path);
        if (!absolute_path) {
            return std::unexpected(absolute_path.error());
        }
        const auto bytes = readFile(*absolute_path, maximum_resource_json_bytes);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        const auto actual_digest = QString::fromLatin1(
            QCryptographicHash::hash(*bytes, QCryptographicHash::Sha256).toHex());
        if (actual_digest != content.digest) {
            return fail(ErrorCode::DigestMismatch,
                        QStringLiteral("Digest mismatch for %1").arg(content.path));
        }
        const auto judge = parseJudge(*bytes, content.path);
        if (!judge) {
            return std::unexpected(judge.error());
        }
        const auto payload_id = QString::fromStdString(judge->id);
        if (payload_ids.contains(payload_id)) {
            return fail(ErrorCode::DuplicatePayloadId,
                        QStringLiteral("Duplicate payload id %1").arg(payload_id));
        }
        payload_ids.insert(payload_id);
        if (payload_id != content.id) {
            return fail(ErrorCode::InvalidJudgeProfile,
                        QStringLiteral("Manifest id %1 does not match payload id %2")
                            .arg(content.id, payload_id));
        }
        judges.push_back(*judge);
    }
    std::ranges::sort(judges, {}, &model::JudgeProfile::id);

    return LoadedPack{
        model::PackRevision{
            model::PackId{pack_id.toStdString()}, version.toStdString(),
            canonicalDigest(pack_id, version, capabilities, dependencies, contents)},
        std::move(capabilities),
        std::move(dependencies),
        std::move(judges),
    };
}

} // namespace appellate::packs
