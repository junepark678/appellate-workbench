#include "appellate/packs/runtime_pack.hpp"

#include <QDate>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace appellate::packs {
namespace {

constexpr std::size_t maximum_resources = 10'000;
constexpr qsizetype maximum_stages = 256;
constexpr qsizetype maximum_operations = 4096;
constexpr qsizetype maximum_routes = 2048;
constexpr qsizetype maximum_route_items = 256;
constexpr qsizetype maximum_holidays = 4096;
constexpr qsizetype maximum_case_items = 4096;
constexpr qsizetype maximum_bench_seats = 32;
constexpr qsizetype maximum_authorities = 32;

template <typename Value> using Result = std::expected<Value, RuntimePackError>;

[[nodiscard]] auto fail(RuntimePackErrorCode code, std::string message)
    -> std::unexpected<RuntimePackError> {
    return std::unexpected(RuntimePackError{code, std::move(message)});
}

[[nodiscard]] std::string utf8(const QString& value) { return value.toUtf8().toStdString(); }

[[nodiscard]] bool isNamespacedId(const QString& value) {
    static const QRegularExpression pattern(
        QStringLiteral(R"(^[a-z0-9]+(?:[.-][a-z0-9]+)+(?:[-.][a-z0-9]+)*$)"));
    return value.size() >= 3 && value.size() <= 128 && pattern.match(value).hasMatch();
}

[[nodiscard]] bool isSha256(const QString& value) {
    static const QRegularExpression pattern(QStringLiteral(R"(^[a-f0-9]{64}$)"));
    return pattern.match(value).hasMatch();
}

[[nodiscard]] bool isPortablePath(const QString& value) {
    static const QRegularExpression pattern(
        QStringLiteral(R"(^[a-z0-9][a-z0-9._-]*(?:/[a-z0-9][a-z0-9._-]*)*$)"));
    return !value.isEmpty() && value.size() <= 240 && !QDir::isAbsolutePath(value) &&
           QDir::cleanPath(value) == value && pattern.match(value).hasMatch();
}

[[nodiscard]] Result<std::string> requiredString(const QJsonObject& object, const char* key,
                                                 const std::string& path) {
    const auto value = object.value(QLatin1StringView(key));
    if (!value.isString()) {
        return fail(RuntimePackErrorCode::InvalidResource, path + "." + key + " must be a string");
    }
    const auto text = value.toString();
    if (text.isEmpty()) {
        return fail(RuntimePackErrorCode::InvalidResource, path + "." + key + " must not be empty");
    }
    return utf8(text);
}

[[nodiscard]] Result<std::string> requiredId(const QJsonObject& object, const char* key,
                                             const std::string& path) {
    const auto value = object.value(QLatin1StringView(key));
    if (!value.isString() || !isNamespacedId(value.toString())) {
        return fail(RuntimePackErrorCode::InvalidResource,
                    path + "." + key + " must be a namespaced identifier");
    }
    return utf8(value.toString());
}

[[nodiscard]] Result<std::string> requiredSha256(const QJsonObject& object, const char* key,
                                                 const std::string& path) {
    const auto value = object.value(QLatin1StringView(key));
    if (!value.isString() || !isSha256(value.toString())) {
        return fail(RuntimePackErrorCode::InvalidResource,
                    path + "." + key + " must be a lowercase SHA-256 digest");
    }
    return utf8(value.toString());
}

[[nodiscard]] Result<std::string> requiredPortablePath(const QJsonObject& object, const char* key,
                                                       const std::string& path) {
    const auto value = object.value(QLatin1StringView(key));
    if (!value.isString() || !isPortablePath(value.toString())) {
        return fail(RuntimePackErrorCode::InvalidResource,
                    path + "." + key + " must be a safe portable path");
    }
    return utf8(value.toString());
}

[[nodiscard]] Result<QJsonObject> requiredObject(const QJsonObject& object, const char* key,
                                                 const std::string& path) {
    const auto value = object.value(QLatin1StringView(key));
    if (!value.isObject()) {
        return fail(RuntimePackErrorCode::InvalidResource, path + "." + key + " must be an object");
    }
    return value.toObject();
}

[[nodiscard]] Result<QJsonArray> requiredArray(const QJsonObject& object, const char* key,
                                               const std::string& path, qsizetype minimum,
                                               qsizetype maximum) {
    const auto value = object.value(QLatin1StringView(key));
    if (!value.isArray()) {
        return fail(RuntimePackErrorCode::InvalidResource, path + "." + key + " must be an array");
    }
    const auto array = value.toArray();
    if (array.size() < minimum || array.size() > maximum) {
        return fail(RuntimePackErrorCode::InvalidResource,
                    path + "." + key + " is outside its supported bounds");
    }
    return array;
}

[[nodiscard]] Result<std::uint32_t> requiredUnsigned(const QJsonObject& object, const char* key,
                                                     const std::string& path, std::uint32_t minimum,
                                                     std::uint32_t maximum) {
    const auto value = object.value(QLatin1StringView(key));
    if (!value.isDouble()) {
        return fail(RuntimePackErrorCode::InvalidResource,
                    path + "." + key + " must be an integer");
    }
    const auto number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number ||
        number < static_cast<double>(minimum) || number > static_cast<double>(maximum)) {
        return fail(RuntimePackErrorCode::InvalidResource,
                    path + "." + key + " is outside its integer bounds");
    }
    return static_cast<std::uint32_t>(number);
}

[[nodiscard]] Result<bool> requiredBoolean(const QJsonObject& object, const char* key,
                                           const std::string& path) {
    const auto value = object.value(QLatin1StringView(key));
    if (!value.isBool()) {
        return fail(RuntimePackErrorCode::InvalidResource, path + "." + key + " must be a boolean");
    }
    return value.toBool();
}

[[nodiscard]] Result<model::LegalDate> dateValue(const QJsonValue& value, const std::string& path) {
    if (!value.isString()) {
        return fail(RuntimePackErrorCode::InvalidResource, path + " must be an ISO date");
    }
    const auto text = value.toString();
    const auto date = QDate::fromString(text, Qt::ISODate);
    if (!date.isValid() || date.toString(Qt::ISODate) != text) {
        return fail(RuntimePackErrorCode::InvalidResource, path + " must be an ISO date");
    }
    return model::LegalDate{std::chrono::year{date.year()} /
                            std::chrono::month{static_cast<unsigned>(date.month())} /
                            std::chrono::day{static_cast<unsigned>(date.day())}};
}

[[nodiscard]] Result<model::LegalDate> requiredDate(const QJsonObject& object, const char* key,
                                                    const std::string& path) {
    return dateValue(object.value(QLatin1StringView(key)), path + "." + key);
}

[[nodiscard]] Result<std::optional<std::string>>
optionalId(const QJsonObject& object, const char* key, const std::string& path) {
    if (!object.contains(QLatin1StringView(key))) {
        return std::optional<std::string>{};
    }
    auto value = requiredId(object, key, path);
    if (!value) {
        return std::unexpected(value.error());
    }
    return std::optional<std::string>{std::move(*value)};
}

[[nodiscard]] Result<std::vector<std::string>> idArray(const QJsonObject& object, const char* key,
                                                       const std::string& path,
                                                       qsizetype minimum = 0,
                                                       qsizetype maximum = maximum_case_items) {
    const auto values = requiredArray(object, key, path, minimum, maximum);
    if (!values) {
        return std::unexpected(values.error());
    }
    std::vector<std::string> result;
    result.reserve(static_cast<std::size_t>(values->size()));
    std::unordered_set<std::string> seen;
    for (qsizetype index = 0; index < values->size(); ++index) {
        const auto& value = values->at(index);
        if (!value.isString() || !isNamespacedId(value.toString())) {
            return fail(RuntimePackErrorCode::InvalidResource,
                        path + "." + key + " contains an invalid identifier");
        }
        auto identifier = utf8(value.toString());
        if (!seen.emplace(identifier).second) {
            return fail(RuntimePackErrorCode::InvalidResource,
                        path + "." + key + " contains duplicate identifiers");
        }
        result.push_back(std::move(identifier));
    }
    return result;
}

[[nodiscard]] Result<model::CourtRole> courtRole(const QJsonObject& object, const char* key,
                                                 const std::string& path) {
    const auto value = requiredString(object, key, path);
    if (!value) {
        return std::unexpected(value.error());
    }
    if (*value == "district") {
        return model::CourtRole::District;
    }
    if (*value == "appellate") {
        return model::CourtRole::Appellate;
    }
    return fail(RuntimePackErrorCode::InvalidResource,
                path + "." + key + " has an unsupported court role");
}

[[nodiscard]] const char* kindName(model::ResourceKind kind) {
    switch (kind) {
    case model::ResourceKind::ArgumentConfig:
        return "argument_config";
    case model::ResourceKind::AuthoritySet:
        return "authority_set";
    case model::ResourceKind::BenchConfiguration:
        return "bench_configuration";
    case model::ResourceKind::Case:
        return "case";
    case model::ResourceKind::Court:
        return "court";
    case model::ResourceKind::FilingCatalog:
        return "filing_catalog";
    case model::ResourceKind::Form:
        return "form";
    case model::ResourceKind::JudgeProfile:
        return "judge_profile";
    case model::ResourceKind::ProcedureProfile:
        return "procedure_profile";
    case model::ResourceKind::RealismReview:
        return "realism_review";
    case model::ResourceKind::Record:
        return "record";
    case model::ResourceKind::Workflow:
        return "workflow";
    }
    return "";
}

struct ResourceIndex final {
    std::unordered_map<std::string, const ValidatedResource*> resources;
    std::unordered_map<std::string, const model::JudgeProfile*> judge_profiles;

    [[nodiscard]] Result<const ValidatedResource*>
    require(const std::string& id, model::ResourceKind kind, const std::string& owner) const {
        const auto found = resources.find(id);
        if (found == resources.end()) {
            return fail(RuntimePackErrorCode::MissingResource,
                        owner + " references missing resource " + id);
        }
        if (found->second->descriptor.kind != kind) {
            return fail(RuntimePackErrorCode::WrongResourceKind,
                        owner + " references " + id + " with the wrong resource kind");
        }
        return found->second;
    }

    [[nodiscard]] Result<const model::JudgeProfile*> requireJudge(const std::string& id,
                                                                  const std::string& owner) const {
        const auto resource = require(id, model::ResourceKind::JudgeProfile, owner);
        if (!resource) {
            return std::unexpected(resource.error());
        }
        const auto found = judge_profiles.find(id);
        if (found == judge_profiles.end()) {
            return fail(RuntimePackErrorCode::MissingResource,
                        owner + " references judge profile without typed data " + id);
        }
        return found->second;
    }
};

[[nodiscard]] Result<ResourceIndex> makeIndex(const LoadedPack& pack) {
    if (pack.resources.empty() || pack.resources.size() > maximum_resources ||
        !isNamespacedId(QString::fromStdString(pack.revision.id.value)) ||
        pack.revision.version.empty() || !isSha256(QString::fromStdString(pack.revision.digest))) {
        return fail(RuntimePackErrorCode::InvalidPack,
                    "runtime pack has invalid revision metadata or resource bounds");
    }

    ResourceIndex index;
    index.resources.reserve(pack.resources.size());
    for (const auto& resource : pack.resources) {
        const auto path = "resource " + resource.descriptor.id;
        const auto document_id = requiredId(resource.document, "resource_id", path);
        const auto document_kind = requiredString(resource.document, "resource_kind", path);
        const auto schema_version =
            requiredUnsigned(resource.document, "schema_version", path, 1, 1);
        if (!document_id || !document_kind || !schema_version) {
            if (!document_id) {
                return std::unexpected(document_id.error());
            }
            if (!document_kind) {
                return std::unexpected(document_kind.error());
            }
            return std::unexpected(schema_version.error());
        }
        if (resource.descriptor.schema_version != 1 || *schema_version != 1 ||
            resource.descriptor.id != *document_id ||
            *document_kind != kindName(resource.descriptor.kind)) {
            return fail(RuntimePackErrorCode::InvalidResource,
                        path + " does not match its validated descriptor");
        }
        if (!index.resources.emplace(resource.descriptor.id, &resource).second) {
            return fail(RuntimePackErrorCode::DuplicateResource,
                        "runtime pack contains duplicate resource id " + resource.descriptor.id);
        }
    }

    index.judge_profiles.reserve(pack.judge_profiles.size());
    for (const auto& profile : pack.judge_profiles) {
        if (!isNamespacedId(QString::fromStdString(profile.id)) ||
            !index.judge_profiles.emplace(profile.id, &profile).second) {
            return fail(RuntimePackErrorCode::DuplicateResource,
                        "runtime pack contains invalid or duplicate typed judge profile " +
                            profile.id);
        }
        const auto resource = index.require(profile.id, model::ResourceKind::JudgeProfile,
                                            "typed judge profile " + profile.id);
        if (!resource) {
            return std::unexpected(resource.error());
        }
    }
    for (const auto& resource : pack.resources) {
        if (resource.descriptor.kind == model::ResourceKind::JudgeProfile &&
            !index.judge_profiles.contains(resource.descriptor.id)) {
            return fail(RuntimePackErrorCode::MissingResource,
                        "judge resource has no typed profile " + resource.descriptor.id);
        }
    }
    return index;
}

[[nodiscard]] Result<model::AuthorityRef> parseAuthorityReference(const QJsonObject& object,
                                                                  const std::string& path) {
    const auto id = requiredId(object, "authority_id", path);
    const auto citation = requiredString(object, "citation", path);
    const auto source_version = requiredDate(object, "source_version", path);
    const auto proposition = requiredString(object, "proposition", path);
    if (!id || !citation || !source_version || !proposition) {
        if (!id) {
            return std::unexpected(id.error());
        }
        if (!citation) {
            return std::unexpected(citation.error());
        }
        if (!source_version) {
            return std::unexpected(source_version.error());
        }
        return std::unexpected(proposition.error());
    }
    const auto source_text = object.value(QStringLiteral("source_version")).toString();
    return model::AuthorityRef{model::AuthorityId{*id}, *citation, utf8(source_text), *proposition};
}

[[nodiscard]] Result<model::AuthorityBasis> parseAuthorityBasis(const QJsonObject& object,
                                                                const std::string& path) {
    const auto primary_object = requiredObject(object, "primary", path);
    const auto supporting_values =
        requiredArray(object, "supporting", path, 0, maximum_authorities);
    if (!primary_object) {
        return std::unexpected(primary_object.error());
    }
    if (!supporting_values) {
        return std::unexpected(supporting_values.error());
    }
    auto primary = parseAuthorityReference(*primary_object, path + ".primary");
    if (!primary) {
        return std::unexpected(primary.error());
    }
    std::vector<model::AuthorityRef> supporting;
    supporting.reserve(static_cast<std::size_t>(supporting_values->size()));
    std::unordered_set<std::string> seen{primary->id.value};
    for (qsizetype index = 0; index < supporting_values->size(); ++index) {
        if (!supporting_values->at(index).isObject()) {
            return fail(RuntimePackErrorCode::InvalidResource,
                        path + ".supporting must contain objects");
        }
        auto reference =
            parseAuthorityReference(supporting_values->at(index).toObject(), path + ".supporting");
        if (!reference) {
            return std::unexpected(reference.error());
        }
        if (!seen.emplace(reference->id.value).second) {
            return fail(RuntimePackErrorCode::InvalidResource,
                        path + " repeats an authority identifier");
        }
        supporting.push_back(std::move(*reference));
    }
    return model::AuthorityBasis{std::move(*primary), std::move(supporting)};
}

[[nodiscard]] Result<model::WorkflowOpcode> workflowOpcode(const QJsonObject& object,
                                                           const std::string& path) {
    const auto value = requiredString(object, "opcode", path);
    if (!value) {
        return std::unexpected(value.error());
    }
    static const std::unordered_map<std::string, model::WorkflowOpcode> opcodes{
        {"accept_filing", model::WorkflowOpcode::AcceptFiling},
        {"reject_filing", model::WorkflowOpcode::RejectFiling},
        {"issue_deficiency", model::WorkflowOpcode::IssueDeficiency},
        {"calculate_deadline", model::WorkflowOpcode::CalculateDeadline},
        {"enter_order", model::WorkflowOpcode::EnterOrder},
        {"advance_stage", model::WorkflowOpcode::AdvanceStage},
        {"set_sealed", model::WorkflowOpcode::SetSealed},
        {"schedule_argument", model::WorkflowOpcode::ScheduleArgument},
        {"issue_judgment", model::WorkflowOpcode::IssueJudgment},
        {"issue_mandate", model::WorkflowOpcode::IssueMandate},
    };
    const auto found = opcodes.find(*value);
    if (found == opcodes.end()) {
        return fail(RuntimePackErrorCode::InvalidResource,
                    path + ".opcode has an unsupported value");
    }
    return found->second;
}

[[nodiscard]] Result<std::optional<std::uint32_t>> optionalDeadlineDays(const QJsonObject& object,
                                                                        const std::string& path) {
    if (!object.contains(QStringLiteral("deadline_days"))) {
        return std::optional<std::uint32_t>{};
    }
    auto value = requiredUnsigned(object, "deadline_days", path, 0, 3650);
    if (!value) {
        return std::unexpected(value.error());
    }
    return std::optional<std::uint32_t>{*value};
}

[[nodiscard]] Result<std::optional<model::DeadlineCounting>>
optionalDeadlineCounting(const QJsonObject& object, const std::string& path) {
    if (!object.contains(QStringLiteral("deadline_counting"))) {
        return std::optional<model::DeadlineCounting>{};
    }
    const auto value = requiredString(object, "deadline_counting", path);
    if (!value) {
        return std::unexpected(value.error());
    }
    if (*value == "calendar_days") {
        return std::optional{model::DeadlineCounting::CalendarDays};
    }
    if (*value == "business_days") {
        return std::optional{model::DeadlineCounting::BusinessDays};
    }
    return fail(RuntimePackErrorCode::InvalidResource,
                path + ".deadline_counting has an unsupported value");
}

[[nodiscard]] Result<model::WorkflowDeadlinePlan> parseDeadlinePlan(const QJsonObject& object,
                                                                    const std::string& path) {
    const auto deadline_id = requiredId(object, "deadline_id", path);
    const auto operation_id = requiredId(object, "operation_id", path);
    if (!deadline_id) {
        return std::unexpected(deadline_id.error());
    }
    if (!operation_id) {
        return std::unexpected(operation_id.error());
    }
    return model::WorkflowDeadlinePlan{model::WorkflowDeadlineId{*deadline_id},
                                       model::WorkflowOperationId{*operation_id}};
}

[[nodiscard]] Result<model::WorkflowDefinition> parseWorkflow(const ValidatedResource& resource) {
    const auto path = "workflow " + resource.descriptor.id;
    const auto initial_stage = requiredId(resource.document, "initial_stage_id", path);
    const auto stage_ids = idArray(resource.document, "stages", path, 1, maximum_stages);
    const auto operation_values =
        requiredArray(resource.document, "operations", path, 1, maximum_operations);
    const auto route_values =
        requiredArray(resource.document, "filing_routes", path, 1, maximum_routes);
    const auto calendar_object = requiredObject(resource.document, "calendar", path);
    if (!initial_stage || !stage_ids || !operation_values || !route_values || !calendar_object) {
        if (!initial_stage) {
            return std::unexpected(initial_stage.error());
        }
        if (!stage_ids) {
            return std::unexpected(stage_ids.error());
        }
        if (!operation_values) {
            return std::unexpected(operation_values.error());
        }
        if (!route_values) {
            return std::unexpected(route_values.error());
        }
        return std::unexpected(calendar_object.error());
    }

    std::vector<model::WorkflowStageId> stages;
    stages.reserve(stage_ids->size());
    for (auto& id : *stage_ids) {
        stages.push_back(model::WorkflowStageId{std::move(id)});
    }

    std::vector<model::WorkflowOperation> operations;
    operations.reserve(static_cast<std::size_t>(operation_values->size()));
    for (qsizetype index = 0; index < operation_values->size(); ++index) {
        if (!operation_values->at(index).isObject()) {
            return fail(RuntimePackErrorCode::InvalidResource,
                        path + ".operations must contain objects");
        }
        const auto operation = operation_values->at(index).toObject();
        const auto operation_path = path + ".operations[" + std::to_string(index) + "]";
        const auto id = requiredId(operation, "operation_id", operation_path);
        const auto stage = requiredId(operation, "stage_id", operation_path);
        const auto opcode = workflowOpcode(operation, operation_path);
        const auto authority_object = requiredObject(operation, "authority", operation_path);
        const auto next_stage = optionalId(operation, "next_stage_id", operation_path);
        const auto days = optionalDeadlineDays(operation, operation_path);
        const auto counting = optionalDeadlineCounting(operation, operation_path);
        const auto roles = idArray(operation, "authorized_role_ids", operation_path, 0, 64);
        if (!id || !stage || !opcode || !authority_object || !next_stage || !days || !counting ||
            !roles) {
            if (!id) {
                return std::unexpected(id.error());
            }
            if (!stage) {
                return std::unexpected(stage.error());
            }
            if (!opcode) {
                return std::unexpected(opcode.error());
            }
            if (!authority_object) {
                return std::unexpected(authority_object.error());
            }
            if (!next_stage) {
                return std::unexpected(next_stage.error());
            }
            if (!days) {
                return std::unexpected(days.error());
            }
            if (!counting) {
                return std::unexpected(counting.error());
            }
            return std::unexpected(roles.error());
        }
        auto authority = parseAuthorityBasis(*authority_object, operation_path + ".authority");
        if (!authority) {
            return std::unexpected(authority.error());
        }
        if (days->has_value() != counting->has_value()) {
            return fail(RuntimePackErrorCode::InvalidResource,
                        operation_path + " has an incomplete deadline rule");
        }
        std::vector<model::ActorRoleId> authorized_roles;
        authorized_roles.reserve(roles->size());
        for (auto& role : *roles) {
            authorized_roles.push_back(model::ActorRoleId{std::move(role)});
        }
        std::optional<model::WorkflowStageId> next;
        if (next_stage->has_value()) {
            next = model::WorkflowStageId{std::move(**next_stage)};
        }
        operations.push_back(model::WorkflowOperation{
            model::WorkflowOperationId{*id}, model::WorkflowStageId{*stage}, *opcode,
            std::move(*authority), std::move(next), *days, *counting, std::move(authorized_roles)});
    }

    std::vector<model::WorkflowFilingRoute> routes;
    routes.reserve(static_cast<std::size_t>(route_values->size()));
    for (qsizetype index = 0; index < route_values->size(); ++index) {
        if (!route_values->at(index).isObject()) {
            return fail(RuntimePackErrorCode::InvalidResource,
                        path + ".filing_routes must contain objects");
        }
        const auto route = route_values->at(index).toObject();
        const auto route_path = path + ".filing_routes[" + std::to_string(index) + "]";
        const auto filing_type = requiredId(route, "filing_type_id", route_path);
        const auto stage = requiredId(route, "stage_id", route_path);
        const auto authorized = idArray(route, "authorized_role_ids", route_path, 1, 64);
        const auto fields =
            idArray(route, "required_field_ids", route_path, 0, maximum_route_items);
        const auto service = idArray(route, "required_service_role_ids", route_path, 0, 64);
        const auto accept = requiredId(route, "accept_operation_id", route_path);
        const auto reject = requiredId(route, "reject_operation_id", route_path);
        const auto deficiency = optionalId(route, "deficiency_operation_id", route_path);
        const auto advance = optionalId(route, "advance_operation_id", route_path);
        const auto satisfies = optionalId(route, "satisfies_deadline_id", route_path);
        const auto reject_after_deadline =
            requiredBoolean(route, "reject_after_deadline", route_path);
        if (!filing_type || !stage || !authorized || !fields || !service || !accept || !reject ||
            !deficiency || !advance || !satisfies || !reject_after_deadline) {
            if (!filing_type) {
                return std::unexpected(filing_type.error());
            }
            if (!stage) {
                return std::unexpected(stage.error());
            }
            if (!authorized) {
                return std::unexpected(authorized.error());
            }
            if (!fields) {
                return std::unexpected(fields.error());
            }
            if (!service) {
                return std::unexpected(service.error());
            }
            if (!accept) {
                return std::unexpected(accept.error());
            }
            if (!reject) {
                return std::unexpected(reject.error());
            }
            if (!deficiency) {
                return std::unexpected(deficiency.error());
            }
            if (!advance) {
                return std::unexpected(advance.error());
            }
            if (!satisfies) {
                return std::unexpected(satisfies.error());
            }
            return std::unexpected(reject_after_deadline.error());
        }
        std::optional<model::WorkflowDeadlinePlan> deficiency_plan;
        if (route.contains(QStringLiteral("deficiency_deadline"))) {
            if (!deficiency->has_value()) {
                return fail(RuntimePackErrorCode::InvalidResource,
                            route_path + ".deficiency_deadline requires deficiency_operation_id");
            }
            const auto deficiency_plan_object =
                requiredObject(route, "deficiency_deadline", route_path);
            if (!deficiency_plan_object) {
                return std::unexpected(deficiency_plan_object.error());
            }
            auto parsed =
                parseDeadlinePlan(*deficiency_plan_object, route_path + ".deficiency_deadline");
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            deficiency_plan = std::move(*parsed);
        }
        std::optional<model::WorkflowDeadlinePlan> accepted_plan;
        if (route.contains(QStringLiteral("accepted_deadline"))) {
            const auto accepted_object = requiredObject(route, "accepted_deadline", route_path);
            if (!accepted_object) {
                return std::unexpected(accepted_object.error());
            }
            auto parsed = parseDeadlinePlan(*accepted_object, route_path + ".accepted_deadline");
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            accepted_plan = std::move(*parsed);
        }
        std::vector<model::ActorRoleId> authorized_roles;
        for (auto& role : *authorized) {
            authorized_roles.push_back(model::ActorRoleId{std::move(role)});
        }
        std::vector<model::FilingFieldId> required_fields;
        for (auto& field : *fields) {
            required_fields.push_back(model::FilingFieldId{std::move(field)});
        }
        std::vector<model::ActorRoleId> service_roles;
        for (auto& role : *service) {
            service_roles.push_back(model::ActorRoleId{std::move(role)});
        }
        std::optional<model::WorkflowOperationId> advance_id;
        if (advance->has_value()) {
            advance_id = model::WorkflowOperationId{std::move(**advance)};
        }
        std::optional<model::WorkflowDeadlineId> satisfies_id;
        if (satisfies->has_value()) {
            satisfies_id = model::WorkflowDeadlineId{std::move(**satisfies)};
        }
        std::optional<model::WorkflowOperationId> deficiency_id;
        if (deficiency->has_value()) {
            deficiency_id = model::WorkflowOperationId{std::move(**deficiency)};
        }
        routes.push_back(model::WorkflowFilingRoute{
            model::FilingTypeId{*filing_type}, model::WorkflowStageId{*stage},
            std::move(authorized_roles), std::move(required_fields), std::move(service_roles),
            model::WorkflowOperationId{*accept}, model::WorkflowOperationId{*reject},
            std::move(deficiency_id), std::move(deficiency_plan), std::move(accepted_plan),
            std::move(advance_id), std::move(satisfies_id), *reject_after_deadline});
    }

    const auto holiday_values =
        requiredArray(*calendar_object, "holidays", path + ".calendar", 0, maximum_holidays);
    if (!holiday_values) {
        return std::unexpected(holiday_values.error());
    }
    std::vector<model::LegalDate> holidays;
    holidays.reserve(static_cast<std::size_t>(holiday_values->size()));
    for (qsizetype index = 0; index < holiday_values->size(); ++index) {
        auto holiday = dateValue(holiday_values->at(index), path + ".calendar.holidays");
        if (!holiday) {
            return std::unexpected(holiday.error());
        }
        holidays.push_back(*holiday);
    }
    model::WorkflowDefinition definition{model::WorkflowId{resource.descriptor.id},
                                         model::WorkflowStageId{*initial_stage},
                                         std::move(stages),
                                         std::move(operations),
                                         std::move(routes),
                                         model::CourtCalendar{std::move(holidays)}};

    std::unordered_set<std::string> declared_stages;
    declared_stages.reserve(definition.stages.size());
    for (const auto& declared_stage : definition.stages) {
        declared_stages.insert(declared_stage.value);
    }
    if (!declared_stages.contains(definition.initial_stage_id.value)) {
        return fail(RuntimePackErrorCode::CrossReferenceFailure,
                    path + " initial stage is not declared");
    }

    std::unordered_map<std::string, const model::WorkflowOperation*> operations_by_id;
    operations_by_id.reserve(definition.operations.size());
    for (const auto& operation : definition.operations) {
        if (!operations_by_id.emplace(operation.id.value, &operation).second ||
            !declared_stages.contains(operation.stage_id.value) ||
            (operation.next_stage_id.has_value() &&
             !declared_stages.contains(operation.next_stage_id->value))) {
            return fail(RuntimePackErrorCode::CrossReferenceFailure,
                        path + " has duplicate operations or undeclared stages");
        }
        const auto has_deadline = operation.deadline_days.has_value();
        const auto can_have_deadline =
            operation.opcode == model::WorkflowOpcode::CalculateDeadline ||
            operation.opcode == model::WorkflowOpcode::EnterOrder;
        if (has_deadline != operation.deadline_counting.has_value() ||
            (operation.opcode == model::WorkflowOpcode::CalculateDeadline && !has_deadline) ||
            (!can_have_deadline && has_deadline) ||
            (operation.opcode == model::WorkflowOpcode::AdvanceStage &&
             !operation.next_stage_id.has_value())) {
            return fail(RuntimePackErrorCode::CrossReferenceFailure,
                        path + " has an incompatible workflow operation");
        }
    }
    const auto operationForRoute = [&operations_by_id](const model::WorkflowOperationId& id,
                                                       model::WorkflowOpcode opcode,
                                                       const model::WorkflowStageId& stage) {
        const auto found = operations_by_id.find(id.value);
        return found != operations_by_id.end() && found->second->opcode == opcode &&
               found->second->stage_id == stage;
    };

    std::unordered_set<std::string> route_keys;
    std::unordered_set<std::string> deadline_ids;
    std::unordered_set<std::string> accepted_deadline_ids;
    for (const auto& route : definition.filing_routes) {
        if (!route_keys.emplace(route.stage_id.value + "\n" + route.filing_type.value).second ||
            !declared_stages.contains(route.stage_id.value) ||
            !operationForRoute(route.accept_operation_id, model::WorkflowOpcode::AcceptFiling,
                               route.stage_id) ||
            !operationForRoute(route.reject_operation_id, model::WorkflowOpcode::RejectFiling,
                               route.stage_id)) {
            return fail(RuntimePackErrorCode::CrossReferenceFailure,
                        path + " has a duplicate or incompatible filing route");
        }
        if (route.deficiency_operation_id.has_value() &&
            !operationForRoute(*route.deficiency_operation_id,
                               model::WorkflowOpcode::IssueDeficiency, route.stage_id)) {
            return fail(RuntimePackErrorCode::CrossReferenceFailure,
                        path + " has an incompatible deficiency operation");
        }
        if (route.deficiency_deadline.has_value()) {
            if (!route.deficiency_operation_id.has_value() ||
                !deadline_ids.emplace(route.deficiency_deadline->deadline_id.value).second ||
                !operationForRoute(route.deficiency_deadline->operation_id,
                                   model::WorkflowOpcode::CalculateDeadline, route.stage_id)) {
                return fail(RuntimePackErrorCode::CrossReferenceFailure,
                            path + " has an incompatible deficiency deadline");
            }
        }
        if (route.accepted_deadline.has_value()) {
            if (!deadline_ids.emplace(route.accepted_deadline->deadline_id.value).second ||
                !accepted_deadline_ids.emplace(route.accepted_deadline->deadline_id.value).second ||
                !operationForRoute(route.accepted_deadline->operation_id,
                                   model::WorkflowOpcode::CalculateDeadline, route.stage_id)) {
                return fail(RuntimePackErrorCode::CrossReferenceFailure,
                            path + " has an incompatible accepted deadline");
            }
        }
        if (route.advance_operation_id.has_value() &&
            !operationForRoute(*route.advance_operation_id, model::WorkflowOpcode::AdvanceStage,
                               route.stage_id)) {
            return fail(RuntimePackErrorCode::CrossReferenceFailure,
                        path + " has an incompatible advance operation");
        }
    }
    for (const auto& route : definition.filing_routes) {
        if (route.satisfies_deadline_id.has_value() &&
            !accepted_deadline_ids.contains(route.satisfies_deadline_id->value)) {
            return fail(RuntimePackErrorCode::CrossReferenceFailure,
                        path + " satisfies a deadline that this workflow does not produce");
        }
    }
    return definition;
}

struct ParsedCase final {
    model::CaseDefinition definition;
    std::string title;
    RuntimeRecordId record_id;
    std::vector<RuntimeIssue> issues;
    model::WorkflowOperationId authored_disposition_id;
};

[[nodiscard]] Result<ParsedCase> parseCase(const ValidatedResource& resource) {
    const auto path = "case " + resource.descriptor.id;
    const auto title = requiredString(resource.document, "title", path);
    const auto procedure = requiredId(resource.document, "procedure_profile_id", path);
    const auto record = requiredId(resource.document, "record_id", path);
    const auto actor_values =
        requiredArray(resource.document, "actors", path, 2, maximum_case_items);
    const auto issue_values =
        requiredArray(resource.document, "issues", path, 1, maximum_case_items);
    const auto disposition = requiredId(resource.document, "authored_disposition_id", path);
    if (!title || !procedure || !record || !actor_values || !issue_values || !disposition) {
        if (!title) {
            return std::unexpected(title.error());
        }
        if (!procedure) {
            return std::unexpected(procedure.error());
        }
        if (!record) {
            return std::unexpected(record.error());
        }
        if (!actor_values) {
            return std::unexpected(actor_values.error());
        }
        if (!issue_values) {
            return std::unexpected(issue_values.error());
        }
        return std::unexpected(disposition.error());
    }
    std::vector<model::CaseActor> actors;
    actors.reserve(static_cast<std::size_t>(actor_values->size()));
    std::unordered_set<std::string> actor_ids;
    for (qsizetype index = 0; index < actor_values->size(); ++index) {
        if (!actor_values->at(index).isObject()) {
            return fail(RuntimePackErrorCode::InvalidResource,
                        path + ".actors must contain objects");
        }
        const auto actor = actor_values->at(index).toObject();
        const auto actor_path = path + ".actors[" + std::to_string(index) + "]";
        const auto id = requiredId(actor, "actor_id", actor_path);
        const auto role = requiredId(actor, "role_id", actor_path);
        const auto display_name = requiredString(actor, "display_name", actor_path);
        const auto synthetic = requiredBoolean(actor, "synthetic", actor_path);
        if (!id || !role || !display_name || !synthetic) {
            if (!id) {
                return std::unexpected(id.error());
            }
            if (!role) {
                return std::unexpected(role.error());
            }
            if (!display_name) {
                return std::unexpected(display_name.error());
            }
            return std::unexpected(synthetic.error());
        }
        if (!*synthetic || !actor_ids.emplace(*id).second) {
            return fail(RuntimePackErrorCode::InvalidResource,
                        actor_path + " must be synthetic and uniquely identified");
        }
        actors.push_back(model::CaseActor{model::ActorId{*id}, model::ActorRoleId{*role}});
    }

    std::vector<RuntimeIssue> issues;
    issues.reserve(static_cast<std::size_t>(issue_values->size()));
    std::unordered_set<std::string> issue_ids;
    for (qsizetype index = 0; index < issue_values->size(); ++index) {
        if (!issue_values->at(index).isObject()) {
            return fail(RuntimePackErrorCode::InvalidResource,
                        path + ".issues must contain objects");
        }
        const auto issue = issue_values->at(index).toObject();
        const auto issue_path = path + ".issues[" + std::to_string(index) + "]";
        const auto id = requiredId(issue, "issue_id", issue_path);
        const auto issue_title = requiredString(issue, "title", issue_path);
        const auto authorities = idArray(issue, "authority_ids", issue_path, 1, maximum_case_items);
        const auto anchors = idArray(issue, "record_anchor_ids", issue_path, 1, maximum_case_items);
        if (!id || !issue_title || !authorities || !anchors) {
            if (!id) {
                return std::unexpected(id.error());
            }
            if (!issue_title) {
                return std::unexpected(issue_title.error());
            }
            if (!authorities) {
                return std::unexpected(authorities.error());
            }
            return std::unexpected(anchors.error());
        }
        if (!issue_ids.emplace(*id).second) {
            return fail(RuntimePackErrorCode::InvalidResource,
                        issue_path + " repeats an issue identifier");
        }
        std::vector<model::AuthorityId> authority_ids;
        for (auto& authority : *authorities) {
            authority_ids.push_back(model::AuthorityId{std::move(authority)});
        }
        std::vector<RuntimeRecordEntryId> anchor_ids;
        for (auto& anchor : *anchors) {
            anchor_ids.push_back(RuntimeRecordEntryId{std::move(anchor)});
        }
        issues.push_back(RuntimeIssue{RuntimeIssueId{*id}, *issue_title, std::move(authority_ids),
                                      std::move(anchor_ids)});
    }
    return ParsedCase{model::CaseDefinition{model::CaseId{resource.descriptor.id},
                                            model::ProcedureId{*procedure}, std::move(actors)},
                      *title, RuntimeRecordId{*record}, std::move(issues),
                      model::WorkflowOperationId{*disposition}};
}

[[nodiscard]] Result<RuntimeRecord> parseRecord(const ValidatedResource& resource) {
    const auto path = "record " + resource.descriptor.id;
    const auto caption = requiredString(resource.document, "caption", path);
    const auto entries =
        requiredArray(resource.document, "docket_entries", path, 1, maximum_case_items);
    if (!caption) {
        return std::unexpected(caption.error());
    }
    if (!entries) {
        return std::unexpected(entries.error());
    }
    std::vector<RuntimeDocketEntry> docket;
    docket.reserve(static_cast<std::size_t>(entries->size()));
    std::unordered_set<std::string> ids;
    std::unordered_set<std::uint32_t> numbers;
    for (qsizetype index = 0; index < entries->size(); ++index) {
        if (!entries->at(index).isObject()) {
            return fail(RuntimePackErrorCode::InvalidResource,
                        path + ".docket_entries must contain objects");
        }
        const auto entry = entries->at(index).toObject();
        const auto entry_path = path + ".docket_entries[" + std::to_string(index) + "]";
        const auto id = requiredId(entry, "entry_id", entry_path);
        const auto number = requiredUnsigned(entry, "entry_number", entry_path, 1,
                                             std::numeric_limits<std::uint32_t>::max());
        const auto filed_on = requiredDate(entry, "filed_on", entry_path);
        const auto title = requiredString(entry, "title", entry_path);
        const auto asset_path = requiredPortablePath(entry, "asset_path", entry_path);
        const auto digest = requiredSha256(entry, "asset_sha256", entry_path);
        const auto pages = requiredUnsigned(entry, "page_count", entry_path, 1, 10'000);
        const auto sealed = requiredBoolean(entry, "sealed", entry_path);
        if (!id || !number || !filed_on || !title || !asset_path || !digest || !pages || !sealed) {
            if (!id) {
                return std::unexpected(id.error());
            }
            if (!number) {
                return std::unexpected(number.error());
            }
            if (!filed_on) {
                return std::unexpected(filed_on.error());
            }
            if (!title) {
                return std::unexpected(title.error());
            }
            if (!asset_path) {
                return std::unexpected(asset_path.error());
            }
            if (!digest) {
                return std::unexpected(digest.error());
            }
            if (!pages) {
                return std::unexpected(pages.error());
            }
            return std::unexpected(sealed.error());
        }
        if (!ids.emplace(*id).second || !numbers.emplace(*number).second) {
            return fail(RuntimePackErrorCode::InvalidResource,
                        entry_path + " repeats a docket id or number");
        }
        docket.push_back(RuntimeDocketEntry{RuntimeRecordEntryId{*id}, *number, *filed_on, *title,
                                            *asset_path, *digest, *pages, *sealed});
    }
    return RuntimeRecord{RuntimeRecordId{resource.descriptor.id}, *caption, std::move(docket)};
}

struct RuntimeCatalogFiling final {
    std::unordered_set<std::string> authorized_roles;
    std::unordered_set<std::string> required_fields;
};

using RuntimeCatalog = std::unordered_map<std::string, RuntimeCatalogFiling>;

[[nodiscard]] Result<RuntimeCatalog> parseFilingCatalog(const ValidatedResource& resource) {
    const auto path = "filing catalog " + resource.descriptor.id;
    const auto filing_values = requiredArray(resource.document, "filings", path, 1, 512);
    if (!filing_values) {
        return std::unexpected(filing_values.error());
    }
    RuntimeCatalog catalog;
    catalog.reserve(static_cast<std::size_t>(filing_values->size()));
    for (qsizetype index = 0; index < filing_values->size(); ++index) {
        if (!filing_values->at(index).isObject()) {
            return fail(RuntimePackErrorCode::InvalidResource,
                        path + ".filings must contain objects");
        }
        const auto filing = filing_values->at(index).toObject();
        const auto filing_path = path + ".filings[" + std::to_string(index) + "]";
        const auto id = requiredId(filing, "filing_id", filing_path);
        const auto title = requiredString(filing, "title", filing_path);
        const auto roles = idArray(filing, "actor_role_ids", filing_path, 1, maximum_case_items);
        const auto fields =
            idArray(filing, "required_field_ids", filing_path, 0, maximum_route_items);
        const auto authority = requiredId(filing, "authority_id", filing_path);
        if (!id || !title || !roles || !fields || !authority) {
            if (!id) {
                return std::unexpected(id.error());
            }
            if (!title) {
                return std::unexpected(title.error());
            }
            if (!roles) {
                return std::unexpected(roles.error());
            }
            if (!fields) {
                return std::unexpected(fields.error());
            }
            return std::unexpected(authority.error());
        }
        RuntimeCatalogFiling parsed;
        parsed.authorized_roles.insert(roles->begin(), roles->end());
        parsed.required_fields.insert(fields->begin(), fields->end());
        if (!catalog.emplace(*id, std::move(parsed)).second) {
            return fail(RuntimePackErrorCode::DuplicateResource,
                        path + " repeats filing type " + *id);
        }
    }
    return catalog;
}

[[nodiscard]] Result<RuntimeProcedure> parseProcedure(const ValidatedResource& resource) {
    const auto path = "procedure " + resource.descriptor.id;
    const auto court = requiredId(resource.document, "court_id", path);
    const auto proceeding = requiredString(resource.document, "proceeding_type", path);
    const auto roles = idArray(resource.document, "actor_roles", path, 2, maximum_case_items);
    const auto catalog = requiredId(resource.document, "filing_catalog_id", path);
    const auto workflow = requiredId(resource.document, "workflow_id", path);
    const auto authority_sets =
        idArray(resource.document, "authority_set_ids", path, 1, maximum_case_items);
    if (!court || !proceeding || !roles || !catalog || !workflow || !authority_sets) {
        if (!court) {
            return std::unexpected(court.error());
        }
        if (!proceeding) {
            return std::unexpected(proceeding.error());
        }
        if (!roles) {
            return std::unexpected(roles.error());
        }
        if (!catalog) {
            return std::unexpected(catalog.error());
        }
        if (!workflow) {
            return std::unexpected(workflow.error());
        }
        return std::unexpected(authority_sets.error());
    }
    RuntimeProceedingType proceeding_type{};
    if (*proceeding == "civil_appeal") {
        proceeding_type = RuntimeProceedingType::CivilAppeal;
    } else if (*proceeding == "criminal_appeal") {
        proceeding_type = RuntimeProceedingType::CriminalAppeal;
    } else if (*proceeding == "agency_review") {
        proceeding_type = RuntimeProceedingType::AgencyReview;
    } else if (*proceeding == "original_writ") {
        proceeding_type = RuntimeProceedingType::OriginalWrit;
    } else {
        return fail(RuntimePackErrorCode::InvalidResource,
                    path + ".proceeding_type has an unsupported value");
    }
    std::vector<model::ActorRoleId> actor_roles;
    for (auto& role : *roles) {
        actor_roles.push_back(model::ActorRoleId{std::move(role)});
    }
    std::vector<RuntimeAuthoritySetId> authority_ids;
    for (auto& authority : *authority_sets) {
        authority_ids.push_back(RuntimeAuthoritySetId{std::move(authority)});
    }
    return RuntimeProcedure{model::ProcedureId{resource.descriptor.id},
                            RuntimeCourtId{*court},
                            proceeding_type,
                            std::move(actor_roles),
                            RuntimeFilingCatalogId{*catalog},
                            model::WorkflowId{*workflow},
                            std::move(authority_ids)};
}

[[nodiscard]] Result<RuntimeCourt> parseCourt(const ValidatedResource& resource) {
    const auto path = "court " + resource.descriptor.id;
    const auto jurisdiction = requiredId(resource.document, "jurisdiction_id", path);
    const auto name = requiredString(resource.document, "name", path);
    const auto role = courtRole(resource.document, "court_role", path);
    const auto authorities =
        idArray(resource.document, "authority_set_ids", path, 1, maximum_case_items);
    const auto holiday_values =
        requiredArray(resource.document, "holidays", path, 0, maximum_holidays);
    if (!jurisdiction || !name || !role || !authorities || !holiday_values) {
        if (!jurisdiction) {
            return std::unexpected(jurisdiction.error());
        }
        if (!name) {
            return std::unexpected(name.error());
        }
        if (!role) {
            return std::unexpected(role.error());
        }
        if (!authorities) {
            return std::unexpected(authorities.error());
        }
        return std::unexpected(holiday_values.error());
    }
    std::vector<RuntimeAuthoritySetId> authority_ids;
    for (auto& authority : *authorities) {
        authority_ids.push_back(RuntimeAuthoritySetId{std::move(authority)});
    }
    std::vector<model::LegalDate> holidays;
    holidays.reserve(static_cast<std::size_t>(holiday_values->size()));
    for (qsizetype index = 0; index < holiday_values->size(); ++index) {
        auto holiday = dateValue(holiday_values->at(index), path + ".holidays");
        if (!holiday) {
            return std::unexpected(holiday.error());
        }
        holidays.push_back(*holiday);
    }
    return RuntimeCourt{RuntimeCourtId{resource.descriptor.id},
                        RuntimeJurisdictionId{*jurisdiction},
                        *name,
                        *role,
                        std::move(authority_ids),
                        model::CourtCalendar{std::move(holidays)}};
}

[[nodiscard]] bool profileSupports(const model::JudgeProfile& profile, model::CourtRole role,
                                   const RuntimeJurisdictionId& jurisdiction) {
    return std::ranges::find(profile.compatibility.court_roles, role) !=
               profile.compatibility.court_roles.end() &&
           std::ranges::find(profile.compatibility.jurisdiction_ids, jurisdiction.value) !=
               profile.compatibility.jurisdiction_ids.end();
}

[[nodiscard]] Result<RuntimeBenchConfiguration> parseBench(const ValidatedResource& resource,
                                                           const ResourceIndex& index,
                                                           const RuntimeCourt& case_court) {
    const auto path = "bench " + resource.descriptor.id;
    const auto court = requiredId(resource.document, "court_id", path);
    const auto presiding = requiredId(resource.document, "presiding_seat_id", path);
    const auto seat_values =
        requiredArray(resource.document, "seats", path, 1, maximum_bench_seats);
    if (!court) {
        return std::unexpected(court.error());
    }
    if (!presiding) {
        return std::unexpected(presiding.error());
    }
    if (!seat_values) {
        return std::unexpected(seat_values.error());
    }
    if (*court != case_court.id.value) {
        return fail(RuntimePackErrorCode::CrossReferenceFailure,
                    path + " belongs to a different court than its case");
    }
    std::vector<RuntimeBenchSeat> seats;
    seats.reserve(static_cast<std::size_t>(seat_values->size()));
    std::unordered_set<std::string> seat_ids;
    bool found_presiding = false;
    for (qsizetype seat_index = 0; seat_index < seat_values->size(); ++seat_index) {
        if (!seat_values->at(seat_index).isObject()) {
            return fail(RuntimePackErrorCode::InvalidResource,
                        path + ".seats must contain objects");
        }
        const auto seat = seat_values->at(seat_index).toObject();
        const auto seat_path = path + ".seats[" + std::to_string(seat_index) + "]";
        const auto id = requiredId(seat, "seat_id", seat_path);
        const auto profile_id = requiredId(seat, "profile_id", seat_path);
        const auto role = courtRole(seat, "court_role", seat_path);
        if (!id || !profile_id || !role) {
            if (!id) {
                return std::unexpected(id.error());
            }
            if (!profile_id) {
                return std::unexpected(profile_id.error());
            }
            return std::unexpected(role.error());
        }
        if (!seat_ids.emplace(*id).second || *role != case_court.role) {
            return fail(RuntimePackErrorCode::CrossReferenceFailure,
                        seat_path + " is duplicated or has the wrong court role");
        }
        const auto profile = index.requireJudge(*profile_id, seat_path);
        if (!profile) {
            return std::unexpected(profile.error());
        }
        if (!profileSupports(**profile, *role, case_court.jurisdiction_id)) {
            return fail(RuntimePackErrorCode::CrossReferenceFailure,
                        seat_path + " selects an incompatible judge profile");
        }
        found_presiding = found_presiding || *id == *presiding;
        seats.push_back(RuntimeBenchSeat{RuntimeBenchSeatId{*id},
                                         RuntimeJudgeProfileId{*profile_id}, *role, **profile});
    }
    if (!found_presiding) {
        return fail(RuntimePackErrorCode::CrossReferenceFailure,
                    path + " does not contain its presiding seat");
    }
    return RuntimeBenchConfiguration{RuntimeBenchConfigurationId{resource.descriptor.id},
                                     RuntimeCourtId{*court}, RuntimeBenchSeatId{*presiding},
                                     std::move(seats)};
}

[[nodiscard]] bool issueExists(const std::vector<RuntimeIssue>& issues, const std::string& id) {
    return std::ranges::any_of(issues,
                               [&id](const RuntimeIssue& issue) { return issue.id.value == id; });
}

[[nodiscard]] Result<RuntimeArgumentConfiguration> parseArgument(const ValidatedResource& resource,
                                                                 const ResourceIndex& index,
                                                                 const ParsedCase& case_document,
                                                                 const RuntimeCourt& court) {
    const auto path = "argument configuration " + resource.descriptor.id;
    const auto case_id = requiredId(resource.document, "case_id", path);
    const auto bench_id = requiredId(resource.document, "bench_configuration_id", path);
    const auto total = requiredUnsigned(resource.document, "total_seconds", path, 60, 7200);
    const auto rebuttal = requiredUnsigned(resource.document, "rebuttal_seconds", path, 0, 1800);
    const auto issue_ids =
        idArray(resource.document, "permitted_issue_ids", path, 1, maximum_case_items);
    if (!case_id || !bench_id || !total || !rebuttal || !issue_ids) {
        if (!case_id) {
            return std::unexpected(case_id.error());
        }
        if (!bench_id) {
            return std::unexpected(bench_id.error());
        }
        if (!total) {
            return std::unexpected(total.error());
        }
        if (!rebuttal) {
            return std::unexpected(rebuttal.error());
        }
        return std::unexpected(issue_ids.error());
    }
    if (*case_id != case_document.definition.id.value || *rebuttal > *total) {
        return fail(RuntimePackErrorCode::CrossReferenceFailure,
                    path + " has inconsistent case or time limits");
    }
    const auto bench_resource =
        index.require(*bench_id, model::ResourceKind::BenchConfiguration, path);
    if (!bench_resource) {
        return std::unexpected(bench_resource.error());
    }
    auto bench = parseBench(**bench_resource, index, court);
    if (!bench) {
        return std::unexpected(bench.error());
    }
    std::vector<RuntimeIssueId> permitted;
    permitted.reserve(issue_ids->size());
    for (auto& issue : *issue_ids) {
        if (!issueExists(case_document.issues, issue)) {
            return fail(RuntimePackErrorCode::CrossReferenceFailure,
                        path + " references issue outside its case: " + issue);
        }
        permitted.push_back(RuntimeIssueId{std::move(issue)});
    }
    return RuntimeArgumentConfiguration{RuntimeArgumentConfigId{resource.descriptor.id},
                                        model::CaseId{*case_id},
                                        std::move(*bench),
                                        *total,
                                        *rebuttal,
                                        std::move(permitted)};
}

[[nodiscard]] bool sameCalendar(const model::CourtCalendar& left,
                                const model::CourtCalendar& right) {
    if (left.holidays.size() != right.holidays.size()) {
        return false;
    }
    std::unordered_set<std::int64_t> dates;
    dates.reserve(left.holidays.size());
    for (const auto& date : left.holidays) {
        dates.insert(std::chrono::sys_days{date.value}.time_since_epoch().count());
    }
    return std::ranges::all_of(right.holidays, [&dates](const model::LegalDate& date) {
        return dates.contains(std::chrono::sys_days{date.value}.time_since_epoch().count());
    });
}

[[nodiscard]] bool hasRole(const RuntimeProcedure& procedure, const model::ActorRoleId& role) {
    return std::ranges::find(procedure.actor_roles, role) != procedure.actor_roles.end();
}

[[nodiscard]] Result<RuntimeCase>
assembleCase(const ValidatedResource& resource, const ResourceIndex& index,
             const std::vector<const ValidatedResource*>& arguments) {
    auto parsed_case = parseCase(resource);
    if (!parsed_case) {
        return std::unexpected(parsed_case.error());
    }
    const auto procedure_resource =
        index.require(parsed_case->definition.procedure_id.value,
                      model::ResourceKind::ProcedureProfile, "case " + resource.descriptor.id);
    if (!procedure_resource) {
        return std::unexpected(procedure_resource.error());
    }
    auto procedure = parseProcedure(**procedure_resource);
    if (!procedure) {
        return std::unexpected(procedure.error());
    }
    const auto workflow_resource =
        index.require(procedure->workflow_id.value, model::ResourceKind::Workflow,
                      "procedure " + procedure->id.value);
    const auto court_resource = index.require(procedure->court_id.value, model::ResourceKind::Court,
                                              "procedure " + procedure->id.value);
    const auto record_resource =
        index.require(parsed_case->record_id.value, model::ResourceKind::Record,
                      "case " + parsed_case->definition.id.value);
    const auto catalog_resource =
        index.require(procedure->filing_catalog_id.value, model::ResourceKind::FilingCatalog,
                      "procedure " + procedure->id.value);
    if (!workflow_resource || !court_resource || !record_resource || !catalog_resource) {
        if (!workflow_resource) {
            return std::unexpected(workflow_resource.error());
        }
        if (!court_resource) {
            return std::unexpected(court_resource.error());
        }
        if (!record_resource) {
            return std::unexpected(record_resource.error());
        }
        return std::unexpected(catalog_resource.error());
    }
    for (const auto& authority_id : procedure->authority_set_ids) {
        const auto authority = index.require(authority_id.value, model::ResourceKind::AuthoritySet,
                                             "procedure " + procedure->id.value);
        if (!authority) {
            return std::unexpected(authority.error());
        }
    }
    auto workflow = parseWorkflow(**workflow_resource);
    auto court = parseCourt(**court_resource);
    auto record = parseRecord(**record_resource);
    auto catalog = parseFilingCatalog(**catalog_resource);
    if (!workflow) {
        return std::unexpected(workflow.error());
    }
    if (!court) {
        return std::unexpected(court.error());
    }
    if (!record) {
        return std::unexpected(record.error());
    }
    if (!catalog) {
        return std::unexpected(catalog.error());
    }
    for (const auto& authority_id : court->authority_set_ids) {
        const auto authority = index.require(authority_id.value, model::ResourceKind::AuthoritySet,
                                             "court " + court->id.value);
        if (!authority) {
            return std::unexpected(authority.error());
        }
    }
    if (!sameCalendar(workflow->calendar, court->calendar)) {
        return fail(RuntimePackErrorCode::CrossReferenceFailure,
                    "case " + parsed_case->definition.id.value +
                        " has different court and workflow calendars");
    }
    std::unordered_set<std::string> procedure_roles;
    procedure_roles.reserve(procedure->actor_roles.size());
    for (const auto& role : procedure->actor_roles) {
        procedure_roles.insert(role.value);
    }
    for (const auto& [filing_id, filing] : *catalog) {
        if (std::ranges::any_of(filing.authorized_roles, [&](const auto& role) {
                return !procedure_roles.contains(role);
            })) {
            return fail(RuntimePackErrorCode::CrossReferenceFailure,
                        "filing catalog entry " + filing_id + " uses a role outside its procedure");
        }
    }
    for (const auto& operation : workflow->operations) {
        if (std::ranges::any_of(operation.authorized_roles, [&](const auto& role) {
                return !procedure_roles.contains(role.value);
            })) {
            return fail(RuntimePackErrorCode::CrossReferenceFailure,
                        "workflow " + workflow->id.value +
                            " uses an operation role outside its procedure");
        }
    }
    for (const auto& route : workflow->filing_routes) {
        const auto declared = catalog->find(route.filing_type.value);
        std::unordered_set<std::string> route_roles;
        std::unordered_set<std::string> route_fields;
        route_roles.reserve(route.authorized_roles.size());
        route_fields.reserve(route.required_fields.size());
        for (const auto& role : route.authorized_roles) {
            route_roles.insert(role.value);
        }
        for (const auto& field : route.required_fields) {
            route_fields.insert(field.value);
        }
        if (declared == catalog->end() || route_roles != declared->second.authorized_roles ||
            route_fields != declared->second.required_fields ||
            std::ranges::any_of(
                route.authorized_roles,
                [&](const auto& role) { return !procedure_roles.contains(role.value); }) ||
            std::ranges::any_of(route.required_service_roles, [&](const auto& role) {
                return !procedure_roles.contains(role.value);
            })) {
            return fail(RuntimePackErrorCode::CrossReferenceFailure,
                        "workflow " + workflow->id.value +
                            " route conflicts with its procedure filing catalog");
        }
    }
    for (const auto& actor : parsed_case->definition.actors) {
        if (!hasRole(*procedure, actor.role)) {
            return fail(RuntimePackErrorCode::CrossReferenceFailure,
                        "case " + parsed_case->definition.id.value +
                            " uses a role outside its procedure");
        }
    }
    std::unordered_set<std::string> record_entries;
    for (const auto& entry : record->docket_entries) {
        record_entries.insert(entry.id.value);
    }
    for (const auto& issue : parsed_case->issues) {
        for (const auto& anchor : issue.record_anchor_ids) {
            if (!record_entries.contains(anchor.value)) {
                return fail(RuntimePackErrorCode::CrossReferenceFailure,
                            "case " + parsed_case->definition.id.value +
                                " references an entry outside its record");
            }
        }
    }
    const auto authored_operation = std::ranges::find(
        workflow->operations, parsed_case->authored_disposition_id, &model::WorkflowOperation::id);
    if (authored_operation == workflow->operations.end() ||
        authored_operation->opcode != model::WorkflowOpcode::IssueJudgment) {
        return fail(RuntimePackErrorCode::CrossReferenceFailure,
                    "case " + parsed_case->definition.id.value +
                        " has no authored judgment operation");
    }
    if (arguments.empty()) {
        return fail(RuntimePackErrorCode::MissingArgumentConfiguration,
                    "case " + parsed_case->definition.id.value +
                        " has no oral-argument configuration");
    }
    std::vector<RuntimeArgumentConfiguration> runtime_arguments;
    runtime_arguments.reserve(arguments.size());
    for (const auto* argument : arguments) {
        auto parsed_argument = parseArgument(*argument, index, *parsed_case, *court);
        if (!parsed_argument) {
            return std::unexpected(parsed_argument.error());
        }
        runtime_arguments.push_back(std::move(*parsed_argument));
    }
    return RuntimeCase{std::move(parsed_case->definition),
                       std::move(parsed_case->title),
                       std::move(*procedure),
                       std::move(*court),
                       std::move(*workflow),
                       std::move(*record),
                       std::move(parsed_case->issues),
                       std::move(parsed_case->authored_disposition_id),
                       std::move(runtime_arguments)};
}

} // namespace

std::expected<RuntimePack, RuntimePackError> loadRuntimePack(const LoadedPack& pack) {
    const auto index = makeIndex(pack);
    if (!index) {
        return std::unexpected(index.error());
    }

    std::vector<const ValidatedResource*> cases;
    std::unordered_map<std::string, std::vector<const ValidatedResource*>> arguments_by_case;
    for (const auto& resource : pack.resources) {
        if (resource.descriptor.kind == model::ResourceKind::Case) {
            cases.push_back(&resource);
        } else if (resource.descriptor.kind == model::ResourceKind::ArgumentConfig) {
            const auto path = "argument configuration " + resource.descriptor.id;
            const auto case_id = requiredId(resource.document, "case_id", path);
            if (!case_id) {
                return std::unexpected(case_id.error());
            }
            const auto case_resource = index->require(*case_id, model::ResourceKind::Case, path);
            if (!case_resource) {
                return std::unexpected(case_resource.error());
            }
            arguments_by_case[*case_id].push_back(&resource);
        }
    }
    if (cases.empty()) {
        return fail(RuntimePackErrorCode::MissingResource,
                    "runtime pack contains no case resources");
    }
    std::ranges::sort(cases, [](const ValidatedResource* left, const ValidatedResource* right) {
        return left->descriptor.id < right->descriptor.id;
    });
    for (auto& [case_id, arguments] : arguments_by_case) {
        static_cast<void>(case_id);
        std::ranges::sort(arguments,
                          [](const ValidatedResource* left, const ValidatedResource* right) {
                              return left->descriptor.id < right->descriptor.id;
                          });
    }

    std::vector<RuntimeCase> runtime_cases;
    runtime_cases.reserve(cases.size());
    for (const auto* resource : cases) {
        const auto found = arguments_by_case.find(resource->descriptor.id);
        const std::vector<const ValidatedResource*> no_arguments;
        const auto& arguments = found == arguments_by_case.end() ? no_arguments : found->second;
        auto runtime_case = assembleCase(*resource, *index, arguments);
        if (!runtime_case) {
            return std::unexpected(runtime_case.error());
        }
        runtime_cases.push_back(std::move(*runtime_case));
    }
    return RuntimePack{pack.revision, std::move(runtime_cases)};
}

} // namespace appellate::packs
