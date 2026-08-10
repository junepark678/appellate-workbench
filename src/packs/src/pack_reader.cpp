#include "appellate/packs/pack_reader.hpp"
#include "appellate/packs/schema_validator.hpp"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
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
constexpr quint64 maximum_blob_bytes = 512ULL * 1024ULL * 1024ULL;
constexpr quint64 maximum_total_blob_bytes = 3ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr qsizetype maximum_issue_focus_items = 32;
constexpr qsizetype maximum_jurisdictions = 64;
constexpr qsizetype blob_stream_buffer_bytes = 64 * 1024;
constexpr qsizetype pdf_tail_bytes = 1024;

struct ContentDescriptor final {
    QString id;
    QString kind;
    int schema_version{};
    QString path;
    QString digest;
};

struct KindDefinition final {
    model::ResourceKind kind;
    QString schema_file;
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

[[nodiscard]] std::optional<QString> overlappingPath(const std::vector<QString>& paths) {
    QSet<QString> declared;
    declared.reserve(static_cast<qsizetype>(paths.size()));
    for (const auto& path : paths) {
        if (declared.contains(path)) {
            return path;
        }
        declared.insert(path);
    }
    for (const auto& path : paths) {
        auto separator = path.indexOf(u'/');
        while (separator >= 0) {
            if (declared.contains(path.first(separator))) {
                return path;
            }
            separator = path.indexOf(u'/', separator + 1);
        }
    }
    return std::nullopt;
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

[[nodiscard]] auto kindDefinition(const QString& kind) -> std::optional<KindDefinition> {
    if (kind == QStringLiteral("argument_config")) {
        return KindDefinition{model::ResourceKind::ArgumentConfig,
                              QStringLiteral("argument-config.schema.json")};
    }
    if (kind == QStringLiteral("authority_set")) {
        return KindDefinition{model::ResourceKind::AuthoritySet,
                              QStringLiteral("authority-set.schema.json")};
    }
    if (kind == QStringLiteral("bench_configuration")) {
        return KindDefinition{model::ResourceKind::BenchConfiguration,
                              QStringLiteral("bench-configuration.schema.json")};
    }
    if (kind == QStringLiteral("case")) {
        return KindDefinition{model::ResourceKind::Case, QStringLiteral("case.schema.json")};
    }
    if (kind == QStringLiteral("court")) {
        return KindDefinition{model::ResourceKind::Court, QStringLiteral("court.schema.json")};
    }
    if (kind == QStringLiteral("filing_catalog")) {
        return KindDefinition{model::ResourceKind::FilingCatalog,
                              QStringLiteral("filing-catalog.schema.json")};
    }
    if (kind == QStringLiteral("form")) {
        return KindDefinition{model::ResourceKind::Form, QStringLiteral("form.schema.json")};
    }
    if (kind == QStringLiteral("judge_profile")) {
        return KindDefinition{model::ResourceKind::JudgeProfile,
                              QStringLiteral("judge-profile.schema.json")};
    }
    if (kind == QStringLiteral("procedure_profile")) {
        return KindDefinition{model::ResourceKind::ProcedureProfile,
                              QStringLiteral("procedure-profile.schema.json")};
    }
    if (kind == QStringLiteral("realism_review")) {
        return KindDefinition{model::ResourceKind::RealismReview,
                              QStringLiteral("realism-review.schema.json")};
    }
    if (kind == QStringLiteral("record")) {
        return KindDefinition{model::ResourceKind::Record, QStringLiteral("record.schema.json")};
    }
    if (kind == QStringLiteral("workflow")) {
        return KindDefinition{model::ResourceKind::Workflow,
                              QStringLiteral("workflow.schema.json")};
    }
    return std::nullopt;
}

[[nodiscard]] auto parseJudge(const QJsonObject& object, const QString& name)
    -> std::expected<model::JudgeProfile, Error> {
    if (!hasExactKeys(object, {"schema_version", "resource_kind", "resource_id", "display_name",
                               "profile_class", "compatibility", "interaction", "voice"}) ||
        !isExactInteger(object.value(QStringLiteral("schema_version")), supported_schema_version,
                        supported_schema_version) ||
        object.value(QStringLiteral("resource_kind")).toString() !=
            QStringLiteral("judge_profile")) {
        return fail(
            ErrorCode::InvalidJudgeProfile,
            QStringLiteral("Unknown, missing, or invalid judge profile fields in %1").arg(name));
    }

    const auto id = object.value(QStringLiteral("resource_id")).toString();
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

[[nodiscard]] bool isPdfWhitespace(char value) {
    switch (static_cast<unsigned char>(value)) {
    case 0x00:
    case 0x09:
    case 0x0a:
    case 0x0c:
    case 0x0d:
    case 0x20:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool hasPdfSignature(QByteArrayView header) {
    return header.size() >= 8 && header.first(5) == QByteArrayView("%PDF-") &&
           (header.at(5) == '1' || header.at(5) == '2') && header.at(6) == '.' &&
           header.at(7) >= '0' && header.at(7) <= '9';
}

[[nodiscard]] bool hasPdfTrailer(QByteArrayView tail) {
    auto end = tail.size();
    while (end > 0 && isPdfWhitespace(tail.at(end - 1))) {
        --end;
    }
    return end >= 5 && tail.sliced(end - 5, 5) == QByteArrayView("%%EOF");
}

[[nodiscard]] auto validateBlobFile(const QString& absolute_path,
                                    const model::BlobDescriptor& descriptor)
    -> std::expected<void, Error> {
    const QFileInfo info(absolute_path);
    if (info.size() < 0 || static_cast<quint64>(info.size()) != descriptor.byte_size) {
        return fail(ErrorCode::DigestMismatch, QStringLiteral("Blob size mismatch for %1")
                                                   .arg(QString::fromStdString(descriptor.path)));
    }

    QFile file(absolute_path);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(
            ErrorCode::CannotRead,
            QStringLiteral("Cannot read blob %1").arg(QString::fromStdString(descriptor.path)));
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::array<char, blob_stream_buffer_bytes> buffer{};
    QByteArray header;
    QByteArray tail;
    header.reserve(8);
    tail.reserve(pdf_tail_bytes);
    quint64 total = 0;
    while (true) {
        const auto read_size = file.read(buffer.data(), static_cast<qint64>(buffer.size()));
        if (read_size < 0) {
            return fail(ErrorCode::CannotRead, QStringLiteral("Cannot read complete blob %1")
                                                   .arg(QString::fromStdString(descriptor.path)));
        }
        if (read_size == 0) {
            break;
        }
        const auto chunk_size = static_cast<quint64>(read_size);
        if (chunk_size > descriptor.byte_size || total > descriptor.byte_size - chunk_size) {
            return fail(ErrorCode::DigestMismatch,
                        QStringLiteral("Blob size mismatch for %1")
                            .arg(QString::fromStdString(descriptor.path)));
        }
        const QByteArrayView chunk(buffer.data(), read_size);
        if (header.size() < 8) {
            const auto needed = 8 - header.size();
            header.append(chunk.first(std::min(needed, chunk.size())));
        }
        tail.append(chunk);
        if (tail.size() > pdf_tail_bytes) {
            tail.remove(0, tail.size() - pdf_tail_bytes);
        }
        hash.addData(chunk);
        total += chunk_size;
    }
    if (file.error() != QFileDevice::NoError) {
        return fail(ErrorCode::CannotRead, QStringLiteral("Cannot read complete blob %1")
                                               .arg(QString::fromStdString(descriptor.path)));
    }
    if (total != descriptor.byte_size) {
        return fail(ErrorCode::DigestMismatch, QStringLiteral("Blob size mismatch for %1")
                                                   .arg(QString::fromStdString(descriptor.path)));
    }
    const auto actual_digest = QString::fromLatin1(hash.result().toHex()).toStdString();
    if (actual_digest != descriptor.sha256) {
        return fail(
            ErrorCode::DigestMismatch,
            QStringLiteral("Digest mismatch for %1").arg(QString::fromStdString(descriptor.path)));
    }
    if (descriptor.media_type != "application/pdf" || !hasPdfSignature(QByteArrayView(header)) ||
        !hasPdfTrailer(QByteArrayView(tail))) {
        return fail(ErrorCode::InvalidManifest,
                    QStringLiteral("Blob is not a structurally recognizable PDF: %1")
                        .arg(QString::fromStdString(descriptor.path)));
    }
    return {};
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
                                   std::vector<ContentDescriptor> contents,
                                   std::vector<model::BlobDescriptor> blobs) -> std::string {
    std::ranges::sort(capabilities, {}, &model::RequiredCapability::id);
    std::ranges::sort(dependencies, [](const auto& left, const auto& right) {
        return std::tie(left.revision.id.value, left.revision.version, left.revision.digest) <
               std::tie(right.revision.id.value, right.revision.version, right.revision.digest);
    });
    std::ranges::sort(contents, [](const auto& left, const auto& right) {
        return std::tie(left.id, left.kind, left.path) < std::tie(right.id, right.kind, right.path);
    });
    std::ranges::sort(blobs, [](const auto& left, const auto& right) {
        return std::tie(left.path, left.media_type, left.byte_size, left.sha256) <
               std::tie(right.path, right.media_type, right.byte_size, right.sha256);
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
    addUint64(hash, static_cast<quint64>(blobs.size()));
    for (const auto& blob : blobs) {
        addFrame(hash, QString::fromStdString(blob.path));
        addFrame(hash, QString::fromStdString(blob.media_type));
        addUint64(hash, blob.byte_size);
        addFrame(hash, QString::fromStdString(blob.sha256));
    }
    return QString::fromLatin1(hash.result().toHex()).toStdString();
}

[[nodiscard]] auto crossReferenceFailure(const ValidatedResource& resource, QString field,
                                         QString detail) -> std::unexpected<Error> {
    return fail(ErrorCode::CrossReferenceFailure,
                QStringLiteral("Resource %1 has invalid reference %2: %3")
                    .arg(QString::fromStdString(resource.descriptor.id), std::move(field),
                         std::move(detail)));
}

[[nodiscard]] QSet<QString> stringSet(const QJsonArray& values) {
    QSet<QString> result;
    for (const auto& value : values) {
        result.insert(value.toString());
    }
    return result;
}

[[nodiscard]] auto validateResourceGraph(const std::vector<ValidatedResource>& resources,
                                         const std::vector<model::BlobDescriptor>& blobs)
    -> std::expected<void, Error> {
    QHash<QString, const ValidatedResource*> by_id;
    for (const auto& resource : resources) {
        const auto id = QString::fromStdString(resource.descriptor.id);
        if (by_id.contains(id)) {
            return crossReferenceFailure(resource, QStringLiteral("resource_id"),
                                         QStringLiteral("resource identifiers must be unique"));
        }
        by_id.insert(id, &resource);
    }

    QHash<QString, const model::BlobDescriptor*> blobs_by_path;
    for (const auto& blob : blobs) {
        const auto path = QString::fromStdString(blob.path);
        if (blobs_by_path.contains(path)) {
            return fail(ErrorCode::CrossReferenceFailure,
                        QStringLiteral("Blob path resolves more than once: %1").arg(path));
        }
        blobs_by_path.insert(path, &blob);
    }
    QSet<QString> referenced_blob_paths;

    const auto requireKind =
        [&by_id](const ValidatedResource& owner, const QString& field, const QString& id,
                 model::ResourceKind expected) -> std::expected<const ValidatedResource*, Error> {
        const auto found = by_id.constFind(id);
        if (found == by_id.constEnd()) {
            return crossReferenceFailure(owner, field, QStringLiteral("unresolved id %1").arg(id));
        }
        if ((*found)->descriptor.kind != expected) {
            return crossReferenceFailure(
                owner, field, QStringLiteral("%1 resolves to the wrong resource kind").arg(id));
        }
        return *found;
    };

    QSet<QString> authority_ids;
    QHash<QString, QJsonObject> authorities_by_id;
    QSet<QString> filing_ids;
    QHash<QString, QSet<QString>> catalog_filings;
    QHash<QString, QSet<QString>> filing_required_fields;
    QHash<QString, QSet<QString>> filing_authorized_roles;
    QHash<QString, QSet<QString>> form_fields_by_filing;
    QHash<QString, QSet<QString>> workflow_stages;
    QHash<QString, QSet<QString>> workflow_operations;
    QHash<QString, QSet<QString>> record_entries;
    QHash<QString, QSet<QString>> case_issues;
    QHash<QString, QSet<QString>> catalog_roles;

    for (const auto& resource : resources) {
        const auto id = QString::fromStdString(resource.descriptor.id);
        const auto& document = resource.document;
        switch (resource.descriptor.kind) {
        case model::ResourceKind::AuthoritySet:
            for (const auto& value : document.value(QStringLiteral("authorities")).toArray()) {
                const auto authority_id =
                    value.toObject().value(QStringLiteral("authority_id")).toString();
                if (authority_ids.contains(authority_id)) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("authorities"),
                        QStringLiteral("duplicate authority id %1").arg(authority_id));
                }
                authority_ids.insert(authority_id);
                authorities_by_id.insert(authority_id, value.toObject());
            }
            break;
        case model::ResourceKind::FilingCatalog:
            for (const auto& value : document.value(QStringLiteral("filings")).toArray()) {
                const auto filing = value.toObject();
                const auto filing_id = filing.value(QStringLiteral("filing_id")).toString();
                if (filing_ids.contains(filing_id)) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("filings"),
                        QStringLiteral("duplicate filing id %1").arg(filing_id));
                }
                filing_ids.insert(filing_id);
                catalog_filings[id].insert(filing_id);
                filing_required_fields.insert(
                    filing_id,
                    stringSet(filing.value(QStringLiteral("required_field_ids")).toArray()));
                filing_authorized_roles.insert(
                    filing_id, stringSet(filing.value(QStringLiteral("actor_role_ids")).toArray()));
            }
            break;
        case model::ResourceKind::Form: {
            const auto filing_id = document.value(QStringLiteral("filing_id")).toString();
            auto& fields = form_fields_by_filing[filing_id];
            for (const auto& value : document.value(QStringLiteral("fields")).toArray()) {
                const auto field = value.toObject();
                const auto field_id = field.value(QStringLiteral("field_id")).toString();
                if (fields.contains(field_id)) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("fields"),
                        QStringLiteral("duplicate field id %1 for filing %2")
                            .arg(field_id, filing_id));
                }
                fields.insert(field_id);
                const auto is_choice = field.value(QStringLiteral("value_type")).toString() ==
                                       QStringLiteral("choice");
                if (is_choice != field.contains(QStringLiteral("choices"))) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("fields/choices"),
                        QStringLiteral("choices are required only for choice fields"));
                }
            }
            break;
        }
        case model::ResourceKind::Workflow: {
            const auto stages = stringSet(document.value(QStringLiteral("stages")).toArray());
            if (!stages.contains(document.value(QStringLiteral("initial_stage_id")).toString())) {
                return crossReferenceFailure(resource, QStringLiteral("initial_stage_id"),
                                             QStringLiteral("stage is not declared"));
            }
            QSet<QString> operations;
            QHash<QString, QJsonObject> operation_documents;
            for (const auto& value : document.value(QStringLiteral("operations")).toArray()) {
                const auto operation = value.toObject();
                const auto operation_id =
                    operation.value(QStringLiteral("operation_id")).toString();
                if (operations.contains(operation_id)) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("operations"),
                        QStringLiteral("duplicate operation id %1").arg(operation_id));
                }
                operations.insert(operation_id);
                operation_documents.insert(operation_id, operation);
                if (!stages.contains(operation.value(QStringLiteral("stage_id")).toString())) {
                    return crossReferenceFailure(resource, QStringLiteral("operations/stage_id"),
                                                 QStringLiteral("stage is not declared"));
                }
                if (operation.contains(QStringLiteral("next_stage_id")) &&
                    !stages.contains(operation.value(QStringLiteral("next_stage_id")).toString())) {
                    return crossReferenceFailure(resource,
                                                 QStringLiteral("operations/next_stage_id"),
                                                 QStringLiteral("stage is not declared"));
                }
                const auto has_days = operation.contains(QStringLiteral("deadline_days"));
                const auto has_counting = operation.contains(QStringLiteral("deadline_counting"));
                if (has_days != has_counting) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("operations/deadline"),
                        QStringLiteral("deadline_days and deadline_counting must appear together"));
                }
                const auto opcode = operation.value(QStringLiteral("opcode")).toString();
                if (opcode == QStringLiteral("calculate_deadline") && !has_days) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("operations/deadline_days"),
                        QStringLiteral("calculate_deadline requires a complete deadline rule"));
                }
                if (opcode != QStringLiteral("calculate_deadline") &&
                    opcode != QStringLiteral("enter_order") && has_days) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("operations/deadline_days"),
                        QStringLiteral("this opcode cannot carry a deadline rule"));
                }
                if (opcode == QStringLiteral("advance_stage") &&
                    !operation.contains(QStringLiteral("next_stage_id"))) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("operations/next_stage_id"),
                        QStringLiteral("advance_stage requires a next stage"));
                }
                static const QSet<QString> court_opcodes{
                    QStringLiteral("enter_order"),       QStringLiteral("set_sealed"),
                    QStringLiteral("schedule_argument"), QStringLiteral("issue_judgment"),
                    QStringLiteral("issue_mandate"),
                };
                if (court_opcodes.contains(opcode) &&
                    operation.value(QStringLiteral("authorized_role_ids")).toArray().isEmpty()) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("operations/authorized_role_ids"),
                        QStringLiteral("court operations require an authorized role"));
                }
            }

            const auto operationForId = [&operation_documents](const QString& operation_id,
                                                               const QString& opcode,
                                                               const QString& stage_id) {
                const auto found = operation_documents.constFind(operation_id);
                return found != operation_documents.constEnd() &&
                       found->value(QStringLiteral("opcode")).toString() == opcode &&
                       found->value(QStringLiteral("stage_id")).toString() == stage_id;
            };
            QSet<QString> route_keys;
            QSet<QString> declared_deadline_ids;
            QSet<QString> accepted_deadline_ids;
            const auto filing_routes = document.value(QStringLiteral("filing_routes")).toArray();
            if (filing_routes.isEmpty()) {
                return crossReferenceFailure(
                    resource, QStringLiteral("filing_routes"),
                    QStringLiteral("a runnable workflow requires an executable filing route"));
            }
            for (const auto& value : filing_routes) {
                const auto route = value.toObject();
                const auto stage_id = route.value(QStringLiteral("stage_id")).toString();
                const auto filing_type_id =
                    route.value(QStringLiteral("filing_type_id")).toString();
                const auto route_key = stage_id + u'|' + filing_type_id;
                if (!stages.contains(stage_id) || route_keys.contains(route_key)) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("filing_routes"),
                        QStringLiteral(
                            "route stage and filing type pairs must be valid and unique"));
                }
                route_keys.insert(route_key);
                if (!operationForId(route.value(QStringLiteral("accept_operation_id")).toString(),
                                    QStringLiteral("accept_filing"), stage_id) ||
                    !operationForId(route.value(QStringLiteral("reject_operation_id")).toString(),
                                    QStringLiteral("reject_filing"), stage_id)) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("filing_routes"),
                        QStringLiteral("route accept and reject operations are incompatible"));
                }
                const auto has_deficiency_operation =
                    route.contains(QStringLiteral("deficiency_operation_id"));
                const auto has_deficiency_deadline =
                    route.contains(QStringLiteral("deficiency_deadline"));
                if ((has_deficiency_operation &&
                     !operationForId(
                         route.value(QStringLiteral("deficiency_operation_id")).toString(),
                         QStringLiteral("issue_deficiency"), stage_id)) ||
                    (has_deficiency_deadline && !has_deficiency_operation)) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("filing_routes/deficiency"),
                        QStringLiteral("route deficiency behavior is incompatible"));
                }
                const auto validateDeadlinePlan =
                    [&](const QJsonObject& plan) -> std::expected<void, Error> {
                    const auto deadline_id = plan.value(QStringLiteral("deadline_id")).toString();
                    if (declared_deadline_ids.contains(deadline_id) ||
                        !operationForId(plan.value(QStringLiteral("operation_id")).toString(),
                                        QStringLiteral("calculate_deadline"), stage_id)) {
                        return crossReferenceFailure(
                            resource, QStringLiteral("filing_routes/deadline"),
                            QStringLiteral(
                                "deadline ids must be unique and use a local calculation"));
                    }
                    declared_deadline_ids.insert(deadline_id);
                    return {};
                };
                if (has_deficiency_deadline) {
                    const auto deficiency_plan = validateDeadlinePlan(
                        route.value(QStringLiteral("deficiency_deadline")).toObject());
                    if (!deficiency_plan) {
                        return std::unexpected(deficiency_plan.error());
                    }
                }
                if (route.contains(QStringLiteral("accepted_deadline"))) {
                    const auto accepted_plan =
                        route.value(QStringLiteral("accepted_deadline")).toObject();
                    const auto accepted_result = validateDeadlinePlan(accepted_plan);
                    if (!accepted_result) {
                        return std::unexpected(accepted_result.error());
                    }
                    accepted_deadline_ids.insert(
                        accepted_plan.value(QStringLiteral("deadline_id")).toString());
                }
                if (route.contains(QStringLiteral("advance_operation_id")) &&
                    !operationForId(route.value(QStringLiteral("advance_operation_id")).toString(),
                                    QStringLiteral("advance_stage"), stage_id)) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("filing_routes/advance_operation_id"),
                        QStringLiteral("advance operation is incompatible with the route"));
                }
            }
            for (const auto& value : filing_routes) {
                const auto route = value.toObject();
                if (route.contains(QStringLiteral("satisfies_deadline_id")) &&
                    !accepted_deadline_ids.contains(
                        route.value(QStringLiteral("satisfies_deadline_id")).toString())) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("filing_routes/satisfies_deadline_id"),
                        QStringLiteral("satisfied deadline is not produced by this workflow"));
                }
            }
            workflow_stages.insert(id, stages);
            workflow_operations.insert(id, operations);
            break;
        }
        case model::ResourceKind::Record: {
            QSet<QString> entries;
            QSet<int> entry_numbers;
            for (const auto& value : document.value(QStringLiteral("docket_entries")).toArray()) {
                const auto entry = value.toObject();
                const auto entry_id = entry.value(QStringLiteral("entry_id")).toString();
                const auto entry_number = entry.value(QStringLiteral("entry_number")).toInt();
                const auto asset_path = entry.value(QStringLiteral("asset_path")).toString();
                const auto asset_digest = entry.value(QStringLiteral("asset_sha256")).toString();
                if (entries.contains(entry_id) || entry_numbers.contains(entry_number)) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("docket_entries"),
                        QStringLiteral("entry ids and numbers must be unique"));
                }
                entries.insert(entry_id);
                entry_numbers.insert(entry_number);
                const auto blob = blobs_by_path.constFind(asset_path);
                if (blob == blobs_by_path.constEnd()) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("docket_entries/asset_path"),
                        QStringLiteral("unresolved blob path %1").arg(asset_path));
                }
                if (QString::fromStdString((*blob)->sha256) != asset_digest) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("docket_entries/asset_sha256"),
                        QStringLiteral("digest does not match blob %1").arg(asset_path));
                }
                referenced_blob_paths.insert(asset_path);
            }
            record_entries.insert(id, entries);
            break;
        }
        case model::ResourceKind::Case: {
            QSet<QString> actor_ids;
            for (const auto& value : document.value(QStringLiteral("actors")).toArray()) {
                const auto actor_id = value.toObject().value(QStringLiteral("actor_id")).toString();
                if (actor_ids.contains(actor_id)) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("actors"),
                        QStringLiteral("duplicate actor id %1").arg(actor_id));
                }
                actor_ids.insert(actor_id);
            }
            QSet<QString> issues;
            for (const auto& value : document.value(QStringLiteral("issues")).toArray()) {
                const auto issue_id = value.toObject().value(QStringLiteral("issue_id")).toString();
                if (issues.contains(issue_id)) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("issues"),
                        QStringLiteral("duplicate issue id %1").arg(issue_id));
                }
                issues.insert(issue_id);
            }
            case_issues.insert(id, issues);
            break;
        }
        case model::ResourceKind::BenchConfiguration: {
            QSet<QString> seats;
            for (const auto& value : document.value(QStringLiteral("seats")).toArray()) {
                const auto seat_id = value.toObject().value(QStringLiteral("seat_id")).toString();
                if (seats.contains(seat_id)) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("seats"),
                        QStringLiteral("duplicate seat id %1").arg(seat_id));
                }
                seats.insert(seat_id);
            }
            if (!seats.contains(document.value(QStringLiteral("presiding_seat_id")).toString())) {
                return crossReferenceFailure(resource, QStringLiteral("presiding_seat_id"),
                                             QStringLiteral("seat is not declared"));
            }
            break;
        }
        default:
            break;
        }
    }

    // Procedure profiles establish the contextual role vocabulary for catalogs
    // and the workflow used by each case.
    for (const auto& resource : resources) {
        if (resource.descriptor.kind != model::ResourceKind::ProcedureProfile) {
            continue;
        }
        const auto& document = resource.document;
        const auto court = requireKind(resource, QStringLiteral("court_id"),
                                       document.value(QStringLiteral("court_id")).toString(),
                                       model::ResourceKind::Court);
        if (!court) {
            return std::unexpected(court.error());
        }
        const auto catalog_id = document.value(QStringLiteral("filing_catalog_id")).toString();
        const auto catalog = requireKind(resource, QStringLiteral("filing_catalog_id"), catalog_id,
                                         model::ResourceKind::FilingCatalog);
        if (!catalog) {
            return std::unexpected(catalog.error());
        }
        const auto workflow = requireKind(resource, QStringLiteral("workflow_id"),
                                          document.value(QStringLiteral("workflow_id")).toString(),
                                          model::ResourceKind::Workflow);
        if (!workflow) {
            return std::unexpected(workflow.error());
        }
        for (const auto& value : document.value(QStringLiteral("authority_set_ids")).toArray()) {
            const auto authority_set =
                requireKind(resource, QStringLiteral("authority_set_ids"), value.toString(),
                            model::ResourceKind::AuthoritySet);
            if (!authority_set) {
                return std::unexpected(authority_set.error());
            }
        }
        const auto procedure_roles =
            stringSet(document.value(QStringLiteral("actor_roles")).toArray());
        catalog_roles[catalog_id].unite(procedure_roles);
        const auto& workflow_document = (*workflow)->document;
        if (stringSet(workflow_document.value(QStringLiteral("calendar"))
                          .toObject()
                          .value(QStringLiteral("holidays"))
                          .toArray()) !=
            stringSet((*court)->document.value(QStringLiteral("holidays")).toArray())) {
            return crossReferenceFailure(
                resource, QStringLiteral("workflow_id/calendar"),
                QStringLiteral("workflow calendar must match its court calendar"));
        }
        const auto rolesAreDeclared = [&procedure_roles](const QJsonArray& values) {
            return std::ranges::all_of(values, [&procedure_roles](const QJsonValue& value) {
                return procedure_roles.contains(value.toString());
            });
        };
        for (const auto& value : workflow_document.value(QStringLiteral("operations")).toArray()) {
            if (!rolesAreDeclared(
                    value.toObject().value(QStringLiteral("authorized_role_ids")).toArray())) {
                return crossReferenceFailure(
                    resource, QStringLiteral("workflow_id/operations/authorized_role_ids"),
                    QStringLiteral("workflow operation role is not declared by the procedure"));
            }
        }
        for (const auto& value :
             workflow_document.value(QStringLiteral("filing_routes")).toArray()) {
            const auto route = value.toObject();
            const auto filing_type_id = route.value(QStringLiteral("filing_type_id")).toString();
            if (!catalog_filings.value(catalog_id).contains(filing_type_id) ||
                !rolesAreDeclared(route.value(QStringLiteral("authorized_role_ids")).toArray()) ||
                !rolesAreDeclared(
                    route.value(QStringLiteral("required_service_role_ids")).toArray()) ||
                stringSet(route.value(QStringLiteral("authorized_role_ids")).toArray()) !=
                    filing_authorized_roles.value(filing_type_id) ||
                stringSet(route.value(QStringLiteral("required_field_ids")).toArray()) !=
                    filing_required_fields.value(filing_type_id)) {
                return crossReferenceFailure(
                    resource, QStringLiteral("workflow_id/filing_routes"),
                    QStringLiteral("workflow route conflicts with its procedure filing catalog"));
            }
        }
    }

    for (const auto& resource : resources) {
        const auto& document = resource.document;
        switch (resource.descriptor.kind) {
        case model::ResourceKind::Court:
            for (const auto& value :
                 document.value(QStringLiteral("authority_set_ids")).toArray()) {
                const auto authority_set =
                    requireKind(resource, QStringLiteral("authority_set_ids"), value.toString(),
                                model::ResourceKind::AuthoritySet);
                if (!authority_set) {
                    return std::unexpected(authority_set.error());
                }
            }
            break;
        case model::ResourceKind::FilingCatalog:
            for (const auto& value : document.value(QStringLiteral("filings")).toArray()) {
                const auto filing = value.toObject();
                const auto filing_id = filing.value(QStringLiteral("filing_id")).toString();
                const auto authority_id = filing.value(QStringLiteral("authority_id")).toString();
                if (!authority_ids.contains(authority_id)) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("filings/authority_id"),
                        QStringLiteral("unresolved authority %1").arg(authority_id));
                }
                const auto roles =
                    catalog_roles.value(QString::fromStdString(resource.descriptor.id));
                if (!roles.isEmpty()) {
                    for (const auto& role :
                         filing.value(QStringLiteral("actor_role_ids")).toArray()) {
                        if (!roles.contains(role.toString())) {
                            return crossReferenceFailure(
                                resource, QStringLiteral("filings/actor_role_ids"),
                                QStringLiteral("role %1 is not declared by the procedure")
                                    .arg(role.toString()));
                        }
                    }
                }
                const auto available_fields = form_fields_by_filing.value(filing_id);
                for (const auto& field :
                     filing.value(QStringLiteral("required_field_ids")).toArray()) {
                    if (!available_fields.contains(field.toString())) {
                        return crossReferenceFailure(
                            resource, QStringLiteral("filings/required_field_ids"),
                            QStringLiteral("field %1 has no form declaration")
                                .arg(field.toString()));
                    }
                }
            }
            break;
        case model::ResourceKind::Form: {
            const auto filing_id = document.value(QStringLiteral("filing_id")).toString();
            if (!filing_ids.contains(filing_id)) {
                return crossReferenceFailure(resource, QStringLiteral("filing_id"),
                                             QStringLiteral("unresolved filing %1").arg(filing_id));
            }
            break;
        }
        case model::ResourceKind::Workflow:
            for (const auto& value : document.value(QStringLiteral("operations")).toArray()) {
                const auto authority =
                    value.toObject().value(QStringLiteral("authority")).toObject();
                const auto authorityMatchesCanonical =
                    [&authorities_by_id](const QJsonObject& reference) {
                        const auto found = authorities_by_id.constFind(
                            reference.value(QStringLiteral("authority_id")).toString());
                        if (found == authorities_by_id.constEnd()) {
                            return false;
                        }
                        for (const auto& field :
                             {QStringLiteral("citation"), QStringLiteral("source_version"),
                              QStringLiteral("proposition")}) {
                            if (reference.value(field) != found->value(field)) {
                                return false;
                            }
                        }
                        return true;
                    };
                const auto primary = authority.value(QStringLiteral("primary")).toObject();
                if (!authorityMatchesCanonical(primary)) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("operations/authority/primary"),
                        QStringLiteral(
                            "authority is unresolved or conflicts with its authority set"));
                }
                QSet<QString> basis_ids{primary.value(QStringLiteral("authority_id")).toString()};
                for (const auto& supporting :
                     authority.value(QStringLiteral("supporting")).toArray()) {
                    const auto supporting_reference = supporting.toObject();
                    const auto supporting_id =
                        supporting_reference.value(QStringLiteral("authority_id")).toString();
                    if (basis_ids.contains(supporting_id) ||
                        !authorityMatchesCanonical(supporting_reference)) {
                        return crossReferenceFailure(
                            resource, QStringLiteral("operations/authority/supporting"),
                            QStringLiteral("supporting authorities must be unique canonical refs"));
                    }
                    basis_ids.insert(supporting_id);
                }
            }
            break;
        case model::ResourceKind::Case: {
            const auto procedure =
                requireKind(resource, QStringLiteral("procedure_profile_id"),
                            document.value(QStringLiteral("procedure_profile_id")).toString(),
                            model::ResourceKind::ProcedureProfile);
            if (!procedure) {
                return std::unexpected(procedure.error());
            }
            const auto record_id = document.value(QStringLiteral("record_id")).toString();
            const auto record = requireKind(resource, QStringLiteral("record_id"), record_id,
                                            model::ResourceKind::Record);
            if (!record) {
                return std::unexpected(record.error());
            }
            const auto roles =
                stringSet((*procedure)->document.value(QStringLiteral("actor_roles")).toArray());
            for (const auto& value : document.value(QStringLiteral("actors")).toArray()) {
                const auto role_id = value.toObject().value(QStringLiteral("role_id")).toString();
                if (!roles.contains(role_id)) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("actors/role_id"),
                        QStringLiteral("role %1 is not declared by the procedure").arg(role_id));
                }
            }
            for (const auto& value : document.value(QStringLiteral("issues")).toArray()) {
                const auto issue = value.toObject();
                for (const auto& authority :
                     issue.value(QStringLiteral("authority_ids")).toArray()) {
                    if (!authority_ids.contains(authority.toString())) {
                        return crossReferenceFailure(
                            resource, QStringLiteral("issues/authority_ids"),
                            QStringLiteral("unresolved authority %1").arg(authority.toString()));
                    }
                }
                for (const auto& anchor :
                     issue.value(QStringLiteral("record_anchor_ids")).toArray()) {
                    if (!record_entries.value(record_id).contains(anchor.toString())) {
                        return crossReferenceFailure(
                            resource, QStringLiteral("issues/record_anchor_ids"),
                            QStringLiteral("anchor %1 is not in the case record")
                                .arg(anchor.toString()));
                    }
                }
            }
            const auto workflow_id =
                (*procedure)->document.value(QStringLiteral("workflow_id")).toString();
            const auto disposition =
                document.value(QStringLiteral("authored_disposition_id")).toString();
            const auto case_workflow =
                requireKind(resource, QStringLiteral("authored_disposition_id"), workflow_id,
                            model::ResourceKind::Workflow);
            if (!case_workflow) {
                return std::unexpected(case_workflow.error());
            }
            const auto operation_values =
                (*case_workflow)->document.value(QStringLiteral("operations")).toArray();
            const auto authored_operation =
                std::ranges::find_if(operation_values, [&disposition](const QJsonValue& value) {
                    const auto operation = value.toObject();
                    return operation.value(QStringLiteral("operation_id")).toString() ==
                               disposition &&
                           operation.value(QStringLiteral("opcode")).toString() ==
                               QStringLiteral("issue_judgment");
                });
            if (!workflow_operations.value(workflow_id).contains(disposition) ||
                authored_operation == operation_values.end()) {
                return crossReferenceFailure(
                    resource, QStringLiteral("authored_disposition_id"),
                    QStringLiteral("operation is not a judgment in the case workflow"));
            }
            break;
        }
        case model::ResourceKind::ArgumentConfig: {
            const auto case_id = document.value(QStringLiteral("case_id")).toString();
            const auto case_resource = requireKind(resource, QStringLiteral("case_id"), case_id,
                                                   model::ResourceKind::Case);
            if (!case_resource) {
                return std::unexpected(case_resource.error());
            }
            const auto bench =
                requireKind(resource, QStringLiteral("bench_configuration_id"),
                            document.value(QStringLiteral("bench_configuration_id")).toString(),
                            model::ResourceKind::BenchConfiguration);
            if (!bench) {
                return std::unexpected(bench.error());
            }
            for (const auto& issue :
                 document.value(QStringLiteral("permitted_issue_ids")).toArray()) {
                if (!case_issues.value(case_id).contains(issue.toString())) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("permitted_issue_ids"),
                        QStringLiteral("issue %1 is not in the configured case")
                            .arg(issue.toString()));
                }
            }
            if (document.value(QStringLiteral("rebuttal_seconds")).toInt() >
                document.value(QStringLiteral("total_seconds")).toInt()) {
                return crossReferenceFailure(resource, QStringLiteral("rebuttal_seconds"),
                                             QStringLiteral("cannot exceed total_seconds"));
            }
            break;
        }
        case model::ResourceKind::BenchConfiguration: {
            const auto court = requireKind(resource, QStringLiteral("court_id"),
                                           document.value(QStringLiteral("court_id")).toString(),
                                           model::ResourceKind::Court);
            if (!court) {
                return std::unexpected(court.error());
            }
            const auto court_role =
                (*court)->document.value(QStringLiteral("court_role")).toString();
            const auto jurisdiction =
                (*court)->document.value(QStringLiteral("jurisdiction_id")).toString();
            for (const auto& value : document.value(QStringLiteral("seats")).toArray()) {
                const auto seat = value.toObject();
                if (seat.value(QStringLiteral("court_role")).toString() != court_role) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("seats/court_role"),
                        QStringLiteral("seat role does not match the court"));
                }
                const auto profile =
                    requireKind(resource, QStringLiteral("seats/profile_id"),
                                seat.value(QStringLiteral("profile_id")).toString(),
                                model::ResourceKind::JudgeProfile);
                if (!profile) {
                    return std::unexpected(profile.error());
                }
                const auto compatibility =
                    (*profile)->document.value(QStringLiteral("compatibility")).toObject();
                if (!stringSet(compatibility.value(QStringLiteral("court_roles")).toArray())
                         .contains(court_role) ||
                    !stringSet(compatibility.value(QStringLiteral("jurisdiction_ids")).toArray())
                         .contains(jurisdiction)) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("seats/profile_id"),
                        QStringLiteral("judge profile is incompatible with the court"));
                }
            }
            break;
        }
        case model::ResourceKind::RealismReview: {
            const auto case_resource = requireKind(
                resource, QStringLiteral("case_id"),
                document.value(QStringLiteral("case_id")).toString(), model::ResourceKind::Case);
            if (!case_resource) {
                return std::unexpected(case_resource.error());
            }
            if (document.value(QStringLiteral("review_state")).toString() ==
                    QStringLiteral("independently_reviewed") &&
                (!document.contains(QStringLiteral("reviewed_on")) ||
                 !document.contains(QStringLiteral("reviewer_reference")))) {
                return crossReferenceFailure(
                    resource, QStringLiteral("review_state"),
                    QStringLiteral("independent review requires date and reviewer reference"));
            }
            break;
        }
        default:
            break;
        }
    }
    for (const auto& blob : blobs) {
        const auto path = QString::fromStdString(blob.path);
        if (!referenced_blob_paths.contains(path)) {
            return fail(ErrorCode::CrossReferenceFailure,
                        QStringLiteral("Orphan blob is not referenced by a record: %1").arg(path));
        }
    }
    return {};
}

} // namespace

std::expected<LoadedPack, Error> PackReader::readDirectory(const QString& directory) {
    const QFileInfo root_info(directory);
    if (!root_info.isDir() || root_info.isSymLink()) {
        return fail(ErrorCode::UnsafePath,
                    QStringLiteral("Pack root must be a real directory: %1").arg(directory));
    }
    const QDir root(root_info.absoluteFilePath());
    const auto schema_validator = SchemaValidator::fromBundledSchemas();
    if (!schema_validator) {
        return std::unexpected(schema_validator.error());
    }
    const auto manifest_path = root.filePath(QStringLiteral("manifest.json"));
    const auto manifest_bytes = readFile(manifest_path, maximum_manifest_bytes);
    if (!manifest_bytes) {
        return std::unexpected(manifest_bytes.error());
    }
    const auto parsed = SchemaValidator::parseObject(*manifest_bytes, manifest_path);
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
                                 "dependencies", "contents", "blobs"}) ||
        !manifest.value(QStringLiteral("required_capabilities")).isArray() ||
        !manifest.value(QStringLiteral("dependencies")).isArray() ||
        !manifest.value(QStringLiteral("contents")).isArray() ||
        !manifest.value(QStringLiteral("blobs")).isArray()) {
        return fail(ErrorCode::InvalidManifest,
                    QStringLiteral("Manifest contains unknown, missing, or invalid fields"));
    }

    const auto pack_id = manifest.value(QStringLiteral("pack_id")).toString();
    const auto version = manifest.value(QStringLiteral("version")).toString();
    const auto capability_values =
        manifest.value(QStringLiteral("required_capabilities")).toArray();
    const auto dependency_values = manifest.value(QStringLiteral("dependencies")).toArray();
    const auto content_values = manifest.value(QStringLiteral("contents")).toArray();
    const auto blob_values = manifest.value(QStringLiteral("blobs")).toArray();
    if (!isNamespacedId(pack_id) || !isSemanticVersion(version) ||
        capability_values.size() > maximum_capabilities ||
        dependency_values.size() > maximum_dependencies || content_values.isEmpty() ||
        content_values.size() > maximum_contents ||
        blob_values.size() > maximum_contents - content_values.size()) {
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
    std::vector<QString> declared_payload_paths;
    declared_payload_paths.reserve(
        static_cast<std::size_t>(content_values.size() + blob_values.size()));
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
            !isNamespacedId(id) || !object.value(QStringLiteral("kind")).isString() ||
            !content_schema.isDouble() || !isSafeRelativePath(path) ||
            path == QStringLiteral("manifest.json") || !isSha256(digest)) {
            const auto code =
                isSafeRelativePath(path) ? ErrorCode::InvalidManifest : ErrorCode::UnsafePath;
            return fail(code, QStringLiteral("Invalid content entry %1").arg(id));
        }
        if (!kindDefinition(kind)) {
            return fail(ErrorCode::UnsupportedResourceKind,
                        QStringLiteral("Unsupported resource kind %1").arg(kind));
        }
        if (!isExactInteger(content_schema, supported_schema_version, supported_schema_version)) {
            return fail(ErrorCode::UnsupportedSchema,
                        QStringLiteral("Unsupported resource schema version for %1").arg(id));
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
        declared_payload_paths.push_back(path);
        contents.push_back(ContentDescriptor{id, kind, supported_schema_version, path, digest});
    }

    std::vector<model::BlobDescriptor> blobs;
    blobs.reserve(static_cast<std::size_t>(blob_values.size()));
    quint64 total_blob_bytes = 0;
    for (const auto& value : blob_values) {
        if (!value.isObject()) {
            return fail(ErrorCode::InvalidManifest,
                        QStringLiteral("Every blob descriptor must be an object"));
        }
        const auto object = value.toObject();
        const auto path = object.value(QStringLiteral("path")).toString();
        const auto media_type = object.value(QStringLiteral("media_type")).toString();
        const auto byte_size_value = object.value(QStringLiteral("byte_size"));
        const auto digest = object.value(QStringLiteral("sha256")).toString();
        if (!hasExactKeys(object, {"path", "media_type", "byte_size", "sha256"}) ||
            !isSafeRelativePath(path) || path == QStringLiteral("manifest.json") ||
            !object.value(QStringLiteral("media_type")).isString() ||
            media_type != QStringLiteral("application/pdf") ||
            !isExactInteger(byte_size_value, 1, static_cast<qint64>(maximum_blob_bytes)) ||
            !isSha256(digest)) {
            const auto code =
                isSafeRelativePath(path) ? ErrorCode::InvalidManifest : ErrorCode::UnsafePath;
            return fail(code, QStringLiteral("Invalid blob descriptor for %1").arg(path));
        }
        if (declared_files.contains(path)) {
            return fail(ErrorCode::DuplicateContentPath,
                        QStringLiteral("Duplicate blob path %1").arg(path));
        }
        const auto byte_size = static_cast<quint64>(byte_size_value.toDouble());
        if (byte_size > maximum_total_blob_bytes ||
            total_blob_bytes > maximum_total_blob_bytes - byte_size) {
            return fail(ErrorCode::ResourceTooLarge,
                        QStringLiteral("Declared blobs exceed the total size limit"));
        }
        total_blob_bytes += byte_size;
        declared_files.insert(path);
        declared_payload_paths.push_back(path);
        blobs.push_back(model::BlobDescriptor{path.toStdString(), media_type.toStdString(),
                                              byte_size, digest.toStdString()});
    }
    if (const auto overlap = overlappingPath(declared_payload_paths); overlap) {
        return fail(ErrorCode::DuplicateContentPath,
                    QStringLiteral("Overlapping payload path %1").arg(*overlap));
    }

    const auto manifest_schema =
        schema_validator->validate(QStringLiteral("manifest.schema.json"), manifest);
    if (!manifest_schema) {
        auto error = manifest_schema.error();
        if (error.code == ErrorCode::SchemaViolation) {
            error.code = ErrorCode::InvalidManifest;
        }
        return std::unexpected(std::move(error));
    }

    const auto file_set_result = validateDeclaredFileSet(root, declared_files);
    if (!file_set_result) {
        return std::unexpected(file_set_result.error());
    }

    for (const auto& blob : blobs) {
        const auto relative_path = QString::fromStdString(blob.path);
        const auto absolute_path = validateRegularPath(root, relative_path);
        if (!absolute_path) {
            return std::unexpected(absolute_path.error());
        }
        const auto validated = validateBlobFile(*absolute_path, blob);
        if (!validated) {
            return std::unexpected(validated.error());
        }
    }
    std::ranges::sort(blobs, {}, &model::BlobDescriptor::path);

    std::vector<model::JudgeProfile> judges;
    std::vector<ValidatedResource> resources;
    resources.reserve(contents.size());
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
        const auto definition = kindDefinition(content.kind);
        if (!definition) {
            return fail(ErrorCode::UnsupportedResourceKind,
                        QStringLiteral("Unsupported resource kind %1").arg(content.kind));
        }
        const auto object = SchemaValidator::parseObject(*bytes, content.path);
        if (!object) {
            return std::unexpected(object.error());
        }
        const auto schema_result = schema_validator->validate(definition->schema_file, *object);
        if (!schema_result) {
            auto error = schema_result.error();
            if (definition->kind == model::ResourceKind::JudgeProfile &&
                error.code == ErrorCode::SchemaViolation) {
                error.code = ErrorCode::InvalidJudgeProfile;
            }
            return std::unexpected(std::move(error));
        }
        const auto payload_id = object->value(QStringLiteral("resource_id")).toString();
        if (payload_ids.contains(payload_id)) {
            return fail(ErrorCode::DuplicatePayloadId,
                        QStringLiteral("Duplicate payload id %1").arg(payload_id));
        }
        payload_ids.insert(payload_id);
        if (payload_id != content.id ||
            object->value(QStringLiteral("resource_kind")).toString() != content.kind ||
            !isExactInteger(object->value(QStringLiteral("schema_version")), content.schema_version,
                            content.schema_version)) {
            return fail(ErrorCode::SchemaViolation,
                        QStringLiteral("Descriptor and payload identity disagree for %1")
                            .arg(content.path));
        }
        resources.push_back(ValidatedResource{
            model::DeclarativeResource{definition->kind, content.id.toStdString(),
                                       static_cast<std::uint32_t>(content.schema_version),
                                       content.path.toStdString(), content.digest.toStdString()},
            *object,
        });
        if (definition->kind == model::ResourceKind::JudgeProfile) {
            const auto judge = parseJudge(*object, content.path);
            if (!judge) {
                return std::unexpected(judge.error());
            }
            judges.push_back(*judge);
        }
    }
    std::ranges::sort(judges, {}, &model::JudgeProfile::id);
    std::ranges::sort(resources, [](const auto& left, const auto& right) {
        return left.descriptor.id < right.descriptor.id;
    });
    const auto graph_result = validateResourceGraph(resources, blobs);
    if (!graph_result) {
        return std::unexpected(graph_result.error());
    }

    return LoadedPack{
        model::PackRevision{
            model::PackId{pack_id.toStdString()}, version.toStdString(),
            canonicalDigest(pack_id, version, capabilities, dependencies, contents, blobs)},
        std::move(capabilities),
        std::move(dependencies),
        std::move(resources),
        std::move(blobs),
        std::move(judges),
    };
}

} // namespace appellate::packs
