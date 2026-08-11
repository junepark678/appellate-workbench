#include "appellate/engine/workflow_engine.hpp"

#include "appellate/engine/business_calendar.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace appellate::engine {
namespace {

constexpr std::size_t max_stages = 256;
constexpr std::size_t max_operations = 4096;
constexpr std::size_t max_routes = 4096;
constexpr std::size_t max_holidays = 4096;
constexpr std::size_t max_case_actors = 1024;
constexpr std::size_t max_route_items = 256;
constexpr std::size_t max_authorities = 32;
constexpr std::size_t max_state_items = 4096;
constexpr std::size_t max_disposition_targets = 4096;
constexpr std::size_t max_disposition_plans = 64;
constexpr std::size_t max_disposition_components = 32;
constexpr std::size_t max_record_anchors = 32;
constexpr std::size_t max_preconditions = 32;
constexpr std::uint32_t max_events_per_command = 3;

[[nodiscard]] auto fail(WorkflowErrorCode code, std::string message)
    -> std::unexpected<WorkflowError> {
    return std::unexpected(WorkflowError{code, std::move(message)});
}

[[nodiscard]] bool validText(std::string_view value, std::size_t maximum = 512) {
    return !value.empty() && value.size() <= maximum;
}

[[nodiscard]] bool validNamespacedId(std::string_view value) {
    if (value.size() < 3 || value.size() > 160) {
        return false;
    }
    bool has_separator = false;
    bool previous_was_separator = true;
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        const auto is_alphanumeric =
            std::isdigit(character) != 0 || (character >= static_cast<unsigned char>('a') &&
                                             character <= static_cast<unsigned char>('z'));
        const auto is_separator = character == static_cast<unsigned char>('.') ||
                                  character == static_cast<unsigned char>('-');
        if (!is_alphanumeric && !is_separator) {
            return false;
        }
        if (is_separator && previous_was_separator) {
            return false;
        }
        has_separator = has_separator || is_separator;
        previous_was_separator = is_separator;
    }
    return has_separator && !previous_was_separator;
}

[[nodiscard]] bool validLegalDate(model::LegalDate date) {
    if (!date.value.ok()) {
        return false;
    }
    const auto year = static_cast<int>(date.value.year());
    return year >= 1 && year <= 9999;
}

[[nodiscard]] bool validSourceVersion(std::string_view value) {
    if (value.size() != 10 || value[4] != '-' || value[7] != '-') {
        return false;
    }
    for (const auto index : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{3},
                             std::size_t{5}, std::size_t{6}, std::size_t{8}, std::size_t{9}}) {
        if (std::isdigit(static_cast<unsigned char>(value[index])) == 0) {
            return false;
        }
    }
    const auto digit = [&](std::size_t index) { return value[index] - '0'; };
    const auto year = digit(0) * 1000 + digit(1) * 100 + digit(2) * 10 + digit(3);
    const auto month = digit(5) * 10 + digit(6);
    const auto day = digit(8) * 10 + digit(9);
    return year >= 1 &&
           (std::chrono::year{year} / std::chrono::month{static_cast<unsigned>(month)} /
            std::chrono::day{static_cast<unsigned>(day)})
               .ok();
}

[[nodiscard]] bool validDigest(std::string_view digest) {
    return digest.size() == 64 && std::ranges::all_of(digest, [](unsigned char character) {
               return std::isdigit(character) != 0 ||
                      (character >= static_cast<unsigned char>('a') &&
                       character <= static_cast<unsigned char>('f'));
           });
}

[[nodiscard]] auto sha256(std::span<const std::uint8_t> input) -> std::string {
    constexpr std::array<std::uint32_t, 64> constants{
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
        0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
        0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
        0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
        0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
        0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
        0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
        0xc67178f2U};
    std::array<std::uint32_t, 8> hash{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::vector<std::uint8_t> padded(input.begin(), input.end());
    const auto bit_length = static_cast<std::uint64_t>(padded.size()) * 8U;
    padded.push_back(0x80U);
    while (padded.size() % 64U != 56U) {
        padded.push_back(0U);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        padded.push_back(static_cast<std::uint8_t>(bit_length >> static_cast<unsigned>(shift)));
    }

    for (std::size_t offset = 0; offset < padded.size(); offset += 64U) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16U; ++index) {
            const auto position = offset + index * 4U;
            words[index] = (static_cast<std::uint32_t>(padded[position]) << 24U) |
                           (static_cast<std::uint32_t>(padded[position + 1U]) << 16U) |
                           (static_cast<std::uint32_t>(padded[position + 2U]) << 8U) |
                           static_cast<std::uint32_t>(padded[position + 3U]);
        }
        for (std::size_t index = 16U; index < words.size(); ++index) {
            const auto sigma0 = std::rotr(words[index - 15U], 7) ^
                                std::rotr(words[index - 15U], 18) ^ (words[index - 15U] >> 3U);
            const auto sigma1 = std::rotr(words[index - 2U], 17) ^
                                std::rotr(words[index - 2U], 19) ^ (words[index - 2U] >> 10U);
            words[index] = words[index - 16U] + sigma0 + words[index - 7U] + sigma1;
        }

        auto a = hash[0];
        auto b = hash[1];
        auto c = hash[2];
        auto d = hash[3];
        auto e = hash[4];
        auto f = hash[5];
        auto g = hash[6];
        auto h = hash[7];
        for (std::size_t index = 0; index < words.size(); ++index) {
            const auto sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const auto choice = (e & f) ^ ((~e) & g);
            const auto temporary1 = h + sum1 + choice + constants[index] + words[index];
            const auto sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temporary2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        hash[0] += a;
        hash[1] += b;
        hash[2] += c;
        hash[3] += d;
        hash[4] += e;
        hash[5] += f;
        hash[6] += g;
        hash[7] += h;
    }

    constexpr std::string_view hexadecimal = "0123456789abcdef";
    std::string result;
    result.reserve(64U);
    for (const auto word : hash) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            const auto digit = static_cast<std::size_t>((word >> static_cast<unsigned>(shift)) &
                                                        std::uint32_t{0x0fU});
            result.push_back(hexadecimal[digit]);
        }
    }
    return result;
}

void appendUint64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        output.push_back(static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
    }
}

void appendCanonicalString(std::vector<std::uint8_t>& output, std::string_view value) {
    appendUint64(output, static_cast<std::uint64_t>(value.size()));
    output.insert(output.end(), value.begin(), value.end());
}

[[nodiscard]] auto dispositionFinalityName(model::DispositionFinality finality)
    -> std::string_view {
    switch (finality) {
    case model::DispositionFinality::Final:
        return "final";
    case model::DispositionFinality::Nonfinal:
        return "nonfinal";
    }
    return {};
}

[[nodiscard]] auto dispositionScopeName(model::DispositionScope scope) -> std::string_view {
    switch (scope) {
    case model::DispositionScope::Whole:
        return "whole";
    case model::DispositionScope::Part:
        return "part";
    }
    return {};
}

[[nodiscard]] auto dispositionActionName(model::DispositionAction action) -> std::string_view {
    switch (action) {
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

[[nodiscard]] auto canonicalDispositionDigest(const model::CaseDefinition& case_definition,
                                              const model::DispositionPlan& plan) -> std::string {
    std::vector<std::uint8_t> canonical;
    constexpr std::string_view domain = "appellate-workbench-disposition-plan-v1";
    appendCanonicalString(canonical, domain);
    appendCanonicalString(canonical, case_definition.id.value);
    appendCanonicalString(canonical, case_definition.authored_disposition_operation_id->value);
    appendCanonicalString(canonical, plan.id.value);
    appendCanonicalString(canonical, dispositionFinalityName(plan.finality));

    auto components = plan.components;
    std::ranges::sort(components, {}, [](const auto& component) {
        return std::tie(component.issue_id.value, component.target_id.value);
    });
    appendUint64(canonical, static_cast<std::uint64_t>(components.size()));
    for (const auto& component : components) {
        appendCanonicalString(canonical, component.issue_id.value);
        appendCanonicalString(canonical, component.target_id.value);
        appendCanonicalString(canonical, dispositionScopeName(component.scope));
        appendCanonicalString(canonical, dispositionActionName(component.action));
        appendUint64(canonical, component.remand ? std::uint64_t{1U} : std::uint64_t{0U});

        auto authorities = component.authority_ids;
        std::ranges::sort(authorities, {}, &model::AuthorityId::value);
        appendUint64(canonical, static_cast<std::uint64_t>(authorities.size()));
        for (const auto& authority : authorities) {
            appendCanonicalString(canonical, authority.value);
        }

        auto anchors = component.record_anchor_ids;
        std::ranges::sort(anchors, {}, &model::RecordAnchorId::value);
        appendUint64(canonical, static_cast<std::uint64_t>(anchors.size()));
        for (const auto& anchor : anchors) {
            appendCanonicalString(canonical, anchor.value);
        }
    }
    return sha256(canonical);
}

[[nodiscard]] bool validAuthorityType(model::AuthorityType type) {
    switch (type) {
    case model::AuthorityType::Constitution:
    case model::AuthorityType::Statute:
    case model::AuthorityType::Rule:
    case model::AuthorityType::Regulation:
    case model::AuthorityType::Case:
    case model::AuthorityType::Order:
    case model::AuthorityType::AdministrativeDecision:
    case model::AuthorityType::Other:
        return true;
    }
    return false;
}

[[nodiscard]] bool validPrecedentialStatus(model::PrecedentialStatus status) {
    switch (status) {
    case model::PrecedentialStatus::NotApplicable:
    case model::PrecedentialStatus::Precedential:
    case model::PrecedentialStatus::Nonprecedential:
        return true;
    }
    return false;
}

[[nodiscard]] bool validProvenance(const model::AuthorityProvenance& provenance) {
    return validAuthorityType(provenance.type) && validNamespacedId(provenance.jurisdiction_id) &&
           validNamespacedId(provenance.issuing_body_id) &&
           validPrecedentialStatus(provenance.precedential_status) &&
           validSourceVersion(provenance.checked_on) &&
           model::isCanonicalAuthorityText(provenance.locator, 4096) &&
           model::isCanonicalAuthoritySourceUrl(provenance.source_url);
}

[[nodiscard]] bool validAuthorityRef(const model::AuthorityRef& authority) {
    if (!authority.provenance.has_value()) {
        return validNamespacedId(authority.id.value) && validText(authority.citation, 4096) &&
               validSourceVersion(authority.source_version) &&
               validText(authority.proposition, 4096);
    }
    return validNamespacedId(authority.id.value) && validSourceVersion(authority.source_version) &&
           model::isCanonicalAuthorityText(authority.citation, 4096) &&
           model::isCanonicalAuthorityText(authority.proposition, 4096) &&
           validProvenance(*authority.provenance) &&
           model::authorityVerificationNotBeforeSource(authority.source_version,
                                                       authority.provenance->checked_on);
}

[[nodiscard]] bool validAuthority(const model::AuthorityBasis& authority) {
    if (!validAuthorityRef(authority.primary) || authority.supporting.size() > max_authorities ||
        !std::ranges::all_of(authority.supporting, [&](const auto& supporting) {
            return supporting.provenance.has_value() == authority.primary.provenance.has_value() &&
                   validAuthorityRef(supporting);
        })) {
        return false;
    }
    std::unordered_set<std::string> identifiers{authority.primary.id.value};
    return std::ranges::all_of(authority.supporting, [&](const auto& supporting) {
        return identifiers.emplace(supporting.id.value).second;
    });
}

template <typename Range, typename Projection>
[[nodiscard]] bool hasDuplicates(const Range& range, Projection projection) {
    std::unordered_set<std::string> identifiers;
    identifiers.reserve(range.size());
    for (const auto& item : range) {
        if (!identifiers.emplace(std::invoke(projection, item)).second) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool validOpcode(model::WorkflowOpcode opcode) {
    switch (opcode) {
    case model::WorkflowOpcode::AcceptFiling:
    case model::WorkflowOpcode::RejectFiling:
    case model::WorkflowOpcode::IssueDeficiency:
    case model::WorkflowOpcode::CalculateDeadline:
    case model::WorkflowOpcode::EnterOrder:
    case model::WorkflowOpcode::AdvanceStage:
    case model::WorkflowOpcode::SetSealed:
    case model::WorkflowOpcode::ScheduleArgument:
    case model::WorkflowOpcode::IssueJudgment:
    case model::WorkflowOpcode::IssueMandate:
        return true;
    }
    return false;
}

[[nodiscard]] bool validDeadlineCounting(model::DeadlineCounting counting) {
    return counting == model::DeadlineCounting::CalendarDays ||
           counting == model::DeadlineCounting::BusinessDays;
}

[[nodiscard]] bool validDeadlinePurpose(model::WorkflowDeadlinePurpose purpose) {
    return purpose == model::WorkflowDeadlinePurpose::Filing ||
           purpose == model::WorkflowDeadlinePurpose::DeficiencyCure;
}

[[nodiscard]] bool validDeadlineStatus(model::WorkflowDeadlineStatus status) {
    return status == model::WorkflowDeadlineStatus::Open ||
           status == model::WorkflowDeadlineStatus::Satisfied;
}

[[nodiscard]] bool validOrderDisposition(model::WorkflowOrderDisposition disposition) {
    return disposition == model::WorkflowOrderDisposition::Granted ||
           disposition == model::WorkflowOrderDisposition::Denied ||
           disposition == model::WorkflowOrderDisposition::Other;
}

[[nodiscard]] bool validDeadlineCondition(model::WorkflowDeadlineCondition condition) {
    return condition == model::WorkflowDeadlineCondition::Open ||
           condition == model::WorkflowDeadlineCondition::Satisfied ||
           condition == model::WorkflowDeadlineCondition::Elapsed ||
           condition == model::WorkflowDeadlineCondition::NotElapsed;
}

[[nodiscard]] bool validDispositionScope(model::DispositionScope scope) {
    return scope == model::DispositionScope::Whole || scope == model::DispositionScope::Part;
}

[[nodiscard]] bool validDispositionAction(model::DispositionAction action) {
    switch (action) {
    case model::DispositionAction::Affirm:
    case model::DispositionAction::Reverse:
    case model::DispositionAction::Vacate:
    case model::DispositionAction::Dismiss:
    case model::DispositionAction::Grant:
    case model::DispositionAction::Deny:
        return true;
    }
    return false;
}

[[nodiscard]] bool validDispositionFinality(model::DispositionFinality finality) {
    return finality == model::DispositionFinality::Final ||
           finality == model::DispositionFinality::Nonfinal;
}

[[nodiscard]] bool mayRemand(model::DispositionAction action) {
    return action == model::DispositionAction::Reverse ||
           action == model::DispositionAction::Vacate ||
           action == model::DispositionAction::Dismiss || action == model::DispositionAction::Grant;
}

[[nodiscard]] auto operationFor(const model::WorkflowDefinition& workflow,
                                const model::WorkflowOperationId& id)
    -> const model::WorkflowOperation* {
    const auto found = std::ranges::find(workflow.operations, id, &model::WorkflowOperation::id);
    return found == workflow.operations.end() ? nullptr : &*found;
}

[[nodiscard]] auto actorFor(const model::CaseDefinition& case_definition, const model::ActorId& id)
    -> const model::CaseActor* {
    const auto found = std::ranges::find(case_definition.actors, id, &model::CaseActor::id);
    return found == case_definition.actors.end() ? nullptr : &*found;
}

[[nodiscard]] bool roleAllowed(const std::vector<model::ActorRoleId>& roles,
                               const model::ActorRoleId& role) {
    return std::ranges::find(roles, role) != roles.end();
}

[[nodiscard]] auto routeFor(const model::WorkflowDefinition& workflow,
                            const model::WorkflowStageId& stage,
                            const model::FilingTypeId& filing_type)
    -> const model::WorkflowFilingRoute* {
    const auto found = std::ranges::find_if(workflow.filing_routes, [&](const auto& route) {
        return route.stage_id == stage && route.filing_type == filing_type;
    });
    return found == workflow.filing_routes.end() ? nullptr : &*found;
}

[[nodiscard]] auto deadlineFor(model::WorkflowState& state, const model::WorkflowDeadlineId& id)
    -> model::WorkflowDeadlineRecord* {
    const auto found =
        std::ranges::find(state.deadlines, id, &model::WorkflowDeadlineRecord::deadline_id);
    return found == state.deadlines.end() ? nullptr : &*found;
}

[[nodiscard]] auto deadlineFor(const model::WorkflowState& state,
                               const model::WorkflowDeadlineId& id)
    -> const model::WorkflowDeadlineRecord* {
    const auto found =
        std::ranges::find(state.deadlines, id, &model::WorkflowDeadlineRecord::deadline_id);
    return found == state.deadlines.end() ? nullptr : &*found;
}

[[nodiscard]] bool
preconditionsSatisfied(const std::vector<model::WorkflowPrecondition>& preconditions,
                       const model::WorkflowState& state, model::LegalDate command_date) {
    return std::ranges::all_of(preconditions, [&](const auto& precondition) {
        return std::visit(
            [&](const auto& concrete) {
                using Precondition = std::remove_cvref_t<decltype(concrete)>;
                if constexpr (std::same_as<Precondition, model::WorkflowFilingPrecondition>) {
                    const auto present =
                        std::ranges::find(state.accepted_filings, concrete.filing_type,
                                          &model::WorkflowFilingRecord::filing_type) !=
                        state.accepted_filings.end();
                    return present == concrete.present;
                } else if constexpr (std::same_as<Precondition, model::WorkflowOrderPrecondition>) {
                    const auto order = std::ranges::find(state.orders, concrete.order_id,
                                                         &model::WorkflowOrderRecord::order_id);
                    return order != state.orders.end() &&
                           order->disposition == concrete.disposition;
                } else if constexpr (std::same_as<Precondition,
                                                  model::WorkflowDeadlinePrecondition>) {
                    const auto* deadline = deadlineFor(state, concrete.deadline_id);
                    if (deadline == nullptr) {
                        return false;
                    }
                    switch (concrete.condition) {
                    case model::WorkflowDeadlineCondition::Open:
                        return deadline->status == model::WorkflowDeadlineStatus::Open;
                    case model::WorkflowDeadlineCondition::Satisfied:
                        return deadline->status == model::WorkflowDeadlineStatus::Satisfied;
                    case model::WorkflowDeadlineCondition::Elapsed:
                        return std::chrono::sys_days{command_date.value} >
                               std::chrono::sys_days{deadline->due_date.value};
                    case model::WorkflowDeadlineCondition::NotElapsed:
                        return std::chrono::sys_days{command_date.value} <=
                               std::chrono::sys_days{deadline->due_date.value};
                    }
                    return false;
                } else if constexpr (std::same_as<Precondition,
                                                  model::WorkflowArgumentPrecondition>) {
                    return state.argument_date.has_value() == concrete.scheduled;
                } else {
                    return state.judgment_sha256.has_value() == concrete.issued;
                }
            },
            precondition);
    });
}

[[nodiscard]] auto deficiencyFor(model::WorkflowState& state, const model::WorkflowDeficiencyId& id)
    -> model::WorkflowDeficiencyRecord* {
    const auto found =
        std::ranges::find(state.deficiencies, id, &model::WorkflowDeficiencyRecord::deficiency_id);
    return found == state.deficiencies.end() ? nullptr : &*found;
}

[[nodiscard]] auto deficiencyFor(const model::WorkflowState& state,
                                 const model::WorkflowDeficiencyId& id)
    -> const model::WorkflowDeficiencyRecord* {
    const auto found =
        std::ranges::find(state.deficiencies, id, &model::WorkflowDeficiencyRecord::deficiency_id);
    return found == state.deficiencies.end() ? nullptr : &*found;
}

[[nodiscard]] bool isLater(model::LegalDate left, model::LegalDate right) {
    return std::chrono::sys_days{left.value} > std::chrono::sys_days{right.value};
}

[[nodiscard]] auto deadlineRule(const model::WorkflowOperation& operation)
    -> model::CureDeadlineRule {
    return model::CureDeadlineRule{*operation.deadline_days, *operation.deadline_counting, true};
}

[[nodiscard]] auto deficiencyIdFor(const model::WorkflowCommandId& command_id)
    -> model::WorkflowDeficiencyId {
    return model::WorkflowDeficiencyId{command_id.value + ".deficiency"};
}

[[nodiscard]] auto deficiencyDeadlineIdFor(const model::WorkflowDeadlinePlan& plan,
                                           const model::WorkflowCommandId& command_id)
    -> model::WorkflowDeadlineId {
    return model::WorkflowDeadlineId{plan.deadline_id.value + "." + command_id.value};
}

[[nodiscard]] bool validPrecondition(const model::WorkflowPrecondition& precondition) {
    return std::visit(
        [](const auto& concrete) {
            using Precondition = std::remove_cvref_t<decltype(concrete)>;
            if constexpr (std::same_as<Precondition, model::WorkflowFilingPrecondition>) {
                return validNamespacedId(concrete.filing_type.value);
            } else if constexpr (std::same_as<Precondition, model::WorkflowOrderPrecondition>) {
                return validNamespacedId(concrete.order_id.value) &&
                       validOrderDisposition(concrete.disposition);
            } else if constexpr (std::same_as<Precondition, model::WorkflowDeadlinePrecondition>) {
                return validNamespacedId(concrete.deadline_id.value) &&
                       validDeadlineCondition(concrete.condition);
            } else {
                return true;
            }
        },
        precondition);
}

[[nodiscard]] bool
preconditionsAreConsistent(const std::vector<model::WorkflowPrecondition>& preconditions) {
    if (preconditions.size() > max_preconditions ||
        !std::ranges::all_of(preconditions, validPrecondition)) {
        return false;
    }
    for (std::size_t left_index = 0; left_index < preconditions.size(); ++left_index) {
        for (std::size_t right_index = left_index + 1; right_index < preconditions.size();
             ++right_index) {
            const auto& left = preconditions[left_index];
            const auto& right = preconditions[right_index];
            if (left == right) {
                return false;
            }
            if (const auto* left_filing = std::get_if<model::WorkflowFilingPrecondition>(&left)) {
                const auto* right_filing = std::get_if<model::WorkflowFilingPrecondition>(&right);
                if (right_filing != nullptr &&
                    left_filing->filing_type == right_filing->filing_type) {
                    return false;
                }
            } else if (const auto* left_order =
                           std::get_if<model::WorkflowOrderPrecondition>(&left)) {
                const auto* right_order = std::get_if<model::WorkflowOrderPrecondition>(&right);
                if (right_order != nullptr && left_order->order_id == right_order->order_id) {
                    return false;
                }
            } else if (const auto* left_deadline =
                           std::get_if<model::WorkflowDeadlinePrecondition>(&left)) {
                const auto* right_deadline =
                    std::get_if<model::WorkflowDeadlinePrecondition>(&right);
                if (right_deadline != nullptr &&
                    left_deadline->deadline_id == right_deadline->deadline_id &&
                    ((left_deadline->condition == model::WorkflowDeadlineCondition::Open &&
                      right_deadline->condition == model::WorkflowDeadlineCondition::Satisfied) ||
                     (left_deadline->condition == model::WorkflowDeadlineCondition::Satisfied &&
                      right_deadline->condition == model::WorkflowDeadlineCondition::Open) ||
                     (left_deadline->condition == model::WorkflowDeadlineCondition::Elapsed &&
                      right_deadline->condition == model::WorkflowDeadlineCondition::NotElapsed) ||
                     (left_deadline->condition == model::WorkflowDeadlineCondition::NotElapsed &&
                      right_deadline->condition == model::WorkflowDeadlineCondition::Elapsed))) {
                    return false;
                }
            } else if (std::holds_alternative<model::WorkflowArgumentPrecondition>(left) &&
                       std::holds_alternative<model::WorkflowArgumentPrecondition>(right)) {
                return false;
            } else if (std::holds_alternative<model::WorkflowJudgmentPrecondition>(left) &&
                       std::holds_alternative<model::WorkflowJudgmentPrecondition>(right)) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool validDispositionInventory(const model::CaseDefinition& case_definition) {
    const auto legacy = case_definition.disposition_targets.empty() &&
                        case_definition.disposition_plans.empty() &&
                        !case_definition.authored_disposition_plan_id.has_value();
    if (legacy) {
        return !case_definition.authored_disposition_operation_id.has_value() ||
               validNamespacedId(case_definition.authored_disposition_operation_id->value);
    }
    if (case_definition.disposition_targets.empty() ||
        case_definition.disposition_targets.size() > max_disposition_targets ||
        case_definition.disposition_plans.empty() ||
        case_definition.disposition_plans.size() > max_disposition_plans ||
        !case_definition.authored_disposition_plan_id.has_value() ||
        !case_definition.authored_disposition_operation_id.has_value() ||
        !validNamespacedId(case_definition.authored_disposition_plan_id->value) ||
        !validNamespacedId(case_definition.authored_disposition_operation_id->value) ||
        hasDuplicates(case_definition.disposition_targets,
                      [](const auto& target) { return target.target_id.value; }) ||
        hasDuplicates(case_definition.disposition_plans,
                      [](const auto& plan) { return plan.id.value; }) ||
        std::ranges::any_of(case_definition.disposition_targets, [](const auto& target) {
            return !validNamespacedId(target.issue_id.value) ||
                   !validNamespacedId(target.target_id.value);
        })) {
        return false;
    }

    std::unordered_set<std::string> targets;
    targets.reserve(case_definition.disposition_targets.size());
    for (const auto& target : case_definition.disposition_targets) {
        targets.emplace(target.issue_id.value + "\n" + target.target_id.value);
    }
    for (const auto& plan : case_definition.disposition_plans) {
        if (!validNamespacedId(plan.id.value) || !validDispositionFinality(plan.finality) ||
            !validDigest(plan.canonical_sha256) || plan.components.empty() ||
            plan.components.size() > max_disposition_components) {
            return false;
        }
        std::unordered_set<std::string> covered_targets;
        covered_targets.reserve(plan.components.size());
        for (const auto& component : plan.components) {
            const auto target_key = component.issue_id.value + "\n" + component.target_id.value;
            if (!validNamespacedId(component.issue_id.value) ||
                !validNamespacedId(component.target_id.value) ||
                !validDispositionScope(component.scope) ||
                !validDispositionAction(component.action) ||
                (component.remand && !mayRemand(component.action)) ||
                component.authority_ids.empty() ||
                component.authority_ids.size() > max_authorities ||
                component.record_anchor_ids.empty() ||
                component.record_anchor_ids.size() > max_record_anchors ||
                !targets.contains(target_key) || !covered_targets.emplace(target_key).second ||
                hasDuplicates(component.authority_ids,
                              [](const auto& authority) { return authority.value; }) ||
                hasDuplicates(component.record_anchor_ids,
                              [](const auto& anchor) { return anchor.value; }) ||
                std::ranges::any_of(
                    component.authority_ids,
                    [](const auto& authority) { return !validNamespacedId(authority.value); }) ||
                std::ranges::any_of(component.record_anchor_ids, [](const auto& anchor) {
                    return !validNamespacedId(anchor.value);
                })) {
                return false;
            }
        }
        if (canonicalDispositionDigest(case_definition, plan) != plan.canonical_sha256) {
            return false;
        }
    }
    return std::ranges::find(
               case_definition.disposition_plans, *case_definition.authored_disposition_plan_id,
               &model::DispositionPlan::id) != case_definition.disposition_plans.end();
}

[[nodiscard]] auto dispositionPlanFor(const model::CaseDefinition& case_definition,
                                      const model::DispositionPlanId& id)
    -> const model::DispositionPlan* {
    const auto found =
        std::ranges::find(case_definition.disposition_plans, id, &model::DispositionPlan::id);
    return found == case_definition.disposition_plans.end() ? nullptr : &*found;
}

[[nodiscard]] bool usesStructuredDisposition(const model::CaseDefinition& case_definition) {
    return case_definition.authored_disposition_plan_id.has_value();
}

[[nodiscard]] bool validJudgmentDisposition(const model::CaseDefinition& case_definition,
                                            const model::WorkflowJudgmentDisposition& disposition) {
    if (const auto* legacy = std::get_if<std::string>(&disposition)) {
        return !usesStructuredDisposition(case_definition) && validText(*legacy, 4096);
    }
    const auto& plan = std::get<model::DispositionPlan>(disposition);
    const auto* authored =
        case_definition.authored_disposition_plan_id.has_value()
            ? dispositionPlanFor(case_definition, *case_definition.authored_disposition_plan_id)
            : nullptr;
    return authored != nullptr && plan == *authored;
}

[[nodiscard]] auto validateDefinition(const model::WorkflowDefinition& workflow,
                                      const model::CaseDefinition& case_definition,
                                      const model::WorkflowState& state)
    -> std::expected<void, WorkflowError> {
    if (!validNamespacedId(workflow.id.value) || workflow.stages.empty() ||
        workflow.stages.size() > max_stages || workflow.operations.empty() ||
        workflow.operations.size() > max_operations || workflow.filing_routes.empty() ||
        workflow.filing_routes.size() > max_routes ||
        workflow.calendar.holidays.size() > max_holidays ||
        std::ranges::find(workflow.stages, workflow.initial_stage_id) == workflow.stages.end() ||
        hasDuplicates(workflow.stages, [](const auto& stage) { return stage.value; }) ||
        hasDuplicates(workflow.operations,
                      [](const auto& operation) { return operation.id.value; })) {
        return fail(WorkflowErrorCode::InvalidDefinition, "invalid workflow identity or inventory");
    }
    if (std::ranges::any_of(workflow.stages,
                            [](const auto& stage) { return !validNamespacedId(stage.value); }) ||
        std::ranges::any_of(workflow.calendar.holidays,
                            [](const auto& holiday) { return !validLegalDate(holiday); }) ||
        hasDuplicates(workflow.calendar.holidays, [](const auto& holiday) {
            return std::to_string(std::chrono::sys_days{holiday.value}.time_since_epoch().count());
        })) {
        return fail(WorkflowErrorCode::InvalidDefinition, "invalid stages or court holidays");
    }

    const auto uses_canonical_authority =
        workflow.operations.front().authority.primary.provenance.has_value();
    const auto uses_schema3_contract =
        !case_definition.disposition_targets.empty() ||
        !case_definition.disposition_plans.empty() ||
        case_definition.authored_disposition_plan_id.has_value() ||
        std::ranges::any_of(workflow.operations,
                            [](const auto& operation) { return !operation.preconditions.empty(); });
    if (uses_schema3_contract && !uses_canonical_authority) {
        return fail(WorkflowErrorCode::InvalidDefinition,
                    "structured dispositions and preconditions require canonical authority");
    }
    for (const auto& operation : workflow.operations) {
        if (!validNamespacedId(operation.id.value) || !validOpcode(operation.opcode) ||
            std::ranges::find(workflow.stages, operation.stage_id) == workflow.stages.end() ||
            (operation.next_stage_id.has_value() &&
             std::ranges::find(workflow.stages, *operation.next_stage_id) ==
                 workflow.stages.end()) ||
            operation.authorized_roles.size() > max_route_items ||
            !preconditionsAreConsistent(operation.preconditions) ||
            hasDuplicates(operation.authorized_roles,
                          [](const auto& role) { return role.value; }) ||
            std::ranges::any_of(operation.authorized_roles,
                                [](const auto& role) { return !validNamespacedId(role.value); })) {
            return fail(WorkflowErrorCode::InvalidDefinition,
                        "operation references an unknown stage");
        }
        if (!validAuthority(operation.authority)) {
            return fail(WorkflowErrorCode::MissingAuthority,
                        "every workflow operation requires complete versioned authority");
        }
        if (operation.authority.primary.provenance.has_value() != uses_canonical_authority) {
            return fail(WorkflowErrorCode::MissingAuthority,
                        "a workflow cannot mix legacy and canonical authority generations");
        }
        const auto may_calculate_deadline =
            operation.opcode == model::WorkflowOpcode::CalculateDeadline ||
            operation.opcode == model::WorkflowOpcode::EnterOrder;
        const auto has_deadline_rule = operation.deadline_days.has_value();
        if (operation.deadline_days.has_value() != operation.deadline_counting.has_value() ||
            (operation.opcode == model::WorkflowOpcode::CalculateDeadline && !has_deadline_rule) ||
            (!may_calculate_deadline && has_deadline_rule) ||
            (operation.deadline_days.has_value() && *operation.deadline_days > 3650) ||
            (operation.deadline_counting.has_value() &&
             !validDeadlineCounting(*operation.deadline_counting))) {
            return fail(WorkflowErrorCode::InvalidDefinition,
                        "deadline operation has invalid parameters");
        }
        if (operation.opcode == model::WorkflowOpcode::AdvanceStage &&
            !operation.next_stage_id.has_value()) {
            return fail(WorkflowErrorCode::InvalidDefinition,
                        "advance_stage requires next_stage_id");
        }
        const auto may_advance = operation.opcode == model::WorkflowOpcode::AdvanceStage ||
                                 operation.opcode == model::WorkflowOpcode::ScheduleArgument ||
                                 operation.opcode == model::WorkflowOpcode::IssueJudgment ||
                                 operation.opcode == model::WorkflowOpcode::IssueMandate;
        if (!may_advance && operation.next_stage_id.has_value()) {
            return fail(WorkflowErrorCode::InvalidDefinition,
                        "operation opcode cannot carry next_stage_id");
        }
        if ((operation.opcode == model::WorkflowOpcode::EnterOrder ||
             operation.opcode == model::WorkflowOpcode::SetSealed ||
             operation.opcode == model::WorkflowOpcode::ScheduleArgument ||
             operation.opcode == model::WorkflowOpcode::IssueJudgment ||
             operation.opcode == model::WorkflowOpcode::IssueMandate) &&
            operation.authorized_roles.empty()) {
            return fail(WorkflowErrorCode::InvalidDefinition,
                        "court operation requires authorized roles");
        }
    }
    if (case_definition.authored_disposition_operation_id.has_value()) {
        const auto* authored_operation =
            operationFor(workflow, *case_definition.authored_disposition_operation_id);
        if (authored_operation == nullptr ||
            authored_operation->opcode != model::WorkflowOpcode::IssueJudgment) {
            return fail(WorkflowErrorCode::InvalidCase,
                        "authored disposition operation is not a judgment operation");
        }
    }

    if (hasDuplicates(workflow.filing_routes, [](const auto& route) {
            return route.stage_id.value + "\n" + route.filing_type.value;
        })) {
        return fail(WorkflowErrorCode::InvalidDefinition, "duplicate filing route");
    }
    std::unordered_set<std::string> declared_deadline_ids;
    std::unordered_set<std::string> produced_deadline_ids;
    std::unordered_set<std::string> filing_types;
    for (const auto& route : workflow.filing_routes) {
        const auto* accept = operationFor(workflow, route.accept_operation_id);
        const auto* reject = operationFor(workflow, route.reject_operation_id);
        const auto* deficiency = route.deficiency_operation_id
                                     ? operationFor(workflow, *route.deficiency_operation_id)
                                     : nullptr;
        const auto* deficiency_deadline =
            route.deficiency_deadline
                ? operationFor(workflow, route.deficiency_deadline->operation_id)
                : nullptr;
        const auto* accepted_deadline =
            route.accepted_deadline ? operationFor(workflow, route.accepted_deadline->operation_id)
                                    : nullptr;
        const auto* advance = route.advance_operation_id
                                  ? operationFor(workflow, *route.advance_operation_id)
                                  : nullptr;
        if (!validNamespacedId(route.filing_type.value) ||
            std::ranges::find(workflow.stages, route.stage_id) == workflow.stages.end() ||
            route.authorized_roles.empty() || route.authorized_roles.size() > max_route_items ||
            route.required_fields.size() > max_route_items ||
            route.required_service_roles.size() > max_route_items ||
            hasDuplicates(route.authorized_roles, [](const auto& role) { return role.value; }) ||
            hasDuplicates(route.required_fields, [](const auto& field) { return field.value; }) ||
            hasDuplicates(route.required_service_roles,
                          [](const auto& role) { return role.value; }) ||
            std::ranges::any_of(route.authorized_roles,
                                [](const auto& role) { return !validNamespacedId(role.value); }) ||
            std::ranges::any_of(
                route.required_fields,
                [](const auto& field) { return !validNamespacedId(field.value); }) ||
            std::ranges::any_of(route.required_service_roles,
                                [](const auto& role) { return !validNamespacedId(role.value); }) ||
            accept == nullptr || reject == nullptr ||
            accept->opcode != model::WorkflowOpcode::AcceptFiling ||
            reject->opcode != model::WorkflowOpcode::RejectFiling ||
            (route.deficiency_operation_id.has_value() &&
             (deficiency == nullptr ||
              deficiency->opcode != model::WorkflowOpcode::IssueDeficiency)) ||
            (route.deficiency_deadline.has_value() &&
             (!route.deficiency_operation_id.has_value() || deficiency_deadline == nullptr ||
              deficiency_deadline->opcode != model::WorkflowOpcode::CalculateDeadline ||
              !validNamespacedId(route.deficiency_deadline->deadline_id.value))) ||
            (route.accepted_deadline.has_value() &&
             (accepted_deadline == nullptr ||
              accepted_deadline->opcode != model::WorkflowOpcode::CalculateDeadline)) ||
            (route.advance_operation_id.has_value() &&
             (advance == nullptr || advance->opcode != model::WorkflowOpcode::AdvanceStage ||
              !advance->authorized_roles.empty()))) {
            return fail(WorkflowErrorCode::InvalidDefinition,
                        "filing route references incompatible operations");
        }
        if (route.accepted_deadline.has_value() &&
            (!validNamespacedId(route.accepted_deadline->deadline_id.value) ||
             !declared_deadline_ids.emplace(route.accepted_deadline->deadline_id.value).second ||
             !produced_deadline_ids.emplace(route.accepted_deadline->deadline_id.value).second)) {
            return fail(WorkflowErrorCode::InvalidDefinition,
                        "accepted deadline identifiers must be valid and unique");
        }
        if (route.deficiency_deadline.has_value() &&
            !declared_deadline_ids.emplace(route.deficiency_deadline->deadline_id.value).second) {
            return fail(WorkflowErrorCode::InvalidDefinition,
                        "deficiency deadline identifiers must be unique");
        }
        if (route.satisfies_deadline_id.has_value() &&
            !validNamespacedId(route.satisfies_deadline_id->value)) {
            return fail(WorkflowErrorCode::InvalidDefinition,
                        "filing route has an invalid deadline reference");
        }
        for (const auto* operation :
             {accept, reject, deficiency, deficiency_deadline, accepted_deadline, advance}) {
            if (operation != nullptr && operation->stage_id != route.stage_id) {
                return fail(WorkflowErrorCode::InvalidDefinition,
                            "filing-route operations must share its stage");
            }
        }
        filing_types.emplace(route.filing_type.value);
    }
    for (const auto& route : workflow.filing_routes) {
        if (route.satisfies_deadline_id.has_value() &&
            !produced_deadline_ids.contains(route.satisfies_deadline_id->value)) {
            return fail(WorkflowErrorCode::InvalidDefinition,
                        "filing route references an unproduced deadline");
        }
    }
    for (const auto& operation : workflow.operations) {
        if (std::ranges::any_of(operation.preconditions, [&](const auto& precondition) {
                const auto* filing = std::get_if<model::WorkflowFilingPrecondition>(&precondition);
                return filing != nullptr && !filing_types.contains(filing->filing_type.value);
            })) {
            return fail(WorkflowErrorCode::InvalidDefinition,
                        "filing precondition references an unknown filing type");
        }
    }

    if (!validNamespacedId(case_definition.id.value) ||
        !validNamespacedId(case_definition.procedure_id.value) || case_definition.actors.empty() ||
        case_definition.actors.size() > max_case_actors ||
        !validDispositionInventory(case_definition) ||
        hasDuplicates(case_definition.actors, [](const auto& actor) { return actor.id.value; }) ||
        std::ranges::any_of(case_definition.actors, [](const auto& actor) {
            return !validNamespacedId(actor.id.value) || !validNamespacedId(actor.role.value);
        })) {
        return fail(WorkflowErrorCode::InvalidCase, "invalid case actors");
    }
    const auto histories_are_bounded = state.decided_commands.size() <= max_state_items &&
                                       state.accepted_filings.size() <= max_state_items &&
                                       state.deadlines.size() <= max_state_items &&
                                       state.deficiencies.size() <= max_state_items &&
                                       state.orders.size() <= max_state_items;
    if (!validNamespacedId(state.session_id) || state.workflow_id != workflow.id ||
        std::ranges::find(workflow.stages, state.current_stage_id) == workflow.stages.end() ||
        state.next_event_sequence == 0 || !histories_are_bounded ||
        hasDuplicates(state.decided_commands, [](const auto& command) { return command.value; }) ||
        hasDuplicates(state.accepted_filings,
                      [](const auto& filing) { return filing.filing_id.value; }) ||
        hasDuplicates(state.deadlines,
                      [](const auto& deadline) { return deadline.deadline_id.value; }) ||
        hasDuplicates(state.deficiencies,
                      [](const auto& deficiency) { return deficiency.deficiency_id.value; }) ||
        hasDuplicates(state.orders, [](const auto& order) { return order.order_id.value; })) {
        return fail(WorkflowErrorCode::InvalidState, "invalid workflow state");
    }

    const auto valid_pending =
        !state.pending_command.has_value() ||
        (validNamespacedId(state.pending_command->command_id.value) &&
         state.pending_command->next_event_index > 0 &&
         state.pending_command->event_count <= max_events_per_command &&
         state.pending_command->next_event_index < state.pending_command->event_count &&
         std::ranges::find(state.decided_commands, state.pending_command->command_id) ==
             state.decided_commands.end());
    const auto applied_event_count = state.next_event_sequence - 1;
    const auto pending_event_count = state.pending_command.has_value()
                                         ? state.pending_command->next_event_index
                                         : std::uint32_t{0};
    const auto minimum_event_count = state.decided_commands.size() + pending_event_count;
    const auto maximum_event_count =
        state.decided_commands.size() * max_events_per_command + pending_event_count;
    if (!valid_pending || applied_event_count < minimum_event_count ||
        applied_event_count > maximum_event_count ||
        (applied_event_count == 0) != !state.legal_time_cursor.has_value() ||
        (state.legal_time_cursor.has_value() &&
         !validLegalDate(state.legal_time_cursor->court_date)) ||
        std::ranges::any_of(state.decided_commands, [](const auto& command) {
            return !validNamespacedId(command.value);
        })) {
        return fail(WorkflowErrorCode::InvalidState, "invalid workflow event-history snapshot");
    }

    std::unordered_set<std::string> actor_ids;
    actor_ids.reserve(case_definition.actors.size());
    for (const auto& actor : case_definition.actors) {
        actor_ids.emplace(actor.id.value);
    }
    for (const auto& filing : state.accepted_filings) {
        if (!validNamespacedId(filing.filing_id.value) ||
            !validNamespacedId(filing.filing_type.value) ||
            !actor_ids.contains(filing.actor_id.value) || !validDigest(filing.document_sha256) ||
            !validLegalDate(filing.accepted_at.court_date) ||
            filing.served_actors.size() > max_case_actors ||
            hasDuplicates(filing.served_actors, [](const auto& actor) { return actor.value; }) ||
            std::ranges::any_of(filing.served_actors,
                                [&](const auto& actor) {
                                    return actor == filing.actor_id ||
                                           !actor_ids.contains(actor.value);
                                }) ||
            (state.legal_time_cursor.has_value() &&
             (filing.accepted_at.instant > state.legal_time_cursor->instant ||
              isLater(filing.accepted_at.court_date, state.legal_time_cursor->court_date)))) {
            return fail(WorkflowErrorCode::InvalidState, "invalid accepted-filing snapshot");
        }
    }
    std::unordered_map<std::string, const model::WorkflowDeadlineRecord*> deadlines;
    deadlines.reserve(state.deadlines.size());
    for (const auto& deadline : state.deadlines) {
        if (!validNamespacedId(deadline.deadline_id.value) ||
            !validDeadlinePurpose(deadline.purpose) || !validLegalDate(deadline.due_date) ||
            !validDeadlineStatus(deadline.status)) {
            return fail(WorkflowErrorCode::InvalidState, "invalid deadline snapshot");
        }
        deadlines.emplace(deadline.deadline_id.value, &deadline);
    }
    for (const auto& deficiency : state.deficiencies) {
        const auto deadline = deficiency.cure_deadline_id
                                  ? deadlines.find(deficiency.cure_deadline_id->value)
                                  : deadlines.end();
        if (!validNamespacedId(deficiency.deficiency_id.value) ||
            !validNamespacedId(deficiency.filing_id.value) ||
            !validNamespacedId(deficiency.filing_type.value) ||
            !actor_ids.contains(deficiency.actor_id.value) ||
            deficiency.missing_requirements.empty() ||
            deficiency.missing_requirements.size() > max_route_items ||
            hasDuplicates(deficiency.missing_requirements,
                          [](const auto& requirement) { return requirement.value; }) ||
            std::ranges::any_of(
                deficiency.missing_requirements,
                [](const auto& requirement) { return !validNamespacedId(requirement.value); }) ||
            (deficiency.cure_deadline_id.has_value() &&
             (!validNamespacedId(deficiency.cure_deadline_id->value) ||
              deadline == deadlines.end() ||
              deadline->second->purpose != model::WorkflowDeadlinePurpose::DeficiencyCure ||
              deficiency.cured !=
                  (deadline->second->status == model::WorkflowDeadlineStatus::Satisfied)))) {
            return fail(WorkflowErrorCode::InvalidState, "invalid deficiency snapshot");
        }
    }
    for (const auto& order : state.orders) {
        if (!validNamespacedId(order.order_id.value) || !validOrderDisposition(order.disposition) ||
            !validDigest(order.document_sha256)) {
            return fail(WorkflowErrorCode::InvalidState, "invalid order snapshot");
        }
    }
    if ((state.argument_date.has_value() && !validLegalDate(*state.argument_date)) ||
        (state.judgment_sha256.has_value() && !validDigest(*state.judgment_sha256)) ||
        (state.judgment_sha256.has_value() != state.judgment_disposition.has_value()) ||
        (state.judgment_disposition.has_value() &&
         !validJudgmentDisposition(case_definition, *state.judgment_disposition)) ||
        (state.mandate_sha256.has_value() && !validDigest(*state.mandate_sha256)) ||
        (state.mandate_sha256.has_value() && !state.judgment_sha256.has_value()) ||
        (state.next_event_sequence == 1 &&
         (state.current_stage_id != workflow.initial_stage_id ||
          state.pending_command.has_value() || !state.decided_commands.empty() ||
          !state.accepted_filings.empty() || !state.deadlines.empty() ||
          !state.deficiencies.empty() || !state.orders.empty() || state.sealed ||
          state.argument_date.has_value() || state.judgment_sha256.has_value() ||
          state.mandate_sha256.has_value() || state.legal_time_cursor.has_value() ||
          state.judgment_disposition.has_value()))) {
        return fail(WorkflowErrorCode::InvalidState, "invalid adjudicative snapshot");
    }
    return {};
}

[[nodiscard]] auto commandHeader(const model::WorkflowCommand& command)
    -> const model::WorkflowCommandHeader& {
    return std::visit(
        [](const auto& concrete) -> const model::WorkflowCommandHeader& { return concrete.header; },
        command);
}

[[nodiscard]] bool commandWasDecided(const model::WorkflowState& state,
                                     const model::WorkflowCommandId& id) {
    return std::ranges::find(state.decided_commands, id) != state.decided_commands.end();
}

[[nodiscard]] auto makeHeader(const model::WorkflowDefinition& workflow,
                              const model::WorkflowState& state,
                              const model::WorkflowCommandHeader& command,
                              const model::WorkflowOperation& operation, std::uint32_t index,
                              std::uint32_t count) -> model::WorkflowEventHeader {
    return model::WorkflowEventHeader{state.session_id,
                                      workflow.id,
                                      command.command_id,
                                      operation.id,
                                      state.next_event_sequence + index,
                                      index,
                                      count,
                                      command.occurred_at,
                                      operation.authority,
                                      operation.preconditions};
}

[[nodiscard]] auto
rejectFiling(const model::WorkflowDefinition& workflow, const model::WorkflowState& state,
             const model::WorkflowFilingRoute& route, const model::SubmitWorkflowFiling& command,
             model::WorkflowFilingRejectionReason reason)
    -> std::expected<std::vector<model::WorkflowEvent>, WorkflowError> {
    const auto* operation = operationFor(workflow, route.reject_operation_id);
    if (operation == nullptr) {
        return fail(WorkflowErrorCode::InvalidDefinition,
                    "filing route has no compatible rejection operation");
    }
    return std::vector<model::WorkflowEvent>{model::WorkflowFilingRejected{
        makeHeader(workflow, state, command.header, *operation, 0, 1), command.filing_id,
        command.filing_type, command.header.actor_id, reason}};
}

[[nodiscard]] auto requiredServiceActors(const model::WorkflowFilingRoute& route,
                                         const model::CaseDefinition& case_definition,
                                         const model::ActorId& filer)
    -> std::vector<model::ActorId> {
    std::vector<model::ActorId> required;
    for (const auto& actor : case_definition.actors) {
        if (actor.id != filer && roleAllowed(route.required_service_roles, actor.role)) {
            required.push_back(actor.id);
        }
    }
    std::ranges::sort(required, {}, &model::ActorId::value);
    return required;
}

[[nodiscard]] auto missingRequirements(const model::WorkflowFilingRoute& route,
                                       const model::CaseDefinition& case_definition,
                                       const model::SubmitWorkflowFiling& command)
    -> std::vector<model::WorkflowRequirementId> {
    std::vector<model::WorkflowRequirementId> missing;
    for (const auto& field : route.required_fields) {
        const auto supplied =
            std::ranges::find(command.fields, field, &model::WorkflowFieldValue::id);
        if (supplied == command.fields.end() || supplied->value.empty()) {
            missing.push_back(model::WorkflowRequirementId{field.value});
        }
    }
    for (const auto& actor :
         requiredServiceActors(route, case_definition, command.header.actor_id)) {
        if (std::ranges::find(command.served_actors, actor) == command.served_actors.end()) {
            missing.push_back(model::WorkflowRequirementId{"service." + actor.value});
        }
    }
    std::ranges::sort(missing, {}, &model::WorkflowRequirementId::value);
    return missing;
}

[[nodiscard]] auto decideFiling(const model::WorkflowDefinition& workflow,
                                const model::CaseDefinition& case_definition,
                                const model::WorkflowState& state,
                                const model::SubmitWorkflowFiling& command)
    -> std::expected<std::vector<model::WorkflowEvent>, WorkflowError> {
    if (!validNamespacedId(command.filing_id.value) ||
        !validNamespacedId(command.filing_type.value) || !validDigest(command.document_sha256) ||
        command.fields.size() > max_route_items || command.served_actors.size() > max_case_actors ||
        hasDuplicates(command.fields, [](const auto& field) { return field.id.value; }) ||
        hasDuplicates(command.served_actors, [](const auto& actor) { return actor.value; }) ||
        std::ranges::any_of(command.fields,
                            [](const auto& field) {
                                return !validNamespacedId(field.id.value) ||
                                       field.value.size() > 4096;
                            }) ||
        std::ranges::any_of(command.served_actors,
                            [&](const auto& served) {
                                return !validNamespacedId(served.value) ||
                                       actorFor(case_definition, served) == nullptr;
                            }) ||
        (command.cures_deficiency_id.has_value() &&
         !validNamespacedId(command.cures_deficiency_id->value)) ||
        std::ranges::find(state.accepted_filings, command.filing_id,
                          &model::WorkflowFilingRecord::filing_id) !=
            state.accepted_filings.end() ||
        std::ranges::find(state.deficiencies, command.filing_id,
                          &model::WorkflowDeficiencyRecord::filing_id) !=
            state.deficiencies.end() ||
        std::ranges::find(command.served_actors, command.header.actor_id) !=
            command.served_actors.end()) {
        return fail(WorkflowErrorCode::InvalidCommand, "invalid filing command");
    }
    const auto* actor = actorFor(case_definition, command.header.actor_id);
    if (actor == nullptr) {
        return fail(WorkflowErrorCode::UnknownActor, "filing actor is not part of the case");
    }
    const auto* route = routeFor(workflow, state.current_stage_id, command.filing_type);
    if (route == nullptr) {
        return fail(WorkflowErrorCode::InvalidCommand,
                    "filing type has no executable route in the current stage");
    }
    if (!roleAllowed(route->authorized_roles, actor->role)) {
        return rejectFiling(workflow, state, *route, command,
                            model::WorkflowFilingRejectionReason::UnauthorizedActor);
    }

    if (command.cures_deficiency_id.has_value()) {
        const auto* deficiency = deficiencyFor(state, *command.cures_deficiency_id);
        if (deficiency == nullptr || deficiency->cured ||
            deficiency->filing_type != command.filing_type ||
            deficiency->actor_id != command.header.actor_id) {
            return rejectFiling(workflow, state, *route, command,
                                model::WorkflowFilingRejectionReason::UnknownDeficiency);
        }
        if (deficiency->cure_deadline_id.has_value()) {
            const auto* deadline = deadlineFor(state, *deficiency->cure_deadline_id);
            if (deadline == nullptr || deadline->status != model::WorkflowDeadlineStatus::Open ||
                isLater(command.header.occurred_at.court_date, deadline->due_date)) {
                return rejectFiling(workflow, state, *route, command,
                                    model::WorkflowFilingRejectionReason::DeadlineExpired);
            }
        }
    }
    if (route->satisfies_deadline_id.has_value()) {
        const auto* deadline = deadlineFor(state, *route->satisfies_deadline_id);
        if (deadline == nullptr || deadline->status != model::WorkflowDeadlineStatus::Open) {
            return rejectFiling(workflow, state, *route, command,
                                model::WorkflowFilingRejectionReason::IneligibleFiling);
        }
        if (route->reject_after_deadline &&
            isLater(command.header.occurred_at.court_date, deadline->due_date)) {
            return rejectFiling(workflow, state, *route, command,
                                model::WorkflowFilingRejectionReason::DeadlineExpired);
        }
    }

    auto missing = missingRequirements(*route, case_definition, command);
    if (!missing.empty()) {
        if (!route->deficiency_operation_id.has_value()) {
            return rejectFiling(workflow, state, *route, command,
                                model::WorkflowFilingRejectionReason::NonconformingFiling);
        }
        const auto* deficiency = operationFor(workflow, *route->deficiency_operation_id);
        const auto* calculation =
            route->deficiency_deadline
                ? operationFor(workflow, route->deficiency_deadline->operation_id)
                : nullptr;
        const auto event_count =
            static_cast<std::uint32_t>(1 + route->deficiency_deadline.has_value());
        const auto deficiency_id = deficiencyIdFor(command.header.command_id);
        const auto cure_deadline_id =
            route->deficiency_deadline
                ? std::optional{deficiencyDeadlineIdFor(*route->deficiency_deadline,
                                                        command.header.command_id)}
                : std::nullopt;
        if (!validNamespacedId(deficiency_id.value) ||
            (cure_deadline_id.has_value() && !validNamespacedId(cure_deadline_id->value)) ||
            state.deficiencies.size() == max_state_items ||
            (cure_deadline_id.has_value() && state.deadlines.size() == max_state_items) ||
            deficiencyFor(state, deficiency_id) != nullptr ||
            (cure_deadline_id.has_value() && deadlineFor(state, *cure_deadline_id) != nullptr)) {
            return fail(WorkflowErrorCode::InvalidCommand,
                        "deficiency instance identifiers already exist");
        }
        std::vector<model::WorkflowEvent> events;
        events.reserve(event_count);
        events.emplace_back(model::WorkflowDeficiencyIssued{
            makeHeader(workflow, state, command.header, *deficiency, 0, event_count), deficiency_id,
            command.filing_id, command.filing_type, command.header.actor_id, std::move(missing),
            cure_deadline_id});
        if (cure_deadline_id.has_value()) {
            const auto due =
                calculateDeadline(workflow.calendar, command.header.occurred_at.court_date,
                                  deadlineRule(*calculation));
            if (!validLegalDate(due)) {
                return fail(WorkflowErrorCode::InvalidCommand,
                            "deficiency deadline is outside the supported legal calendar");
            }
            events.emplace_back(model::WorkflowDeadlineCalculated{
                makeHeader(workflow, state, command.header, *calculation, 1, event_count),
                *cure_deadline_id, model::WorkflowDeadlinePurpose::DeficiencyCure,
                command.header.occurred_at.court_date, due});
        }
        return events;
    }

    if (route->accepted_deadline.has_value() &&
        deadlineFor(state, route->accepted_deadline->deadline_id) != nullptr) {
        return rejectFiling(workflow, state, *route, command,
                            model::WorkflowFilingRejectionReason::IneligibleFiling);
    }
    if (state.accepted_filings.size() == max_state_items ||
        (route->accepted_deadline.has_value() && state.deadlines.size() == max_state_items)) {
        return fail(WorkflowErrorCode::InvalidState, "workflow filing history is full");
    }

    const auto* accept = operationFor(workflow, route->accept_operation_id);
    const auto count = static_cast<std::uint32_t>(1 + route->accepted_deadline.has_value() +
                                                  route->advance_operation_id.has_value());
    std::vector<model::ActorId> served = command.served_actors;
    std::ranges::sort(served, {}, &model::ActorId::value);
    std::vector<model::WorkflowEvent> events;
    events.reserve(count);
    events.emplace_back(model::WorkflowFilingAccepted{
        makeHeader(workflow, state, command.header, *accept, 0, count), command.filing_id,
        command.filing_type, command.header.actor_id, command.document_sha256, std::move(served),
        command.cures_deficiency_id, route->satisfies_deadline_id});
    std::uint32_t index = 1;
    if (route->accepted_deadline.has_value()) {
        const auto* calculation = operationFor(workflow, route->accepted_deadline->operation_id);
        const auto due = calculateDeadline(workflow.calendar, command.header.occurred_at.court_date,
                                           deadlineRule(*calculation));
        if (!validLegalDate(due)) {
            return fail(WorkflowErrorCode::InvalidCommand,
                        "filing deadline is outside the supported legal calendar");
        }
        events.emplace_back(model::WorkflowDeadlineCalculated{
            makeHeader(workflow, state, command.header, *calculation, index++, count),
            route->accepted_deadline->deadline_id, model::WorkflowDeadlinePurpose::Filing,
            command.header.occurred_at.court_date, due});
    }
    if (route->advance_operation_id.has_value()) {
        const auto* advance = operationFor(workflow, *route->advance_operation_id);
        events.emplace_back(model::WorkflowStageAdvanced{
            makeHeader(workflow, state, command.header, *advance, index, count),
            state.current_stage_id, *advance->next_stage_id});
    }
    return events;
}

template <typename Command>
[[nodiscard]] auto requireCourtOperation(const model::WorkflowDefinition& workflow,
                                         const model::CaseDefinition& case_definition,
                                         const model::WorkflowState& state, const Command& command,
                                         model::WorkflowOpcode opcode)
    -> std::expected<const model::WorkflowOperation*, WorkflowError> {
    if (!validNamespacedId(command.operation_id.value)) {
        return fail(WorkflowErrorCode::InvalidCommand, "invalid court operation identifier");
    }
    const auto* operation = operationFor(workflow, command.operation_id);
    const auto* actor = actorFor(case_definition, command.header.actor_id);
    if (actor == nullptr) {
        return fail(WorkflowErrorCode::UnknownActor, "court actor is not part of the case");
    }
    if (operation == nullptr || operation->opcode != opcode ||
        operation->stage_id != state.current_stage_id) {
        return fail(WorkflowErrorCode::InvalidCommand,
                    "operation is not eligible in the current stage");
    }
    if (!roleAllowed(operation->authorized_roles, actor->role)) {
        return fail(WorkflowErrorCode::UnauthorizedActor,
                    "actor is not authorized for the court operation");
    }
    return operation;
}

[[nodiscard]] auto
decideOrder(const model::WorkflowDefinition& workflow, const model::CaseDefinition& case_definition,
            const model::WorkflowState& state, const model::EnterWorkflowOrder& command)
    -> std::expected<std::vector<model::WorkflowEvent>, WorkflowError> {
    const auto operation = requireCourtOperation(workflow, case_definition, state, command,
                                                 model::WorkflowOpcode::EnterOrder);
    if (!operation) {
        return std::unexpected(operation.error());
    }
    if (!validText(command.order_id.value) || !validDigest(command.document_sha256) ||
        !validNamespacedId(command.operation_id.value) ||
        !validNamespacedId(command.order_id.value) || !validOrderDisposition(command.disposition) ||
        state.orders.size() == max_state_items ||
        (command.extension_deadline_id.has_value() &&
         !validNamespacedId(command.extension_deadline_id->value)) ||
        std::ranges::find(state.orders, command.order_id, &model::WorkflowOrderRecord::order_id) !=
            state.orders.end()) {
        return fail(WorkflowErrorCode::InvalidCommand, "invalid or duplicate court order");
    }

    std::optional<model::WorkflowDeadlineExtension> extension;
    if (command.extension_deadline_id.has_value()) {
        const auto* existing = deadlineFor(state, *command.extension_deadline_id);
        if (existing == nullptr || existing->status != model::WorkflowDeadlineStatus::Open) {
            return fail(WorkflowErrorCode::InvalidCommand, "extension deadline is not open");
        }
        if (command.disposition == model::WorkflowOrderDisposition::Granted) {
            if (!(**operation).deadline_days.has_value()) {
                return fail(WorkflowErrorCode::InvalidDefinition,
                            "extension order has no deadline calculation");
            }
            const auto extended_due_date =
                calculateDeadline(workflow.calendar, existing->due_date, deadlineRule(**operation));
            if (!validLegalDate(extended_due_date)) {
                return fail(WorkflowErrorCode::InvalidCommand,
                            "extension is outside the supported legal calendar");
            }
            extension = model::WorkflowDeadlineExtension{*command.extension_deadline_id,
                                                         existing->due_date, extended_due_date};
        }
    }
    return std::vector<model::WorkflowEvent>{model::WorkflowOrderEntered{
        makeHeader(workflow, state, command.header, **operation, 0, 1), command.order_id,
        command.disposition, command.document_sha256, extension}};
}

[[nodiscard]] auto decideDeadline(const model::WorkflowDefinition& workflow,
                                  const model::CaseDefinition& case_definition,
                                  const model::WorkflowState& state,
                                  const model::CalculateWorkflowDeadline& command)
    -> std::expected<std::vector<model::WorkflowEvent>, WorkflowError> {
    const auto operation = requireCourtOperation(workflow, case_definition, state, command,
                                                 model::WorkflowOpcode::CalculateDeadline);
    if (!operation) {
        return std::unexpected(operation.error());
    }
    if (!validNamespacedId(command.deadline_id.value) ||
        state.deadlines.size() == max_state_items ||
        deadlineFor(state, command.deadline_id) != nullptr ||
        !(**operation).deadline_days.has_value() || !(**operation).deadline_counting.has_value()) {
        return fail(WorkflowErrorCode::InvalidCommand,
                    "invalid or duplicate court-calculated deadline");
    }
    const auto due = calculateDeadline(workflow.calendar, command.header.occurred_at.court_date,
                                       deadlineRule(**operation));
    if (!validLegalDate(due)) {
        return fail(WorkflowErrorCode::InvalidCommand,
                    "court-calculated deadline is outside the supported legal calendar");
    }
    return std::vector<model::WorkflowEvent>{model::WorkflowDeadlineCalculated{
        makeHeader(workflow, state, command.header, **operation, 0, 1), command.deadline_id,
        model::WorkflowDeadlinePurpose::Filing, command.header.occurred_at.court_date, due}};
}

[[nodiscard]] auto decideAdvance(const model::WorkflowDefinition& workflow,
                                 const model::CaseDefinition& case_definition,
                                 const model::WorkflowState& state,
                                 const model::AdvanceWorkflowStage& command)
    -> std::expected<std::vector<model::WorkflowEvent>, WorkflowError> {
    const auto operation = requireCourtOperation(workflow, case_definition, state, command,
                                                 model::WorkflowOpcode::AdvanceStage);
    if (!operation) {
        return std::unexpected(operation.error());
    }
    if (!(**operation).next_stage_id.has_value()) {
        return fail(WorkflowErrorCode::InvalidDefinition,
                    "advance operation has no destination stage");
    }
    return std::vector<model::WorkflowEvent>{
        model::WorkflowStageAdvanced{makeHeader(workflow, state, command.header, **operation, 0, 1),
                                     state.current_stage_id, *(**operation).next_stage_id}};
}

template <typename Event>
[[nodiscard]] auto oneEvent(const model::WorkflowDefinition& workflow,
                            const model::WorkflowState& state,
                            const model::WorkflowCommandHeader& header,
                            const model::WorkflowOperation& operation, Event event)
    -> std::vector<model::WorkflowEvent> {
    event.header = makeHeader(workflow, state, header, operation, 0, 1);
    return {model::WorkflowEvent{std::move(event)}};
}

[[nodiscard]] auto eventHeader(const model::WorkflowEvent& event)
    -> const model::WorkflowEventHeader& {
    return std::visit(
        [](const auto& concrete) -> const model::WorkflowEventHeader& { return concrete.header; },
        event);
}

[[nodiscard]] auto expectedOpcode(const model::WorkflowEvent& event) -> model::WorkflowOpcode {
    return std::visit(
        [](const auto& concrete) {
            using Event = std::remove_cvref_t<decltype(concrete)>;
            if constexpr (std::same_as<Event, model::WorkflowFilingAccepted>) {
                return model::WorkflowOpcode::AcceptFiling;
            } else if constexpr (std::same_as<Event, model::WorkflowFilingRejected>) {
                return model::WorkflowOpcode::RejectFiling;
            } else if constexpr (std::same_as<Event, model::WorkflowDeficiencyIssued>) {
                return model::WorkflowOpcode::IssueDeficiency;
            } else if constexpr (std::same_as<Event, model::WorkflowDeadlineCalculated>) {
                return model::WorkflowOpcode::CalculateDeadline;
            } else if constexpr (std::same_as<Event, model::WorkflowOrderEntered>) {
                return model::WorkflowOpcode::EnterOrder;
            } else if constexpr (std::same_as<Event, model::WorkflowStageAdvanced>) {
                return model::WorkflowOpcode::AdvanceStage;
            } else if constexpr (std::same_as<Event, model::WorkflowSealedSet>) {
                return model::WorkflowOpcode::SetSealed;
            } else if constexpr (std::same_as<Event, model::WorkflowArgumentScheduled>) {
                return model::WorkflowOpcode::ScheduleArgument;
            } else if constexpr (std::same_as<Event, model::WorkflowJudgmentIssued>) {
                return model::WorkflowOpcode::IssueJudgment;
            } else {
                return model::WorkflowOpcode::IssueMandate;
            }
        },
        event);
}

[[nodiscard]] auto validateEventEnvelope(const model::WorkflowDefinition& workflow,
                                         const model::WorkflowState& state,
                                         const model::WorkflowEvent& event)
    -> std::expected<const model::WorkflowOperation*, WorkflowError> {
    const auto& header = eventHeader(event);
    const auto* operation = operationFor(workflow, header.operation_id);
    if (!validNamespacedId(header.session_id) || !validNamespacedId(header.command_id.value) ||
        !validNamespacedId(header.operation_id.value) || header.session_id != state.session_id ||
        header.workflow_id != workflow.id || header.sequence != state.next_event_sequence ||
        header.sequence == std::numeric_limits<std::uint64_t>::max() ||
        header.command_event_count == 0 || header.command_event_count > max_events_per_command ||
        header.command_event_index >= header.command_event_count ||
        !validLegalDate(header.occurred_at.court_date) || operation == nullptr ||
        operation->opcode != expectedOpcode(event) ||
        operation->stage_id != state.current_stage_id || !validAuthority(header.authority) ||
        header.authority != operation->authority ||
        header.preconditions != operation->preconditions) {
        return fail(WorkflowErrorCode::InvalidEvent, "event envelope is inconsistent");
    }
    if (state.legal_time_cursor.has_value() &&
        (header.occurred_at.instant < state.legal_time_cursor->instant ||
         isLater(state.legal_time_cursor->court_date, header.occurred_at.court_date) ||
         (state.pending_command.has_value() && header.occurred_at != *state.legal_time_cursor))) {
        return fail(WorkflowErrorCode::InvalidEvent, "event backdates its command group");
    }
    if (state.pending_command.has_value()) {
        if (state.pending_command->command_id != header.command_id ||
            state.pending_command->next_event_index != header.command_event_index ||
            state.pending_command->event_count != header.command_event_count) {
            return fail(WorkflowErrorCode::InvalidEvent, "event command grouping is inconsistent");
        }
    } else if (header.command_event_index != 0 || commandWasDecided(state, header.command_id)) {
        return fail(WorkflowErrorCode::InvalidEvent, "event starts an invalid command group");
    }
    return operation;
}

void advanceEventEnvelope(model::WorkflowState& state, const model::WorkflowEventHeader& header) {
    state.legal_time_cursor = header.occurred_at;
    ++state.next_event_sequence;
    if (header.command_event_index + 1 == header.command_event_count) {
        state.pending_command.reset();
        state.decided_commands.push_back(header.command_id);
    } else {
        state.pending_command = model::WorkflowPendingCommand{
            header.command_id, header.command_event_index + 1, header.command_event_count};
    }
}

} // namespace

static std::expected<std::vector<model::WorkflowEvent>, WorkflowError>
decideWorkflowImpl(const model::WorkflowDefinition& workflow,
                   const model::CaseDefinition& case_definition, const model::WorkflowState& state,
                   const model::WorkflowCommand& command, bool validate_inputs) {
    if (validate_inputs) {
        if (const auto valid = validateDefinition(workflow, case_definition, state); !valid) {
            return std::unexpected(valid.error());
        }
    }
    const auto& header = commandHeader(command);
    if (!validNamespacedId(header.session_id) || header.session_id != state.session_id ||
        !validNamespacedId(header.command_id.value) || !validNamespacedId(header.actor_id.value) ||
        !validLegalDate(header.occurred_at.court_date) || state.pending_command.has_value() ||
        state.decided_commands.size() == max_state_items) {
        return fail(WorkflowErrorCode::InvalidCommand, "invalid explicit workflow input");
    }
    if (state.legal_time_cursor.has_value() &&
        (header.occurred_at.instant < state.legal_time_cursor->instant ||
         isLater(state.legal_time_cursor->court_date, header.occurred_at.court_date))) {
        return fail(WorkflowErrorCode::BackdatedCommand,
                    "command precedes the explicit legal-time cursor");
    }
    if (commandWasDecided(state, header.command_id)) {
        return fail(WorkflowErrorCode::DuplicateCommand, "command was already decided");
    }

    const auto result = std::visit(
        [&](const auto& concrete)
            -> std::expected<std::vector<model::WorkflowEvent>, WorkflowError> {
            using Command = std::remove_cvref_t<decltype(concrete)>;
            if constexpr (std::same_as<Command, model::SubmitWorkflowFiling>) {
                return decideFiling(workflow, case_definition, state, concrete);
            } else if constexpr (std::same_as<Command, model::EnterWorkflowOrder>) {
                return decideOrder(workflow, case_definition, state, concrete);
            } else if constexpr (std::same_as<Command, model::CalculateWorkflowDeadline>) {
                return decideDeadline(workflow, case_definition, state, concrete);
            } else if constexpr (std::same_as<Command, model::AdvanceWorkflowStage>) {
                return decideAdvance(workflow, case_definition, state, concrete);
            } else if constexpr (std::same_as<Command, model::SetWorkflowSealed>) {
                const auto operation = requireCourtOperation(
                    workflow, case_definition, state, concrete, model::WorkflowOpcode::SetSealed);
                if (!operation) {
                    return std::unexpected(operation.error());
                }
                return oneEvent(workflow, state, concrete.header, **operation,
                                model::WorkflowSealedSet{{}, concrete.sealed});
            } else if constexpr (std::same_as<Command, model::ScheduleWorkflowArgument>) {
                const auto operation =
                    requireCourtOperation(workflow, case_definition, state, concrete,
                                          model::WorkflowOpcode::ScheduleArgument);
                if (!operation) {
                    return std::unexpected(operation.error());
                }
                if (!validLegalDate(concrete.argument_date) ||
                    isLater(concrete.header.occurred_at.court_date, concrete.argument_date)) {
                    return fail(WorkflowErrorCode::InvalidCommand, "argument date is in the past");
                }
                return oneEvent(workflow, state, concrete.header, **operation,
                                model::WorkflowArgumentScheduled{
                                    {}, concrete.argument_date, (**operation).next_stage_id});
            } else if constexpr (std::same_as<Command, model::IssueWorkflowJudgment>) {
                const auto operation =
                    requireCourtOperation(workflow, case_definition, state, concrete,
                                          model::WorkflowOpcode::IssueJudgment);
                if (!operation) {
                    return std::unexpected(operation.error());
                }
                if (!validDigest(concrete.document_sha256) || state.judgment_sha256.has_value()) {
                    return fail(WorkflowErrorCode::InvalidCommand, "invalid judgment");
                }
                model::WorkflowJudgmentDisposition disposition;
                if (const auto* legacy = std::get_if<std::string>(&concrete.disposition)) {
                    if (usesStructuredDisposition(case_definition) || !validText(*legacy, 4096)) {
                        return fail(WorkflowErrorCode::InvalidCommand,
                                    "legacy judgment text is not eligible");
                    }
                    disposition = *legacy;
                } else {
                    const auto& selected = std::get<model::DispositionPlanId>(concrete.disposition);
                    const auto* plan = dispositionPlanFor(case_definition, selected);
                    if (!usesStructuredDisposition(case_definition) || plan == nullptr ||
                        selected != *case_definition.authored_disposition_plan_id ||
                        concrete.operation_id !=
                            *case_definition.authored_disposition_operation_id) {
                        return fail(WorkflowErrorCode::InvalidCommand,
                                    "judgment does not select the authored disposition plan");
                    }
                    disposition = *plan;
                }
                return oneEvent(workflow, state, concrete.header, **operation,
                                model::WorkflowJudgmentIssued{{},
                                                              concrete.document_sha256,
                                                              std::move(disposition),
                                                              (**operation).next_stage_id});
            } else {
                const auto operation =
                    requireCourtOperation(workflow, case_definition, state, concrete,
                                          model::WorkflowOpcode::IssueMandate);
                if (!operation) {
                    return std::unexpected(operation.error());
                }
                if (!validDigest(concrete.document_sha256) || !state.judgment_sha256.has_value() ||
                    state.mandate_sha256.has_value()) {
                    return fail(WorkflowErrorCode::InvalidCommand, "invalid mandate");
                }
                return oneEvent(workflow, state, concrete.header, **operation,
                                model::WorkflowMandateIssued{
                                    {}, concrete.document_sha256, (**operation).next_stage_id});
            }
        },
        command);
    if (!result) {
        return std::unexpected(result.error());
    }
    for (const auto& event : *result) {
        const auto& event_header = eventHeader(event);
        const auto* operation = operationFor(workflow, event_header.operation_id);
        if (operation == nullptr || !preconditionsSatisfied(operation->preconditions, state,
                                                            event_header.occurred_at.court_date)) {
            return fail(WorkflowErrorCode::UnmetPrecondition,
                        "operation preconditions are not satisfied");
        }
    }
    if (result->empty() || result->size() > max_events_per_command ||
        result->size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
        result->size() > std::numeric_limits<std::uint64_t>::max() - state.next_event_sequence) {
        return fail(WorkflowErrorCode::SequenceOverflow, "workflow event sequence overflow");
    }
    return result;
}

std::expected<std::vector<model::WorkflowEvent>, WorkflowError>
decideWorkflow(const model::WorkflowDefinition& workflow,
               const model::CaseDefinition& case_definition, const model::WorkflowState& state,
               const model::WorkflowCommand& command) {
    return decideWorkflowImpl(workflow, case_definition, state, command, true);
}

static std::expected<model::WorkflowState, WorkflowError>
applyWorkflowEvent(const model::WorkflowDefinition& workflow,
                   const model::CaseDefinition& case_definition, const model::WorkflowState& state,
                   const model::WorkflowEvent& event) {
    const auto operation = validateEventEnvelope(workflow, state, event);
    if (!operation) {
        return std::unexpected(operation.error());
    }
    auto next = state;
    const auto applied = std::visit(
        [&](const auto& concrete) -> std::expected<void, WorkflowError> {
            using Event = std::remove_cvref_t<decltype(concrete)>;
            if constexpr (std::same_as<Event, model::WorkflowFilingAccepted>) {
                const auto* route =
                    routeFor(workflow, state.current_stage_id, concrete.filing_type);
                const auto* actor = actorFor(case_definition, concrete.actor_id);
                if (route == nullptr || actor == nullptr ||
                    route->accept_operation_id != concrete.header.operation_id ||
                    !roleAllowed(route->authorized_roles, actor->role) ||
                    !validNamespacedId(concrete.filing_id.value) ||
                    state.accepted_filings.size() == max_state_items ||
                    !validDigest(concrete.document_sha256) ||
                    std::ranges::find(state.accepted_filings, concrete.filing_id,
                                      &model::WorkflowFilingRecord::filing_id) !=
                        state.accepted_filings.end()) {
                    return fail(WorkflowErrorCode::InvalidTransition, "invalid accepted filing");
                }
                if (concrete.cured_deficiency_id.has_value()) {
                    auto* deficiency = deficiencyFor(next, *concrete.cured_deficiency_id);
                    if (deficiency == nullptr || deficiency->cured) {
                        return fail(WorkflowErrorCode::InvalidTransition,
                                    "invalid deficiency cure");
                    }
                    deficiency->cured = true;
                    if (deficiency->cure_deadline_id.has_value()) {
                        auto* due = deadlineFor(next, *deficiency->cure_deadline_id);
                        if (due == nullptr || due->status != model::WorkflowDeadlineStatus::Open) {
                            return fail(WorkflowErrorCode::InvalidTransition,
                                        "invalid deficiency deadline cure");
                        }
                        due->status = model::WorkflowDeadlineStatus::Satisfied;
                    }
                }
                if (concrete.satisfied_deadline_id.has_value()) {
                    auto* due = deadlineFor(next, *concrete.satisfied_deadline_id);
                    if (due == nullptr || due->status != model::WorkflowDeadlineStatus::Open) {
                        return fail(WorkflowErrorCode::InvalidTransition,
                                    "filing cannot satisfy the deadline");
                    }
                    due->status = model::WorkflowDeadlineStatus::Satisfied;
                }
                next.accepted_filings.push_back(model::WorkflowFilingRecord{
                    concrete.filing_id, concrete.filing_type, concrete.actor_id,
                    concrete.document_sha256, concrete.header.occurred_at, concrete.served_actors});
            } else if constexpr (std::same_as<Event, model::WorkflowFilingRejected>) {
                if ((**operation).opcode != model::WorkflowOpcode::RejectFiling) {
                    return fail(WorkflowErrorCode::InvalidTransition, "invalid filing rejection");
                }
            } else if constexpr (std::same_as<Event, model::WorkflowDeficiencyIssued>) {
                const auto* route =
                    routeFor(workflow, state.current_stage_id, concrete.filing_type);
                const auto expected_deficiency_id = deficiencyIdFor(concrete.header.command_id);
                const auto expected_deadline_id =
                    route != nullptr && route->deficiency_deadline.has_value()
                        ? std::optional{deficiencyDeadlineIdFor(*route->deficiency_deadline,
                                                                concrete.header.command_id)}
                        : std::nullopt;
                if (route == nullptr || state.deficiencies.size() == max_state_items ||
                    !route->deficiency_operation_id.has_value() ||
                    *route->deficiency_operation_id != concrete.header.operation_id ||
                    concrete.deficiency_id != expected_deficiency_id ||
                    concrete.cure_deadline_id != expected_deadline_id ||
                    concrete.missing_requirements.empty() ||
                    deficiencyFor(state, concrete.deficiency_id) != nullptr) {
                    return fail(WorkflowErrorCode::InvalidTransition, "invalid deficiency");
                }
                next.deficiencies.push_back(model::WorkflowDeficiencyRecord{
                    concrete.deficiency_id, concrete.filing_id, concrete.filing_type,
                    concrete.actor_id, concrete.missing_requirements, concrete.cure_deadline_id,
                    false});
            } else if constexpr (std::same_as<Event, model::WorkflowDeadlineCalculated>) {
                if (!(**operation).deadline_days.has_value() ||
                    state.deadlines.size() == max_state_items ||
                    !validNamespacedId(concrete.deadline_id.value) ||
                    !validDeadlinePurpose(concrete.purpose) ||
                    !validLegalDate(concrete.base_date) || !validLegalDate(concrete.due_date) ||
                    deadlineFor(state, concrete.deadline_id) != nullptr ||
                    concrete.due_date != calculateDeadline(workflow.calendar, concrete.base_date,
                                                           deadlineRule(**operation))) {
                    return fail(WorkflowErrorCode::InvalidTransition, "invalid deadline");
                }
                next.deadlines.push_back(model::WorkflowDeadlineRecord{
                    concrete.deadline_id, concrete.purpose, concrete.due_date,
                    model::WorkflowDeadlineStatus::Open});
            } else if constexpr (std::same_as<Event, model::WorkflowOrderEntered>) {
                if (!validNamespacedId(concrete.order_id.value) ||
                    !validOrderDisposition(concrete.disposition) ||
                    state.orders.size() == max_state_items ||
                    !validDigest(concrete.document_sha256) ||
                    std::ranges::find(state.orders, concrete.order_id,
                                      &model::WorkflowOrderRecord::order_id) !=
                        state.orders.end()) {
                    return fail(WorkflowErrorCode::InvalidTransition, "invalid court order");
                }
                if (concrete.extension.has_value()) {
                    auto* due = deadlineFor(next, concrete.extension->deadline_id);
                    if (due == nullptr || due->status != model::WorkflowDeadlineStatus::Open ||
                        due->due_date != concrete.extension->previous_due_date ||
                        !(**operation).deadline_days.has_value() ||
                        concrete.extension->extended_due_date !=
                            calculateDeadline(workflow.calendar, due->due_date,
                                              deadlineRule(**operation))) {
                        return fail(WorkflowErrorCode::InvalidTransition,
                                    "invalid extension order");
                    }
                    due->due_date = concrete.extension->extended_due_date;
                }
                next.orders.push_back(model::WorkflowOrderRecord{
                    concrete.order_id, concrete.disposition, concrete.document_sha256});
            } else if constexpr (std::same_as<Event, model::WorkflowStageAdvanced>) {
                if (concrete.previous_stage_id != state.current_stage_id ||
                    !(**operation).next_stage_id.has_value() ||
                    concrete.next_stage_id != *(**operation).next_stage_id) {
                    return fail(WorkflowErrorCode::InvalidTransition, "invalid stage transition");
                }
                next.current_stage_id = concrete.next_stage_id;
            } else if constexpr (std::same_as<Event, model::WorkflowSealedSet>) {
                next.sealed = concrete.sealed;
            } else if constexpr (std::same_as<Event, model::WorkflowArgumentScheduled>) {
                if (!validLegalDate(concrete.argument_date) ||
                    isLater(concrete.header.occurred_at.court_date, concrete.argument_date) ||
                    concrete.next_stage_id != (**operation).next_stage_id) {
                    return fail(WorkflowErrorCode::InvalidTransition, "invalid argument schedule");
                }
                next.argument_date = concrete.argument_date;
                if (concrete.next_stage_id) {
                    next.current_stage_id = *concrete.next_stage_id;
                }
            } else if constexpr (std::same_as<Event, model::WorkflowJudgmentIssued>) {
                if (!validDigest(concrete.document_sha256) ||
                    !validJudgmentDisposition(case_definition, concrete.disposition) ||
                    state.judgment_sha256.has_value() ||
                    concrete.next_stage_id != (**operation).next_stage_id) {
                    return fail(WorkflowErrorCode::InvalidTransition, "invalid judgment");
                }
                next.judgment_sha256 = concrete.document_sha256;
                next.judgment_disposition = concrete.disposition;
                if (concrete.next_stage_id) {
                    next.current_stage_id = *concrete.next_stage_id;
                }
            } else {
                if (!validDigest(concrete.document_sha256) || !state.judgment_sha256.has_value() ||
                    state.mandate_sha256.has_value() ||
                    concrete.next_stage_id != (**operation).next_stage_id) {
                    return fail(WorkflowErrorCode::InvalidTransition, "invalid mandate");
                }
                next.mandate_sha256 = concrete.document_sha256;
                if (concrete.next_stage_id) {
                    next.current_stage_id = *concrete.next_stage_id;
                }
            }
            return {};
        },
        event);
    if (!applied) {
        return std::unexpected(applied.error());
    }
    advanceEventEnvelope(next, eventHeader(event));
    return next;
}

std::expected<model::WorkflowState, WorkflowError>
replayWorkflow(const model::WorkflowDefinition& workflow,
               const model::CaseDefinition& case_definition, model::WorkflowState initial_state,
               std::span<const model::WorkflowJournalEntry> journal) {
    if (journal.size() > max_state_items) {
        return fail(WorkflowErrorCode::InvalidEvent, "workflow journal exceeds its bound");
    }
    if (const auto valid = validateDefinition(workflow, case_definition, initial_state); !valid) {
        return std::unexpected(valid.error());
    }
    if (initial_state.next_event_sequence != 1) {
        return fail(WorkflowErrorCode::InvalidState,
                    "workflow replay requires a clean initial state");
    }
    for (const auto& entry : journal) {
        if (entry.events.empty() || entry.events.size() > max_events_per_command) {
            return fail(WorkflowErrorCode::InvalidEvent, "invalid journal event-batch size");
        }
        const auto decided =
            decideWorkflowImpl(workflow, case_definition, initial_state, entry.command, false);
        if (!decided || *decided != entry.events) {
            return fail(WorkflowErrorCode::InvalidEvent,
                        "journal events do not exactly match their recorded command");
        }
        for (const auto& event : entry.events) {
            auto next = applyWorkflowEvent(workflow, case_definition, initial_state, event);
            if (!next) {
                return std::unexpected(next.error());
            }
            initial_state = std::move(*next);
        }
        if (initial_state.pending_command.has_value()) {
            return fail(WorkflowErrorCode::InvalidEvent,
                        "journal entry ends inside a command group");
        }
    }
    if (const auto valid = validateDefinition(workflow, case_definition, initial_state); !valid) {
        return std::unexpected(valid.error());
    }
    return initial_state;
}

} // namespace appellate::engine
