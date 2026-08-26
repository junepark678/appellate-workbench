#include "appellate/engine/procedure_engine.hpp"

#include "appellate/engine/business_calendar.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <functional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace appellate::engine {
namespace {

constexpr std::size_t maximum_supporting_authorities = 32;

[[nodiscard]] auto fail(ErrorCode code, std::string message) -> std::unexpected<Error> {
    return std::unexpected(Error{code, std::move(message)});
}

[[nodiscard]] bool validNamespacedId(std::string_view value) {
    if (value.size() < 3 || value.size() > 160) {
        return false;
    }
    bool has_separator = false;
    bool previous_was_separator = true;
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        const auto alphanumeric =
            std::isdigit(character) != 0 || (character >= static_cast<unsigned char>('a') &&
                                             character <= static_cast<unsigned char>('z'));
        const auto separator = character == static_cast<unsigned char>('.') ||
                               character == static_cast<unsigned char>('-');
        if ((!alphanumeric && !separator) || (separator && previous_was_separator)) {
            return false;
        }
        has_separator = has_separator || separator;
        previous_was_separator = separator;
    }
    return has_separator && !previous_was_separator;
}

[[nodiscard]] bool validDateText(std::string_view value) {
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
           validDateText(provenance.checked_on) &&
           model::isCanonicalAuthorityText(provenance.locator, 4096) &&
           model::isCanonicalAuthoritySourceUrl(provenance.source_url);
}

[[nodiscard]] bool validAuthorityRef(const model::AuthorityRef& authority) {
    const auto legacy_complete = !authority.id.value.empty() && !authority.citation.empty() &&
                                 !authority.source_version.empty() &&
                                 !authority.proposition.empty();
    if (!legacy_complete || !authority.provenance.has_value()) {
        return legacy_complete;
    }
    return validNamespacedId(authority.id.value) &&
           model::isCanonicalAuthorityText(authority.citation, 4096) &&
           validDateText(authority.source_version) &&
           model::isCanonicalAuthorityText(authority.proposition, 4096) &&
           validProvenance(*authority.provenance) &&
           model::authorityVerificationNotBeforeSource(authority.source_version,
                                                       authority.provenance->checked_on);
}

[[nodiscard]] bool validAuthority(const model::AuthorityBasis& authority) {
    const auto has_provenance = authority.primary.provenance.has_value();
    if (!validAuthorityRef(authority.primary) ||
        authority.supporting.size() > maximum_supporting_authorities ||
        !std::ranges::all_of(authority.supporting, [&](const auto& supporting) {
            return supporting.provenance.has_value() == has_provenance &&
                   validAuthorityRef(supporting);
        })) {
        return false;
    }
    std::unordered_set<std::string> identifiers{authority.primary.id.value};
    return std::ranges::all_of(authority.supporting, [&](const auto& supporting) {
        return identifiers.emplace(supporting.id.value).second;
    });
}

[[nodiscard]] bool validDigest(std::string_view digest) {
    return digest.size() == 64 && std::ranges::all_of(digest, [](unsigned char character) {
               return std::isdigit(character) != 0 ||
                      (character >= static_cast<unsigned char>('a') &&
                       character <= static_cast<unsigned char>('f'));
           });
}

template <typename Range, typename Projection>
[[nodiscard]] bool hasDuplicates(const Range& range, Projection projection) {
    for (auto first = range.begin(); first != range.end(); ++first) {
        if (std::find_if(std::next(first), range.end(), [&](const auto& candidate) {
                return std::invoke(projection, *first) == std::invoke(projection, candidate);
            }) != range.end()) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] auto validate(const model::ProcedureDefinition& procedure,
                            const model::CaseDefinition& case_definition,
                            const model::SessionState& state) -> std::expected<void, Error> {
    const auto& rule = procedure.initiating_filing;
    if (procedure.id.value.empty() || rule.filing_type.value.empty() ||
        rule.authorized_roles.empty() || rule.cure_deadline.days == 0 ||
        std::ranges::any_of(procedure.calendar.holidays,
                            [](model::LegalDate date) { return !date.value.ok(); }) ||
        std::ranges::any_of(rule.authorized_roles,
                            [](const model::ActorRoleId& role) { return role.value.empty(); }) ||
        std::ranges::any_of(
            rule.required_fields,
            [](const model::FilingFieldId& field) { return field.value.empty(); }) ||
        hasDuplicates(rule.authorized_roles,
                      [](const model::ActorRoleId& role) { return role.value; }) ||
        hasDuplicates(rule.required_fields,
                      [](const model::FilingFieldId& field) { return field.value; })) {
        return fail(ErrorCode::InvalidDefinition, "invalid initiating-filing definition");
    }
    if (!validAuthority(rule.filing_authority) || !validAuthority(rule.actor_authority) ||
        !validAuthority(rule.deficiency_authority)) {
        return fail(ErrorCode::MissingAuthority,
                    "every rule-driven filing transition requires a complete authority");
    }
    const auto uses_canonical_authority = rule.filing_authority.primary.provenance.has_value();
    if (rule.actor_authority.primary.provenance.has_value() != uses_canonical_authority ||
        rule.deficiency_authority.primary.provenance.has_value() != uses_canonical_authority) {
        return fail(ErrorCode::MissingAuthority,
                    "a procedure cannot mix legacy and canonical authority generations");
    }
    if (case_definition.id.value.empty() || case_definition.procedure_id != procedure.id ||
        case_definition.actors.empty() ||
        hasDuplicates(case_definition.actors,
                      [](const model::CaseActor& actor) { return actor.id.value; })) {
        return fail(ErrorCode::InvalidCase, "case does not match the procedure");
    }
    if (state.id.value.empty() || state.procedure_id != procedure.id ||
        state.case_id != case_definition.id || state.next_docket_sequence == 0) {
        return fail(ErrorCode::InvalidSession, "session does not match its pinned definitions");
    }
    return {};
}

[[nodiscard]] auto actorFor(const model::CaseDefinition& case_definition,
                            const model::ActorId& actor_id) -> const model::CaseActor* {
    const auto found = std::ranges::find(case_definition.actors, actor_id, &model::CaseActor::id);
    return found == case_definition.actors.end() ? nullptr : &*found;
}

[[nodiscard]] bool isAuthorized(const model::InitiatingFilingRule& rule,
                                const model::ActorRoleId& role) {
    return std::ranges::find(rule.authorized_roles, role) != rule.authorized_roles.end();
}

[[nodiscard]] bool wasDecided(const model::SessionState& state,
                              const model::SubmissionId& submission_id) {
    return std::ranges::find(state.decided_submissions, submission_id) !=
           state.decided_submissions.end();
}

[[nodiscard]] auto expectedAuthority(const model::InitiatingFilingRule& rule,
                                     model::FilingRejectionReason reason)
    -> const model::AuthorityBasis& {
    switch (reason) {
    case model::FilingRejectionReason::UnauthorizedActor:
        return rule.actor_authority;
    case model::FilingRejectionReason::CureDeadlineExpired:
    case model::FilingRejectionReason::DeficiencyNotCured:
        return rule.deficiency_authority;
    case model::FilingRejectionReason::WrongFilingType:
    case model::FilingRejectionReason::ProceedingAlreadyDocketed:
        return rule.filing_authority;
    }
    std::unreachable();
}

[[nodiscard]] auto missingFields(const model::InitiatingFilingRule& rule,
                                 const model::SubmitFiling& command)
    -> std::vector<model::FilingFieldId> {
    std::vector<model::FilingFieldId> missing;
    for (const auto& required : rule.required_fields) {
        const auto supplied =
            std::ranges::find(command.fields, required, &model::SubmittedField::id);
        if (supplied == command.fields.end() || supplied->value.empty()) {
            missing.push_back(required);
        }
    }
    std::ranges::sort(missing, {}, &model::FilingFieldId::value);
    return missing;
}

[[nodiscard]] auto authorityOf(const model::LegalEvent& event) -> const model::AuthorityBasis& {
    return std::visit(
        [](const auto& concrete) -> const model::AuthorityBasis& { return concrete.authority; },
        event);
}

[[nodiscard]] auto sessionOf(const model::LegalEvent& event) -> const model::SessionId& {
    return std::visit(
        [](const auto& concrete) -> const model::SessionId& { return concrete.session_id; }, event);
}

[[nodiscard]] auto submissionOf(const model::LegalEvent& event) -> const model::SubmissionId& {
    return std::visit(
        [](const auto& concrete) -> const model::SubmissionId& { return concrete.submission_id; },
        event);
}

} // namespace

std::expected<std::vector<model::LegalEvent>, Error>
decide(const model::ProcedureDefinition& procedure, const model::CaseDefinition& case_definition,
       const model::SessionState& state, const model::SubmitFiling& command) {
    if (const auto valid = validate(procedure, case_definition, state); !valid) {
        return std::unexpected(valid.error());
    }
    if (command.session_id != state.id || command.submission_id.value.empty() ||
        !command.submitted_at.court_date.value.ok() || !validDigest(command.document_sha256) ||
        std::ranges::any_of(
            command.fields,
            [](const model::SubmittedField& field) { return field.id.value.empty(); }) ||
        hasDuplicates(command.fields,
                      [](const model::SubmittedField& field) { return field.id.value; })) {
        return fail(ErrorCode::InvalidCommand, "invalid filing command");
    }
    if (wasDecided(state, command.submission_id)) {
        return fail(ErrorCode::DuplicateSubmission, "submission was already decided");
    }

    const auto* actor = actorFor(case_definition, command.actor_id);
    if (actor == nullptr) {
        return fail(ErrorCode::UnknownActor, "actor is not part of the case");
    }

    const auto& rule = procedure.initiating_filing;
    const auto reject = [&](model::FilingRejectionReason reason,
                            const model::AuthorityBasis& authority) {
        return std::vector<model::LegalEvent>{model::FilingRejected{
            command.session_id,
            command.submission_id,
            command.actor_id,
            command.filing_type,
            command.submitted_at,
            reason,
            authority,
        }};
    };

    if (!isAuthorized(rule, actor->role)) {
        return reject(model::FilingRejectionReason::UnauthorizedActor, rule.actor_authority);
    }
    if (command.filing_type != rule.filing_type) {
        return reject(model::FilingRejectionReason::WrongFilingType, rule.filing_authority);
    }
    if (state.phase == model::SessionPhase::Docketed) {
        return reject(model::FilingRejectionReason::ProceedingAlreadyDocketed,
                      rule.filing_authority);
    }
    if (state.pending_deficiency.has_value() &&
        std::chrono::sys_days{command.submitted_at.court_date.value} >
            std::chrono::sys_days{state.pending_deficiency->cure_deadline.value}) {
        return reject(model::FilingRejectionReason::CureDeadlineExpired, rule.deficiency_authority);
    }

    auto missing = missingFields(rule, command);
    if (!missing.empty()) {
        if (state.pending_deficiency.has_value()) {
            return reject(model::FilingRejectionReason::DeficiencyNotCured,
                          rule.deficiency_authority);
        }
        return std::vector<model::LegalEvent>{model::FilingDeficiencyIssued{
            command.session_id,
            command.submission_id,
            command.actor_id,
            command.filing_type,
            command.submitted_at,
            command.document_sha256,
            std::move(missing),
            calculateDeadline(procedure.calendar, command.submitted_at.court_date,
                              rule.cure_deadline),
            state.next_docket_sequence,
            rule.deficiency_authority,
        }};
    }

    return std::vector<model::LegalEvent>{model::FilingAccepted{
        command.session_id,
        command.submission_id,
        command.actor_id,
        command.filing_type,
        command.submitted_at,
        command.document_sha256,
        state.next_docket_sequence,
        rule.filing_authority,
    }};
}

std::expected<model::SessionState, Error> apply(const model::ProcedureDefinition& procedure,
                                                const model::CaseDefinition& case_definition,
                                                const model::SessionState& state,
                                                const model::LegalEvent& event) {
    if (const auto valid = validate(procedure, case_definition, state); !valid) {
        return std::unexpected(valid.error());
    }
    if (sessionOf(event) != state.id || !validAuthority(authorityOf(event)) ||
        wasDecided(state, submissionOf(event))) {
        return fail(ErrorCode::InvalidEvent, "event does not belong to the session");
    }

    auto next = state;
    const auto result = std::visit(
        [&](const auto& concrete) -> std::expected<void, Error> {
            using Event = std::remove_cvref_t<decltype(concrete)>;
            if constexpr (std::same_as<Event, model::FilingAccepted>) {
                if (state.phase == model::SessionPhase::Docketed ||
                    concrete.docket_sequence != state.next_docket_sequence ||
                    concrete.filing_type != procedure.initiating_filing.filing_type ||
                    concrete.authority != procedure.initiating_filing.filing_authority ||
                    !concrete.submitted_at.court_date.value.ok() ||
                    !validDigest(concrete.document_sha256)) {
                    return fail(ErrorCode::InvalidTransition, "filing cannot be accepted here");
                }
                const auto* actor = actorFor(case_definition, concrete.actor_id);
                if (actor == nullptr || !isAuthorized(procedure.initiating_filing, actor->role)) {
                    return fail(ErrorCode::InvalidTransition,
                                "accepted filing has an unauthorized actor");
                }
                if (state.pending_deficiency.has_value() &&
                    std::chrono::sys_days{concrete.submitted_at.court_date.value} >
                        std::chrono::sys_days{state.pending_deficiency->cure_deadline.value}) {
                    return fail(ErrorCode::InvalidTransition, "accepted cure is late");
                }
                next.phase = model::SessionPhase::Docketed;
                next.pending_deficiency.reset();
                next.accepted_filing = model::AcceptedFiling{
                    concrete.submission_id,   concrete.actor_id,        concrete.filing_type,
                    concrete.document_sha256, concrete.docket_sequence,
                };
                ++next.next_docket_sequence;
            } else if constexpr (std::same_as<Event, model::FilingDeficiencyIssued>) {
                if (state.phase != model::SessionPhase::AwaitingInitiatingFiling ||
                    concrete.docket_sequence != state.next_docket_sequence ||
                    concrete.missing_fields.empty() ||
                    concrete.filing_type != procedure.initiating_filing.filing_type ||
                    concrete.authority != procedure.initiating_filing.deficiency_authority ||
                    !concrete.submitted_at.court_date.value.ok() ||
                    !validDigest(concrete.document_sha256) ||
                    concrete.cure_deadline !=
                        calculateDeadline(procedure.calendar, concrete.submitted_at.court_date,
                                          procedure.initiating_filing.cure_deadline)) {
                    return fail(ErrorCode::InvalidTransition, "deficiency cannot be issued here");
                }
                const auto* actor = actorFor(case_definition, concrete.actor_id);
                if (actor == nullptr || !isAuthorized(procedure.initiating_filing, actor->role) ||
                    std::ranges::any_of(concrete.missing_fields, [&](const auto& missing) {
                        return std::ranges::find(procedure.initiating_filing.required_fields,
                                                 missing) ==
                               procedure.initiating_filing.required_fields.end();
                    })) {
                    return fail(ErrorCode::InvalidTransition, "deficiency fields are inconsistent");
                }
                next.phase = model::SessionPhase::DeficiencyPending;
                next.pending_deficiency = model::PendingDeficiency{
                    concrete.submission_id,
                    concrete.cure_deadline,
                    concrete.missing_fields,
                };
                ++next.next_docket_sequence;
            } else {
                const auto* actor = actorFor(case_definition, concrete.actor_id);
                if (actor == nullptr ||
                    concrete.authority !=
                        expectedAuthority(procedure.initiating_filing, concrete.reason)) {
                    return fail(ErrorCode::InvalidTransition,
                                "rejection authority is inconsistent");
                }
                const auto authorized = isAuthorized(procedure.initiating_filing, actor->role);
                const auto reason_is_consistent = [&] {
                    switch (concrete.reason) {
                    case model::FilingRejectionReason::UnauthorizedActor:
                        return !authorized;
                    case model::FilingRejectionReason::WrongFilingType:
                        return authorized &&
                               concrete.filing_type != procedure.initiating_filing.filing_type;
                    case model::FilingRejectionReason::CureDeadlineExpired:
                        return authorized && state.pending_deficiency.has_value() &&
                               std::chrono::sys_days{concrete.submitted_at.court_date.value} >
                                   std::chrono::sys_days{
                                       state.pending_deficiency->cure_deadline.value};
                    case model::FilingRejectionReason::DeficiencyNotCured:
                        return authorized && state.pending_deficiency.has_value();
                    case model::FilingRejectionReason::ProceedingAlreadyDocketed:
                        return authorized && state.phase == model::SessionPhase::Docketed;
                    }
                    std::unreachable();
                }();
                if (!concrete.submitted_at.court_date.value.ok() || !reason_is_consistent) {
                    return fail(ErrorCode::InvalidTransition, "rejection reason is inconsistent");
                }
            }
            return {};
        },
        event);
    if (!result) {
        return std::unexpected(result.error());
    }

    next.decided_submissions.push_back(submissionOf(event));
    return next;
}

std::expected<model::SessionState, Error> replay(const model::ProcedureDefinition& procedure,
                                                 const model::CaseDefinition& case_definition,
                                                 model::SessionState initial_state,
                                                 std::span<const model::LegalEvent> events) {
    for (const auto& event : events) {
        auto next = apply(procedure, case_definition, initial_state, event);
        if (!next) {
            return std::unexpected(next.error());
        }
        initial_state = std::move(*next);
    }
    return initial_state;
}

} // namespace appellate::engine
