#include "appellate/packs/pack_reader.hpp"
#include "appellate/model/authority_ref.hpp"
#include "appellate/model/oral_argument.hpp"
#include "appellate/packs/capability_registry.hpp"
#include "appellate/packs/pack_version.hpp"
#include "appellate/packs/schema_validator.hpp"
#include "realism_evidence.hpp"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDate>
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
#include <tuple>
#include <utility>
#include <vector>

namespace appellate::packs {
namespace {

constexpr auto minimum_supported_schema_version = 1;
constexpr auto maximum_supported_schema_version = 2;
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
constexpr qsizetype maximum_voice_phrases = 8;
constexpr qsizetype maximum_workflow_preconditions = 32;
constexpr qsizetype maximum_disposition_targets = 4096;
constexpr qsizetype maximum_disposition_plans = 64;
constexpr qsizetype maximum_disposition_components = 32;
constexpr qsizetype maximum_component_authorities = 32;
constexpr qsizetype maximum_component_record_anchors = 32;
constexpr qsizetype maximum_argument_issue_bindings = 64;
constexpr qsizetype maximum_argument_topics_per_issue = 8;
constexpr qsizetype maximum_authored_questions = 128;
constexpr qsizetype maximum_authored_questions_per_issue = 16;
constexpr qsizetype maximum_question_grounding = 16;
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

struct KindRegistration final {
    const char* name;
    model::ResourceKind kind;
    const char* schema_file;
};

constexpr std::array v1_kind_registry{
    KindRegistration{"argument_config", model::ResourceKind::ArgumentConfig,
                     "argument-config.schema.json"},
    KindRegistration{"authority_set", model::ResourceKind::AuthoritySet,
                     "authority-set.schema.json"},
    KindRegistration{"bench_configuration", model::ResourceKind::BenchConfiguration,
                     "bench-configuration.schema.json"},
    KindRegistration{"case", model::ResourceKind::Case, "case.schema.json"},
    KindRegistration{"court", model::ResourceKind::Court, "court.schema.json"},
    KindRegistration{"filing_catalog", model::ResourceKind::FilingCatalog,
                     "filing-catalog.schema.json"},
    KindRegistration{"form", model::ResourceKind::Form, "form.schema.json"},
    KindRegistration{"judge_profile", model::ResourceKind::JudgeProfile,
                     "judge-profile.schema.json"},
    KindRegistration{"procedure_profile", model::ResourceKind::ProcedureProfile,
                     "procedure-profile.schema.json"},
    KindRegistration{"realism_review", model::ResourceKind::RealismReview,
                     "realism-review.schema.json"},
    KindRegistration{"record", model::ResourceKind::Record, "record.schema.json"},
    KindRegistration{"workflow", model::ResourceKind::Workflow, "workflow.schema.json"},
};

// Deliberately independent from v1. New resource generations are registered
// here without widening the v1 trust boundary.
constexpr std::array v2_kind_registry{
    KindRegistration{"argument_config", model::ResourceKind::ArgumentConfig,
                     "argument-config.schema.json"},
    KindRegistration{"authority_set", model::ResourceKind::AuthoritySet,
                     "authority-set.schema.json"},
    KindRegistration{"bench_configuration", model::ResourceKind::BenchConfiguration,
                     "bench-configuration.schema.json"},
    KindRegistration{"case", model::ResourceKind::Case, "case.schema.json"},
    KindRegistration{"court", model::ResourceKind::Court, "court.schema.json"},
    KindRegistration{"filing_catalog", model::ResourceKind::FilingCatalog,
                     "filing-catalog.schema.json"},
    KindRegistration{"form", model::ResourceKind::Form, "form.schema.json"},
    KindRegistration{"judge_profile", model::ResourceKind::JudgeProfile,
                     "judge-profile.schema.json"},
    KindRegistration{"procedure_profile", model::ResourceKind::ProcedureProfile,
                     "procedure-profile.schema.json"},
    KindRegistration{"realism_review", model::ResourceKind::RealismReview,
                     "realism-review.schema.json"},
    KindRegistration{"record", model::ResourceKind::Record, "record.schema.json"},
    KindRegistration{"workflow", model::ResourceKind::Workflow, "workflow.schema.json"},
};

[[nodiscard]] std::span<const KindRegistration> kindRegistry(int schema_version) {
    if (schema_version == 1) {
        return v1_kind_registry;
    }
    if (schema_version == 2) {
        return v2_kind_registry;
    }
    return {};
}

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
    return value.size() >= 3 && value.size() <= 160 && pattern.match(value).hasMatch();
}

[[nodiscard]] bool deadlineNamespaceContains(const QString& prefix, const QString& id) {
    return id == prefix || id.startsWith(prefix + u'.');
}

[[nodiscard]] bool deadlineNamespacesOverlap(const QString& left, const QString& right) {
    return deadlineNamespaceContains(left, right) || deadlineNamespaceContains(right, left);
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

[[nodiscard]] auto parseQuestionFraming(const QString& value)
    -> std::optional<model::QuestionFraming> {
    if (value == QStringLiteral("direct")) {
        return model::QuestionFraming::Direct;
    }
    if (value == QStringLiteral("socratic")) {
        return model::QuestionFraming::Socratic;
    }
    if (value == QStringLiteral("narrative")) {
        return model::QuestionFraming::Narrative;
    }
    return std::nullopt;
}

[[nodiscard]] auto parseAddressConvention(const QString& value)
    -> std::optional<model::CounselAddress> {
    if (value == QStringLiteral("counsel")) {
        return model::CounselAddress::Counsel;
    }
    if (value == QStringLiteral("advocate")) {
        return model::CounselAddress::Advocate;
    }
    return std::nullopt;
}

[[nodiscard]] bool isVoicePhrase(const QString& phrase) {
    return !phrase.isEmpty() && phrase.size() <= 128 && phrase.toUtf8().size() <= 128 &&
           phrase.trimmed() == phrase && std::ranges::all_of(phrase, [](QChar character) {
               return (character.isPrint() || character == u' ') && character != u'{' &&
                      character != u'}';
           });
}

[[nodiscard]] auto parseVoicePhrases(const QJsonValue& value)
    -> std::optional<std::vector<std::string>> {
    if (!value.isArray()) {
        return std::nullopt;
    }
    const auto phrases = value.toArray();
    if (phrases.isEmpty() || phrases.size() > maximum_voice_phrases) {
        return std::nullopt;
    }
    QSet<QString> seen;
    std::vector<std::string> result;
    result.reserve(static_cast<std::size_t>(phrases.size()));
    for (const auto& value_item : phrases) {
        if (!value_item.isString()) {
            return std::nullopt;
        }
        const auto phrase = value_item.toString();
        if (!isVoicePhrase(phrase) || seen.contains(phrase)) {
            return std::nullopt;
        }
        seen.insert(phrase);
        result.push_back(phrase.toUtf8().toStdString());
    }
    return result;
}

[[nodiscard]] auto kindDefinition(const QString& kind, int schema_version)
    -> std::optional<KindDefinition> {
    const auto registry = kindRegistry(schema_version);
    const auto found = std::ranges::find_if(registry, [&kind](const auto& registration) {
        return kind == QLatin1StringView(registration.name);
    });
    if (found != registry.end()) {
        return KindDefinition{found->kind, QString::fromLatin1(found->schema_file)};
    }
    return std::nullopt;
}

[[nodiscard]] auto parseJudge(const QJsonObject& object, const QString& name, int schema_version)
    -> std::expected<model::JudgeProfile, Error> {
    if (!hasExactKeys(object, {"schema_version", "resource_kind", "resource_id", "display_name",
                               "profile_class", "compatibility", "interaction", "voice"}) ||
        !isExactInteger(object.value(QStringLiteral("schema_version")), schema_version,
                        schema_version) ||
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
        "concession_recall", "record_pin_demand",
        "time_strictness",
    };
    if (!hasExactKeys(interaction,
                      {"directness", "formality", "question_length", "interruption_frequency",
                       "follow_up_depth", "hypothetical_frequency", "concession_recall",
                       "record_pin_demand", "time_strictness", "issue_focus"}) ||
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
    const auto framing =
        parseQuestionFraming(voice.value(QStringLiteral("question_framing")).toString());
    const auto address =
        parseAddressConvention(voice.value(QStringLiteral("address_convention")).toString());
    auto question_phrases = parseVoicePhrases(voice.value(QStringLiteral("question_phrases")));
    auto interruption_phrases =
        parseVoicePhrases(voice.value(QStringLiteral("interruption_phrases")));
    auto clarification_phrases =
        parseVoicePhrases(voice.value(QStringLiteral("clarification_phrases")));
    if (!hasExactKeys(voice, {"register", "cadence", "question_framing", "address_convention",
                              "verbosity", "sentence_complexity", "question_phrases",
                              "interruption_phrases", "clarification_phrases"}) ||
        !register_style || !cadence || !framing || !address || !question_phrases ||
        !interruption_phrases || !clarification_phrases ||
        !isUnitInterval(voice.value(QStringLiteral("verbosity"))) ||
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
            interaction.value(QStringLiteral("record_pin_demand")).toDouble(),
            interaction.value(QStringLiteral("time_strictness")).toDouble(),
            std::move(focus),
        },
        model::VoiceStyle{
            *register_style,
            *cadence,
            *framing,
            *address,
            voice.value(QStringLiteral("verbosity")).toDouble(),
            voice.value(QStringLiteral("sentence_complexity")).toDouble(),
            std::move(*question_phrases),
            std::move(*interruption_phrases),
            std::move(*clarification_phrases),
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

[[nodiscard]] auto canonicalDigest(int manifest_schema_version, const QString& pack_id,
                                   const QString& version,
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
    if (manifest_schema_version == 1) {
        addFrame(hash, QByteArrayView("appellate-workbench-pack-revision-v1"));
    } else {
        addFrame(hash, QByteArrayView("appellate-workbench-pack-revision-v2"));
    }
    addUint64(hash, static_cast<quint64>(manifest_schema_version));
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

[[nodiscard]] QString canonicalDispositionPlanDigest(const QString& case_id,
                                                     const QString& authored_operation_id,
                                                     const QJsonObject& plan) {
    std::vector<QJsonObject> components;
    const auto component_values = plan.value(QStringLiteral("components")).toArray();
    components.reserve(static_cast<std::size_t>(component_values.size()));
    for (const auto& value : component_values) {
        components.push_back(value.toObject());
    }
    std::ranges::sort(components, [](const QJsonObject& left, const QJsonObject& right) {
        return std::tuple{left.value(QStringLiteral("issue_id")).toString(),
                          left.value(QStringLiteral("target_id")).toString()} <
               std::tuple{right.value(QStringLiteral("issue_id")).toString(),
                          right.value(QStringLiteral("target_id")).toString()};
    });

    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, QByteArrayView("appellate-workbench-disposition-plan-v1"));
    addFrame(hash, case_id);
    addFrame(hash, authored_operation_id);
    addFrame(hash, plan.value(QStringLiteral("plan_id")).toString());
    addFrame(hash, plan.value(QStringLiteral("finality")).toString());
    addUint64(hash, static_cast<quint64>(components.size()));
    for (const auto& component : components) {
        addFrame(hash, component.value(QStringLiteral("issue_id")).toString());
        addFrame(hash, component.value(QStringLiteral("target_id")).toString());
        addFrame(hash, component.value(QStringLiteral("scope")).toString());
        addFrame(hash, component.value(QStringLiteral("action")).toString());
        addUint64(hash, component.value(QStringLiteral("remand")).toBool() ? 1U : 0U);
        for (const auto& field :
             {QStringLiteral("authority_ids"), QStringLiteral("record_anchor_ids")}) {
            std::vector<QString> identifiers;
            const auto identifier_values = component.value(field).toArray();
            identifiers.reserve(static_cast<std::size_t>(identifier_values.size()));
            for (const auto& identifier : identifier_values) {
                identifiers.push_back(identifier.toString());
            }
            std::ranges::sort(identifiers);
            addUint64(hash, static_cast<quint64>(identifiers.size()));
            for (const auto& identifier : identifiers) {
                addFrame(hash, identifier);
            }
        }
    }
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] QString
canonicalQuestionBankDigest(const QString& case_id, const QString& argument_configuration_id,
                            const QJsonObject& bank,
                            const QHash<QString, QJsonObject>& authorities_by_id,
                            const QHash<QString, QJsonObject>& record_entries_by_id,
                            const QHash<QString, QJsonObject>& record_anchors_by_id) {
    std::vector<QJsonObject> bindings;
    for (const auto& value : bank.value(QStringLiteral("issue_topic_bindings")).toArray()) {
        bindings.push_back(value.toObject());
    }
    std::ranges::sort(bindings, [](const QJsonObject& left, const QJsonObject& right) {
        return left.value(QStringLiteral("issue_id")).toString() <
               right.value(QStringLiteral("issue_id")).toString();
    });

    std::vector<QJsonObject> questions;
    for (const auto& value : bank.value(QStringLiteral("questions")).toArray()) {
        questions.push_back(value.toObject());
    }
    std::ranges::sort(questions, [](const QJsonObject& left, const QJsonObject& right) {
        return left.value(QStringLiteral("question_id")).toString() <
               right.value(QStringLiteral("question_id")).toString();
    });

    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, QByteArrayView("appellate-workbench-grounded-question-bank-v1"));
    addFrame(hash, case_id);
    addFrame(hash, argument_configuration_id);
    addFrame(hash, bank.value(QStringLiteral("mode")).toString());
    addUint64(hash, static_cast<quint64>(bindings.size()));
    for (const auto& binding : bindings) {
        addFrame(hash, binding.value(QStringLiteral("issue_id")).toString());
        std::vector<QString> topics;
        for (const auto& topic : binding.value(QStringLiteral("topic_ids")).toArray()) {
            topics.push_back(topic.toString());
        }
        std::ranges::sort(topics);
        addUint64(hash, static_cast<quint64>(topics.size()));
        for (const auto& topic : topics) {
            addFrame(hash, topic);
        }
    }

    addUint64(hash, static_cast<quint64>(questions.size()));
    for (const auto& question : questions) {
        addFrame(hash, question.value(QStringLiteral("question_id")).toString());
        addFrame(hash, question.value(QStringLiteral("issue_id")).toString());
        addFrame(hash, question.value(QStringLiteral("topic_id")).toString());
        addFrame(hash, question.value(QStringLiteral("prompt")).toString());
        std::vector<QJsonObject> grounding;
        for (const auto& value : question.value(QStringLiteral("grounding")).toArray()) {
            grounding.push_back(value.toObject());
        }
        std::ranges::sort(grounding, [](const QJsonObject& left, const QJsonObject& right) {
            return left.value(QStringLiteral("grounding_id")).toString() <
                   right.value(QStringLiteral("grounding_id")).toString();
        });
        addUint64(hash, static_cast<quint64>(grounding.size()));
        for (const auto& reference : grounding) {
            const auto kind = reference.value(QStringLiteral("kind")).toString();
            addFrame(hash, reference.value(QStringLiteral("grounding_id")).toString());
            addFrame(hash, kind);
            if (kind == QStringLiteral("authority")) {
                const auto authority = authorities_by_id.value(
                    reference.value(QStringLiteral("authority_id")).toString());
                for (const auto& field :
                     {QStringLiteral("authority_id"), QStringLiteral("citation"),
                      QStringLiteral("source_version"), QStringLiteral("proposition")}) {
                    addFrame(hash, authority.value(field).toString());
                }
                addUint64(hash, 1U);
                for (const auto& field :
                     {QStringLiteral("authority_type"), QStringLiteral("jurisdiction_id"),
                      QStringLiteral("issuing_body_id"), QStringLiteral("precedential_status")}) {
                    addFrame(hash, authority.value(field).toString());
                }
                addUint64(hash,
                          authority.value(QStringLiteral("official_source")).toBool() ? 1U : 0U);
                for (const auto& field : {QStringLiteral("checked_on"), QStringLiteral("locator"),
                                          QStringLiteral("source_url")}) {
                    addFrame(hash, authority.value(field).toString());
                }
            } else if (kind == QStringLiteral("brief_page")) {
                const auto entry_id = reference.value(QStringLiteral("entry_id")).toString();
                const auto entry = record_entries_by_id.value(entry_id);
                addFrame(hash, entry_id);
                addUint64(hash, static_cast<quint64>(
                                    reference.value(QStringLiteral("page_number")).toInt()));
                addFrame(hash, entry.value(QStringLiteral("asset_sha256")).toString());
            } else {
                const auto anchor_id = reference.value(QStringLiteral("anchor_id")).toString();
                const auto anchor = record_anchors_by_id.value(anchor_id);
                const auto entry_id = anchor.value(QStringLiteral("entry_id")).toString();
                const auto entry = record_entries_by_id.value(entry_id);
                addFrame(hash, anchor_id);
                addFrame(hash, entry_id);
                addUint64(hash, static_cast<quint64>(
                                    anchor.value(QStringLiteral("page_number")).toInt()));
                addFrame(hash, entry.value(QStringLiteral("asset_sha256")).toString());
                const auto has_citation = anchor.contains(QStringLiteral("citation_label"));
                addUint64(hash, has_citation ? 1U : 0U);
                if (has_citation) {
                    addFrame(hash, anchor.value(QStringLiteral("citation_label")).toString());
                }
            }
        }
    }
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] bool isCanonicalQuestionPrompt(const QString& prompt) {
    const auto utf8 = prompt.toUtf8().toStdString();
    if (!model::isCanonicalAuthorityText(utf8, 512) || utf8.front() == ' ' || utf8.back() == ' ') {
        return false;
    }
    return std::ranges::any_of(prompt, [](QChar scalar) { return !scalar.isSpace(); });
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

[[nodiscard]] bool usesWorkflowPreconditions(std::span<const ValidatedResource> resources) {
    return std::ranges::any_of(resources, [](const ValidatedResource& resource) {
        if (resource.descriptor.kind != model::ResourceKind::Workflow) {
            return false;
        }
        return std::ranges::any_of(resource.document.value(QStringLiteral("operations")).toArray(),
                                   [](const QJsonValue& operation) {
                                       return !operation.toObject()
                                                   .value(QStringLiteral("preconditions"))
                                                   .toArray()
                                                   .isEmpty();
                                   });
    });
}

[[nodiscard]] bool usesDependentDeadlines(std::span<const ValidatedResource> resources) {
    return std::ranges::any_of(resources, [](const ValidatedResource& resource) {
        if (resource.descriptor.kind != model::ResourceKind::Workflow) {
            return false;
        }
        return std::ranges::any_of(
            resource.document.value(QStringLiteral("operations")).toArray(),
            [](const QJsonValue& operation_value) {
                const auto operation = operation_value.toObject();
                if (operation.contains(QStringLiteral("deadline_base_id"))) {
                    return true;
                }
                return std::ranges::any_of(
                    operation.value(QStringLiteral("preconditions")).toArray(),
                    [](const QJsonValue& precondition_value) {
                        const auto precondition = precondition_value.toObject();
                        return precondition.value(QStringLiteral("kind")).toString() ==
                                   QStringLiteral("deadline_status") &&
                               precondition.value(QStringLiteral("status")).toString() ==
                                   QStringLiteral("reached");
                    });
            });
    });
}

[[nodiscard]] bool usesNamedDeadlines(std::span<const ValidatedResource> resources) {
    return std::ranges::any_of(resources, [](const ValidatedResource& resource) {
        if (resource.descriptor.kind != model::ResourceKind::Workflow) {
            return false;
        }
        return std::ranges::any_of(resource.document.value(QStringLiteral("operations")).toArray(),
                                   [](const QJsonValue& operation_value) {
                                       return operation_value.toObject().contains(
                                           QStringLiteral("produced_deadline_id"));
                                   });
    });
}

[[nodiscard]] bool usesEventDateDeadlines(std::span<const ValidatedResource> resources) {
    return std::ranges::any_of(resources, [](const ValidatedResource& resource) {
        if (resource.descriptor.kind != model::ResourceKind::Workflow) {
            return false;
        }
        return std::ranges::any_of(resource.document.value(QStringLiteral("operations")).toArray(),
                                   [](const QJsonValue& operation_value) {
                                       return operation_value.toObject().contains(
                                           QStringLiteral("deadline_event_base"));
                                   });
    });
}

[[nodiscard]] bool usesArgumentDateGuards(std::span<const ValidatedResource> resources) {
    return std::ranges::any_of(resources, [](const ValidatedResource& resource) {
        if (resource.descriptor.kind != model::ResourceKind::Workflow) {
            return false;
        }
        return std::ranges::any_of(
            resource.document.value(QStringLiteral("operations")).toArray(),
            [](const QJsonValue& operation_value) {
                return std::ranges::any_of(
                    operation_value.toObject().value(QStringLiteral("preconditions")).toArray(),
                    [](const QJsonValue& precondition_value) {
                        return precondition_value.toObject()
                                   .value(QStringLiteral("kind"))
                                   .toString() == QStringLiteral("argument_date_status");
                    });
            });
    });
}

[[nodiscard]] bool usesStructuredDisposition(std::span<const ValidatedResource> resources) {
    return std::ranges::any_of(resources, [](const ValidatedResource& resource) {
        if (resource.descriptor.kind != model::ResourceKind::Case) {
            return false;
        }
        if (resource.document.contains(QStringLiteral("disposition_plans")) ||
            resource.document.contains(QStringLiteral("authored_disposition_plan_id"))) {
            return true;
        }
        return std::ranges::any_of(resource.document.value(QStringLiteral("issues")).toArray(),
                                   [](const QJsonValue& issue) {
                                       return issue.toObject().contains(
                                           QStringLiteral("target_ids"));
                                   });
    });
}

[[nodiscard]] bool usesGroundedQuestions(std::span<const ValidatedResource> resources) {
    return std::ranges::any_of(resources, [](const ValidatedResource& resource) {
        return resource.descriptor.kind == model::ResourceKind::ArgumentConfig &&
               resource.document.contains(QStringLiteral("grounded_question_bank"));
    });
}

[[nodiscard]] bool usesRealismEvidence(std::span<const ValidatedResource> resources) {
    return std::ranges::any_of(resources, [](const ValidatedResource& resource) {
        return resource.descriptor.kind == model::ResourceKind::RealismReview &&
               (resource.document.contains(QStringLiteral("evidence")) ||
                resource.document.contains(QStringLiteral("reviewer")) ||
                std::ranges::any_of(
                    resource.document.value(QStringLiteral("known_uncertainty")).toArray(),
                    [](const QJsonValue& uncertainty) { return uncertainty.isObject(); }));
    });
}

[[nodiscard]] bool usesSealedRecordTwins(std::span<const ValidatedResource> resources) {
    return std::ranges::any_of(resources, [](const ValidatedResource& resource) {
        return resource.descriptor.kind == model::ResourceKind::Record &&
               (resource.document.contains(QStringLiteral("disclosure_policy")) ||
                resource.document.contains(QStringLiteral("sealed_disclosures")));
    });
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
    QHash<QString, QSet<QString>> catalog_authority_ids;
    QHash<QString, QSet<QString>> form_fields_by_filing;
    QHash<QString, QSet<QString>> workflow_stages;
    QHash<QString, QSet<QString>> workflow_operations;
    QHash<QString, QSet<QString>> workflow_authority_ids;
    QHash<QString, QSet<QString>> record_entries;
    QHash<QString, QHash<QString, QJsonObject>> record_docket_entries;
    QHash<QString, QHash<QString, QJsonObject>> record_page_anchors;
    QHash<QString, QSet<QString>> case_issues;
    QHash<QString, QString> case_record_ids;
    QHash<QString, QHash<QString, QSet<QString>>> case_issue_authorities;
    QHash<QString, QHash<QString, QSet<QString>>> case_issue_record_anchors;
    QHash<QString, QSet<QString>> catalog_roles;

    // Authority references may appear before their authority-set resource in
    // manifest order, so index identities before validating resource bodies.
    for (const auto& resource : resources) {
        if (resource.descriptor.kind != model::ResourceKind::AuthoritySet) {
            continue;
        }
        for (const auto& value : resource.document.value(QStringLiteral("authorities")).toArray()) {
            const auto authority = value.toObject();
            const auto authority_id = authority.value(QStringLiteral("authority_id")).toString();
            if (authority_ids.contains(authority_id)) {
                return crossReferenceFailure(
                    resource, QStringLiteral("authorities"),
                    QStringLiteral("duplicate authority id %1").arg(authority_id));
            }
            authority_ids.insert(authority_id);
            authorities_by_id.insert(authority_id, authority);
        }
    }

    for (const auto& resource : resources) {
        const auto id = QString::fromStdString(resource.descriptor.id);
        const auto& document = resource.document;
        switch (resource.descriptor.kind) {
        case model::ResourceKind::AuthoritySet: {
            const auto source_cutoff = QDate::fromString(
                document.value(QStringLiteral("source_cutoff")).toString(), Qt::ISODate);
            for (const auto& value : document.value(QStringLiteral("authorities")).toArray()) {
                const auto authority = value.toObject();
                const auto authority_id =
                    authority.value(QStringLiteral("authority_id")).toString();
                if (resource.descriptor.schema_version == 2) {
                    const auto source_version = QDate::fromString(
                        authority.value(QStringLiteral("source_version")).toString(), Qt::ISODate);
                    const auto checked_on = QDate::fromString(
                        authority.value(QStringLiteral("checked_on")).toString(), Qt::ISODate);
                    const auto source_url = authority.value(QStringLiteral("source_url"))
                                                .toString()
                                                .toUtf8()
                                                .toStdString();
                    const auto citation = authority.value(QStringLiteral("citation"))
                                              .toString()
                                              .toUtf8()
                                              .toStdString();
                    const auto proposition = authority.value(QStringLiteral("proposition"))
                                                 .toString()
                                                 .toUtf8()
                                                 .toStdString();
                    const auto locator = authority.value(QStringLiteral("locator"))
                                             .toString()
                                             .toUtf8()
                                             .toStdString();
                    if (!source_cutoff.isValid() || !source_version.isValid() ||
                        !checked_on.isValid() || source_version > source_cutoff ||
                        checked_on < source_version ||
                        !model::isCanonicalAuthorityText(citation, 4096) ||
                        !model::isCanonicalAuthorityText(proposition, 4096) ||
                        !model::isCanonicalAuthorityText(locator, 1024) ||
                        !model::isCanonicalAuthoritySourceUrl(source_url)) {
                        return crossReferenceFailure(
                            resource, QStringLiteral("authorities/provenance"),
                            QStringLiteral("authority source chronology or URL is noncanonical"));
                    }
                }
            }
            break;
        }
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
                catalog_authority_ids[id].insert(
                    filing.value(QStringLiteral("authority_id")).toString());
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
            QSet<QString> precondition_filing_ids;
            QSet<QString> named_deadline_ids;
            QSet<QString> deadline_base_ids;
            QSet<QString> precondition_deadline_ids;
            QVector<QPair<QString, QString>> order_event_bases;
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
                const auto authority = operation.value(QStringLiteral("authority")).toObject();
                if (resource.descriptor.schema_version == 2) {
                    workflow_authority_ids[id].insert(
                        authority.value(QStringLiteral("primary_authority_id")).toString());
                    workflow_authority_ids[id].unite(stringSet(
                        authority.value(QStringLiteral("supporting_authority_ids")).toArray()));
                } else {
                    workflow_authority_ids[id].insert(authority.value(QStringLiteral("primary"))
                                                          .toObject()
                                                          .value(QStringLiteral("authority_id"))
                                                          .toString());
                    for (const auto& supporting :
                         authority.value(QStringLiteral("supporting")).toArray()) {
                        workflow_authority_ids[id].insert(
                            supporting.toObject().value(QStringLiteral("authority_id")).toString());
                    }
                }
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
                const auto has_base = operation.contains(QStringLiteral("deadline_base_id"));
                const auto has_produced =
                    operation.contains(QStringLiteral("produced_deadline_id"));
                const auto has_event_base =
                    operation.contains(QStringLiteral("deadline_event_base"));
                const auto uses_extended_event_precondition = std::ranges::any_of(
                    operation.value(QStringLiteral("preconditions")).toArray(),
                    [](const QJsonValue& precondition_value) {
                        const auto precondition = precondition_value.toObject();
                        const auto kind = precondition.value(QStringLiteral("kind")).toString();
                        return kind == QStringLiteral("argument_date_status") ||
                               (kind == QStringLiteral("deadline_status") &&
                                precondition.value(QStringLiteral("status")).toString() ==
                                    QStringLiteral("reached"));
                    });
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
                if (has_base &&
                    (resource.descriptor.schema_version != 2 ||
                     opcode != QStringLiteral("calculate_deadline") || !has_produced ||
                     has_event_base ||
                     !isNamespacedId(
                         operation.value(QStringLiteral("deadline_base_id")).toString()))) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("operations/deadline_base_id"),
                        QStringLiteral("only a schema-2 calculate_deadline operation can depend "
                                       "on an existing deadline"));
                }
                const auto produced_deadline_id =
                    operation.value(QStringLiteral("produced_deadline_id")).toString();
                if (has_produced && (resource.descriptor.schema_version != 2 ||
                                     opcode != QStringLiteral("calculate_deadline") ||
                                     !isNamespacedId(produced_deadline_id) ||
                                     named_deadline_ids.contains(produced_deadline_id))) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("operations/produced_deadline_id"),
                        QStringLiteral("schema-2 named deadline outputs must be canonical, unique, "
                                       "and owned by calculate_deadline operations"));
                }
                if (has_produced) {
                    named_deadline_ids.insert(produced_deadline_id);
                }
                if (has_event_base) {
                    const auto event_base =
                        operation.value(QStringLiteral("deadline_event_base")).toObject();
                    const auto kind = event_base.value(QStringLiteral("kind")).toString();
                    if (resource.descriptor.schema_version != 2 ||
                        opcode != QStringLiteral("calculate_deadline") || !has_produced ||
                        has_base ||
                        (kind != QStringLiteral("judgment_occurred") &&
                         kind != QStringLiteral("order_occurred"))) {
                        return crossReferenceFailure(
                            resource, QStringLiteral("operations/deadline_event_base"),
                            QStringLiteral("event-date bases require a named schema-2 deadline "
                                           "calculation and cannot mix with a deadline-id base"));
                    }
                    if (kind == QStringLiteral("order_occurred")) {
                        order_event_bases.push_back(
                            {event_base.value(QStringLiteral("order_id")).toString(),
                             event_base.value(QStringLiteral("operation_id")).toString()});
                    }
                }
                if (opcode == QStringLiteral("calculate_deadline") &&
                    uses_extended_event_precondition && !has_produced) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("operations/preconditions"),
                        QStringLiteral("extended deadline-event preconditions require an exact "
                                       "named output"));
                }
                if (has_base) {
                    deadline_base_ids.insert(
                        operation.value(QStringLiteral("deadline_base_id")).toString());
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
                if (operation.contains(QStringLiteral("preconditions"))) {
                    const auto precondition_value =
                        operation.value(QStringLiteral("preconditions"));
                    if (resource.descriptor.schema_version != 2 || !precondition_value.isArray() ||
                        precondition_value.toArray().isEmpty() ||
                        precondition_value.toArray().size() > maximum_workflow_preconditions) {
                        return crossReferenceFailure(
                            resource, QStringLiteral("operations/preconditions"),
                            QStringLiteral("preconditions must be a bounded schema-2 array"));
                    }
                    QSet<QString> precondition_subjects;
                    for (const auto& precondition_value_item : precondition_value.toArray()) {
                        if (!precondition_value_item.isObject()) {
                            return crossReferenceFailure(
                                resource, QStringLiteral("operations/preconditions"),
                                QStringLiteral("preconditions must contain typed objects"));
                        }
                        const auto precondition = precondition_value_item.toObject();
                        const auto kind = precondition.value(QStringLiteral("kind")).toString();
                        QString subject_key;
                        QStringList contradictory_subject_keys;
                        if (kind == QStringLiteral("filing_presence") &&
                            hasExactKeys(precondition, {"kind", "filing_type_id", "present"}) &&
                            precondition.value(QStringLiteral("present")).isBool()) {
                            const auto filing_id =
                                precondition.value(QStringLiteral("filing_type_id")).toString();
                            if (!isNamespacedId(filing_id)) {
                                return crossReferenceFailure(
                                    resource, QStringLiteral("operations/preconditions"),
                                    QStringLiteral("filing precondition id is not canonical"));
                            }
                            subject_key = kind + u':' + filing_id;
                            precondition_filing_ids.insert(filing_id);
                        } else if (kind == QStringLiteral("order_disposition") &&
                                   hasExactKeys(precondition,
                                                {"kind", "order_id", "disposition"}) &&
                                   QSet<QString>{QStringLiteral("granted"),
                                                 QStringLiteral("denied"), QStringLiteral("other")}
                                       .contains(precondition.value(QStringLiteral("disposition"))
                                                     .toString())) {
                            const auto order_id =
                                precondition.value(QStringLiteral("order_id")).toString();
                            if (!isNamespacedId(order_id)) {
                                return crossReferenceFailure(
                                    resource, QStringLiteral("operations/preconditions"),
                                    QStringLiteral("order precondition id is not canonical"));
                            }
                            subject_key = kind + u':' + order_id;
                        } else if (kind == QStringLiteral("deadline_status") &&
                                   hasExactKeys(precondition, {"kind", "deadline_id", "status"}) &&
                                   QSet<QString>{
                                       QStringLiteral("open"), QStringLiteral("satisfied"),
                                       QStringLiteral("reached"), QStringLiteral("elapsed"),
                                       QStringLiteral("not_elapsed")}
                                       .contains(precondition.value(QStringLiteral("status"))
                                                     .toString())) {
                            const auto deadline_id =
                                precondition.value(QStringLiteral("deadline_id")).toString();
                            if (!isNamespacedId(deadline_id)) {
                                return crossReferenceFailure(
                                    resource, QStringLiteral("operations/preconditions"),
                                    QStringLiteral("deadline precondition id is not canonical"));
                            }
                            const auto status =
                                precondition.value(QStringLiteral("status")).toString();
                            precondition_deadline_ids.insert(deadline_id);
                            subject_key = kind + u':' + deadline_id + u':' + status;
                            if (status == QStringLiteral("open")) {
                                contradictory_subject_keys.push_back(
                                    kind + u':' + deadline_id + u':' + QStringLiteral("satisfied"));
                            } else if (status == QStringLiteral("satisfied")) {
                                contradictory_subject_keys.push_back(kind + u':' + deadline_id +
                                                                     u':' + QStringLiteral("open"));
                            } else if (status == QStringLiteral("elapsed")) {
                                contradictory_subject_keys.push_back(kind + u':' + deadline_id +
                                                                     u':' +
                                                                     QStringLiteral("not_elapsed"));
                            } else if (status == QStringLiteral("not_elapsed")) {
                                contradictory_subject_keys.push_back(
                                    kind + u':' + deadline_id + u':' + QStringLiteral("elapsed"));
                            }
                        } else if (kind == QStringLiteral("argument_scheduled") &&
                                   hasExactKeys(precondition, {"kind", "scheduled"}) &&
                                   precondition.value(QStringLiteral("scheduled")).isBool()) {
                            const auto scheduled =
                                precondition.value(QStringLiteral("scheduled")).toBool();
                            subject_key = kind + u':' + (scheduled ? u"true" : u"false");
                            contradictory_subject_keys.push_back(kind + u':' +
                                                                 (scheduled ? u"false" : u"true"));
                            if (!scheduled) {
                                contradictory_subject_keys.push_back(
                                    QStringLiteral("argument_date_status:reached"));
                            }
                        } else if (kind == QStringLiteral("argument_date_status") &&
                                   hasExactKeys(precondition, {"kind", "status"}) &&
                                   precondition.value(QStringLiteral("status")).toString() ==
                                       QStringLiteral("reached")) {
                            subject_key = QStringLiteral("argument_date_status:reached");
                            contradictory_subject_keys.push_back(
                                QStringLiteral("argument_scheduled:false"));
                        } else if (kind == QStringLiteral("judgment_issued") &&
                                   hasExactKeys(precondition, {"kind", "issued"}) &&
                                   precondition.value(QStringLiteral("issued")).isBool()) {
                            subject_key = kind;
                        } else {
                            return crossReferenceFailure(
                                resource, QStringLiteral("operations/preconditions"),
                                QStringLiteral("precondition tag and operands do not match a "
                                               "closed supported form"));
                        }
                        if (subject_key.isEmpty() || precondition_subjects.contains(subject_key) ||
                            std::ranges::any_of(
                                contradictory_subject_keys, [&](const QString& contradictory) {
                                    return precondition_subjects.contains(contradictory);
                                })) {
                            return crossReferenceFailure(
                                resource, QStringLiteral("operations/preconditions"),
                                QStringLiteral("an operation repeats or contradicts a "
                                               "precondition subject"));
                        }
                        precondition_subjects.insert(subject_key);
                    }
                }
            }

            for (const auto& [order_id, operation_id] : order_event_bases) {
                const auto operation = operation_documents.constFind(operation_id);
                if (!isNamespacedId(order_id) || operation == operation_documents.constEnd() ||
                    operation->value(QStringLiteral("opcode")).toString() !=
                        QStringLiteral("enter_order")) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("operations/deadline_event_base"),
                        QStringLiteral("order-occurrence bases must identify a canonical order and "
                                       "an EnterOrder operation"));
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
            QSet<QString> declared_filing_type_ids;
            QSet<QString> declared_deadline_ids = named_deadline_ids;
            QSet<QString> exact_deadline_ids = named_deadline_ids;
            QSet<QString> deficiency_deadline_prefixes;
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
                declared_filing_type_ids.insert(filing_type_id);
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
                    [&](const QJsonObject& plan, bool deficiency) -> std::expected<void, Error> {
                    const auto deadline_id = plan.value(QStringLiteral("deadline_id")).toString();
                    const auto operation_id = plan.value(QStringLiteral("operation_id")).toString();
                    const auto operation = operation_documents.constFind(operation_id);
                    if (declared_deadline_ids.contains(deadline_id) ||
                        operation == operation_documents.constEnd() ||
                        operation->value(QStringLiteral("opcode")).toString() !=
                            QStringLiteral("calculate_deadline") ||
                        operation->value(QStringLiteral("stage_id")).toString() != stage_id ||
                        operation->contains(QStringLiteral("deadline_base_id")) ||
                        operation->contains(QStringLiteral("produced_deadline_id")) ||
                        operation->contains(QStringLiteral("deadline_event_base"))) {
                        return crossReferenceFailure(
                            resource, QStringLiteral("filing_routes/deadline"),
                            QStringLiteral(
                                "deadline ids must be unique and use a local, independent "
                                "calculation"));
                    }
                    const auto namespace_overlap =
                        deficiency ? std::ranges::any_of(exact_deadline_ids,
                                                         [&](const QString& exact) {
                                                             return deadlineNamespacesOverlap(
                                                                 deadline_id, exact);
                                                         }) ||
                                         std::ranges::any_of(deficiency_deadline_prefixes,
                                                             [&](const QString& prefix) {
                                                                 return deadlineNamespacesOverlap(
                                                                     deadline_id, prefix);
                                                             })
                                   : std::ranges::any_of(
                                         deficiency_deadline_prefixes, [&](const QString& prefix) {
                                             return deadlineNamespacesOverlap(deadline_id, prefix);
                                         });
                    if (namespace_overlap) {
                        return crossReferenceFailure(
                            resource, QStringLiteral("filing_routes/deadline"),
                            QStringLiteral("deadline ids and dynamic deficiency namespaces must "
                                           "be disjoint"));
                    }
                    declared_deadline_ids.insert(deadline_id);
                    if (deficiency) {
                        deficiency_deadline_prefixes.insert(deadline_id);
                    } else {
                        exact_deadline_ids.insert(deadline_id);
                    }
                    return {};
                };
                if (has_deficiency_deadline) {
                    const auto deficiency_plan = validateDeadlinePlan(
                        route.value(QStringLiteral("deficiency_deadline")).toObject(), true);
                    if (!deficiency_plan) {
                        return std::unexpected(deficiency_plan.error());
                    }
                }
                if (route.contains(QStringLiteral("accepted_deadline"))) {
                    const auto accepted_plan =
                        route.value(QStringLiteral("accepted_deadline")).toObject();
                    const auto accepted_result = validateDeadlinePlan(accepted_plan, false);
                    if (!accepted_result) {
                        return std::unexpected(accepted_result.error());
                    }
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
                    !exact_deadline_ids.contains(
                        route.value(QStringLiteral("satisfies_deadline_id")).toString())) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("filing_routes/satisfies_deadline_id"),
                        QStringLiteral("satisfied deadline is not produced by this workflow"));
                }
            }
            if (!std::ranges::all_of(deadline_base_ids, [&](const QString& deadline_id) {
                    return exact_deadline_ids.contains(deadline_id);
                })) {
                return crossReferenceFailure(
                    resource, QStringLiteral("operations/deadline_base_id"),
                    QStringLiteral("dependent deadline bases must resolve to exact produced "
                                   "deadline ids"));
            }
            if (!std::ranges::all_of(precondition_deadline_ids, [&](const QString& deadline_id) {
                    return exact_deadline_ids.contains(deadline_id);
                })) {
                return crossReferenceFailure(
                    resource, QStringLiteral("operations/preconditions/deadline_id"),
                    QStringLiteral("deadline preconditions must resolve to exact produced "
                                   "deadline ids"));
            }
            if (!std::ranges::all_of(precondition_filing_ids,
                                     [&declared_filing_type_ids](const QString& filing_id) {
                                         return declared_filing_type_ids.contains(filing_id);
                                     })) {
                return crossReferenceFailure(
                    resource, QStringLiteral("operations/preconditions"),
                    QStringLiteral(
                        "filing preconditions must reference declarations in the workflow"));
            }
            workflow_stages.insert(id, stages);
            workflow_operations.insert(id, operations);
            break;
        }
        case model::ResourceKind::Record: {
            QSet<QString> entries;
            QSet<int> entry_numbers;
            QSet<QString> dockets;
            for (const auto& value : document.value(QStringLiteral("dockets")).toArray()) {
                const auto docket = value.toObject();
                const auto docket_id = docket.value(QStringLiteral("docket_id")).toString();
                if (dockets.contains(docket_id)) {
                    return crossReferenceFailure(resource, QStringLiteral("dockets"),
                                                 QStringLiteral("docket ids must be unique"));
                }
                dockets.insert(docket_id);
                if (docket.contains(QStringLiteral("court_id"))) {
                    const auto docket_court =
                        requireKind(resource, QStringLiteral("dockets/court_id"),
                                    docket.value(QStringLiteral("court_id")).toString(),
                                    model::ResourceKind::Court);
                    if (!docket_court) {
                        return std::unexpected(docket_court.error());
                    }
                }
            }
            QHash<QString, QJsonObject> entries_by_id;
            QHash<QString, QString> parent_by_entry;
            QSet<QString> display_labels;
            for (const auto& value : document.value(QStringLiteral("docket_entries")).toArray()) {
                const auto entry = value.toObject();
                const auto entry_id = entry.value(QStringLiteral("entry_id")).toString();
                const auto entry_number = entry.value(QStringLiteral("entry_number")).toInt();
                const auto docket_id = entry.value(QStringLiteral("docket_id")).toString();
                const auto asset_path = entry.value(QStringLiteral("asset_path")).toString();
                const auto asset_digest = entry.value(QStringLiteral("asset_sha256")).toString();
                if (entries.contains(entry_id) || entry_numbers.contains(entry_number)) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("docket_entries"),
                        QStringLiteral("entry ids and numbers must be unique"));
                }
                entries.insert(entry_id);
                entry_numbers.insert(entry_number);
                entries_by_id.insert(entry_id, entry);
                if (entry.contains(QStringLiteral("docket_id")) && !dockets.contains(docket_id)) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("docket_entries/docket_id"),
                        QStringLiteral("entry %1 references undeclared docket %2")
                            .arg(entry_id, docket_id));
                }
                if (entry.contains(QStringLiteral("entry_label"))) {
                    const auto label_key =
                        docket_id + u'\n' + entry.value(QStringLiteral("entry_label")).toString();
                    if (display_labels.contains(label_key)) {
                        return crossReferenceFailure(
                            resource, QStringLiteral("docket_entries/entry_label"),
                            QStringLiteral("display labels must be unique within a docket"));
                    }
                    display_labels.insert(label_key);
                }
                const auto has_parent = entry.contains(QStringLiteral("parent_entry_id"));
                const auto has_relationship = entry.contains(QStringLiteral("relationship"));
                if (has_parent != has_relationship) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("docket_entries/parent_entry_id"),
                        QStringLiteral(
                            "parent_entry_id and relationship must be declared together"));
                }
                if (has_parent) {
                    parent_by_entry.insert(
                        entry_id, entry.value(QStringLiteral("parent_entry_id")).toString());
                }
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
            for (auto parent = parent_by_entry.constBegin(); parent != parent_by_entry.constEnd();
                 ++parent) {
                if (!entries_by_id.contains(parent.value()) || parent.key() == parent.value()) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("docket_entries/parent_entry_id"),
                        QStringLiteral("entry %1 has an orphaned or self parent")
                            .arg(parent.key()));
                }
                const auto child_docket =
                    entries_by_id.value(parent.key()).value(QStringLiteral("docket_id"));
                const auto parent_docket =
                    entries_by_id.value(parent.value()).value(QStringLiteral("docket_id"));
                if (child_docket != parent_docket) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("docket_entries/parent_entry_id"),
                        QStringLiteral("parent and child must belong to the same docket"));
                }
            }
            QSet<QString> resolved_parent_chains;
            for (const auto& entry_id : entries) {
                QSet<QString> chain;
                auto current = entry_id;
                while (parent_by_entry.contains(current) &&
                       !resolved_parent_chains.contains(current)) {
                    if (chain.contains(current)) {
                        return crossReferenceFailure(
                            resource, QStringLiteral("docket_entries/parent_entry_id"),
                            QStringLiteral("docket entry parent graph contains a cycle"));
                    }
                    chain.insert(current);
                    current = parent_by_entry.value(current);
                }
                resolved_parent_chains.unite(chain);
            }
            QSet<QString> page_anchor_ids;
            QSet<QString> citation_labels;
            QHash<QString, QJsonObject> anchors_by_id;
            for (const auto& value : document.value(QStringLiteral("page_anchors")).toArray()) {
                const auto anchor = value.toObject();
                const auto anchor_id = anchor.value(QStringLiteral("anchor_id")).toString();
                const auto entry_id = anchor.value(QStringLiteral("entry_id")).toString();
                const auto page_number = anchor.value(QStringLiteral("page_number")).toInt();
                if (page_anchor_ids.contains(anchor_id) || entries.contains(anchor_id) ||
                    !entries_by_id.contains(entry_id) ||
                    page_number >
                        entries_by_id.value(entry_id).value(QStringLiteral("page_count")).toInt()) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("page_anchors"),
                        QStringLiteral(
                            "anchors must be unique, unambiguous, attached, and in page range"));
                }
                page_anchor_ids.insert(anchor_id);
                anchors_by_id.insert(anchor_id, anchor);
                if (anchor.contains(QStringLiteral("citation_label"))) {
                    const auto citation = anchor.value(QStringLiteral("citation_label")).toString();
                    if (!model::isCanonicalAuthorityText(citation.toUtf8().toStdString(), 120) ||
                        citation_labels.contains(citation)) {
                        return crossReferenceFailure(
                            resource, QStringLiteral("page_anchors/citation_label"),
                            QStringLiteral("citation labels must be canonical and unique"));
                    }
                    citation_labels.insert(citation);
                }
            }

            const auto has_policy = document.contains(QStringLiteral("disclosure_policy"));
            const auto has_disclosures = document.contains(QStringLiteral("sealed_disclosures"));
            if (has_policy != has_disclosures) {
                return crossReferenceFailure(
                    resource, QStringLiteral("disclosure_policy"),
                    QStringLiteral("disclosure_policy and sealed_disclosures must be declared "
                                   "together"));
            }
            QSet<QString> sealed_issue_hidden_ids;
            if (has_policy) {
                const auto policy = document.value(QStringLiteral("disclosure_policy")).toObject();
                if (policy.value(QStringLiteral("unauthorized_projection")).toString() !=
                        QStringLiteral("public_counterparts_only") ||
                    policy.value(QStringLiteral("authorized_projection")).toString() !=
                        QStringLiteral("public_and_authorized_sealed") ||
                    policy.value(QStringLiteral("sealed_asset_access")).toString() !=
                        QStringLiteral("session_event_grant_required")) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("disclosure_policy"),
                        QStringLiteral("disclosure projection semantics must use the closed "
                                       "schema-v2 contract"));
                }

                QSet<QString> disclosure_ids;
                QSet<QString> disclosed_sealed_entries;
                QSet<QString> public_counterparts;
                QSet<QString> stable_anchor_ids;
                QSet<QString> mapped_sealed_anchors;
                QSet<QString> mapped_public_anchors;
                QHash<QString, QJsonObject> stable_public_anchors;
                for (const auto& disclosure_value :
                     document.value(QStringLiteral("sealed_disclosures")).toArray()) {
                    const auto disclosure = disclosure_value.toObject();
                    const auto disclosure_id =
                        disclosure.value(QStringLiteral("disclosure_id")).toString();
                    const auto sealed_id =
                        disclosure.value(QStringLiteral("sealed_entry_id")).toString();
                    const auto public_id =
                        disclosure.value(QStringLiteral("public_entry_id")).toString();
                    const auto motion_id =
                        disclosure.value(QStringLiteral("motion_entry_id")).toString();
                    const auto certificate_id =
                        disclosure.value(QStringLiteral("certificate_entry_id")).toString();
                    const auto authorization_authority_id =
                        disclosure.value(QStringLiteral("authorization_authority_id")).toString();
                    if (disclosure_ids.contains(disclosure_id) ||
                        stable_anchor_ids.contains(disclosure_id) ||
                        entries.contains(disclosure_id) || dockets.contains(disclosure_id) ||
                        page_anchor_ids.contains(disclosure_id) ||
                        !entries_by_id.contains(sealed_id) ||
                        !entries_by_id.value(sealed_id).value(QStringLiteral("sealed")).toBool() ||
                        disclosed_sealed_entries.contains(sealed_id)) {
                        return crossReferenceFailure(
                            resource, QStringLiteral("sealed_disclosures"),
                            QStringLiteral("disclosure and sealed-entry identities must be "
                                           "unique, unambiguous, and sealed"));
                    }
                    const auto canonical_authority =
                        authorities_by_id.constFind(authorization_authority_id);
                    if (canonical_authority == authorities_by_id.constEnd() ||
                        !canonical_authority.value().contains(QStringLiteral("authority_type")) ||
                        !canonical_authority.value().contains(QStringLiteral("source_url"))) {
                        return crossReferenceFailure(
                            resource,
                            QStringLiteral("sealed_disclosures/authorization_authority_id"),
                            QStringLiteral("authorization authority must resolve to exact "
                                           "canonical provenance"));
                    }
                    disclosure_ids.insert(disclosure_id);
                    disclosed_sealed_entries.insert(sealed_id);
                    sealed_issue_hidden_ids.insert(sealed_id);

                    const auto valid_public_entry = [&](const QString& entry_id) {
                        return entry_id.isEmpty() || (entries_by_id.contains(entry_id) &&
                                                      !entries_by_id.value(entry_id)
                                                           .value(QStringLiteral("sealed"))
                                                           .toBool() &&
                                                      entry_id != sealed_id);
                    };
                    if (!valid_public_entry(public_id) || !valid_public_entry(motion_id) ||
                        !valid_public_entry(certificate_id)) {
                        return crossReferenceFailure(
                            resource, QStringLiteral("sealed_disclosures"),
                            QStringLiteral("counterpart, motion, and certificate must resolve to "
                                           "distinct public entries"));
                    }
                    if (!public_id.isEmpty() &&
                        (public_counterparts.contains(public_id) ||
                         entries_by_id.value(public_id).value(QStringLiteral("docket_id")) !=
                             entries_by_id.value(sealed_id).value(QStringLiteral("docket_id")))) {
                        return crossReferenceFailure(
                            resource, QStringLiteral("sealed_disclosures/public_entry_id"),
                            QStringLiteral("public counterpart must be one-to-one in the same "
                                           "docket"));
                    }
                    if (!public_id.isEmpty()) {
                        public_counterparts.insert(public_id);
                    }
                    QSet<QString> support_ids;
                    for (const auto& support_id : {public_id, motion_id, certificate_id}) {
                        if (!support_id.isEmpty() && support_ids.contains(support_id)) {
                            return crossReferenceFailure(
                                resource, QStringLiteral("sealed_disclosures"),
                                QStringLiteral("public disclosure support entries must be "
                                               "distinct"));
                        }
                        if (!support_id.isEmpty()) {
                            support_ids.insert(support_id);
                        }
                    }

                    const auto mappings =
                        disclosure.value(QStringLiteral("anchor_mappings")).toArray();
                    if (public_id.isEmpty() && !mappings.isEmpty()) {
                        return crossReferenceFailure(
                            resource, QStringLiteral("sealed_disclosures/anchor_mappings"),
                            QStringLiteral("stable anchors require a public counterpart"));
                    }
                    for (const auto& mapping_value : mappings) {
                        const auto mapping = mapping_value.toObject();
                        const auto stable_id =
                            mapping.value(QStringLiteral("stable_anchor_id")).toString();
                        const auto sealed_anchor_id =
                            mapping.value(QStringLiteral("sealed_anchor_id")).toString();
                        const auto public_anchor_id =
                            mapping.value(QStringLiteral("public_anchor_id")).toString();
                        if (stable_anchor_ids.contains(stable_id) || entries.contains(stable_id) ||
                            dockets.contains(stable_id) || page_anchor_ids.contains(stable_id) ||
                            disclosure_ids.contains(stable_id) ||
                            mapped_sealed_anchors.contains(sealed_anchor_id) ||
                            mapped_public_anchors.contains(public_anchor_id) ||
                            !anchors_by_id.contains(sealed_anchor_id) ||
                            !anchors_by_id.contains(public_anchor_id) ||
                            anchors_by_id.value(sealed_anchor_id)
                                    .value(QStringLiteral("entry_id"))
                                    .toString() != sealed_id ||
                            anchors_by_id.value(public_anchor_id)
                                    .value(QStringLiteral("entry_id"))
                                    .toString() != public_id) {
                            return crossReferenceFailure(
                                resource, QStringLiteral("sealed_disclosures/anchor_mappings"),
                                QStringLiteral("twin anchors must be unique and resolve to their "
                                               "declared sides"));
                        }
                        stable_anchor_ids.insert(stable_id);
                        mapped_sealed_anchors.insert(sealed_anchor_id);
                        mapped_public_anchors.insert(public_anchor_id);
                        sealed_issue_hidden_ids.insert(sealed_anchor_id);
                        auto public_anchor = anchors_by_id.value(public_anchor_id);
                        public_anchor.insert(QStringLiteral("anchor_id"), stable_id);
                        stable_public_anchors.insert(stable_id, std::move(public_anchor));
                    }
                }
                // Every physical anchor on a disclosed sealed entry is
                // sensitive, including anchors that have no public twin.
                // Case issues may ground only through declared stable IDs.
                for (auto anchor = anchors_by_id.constBegin(); anchor != anchors_by_id.constEnd();
                     ++anchor) {
                    if (disclosed_sealed_entries.contains(
                            anchor.value().value(QStringLiteral("entry_id")).toString())) {
                        sealed_issue_hidden_ids.insert(anchor.key());
                    }
                }
                // A projected public child must never retain a raw reference
                // to a sealed parent that is absent from the public graph.
                for (auto parent = parent_by_entry.constBegin();
                     parent != parent_by_entry.constEnd(); ++parent) {
                    const auto& child = entries_by_id.value(parent.key());
                    if (!child.value(QStringLiteral("sealed")).toBool() &&
                        disclosed_sealed_entries.contains(parent.value())) {
                        return crossReferenceFailure(
                            resource, QStringLiteral("docket_entries/parent_entry_id"),
                            QStringLiteral("a public entry cannot have a sealed parent"));
                    }
                }
                for (auto entry = entries_by_id.constBegin(); entry != entries_by_id.constEnd();
                     ++entry) {
                    if (entry.value().value(QStringLiteral("sealed")).toBool() &&
                        !disclosed_sealed_entries.contains(entry.key())) {
                        return crossReferenceFailure(
                            resource, QStringLiteral("sealed_disclosures"),
                            QStringLiteral("every sealed entry requires exactly one disclosure"));
                    }
                }
                for (auto stable = stable_public_anchors.constBegin();
                     stable != stable_public_anchors.constEnd(); ++stable) {
                    anchors_by_id.insert(stable.key(), stable.value());
                    page_anchor_ids.insert(stable.key());
                }
            }
            record_docket_entries.insert(id, entries_by_id);
            record_page_anchors.insert(id, anchors_by_id);
            entries.unite(page_anchor_ids);
            entries.subtract(sealed_issue_hidden_ids);
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
            QHash<QString, QSet<QString>> authorities_by_issue;
            QHash<QString, QSet<QString>> anchors_by_issue;
            for (const auto& value : document.value(QStringLiteral("issues")).toArray()) {
                const auto issue = value.toObject();
                const auto issue_id = issue.value(QStringLiteral("issue_id")).toString();
                if (issues.contains(issue_id)) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("issues"),
                        QStringLiteral("duplicate issue id %1").arg(issue_id));
                }
                issues.insert(issue_id);
                authorities_by_issue.insert(
                    issue_id, stringSet(issue.value(QStringLiteral("authority_ids")).toArray()));
                anchors_by_issue.insert(
                    issue_id,
                    stringSet(issue.value(QStringLiteral("record_anchor_ids")).toArray()));
            }
            case_issues.insert(id, issues);
            case_record_ids.insert(id, document.value(QStringLiteral("record_id")).toString());
            case_issue_authorities.insert(id, std::move(authorities_by_issue));
            case_issue_record_anchors.insert(id, std::move(anchors_by_issue));
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
        QSet<QString> procedure_authority_ids;
        for (const auto& value : document.value(QStringLiteral("authority_set_ids")).toArray()) {
            const auto authority_set =
                requireKind(resource, QStringLiteral("authority_set_ids"), value.toString(),
                            model::ResourceKind::AuthoritySet);
            if (!authority_set) {
                return std::unexpected(authority_set.error());
            }
            for (const auto& authority_value :
                 (*authority_set)->document.value(QStringLiteral("authorities")).toArray()) {
                procedure_authority_ids.insert(
                    authority_value.toObject().value(QStringLiteral("authority_id")).toString());
            }
        }
        if (resource.descriptor.schema_version == 2) {
            const auto workflow_id = document.value(QStringLiteral("workflow_id")).toString();
            const auto workflow_authorities = workflow_authority_ids.value(workflow_id);
            const auto catalog_authorities = catalog_authority_ids.value(catalog_id);
            const auto undeclared_workflow_authority = std::ranges::find_if(
                workflow_authorities, [&procedure_authority_ids](const QString& authority_id) {
                    return !procedure_authority_ids.contains(authority_id);
                });
            const auto undeclared_catalog_authority = std::ranges::find_if(
                catalog_authorities, [&procedure_authority_ids](const QString& authority_id) {
                    return !procedure_authority_ids.contains(authority_id);
                });
            if (undeclared_workflow_authority != workflow_authorities.end() ||
                undeclared_catalog_authority != catalog_authorities.end()) {
                return crossReferenceFailure(
                    resource, QStringLiteral("authority_set_ids"),
                    QStringLiteral("procedure authority sets do not cover its workflow and filing "
                                   "catalog authority ids"));
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
                if (resource.descriptor.schema_version == 2) {
                    const auto primary_id =
                        authority.value(QStringLiteral("primary_authority_id")).toString();
                    if (!authority_ids.contains(primary_id)) {
                        return crossReferenceFailure(
                            resource, QStringLiteral("operations/authority/primary_authority_id"),
                            QStringLiteral("unresolved canonical authority %1").arg(primary_id));
                    }
                    QSet<QString> basis_ids{primary_id};
                    for (const auto& supporting :
                         authority.value(QStringLiteral("supporting_authority_ids")).toArray()) {
                        const auto supporting_id = supporting.toString();
                        if (basis_ids.contains(supporting_id) ||
                            !authority_ids.contains(supporting_id)) {
                            return crossReferenceFailure(
                                resource,
                                QStringLiteral("operations/authority/supporting_authority_ids"),
                                QStringLiteral(
                                    "supporting authority ids must be unique, resolved, and "
                                    "different from the primary authority"));
                        }
                        basis_ids.insert(supporting_id);
                    }
                    continue;
                }
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
            QSet<QString> case_authority_ids;
            if (resource.descriptor.schema_version == 2) {
                for (const auto& set_id :
                     (*procedure)->document.value(QStringLiteral("authority_set_ids")).toArray()) {
                    const auto authority_set = requireKind(
                        resource, QStringLiteral("procedure_profile_id/authority_set_ids"),
                        set_id.toString(), model::ResourceKind::AuthoritySet);
                    if (!authority_set) {
                        return std::unexpected(authority_set.error());
                    }
                    for (const auto& authority_value :
                         (*authority_set)
                             ->document.value(QStringLiteral("authorities"))
                             .toArray()) {
                        case_authority_ids.insert(authority_value.toObject()
                                                      .value(QStringLiteral("authority_id"))
                                                      .toString());
                    }
                }
            }
            for (const auto& value : document.value(QStringLiteral("actors")).toArray()) {
                const auto role_id = value.toObject().value(QStringLiteral("role_id")).toString();
                if (!roles.contains(role_id)) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("actors/role_id"),
                        QStringLiteral("role %1 is not declared by the procedure").arg(role_id));
                }
            }
            QHash<QString, QSet<QString>> issue_targets;
            QHash<QString, QSet<QString>> issue_authorities;
            QHash<QString, QSet<QString>> issue_record_anchors;
            QSet<QString> issue_ids;
            QSet<QString> all_target_ids;
            for (const auto& value : document.value(QStringLiteral("issues")).toArray()) {
                const auto issue = value.toObject();
                const auto issue_id = issue.value(QStringLiteral("issue_id")).toString();
                if (!isNamespacedId(issue_id) || issue_ids.contains(issue_id)) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("issues/issue_id"),
                        QStringLiteral("issue ids must be canonical and unique"));
                }
                issue_ids.insert(issue_id);
                issue_authorities.insert(
                    issue_id, stringSet(issue.value(QStringLiteral("authority_ids")).toArray()));
                issue_record_anchors.insert(
                    issue_id,
                    stringSet(issue.value(QStringLiteral("record_anchor_ids")).toArray()));
                if (issue.contains(QStringLiteral("target_ids"))) {
                    const auto target_values = issue.value(QStringLiteral("target_ids"));
                    const auto targets = stringSet(target_values.toArray());
                    if (!target_values.isArray() || targets.isEmpty() ||
                        target_values.toArray().size() > maximum_disposition_targets ||
                        targets.size() != target_values.toArray().size() ||
                        all_target_ids.size() + targets.size() > maximum_disposition_targets ||
                        std::ranges::any_of(
                            targets,
                            [](const QString& target_id) { return !isNamespacedId(target_id); }) ||
                        std::ranges::any_of(targets, [&all_target_ids](const QString& target_id) {
                            return all_target_ids.contains(target_id);
                        })) {
                        return crossReferenceFailure(
                            resource, QStringLiteral("issues/target_ids"),
                            QStringLiteral("disposition target ids must be nonempty and unique "
                                           "across the case"));
                    }
                    issue_targets.insert(issue_id, targets);
                    all_target_ids.unite(targets);
                }
                for (const auto& authority :
                     issue.value(QStringLiteral("authority_ids")).toArray()) {
                    if (!authority_ids.contains(authority.toString()) ||
                        (resource.descriptor.schema_version == 2 &&
                         !case_authority_ids.contains(authority.toString()))) {
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
            const auto has_structured_plan =
                document.contains(QStringLiteral("disposition_plans")) ||
                document.contains(QStringLiteral("authored_disposition_plan_id")) ||
                !issue_targets.isEmpty();
            if (has_structured_plan) {
                const auto plan_value = document.value(QStringLiteral("disposition_plans"));
                const auto authored_plan_id =
                    document.value(QStringLiteral("authored_disposition_plan_id")).toString();
                if (issue_targets.size() !=
                        document.value(QStringLiteral("issues")).toArray().size() ||
                    !plan_value.isArray() || plan_value.toArray().isEmpty() ||
                    plan_value.toArray().size() > maximum_disposition_plans ||
                    !isNamespacedId(authored_plan_id)) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("disposition_plans"),
                        QStringLiteral("structured disposition fields must form one complete "
                                       "capability-gated contract"));
                }
                QSet<QString> plan_ids;
                for (const auto& plan_value_item : plan_value.toArray()) {
                    if (!plan_value_item.isObject()) {
                        return crossReferenceFailure(
                            resource, QStringLiteral("disposition_plans"),
                            QStringLiteral("disposition plans must contain objects"));
                    }
                    const auto plan = plan_value_item.toObject();
                    const auto plan_id = plan.value(QStringLiteral("plan_id")).toString();
                    const auto finality = plan.value(QStringLiteral("finality")).toString();
                    const auto digest = plan.value(QStringLiteral("digest")).toString();
                    const auto components = plan.value(QStringLiteral("components")).toArray();
                    if (!hasExactKeys(plan, {"plan_id", "finality", "digest", "components"}) ||
                        !isNamespacedId(plan_id) || plan_ids.contains(plan_id) ||
                        !plan.value(QStringLiteral("components")).isArray() ||
                        components.isEmpty() ||
                        components.size() > maximum_disposition_components || !isSha256(digest) ||
                        (finality != QStringLiteral("final") &&
                         finality != QStringLiteral("nonfinal")) ||
                        digest != canonicalDispositionPlanDigest(
                                      QString::fromStdString(resource.descriptor.id), disposition,
                                      plan)) {
                        return crossReferenceFailure(
                            resource, QStringLiteral("disposition_plans"),
                            QStringLiteral("plan ids, finality, components, and canonical digest "
                                           "must be valid"));
                    }
                    plan_ids.insert(plan_id);
                    QSet<QString> covered_targets;
                    for (const auto& component_value : components) {
                        if (!component_value.isObject()) {
                            return crossReferenceFailure(
                                resource, QStringLiteral("disposition_plans/components"),
                                QStringLiteral("components must be objects"));
                        }
                        const auto component = component_value.toObject();
                        const auto component_issue =
                            component.value(QStringLiteral("issue_id")).toString();
                        const auto component_target =
                            component.value(QStringLiteral("target_id")).toString();
                        const auto target_key = component_issue + u'\n' + component_target;
                        const auto scope = component.value(QStringLiteral("scope")).toString();
                        const auto action = component.value(QStringLiteral("action")).toString();
                        const auto remand = component.value(QStringLiteral("remand"));
                        const auto component_authority_values =
                            component.value(QStringLiteral("authority_ids"));
                        const auto component_anchor_values =
                            component.value(QStringLiteral("record_anchor_ids"));
                        static const QSet<QString> actions{
                            QStringLiteral("affirm"), QStringLiteral("reverse"),
                            QStringLiteral("vacate"), QStringLiteral("dismiss"),
                            QStringLiteral("grant"),  QStringLiteral("deny"),
                        };
                        static const QSet<QString> remand_actions{
                            QStringLiteral("reverse"),
                            QStringLiteral("vacate"),
                            QStringLiteral("dismiss"),
                            QStringLiteral("grant"),
                        };
                        if (!hasExactKeys(component,
                                          {"issue_id", "target_id", "scope", "action", "remand",
                                           "authority_ids", "record_anchor_ids"}) ||
                            !isNamespacedId(component_issue) || !isNamespacedId(component_target) ||
                            !issue_targets.value(component_issue).contains(component_target) ||
                            covered_targets.contains(target_key) ||
                            (scope != QStringLiteral("whole") && scope != QStringLiteral("part")) ||
                            !actions.contains(action) || !remand.isBool() ||
                            (remand.toBool() && !remand_actions.contains(action))) {
                            return crossReferenceFailure(
                                resource, QStringLiteral("disposition_plans/components"),
                                QStringLiteral("component targets, scope, action, remand, and "
                                               "overlap must be valid"));
                        }
                        covered_targets.insert(target_key);
                        const auto component_authorities =
                            stringSet(component_authority_values.toArray());
                        const auto component_anchors = stringSet(component_anchor_values.toArray());
                        if (!component_authority_values.isArray() ||
                            !component_anchor_values.isArray() || component_authorities.isEmpty() ||
                            component_anchors.isEmpty() ||
                            component_authority_values.toArray().size() >
                                maximum_component_authorities ||
                            component_anchor_values.toArray().size() >
                                maximum_component_record_anchors ||
                            component_authorities.size() !=
                                component_authority_values.toArray().size() ||
                            component_anchors.size() != component_anchor_values.toArray().size() ||
                            std::ranges::any_of(component_authorities,
                                                [](const QString& authority_id) {
                                                    return !isNamespacedId(authority_id);
                                                }) ||
                            std::ranges::any_of(component_anchors,
                                                [](const QString& anchor_id) {
                                                    return !isNamespacedId(anchor_id);
                                                }) ||
                            !std::ranges::all_of(component_authorities,
                                                 [&](const QString& authority_id) {
                                                     return authority_ids.contains(authority_id) &&
                                                            case_authority_ids.contains(
                                                                authority_id) &&
                                                            issue_authorities.value(component_issue)
                                                                .contains(authority_id);
                                                 }) ||
                            !std::ranges::all_of(component_anchors, [&](const QString& anchor_id) {
                                return record_entries.value(record_id).contains(anchor_id) &&
                                       issue_record_anchors.value(component_issue)
                                           .contains(anchor_id);
                            })) {
                            return crossReferenceFailure(
                                resource, QStringLiteral("disposition_plans/components"),
                                QStringLiteral("component authority and record grounding must "
                                               "resolve within its issue"));
                        }
                    }
                }
                if (!plan_ids.contains(authored_plan_id)) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("authored_disposition_plan_id"),
                        QStringLiteral("authored plan is not declared by the case"));
                }
            }
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
            QSet<QString> permitted_issues;
            for (const auto& issue :
                 document.value(QStringLiteral("permitted_issue_ids")).toArray()) {
                if (!case_issues.value(case_id).contains(issue.toString())) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("permitted_issue_ids"),
                        QStringLiteral("issue %1 is not in the configured case")
                            .arg(issue.toString()));
                }
                permitted_issues.insert(issue.toString());
            }
            if (document.value(QStringLiteral("rebuttal_seconds")).toInt() >
                document.value(QStringLiteral("total_seconds")).toInt()) {
                return crossReferenceFailure(resource, QStringLiteral("rebuttal_seconds"),
                                             QStringLiteral("cannot exceed total_seconds"));
            }
            if (!document.contains(QStringLiteral("grounded_question_bank"))) {
                break;
            }

            const auto bank_value = document.value(QStringLiteral("grounded_question_bank"));
            const auto bank = bank_value.toObject();
            const auto mode = bank.value(QStringLiteral("mode")).toString();
            const auto bindings_value = bank.value(QStringLiteral("issue_topic_bindings"));
            const auto questions_value = bank.value(QStringLiteral("questions"));
            if (!bank_value.isObject() ||
                !hasExactKeys(bank,
                              {"mode", "grounding_digest", "issue_topic_bindings", "questions"}) ||
                (mode != QStringLiteral("actual_record") &&
                 mode != QStringLiteral("counterfactual_training")) ||
                !isSha256(bank.value(QStringLiteral("grounding_digest")).toString()) ||
                !bindings_value.isArray() || bindings_value.toArray().isEmpty() ||
                bindings_value.toArray().size() > maximum_argument_issue_bindings ||
                !questions_value.isArray() || questions_value.toArray().isEmpty() ||
                questions_value.toArray().size() > maximum_authored_questions) {
                return crossReferenceFailure(
                    resource, QStringLiteral("grounded_question_bank"),
                    QStringLiteral("bank shape, mode, digest, or bounds are invalid"));
            }

            const auto is_focus_topic = [](const QString& topic_id) {
                return model::argumentFocusTopicFromId(topic_id.toStdString()).has_value();
            };
            QHash<QString, QSet<QString>> topics_by_issue;
            for (const auto& binding_value : bindings_value.toArray()) {
                const auto binding = binding_value.toObject();
                const auto issue_id = binding.value(QStringLiteral("issue_id")).toString();
                const auto topic_values = binding.value(QStringLiteral("topic_ids"));
                const auto topics = stringSet(topic_values.toArray());
                if (!binding_value.isObject() ||
                    !hasExactKeys(binding, {"issue_id", "topic_ids"}) ||
                    !permitted_issues.contains(issue_id) || topics_by_issue.contains(issue_id) ||
                    !topic_values.isArray() || topics.isEmpty() ||
                    topic_values.toArray().size() > maximum_argument_topics_per_issue ||
                    topics.size() != topic_values.toArray().size() ||
                    std::ranges::any_of(topics, [&is_focus_topic](const QString& topic_id) {
                        return !is_focus_topic(topic_id);
                    })) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("grounded_question_bank/issue_topic_bindings"),
                        QStringLiteral("bindings must uniquely cover permitted issues with "
                                       "closed reusable topics"));
                }
                topics_by_issue.insert(issue_id, topics);
            }
            if (topics_by_issue.size() != permitted_issues.size() ||
                std::ranges::any_of(permitted_issues, [&topics_by_issue](const QString& issue_id) {
                    return !topics_by_issue.contains(issue_id);
                })) {
                return crossReferenceFailure(
                    resource, QStringLiteral("grounded_question_bank/issue_topic_bindings"),
                    QStringLiteral("bindings must exactly cover permitted_issue_ids"));
            }

            QSet<QString> bank_topics;
            for (auto topics = topics_by_issue.cbegin(); topics != topics_by_issue.cend();
                 ++topics) {
                bank_topics.unite(topics.value());
            }
            for (const auto& seat_value :
                 (*bench)->document.value(QStringLiteral("seats")).toArray()) {
                const auto profile = requireKind(
                    resource, QStringLiteral("bench_configuration_id/seats/profile_id"),
                    seat_value.toObject().value(QStringLiteral("profile_id")).toString(),
                    model::ResourceKind::JudgeProfile);
                if (!profile) {
                    return std::unexpected(profile.error());
                }
                bool has_positive_bank_focus = false;
                for (const auto& focus_value : (*profile)
                                                   ->document.value(QStringLiteral("interaction"))
                                                   .toObject()
                                                   .value(QStringLiteral("issue_focus"))
                                                   .toArray()) {
                    const auto focus = focus_value.toObject();
                    const auto topic_id = focus.value(QStringLiteral("topic_id")).toString();
                    if (!is_focus_topic(topic_id)) {
                        return crossReferenceFailure(
                            resource, QStringLiteral("bench_configuration_id/issue_focus"),
                            QStringLiteral("grounded-question benches require closed reusable "
                                           "focus topics"));
                    }
                    has_positive_bank_focus =
                        has_positive_bank_focus ||
                        (focus.value(QStringLiteral("weight")).toDouble() > 0.0 &&
                         bank_topics.contains(topic_id));
                }
                if (!has_positive_bank_focus) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("bench_configuration_id/issue_focus"),
                        QStringLiteral("every grounded-question bench seat requires a positive "
                                       "focus represented in the question bank"));
                }
            }

            const auto record_id = case_record_ids.value(case_id);
            const auto record_entry_map = record_docket_entries.value(record_id);
            const auto record_anchor_map = record_page_anchors.value(record_id);
            const auto issue_authority_map = case_issue_authorities.value(case_id);
            const auto issue_anchor_map = case_issue_record_anchors.value(case_id);
            QSet<QString> question_ids;
            QSet<QString> grounding_ids;
            QSet<QString> covered_issue_topics;
            QHash<QString, qsizetype> questions_per_issue;
            for (const auto& question_value : questions_value.toArray()) {
                const auto question = question_value.toObject();
                const auto question_id = question.value(QStringLiteral("question_id")).toString();
                const auto issue_id = question.value(QStringLiteral("issue_id")).toString();
                const auto topic_id = question.value(QStringLiteral("topic_id")).toString();
                const auto prompt = question.value(QStringLiteral("prompt")).toString();
                const auto grounding_value = question.value(QStringLiteral("grounding"));
                if (!question_value.isObject() ||
                    !hasExactKeys(question,
                                  {"question_id", "issue_id", "topic_id", "prompt", "grounding"}) ||
                    !isNamespacedId(question_id) || question_ids.contains(question_id) ||
                    !permitted_issues.contains(issue_id) ||
                    !topics_by_issue.value(issue_id).contains(topic_id) ||
                    !isCanonicalQuestionPrompt(prompt) || !grounding_value.isArray() ||
                    grounding_value.toArray().isEmpty() ||
                    grounding_value.toArray().size() > maximum_question_grounding ||
                    ++questions_per_issue[issue_id] > maximum_authored_questions_per_issue) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("grounded_question_bank/questions"),
                        QStringLiteral("question identity, topic, prompt, grounding, or bounds "
                                       "are invalid"));
                }
                question_ids.insert(question_id);
                covered_issue_topics.insert(issue_id + u'\n' + topic_id);
                for (const auto& grounding_value_item : grounding_value.toArray()) {
                    const auto grounding = grounding_value_item.toObject();
                    const auto grounding_id =
                        grounding.value(QStringLiteral("grounding_id")).toString();
                    const auto kind = grounding.value(QStringLiteral("kind")).toString();
                    if (!grounding_value_item.isObject() || !isNamespacedId(grounding_id) ||
                        grounding_ids.contains(grounding_id)) {
                        return crossReferenceFailure(
                            resource,
                            QStringLiteral("grounded_question_bank/questions/grounding_id"),
                            QStringLiteral("grounding ids must be canonical and bank-unique"));
                    }
                    grounding_ids.insert(grounding_id);
                    if (kind == QStringLiteral("authority")) {
                        const auto authority_id =
                            grounding.value(QStringLiteral("authority_id")).toString();
                        if (!hasExactKeys(grounding, {"grounding_id", "kind", "authority_id"}) ||
                            !authorities_by_id.contains(authority_id) ||
                            !issue_authority_map.value(issue_id).contains(authority_id)) {
                            return crossReferenceFailure(
                                resource,
                                QStringLiteral("grounded_question_bank/questions/authority_id"),
                                QStringLiteral("authority grounding must resolve within its "
                                               "case issue"));
                        }
                    } else if (kind == QStringLiteral("brief_page")) {
                        const auto entry_id =
                            grounding.value(QStringLiteral("entry_id")).toString();
                        const auto entry = record_entry_map.value(entry_id);
                        const auto page_number = grounding.value(QStringLiteral("page_number"));
                        if (!hasExactKeys(grounding,
                                          {"grounding_id", "kind", "entry_id", "page_number"}) ||
                            !issue_anchor_map.value(issue_id).contains(entry_id) ||
                            entry.isEmpty() || entry.value(QStringLiteral("sealed")).toBool() ||
                            !stringSet(entry.value(QStringLiteral("tags")).toArray())
                                 .contains(QStringLiteral("brief")) ||
                            !isExactInteger(page_number, 1,
                                            entry.value(QStringLiteral("page_count")).toInt())) {
                            return crossReferenceFailure(
                                resource,
                                QStringLiteral("grounded_question_bank/questions/brief_page"),
                                QStringLiteral("brief pages must be issue-scoped, tagged, "
                                               "unsealed, and in range"));
                        }
                    } else if (kind == QStringLiteral("record_page")) {
                        const auto anchor_id =
                            grounding.value(QStringLiteral("anchor_id")).toString();
                        const auto anchor = record_anchor_map.value(anchor_id);
                        const auto entry = record_entry_map.value(
                            anchor.value(QStringLiteral("entry_id")).toString());
                        if (!hasExactKeys(grounding, {"grounding_id", "kind", "anchor_id"}) ||
                            !issue_anchor_map.value(issue_id).contains(anchor_id) ||
                            anchor.isEmpty() || entry.isEmpty() ||
                            entry.value(QStringLiteral("sealed")).toBool()) {
                            return crossReferenceFailure(
                                resource,
                                QStringLiteral("grounded_question_bank/questions/record_page"),
                                QStringLiteral("record pages must resolve to an issue-scoped "
                                               "unsealed page anchor"));
                        }
                    } else {
                        return crossReferenceFailure(
                            resource, QStringLiteral("grounded_question_bank/questions/kind"),
                            QStringLiteral("grounding kind is unsupported"));
                    }
                }
            }
            for (auto topics = topics_by_issue.constBegin(); topics != topics_by_issue.constEnd();
                 ++topics) {
                for (const auto& topic_id : topics.value()) {
                    if (!covered_issue_topics.contains(topics.key() + u'\n' + topic_id)) {
                        return crossReferenceFailure(
                            resource, QStringLiteral("grounded_question_bank/questions"),
                            QStringLiteral("every permitted issue-topic binding requires an "
                                           "authored grounded question"));
                    }
                }
            }
            const auto expected_digest = canonicalQuestionBankDigest(
                case_id, QString::fromStdString(resource.descriptor.id), bank, authorities_by_id,
                record_entry_map, record_anchor_map);
            if (bank.value(QStringLiteral("grounding_digest")).toString() != expected_digest) {
                return crossReferenceFailure(
                    resource, QStringLiteral("grounded_question_bank/grounding_digest"),
                    QStringLiteral("digest does not match the resolved canonical question bank"));
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

namespace {

std::expected<LoadedPack, Error>
readDirectoryImpl(const QString& directory, PackValidationScope scope,
                  const std::optional<QString>& authoring_review_id) {
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
    const auto parsed = SchemaValidator::parseObject(*manifest_bytes, manifest_path);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    const auto manifest = *parsed;
    const auto manifest_schema_value = manifest.value(QStringLiteral("schema_version"));
    if (!isExactInteger(manifest_schema_value, minimum_supported_schema_version,
                        maximum_supported_schema_version)) {
        return fail(ErrorCode::UnsupportedSchema,
                    QStringLiteral("Unsupported manifest schema version"));
    }
    const auto manifest_schema_version = static_cast<int>(manifest_schema_value.toDouble());
    const auto schema_validator =
        SchemaValidator::fromBundledSchemas(static_cast<std::uint32_t>(manifest_schema_version));
    if (!schema_validator) {
        return std::unexpected(schema_validator.error());
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
    if (!isNamespacedId(pack_id) ||
        !isValidPackVersion(version, static_cast<std::uint32_t>(manifest_schema_version)) ||
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
    const auto supported_capabilities = CapabilityRegistry::validateDeclarations(
        static_cast<std::uint32_t>(manifest_schema_version), capabilities);
    if (!supported_capabilities) {
        return std::unexpected(supported_capabilities.error());
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
            !isValidPackVersion(dependency_version,
                                static_cast<std::uint32_t>(manifest_schema_version)) ||
            !isSha256(digest) || id == pack_id || dependency_ids.contains(id)) {
            return fail(ErrorCode::InvalidManifest,
                        QStringLiteral("Invalid, duplicate, or self-referential dependency"));
        }
        dependency_ids.insert(id);
        dependencies.push_back(model::PackDependency{
            model::PackRevision{model::PackId{id.toStdString()}, dependency_version.toStdString(),
                                digest.toStdString()}});
    }

    std::vector<ContentDescriptor> contents;
    std::vector<model::ResourceKind> resource_kinds;
    QSet<QString> content_ids;
    QSet<QString> content_paths;
    QSet<QString> declared_files{QStringLiteral("manifest.json")};
    std::vector<QString> declared_payload_paths;
    declared_payload_paths.reserve(
        static_cast<std::size_t>(content_values.size() + blob_values.size()));
    resource_kinds.reserve(static_cast<std::size_t>(content_values.size()));
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
        if (!isExactInteger(content_schema, minimum_supported_schema_version,
                            maximum_supported_schema_version) ||
            static_cast<int>(content_schema.toDouble()) != manifest_schema_version) {
            return fail(
                ErrorCode::UnsupportedSchema,
                QStringLiteral("Unsupported or cross-version resource schema for %1").arg(id));
        }
        const auto content_schema_version = static_cast<int>(content_schema.toDouble());
        const auto definition = kindDefinition(kind, content_schema_version);
        if (!definition) {
            return fail(ErrorCode::UnsupportedResourceKind,
                        QStringLiteral("Unsupported resource kind/version %1 v%2")
                            .arg(kind)
                            .arg(content_schema_version));
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
        contents.push_back(ContentDescriptor{id, kind, content_schema_version, path, digest});
        resource_kinds.push_back(definition->kind);
    }

    const auto capability_coverage = CapabilityRegistry::validateCoverage(
        static_cast<std::uint32_t>(manifest_schema_version), capabilities, resource_kinds, false,
        false, false, false, false, false, false, false, false);
    if (!capability_coverage) {
        return std::unexpected(capability_coverage.error());
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
        const auto definition = kindDefinition(content.kind, content.schema_version);
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
            const auto judge = parseJudge(*object, content.path, content.schema_version);
            if (!judge) {
                return std::unexpected(judge.error());
            }
            judges.push_back(*judge);
        }
    }
    const auto content_capability_coverage = CapabilityRegistry::validateCoverage(
        static_cast<std::uint32_t>(manifest_schema_version), capabilities, resource_kinds,
        usesWorkflowPreconditions(resources), usesDependentDeadlines(resources),
        usesNamedDeadlines(resources), usesEventDateDeadlines(resources),
        usesArgumentDateGuards(resources), usesStructuredDisposition(resources),
        usesGroundedQuestions(resources), usesRealismEvidence(resources),
        usesSealedRecordTwins(resources));
    if (!content_capability_coverage) {
        return std::unexpected(content_capability_coverage.error());
    }
    std::ranges::sort(judges, {}, &model::JudgeProfile::id);
    std::ranges::sort(resources, [](const auto& left, const auto& right) {
        return left.descriptor.id < right.descriptor.id;
    });
    auto graph_state = PackGraphState::DeferredReferences;
    if (scope == PackValidationScope::Standalone || dependencies.empty()) {
        const auto graph_result = validateResourceGraph(resources, blobs);
        if (!graph_result) {
            return std::unexpected(graph_result.error());
        }
        graph_state = PackGraphState::StandaloneValidated;
    }

    LoadedPack loaded{
        static_cast<std::uint32_t>(manifest_schema_version),
        model::PackRevision{model::PackId{pack_id.toStdString()}, version.toStdString(),
                            canonicalDigest(manifest_schema_version, pack_id, version, capabilities,
                                            dependencies, contents, blobs)},
        std::move(capabilities),
        std::move(dependencies),
        std::move(resources),
        std::move(blobs),
        std::move(judges),
        graph_state,
    };
    if (graph_state == PackGraphState::StandaloneValidated) {
        std::expected<void, Error> evidence_result;
        if (authoring_review_id.has_value()) {
            auto evidence_root = loaded;
            std::erase_if(evidence_root.resources, [&](const ValidatedResource& resource) {
                return resource.descriptor.kind == model::ResourceKind::RealismReview &&
                       QString::fromStdString(resource.descriptor.id) == *authoring_review_id;
            });
            evidence_result =
                validateRealismEvidence(evidence_root, std::span<const LoadedPack* const>{});
        } else {
            evidence_result = validateRealismEvidence(loaded, std::span<const LoadedPack* const>{});
        }
        if (!evidence_result) {
            return std::unexpected(evidence_result.error());
        }
    }
    return loaded;
}

} // namespace

std::expected<LoadedPack, Error> PackReader::readDirectory(const QString& directory,
                                                           PackValidationScope scope) {
    return readDirectoryImpl(directory, scope, std::nullopt);
}

std::expected<LoadedPack, Error>
PackReader::readDirectoryForRealismAuthoring(const QString& directory,
                                             const QString& review_resource_id) {
    return readDirectoryImpl(directory, PackValidationScope::ResolvedClosure, review_resource_id);
}

std::expected<void, Error> PackReader::validateResolvedGraph(
    const LoadedPack& root, std::span<const LoadedPack* const> dependencies_dependency_first) {
    const auto validate_capabilities = [](const LoadedPack& pack) -> std::expected<void, Error> {
        std::vector<model::ResourceKind> resource_kinds;
        resource_kinds.reserve(pack.resources.size());
        QSet<QString> owned_case_ids;
        for (const auto& resource : pack.resources) {
            resource_kinds.push_back(resource.descriptor.kind);
            if (resource.descriptor.kind == model::ResourceKind::Case) {
                owned_case_ids.insert(QString::fromStdString(resource.descriptor.id));
            }
        }
        for (const auto& resource : pack.resources) {
            if (resource.descriptor.kind == model::ResourceKind::ArgumentConfig &&
                resource.document.contains(QStringLiteral("grounded_question_bank")) &&
                !owned_case_ids.contains(
                    resource.document.value(QStringLiteral("case_id")).toString())) {
                return crossReferenceFailure(
                    resource, QStringLiteral("case_id"),
                    QStringLiteral("a grounded question bank must target a case owned by the "
                                   "same exact pack"));
            }
        }
        return CapabilityRegistry::validateCoverage(
            pack.manifest_schema_version, pack.required_capabilities, resource_kinds,
            usesWorkflowPreconditions(pack.resources), usesDependentDeadlines(pack.resources),
            usesNamedDeadlines(pack.resources), usesEventDateDeadlines(pack.resources),
            usesArgumentDateGuards(pack.resources), usesStructuredDisposition(pack.resources),
            usesGroundedQuestions(pack.resources), usesRealismEvidence(pack.resources),
            usesSealedRecordTwins(pack.resources));
    };
    const auto root_capabilities = validate_capabilities(root);
    if (!root_capabilities) {
        return std::unexpected(root_capabilities.error());
    }

    std::size_t total_resources = root.resources.size();
    std::size_t total_blobs = root.blobs.size();
    if (total_resources > maximum_contents || total_blobs > maximum_contents) {
        return fail(ErrorCode::ResourceTooLarge,
                    QStringLiteral("Resolved closure exceeds the resource graph limit"));
    }
    for (const auto* dependency : dependencies_dependency_first) {
        if (dependency == nullptr ||
            dependency->resources.size() > maximum_contents - total_resources ||
            dependency->blobs.size() > maximum_contents - total_blobs) {
            return fail(ErrorCode::ResourceTooLarge,
                        QStringLiteral("Resolved closure exceeds the resource graph limit"));
        }
        const auto dependency_capabilities = validate_capabilities(*dependency);
        if (!dependency_capabilities) {
            return std::unexpected(dependency_capabilities.error());
        }
        total_resources += dependency->resources.size();
        total_blobs += dependency->blobs.size();
    }

    QHash<QString, const LoadedPack*> packs_by_id;
    for (const auto* dependency : dependencies_dependency_first) {
        packs_by_id.insert(QString::fromStdString(dependency->revision.id.value), dependency);
    }
    packs_by_id.insert(QString::fromStdString(root.revision.id.value), &root);
    const auto validate_disclosure_authorities =
        [&packs_by_id](const LoadedPack& owner) -> std::expected<void, Error> {
        QSet<QString> visible_pack_ids{QString::fromStdString(owner.revision.id.value)};
        bool changed = true;
        while (changed) {
            changed = false;
            const auto visible_snapshot = visible_pack_ids;
            for (const auto& visible_id : visible_snapshot) {
                const auto pack = packs_by_id.constFind(visible_id);
                if (pack == packs_by_id.constEnd()) {
                    return fail(ErrorCode::CrossReferenceFailure,
                                QStringLiteral("Resolved closure contains an unknown pack"));
                }
                for (const auto& dependency : (*pack)->dependencies) {
                    const auto dependency_id = QString::fromStdString(dependency.revision.id.value);
                    const auto resolved = packs_by_id.constFind(dependency_id);
                    if (resolved == packs_by_id.constEnd() ||
                        (*resolved)->revision != dependency.revision) {
                        return fail(ErrorCode::CrossReferenceFailure,
                                    QStringLiteral("Resolved closure dependency revision differs"));
                    }
                    if (!visible_pack_ids.contains(dependency_id)) {
                        visible_pack_ids.insert(dependency_id);
                        changed = true;
                    }
                }
            }
        }

        for (const auto& resource : owner.resources) {
            if (resource.descriptor.kind != model::ResourceKind::Record ||
                !resource.document.contains(QStringLiteral("sealed_disclosures"))) {
                continue;
            }
            for (const auto& disclosure_value :
                 resource.document.value(QStringLiteral("sealed_disclosures")).toArray()) {
                const auto authority_id = disclosure_value.toObject()
                                              .value(QStringLiteral("authorization_authority_id"))
                                              .toString();
                qsizetype matches = 0;
                for (const auto& visible_id : visible_pack_ids) {
                    const auto pack = *packs_by_id.constFind(visible_id);
                    for (const auto& authority_resource : pack->resources) {
                        if (authority_resource.descriptor.kind !=
                            model::ResourceKind::AuthoritySet) {
                            continue;
                        }
                        for (const auto& authority_value :
                             authority_resource.document.value(QStringLiteral("authorities"))
                                 .toArray()) {
                            const auto authority = authority_value.toObject();
                            if (authority.value(QStringLiteral("authority_id")).toString() ==
                                authority_id) {
                                ++matches;
                                if (!authority.contains(QStringLiteral("authority_type")) ||
                                    !authority.contains(QStringLiteral("source_url"))) {
                                    return crossReferenceFailure(
                                        resource,
                                        QStringLiteral("sealed_disclosures/"
                                                       "authorization_authority_id"),
                                        QStringLiteral("authorization authority lacks exact "
                                                       "canonical provenance"));
                                }
                            }
                        }
                    }
                }
                if (matches != 1) {
                    return crossReferenceFailure(
                        resource, QStringLiteral("sealed_disclosures/authorization_authority_id"),
                        QStringLiteral("authorization authority is missing, duplicated, or "
                                       "outside the owner dependency closure"));
                }
            }
        }
        return {};
    };
    for (auto pack = packs_by_id.constBegin(); pack != packs_by_id.constEnd(); ++pack) {
        const auto authorities = validate_disclosure_authorities(**pack);
        if (!authorities) {
            return std::unexpected(authorities.error());
        }
    }
    if (total_resources > maximum_contents || total_blobs > maximum_contents ||
        total_blobs > maximum_contents - total_resources) {
        return fail(ErrorCode::ResourceTooLarge,
                    QStringLiteral("Resolved closure exceeds the resource graph limit"));
    }

    std::vector<ValidatedResource> resources;
    std::vector<model::BlobDescriptor> blobs;
    resources.reserve(total_resources);
    blobs.reserve(total_blobs);
    const auto append_pack = [&](const LoadedPack& pack, std::size_t pack_index) {
        const auto prefix = QStringLiteral("closure-%1/").arg(pack_index).toStdString();
        for (const auto& source : pack.resources) {
            auto resource = source;
            if (resource.descriptor.kind == model::ResourceKind::Record) {
                auto entries = resource.document.value(QStringLiteral("docket_entries")).toArray();
                for (qsizetype index = 0; index < entries.size(); ++index) {
                    auto entry = entries.at(index).toObject();
                    entry.insert(
                        QStringLiteral("asset_path"),
                        QString::fromStdString(
                            prefix +
                            entry.value(QStringLiteral("asset_path")).toString().toStdString()));
                    entries.replace(index, entry);
                }
                resource.document.insert(QStringLiteral("docket_entries"), entries);
            }
            resources.push_back(std::move(resource));
        }
        for (const auto& source : pack.blobs) {
            auto blob = source;
            blob.path = prefix + blob.path;
            blobs.push_back(std::move(blob));
        }
    };
    std::size_t pack_index = 0;
    for (const auto* dependency : dependencies_dependency_first) {
        append_pack(*dependency, pack_index++);
    }
    append_pack(root, pack_index);
    const auto graph_result = validateResourceGraph(resources, blobs);
    if (!graph_result) {
        return std::unexpected(graph_result.error());
    }
    return validateRealismEvidence(root, dependencies_dependency_first);
}

} // namespace appellate::packs
