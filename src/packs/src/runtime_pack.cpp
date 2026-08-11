#include "appellate/packs/runtime_pack.hpp"
#include "appellate/packs/capability_registry.hpp"
#include "appellate/packs/pack_version.hpp"
#include "realism_evidence.hpp"
#include "runtime_pack_internal.hpp"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDate>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
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
constexpr qsizetype maximum_disposition_targets = 4096;
constexpr qsizetype maximum_disposition_plans = 64;
constexpr qsizetype maximum_disposition_components = 32;
constexpr qsizetype maximum_workflow_preconditions = 32;
constexpr qsizetype maximum_component_authorities = 32;
constexpr qsizetype maximum_component_record_anchors = 32;
constexpr qsizetype maximum_argument_issue_bindings = 64;
constexpr qsizetype maximum_argument_topics_per_issue = 8;
constexpr qsizetype maximum_authored_questions = 128;
constexpr qsizetype maximum_authored_questions_per_issue = 16;
constexpr qsizetype maximum_question_grounding = 16;

template <typename Value> using Result = std::expected<Value, RuntimePackError>;

[[nodiscard]] auto fail(RuntimePackErrorCode code, std::string message)
    -> std::unexpected<RuntimePackError> {
    return std::unexpected(RuntimePackError{code, std::move(message)});
}

[[nodiscard]] std::string utf8(const QString& value) { return value.toUtf8().toStdString(); }

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

[[nodiscard]] bool isBoundedUtf8Text(const std::string& value, qsizetype maximum) {
    if (value.empty()) {
        return false;
    }
    const auto text = QString::fromUtf8(value);
    const auto count = unicodeScalarCount(text);
    return count.has_value() && *count <= maximum && utf8(text) == value;
}

[[nodiscard]] bool isNamespacedId(const QString& value) {
    static const QRegularExpression pattern(
        QStringLiteral(R"(^[a-z0-9]+(?:[.-][a-z0-9]+)+(?:[-.][a-z0-9]+)*$)"));
    return value.size() >= 3 && value.size() <= 160 && pattern.match(value).hasMatch();
}

[[nodiscard]] bool deadlineNamespaceContains(std::string_view prefix, std::string_view id) {
    return id == prefix ||
           (id.size() > prefix.size() && id.starts_with(prefix) && id[prefix.size()] == '.');
}

[[nodiscard]] bool deadlineNamespacesOverlap(std::string_view left, std::string_view right) {
    return deadlineNamespaceContains(left, right) || deadlineNamespaceContains(right, left);
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

[[nodiscard]] bool hasExactKeys(const QJsonObject& object,
                                std::initializer_list<const char*> keys) {
    if (object.size() != static_cast<qsizetype>(keys.size())) {
        return false;
    }
    return std::ranges::all_of(
        keys, [&object](const char* key) { return object.contains(QLatin1StringView(key)); });
}

void addDispositionUint64(QCryptographicHash& hash, std::uint64_t value) {
    std::array<char, 8> bytes{};
    for (int index = 7; index >= 0; --index) {
        bytes[static_cast<std::size_t>(index)] = static_cast<char>(value & 0xffU);
        value >>= 8U;
    }
    hash.addData(QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())));
}

void addDispositionFrame(QCryptographicHash& hash, const std::string& value) {
    addDispositionUint64(hash, value.size());
    hash.addData(QByteArrayView(value.data(), static_cast<qsizetype>(value.size())));
}

[[nodiscard]] std::string_view dispositionScopeName(model::DispositionScope value) {
    switch (value) {
    case model::DispositionScope::Whole:
        return "whole";
    case model::DispositionScope::Part:
        return "part";
    }
    return {};
}

[[nodiscard]] std::string_view dispositionActionName(model::DispositionAction value) {
    switch (value) {
    case model::DispositionAction::Affirm:
        return "affirm";
    case model::DispositionAction::Reverse:
        return "reverse";
    case model::DispositionAction::Vacate:
        return "vacate";
    case model::DispositionAction::Dismiss:
        return "dismiss";
    case model::DispositionAction::Grant:
        return "grant";
    case model::DispositionAction::Deny:
        return "deny";
    }
    return {};
}

[[nodiscard]] std::string_view dispositionFinalityName(model::DispositionFinality value) {
    switch (value) {
    case model::DispositionFinality::Final:
        return "final";
    case model::DispositionFinality::Nonfinal:
        return "nonfinal";
    }
    return {};
}

[[nodiscard]] std::string canonicalDispositionPlanDigest(const std::string& case_id,
                                                         const std::string& authored_operation_id,
                                                         const model::DispositionPlan& plan) {
    std::vector<const model::DispositionComponent*> components;
    components.reserve(plan.components.size());
    for (const auto& component : plan.components) {
        components.push_back(&component);
    }
    std::ranges::sort(components, [](const auto* left, const auto* right) {
        return std::tuple{left->issue_id.value, left->target_id.value} <
               std::tuple{right->issue_id.value, right->target_id.value};
    });

    QCryptographicHash hash(QCryptographicHash::Sha256);
    addDispositionFrame(hash, "appellate-workbench-disposition-plan-v1");
    addDispositionFrame(hash, case_id);
    addDispositionFrame(hash, authored_operation_id);
    addDispositionFrame(hash, plan.id.value);
    addDispositionFrame(hash, std::string(dispositionFinalityName(plan.finality)));
    addDispositionUint64(hash, components.size());
    for (const auto* component : components) {
        addDispositionFrame(hash, component->issue_id.value);
        addDispositionFrame(hash, component->target_id.value);
        addDispositionFrame(hash, std::string(dispositionScopeName(component->scope)));
        addDispositionFrame(hash, std::string(dispositionActionName(component->action)));
        addDispositionUint64(hash, component->remand ? 1U : 0U);

        auto authority_ids = component->authority_ids;
        std::ranges::sort(authority_ids, {}, &model::AuthorityId::value);
        addDispositionUint64(hash, authority_ids.size());
        for (const auto& authority_id : authority_ids) {
            addDispositionFrame(hash, authority_id.value);
        }

        auto record_anchor_ids = component->record_anchor_ids;
        std::ranges::sort(record_anchor_ids, {}, &model::RecordAnchorId::value);
        addDispositionUint64(hash, record_anchor_ids.size());
        for (const auto& anchor_id : record_anchor_ids) {
            addDispositionFrame(hash, anchor_id.value);
        }
    }
    return hash.result().toHex().toStdString();
}

[[nodiscard]] std::string_view oralArgumentModeName(model::OralArgumentMode value) {
    switch (value) {
    case model::OralArgumentMode::ActualRecord:
        return "actual_record";
    case model::OralArgumentMode::CounterfactualTraining:
        return "counterfactual_training";
    }
    return {};
}

[[nodiscard]] std::string_view authorityTypeName(model::AuthorityType value) {
    switch (value) {
    case model::AuthorityType::Constitution:
        return "constitution";
    case model::AuthorityType::Statute:
        return "statute";
    case model::AuthorityType::Rule:
        return "rule";
    case model::AuthorityType::Regulation:
        return "regulation";
    case model::AuthorityType::Case:
        return "case";
    case model::AuthorityType::Order:
        return "order";
    case model::AuthorityType::AdministrativeDecision:
        return "administrative_decision";
    case model::AuthorityType::Other:
        return "other";
    }
    return {};
}

[[nodiscard]] std::string_view precedentialStatusName(model::PrecedentialStatus value) {
    switch (value) {
    case model::PrecedentialStatus::NotApplicable:
        return "not_applicable";
    case model::PrecedentialStatus::Precedential:
        return "precedential";
    case model::PrecedentialStatus::Nonprecedential:
        return "nonprecedential";
    }
    return {};
}

[[nodiscard]] const std::string& groundingId(const model::AuthoredArgumentGrounding& grounding) {
    return std::visit([](const auto& value) -> const std::string& { return value.grounding_id; },
                      grounding);
}

[[nodiscard]] std::string canonicalQuestionBankDigest(const model::AuthoredQuestionBank& bank) {
    std::vector<const model::ArgumentIssueTopics*> bindings;
    bindings.reserve(bank.issue_topics.size());
    for (const auto& binding : bank.issue_topics) {
        bindings.push_back(&binding);
    }
    std::ranges::sort(bindings, [](const auto* left, const auto* right) {
        return left->issue_id < right->issue_id;
    });
    std::vector<const model::AuthoredArgumentQuestion*> questions;
    questions.reserve(bank.questions.size());
    for (const auto& question : bank.questions) {
        questions.push_back(&question);
    }
    std::ranges::sort(questions,
                      [](const auto* left, const auto* right) { return left->id < right->id; });

    QCryptographicHash hash(QCryptographicHash::Sha256);
    addDispositionFrame(hash, "appellate-workbench-grounded-question-bank-v1");
    addDispositionFrame(hash, bank.case_id.value);
    addDispositionFrame(hash, bank.argument_configuration_id);
    addDispositionFrame(hash, std::string(oralArgumentModeName(bank.mode)));
    addDispositionUint64(hash, bindings.size());
    for (const auto* binding : bindings) {
        addDispositionFrame(hash, binding->issue_id);
        std::vector<std::string_view> topic_ids;
        topic_ids.reserve(binding->topics.size());
        for (const auto topic : binding->topics) {
            topic_ids.push_back(model::argumentFocusTopicId(topic));
        }
        std::ranges::sort(topic_ids);
        addDispositionUint64(hash, topic_ids.size());
        for (const auto topic_id : topic_ids) {
            addDispositionFrame(hash, std::string(topic_id));
        }
    }
    addDispositionUint64(hash, questions.size());
    for (const auto* question : questions) {
        addDispositionFrame(hash, question->id);
        addDispositionFrame(hash, question->issue_id);
        addDispositionFrame(hash, std::string(model::argumentFocusTopicId(question->topic)));
        addDispositionFrame(hash, question->prompt);
        std::vector<const model::AuthoredArgumentGrounding*> grounding;
        grounding.reserve(question->grounding.size());
        for (const auto& reference : question->grounding) {
            grounding.push_back(&reference);
        }
        std::ranges::sort(grounding, [](const auto* left, const auto* right) {
            return groundingId(*left) < groundingId(*right);
        });
        addDispositionUint64(hash, grounding.size());
        for (const auto* reference : grounding) {
            addDispositionFrame(hash, groundingId(*reference));
            if (const auto* authority = std::get_if<model::AuthorityArgumentGrounding>(reference)) {
                addDispositionFrame(hash, "authority");
                addDispositionFrame(hash, authority->authority.id.value);
                addDispositionFrame(hash, authority->authority.citation);
                addDispositionFrame(hash, authority->authority.source_version);
                addDispositionFrame(hash, authority->authority.proposition);
                addDispositionUint64(hash, authority->authority.provenance.has_value() ? 1U : 0U);
                if (authority->authority.provenance.has_value()) {
                    const auto& provenance = *authority->authority.provenance;
                    addDispositionFrame(hash, std::string(authorityTypeName(provenance.type)));
                    addDispositionFrame(hash, provenance.jurisdiction_id);
                    addDispositionFrame(hash, provenance.issuing_body_id);
                    addDispositionFrame(
                        hash, std::string(precedentialStatusName(provenance.precedential_status)));
                    addDispositionUint64(hash, provenance.official_source ? 1U : 0U);
                    addDispositionFrame(hash, provenance.checked_on);
                    addDispositionFrame(hash, provenance.locator);
                    addDispositionFrame(hash, provenance.source_url);
                }
            } else if (const auto* brief =
                           std::get_if<model::BriefPageArgumentGrounding>(reference)) {
                addDispositionFrame(hash, "brief_page");
                addDispositionFrame(hash, brief->record_entry_id);
                addDispositionUint64(hash, brief->page_number);
                addDispositionFrame(hash, brief->asset_sha256);
            } else {
                const auto& record = std::get<model::RecordPageArgumentGrounding>(*reference);
                addDispositionFrame(hash, "record_page");
                addDispositionFrame(hash, record.record_anchor_id);
                addDispositionFrame(hash, record.record_entry_id);
                addDispositionUint64(hash, record.page_number);
                addDispositionFrame(hash, record.asset_sha256);
                addDispositionUint64(hash, record.citation_label.has_value() ? 1U : 0U);
                if (record.citation_label.has_value()) {
                    addDispositionFrame(hash, *record.citation_label);
                }
            }
        }
    }
    return hash.result().toHex().toStdString();
}

[[nodiscard]] bool isCanonicalQuestionPrompt(const std::string& prompt) {
    if (!model::isCanonicalAuthorityText(prompt, 512) || prompt.front() == ' ' ||
        prompt.back() == ' ') {
        return false;
    }
    const auto unicode = QString::fromUtf8(prompt.data(), static_cast<qsizetype>(prompt.size()));
    return std::ranges::any_of(unicode, [](QChar scalar) { return !scalar.isSpace(); });
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

[[nodiscard]] Result<std::optional<std::string>> optionalString(const QJsonObject& object,
                                                                const char* key,
                                                                const std::string& path,
                                                                qsizetype maximum) {
    if (!object.contains(QLatin1StringView(key))) {
        return std::optional<std::string>{};
    }
    const auto value = object.value(QLatin1StringView(key));
    const auto text = value.toString();
    const auto count = unicodeScalarCount(text);
    if (!value.isString() || text.isEmpty() || !count.has_value() || *count > maximum) {
        return fail(RuntimePackErrorCode::InvalidResource,
                    path + "." + key + " must be bounded nonempty text");
    }
    return std::optional<std::string>{utf8(text)};
}

[[nodiscard]] Result<std::vector<std::string>>
optionalStringArray(const QJsonObject& object, const char* key, const std::string& path,
                    qsizetype maximum_items, qsizetype maximum_text) {
    if (!object.contains(QLatin1StringView(key))) {
        return std::vector<std::string>{};
    }
    const auto values = requiredArray(object, key, path, 0, maximum_items);
    if (!values) {
        return std::unexpected(values.error());
    }
    std::vector<std::string> result;
    result.reserve(static_cast<std::size_t>(values->size()));
    std::unordered_set<std::string> seen;
    for (const auto& value : *values) {
        const auto text_value = value.toString();
        const auto count = unicodeScalarCount(text_value);
        if (!value.isString() || text_value.isEmpty() || !count.has_value() ||
            *count > maximum_text) {
            return fail(RuntimePackErrorCode::InvalidResource,
                        path + "." + key + " must contain bounded nonempty text");
        }
        auto text = utf8(text_value);
        if (!seen.emplace(text).second) {
            return fail(RuntimePackErrorCode::InvalidResource,
                        path + "." + key + " must not contain duplicates");
        }
        result.push_back(std::move(text));
    }
    return result;
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
    struct CanonicalAuthority final {
        model::AuthorityRef reference;
        std::string authority_set_id;
        const model::PackRevision* owner{};
    };

    std::unordered_map<std::string, const ValidatedResource*> resources;
    std::unordered_map<std::string, const model::JudgeProfile*> judge_profiles;
    std::unordered_map<std::string, const model::PackRevision*> resource_owners;
    std::unordered_map<std::string, CanonicalAuthority> authorities;

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

    [[nodiscard]] bool
    authorityInSets(const std::string& id,
                    std::span<const RuntimeAuthoritySetId> authority_set_ids) const {
        const auto authority = authorities.find(id);
        return authority != authorities.end() &&
               std::ranges::any_of(authority_set_ids, [&authority](const auto& set_id) {
                   return set_id.value == authority->second.authority_set_id;
               });
    }

    [[nodiscard]] bool ownedBy(const std::string& id, const model::PackRevision& revision) const {
        const auto found = resource_owners.find(id);
        return found != resource_owners.end() && *found->second == revision;
    }

    [[nodiscard]] Result<model::AuthorityRef> requireAuthority(const std::string& id,
                                                               const std::string& owner) const {
        const auto found = authorities.find(id);
        if (found == authorities.end()) {
            return fail(RuntimePackErrorCode::MissingResource,
                        owner + " references missing canonical authority " + id);
        }
        return found->second.reference;
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

[[nodiscard]] Result<model::AuthorityRef> parseAuthorityReference(const QJsonObject& object,
                                                                  const std::string& path,
                                                                  std::uint32_t schema_version);

[[nodiscard]] Result<ResourceIndex> makeIndex(std::span<const LoadedPack* const> packs,
                                              std::uint32_t manifest_schema_version) {
    if (packs.empty() || (manifest_schema_version != 1 && manifest_schema_version != 2)) {
        return fail(RuntimePackErrorCode::InvalidPack,
                    "runtime pack has invalid revision metadata or resource bounds");
    }

    std::size_t total_resources = 0;
    std::size_t total_judges = 0;
    for (const auto* pack : packs) {
        if (pack == nullptr || pack->manifest_schema_version != manifest_schema_version ||
            pack->revision.id.value.size() > 128 ||
            !isNamespacedId(QString::fromStdString(pack->revision.id.value)) ||
            !isValidPackVersion(QString::fromStdString(pack->revision.version),
                                pack->manifest_schema_version) ||
            !isSha256(QString::fromStdString(pack->revision.digest)) ||
            pack->resources.size() > maximum_resources - total_resources ||
            pack->judge_profiles.size() > maximum_resources - total_judges) {
            return fail(RuntimePackErrorCode::InvalidPack,
                        "runtime closure has invalid revision metadata, schema, or bounds");
        }
        std::vector<model::ResourceKind> resource_kinds;
        resource_kinds.reserve(pack->resources.size());
        std::ranges::transform(pack->resources, std::back_inserter(resource_kinds),
                               [](const auto& resource) { return resource.descriptor.kind; });
        const auto uses_workflow_preconditions =
            std::ranges::any_of(pack->resources, [](const ValidatedResource& resource) {
                return resource.descriptor.kind == model::ResourceKind::Workflow &&
                       std::ranges::any_of(
                           resource.document.value(QStringLiteral("operations")).toArray(),
                           [](const QJsonValue& operation) {
                               return !operation.toObject()
                                           .value(QStringLiteral("preconditions"))
                                           .toArray()
                                           .isEmpty();
                           });
            });
        const auto uses_dependent_deadlines =
            std::ranges::any_of(pack->resources, [](const ValidatedResource& resource) {
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
        const auto uses_named_deadlines =
            std::ranges::any_of(pack->resources, [](const ValidatedResource& resource) {
                if (resource.descriptor.kind != model::ResourceKind::Workflow) {
                    return false;
                }
                return std::ranges::any_of(
                    resource.document.value(QStringLiteral("operations")).toArray(),
                    [](const QJsonValue& operation_value) {
                        return operation_value.toObject().contains(
                            QStringLiteral("produced_deadline_id"));
                    });
            });
        const auto uses_event_date_deadlines =
            std::ranges::any_of(pack->resources, [](const ValidatedResource& resource) {
                if (resource.descriptor.kind != model::ResourceKind::Workflow) {
                    return false;
                }
                return std::ranges::any_of(
                    resource.document.value(QStringLiteral("operations")).toArray(),
                    [](const QJsonValue& operation_value) {
                        return operation_value.toObject().contains(
                            QStringLiteral("deadline_event_base"));
                    });
            });
        const auto uses_argument_date_guards =
            std::ranges::any_of(pack->resources, [](const ValidatedResource& resource) {
                if (resource.descriptor.kind != model::ResourceKind::Workflow) {
                    return false;
                }
                return std::ranges::any_of(
                    resource.document.value(QStringLiteral("operations")).toArray(),
                    [](const QJsonValue& operation_value) {
                        return std::ranges::any_of(
                            operation_value.toObject()
                                .value(QStringLiteral("preconditions"))
                                .toArray(),
                            [](const QJsonValue& precondition_value) {
                                return precondition_value.toObject()
                                           .value(QStringLiteral("kind"))
                                           .toString() == QStringLiteral("argument_date_status");
                            });
                    });
            });
        const auto uses_structured_disposition =
            std::ranges::any_of(pack->resources, [](const ValidatedResource& resource) {
                if (resource.descriptor.kind != model::ResourceKind::Case) {
                    return false;
                }
                if (resource.document.contains(QStringLiteral("disposition_plans")) ||
                    resource.document.contains(QStringLiteral("authored_disposition_plan_id"))) {
                    return true;
                }
                return std::ranges::any_of(
                    resource.document.value(QStringLiteral("issues")).toArray(),
                    [](const QJsonValue& issue) {
                        return issue.toObject().contains(QStringLiteral("target_ids"));
                    });
            });
        const auto uses_grounded_questions =
            std::ranges::any_of(pack->resources, [](const ValidatedResource& resource) {
                return resource.descriptor.kind == model::ResourceKind::ArgumentConfig &&
                       resource.document.contains(QStringLiteral("grounded_question_bank"));
            });
        const auto uses_realism_evidence =
            std::ranges::any_of(pack->resources, [](const ValidatedResource& resource) {
                return resource.descriptor.kind == model::ResourceKind::RealismReview &&
                       (resource.document.contains(QStringLiteral("evidence")) ||
                        resource.document.contains(QStringLiteral("reviewer")) ||
                        std::ranges::any_of(
                            resource.document.value(QStringLiteral("known_uncertainty")).toArray(),
                            [](const QJsonValue& uncertainty) { return uncertainty.isObject(); }));
            });
        const auto uses_sealed_record_twins =
            std::ranges::any_of(pack->resources, [](const ValidatedResource& resource) {
                return resource.descriptor.kind == model::ResourceKind::Record &&
                       (resource.document.contains(QStringLiteral("disclosure_policy")) ||
                        resource.document.contains(QStringLiteral("sealed_disclosures")));
            });
        const auto capabilities = CapabilityRegistry::validateCoverage(
            pack->manifest_schema_version, pack->required_capabilities, resource_kinds,
            uses_workflow_preconditions, uses_dependent_deadlines, uses_named_deadlines,
            uses_event_date_deadlines, uses_argument_date_guards, uses_structured_disposition,
            uses_grounded_questions, uses_realism_evidence, uses_sealed_record_twins);
        if (!capabilities) {
            return fail(RuntimePackErrorCode::InvalidPack,
                        capabilities.error().message.toStdString());
        }
        total_resources += pack->resources.size();
        total_judges += pack->judge_profiles.size();
    }
    if (total_resources == 0) {
        return fail(RuntimePackErrorCode::InvalidPack, "runtime closure contains no resources");
    }

    ResourceIndex index;
    index.resources.reserve(total_resources);
    index.resource_owners.reserve(total_resources);
    for (const auto* pack : packs) {
        for (const auto& resource : pack->resources) {
            const auto path = "resource " + resource.descriptor.id;
            const auto document_id = requiredId(resource.document, "resource_id", path);
            const auto document_kind = requiredString(resource.document, "resource_kind", path);
            const auto schema_version =
                requiredUnsigned(resource.document, "schema_version", path, manifest_schema_version,
                                 manifest_schema_version);
            if (!document_id || !document_kind || !schema_version) {
                if (!document_id) {
                    return std::unexpected(document_id.error());
                }
                if (!document_kind) {
                    return std::unexpected(document_kind.error());
                }
                return std::unexpected(schema_version.error());
            }
            if (resource.descriptor.schema_version != manifest_schema_version ||
                *schema_version != manifest_schema_version ||
                resource.descriptor.id != *document_id ||
                *document_kind != kindName(resource.descriptor.kind)) {
                return fail(RuntimePackErrorCode::InvalidResource,
                            path + " does not match its validated descriptor");
            }
            if (!index.resources.emplace(resource.descriptor.id, &resource).second ||
                !index.resource_owners.emplace(resource.descriptor.id, &pack->revision).second) {
                return fail(RuntimePackErrorCode::DuplicateResource,
                            "runtime closure contains duplicate resource id " +
                                resource.descriptor.id);
            }
        }
    }

    index.judge_profiles.reserve(total_judges);
    for (const auto* pack : packs) {
        for (const auto& profile : pack->judge_profiles) {
            if (!isNamespacedId(QString::fromStdString(profile.id)) ||
                !index.judge_profiles.emplace(profile.id, &profile).second) {
                return fail(RuntimePackErrorCode::DuplicateResource,
                            "runtime closure contains invalid or duplicate typed judge profile " +
                                profile.id);
            }
            const auto resource = index.require(profile.id, model::ResourceKind::JudgeProfile,
                                                "typed judge profile " + profile.id);
            if (!resource) {
                return std::unexpected(resource.error());
            }
        }
    }
    for (const auto* pack : packs) {
        for (const auto& resource : pack->resources) {
            if (resource.descriptor.kind == model::ResourceKind::JudgeProfile &&
                !index.judge_profiles.contains(resource.descriptor.id)) {
                return fail(RuntimePackErrorCode::MissingResource,
                            "judge resource has no typed profile " + resource.descriptor.id);
            }
        }
    }
    index.authorities.reserve(total_resources);
    for (const auto* pack : packs) {
        for (const auto& resource : pack->resources) {
            if (resource.descriptor.kind != model::ResourceKind::AuthoritySet) {
                continue;
            }
            const auto values = requiredArray(resource.document, "authorities",
                                              "authority set " + resource.descriptor.id, 1, 2048);
            if (!values) {
                return std::unexpected(values.error());
            }
            std::optional<model::LegalDate> source_cutoff;
            if (pack->manifest_schema_version == 2) {
                auto parsed_cutoff = requiredDate(resource.document, "source_cutoff",
                                                  "authority set " + resource.descriptor.id);
                if (!parsed_cutoff) {
                    return std::unexpected(parsed_cutoff.error());
                }
                source_cutoff = *parsed_cutoff;
            }
            for (qsizetype authority_index = 0; authority_index < values->size();
                 ++authority_index) {
                if (!values->at(authority_index).isObject()) {
                    return fail(RuntimePackErrorCode::InvalidResource,
                                "authority set " + resource.descriptor.id +
                                    " must contain authority objects");
                }
                const auto path = "authority set " + resource.descriptor.id + ".authorities[" +
                                  std::to_string(authority_index) + "]";
                const auto authority_object = values->at(authority_index).toObject();
                auto authority =
                    parseAuthorityReference(authority_object, path, pack->manifest_schema_version);
                if (!authority) {
                    return std::unexpected(authority.error());
                }
                if (source_cutoff.has_value()) {
                    const auto source_version =
                        requiredDate(authority_object, "source_version", path);
                    const auto checked_on = requiredDate(authority_object, "checked_on", path);
                    if (!source_version || !checked_on) {
                        return std::unexpected(
                            (!source_version ? source_version.error() : checked_on.error()));
                    }
                    if (source_version->value > source_cutoff->value ||
                        checked_on->value < source_version->value) {
                        return fail(RuntimePackErrorCode::InvalidResource,
                                    path + " has inconsistent source chronology");
                    }
                }
                const auto id = authority->id.value;
                if (!index.authorities
                         .emplace(id, ResourceIndex::CanonicalAuthority{std::move(*authority),
                                                                        resource.descriptor.id,
                                                                        &pack->revision})
                         .second) {
                    return fail(RuntimePackErrorCode::DuplicateResource,
                                "runtime closure contains duplicate canonical authority " + id);
                }
            }
        }
    }
    return index;
}

[[nodiscard]] Result<model::AuthorityRef> parseAuthorityReference(const QJsonObject& object,
                                                                  const std::string& path,
                                                                  std::uint32_t schema_version) {
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
    if (!isBoundedUtf8Text(*citation, 4096) || !isBoundedUtf8Text(*proposition, 4096)) {
        return fail(RuntimePackErrorCode::InvalidResource,
                    path + " contains overlong canonical authority text");
    }
    if (schema_version == 1) {
        return model::AuthorityRef{model::AuthorityId{*id}, *citation, utf8(source_text),
                                   *proposition, std::nullopt};
    }
    if (schema_version != 2) {
        return fail(RuntimePackErrorCode::InvalidPack,
                    path + " uses an unsupported authority schema generation");
    }
    if (!model::isCanonicalAuthorityText(*citation, 4096) ||
        !model::isCanonicalAuthorityText(*proposition, 4096)) {
        return fail(RuntimePackErrorCode::InvalidResource,
                    path + " contains noncanonical authority text");
    }

    const auto type_text = requiredString(object, "authority_type", path);
    const auto jurisdiction = requiredId(object, "jurisdiction_id", path);
    const auto issuing_body = requiredId(object, "issuing_body_id", path);
    const auto status_text = requiredString(object, "precedential_status", path);
    const auto official_source = requiredBoolean(object, "official_source", path);
    const auto checked_on = requiredDate(object, "checked_on", path);
    const auto locator = requiredString(object, "locator", path);
    const auto source_url = requiredString(object, "source_url", path);
    if (!type_text || !jurisdiction || !issuing_body || !status_text || !official_source ||
        !checked_on || !locator || !source_url) {
        if (!type_text) {
            return std::unexpected(type_text.error());
        }
        if (!jurisdiction) {
            return std::unexpected(jurisdiction.error());
        }
        if (!issuing_body) {
            return std::unexpected(issuing_body.error());
        }
        if (!status_text) {
            return std::unexpected(status_text.error());
        }
        if (!official_source) {
            return std::unexpected(official_source.error());
        }
        if (!checked_on) {
            return std::unexpected(checked_on.error());
        }
        if (!locator) {
            return std::unexpected(locator.error());
        }
        return std::unexpected(source_url.error());
    }
    static const std::unordered_map<std::string, model::AuthorityType> authority_types{
        {"constitution", model::AuthorityType::Constitution},
        {"statute", model::AuthorityType::Statute},
        {"rule", model::AuthorityType::Rule},
        {"regulation", model::AuthorityType::Regulation},
        {"case", model::AuthorityType::Case},
        {"order", model::AuthorityType::Order},
        {"administrative_decision", model::AuthorityType::AdministrativeDecision},
        {"other", model::AuthorityType::Other},
    };
    static const std::unordered_map<std::string, model::PrecedentialStatus> statuses{
        {"not_applicable", model::PrecedentialStatus::NotApplicable},
        {"precedential", model::PrecedentialStatus::Precedential},
        {"nonprecedential", model::PrecedentialStatus::Nonprecedential},
    };
    const auto type = authority_types.find(*type_text);
    const auto status = statuses.find(*status_text);
    if (type == authority_types.end() || status == statuses.end() ||
        !model::isCanonicalAuthorityText(*locator, 1024) || !isBoundedUtf8Text(*source_url, 2048) ||
        !model::isCanonicalAuthoritySourceUrl(*source_url)) {
        return fail(RuntimePackErrorCode::InvalidResource,
                    path + " contains invalid canonical authority provenance");
    }
    const auto checked_text = object.value(QStringLiteral("checked_on")).toString();
    return model::AuthorityRef{
        model::AuthorityId{*id}, *citation, utf8(source_text), *proposition,
        model::AuthorityProvenance{type->second, *jurisdiction, *issuing_body, status->second,
                                   *official_source, utf8(checked_text), *locator, *source_url}};
}

[[nodiscard]] Result<model::AuthorityBasis> parseAuthorityBasis(const QJsonObject& object,
                                                                const std::string& path,
                                                                const ResourceIndex& resource_index,
                                                                std::uint32_t schema_version) {
    if (schema_version == 2) {
        const auto primary_id = requiredId(object, "primary_authority_id", path);
        const auto supporting_ids =
            idArray(object, "supporting_authority_ids", path, 0, maximum_authorities);
        if (!primary_id) {
            return std::unexpected(primary_id.error());
        }
        if (!supporting_ids) {
            return std::unexpected(supporting_ids.error());
        }
        auto primary = resource_index.requireAuthority(*primary_id, path + ".primary_authority_id");
        if (!primary) {
            return std::unexpected(primary.error());
        }
        std::vector<model::AuthorityRef> supporting;
        supporting.reserve(supporting_ids->size());
        for (const auto& supporting_id : *supporting_ids) {
            if (supporting_id == *primary_id) {
                return fail(RuntimePackErrorCode::InvalidResource,
                            path + " repeats its primary authority as supporting authority");
            }
            auto reference =
                resource_index.requireAuthority(supporting_id, path + ".supporting_authority_ids");
            if (!reference) {
                return std::unexpected(reference.error());
            }
            supporting.push_back(std::move(*reference));
        }
        return model::AuthorityBasis{std::move(*primary), std::move(supporting)};
    }
    const auto primary_object = requiredObject(object, "primary", path);
    const auto supporting_values =
        requiredArray(object, "supporting", path, 0, maximum_authorities);
    if (!primary_object) {
        return std::unexpected(primary_object.error());
    }
    if (!supporting_values) {
        return std::unexpected(supporting_values.error());
    }
    auto primary = parseAuthorityReference(*primary_object, path + ".primary", schema_version);
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
        auto reference = parseAuthorityReference(supporting_values->at(index).toObject(),
                                                 path + ".supporting", schema_version);
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

[[nodiscard]] Result<std::vector<model::WorkflowPrecondition>>
parseWorkflowPreconditions(const QJsonObject& operation, const std::string& path,
                           std::uint32_t schema_version) {
    if (!operation.contains(QStringLiteral("preconditions"))) {
        return std::vector<model::WorkflowPrecondition>{};
    }
    if (schema_version != 2) {
        return fail(RuntimePackErrorCode::InvalidResource,
                    path + ".preconditions are supported only by schema 2 workflows");
    }
    const auto values =
        requiredArray(operation, "preconditions", path, 1, maximum_workflow_preconditions);
    if (!values) {
        return std::unexpected(values.error());
    }

    std::vector<model::WorkflowPrecondition> result;
    result.reserve(static_cast<std::size_t>(values->size()));
    std::unordered_set<std::string> subjects;
    for (qsizetype index = 0; index < values->size(); ++index) {
        if (!values->at(index).isObject()) {
            return fail(RuntimePackErrorCode::InvalidResource,
                        path + ".preconditions must contain objects");
        }
        const auto object = values->at(index).toObject();
        const auto item_path = path + ".preconditions[" + std::to_string(index) + "]";
        const auto kind = requiredString(object, "kind", item_path);
        if (!kind) {
            return std::unexpected(kind.error());
        }

        std::string subject;
        std::vector<std::string> contradictory_subjects;
        if (*kind == "filing_presence" &&
            hasExactKeys(object, {"kind", "filing_type_id", "present"})) {
            const auto filing_type = requiredId(object, "filing_type_id", item_path);
            const auto present = requiredBoolean(object, "present", item_path);
            if (!filing_type || !present) {
                return std::unexpected(!filing_type ? filing_type.error() : present.error());
            }
            subject = *kind + ':' + *filing_type;
            result.emplace_back(
                model::WorkflowFilingPrecondition{model::FilingTypeId{*filing_type}, *present});
        } else if (*kind == "order_disposition" &&
                   hasExactKeys(object, {"kind", "order_id", "disposition"})) {
            const auto order_id = requiredId(object, "order_id", item_path);
            const auto disposition = requiredString(object, "disposition", item_path);
            if (!order_id || !disposition) {
                return std::unexpected(!order_id ? order_id.error() : disposition.error());
            }
            static const std::unordered_map<std::string, model::WorkflowOrderDisposition>
                dispositions{{"granted", model::WorkflowOrderDisposition::Granted},
                             {"denied", model::WorkflowOrderDisposition::Denied},
                             {"other", model::WorkflowOrderDisposition::Other}};
            const auto found = dispositions.find(*disposition);
            if (found == dispositions.end()) {
                return fail(RuntimePackErrorCode::InvalidResource,
                            item_path + ".disposition has an unsupported value");
            }
            subject = *kind + ':' + *order_id;
            result.emplace_back(
                model::WorkflowOrderPrecondition{model::WorkflowOrderId{*order_id}, found->second});
        } else if (*kind == "deadline_status" &&
                   hasExactKeys(object, {"kind", "deadline_id", "status"})) {
            const auto deadline_id = requiredId(object, "deadline_id", item_path);
            const auto status = requiredString(object, "status", item_path);
            if (!deadline_id || !status) {
                return std::unexpected(!deadline_id ? deadline_id.error() : status.error());
            }
            static const std::unordered_map<std::string, model::WorkflowDeadlineCondition>
                conditions{{"open", model::WorkflowDeadlineCondition::Open},
                           {"satisfied", model::WorkflowDeadlineCondition::Satisfied},
                           {"reached", model::WorkflowDeadlineCondition::Reached},
                           {"elapsed", model::WorkflowDeadlineCondition::Elapsed},
                           {"not_elapsed", model::WorkflowDeadlineCondition::NotElapsed}};
            const auto found = conditions.find(*status);
            if (found == conditions.end()) {
                return fail(RuntimePackErrorCode::InvalidResource,
                            item_path + ".status has an unsupported value");
            }
            subject = *kind + ':' + *deadline_id + ':' + *status;
            if (found->second == model::WorkflowDeadlineCondition::Open) {
                contradictory_subjects.push_back(*kind + ':' + *deadline_id + ":satisfied");
            } else if (found->second == model::WorkflowDeadlineCondition::Satisfied) {
                contradictory_subjects.push_back(*kind + ':' + *deadline_id + ":open");
            } else if (found->second == model::WorkflowDeadlineCondition::Elapsed) {
                contradictory_subjects.push_back(*kind + ':' + *deadline_id + ":not_elapsed");
            } else if (found->second == model::WorkflowDeadlineCondition::NotElapsed) {
                contradictory_subjects.push_back(*kind + ':' + *deadline_id + ":elapsed");
            }
            result.emplace_back(model::WorkflowDeadlinePrecondition{
                model::WorkflowDeadlineId{*deadline_id}, found->second});
        } else if (*kind == "argument_scheduled" && hasExactKeys(object, {"kind", "scheduled"})) {
            const auto scheduled = requiredBoolean(object, "scheduled", item_path);
            if (!scheduled) {
                return std::unexpected(scheduled.error());
            }
            subject = *kind + std::string(*scheduled ? ":true" : ":false");
            contradictory_subjects.push_back(*kind + std::string(*scheduled ? ":false" : ":true"));
            if (!*scheduled) {
                contradictory_subjects.push_back("argument_date_status:reached");
            }
            result.emplace_back(model::WorkflowArgumentPrecondition{*scheduled});
        } else if (*kind == "argument_date_status" && hasExactKeys(object, {"kind", "status"})) {
            const auto status = requiredString(object, "status", item_path);
            if (!status) {
                return std::unexpected(status.error());
            }
            if (*status != "reached") {
                return fail(RuntimePackErrorCode::InvalidResource,
                            item_path + ".status has an unsupported value");
            }
            subject = "argument_date_status:reached";
            contradictory_subjects.push_back("argument_scheduled:false");
            result.emplace_back(model::WorkflowArgumentDatePrecondition{
                model::WorkflowArgumentDateCondition::Reached});
        } else if (*kind == "judgment_issued" && hasExactKeys(object, {"kind", "issued"})) {
            const auto issued = requiredBoolean(object, "issued", item_path);
            if (!issued) {
                return std::unexpected(issued.error());
            }
            subject = *kind;
            result.emplace_back(model::WorkflowJudgmentPrecondition{*issued});
        } else {
            return fail(RuntimePackErrorCode::InvalidResource,
                        item_path + " does not match a closed precondition form");
        }
        if (std::ranges::any_of(contradictory_subjects,
                                [&](const std::string& contradictory) {
                                    return subjects.contains(contradictory);
                                }) ||
            !subjects.emplace(std::move(subject)).second) {
            return fail(RuntimePackErrorCode::InvalidResource,
                        path + ".preconditions repeat or contradict a subject");
        }
    }
    return result;
}

[[nodiscard]] Result<model::WorkflowDefinition> parseWorkflow(const ValidatedResource& resource,
                                                              const ResourceIndex& resource_index,
                                                              std::uint32_t schema_version) {
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
        const auto deadline_base = optionalId(operation, "deadline_base_id", operation_path);
        const auto produced_deadline =
            optionalId(operation, "produced_deadline_id", operation_path);
        const auto roles = idArray(operation, "authorized_role_ids", operation_path, 0, 64);
        auto preconditions = parseWorkflowPreconditions(operation, operation_path, schema_version);
        if (!id || !stage || !opcode || !authority_object || !next_stage || !days || !counting ||
            !deadline_base || !produced_deadline || !roles || !preconditions) {
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
            if (!deadline_base) {
                return std::unexpected(deadline_base.error());
            }
            if (!produced_deadline) {
                return std::unexpected(produced_deadline.error());
            }
            if (!roles) {
                return std::unexpected(roles.error());
            }
            return std::unexpected(preconditions.error());
        }
        auto authority = parseAuthorityBasis(*authority_object, operation_path + ".authority",
                                             resource_index, schema_version);
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
        std::optional<model::WorkflowDeadlineId> base;
        if (deadline_base->has_value()) {
            if (schema_version != 2 || !produced_deadline->has_value()) {
                return fail(RuntimePackErrorCode::InvalidResource,
                            operation_path + ".deadline_base_id requires a schema-2 named output");
            }
            base = model::WorkflowDeadlineId{std::move(**deadline_base)};
        }
        std::optional<model::WorkflowDeadlineId> produced;
        if (produced_deadline->has_value()) {
            if (schema_version != 2) {
                return fail(RuntimePackErrorCode::InvalidResource,
                            operation_path +
                                ".produced_deadline_id is supported only by schema 2 workflows");
            }
            produced = model::WorkflowDeadlineId{std::move(**produced_deadline)};
        }
        std::optional<model::WorkflowDeadlineEventBase> event_base;
        if (operation.contains(QStringLiteral("deadline_event_base"))) {
            if (schema_version != 2 || !produced.has_value() || base.has_value()) {
                return fail(RuntimePackErrorCode::InvalidResource,
                            operation_path +
                                ".deadline_event_base requires a named schema-2 calculation and "
                                "cannot mix with deadline_base_id");
            }
            const auto object = requiredObject(operation, "deadline_event_base", operation_path);
            if (!object) {
                return std::unexpected(object.error());
            }
            const auto kind =
                requiredString(*object, "kind", operation_path + ".deadline_event_base");
            if (!kind) {
                return std::unexpected(kind.error());
            }
            if (*kind == "judgment_occurred" && hasExactKeys(*object, {"kind"})) {
                event_base = model::WorkflowJudgmentOccurredDeadlineBase{};
            } else if (*kind == "order_occurred" &&
                       hasExactKeys(*object, {"kind", "order_id", "operation_id"})) {
                const auto order_id =
                    requiredId(*object, "order_id", operation_path + ".deadline_event_base");
                const auto operation_id =
                    requiredId(*object, "operation_id", operation_path + ".deadline_event_base");
                if (!order_id || !operation_id) {
                    return std::unexpected(!order_id ? order_id.error() : operation_id.error());
                }
                event_base = model::WorkflowOrderOccurredDeadlineBase{
                    model::WorkflowOrderId{*order_id}, model::WorkflowOperationId{*operation_id}};
            } else {
                return fail(RuntimePackErrorCode::InvalidResource,
                            operation_path + ".deadline_event_base has an unsupported form");
            }
        }
        operations.push_back(model::WorkflowOperation{
            model::WorkflowOperationId{*id}, model::WorkflowStageId{*stage}, *opcode,
            std::move(*authority), std::move(next), *days, *counting, std::move(authorized_roles),
            std::move(*preconditions), std::move(base), std::move(produced),
            std::move(event_base)});
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
    std::unordered_set<std::string> named_deadline_ids;
    std::unordered_set<std::string> deadline_base_ids;
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
            (operation.deadline_base_id.has_value() &&
             (operation.opcode != model::WorkflowOpcode::CalculateDeadline ||
              !operation.produced_deadline_id.has_value())) ||
            (operation.produced_deadline_id.has_value() &&
             (operation.opcode != model::WorkflowOpcode::CalculateDeadline ||
              !named_deadline_ids.emplace(operation.produced_deadline_id->value).second)) ||
            (operation.deadline_event_base.has_value() &&
             (operation.opcode != model::WorkflowOpcode::CalculateDeadline ||
              !operation.produced_deadline_id.has_value() ||
              operation.deadline_base_id.has_value())) ||
            (operation.opcode == model::WorkflowOpcode::CalculateDeadline &&
             std::ranges::any_of(
                 operation.preconditions,
                 [](const auto& precondition) {
                     if (std::holds_alternative<model::WorkflowArgumentDatePrecondition>(
                             precondition)) {
                         return true;
                     }
                     const auto* deadline =
                         std::get_if<model::WorkflowDeadlinePrecondition>(&precondition);
                     return deadline != nullptr &&
                            deadline->condition == model::WorkflowDeadlineCondition::Reached;
                 }) &&
             !operation.produced_deadline_id.has_value()) ||
            (operation.opcode == model::WorkflowOpcode::AdvanceStage &&
             !operation.next_stage_id.has_value())) {
            return fail(RuntimePackErrorCode::CrossReferenceFailure,
                        path + " has an incompatible workflow operation");
        }
        if (operation.deadline_base_id.has_value()) {
            deadline_base_ids.emplace(operation.deadline_base_id->value);
        }
    }
    for (const auto& operation : definition.operations) {
        if (!operation.deadline_event_base.has_value()) {
            continue;
        }
        const auto* order_base =
            std::get_if<model::WorkflowOrderOccurredDeadlineBase>(&*operation.deadline_event_base);
        if (order_base == nullptr) {
            continue;
        }
        const auto source_operation = operations_by_id.find(order_base->operation_id.value);
        if (source_operation == operations_by_id.end() ||
            source_operation->second->opcode != model::WorkflowOpcode::EnterOrder) {
            return fail(RuntimePackErrorCode::CrossReferenceFailure,
                        path + " has an order-occurrence base outside an EnterOrder operation");
        }
    }
    const auto operationForRoute = [&operations_by_id](const model::WorkflowOperationId& id,
                                                       model::WorkflowOpcode opcode,
                                                       const model::WorkflowStageId& stage) {
        const auto found = operations_by_id.find(id.value);
        return found != operations_by_id.end() && found->second->opcode == opcode &&
               found->second->stage_id == stage;
    };
    const auto independentDeadlineForRoute = [&operations_by_id](
                                                 const model::WorkflowOperationId& id,
                                                 const model::WorkflowStageId& stage) {
        const auto found = operations_by_id.find(id.value);
        return found != operations_by_id.end() &&
               found->second->opcode == model::WorkflowOpcode::CalculateDeadline &&
               found->second->stage_id == stage && !found->second->deadline_base_id.has_value() &&
               !found->second->produced_deadline_id.has_value() &&
               !found->second->deadline_event_base.has_value();
    };

    std::unordered_set<std::string> route_keys;
    std::unordered_set<std::string> declared_filing_types;
    auto deadline_ids = named_deadline_ids;
    auto exact_deadline_ids = named_deadline_ids;
    std::unordered_set<std::string> deficiency_deadline_prefixes;
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
        declared_filing_types.insert(route.filing_type.value);
        if (route.deficiency_operation_id.has_value() &&
            !operationForRoute(*route.deficiency_operation_id,
                               model::WorkflowOpcode::IssueDeficiency, route.stage_id)) {
            return fail(RuntimePackErrorCode::CrossReferenceFailure,
                        path + " has an incompatible deficiency operation");
        }
        if (route.deficiency_deadline.has_value()) {
            const auto& prefix = route.deficiency_deadline->deadline_id.value;
            if (!route.deficiency_operation_id.has_value() ||
                !deadline_ids.emplace(route.deficiency_deadline->deadline_id.value).second ||
                std::ranges::any_of(
                    exact_deadline_ids,
                    [&](const auto& exact) { return deadlineNamespacesOverlap(prefix, exact); }) ||
                std::ranges::any_of(deficiency_deadline_prefixes,
                                    [&](const auto& other_prefix) {
                                        return deadlineNamespacesOverlap(prefix, other_prefix);
                                    }) ||
                !deficiency_deadline_prefixes.emplace(prefix).second ||
                !independentDeadlineForRoute(route.deficiency_deadline->operation_id,
                                             route.stage_id)) {
                return fail(RuntimePackErrorCode::CrossReferenceFailure,
                            path + " has an incompatible deficiency deadline");
            }
        }
        if (route.accepted_deadline.has_value()) {
            const auto& deadline_id = route.accepted_deadline->deadline_id.value;
            if (!deadline_ids.emplace(deadline_id).second ||
                std::ranges::any_of(deficiency_deadline_prefixes,
                                    [&](const auto& prefix) {
                                        return deadlineNamespacesOverlap(prefix, deadline_id);
                                    }) ||
                !exact_deadline_ids.emplace(route.accepted_deadline->deadline_id.value).second ||
                !independentDeadlineForRoute(route.accepted_deadline->operation_id,
                                             route.stage_id)) {
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
            !exact_deadline_ids.contains(route.satisfies_deadline_id->value)) {
            return fail(RuntimePackErrorCode::CrossReferenceFailure,
                        path + " satisfies a deadline that this workflow does not produce");
        }
    }
    if (!std::ranges::all_of(deadline_base_ids, [&](const std::string& deadline_id) {
            return exact_deadline_ids.contains(deadline_id);
        })) {
        return fail(RuntimePackErrorCode::CrossReferenceFailure,
                    path + " has a dependent deadline without an exact produced base");
    }
    for (const auto& operation : definition.operations) {
        for (const auto& precondition : operation.preconditions) {
            const auto* filing = std::get_if<model::WorkflowFilingPrecondition>(&precondition);
            if (filing != nullptr && !declared_filing_types.contains(filing->filing_type.value)) {
                return fail(RuntimePackErrorCode::CrossReferenceFailure,
                            path + " has a filing precondition outside its filing routes");
            }
            const auto* deadline = std::get_if<model::WorkflowDeadlinePrecondition>(&precondition);
            if (deadline != nullptr && !exact_deadline_ids.contains(deadline->deadline_id.value)) {
                return fail(RuntimePackErrorCode::CrossReferenceFailure,
                            path + " has a deadline precondition without an exact producer");
            }
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

[[nodiscard]] Result<ParsedCase> parseCase(const ValidatedResource& resource,
                                           const ResourceIndex& resource_index) {
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
    std::unordered_map<std::string, std::unordered_set<std::string>> targets_by_issue;
    std::unordered_map<std::string, std::unordered_set<std::string>> authorities_by_issue;
    std::unordered_map<std::string, std::unordered_set<std::string>> anchors_by_issue;
    std::unordered_set<std::string> all_target_ids;
    std::vector<model::DispositionTarget> disposition_targets;
    const auto uses_structured_disposition =
        resource.document.contains(QStringLiteral("disposition_plans")) ||
        resource.document.contains(QStringLiteral("authored_disposition_plan_id")) ||
        std::ranges::any_of(*issue_values, [](const QJsonValue& value) {
            return value.toObject().contains(QStringLiteral("target_ids"));
        });
    if (uses_structured_disposition && resource.descriptor.schema_version != 2) {
        return fail(RuntimePackErrorCode::InvalidResource,
                    path + " uses structured disposition fields outside schema 2");
    }
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
        if (issue.contains(QStringLiteral("target_ids"))) {
            const auto target_ids =
                idArray(issue, "target_ids", issue_path, 1, maximum_disposition_targets);
            if (!target_ids) {
                return std::unexpected(target_ids.error());
            }
            if (disposition_targets.size() + target_ids->size() >
                static_cast<std::size_t>(maximum_disposition_targets)) {
                return fail(RuntimePackErrorCode::InvalidResource,
                            path + " exceeds the disposition target bound");
            }
            auto& issue_targets = targets_by_issue[*id];
            for (const auto& target_id : *target_ids) {
                if (!all_target_ids.emplace(target_id).second) {
                    return fail(RuntimePackErrorCode::InvalidResource,
                                path + " repeats a disposition target identifier");
                }
                issue_targets.insert(target_id);
                disposition_targets.push_back(model::DispositionTarget{
                    model::CaseIssueId{*id}, model::DispositionTargetId{target_id}});
            }
        }
        authorities_by_issue.emplace(
            *id, std::unordered_set<std::string>(authorities->begin(), authorities->end()));
        anchors_by_issue.emplace(*id,
                                 std::unordered_set<std::string>(anchors->begin(), anchors->end()));
        std::vector<model::AuthorityId> authority_ids;
        std::vector<model::AuthorityRef> authority_references;
        authority_ids.reserve(authorities->size());
        authority_references.reserve(authorities->size());
        for (const auto& authority : *authorities) {
            auto canonical = resource_index.requireAuthority(authority, issue_path);
            if (!canonical) {
                return std::unexpected(canonical.error());
            }
            authority_ids.push_back(model::AuthorityId{authority});
            authority_references.push_back(std::move(*canonical));
        }
        std::vector<RuntimeRecordAnchorId> anchor_ids;
        for (auto& anchor : *anchors) {
            anchor_ids.push_back(RuntimeRecordAnchorId{std::move(anchor)});
        }
        issues.push_back(RuntimeIssue{RuntimeIssueId{*id}, *issue_title, std::move(authority_ids),
                                      std::move(authority_references), std::move(anchor_ids)});
    }

    std::vector<model::DispositionPlan> disposition_plans;
    std::optional<model::DispositionPlanId> authored_disposition_plan_id;
    std::optional<model::WorkflowOperationId> authored_disposition_operation_id;
    if (uses_structured_disposition) {
        if (targets_by_issue.size() != issue_ids.size() ||
            !resource.document.contains(QStringLiteral("disposition_plans")) ||
            !resource.document.contains(QStringLiteral("authored_disposition_plan_id"))) {
            return fail(RuntimePackErrorCode::InvalidResource,
                        path + " has an incomplete structured disposition contract");
        }
        const auto plan_values = requiredArray(resource.document, "disposition_plans", path, 1,
                                               maximum_disposition_plans);
        const auto authored_plan =
            requiredId(resource.document, "authored_disposition_plan_id", path);
        if (!plan_values || !authored_plan) {
            return std::unexpected(!plan_values ? plan_values.error() : authored_plan.error());
        }
        disposition_plans.reserve(static_cast<std::size_t>(plan_values->size()));
        std::unordered_set<std::string> plan_ids;
        for (qsizetype plan_index = 0; plan_index < plan_values->size(); ++plan_index) {
            if (!plan_values->at(plan_index).isObject()) {
                return fail(RuntimePackErrorCode::InvalidResource,
                            path + ".disposition_plans must contain objects");
            }
            const auto plan_object = plan_values->at(plan_index).toObject();
            const auto plan_path = path + ".disposition_plans[" + std::to_string(plan_index) + "]";
            if (!hasExactKeys(plan_object, {"plan_id", "finality", "digest", "components"})) {
                return fail(RuntimePackErrorCode::InvalidResource,
                            plan_path + " must use the closed disposition plan shape");
            }
            const auto plan_id = requiredId(plan_object, "plan_id", plan_path);
            const auto finality_text = requiredString(plan_object, "finality", plan_path);
            const auto digest = requiredSha256(plan_object, "digest", plan_path);
            const auto component_values = requiredArray(plan_object, "components", plan_path, 1,
                                                        maximum_disposition_components);
            if (!plan_id || !finality_text || !digest || !component_values) {
                if (!plan_id) {
                    return std::unexpected(plan_id.error());
                }
                if (!finality_text) {
                    return std::unexpected(finality_text.error());
                }
                if (!digest) {
                    return std::unexpected(digest.error());
                }
                return std::unexpected(component_values.error());
            }
            if (!plan_ids.emplace(*plan_id).second) {
                return fail(RuntimePackErrorCode::InvalidResource,
                            plan_path + " repeats a disposition plan identifier");
            }
            model::DispositionFinality finality;
            if (*finality_text == "final") {
                finality = model::DispositionFinality::Final;
            } else if (*finality_text == "nonfinal") {
                finality = model::DispositionFinality::Nonfinal;
            } else {
                return fail(RuntimePackErrorCode::InvalidResource,
                            plan_path + ".finality has an unsupported value");
            }

            std::vector<model::DispositionComponent> components;
            components.reserve(static_cast<std::size_t>(component_values->size()));
            std::unordered_set<std::string> covered_targets;
            for (qsizetype component_index = 0; component_index < component_values->size();
                 ++component_index) {
                if (!component_values->at(component_index).isObject()) {
                    return fail(RuntimePackErrorCode::InvalidResource,
                                plan_path + ".components must contain objects");
                }
                const auto component = component_values->at(component_index).toObject();
                const auto component_path =
                    plan_path + ".components[" + std::to_string(component_index) + "]";
                if (!hasExactKeys(component, {"issue_id", "target_id", "scope", "action", "remand",
                                              "authority_ids", "record_anchor_ids"})) {
                    return fail(RuntimePackErrorCode::InvalidResource,
                                component_path +
                                    " must use the closed disposition component shape");
                }
                const auto issue_id = requiredId(component, "issue_id", component_path);
                const auto target_id = requiredId(component, "target_id", component_path);
                const auto scope_text = requiredString(component, "scope", component_path);
                const auto action_text = requiredString(component, "action", component_path);
                const auto remand = requiredBoolean(component, "remand", component_path);
                const auto authority_ids = idArray(component, "authority_ids", component_path, 1,
                                                   maximum_component_authorities);
                const auto record_anchor_ids =
                    idArray(component, "record_anchor_ids", component_path, 1,
                            maximum_component_record_anchors);
                if (!issue_id || !target_id || !scope_text || !action_text || !remand ||
                    !authority_ids || !record_anchor_ids) {
                    if (!issue_id) {
                        return std::unexpected(issue_id.error());
                    }
                    if (!target_id) {
                        return std::unexpected(target_id.error());
                    }
                    if (!scope_text) {
                        return std::unexpected(scope_text.error());
                    }
                    if (!action_text) {
                        return std::unexpected(action_text.error());
                    }
                    if (!remand) {
                        return std::unexpected(remand.error());
                    }
                    if (!authority_ids) {
                        return std::unexpected(authority_ids.error());
                    }
                    return std::unexpected(record_anchor_ids.error());
                }
                const auto target_key = *issue_id + '\n' + *target_id;
                const auto target_issue = targets_by_issue.find(*issue_id);
                if (target_issue == targets_by_issue.end() ||
                    !target_issue->second.contains(*target_id) ||
                    !covered_targets.emplace(target_key).second) {
                    return fail(RuntimePackErrorCode::CrossReferenceFailure,
                                component_path + " has an unresolved or overlapping target");
                }

                model::DispositionScope scope;
                if (*scope_text == "whole") {
                    scope = model::DispositionScope::Whole;
                } else if (*scope_text == "part") {
                    scope = model::DispositionScope::Part;
                } else {
                    return fail(RuntimePackErrorCode::InvalidResource,
                                component_path + ".scope has an unsupported value");
                }
                static const std::unordered_map<std::string, model::DispositionAction> actions{
                    {"affirm", model::DispositionAction::Affirm},
                    {"reverse", model::DispositionAction::Reverse},
                    {"vacate", model::DispositionAction::Vacate},
                    {"dismiss", model::DispositionAction::Dismiss},
                    {"grant", model::DispositionAction::Grant},
                    {"deny", model::DispositionAction::Deny},
                };
                const auto action = actions.find(*action_text);
                if (action == actions.end()) {
                    return fail(RuntimePackErrorCode::InvalidResource,
                                component_path + ".action has an unsupported value");
                }
                const auto supports_remand = action->second == model::DispositionAction::Reverse ||
                                             action->second == model::DispositionAction::Vacate ||
                                             action->second == model::DispositionAction::Dismiss ||
                                             action->second == model::DispositionAction::Grant;
                if (*remand && !supports_remand) {
                    return fail(RuntimePackErrorCode::InvalidResource,
                                component_path + " uses remand with an incompatible action");
                }

                const auto issue_authorities = authorities_by_issue.find(*issue_id);
                const auto issue_anchors = anchors_by_issue.find(*issue_id);
                if (issue_authorities == authorities_by_issue.end() ||
                    issue_anchors == anchors_by_issue.end() ||
                    std::ranges::any_of(*authority_ids,
                                        [&](const std::string& authority_id) {
                                            return !issue_authorities->second.contains(
                                                authority_id);
                                        }) ||
                    std::ranges::any_of(*record_anchor_ids, [&](const std::string& anchor_id) {
                        return !issue_anchors->second.contains(anchor_id);
                    })) {
                    return fail(RuntimePackErrorCode::CrossReferenceFailure,
                                component_path + " is grounded outside its issue");
                }
                std::vector<model::AuthorityId> component_authorities;
                component_authorities.reserve(authority_ids->size());
                for (const auto& authority_id : *authority_ids) {
                    component_authorities.push_back(model::AuthorityId{authority_id});
                }
                std::vector<model::RecordAnchorId> component_anchors;
                component_anchors.reserve(record_anchor_ids->size());
                for (const auto& anchor_id : *record_anchor_ids) {
                    component_anchors.push_back(model::RecordAnchorId{anchor_id});
                }
                components.push_back(model::DispositionComponent{
                    model::CaseIssueId{*issue_id}, model::DispositionTargetId{*target_id}, scope,
                    action->second, *remand, std::move(component_authorities),
                    std::move(component_anchors)});
            }
            model::DispositionPlan plan{model::DispositionPlanId{*plan_id}, finality, *digest,
                                        std::move(components)};
            if (canonicalDispositionPlanDigest(resource.descriptor.id, *disposition, plan) !=
                plan.canonical_sha256) {
                return fail(RuntimePackErrorCode::InvalidResource,
                            plan_path + ".digest does not match its canonical disposition bytes");
            }
            disposition_plans.push_back(std::move(plan));
        }
        if (!plan_ids.contains(*authored_plan)) {
            return fail(RuntimePackErrorCode::CrossReferenceFailure,
                        path + ".authored_disposition_plan_id is not declared");
        }
        authored_disposition_plan_id = model::DispositionPlanId{*authored_plan};
        authored_disposition_operation_id = model::WorkflowOperationId{*disposition};
    }

    return ParsedCase{
        model::CaseDefinition{model::CaseId{resource.descriptor.id}, model::ProcedureId{*procedure},
                              std::move(actors), std::move(disposition_targets),
                              std::move(disposition_plans), std::move(authored_disposition_plan_id),
                              std::move(authored_disposition_operation_id)},
        *title, RuntimeRecordId{*record}, std::move(issues),
        model::WorkflowOperationId{*disposition}};
}

[[nodiscard]] Result<RuntimeRecord> parseRecord(const ValidatedResource& resource,
                                                const ResourceIndex& resource_index) {
    const auto path = "record " + resource.descriptor.id;
    const auto caption = requiredString(resource.document, "caption", path);
    QJsonArray docket_descriptors;
    if (resource.document.contains(QStringLiteral("dockets"))) {
        const auto values = requiredArray(resource.document, "dockets", path, 1, 64);
        if (!values) {
            return std::unexpected(values.error());
        }
        docket_descriptors = *values;
    }
    const auto entries =
        requiredArray(resource.document, "docket_entries", path, 1, maximum_case_items);
    QJsonArray anchor_values;
    if (resource.document.contains(QStringLiteral("page_anchors"))) {
        const auto values = requiredArray(resource.document, "page_anchors", path, 0, 32'768);
        if (!values) {
            return std::unexpected(values.error());
        }
        anchor_values = *values;
    }
    if (!caption || !isBoundedUtf8Text(*caption, 512)) {
        if (!caption) {
            return std::unexpected(caption.error());
        }
        return fail(RuntimePackErrorCode::InvalidResource,
                    path + ".caption exceeds its supported bound");
    }
    if (!entries) {
        return std::unexpected(entries.error());
    }

    std::vector<RuntimeDocketDescriptor> runtime_dockets;
    runtime_dockets.reserve(static_cast<std::size_t>(docket_descriptors.size()));
    std::unordered_set<std::string> docket_ids;
    for (qsizetype index = 0; index < docket_descriptors.size(); ++index) {
        if (!docket_descriptors.at(index).isObject()) {
            return fail(RuntimePackErrorCode::InvalidResource,
                        path + ".dockets must contain objects");
        }
        const auto descriptor = docket_descriptors.at(index).toObject();
        const auto descriptor_path = path + ".dockets[" + std::to_string(index) + "]";
        const auto id = requiredId(descriptor, "docket_id", descriptor_path);
        const auto type_text = requiredString(descriptor, "docket_type", descriptor_path);
        const auto court_id = optionalId(descriptor, "court_id", descriptor_path);
        const auto court_ref = requiredString(descriptor, "court_ref", descriptor_path);
        const auto public_number =
            requiredString(descriptor, "public_docket_number", descriptor_path);
        const auto docket_caption = requiredString(descriptor, "caption", descriptor_path);
        if (!id || !type_text || !court_id || !court_ref || !public_number || !docket_caption) {
            if (!id) {
                return std::unexpected(id.error());
            }
            if (!type_text) {
                return std::unexpected(type_text.error());
            }
            if (!court_id) {
                return std::unexpected(court_id.error());
            }
            if (!court_ref) {
                return std::unexpected(court_ref.error());
            }
            if (!public_number) {
                return std::unexpected(public_number.error());
            }
            return std::unexpected(docket_caption.error());
        }
        RuntimeDocketType type{};
        if (*type_text == "district") {
            type = RuntimeDocketType::District;
        } else if (*type_text == "appellate") {
            type = RuntimeDocketType::Appellate;
        } else if (*type_text == "agency") {
            type = RuntimeDocketType::Agency;
        } else if (*type_text == "original") {
            type = RuntimeDocketType::Original;
        } else {
            return fail(RuntimePackErrorCode::InvalidResource,
                        descriptor_path + ".docket_type is unsupported");
        }
        if (!docket_ids.emplace(*id).second || !isBoundedUtf8Text(*court_ref, 240) ||
            !isBoundedUtf8Text(*public_number, 120) || !isBoundedUtf8Text(*docket_caption, 512)) {
            return fail(RuntimePackErrorCode::InvalidResource,
                        descriptor_path + " is duplicate or exceeds its text bounds");
        }
        std::optional<RuntimeCourtId> typed_court;
        if (court_id->has_value()) {
            typed_court = RuntimeCourtId{**court_id};
        }
        runtime_dockets.push_back(RuntimeDocketDescriptor{RuntimeDocketId{*id}, type,
                                                          std::move(typed_court), *court_ref,
                                                          *public_number, *docket_caption});
    }

    std::vector<RuntimeDocketEntry> docket;
    docket.reserve(static_cast<std::size_t>(entries->size()));
    std::unordered_set<std::string> ids;
    std::unordered_set<std::uint32_t> numbers;
    std::unordered_set<std::string> display_labels;
    std::unordered_map<std::string, std::size_t> entry_indexes;
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
        const auto docket_id = optionalId(entry, "docket_id", entry_path);
        const auto entry_label = optionalString(entry, "entry_label", entry_path, 120);
        const auto filed_on = requiredDate(entry, "filed_on", entry_path);
        const auto title = requiredString(entry, "title", entry_path);
        const auto actor = optionalString(entry, "actor", entry_path, 240);
        const auto description = optionalString(entry, "description", entry_path, 4'096);
        const auto tags = optionalStringArray(entry, "tags", entry_path, 32, 64);
        const auto parent_id = optionalId(entry, "parent_entry_id", entry_path);
        const auto relationship_text = optionalString(entry, "relationship", entry_path, 16);
        const auto asset_path = requiredPortablePath(entry, "asset_path", entry_path);
        const auto digest = requiredSha256(entry, "asset_sha256", entry_path);
        const auto pages = requiredUnsigned(entry, "page_count", entry_path, 1, 10'000);
        const auto sealed = requiredBoolean(entry, "sealed", entry_path);
        if (!id || !number || !docket_id || !entry_label || !filed_on || !title || !actor ||
            !description || !tags || !parent_id || !relationship_text || !asset_path || !digest ||
            !pages || !sealed) {
            if (!id) {
                return std::unexpected(id.error());
            }
            if (!number) {
                return std::unexpected(number.error());
            }
            if (!docket_id) {
                return std::unexpected(docket_id.error());
            }
            if (!entry_label) {
                return std::unexpected(entry_label.error());
            }
            if (!filed_on) {
                return std::unexpected(filed_on.error());
            }
            if (!title) {
                return std::unexpected(title.error());
            }
            if (!actor) {
                return std::unexpected(actor.error());
            }
            if (!description) {
                return std::unexpected(description.error());
            }
            if (!tags) {
                return std::unexpected(tags.error());
            }
            if (!parent_id) {
                return std::unexpected(parent_id.error());
            }
            if (!relationship_text) {
                return std::unexpected(relationship_text.error());
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
        if (!ids.emplace(*id).second || !numbers.emplace(*number).second ||
            !isBoundedUtf8Text(*title, 512)) {
            return fail(RuntimePackErrorCode::InvalidResource,
                        entry_path + " repeats an entry id/number or exceeds its title bound");
        }
        if (docket_id->has_value() && !docket_ids.contains(**docket_id)) {
            return fail(RuntimePackErrorCode::CrossReferenceFailure,
                        entry_path + ".docket_id is not declared by the record");
        }
        if (entry_label->has_value()) {
            const auto label_key =
                (docket_id->has_value() ? **docket_id : std::string{}) + "\n" + **entry_label;
            if (!display_labels.emplace(label_key).second) {
                return fail(RuntimePackErrorCode::InvalidResource,
                            entry_path + ".entry_label is duplicated within its docket");
            }
        }
        if (parent_id->has_value() != relationship_text->has_value()) {
            return fail(RuntimePackErrorCode::InvalidResource,
                        entry_path + " must declare parent_entry_id and relationship together");
        }
        std::optional<RuntimeRecordEntryRelationship> relationship;
        if (relationship_text->has_value()) {
            if (**relationship_text == "attachment") {
                relationship = RuntimeRecordEntryRelationship::Attachment;
            } else if (**relationship_text == "amendment") {
                relationship = RuntimeRecordEntryRelationship::Amendment;
            } else if (**relationship_text == "supplement") {
                relationship = RuntimeRecordEntryRelationship::Supplement;
            } else if (**relationship_text == "component") {
                relationship = RuntimeRecordEntryRelationship::Component;
            } else {
                return fail(RuntimePackErrorCode::InvalidResource,
                            entry_path + ".relationship is unsupported");
            }
        }
        std::optional<RuntimeDocketId> typed_docket;
        if (docket_id->has_value()) {
            typed_docket = RuntimeDocketId{**docket_id};
        }
        std::optional<RuntimeRecordEntryId> typed_parent;
        if (parent_id->has_value()) {
            typed_parent = RuntimeRecordEntryId{**parent_id};
        }
        entry_indexes.emplace(*id, docket.size());
        docket.push_back(RuntimeDocketEntry{
            RuntimeRecordEntryId{*id}, *number, *filed_on, *title, *asset_path, *digest, *pages,
            *sealed, std::move(typed_docket), std::move(*entry_label), std::move(*actor),
            std::move(*description), std::move(*tags), std::move(typed_parent), relationship});
    }

    std::unordered_map<std::string, std::string> parents;
    for (const auto& entry : docket) {
        if (!entry.parent_entry_id.has_value()) {
            continue;
        }
        const auto parent = entry_indexes.find(entry.parent_entry_id->value);
        if (parent == entry_indexes.end() || entry.parent_entry_id->value == entry.id.value) {
            return fail(RuntimePackErrorCode::CrossReferenceFailure,
                        path + " has an orphaned or self-referential parent entry");
        }
        const auto& parent_entry = docket[parent->second];
        if (entry.docket_id != parent_entry.docket_id) {
            return fail(RuntimePackErrorCode::CrossReferenceFailure,
                        path + " links parent and child entries across dockets");
        }
        parents.emplace(entry.id.value, entry.parent_entry_id->value);
    }
    std::unordered_set<std::string> resolved_parent_chains;
    for (const auto& entry : docket) {
        std::unordered_set<std::string> chain;
        auto current = entry.id.value;
        while (parents.contains(current) && !resolved_parent_chains.contains(current)) {
            if (!chain.emplace(current).second) {
                return fail(RuntimePackErrorCode::CrossReferenceFailure,
                            path + " contains a docket-entry parent cycle");
            }
            current = parents.at(current);
        }
        resolved_parent_chains.insert(chain.begin(), chain.end());
    }

    std::vector<RuntimeRecordPageAnchor> anchors;
    anchors.reserve(static_cast<std::size_t>(anchor_values.size()));
    std::unordered_set<std::string> anchor_ids;
    std::unordered_map<std::string, std::size_t> anchor_indexes;
    std::unordered_set<std::string> citation_labels;
    for (qsizetype index = 0; index < anchor_values.size(); ++index) {
        if (!anchor_values.at(index).isObject()) {
            return fail(RuntimePackErrorCode::InvalidResource,
                        path + ".page_anchors must contain objects");
        }
        const auto anchor = anchor_values.at(index).toObject();
        const auto anchor_path = path + ".page_anchors[" + std::to_string(index) + "]";
        const auto id = requiredId(anchor, "anchor_id", anchor_path);
        const auto entry_id = requiredId(anchor, "entry_id", anchor_path);
        const auto page_number = requiredUnsigned(anchor, "page_number", anchor_path, 1, 10'000);
        const auto citation = optionalString(anchor, "citation_label", anchor_path, 120);
        if (!id || !entry_id || !page_number || !citation) {
            if (!id) {
                return std::unexpected(id.error());
            }
            if (!entry_id) {
                return std::unexpected(entry_id.error());
            }
            if (!page_number) {
                return std::unexpected(page_number.error());
            }
            return std::unexpected(citation.error());
        }
        const auto entry = entry_indexes.find(*entry_id);
        if (!anchor_ids.emplace(*id).second || ids.contains(*id) || entry == entry_indexes.end() ||
            *page_number > docket[entry->second].page_count) {
            return fail(RuntimePackErrorCode::CrossReferenceFailure,
                        anchor_path +
                            " is duplicate, ambiguous, orphaned, or outside the declared PDF");
        }
        if (citation->has_value() && (!model::isCanonicalAuthorityText(**citation, 120) ||
                                      !citation_labels.emplace(**citation).second)) {
            return fail(RuntimePackErrorCode::InvalidResource,
                        anchor_path + ".citation_label is noncanonical or duplicated");
        }
        anchor_indexes.emplace(*id, anchors.size());
        anchors.push_back(RuntimeRecordPageAnchor{RuntimeRecordPageAnchorId{*id},
                                                  RuntimeRecordEntryId{*entry_id}, *page_number,
                                                  std::move(*citation)});
    }

    const auto has_policy = resource.document.contains(QStringLiteral("disclosure_policy"));
    const auto has_disclosures = resource.document.contains(QStringLiteral("sealed_disclosures"));
    if (has_policy != has_disclosures) {
        return fail(RuntimePackErrorCode::InvalidResource,
                    path + " must declare disclosure_policy and sealed_disclosures together");
    }

    std::optional<RuntimeRecordDisclosurePolicy> disclosure_policy;
    std::vector<RuntimeSealedDisclosure> sealed_disclosures;
    if (has_policy) {
        const auto policy_value = resource.document.value(QStringLiteral("disclosure_policy"));
        if (!policy_value.isObject()) {
            return fail(RuntimePackErrorCode::InvalidResource,
                        path + ".disclosure_policy must be an object");
        }
        const auto policy = policy_value.toObject();
        const auto policy_id = requiredId(policy, "policy_id", path + ".disclosure_policy");
        const auto unauthorized =
            requiredString(policy, "unauthorized_projection", path + ".disclosure_policy");
        const auto authorized =
            requiredString(policy, "authorized_projection", path + ".disclosure_policy");
        const auto asset_access =
            requiredString(policy, "sealed_asset_access", path + ".disclosure_policy");
        if (!hasExactKeys(policy, {"policy_id", "unauthorized_projection", "authorized_projection",
                                   "sealed_asset_access"}) ||
            !policy_id || !unauthorized || !authorized || !asset_access) {
            return fail(RuntimePackErrorCode::InvalidResource,
                        path + ".disclosure_policy has an invalid closed shape");
        }
        if (*unauthorized != "public_counterparts_only" ||
            *authorized != "public_and_authorized_sealed" ||
            *asset_access != "session_event_grant_required") {
            return fail(RuntimePackErrorCode::InvalidResource,
                        path + ".disclosure_policy has unsupported projection semantics");
        }
        disclosure_policy =
            RuntimeRecordDisclosurePolicy{*policy_id, *unauthorized, *authorized, *asset_access};

        const auto disclosure_values =
            requiredArray(resource.document, "sealed_disclosures", path, 1, maximum_case_items);
        if (!disclosure_values) {
            return std::unexpected(disclosure_values.error());
        }
        sealed_disclosures.reserve(static_cast<std::size_t>(disclosure_values->size()));
        std::unordered_set<std::string> disclosure_ids;
        std::unordered_set<std::string> disclosed_sealed_entries;
        std::unordered_set<std::string> public_counterparts;
        std::unordered_set<std::string> stable_anchor_ids;
        std::unordered_set<std::string> mapped_sealed_anchors;
        std::unordered_set<std::string> mapped_public_anchors;
        for (qsizetype index = 0; index < disclosure_values->size(); ++index) {
            if (!disclosure_values->at(index).isObject()) {
                return fail(RuntimePackErrorCode::InvalidResource,
                            path + ".sealed_disclosures must contain objects");
            }
            const auto disclosure = disclosure_values->at(index).toObject();
            const auto disclosure_path =
                path + ".sealed_disclosures[" + std::to_string(index) + "]";
            static const QSet<QString> allowed_disclosure_keys{
                QStringLiteral("disclosure_id"),
                QStringLiteral("sealed_entry_id"),
                QStringLiteral("public_entry_id"),
                QStringLiteral("motion_entry_id"),
                QStringLiteral("certificate_entry_id"),
                QStringLiteral("authorization_authority_id"),
                QStringLiteral("required_items"),
                QStringLiteral("anchor_mappings"),
            };
            if (std::ranges::any_of(disclosure.keys(), [](const QString& key) {
                    return !allowed_disclosure_keys.contains(key);
                })) {
                return fail(RuntimePackErrorCode::InvalidResource,
                            disclosure_path + " contains an unsupported field");
            }
            const auto disclosure_id = requiredId(disclosure, "disclosure_id", disclosure_path);
            const auto sealed_id = requiredId(disclosure, "sealed_entry_id", disclosure_path);
            const auto public_id = optionalId(disclosure, "public_entry_id", disclosure_path);
            const auto motion_id = optionalId(disclosure, "motion_entry_id", disclosure_path);
            const auto certificate_id =
                optionalId(disclosure, "certificate_entry_id", disclosure_path);
            const auto authority_id =
                requiredId(disclosure, "authorization_authority_id", disclosure_path);
            const auto requirements =
                requiredArray(disclosure, "required_items", disclosure_path, 0, 3);
            const auto mappings =
                requiredArray(disclosure, "anchor_mappings", disclosure_path, 0, 32'768);
            if (!disclosure_id || !sealed_id || !public_id || !motion_id || !certificate_id ||
                !authority_id || !requirements || !mappings) {
                return fail(RuntimePackErrorCode::InvalidResource,
                            disclosure_path + " is incomplete or invalid");
            }
            const auto canonical_authority =
                resource_index.requireAuthority(*authority_id, disclosure_path);
            if (!canonical_authority || !canonical_authority->provenance.has_value()) {
                return fail(RuntimePackErrorCode::CrossReferenceFailure,
                            disclosure_path +
                                " authorization authority is missing canonical provenance");
            }
            const auto sealed_entry = entry_indexes.find(*sealed_id);
            if (!disclosure_ids.emplace(*disclosure_id).second || ids.contains(*disclosure_id) ||
                docket_ids.contains(*disclosure_id) || anchor_ids.contains(*disclosure_id) ||
                stable_anchor_ids.contains(*disclosure_id) || sealed_entry == entry_indexes.end() ||
                !docket[sealed_entry->second].sealed ||
                !disclosed_sealed_entries.emplace(*sealed_id).second) {
                return fail(RuntimePackErrorCode::CrossReferenceFailure,
                            disclosure_path +
                                " has a duplicate, ambiguous, missing, or public sealed target");
            }

            const auto valid_public_entry = [&](const std::optional<std::string>& entry_id) {
                if (!entry_id.has_value()) {
                    return true;
                }
                const auto found = entry_indexes.find(*entry_id);
                return found != entry_indexes.end() && !docket[found->second].sealed &&
                       *entry_id != *sealed_id;
            };
            if (!valid_public_entry(*public_id) || !valid_public_entry(*motion_id) ||
                !valid_public_entry(*certificate_id)) {
                return fail(RuntimePackErrorCode::CrossReferenceFailure,
                            disclosure_path +
                                " public counterpart/support entries must resolve as public");
            }
            if (public_id->has_value()) {
                const auto public_entry = entry_indexes.at(**public_id);
                if (!public_counterparts.emplace(**public_id).second ||
                    docket[public_entry].docket_id != docket[sealed_entry->second].docket_id) {
                    return fail(RuntimePackErrorCode::CrossReferenceFailure,
                                disclosure_path +
                                    " public counterpart must be one-to-one in the same docket");
                }
            }
            std::unordered_set<std::string> support_ids;
            for (const auto* candidate : {&*public_id, &*motion_id, &*certificate_id}) {
                if (candidate->has_value() && !support_ids.emplace(**candidate).second) {
                    return fail(RuntimePackErrorCode::CrossReferenceFailure,
                                disclosure_path + " support entries must be distinct");
                }
            }

            std::vector<RuntimeDisclosureRequirement> runtime_requirements;
            runtime_requirements.reserve(static_cast<std::size_t>(requirements->size()));
            std::unordered_set<std::string> requirement_names;
            for (const auto& value : *requirements) {
                if (!value.isString()) {
                    return fail(RuntimePackErrorCode::InvalidResource,
                                disclosure_path + ".required_items must contain strings");
                }
                const auto name = utf8(value.toString());
                if (!requirement_names.emplace(name).second) {
                    return fail(RuntimePackErrorCode::InvalidResource,
                                disclosure_path + ".required_items contains duplicates");
                }
                if (name == "motion") {
                    runtime_requirements.push_back(RuntimeDisclosureRequirement::Motion);
                } else if (name == "certificate") {
                    runtime_requirements.push_back(RuntimeDisclosureRequirement::Certificate);
                } else if (name == "redacted_counterpart") {
                    runtime_requirements.push_back(
                        RuntimeDisclosureRequirement::RedactedCounterpart);
                } else {
                    return fail(RuntimePackErrorCode::InvalidResource,
                                disclosure_path + ".required_items contains an unsupported kind");
                }
            }

            if (!public_id->has_value() && !mappings->isEmpty()) {
                return fail(RuntimePackErrorCode::CrossReferenceFailure,
                            disclosure_path +
                                " stable anchor mappings require a public counterpart");
            }
            std::vector<RuntimeRecordTwinAnchor> runtime_mappings;
            runtime_mappings.reserve(static_cast<std::size_t>(mappings->size()));
            for (qsizetype mapping_index = 0; mapping_index < mappings->size(); ++mapping_index) {
                if (!mappings->at(mapping_index).isObject()) {
                    return fail(RuntimePackErrorCode::InvalidResource,
                                disclosure_path + ".anchor_mappings must contain objects");
                }
                const auto mapping = mappings->at(mapping_index).toObject();
                const auto mapping_path =
                    disclosure_path + ".anchor_mappings[" + std::to_string(mapping_index) + "]";
                const auto stable_id = requiredId(mapping, "stable_anchor_id", mapping_path);
                const auto sealed_anchor_id = requiredId(mapping, "sealed_anchor_id", mapping_path);
                const auto public_anchor_id = requiredId(mapping, "public_anchor_id", mapping_path);
                if (!hasExactKeys(mapping,
                                  {"stable_anchor_id", "sealed_anchor_id", "public_anchor_id"}) ||
                    !stable_id || !sealed_anchor_id || !public_anchor_id) {
                    return fail(RuntimePackErrorCode::InvalidResource,
                                mapping_path + " has an invalid closed shape");
                }
                const auto sealed_anchor = anchor_indexes.find(*sealed_anchor_id);
                const auto public_anchor = anchor_indexes.find(*public_anchor_id);
                if (!stable_anchor_ids.emplace(*stable_id).second || ids.contains(*stable_id) ||
                    docket_ids.contains(*stable_id) || anchor_ids.contains(*stable_id) ||
                    disclosure_ids.contains(*stable_id) ||
                    !mapped_sealed_anchors.emplace(*sealed_anchor_id).second ||
                    !mapped_public_anchors.emplace(*public_anchor_id).second ||
                    sealed_anchor == anchor_indexes.end() ||
                    public_anchor == anchor_indexes.end() ||
                    anchors[sealed_anchor->second].entry_id.value != *sealed_id ||
                    anchors[public_anchor->second].entry_id.value != **public_id) {
                    return fail(RuntimePackErrorCode::CrossReferenceFailure,
                                mapping_path +
                                    " is duplicate, ambiguous, orphaned, or on the wrong twin");
                }
                runtime_mappings.push_back(
                    RuntimeRecordTwinAnchor{RuntimeRecordPageAnchorId{*stable_id},
                                            RuntimeRecordPageAnchorId{*sealed_anchor_id},
                                            RuntimeRecordPageAnchorId{*public_anchor_id}});
            }

            const auto typed_optional_entry =
                [](const std::optional<std::string>& value) -> std::optional<RuntimeRecordEntryId> {
                if (!value.has_value()) {
                    return std::nullopt;
                }
                return RuntimeRecordEntryId{*value};
            };
            sealed_disclosures.push_back(RuntimeSealedDisclosure{
                RuntimeRecordDisclosureId{*disclosure_id}, RuntimeRecordEntryId{*sealed_id},
                typed_optional_entry(*public_id), typed_optional_entry(*motion_id),
                typed_optional_entry(*certificate_id), model::AuthorityId{*authority_id},
                std::move(runtime_requirements), std::move(runtime_mappings)});
        }
        for (const auto& entry : docket) {
            if (!entry.sealed && entry.parent_entry_id.has_value() &&
                docket[entry_indexes.at(entry.parent_entry_id->value)].sealed) {
                return fail(RuntimePackErrorCode::CrossReferenceFailure,
                            path + " gives a public entry a sealed parent");
            }
        }
        for (const auto& entry : docket) {
            if (entry.sealed && !disclosed_sealed_entries.contains(entry.id.value)) {
                return fail(RuntimePackErrorCode::CrossReferenceFailure,
                            path + " leaves a sealed entry outside the disclosure policy");
            }
        }
    }
    return RuntimeRecord{RuntimeRecordId{resource.descriptor.id},
                         *caption,
                         std::move(runtime_dockets),
                         std::move(docket),
                         std::move(anchors),
                         std::move(disclosure_policy),
                         std::move(sealed_disclosures)};
}

struct RuntimeCatalogFiling final {
    std::unordered_set<std::string> authorized_roles;
    std::unordered_set<std::string> required_fields;
    model::AuthorityRef authority;
};

using RuntimeCatalog = std::unordered_map<std::string, RuntimeCatalogFiling>;

[[nodiscard]] Result<RuntimeCatalog> parseFilingCatalog(const ValidatedResource& resource,
                                                        const ResourceIndex& resource_index) {
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
        auto canonical = resource_index.requireAuthority(*authority, filing_path);
        if (!canonical) {
            return std::unexpected(canonical.error());
        }
        parsed.authority = std::move(*canonical);
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

[[nodiscard]] const RuntimeIssue* issueById(const std::vector<RuntimeIssue>& issues,
                                            const std::string& id) {
    const auto found =
        std::ranges::find(issues, id, [](const RuntimeIssue& issue) { return issue.id.value; });
    return found == issues.end() ? nullptr : &*found;
}

[[nodiscard]] Result<std::optional<model::AuthoredQuestionBank>>
parseQuestionBank(const ValidatedResource& resource, const ResourceIndex& index,
                  const ParsedCase& case_document, const RuntimeRecord& record,
                  const RuntimeBenchConfiguration& bench,
                  std::span<const std::string> permitted_issue_ids) {
    if (!resource.document.contains(QStringLiteral("grounded_question_bank"))) {
        return std::optional<model::AuthoredQuestionBank>{};
    }
    const auto path =
        "argument configuration " + resource.descriptor.id + ".grounded_question_bank";
    if (resource.descriptor.schema_version != 2) {
        return fail(RuntimePackErrorCode::InvalidResource,
                    path + " is unavailable outside resource schema 2");
    }
    const auto bank_result = requiredObject(resource.document, "grounded_question_bank", path);
    if (!bank_result) {
        return std::unexpected(bank_result.error());
    }
    const auto& bank = *bank_result;
    if (!hasExactKeys(bank, {"mode", "grounding_digest", "issue_topic_bindings", "questions"})) {
        return fail(RuntimePackErrorCode::InvalidResource, path + " has unknown or missing fields");
    }
    const auto mode_text = requiredString(bank, "mode", path);
    const auto asserted_digest = requiredSha256(bank, "grounding_digest", path);
    const auto binding_values =
        requiredArray(bank, "issue_topic_bindings", path, 1, maximum_argument_issue_bindings);
    const auto question_values =
        requiredArray(bank, "questions", path, 1, maximum_authored_questions);
    if (!mode_text || !asserted_digest || !binding_values || !question_values) {
        if (!mode_text) {
            return std::unexpected(mode_text.error());
        }
        if (!asserted_digest) {
            return std::unexpected(asserted_digest.error());
        }
        if (!binding_values) {
            return std::unexpected(binding_values.error());
        }
        return std::unexpected(question_values.error());
    }
    model::OralArgumentMode mode{};
    if (*mode_text == "actual_record") {
        mode = model::OralArgumentMode::ActualRecord;
    } else if (*mode_text == "counterfactual_training") {
        mode = model::OralArgumentMode::CounterfactualTraining;
    } else {
        return fail(RuntimePackErrorCode::InvalidResource, path + ".mode is unsupported");
    }

    std::unordered_set<std::string> permitted(permitted_issue_ids.begin(),
                                              permitted_issue_ids.end());
    std::unordered_map<std::string, std::unordered_set<std::string>> topics_by_issue;
    std::vector<model::ArgumentIssueTopics> issue_topics;
    issue_topics.reserve(static_cast<std::size_t>(binding_values->size()));
    for (qsizetype index_value = 0; index_value < binding_values->size(); ++index_value) {
        const auto binding_path =
            path + ".issue_topic_bindings[" + std::to_string(index_value) + "]";
        if (!binding_values->at(index_value).isObject()) {
            return fail(RuntimePackErrorCode::InvalidResource, binding_path + " must be an object");
        }
        const auto binding = binding_values->at(index_value).toObject();
        if (!hasExactKeys(binding, {"issue_id", "topic_ids"})) {
            return fail(RuntimePackErrorCode::InvalidResource,
                        binding_path + " has unknown or missing fields");
        }
        const auto issue_id = requiredId(binding, "issue_id", binding_path);
        const auto topic_values =
            requiredArray(binding, "topic_ids", binding_path, 1, maximum_argument_topics_per_issue);
        if (!issue_id || !topic_values) {
            return std::unexpected(!issue_id ? issue_id.error() : topic_values.error());
        }
        if (!permitted.contains(*issue_id) || topics_by_issue.contains(*issue_id)) {
            return fail(RuntimePackErrorCode::CrossReferenceFailure,
                        binding_path + " must uniquely identify a permitted issue");
        }
        std::unordered_set<std::string> topic_ids;
        std::vector<model::ArgumentFocusTopic> topics;
        topics.reserve(static_cast<std::size_t>(topic_values->size()));
        for (const auto& topic_value : *topic_values) {
            if (!topic_value.isString()) {
                return fail(RuntimePackErrorCode::InvalidResource,
                            binding_path + ".topic_ids must contain strings");
            }
            auto topic_id = utf8(topic_value.toString());
            const auto topic = model::argumentFocusTopicFromId(topic_id);
            if (!topic.has_value() || !topic_ids.emplace(topic_id).second) {
                return fail(RuntimePackErrorCode::InvalidResource,
                            binding_path + ".topic_ids contains an unsupported or duplicate "
                                           "focus topic");
            }
            topics.push_back(*topic);
        }
        std::ranges::sort(
            topics, [](model::ArgumentFocusTopic left, model::ArgumentFocusTopic right) {
                return model::argumentFocusTopicId(left) < model::argumentFocusTopicId(right);
            });
        topics_by_issue.emplace(*issue_id, std::move(topic_ids));
        issue_topics.push_back(model::ArgumentIssueTopics{*issue_id, std::move(topics)});
    }
    if (topics_by_issue.size() != permitted.size() ||
        std::ranges::any_of(permitted, [&topics_by_issue](const std::string& issue_id) {
            return !topics_by_issue.contains(issue_id);
        })) {
        return fail(RuntimePackErrorCode::CrossReferenceFailure,
                    path + ".issue_topic_bindings must exactly cover permitted_issue_ids");
    }
    std::ranges::sort(issue_topics, {}, &model::ArgumentIssueTopics::issue_id);

    std::unordered_set<std::string> bank_topic_ids;
    for (const auto& [issue_id, topic_ids] : topics_by_issue) {
        static_cast<void>(issue_id);
        bank_topic_ids.insert(topic_ids.begin(), topic_ids.end());
    }
    for (const auto& seat : bench.seats) {
        const auto has_invalid_focus = std::ranges::any_of(
            seat.profile.interaction.issue_focus, [](const model::IssueFocus& focus) {
                return !model::argumentFocusTopicFromId(focus.topic_id).has_value();
            });
        const auto has_positive_bank_focus = std::ranges::any_of(
            seat.profile.interaction.issue_focus,
            [&bank_topic_ids](const model::IssueFocus& focus) {
                return focus.weight > 0.0 && bank_topic_ids.contains(focus.topic_id);
            });
        if (has_invalid_focus || !has_positive_bank_focus) {
            return fail(RuntimePackErrorCode::CrossReferenceFailure,
                        path + " uses a bench profile without a positive canonical bank focus");
        }
    }

    std::unordered_set<std::string> question_ids;
    std::unordered_set<std::string> grounding_ids;
    std::unordered_set<std::string> covered_issue_topics;
    std::unordered_map<std::string, std::size_t> questions_per_issue;
    std::vector<model::AuthoredArgumentQuestion> questions;
    questions.reserve(static_cast<std::size_t>(question_values->size()));
    for (qsizetype index_value = 0; index_value < question_values->size(); ++index_value) {
        const auto question_path = path + ".questions[" + std::to_string(index_value) + "]";
        if (!question_values->at(index_value).isObject()) {
            return fail(RuntimePackErrorCode::InvalidResource,
                        question_path + " must be an object");
        }
        const auto question = question_values->at(index_value).toObject();
        if (!hasExactKeys(question,
                          {"question_id", "issue_id", "topic_id", "prompt", "grounding"})) {
            return fail(RuntimePackErrorCode::InvalidResource,
                        question_path + " has unknown or missing fields");
        }
        const auto question_id = requiredId(question, "question_id", question_path);
        const auto issue_id = requiredId(question, "issue_id", question_path);
        const auto topic_id = requiredString(question, "topic_id", question_path);
        const auto prompt = requiredString(question, "prompt", question_path);
        const auto grounding_values =
            requiredArray(question, "grounding", question_path, 1, maximum_question_grounding);
        if (!question_id || !issue_id || !topic_id || !prompt || !grounding_values) {
            if (!question_id) {
                return std::unexpected(question_id.error());
            }
            if (!issue_id) {
                return std::unexpected(issue_id.error());
            }
            if (!topic_id) {
                return std::unexpected(topic_id.error());
            }
            if (!prompt) {
                return std::unexpected(prompt.error());
            }
            return std::unexpected(grounding_values.error());
        }
        const auto topic = model::argumentFocusTopicFromId(*topic_id);
        const auto topics = topics_by_issue.find(*issue_id);
        if (!question_ids.emplace(*question_id).second || topics == topics_by_issue.end() ||
            !topic.has_value() || !topics->second.contains(*topic_id) ||
            !isCanonicalQuestionPrompt(*prompt) ||
            ++questions_per_issue[*issue_id] >
                static_cast<std::size_t>(maximum_authored_questions_per_issue)) {
            return fail(RuntimePackErrorCode::CrossReferenceFailure,
                        question_path + " has invalid identity, issue, topic, prompt, or bounds");
        }
        const auto* issue = issueById(case_document.issues, *issue_id);
        if (issue == nullptr) {
            return fail(RuntimePackErrorCode::CrossReferenceFailure,
                        question_path + " references an unavailable case issue");
        }
        covered_issue_topics.emplace(*issue_id + "\n" + *topic_id);
        std::vector<model::AuthoredArgumentGrounding> grounding;
        grounding.reserve(static_cast<std::size_t>(grounding_values->size()));
        for (qsizetype grounding_index = 0; grounding_index < grounding_values->size();
             ++grounding_index) {
            const auto grounding_path =
                question_path + ".grounding[" + std::to_string(grounding_index) + "]";
            if (!grounding_values->at(grounding_index).isObject()) {
                return fail(RuntimePackErrorCode::InvalidResource,
                            grounding_path + " must be an object");
            }
            const auto grounding_value = grounding_values->at(grounding_index).toObject();
            const auto grounding_id = requiredId(grounding_value, "grounding_id", grounding_path);
            const auto kind = requiredString(grounding_value, "kind", grounding_path);
            if (!grounding_id || !kind) {
                return std::unexpected(!grounding_id ? grounding_id.error() : kind.error());
            }
            if (!grounding_ids.emplace(*grounding_id).second) {
                return fail(RuntimePackErrorCode::InvalidResource,
                            grounding_path + ".grounding_id must be bank-unique");
            }
            if (*kind == "authority") {
                if (!hasExactKeys(grounding_value, {"grounding_id", "kind", "authority_id"})) {
                    return fail(RuntimePackErrorCode::InvalidResource,
                                grounding_path + " has invalid authority fields");
                }
                const auto authority_id =
                    requiredId(grounding_value, "authority_id", grounding_path);
                if (!authority_id) {
                    return std::unexpected(authority_id.error());
                }
                if (std::ranges::none_of(issue->authority_ids, [&authority_id](const auto& id) {
                        return id.value == *authority_id;
                    })) {
                    return fail(RuntimePackErrorCode::CrossReferenceFailure,
                                grounding_path + ".authority_id is outside its case issue");
                }
                auto authority = index.requireAuthority(*authority_id, grounding_path);
                if (!authority) {
                    return std::unexpected(authority.error());
                }
                if (!authority->provenance.has_value()) {
                    return fail(RuntimePackErrorCode::CrossReferenceFailure,
                                grounding_path + ".authority_id lacks canonical provenance");
                }
                grounding.push_back(
                    model::AuthorityArgumentGrounding{*grounding_id, std::move(*authority)});
            } else if (*kind == "brief_page") {
                if (!hasExactKeys(grounding_value,
                                  {"grounding_id", "kind", "entry_id", "page_number"})) {
                    return fail(RuntimePackErrorCode::InvalidResource,
                                grounding_path + " has invalid brief-page fields");
                }
                const auto entry_id = requiredId(grounding_value, "entry_id", grounding_path);
                const auto page_number =
                    requiredUnsigned(grounding_value, "page_number", grounding_path, 1, 10'000);
                if (!entry_id || !page_number) {
                    return std::unexpected(!entry_id ? entry_id.error() : page_number.error());
                }
                const auto entry =
                    std::ranges::find(record.docket_entries, *entry_id,
                                      [](const RuntimeDocketEntry& item) { return item.id.value; });
                const auto issue_scoped =
                    std::ranges::any_of(issue->record_anchor_ids, [&entry_id](const auto& id) {
                        return id.value == *entry_id;
                    });
                if (entry == record.docket_entries.end() || !issue_scoped || entry->sealed ||
                    *page_number > entry->page_count ||
                    !std::ranges::any_of(entry->tags,
                                         [](const std::string& tag) { return tag == "brief"; })) {
                    return fail(RuntimePackErrorCode::CrossReferenceFailure,
                                grounding_path + " must resolve to an issue-scoped tagged "
                                                 "unsealed brief page");
                }
                grounding.push_back(model::BriefPageArgumentGrounding{
                    *grounding_id, *entry_id, *page_number, entry->asset_sha256});
            } else if (*kind == "record_page") {
                if (!hasExactKeys(grounding_value, {"grounding_id", "kind", "anchor_id"})) {
                    return fail(RuntimePackErrorCode::InvalidResource,
                                grounding_path + " has invalid record-page fields");
                }
                const auto anchor_id = requiredId(grounding_value, "anchor_id", grounding_path);
                if (!anchor_id) {
                    return std::unexpected(anchor_id.error());
                }
                auto anchor = std::ranges::find(
                    record.page_anchors, *anchor_id,
                    [](const RuntimeRecordPageAnchor& item) { return item.id.value; });
                if (anchor == record.page_anchors.end()) {
                    const RuntimeRecordTwinAnchor* twin = nullptr;
                    for (const auto& disclosure : record.sealed_disclosures) {
                        const auto mapped =
                            std::ranges::find(disclosure.anchor_mappings, *anchor_id,
                                              [](const RuntimeRecordTwinAnchor& item) {
                                                  return item.stable_anchor_id.value;
                                              });
                        if (mapped != disclosure.anchor_mappings.end()) {
                            twin = &*mapped;
                            break;
                        }
                    }
                    if (twin != nullptr) {
                        anchor = std::ranges::find(
                            record.page_anchors, twin->public_anchor_id.value,
                            [](const RuntimeRecordPageAnchor& item) { return item.id.value; });
                    }
                }
                const auto issue_scoped =
                    std::ranges::any_of(issue->record_anchor_ids, [&anchor_id](const auto& id) {
                        return id.value == *anchor_id;
                    });
                if (anchor == record.page_anchors.end() || !issue_scoped) {
                    return fail(RuntimePackErrorCode::CrossReferenceFailure,
                                grounding_path + " must resolve to an issue-scoped page anchor");
                }
                const auto entry =
                    std::ranges::find(record.docket_entries, anchor->entry_id.value,
                                      [](const RuntimeDocketEntry& item) { return item.id.value; });
                if (entry == record.docket_entries.end() || entry->sealed) {
                    return fail(RuntimePackErrorCode::CrossReferenceFailure,
                                grounding_path + " resolves to a missing or sealed entry");
                }
                grounding.push_back(model::RecordPageArgumentGrounding{
                    *grounding_id, *anchor_id, anchor->entry_id.value, anchor->page_number,
                    entry->asset_sha256, anchor->citation_label});
            } else {
                return fail(RuntimePackErrorCode::InvalidResource,
                            grounding_path + ".kind is unsupported");
            }
        }
        std::ranges::sort(grounding, [](const auto& left, const auto& right) {
            return groundingId(left) < groundingId(right);
        });
        questions.push_back(model::AuthoredArgumentQuestion{*question_id, *issue_id, *topic,
                                                            *prompt, std::move(grounding)});
    }
    for (const auto& [issue_id, topics] : topics_by_issue) {
        for (const auto& topic_id : topics) {
            if (!covered_issue_topics.contains(issue_id + "\n" + topic_id)) {
                return fail(RuntimePackErrorCode::CrossReferenceFailure,
                            path + ".questions does not cover every issue-topic binding");
            }
        }
    }
    std::ranges::sort(questions, {}, &model::AuthoredArgumentQuestion::id);
    model::AuthoredQuestionBank parsed{
        case_document.definition.id, resource.descriptor.id, mode, *asserted_digest,
        std::move(issue_topics),     std::move(questions)};
    if (canonicalQuestionBankDigest(parsed) != parsed.grounding_digest) {
        return fail(RuntimePackErrorCode::CrossReferenceFailure,
                    path + ".grounding_digest does not match its resolved canonical bytes");
    }
    return std::optional<model::AuthoredQuestionBank>{std::move(parsed)};
}

[[nodiscard]] Result<RuntimeArgumentConfiguration>
parseArgument(const ValidatedResource& resource, const ResourceIndex& index,
              const ParsedCase& case_document, const RuntimeCourt& court,
              const RuntimeRecord& record, const model::PackRevision& root_revision) {
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
    if (resource.document.contains(QStringLiteral("grounded_question_bank")) &&
        (!index.ownedBy(resource.descriptor.id, root_revision) ||
         !index.ownedBy(*case_id, root_revision))) {
        return fail(RuntimePackErrorCode::CrossReferenceFailure,
                    path + " and its grounded-question case must share the exact root owner");
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
    for (const auto& issue : *issue_ids) {
        if (!issueExists(case_document.issues, issue)) {
            return fail(RuntimePackErrorCode::CrossReferenceFailure,
                        path + " references issue outside its case: " + issue);
        }
        permitted.push_back(RuntimeIssueId{issue});
    }
    auto question_bank =
        parseQuestionBank(resource, index, case_document, record, *bench, *issue_ids);
    if (!question_bank) {
        return std::unexpected(question_bank.error());
    }
    return RuntimeArgumentConfiguration{RuntimeArgumentConfigId{resource.descriptor.id},
                                        model::CaseId{*case_id},
                                        std::move(*bench),
                                        *total,
                                        *rebuttal,
                                        std::move(permitted),
                                        std::move(*question_bank)};
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
             const std::vector<const ValidatedResource*>& arguments,
             const model::PackRevision& root_revision) {
    auto parsed_case = parseCase(resource, index);
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
    if (!index.ownedBy(parsed_case->record_id.value, root_revision)) {
        return fail(RuntimePackErrorCode::CrossReferenceFailure,
                    "case " + parsed_case->definition.id.value +
                        " must reference a record owned by the exact root pack");
    }
    for (const auto& authority_id : procedure->authority_set_ids) {
        const auto authority = index.require(authority_id.value, model::ResourceKind::AuthoritySet,
                                             "procedure " + procedure->id.value);
        if (!authority) {
            return std::unexpected(authority.error());
        }
    }
    auto workflow =
        parseWorkflow(**workflow_resource, index, (*workflow_resource)->descriptor.schema_version);
    auto court = parseCourt(**court_resource);
    auto record = parseRecord(**record_resource, index);
    auto catalog = parseFilingCatalog(**catalog_resource, index);
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
    if (resource.descriptor.schema_version == 2) {
        for (const auto& operation : workflow->operations) {
            if (!index.authorityInSets(operation.authority.primary.id.value,
                                       procedure->authority_set_ids) ||
                std::ranges::any_of(operation.authority.supporting, [&](const auto& authority) {
                    return !index.authorityInSets(authority.id.value, procedure->authority_set_ids);
                })) {
                return fail(RuntimePackErrorCode::CrossReferenceFailure,
                            "workflow " + workflow->id.value +
                                " uses an authority outside its procedure authority sets");
            }
        }
        if (std::ranges::any_of(*catalog, [&](const auto& entry) {
                return !index.authorityInSets(entry.second.authority.id.value,
                                              procedure->authority_set_ids);
            })) {
            return fail(RuntimePackErrorCode::CrossReferenceFailure,
                        "filing catalog " + procedure->filing_catalog_id.value +
                            " uses an authority outside its procedure authority sets");
        }
        for (const auto& issue : parsed_case->issues) {
            if (std::ranges::any_of(issue.authority_ids, [&](const auto& authority) {
                    return !index.authorityInSets(authority.value, procedure->authority_set_ids);
                })) {
                return fail(RuntimePackErrorCode::CrossReferenceFailure,
                            "case " + parsed_case->definition.id.value +
                                " uses an authority outside its procedure authority sets");
            }
        }
    }
    for (const auto& docket : record->dockets) {
        if (!docket.court_id.has_value()) {
            continue;
        }
        const auto docket_court = index.require(docket.court_id->value, model::ResourceKind::Court,
                                                "record " + record->id.value);
        if (!docket_court) {
            return std::unexpected(docket_court.error());
        }
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
    std::unordered_set<std::string> policy_hidden_ids;
    std::unordered_set<std::string> disclosed_sealed_entries;
    if (record->disclosure_policy.has_value()) {
        for (const auto& disclosure : record->sealed_disclosures) {
            policy_hidden_ids.insert(disclosure.sealed_entry_id.value);
            disclosed_sealed_entries.insert(disclosure.sealed_entry_id.value);
            for (const auto& mapping : disclosure.anchor_mappings) {
                policy_hidden_ids.insert(mapping.sealed_anchor_id.value);
                record_entries.insert(mapping.stable_anchor_id.value);
            }
        }
    }
    for (const auto& entry : record->docket_entries) {
        if (!policy_hidden_ids.contains(entry.id.value)) {
            record_entries.insert(entry.id.value);
        }
    }
    for (const auto& anchor : record->page_anchors) {
        if (!policy_hidden_ids.contains(anchor.id.value) &&
            !disclosed_sealed_entries.contains(anchor.entry_id.value)) {
            record_entries.insert(anchor.id.value);
        }
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
        auto parsed_argument =
            parseArgument(*argument, index, *parsed_case, *court, *record, root_revision);
        if (!parsed_argument) {
            return std::unexpected(parsed_argument.error());
        }
        runtime_arguments.push_back(std::move(*parsed_argument));
    }
    std::vector<RuntimeFilingAuthority> filing_authorities;
    filing_authorities.reserve(catalog->size());
    for (const auto& [filing_type_id, filing] : *catalog) {
        filing_authorities.push_back(
            RuntimeFilingAuthority{model::FilingTypeId{filing_type_id}, filing.authority});
    }
    std::ranges::sort(filing_authorities, {},
                      [](const auto& filing) { return filing.filing_type_id.value; });
    return RuntimeCase{std::move(parsed_case->definition),
                       std::move(parsed_case->title),
                       std::move(*procedure),
                       std::move(*court),
                       std::move(*workflow),
                       std::move(*record),
                       std::move(parsed_case->issues),
                       std::move(filing_authorities),
                       std::move(parsed_case->authored_disposition_id),
                       std::move(runtime_arguments)};
}

[[nodiscard]] Result<RuntimePack> projectRuntimePack(const LoadedPack& pack,
                                                     const ResourceIndex& index) {
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
            const auto case_resource = index.require(*case_id, model::ResourceKind::Case, path);
            if (!case_resource) {
                return std::unexpected(case_resource.error());
            }
            if (resource.document.contains(QStringLiteral("grounded_question_bank")) &&
                !index.ownedBy(*case_id, pack.revision)) {
                return fail(RuntimePackErrorCode::CrossReferenceFailure,
                            path + " targets a dependency-owned case and would be orphaned");
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
        auto runtime_case = assembleCase(*resource, index, arguments, pack.revision);
        if (!runtime_case) {
            return std::unexpected(runtime_case.error());
        }
        runtime_cases.push_back(std::move(*runtime_case));
    }
    return RuntimePack{pack.revision, std::move(runtime_cases)};
}

} // namespace

std::expected<RuntimePack, RuntimePackError>
loadRuntimePackForEvidence(const LoadedPack& case_owner,
                           std::span<const LoadedPack* const> subject_dependency_first) {
    if (subject_dependency_first.empty() || subject_dependency_first.back() != &case_owner) {
        return fail(RuntimePackErrorCode::InvalidPack,
                    "realism evidence subject closure does not end with its case owner");
    }
    const auto index = makeIndex(subject_dependency_first, case_owner.manifest_schema_version);
    if (!index) {
        return std::unexpected(index.error());
    }
    return projectRuntimePack(case_owner, *index);
}

std::expected<RuntimePack, RuntimePackError> loadRuntimePack(const LoadedPack& pack) {
    if (pack.graph_state != PackGraphState::StandaloneValidated || !pack.dependencies.empty()) {
        return fail(RuntimePackErrorCode::InvalidPack,
                    "dependency-bearing or deferred pack requires a catalog-resolved closure");
    }
    const auto evidence = validateRealismEvidence(pack, std::span<const LoadedPack* const>{});
    if (!evidence) {
        return fail(RuntimePackErrorCode::InvalidPack, evidence.error().message.toStdString());
    }
    const std::array packs{&pack};
    const auto index = makeIndex(packs, pack.manifest_schema_version);
    if (!index) {
        return std::unexpected(index.error());
    }
    return projectRuntimePack(pack, *index);
}

std::expected<RuntimePack, RuntimePackError> loadRuntimePack(const ResolvedPack& pack) {
    std::vector<const LoadedPack*> closure;
    closure.reserve(pack.dependenciesDependencyFirst().size() + 1U);
    for (const auto& dependency : pack.dependenciesDependencyFirst()) {
        closure.push_back(&dependency);
    }
    closure.push_back(&pack.root());
    const auto index = makeIndex(closure, pack.root().manifest_schema_version);
    if (!index) {
        return std::unexpected(index.error());
    }
    return projectRuntimePack(pack.root(), *index);
}

} // namespace appellate::packs
