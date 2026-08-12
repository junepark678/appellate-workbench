#include "appellate/engine/workflow_engine.hpp"

#include <QTest>

#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace model = appellate::model;
namespace engine = appellate::engine;

constexpr auto appellant_stage = "test.stage.appellant-brief";
constexpr auto appellee_stage = "test.stage.appellee-brief";
constexpr auto reply_stage = "test.stage.reply-brief";
constexpr auto submitted_stage = "test.stage.submitted";
constexpr auto argument_stage = "test.stage.argument";
constexpr auto judgment_stage = "test.stage.judgment";
constexpr auto closed_stage = "test.stage.closed";
constexpr auto appellant_role = "test.role.appellant";
constexpr auto appellee_role = "test.role.appellee";
constexpr auto court_role = "test.role.court";
constexpr auto response_deadline = "test.deadline.response";
constexpr auto reply_deadline = "test.deadline.reply";
constexpr auto structured_case_id = "example.case.fictional";
constexpr auto structured_judgment_operation = "example.operation.issue-judgment";
constexpr auto structured_plan_id = "example.disposition.fictional";
constexpr auto structured_plan_digest =
    "d9c97181a59eb4a0fd79aa3fcad32bd9cd5e4128aad8a49a68384900a1eb5121";
constexpr auto counterfactual_judgment_operation =
    "example.operation.issue-counterfactual-judgment";
constexpr auto counterfactual_plan_id = "example.disposition.counterfactual";
constexpr auto counterfactual_plan_digest =
    "fad5c3a7c86b3c640031a1d229b062a86482dc914902b58c12a0c793c31c87b5";

static_assert(static_cast<int>(model::WorkflowDeadlineCondition::Open) == 0);
static_assert(static_cast<int>(model::WorkflowDeadlineCondition::Satisfied) == 1);
static_assert(static_cast<int>(model::WorkflowDeadlineCondition::Elapsed) == 2);
static_assert(static_cast<int>(model::WorkflowDeadlineCondition::NotElapsed) == 3);
static_assert(static_cast<int>(model::WorkflowDeadlineCondition::Reached) == 4);

[[nodiscard]] model::LegalDate date(int year, unsigned month, unsigned day) {
    return model::LegalDate{std::chrono::year{year} / std::chrono::month{month} /
                            std::chrono::day{day}};
}

[[nodiscard]] model::LegalTime at(model::LegalDate court_date) {
    return model::LegalTime{std::chrono::sys_seconds{std::chrono::sys_days{court_date.value}},
                            court_date};
}

[[nodiscard]] model::AuthorityBasis authority(const std::string& operation_id) {
    return model::AuthorityBasis{
        model::AuthorityRef{model::AuthorityId{operation_id + ".authority"}, "Test Appellate Rule",
                            "2026-08-11", "Source-grounded test proposition for " + operation_id},
        {},
    };
}

[[nodiscard]] model::AuthorityProvenance provenance() {
    return model::AuthorityProvenance{model::AuthorityType::Rule,
                                      "us.federal",
                                      "us.ca4",
                                      model::PrecedentialStatus::NotApplicable,
                                      true,
                                      "2026-08-11",
                                      "Fed. R. App. P. 3(a)",
                                      "https://www.ca4.uscourts.gov/rules/Rule03.html"};
}

[[nodiscard]] model::WorkflowOperation
operation(std::string id, std::string stage, model::WorkflowOpcode opcode,
          std::optional<std::string> next_stage = std::nullopt,
          std::optional<std::uint32_t> days = std::nullopt,
          std::optional<model::DeadlineCounting> counting = std::nullopt,
          std::vector<std::string> roles = {}) {
    std::vector<model::ActorRoleId> authorized;
    for (auto& role : roles) {
        authorized.push_back(model::ActorRoleId{std::move(role)});
    }
    auto next =
        next_stage ? std::optional{model::WorkflowStageId{std::move(*next_stage)}} : std::nullopt;
    const auto operation_id = id;
    return model::WorkflowOperation{model::WorkflowOperationId{std::move(id)},
                                    model::WorkflowStageId{std::move(stage)},
                                    opcode,
                                    authority(operation_id),
                                    std::move(next),
                                    days,
                                    counting,
                                    std::move(authorized),
                                    {},
                                    std::nullopt};
}

[[nodiscard]] model::WorkflowDefinition workflow() {
    std::vector<model::WorkflowStageId> stages;
    for (const auto* stage : {appellant_stage, appellee_stage, reply_stage, submitted_stage,
                              argument_stage, judgment_stage, closed_stage}) {
        stages.push_back(model::WorkflowStageId{stage});
    }

    std::vector<model::WorkflowOperation> operations;
    operations.push_back(
        operation("test.op.opening.reject", appellant_stage, model::WorkflowOpcode::RejectFiling));
    operations.push_back(
        operation("test.op.extension.reject", appellee_stage, model::WorkflowOpcode::RejectFiling));
    operations.push_back(
        operation("test.op.response.reject", appellee_stage, model::WorkflowOpcode::RejectFiling));
    operations.push_back(
        operation("test.op.reply.reject", reply_stage, model::WorkflowOpcode::RejectFiling));

    operations.push_back(
        operation("test.op.opening.accept", appellant_stage, model::WorkflowOpcode::AcceptFiling));
    operations.push_back(operation("test.op.opening.deficiency", appellant_stage,
                                   model::WorkflowOpcode::IssueDeficiency));
    operations.push_back(operation("test.op.opening.cure-deadline", appellant_stage,
                                   model::WorkflowOpcode::CalculateDeadline, std::nullopt, 2,
                                   model::DeadlineCounting::CalendarDays));
    operations.push_back(operation("test.op.response.deadline", appellant_stage,
                                   model::WorkflowOpcode::CalculateDeadline, std::nullopt, 30,
                                   model::DeadlineCounting::CalendarDays));
    operations.push_back(operation("test.op.court.deadline", appellant_stage,
                                   model::WorkflowOpcode::CalculateDeadline, std::nullopt, 3,
                                   model::DeadlineCounting::CalendarDays, {court_role}));
    operations.push_back(operation("test.op.court-advance", appellant_stage,
                                   model::WorkflowOpcode::AdvanceStage, appellee_stage,
                                   std::nullopt, std::nullopt, {court_role}));
    operations.push_back(operation("test.op.to-appellee", appellant_stage,
                                   model::WorkflowOpcode::AdvanceStage, appellee_stage));

    operations.push_back(
        operation("test.op.extension.accept", appellee_stage, model::WorkflowOpcode::AcceptFiling));
    operations.push_back(
        operation("test.op.response.accept", appellee_stage, model::WorkflowOpcode::AcceptFiling));
    operations.push_back(operation("test.op.appellee.deficiency", appellee_stage,
                                   model::WorkflowOpcode::IssueDeficiency));
    operations.push_back(operation("test.op.appellee.cure-deadline", appellee_stage,
                                   model::WorkflowOpcode::CalculateDeadline, std::nullopt, 3,
                                   model::DeadlineCounting::CalendarDays));
    operations.push_back(operation("test.op.reply.deadline", appellee_stage,
                                   model::WorkflowOpcode::CalculateDeadline, std::nullopt, 14,
                                   model::DeadlineCounting::CalendarDays));
    operations.push_back(operation("test.op.to-reply", appellee_stage,
                                   model::WorkflowOpcode::AdvanceStage, reply_stage));
    operations.push_back(operation("test.op.extension.order", appellee_stage,
                                   model::WorkflowOpcode::EnterOrder, std::nullopt, 7,
                                   model::DeadlineCounting::CalendarDays, {court_role}));

    operations.push_back(
        operation("test.op.reply.accept", reply_stage, model::WorkflowOpcode::AcceptFiling));
    operations.push_back(
        operation("test.op.reply.deficiency", reply_stage, model::WorkflowOpcode::IssueDeficiency));
    operations.push_back(operation("test.op.reply.cure-deadline", reply_stage,
                                   model::WorkflowOpcode::CalculateDeadline, std::nullopt, 3,
                                   model::DeadlineCounting::BusinessDays));
    operations.push_back(operation("test.op.to-submission", reply_stage,
                                   model::WorkflowOpcode::AdvanceStage, submitted_stage));

    operations.push_back(operation("test.op.set-sealed", submitted_stage,
                                   model::WorkflowOpcode::SetSealed, std::nullopt, std::nullopt,
                                   std::nullopt, {court_role}));
    operations.push_back(operation("test.op.schedule-argument", submitted_stage,
                                   model::WorkflowOpcode::ScheduleArgument, argument_stage,
                                   std::nullopt, std::nullopt, {court_role}));
    operations.push_back(operation("test.op.issue-judgment", argument_stage,
                                   model::WorkflowOpcode::IssueJudgment, judgment_stage,
                                   std::nullopt, std::nullopt, {court_role}));
    operations.push_back(operation("test.op.issue-mandate", judgment_stage,
                                   model::WorkflowOpcode::IssueMandate, closed_stage, std::nullopt,
                                   std::nullopt, {court_role}));

    const auto make_route = [](std::string filing_type, std::string stage,
                               std::vector<std::string> authorized_roles,
                               std::vector<std::string> service_roles, std::string accept,
                               std::string reject, std::optional<std::string> deficiency,
                               std::optional<model::WorkflowDeadlinePlan> deficiency_deadline,
                               std::optional<model::WorkflowDeadlinePlan> accepted_deadline,
                               std::optional<std::string> advance,
                               std::optional<std::string> satisfies) {
        std::vector<model::ActorRoleId> authorized;
        for (auto& role : authorized_roles) {
            authorized.push_back(model::ActorRoleId{std::move(role)});
        }
        std::vector<model::ActorRoleId> service;
        for (auto& role : service_roles) {
            service.push_back(model::ActorRoleId{std::move(role)});
        }
        return model::WorkflowFilingRoute{
            model::FilingTypeId{std::move(filing_type)},
            model::WorkflowStageId{std::move(stage)},
            std::move(authorized),
            {model::FilingFieldId{"test.field.certificate"}},
            std::move(service),
            model::WorkflowOperationId{std::move(accept)},
            model::WorkflowOperationId{std::move(reject)},
            deficiency ? std::optional{model::WorkflowOperationId{std::move(*deficiency)}}
                       : std::nullopt,
            std::move(deficiency_deadline),
            std::move(accepted_deadline),
            advance ? std::optional{model::WorkflowOperationId{std::move(*advance)}} : std::nullopt,
            satisfies ? std::optional{model::WorkflowDeadlineId{std::move(*satisfies)}}
                      : std::nullopt,
            true,
        };
    };

    std::vector<model::WorkflowFilingRoute> routes;
    routes.push_back(make_route(
        "test.filing.opening", appellant_stage, {appellant_role}, {appellee_role},
        "test.op.opening.accept", "test.op.opening.reject", "test.op.opening.deficiency",
        model::WorkflowDeadlinePlan{model::WorkflowDeadlineId{"test.deadline.opening-cure"},
                                    model::WorkflowOperationId{"test.op.opening.cure-deadline"}},
        model::WorkflowDeadlinePlan{model::WorkflowDeadlineId{response_deadline},
                                    model::WorkflowOperationId{"test.op.response.deadline"}},
        "test.op.to-appellee", std::nullopt));
    routes.push_back(make_route("test.filing.extension", appellee_stage, {appellee_role},
                                {appellant_role}, "test.op.extension.accept",
                                "test.op.extension.reject", std::nullopt, std::nullopt,
                                std::nullopt, std::nullopt, std::nullopt));
    routes.push_back(make_route(
        "test.filing.response", appellee_stage, {appellee_role}, {appellant_role},
        "test.op.response.accept", "test.op.response.reject", "test.op.appellee.deficiency",
        std::nullopt,
        model::WorkflowDeadlinePlan{model::WorkflowDeadlineId{reply_deadline},
                                    model::WorkflowOperationId{"test.op.reply.deadline"}},
        "test.op.to-reply", response_deadline));
    routes.push_back(make_route(
        "test.filing.reply", reply_stage, {appellant_role}, {appellee_role}, "test.op.reply.accept",
        "test.op.reply.reject", "test.op.reply.deficiency",
        model::WorkflowDeadlinePlan{model::WorkflowDeadlineId{"test.deadline.reply-cure"},
                                    model::WorkflowOperationId{"test.op.reply.cure-deadline"}},
        std::nullopt, "test.op.to-submission", reply_deadline));

    return model::WorkflowDefinition{
        model::WorkflowId{"test.workflow.civil-appeal"},
        model::WorkflowStageId{appellant_stage},
        std::move(stages),
        std::move(operations),
        std::move(routes),
        model::CourtCalendar{{date(2026, 8, 17)}},
    };
}

[[nodiscard]] model::CaseDefinition caseDefinition() {
    return model::CaseDefinition{
        model::CaseId{"test.case.rule54b"},
        model::ProcedureId{"test.procedure.civil"},
        {
            {model::ActorId{"test.actor.appellant"}, model::ActorRoleId{appellant_role}},
            {model::ActorId{"test.actor.appellee"}, model::ActorRoleId{appellee_role}},
            {model::ActorId{"test.actor.court"}, model::ActorRoleId{court_role}},
        },
    };
}

[[nodiscard]] model::WorkflowDefinition canonicalWorkflow() {
    auto definition = workflow();
    for (auto& candidate : definition.operations) {
        candidate.authority.primary.provenance = provenance();
    }
    return definition;
}

[[nodiscard]] model::WorkflowOperation& operationById(model::WorkflowDefinition& definition,
                                                      std::string_view id) {
    const auto found = std::ranges::find_if(
        definition.operations, [&](const auto& candidate) { return candidate.id.value == id; });
    Q_ASSERT(found != definition.operations.end());
    return *found;
}

[[nodiscard]] model::WorkflowFilingRoute& routeByType(model::WorkflowDefinition& definition,
                                                      std::string_view filing_type) {
    const auto found = std::ranges::find_if(definition.filing_routes, [&](const auto& route) {
        return route.filing_type.value == filing_type;
    });
    Q_ASSERT(found != definition.filing_routes.end());
    return *found;
}

[[nodiscard]] model::WorkflowDefinition structuredWorkflow() {
    auto definition = canonicalWorkflow();
    auto judgment = std::ranges::find(definition.operations,
                                      model::WorkflowOperationId{"test.op.issue-judgment"},
                                      &model::WorkflowOperation::id);
    if (judgment == definition.operations.end()) {
        return {};
    }
    judgment->id = model::WorkflowOperationId{structured_judgment_operation};
    judgment->preconditions = {
        model::WorkflowFilingPrecondition{model::FilingTypeId{"test.filing.opening"}, true},
        model::WorkflowOrderPrecondition{model::WorkflowOrderId{"test.order.extension"},
                                         model::WorkflowOrderDisposition::Granted},
        model::WorkflowDeadlinePrecondition{model::WorkflowDeadlineId{response_deadline},
                                            model::WorkflowDeadlineCondition::Satisfied},
        model::WorkflowDeadlinePrecondition{model::WorkflowDeadlineId{response_deadline},
                                            model::WorkflowDeadlineCondition::Elapsed},
        model::WorkflowArgumentPrecondition{true},
        model::WorkflowJudgmentPrecondition{false},
    };
    return definition;
}

[[nodiscard]] model::CaseDefinition structuredCaseDefinition() {
    auto case_definition = caseDefinition();
    case_definition.id = model::CaseId{structured_case_id};
    case_definition.disposition_targets = {
        {model::CaseIssueId{"example.issue.preservation"},
         model::DispositionTargetId{"example.target.preservation"}},
    };
    case_definition.disposition_plans = {
        {model::DispositionPlanId{structured_plan_id},
         model::DispositionFinality::Final,
         structured_plan_digest,
         {{model::CaseIssueId{"example.issue.preservation"},
           model::DispositionTargetId{"example.target.preservation"},
           model::DispositionScope::Whole,
           model::DispositionAction::Dismiss,
           true,
           {model::AuthorityId{"example.authority.rule-one"}},
           {model::RecordAnchorId{"example.record.entry-one"},
            model::RecordAnchorId{"example.record.anchor.ja2"}}}}},
    };
    case_definition.authored_disposition_plan_id = model::DispositionPlanId{structured_plan_id};
    case_definition.authored_disposition_operation_id =
        model::WorkflowOperationId{structured_judgment_operation};
    return case_definition;
}

[[nodiscard]] model::CaseDefinition maximumStructuredCaseDefinition() {
    auto case_definition = caseDefinition();
    case_definition.id = model::CaseId{"test.case.maximum-plan"};
    model::DispositionPlan plan{model::DispositionPlanId{"test.plan.maximum"},
                                model::DispositionFinality::Nonfinal,
                                "4ea2741cdfec1373a23f5f67c69bca286472c1d1b0aead7c485f0c4ad25f4051",
                                {}};
    for (std::size_t component_index = 0; component_index < 32; ++component_index) {
        const auto suffix = std::to_string(component_index);
        const auto issue_id = "test.issue.maximum-" + suffix;
        const auto target_id = "test.target.maximum-" + suffix;
        case_definition.disposition_targets.push_back(
            {model::CaseIssueId{issue_id}, model::DispositionTargetId{target_id}});
        std::vector<model::AuthorityId> authority_ids;
        std::vector<model::RecordAnchorId> anchor_ids;
        for (std::size_t reference_index = 0; reference_index < 32; ++reference_index) {
            const auto reference_suffix = std::to_string(reference_index);
            authority_ids.push_back(
                model::AuthorityId{"test.authority.maximum-" + suffix + "-" + reference_suffix});
            anchor_ids.push_back(
                model::RecordAnchorId{"test.anchor.maximum-" + suffix + "-" + reference_suffix});
        }
        plan.components.push_back({model::CaseIssueId{issue_id},
                                   model::DispositionTargetId{target_id},
                                   model::DispositionScope::Whole, model::DispositionAction::Affirm,
                                   false, std::move(authority_ids), std::move(anchor_ids)});
    }
    case_definition.disposition_plans.push_back(std::move(plan));
    case_definition.authored_disposition_plan_id = model::DispositionPlanId{"test.plan.maximum"};
    case_definition.authored_disposition_operation_id =
        model::WorkflowOperationId{"test.op.issue-judgment"};
    return case_definition;
}

[[nodiscard]] model::WorkflowState initialState() {
    return model::WorkflowState{
        "test.session.one",
        model::WorkflowId{"test.workflow.civil-appeal"},
        model::WorkflowStageId{appellant_stage},
        1,
        std::nullopt,
        {},
        {},
        {},
        {},
        {},
        false,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
    };
}

[[nodiscard]] model::WorkflowCommandHeader header(std::string command, std::string actor,
                                                  model::LegalDate occurred) {
    return model::WorkflowCommandHeader{"test.session.one",
                                        model::WorkflowCommandId{std::move(command)},
                                        model::ActorId{std::move(actor)}, at(occurred)};
}

[[nodiscard]] model::SubmitWorkflowFiling
filing(std::string command, std::string filing_id, std::string actor, std::string type,
       model::LegalDate occurred, std::vector<model::ActorId> served, bool has_certificate = true,
       std::optional<model::WorkflowDeficiencyId> cures = std::nullopt) {
    std::vector<model::WorkflowFieldValue> fields;
    if (has_certificate) {
        fields.push_back({model::FilingFieldId{"test.field.certificate"}, "certificate supplied"});
    }
    return model::SubmitWorkflowFiling{
        header(std::move(command), std::move(actor), occurred),
        model::WorkflowFilingId{std::move(filing_id)},
        model::FilingTypeId{std::move(type)},
        std::string(64, 'a'),
        std::move(fields),
        std::move(served),
        std::move(cures),
    };
}

struct Run final {
    model::WorkflowState initial_state;
    model::WorkflowState state;
    std::vector<model::WorkflowJournalEntry> journal;
    std::vector<model::WorkflowEvent> trace;
};

[[nodiscard]] Run emptyRun() {
    const auto initial = initialState();
    return Run{initial, initial, {}, {}};
}

[[nodiscard]] auto execute(const model::WorkflowDefinition& definition,
                           const model::CaseDefinition& case_definition, Run& run,
                           model::WorkflowCommand command) -> std::expected<void, std::string> {
    const auto events = engine::decideWorkflow(definition, case_definition, run.state, command);
    if (!events) {
        return std::unexpected(events.error().message);
    }
    auto journal = run.journal;
    journal.push_back(model::WorkflowJournalEntry{std::move(command), *events});
    const auto next =
        engine::replayWorkflow(definition, case_definition, run.initial_state, journal);
    if (!next) {
        return std::unexpected(next.error().message);
    }
    run.trace.insert(run.trace.end(), events->begin(), events->end());
    run.journal = std::move(journal);
    run.state = *next;
    return {};
}

[[nodiscard]] auto happyRun() -> std::expected<Run, std::string> {
    const auto definition = workflow();
    const auto case_definition = caseDefinition();
    auto run = emptyRun();
    const auto appellant = model::ActorId{"test.actor.appellant"};
    const auto appellee = model::ActorId{"test.actor.appellee"};

    if (auto result = execute(definition, case_definition, run,
                              filing("command.opening", "filing.opening", "test.actor.appellant",
                                     "test.filing.opening", date(2026, 8, 13), {appellee}));
        !result) {
        return std::unexpected(result.error());
    }
    if (auto result = execute(definition, case_definition, run,
                              filing("command.extension", "filing.extension", "test.actor.appellee",
                                     "test.filing.extension", date(2026, 8, 20), {appellant}));
        !result) {
        return std::unexpected(result.error());
    }
    if (auto result =
            execute(definition, case_definition, run,
                    model::EnterWorkflowOrder{
                        header("command.extension-order", "test.actor.court", date(2026, 8, 21)),
                        model::WorkflowOperationId{"test.op.extension.order"},
                        model::WorkflowOrderId{"test.order.extension"},
                        model::WorkflowOrderDisposition::Granted, std::string(64, 'b'),
                        model::WorkflowDeadlineId{response_deadline}});
        !result) {
        return std::unexpected(result.error());
    }
    if (auto result = execute(definition, case_definition, run,
                              filing("command.response", "filing.response", "test.actor.appellee",
                                     "test.filing.response", date(2026, 9, 21), {appellant}));
        !result) {
        return std::unexpected(result.error());
    }
    if (auto result = execute(definition, case_definition, run,
                              filing("command.reply", "filing.reply", "test.actor.appellant",
                                     "test.filing.reply", date(2026, 10, 5), {appellee}));
        !result) {
        return std::unexpected(result.error());
    }
    if (auto result = execute(
            definition, case_definition, run,
            model::SetWorkflowSealed{header("command.seal", "test.actor.court", date(2026, 10, 6)),
                                     model::WorkflowOperationId{"test.op.set-sealed"}, true});
        !result) {
        return std::unexpected(result.error());
    }
    if (auto result = execute(definition, case_definition, run,
                              model::ScheduleWorkflowArgument{
                                  header("command.argument", "test.actor.court", date(2026, 10, 6)),
                                  model::WorkflowOperationId{"test.op.schedule-argument"},
                                  date(2026, 11, 10)});
        !result) {
        return std::unexpected(result.error());
    }
    if (auto result =
            execute(definition, case_definition, run,
                    model::IssueWorkflowJudgment{
                        header("command.judgment", "test.actor.court", date(2026, 11, 10)),
                        model::WorkflowOperationId{"test.op.issue-judgment"}, std::string(64, 'c'),
                        "affirmed"});
        !result) {
        return std::unexpected(result.error());
    }
    if (auto result =
            execute(definition, case_definition, run,
                    model::IssueWorkflowMandate{
                        header("command.mandate", "test.actor.court", date(2026, 12, 2)),
                        model::WorkflowOperationId{"test.op.issue-mandate"}, std::string(64, 'd')});
        !result) {
        return std::unexpected(result.error());
    }
    return run;
}

[[nodiscard]] auto structuredReadyRun() -> std::expected<Run, std::string> {
    const auto source = happyRun();
    if (!source) {
        return std::unexpected(source.error());
    }
    const auto definition = structuredWorkflow();
    const auto case_definition = structuredCaseDefinition();
    auto run = emptyRun();
    const auto prefix_size = source->journal.size() - 2U;
    for (std::size_t index = 0; index < prefix_size; ++index) {
        if (auto result = execute(definition, case_definition, run, source->journal[index].command);
            !result) {
            return std::unexpected(result.error());
        }
    }
    return run;
}

[[nodiscard]] auto submittedRun() -> std::expected<Run, std::string> {
    const auto definition = workflow();
    const auto case_definition = caseDefinition();
    auto run = emptyRun();
    const auto appellant = model::ActorId{"test.actor.appellant"};
    const auto appellee = model::ActorId{"test.actor.appellee"};
    if (auto result = execute(definition, case_definition, run,
                              filing("command.opening", "filing.opening", "test.actor.appellant",
                                     "test.filing.opening", date(2026, 8, 13), {appellee}));
        !result) {
        return std::unexpected(result.error());
    }
    if (auto result = execute(definition, case_definition, run,
                              filing("command.response", "filing.response", "test.actor.appellee",
                                     "test.filing.response", date(2026, 9, 10), {appellant}));
        !result) {
        return std::unexpected(result.error());
    }
    if (auto result = execute(definition, case_definition, run,
                              filing("command.reply", "filing.reply", "test.actor.appellant",
                                     "test.filing.reply", date(2026, 9, 20), {appellee}));
        !result) {
        return std::unexpected(result.error());
    }
    return run;
}

[[nodiscard]] std::string eventName(const model::WorkflowEvent& event) {
    return std::visit(
        [](const auto& concrete) {
            using Event = std::remove_cvref_t<decltype(concrete)>;
            if constexpr (std::same_as<Event, model::WorkflowFilingAccepted>) {
                return std::string("accepted");
            } else if constexpr (std::same_as<Event, model::WorkflowFilingRejected>) {
                return std::string("rejected");
            } else if constexpr (std::same_as<Event, model::WorkflowDeficiencyIssued>) {
                return std::string("deficiency");
            } else if constexpr (std::same_as<Event, model::WorkflowDeadlineCalculated>) {
                return std::string("deadline");
            } else if constexpr (std::same_as<Event, model::WorkflowOrderEntered>) {
                return std::string("order");
            } else if constexpr (std::same_as<Event, model::WorkflowStageAdvanced>) {
                return std::string("advance");
            } else if constexpr (std::same_as<Event, model::WorkflowSealedSet>) {
                return std::string("sealed");
            } else if constexpr (std::same_as<Event, model::WorkflowArgumentScheduled>) {
                return std::string("argument");
            } else if constexpr (std::same_as<Event, model::WorkflowJudgmentIssued>) {
                return std::string("judgment");
            } else {
                return std::string("mandate");
            }
        },
        event);
}

[[nodiscard]] const model::WorkflowEventHeader& eventHeader(const model::WorkflowEvent& event) {
    return std::visit(
        [](const auto& concrete) -> const model::WorkflowEventHeader& { return concrete.header; },
        event);
}

class WorkflowEngineTest final : public QObject {
    Q_OBJECT

  private slots:
    void completesDeterministicGoldenAppeal();
    void deficiencyUsesServiceAndHolidayRulesAndCanBeCured();
    void deficiencyWithoutSourcedDeadlineReplaysAndCanBeCured();
    void repeatedDeficienciesUseInstanceIdsAndReplay();
    void rejectsUnauthorizedIneligibleAndLateFilings();
    void usesRouteLocalRejectionAuthoritiesAndExplicitNonconformance();
    void courtRecordsSourcedDeadlineTriggers();
    void usesExistingDeadlineAsExactBaseAndReachedBoundary();
    void usesExactEventDatesAndArgumentDateBoundary();
    void courtExplicitlyAdvancesSourcedStages();
    void rejectsUnauthorizedCourtActionAndMissingAuthority();
    void validatesCompleteProvenanceAndRejectsMutations();
    void replayRejectsImpossibleGroupsAndForgedCourtActions();
    void rejectsBackdatedExplicitLegalTime();
    void rejectsDirtySnapshotsAndMalformedDefinitions();
    void rejectsMalformedRouteOutcomeCombinations();
    void replayRejectsTamperedTrace();
    void usesStructuredDispositionPlanAndCanonicalDigest();
    void enforcesOperationDispositionBindings();
    void enforcesBoundedAllOfPreconditionsAndReplaySnapshots();
    void enforcesExactFilingAndOrderInstancePreconditions();
    void enforcesStaticDeficiencyDeadlineIdentityAndReplay();
    void enforcesOperationDocumentAndArgumentBindings();
    void rejectsMalformedAndOversizedDispositionInventories();
};

void WorkflowEngineTest::completesDeterministicGoldenAppeal() {
    const auto first = happyRun();
    const auto second = happyRun();
    QVERIFY2(first.has_value(), first ? "" : first.error().c_str());
    QVERIFY2(second.has_value(), second ? "" : second.error().c_str());
    QVERIFY(first->trace == second->trace);
    QVERIFY(first->state == second->state);

    std::vector<std::string> names;
    for (const auto& event : first->trace) {
        names.push_back(eventName(event));
        const auto& metadata = eventHeader(event);
        QVERIFY(!metadata.authority.primary.id.value.empty());
        QVERIFY(!metadata.authority.primary.source_version.empty());
        QVERIFY(!metadata.authority.primary.proposition.empty());
    }
    const std::vector<std::string> golden{"accepted", "deadline", "advance",  "accepted", "order",
                                          "accepted", "deadline", "advance",  "accepted", "advance",
                                          "sealed",   "argument", "judgment", "mandate"};
    QCOMPARE(names, golden);
    QCOMPARE(first->trace.size(), std::size_t{14});
    QCOMPARE(first->state.next_event_sequence, std::uint64_t{15});
    QCOMPARE(first->state.current_stage_id.value, std::string(closed_stage));
    QVERIFY(first->state.sealed);
    QVERIFY(first->state.argument_date == date(2026, 11, 10));
    QVERIFY(first->state.judgment_sha256.has_value());
    QVERIFY(first->state.mandate_sha256.has_value());
    QCOMPARE(first->state.accepted_filings.front().served_actors,
             std::vector{model::ActorId{"test.actor.appellee"}});

    const auto response =
        std::ranges::find(first->state.deadlines, model::WorkflowDeadlineId{response_deadline},
                          &model::WorkflowDeadlineRecord::deadline_id);
    QVERIFY(response != first->state.deadlines.end());
    QVERIFY(response->due_date == date(2026, 9, 21));
    QCOMPARE(response->status, model::WorkflowDeadlineStatus::Satisfied);
}

void WorkflowEngineTest::deficiencyUsesServiceAndHolidayRulesAndCanBeCured() {
    const auto definition = workflow();
    const auto case_definition = caseDefinition();
    auto run = emptyRun();
    const auto deficient_command =
        filing("command.deficient", "filing.opening", "test.actor.appellant", "test.filing.opening",
               date(2026, 8, 14), {}, false);
    const auto decision =
        engine::decideWorkflow(definition, case_definition, run.state, deficient_command);
    QVERIFY(decision.has_value());
    QCOMPARE(decision->size(), std::size_t{2});
    const auto* deficiency = std::get_if<model::WorkflowDeficiencyIssued>(&decision->front());
    const auto* deadline = std::get_if<model::WorkflowDeadlineCalculated>(&decision->back());
    QVERIFY(deficiency != nullptr);
    QVERIFY(deadline != nullptr);
    QVERIFY(deficiency->cure_deadline_id.has_value());
    QCOMPARE(deficiency->missing_requirements,
             (std::vector{model::WorkflowRequirementId{"service.test.actor.appellee"},
                          model::WorkflowRequirementId{"test.field.certificate"}}));
    QVERIFY(deadline->due_date == date(2026, 8, 18));

    run.journal.push_back(model::WorkflowJournalEntry{deficient_command, *decision});
    const auto pending =
        engine::replayWorkflow(definition, case_definition, run.initial_state, run.journal);
    QVERIFY(pending.has_value());
    const auto cure =
        filing("command.cure", "filing.opening.cured", "test.actor.appellant",
               "test.filing.opening", date(2026, 8, 18), {model::ActorId{"test.actor.appellee"}},
               true, deficiency->deficiency_id);
    const auto cured_events = engine::decideWorkflow(definition, case_definition, *pending, cure);
    QVERIFY(cured_events.has_value());
    run.journal.push_back(model::WorkflowJournalEntry{cure, *cured_events});
    const auto cured =
        engine::replayWorkflow(definition, case_definition, run.initial_state, run.journal);
    QVERIFY(cured.has_value());
    QVERIFY(cured->deficiencies.front().cured);
    QCOMPARE(cured->deadlines.front().status, model::WorkflowDeadlineStatus::Satisfied);
    QCOMPARE(cured->current_stage_id.value, std::string(appellee_stage));
}

void WorkflowEngineTest::deficiencyWithoutSourcedDeadlineReplaysAndCanBeCured() {
    const auto definition = workflow();
    const auto case_definition = caseDefinition();
    auto run = emptyRun();
    const auto appellant = model::ActorId{"test.actor.appellant"};
    QVERIFY(execute(definition, case_definition, run,
                    filing("command.opening", "filing.opening", "test.actor.appellant",
                           "test.filing.opening", date(2026, 8, 13),
                           {model::ActorId{"test.actor.appellee"}}))
                .has_value());

    const auto deficient_command =
        filing("command.response-deficient", "filing.response-deficient", "test.actor.appellee",
               "test.filing.response", date(2026, 8, 20), {appellant}, false);
    const auto decision =
        engine::decideWorkflow(definition, case_definition, run.state, deficient_command);
    QVERIFY(decision.has_value());
    QCOMPARE(decision->size(), std::size_t{1});
    const auto* deficiency = std::get_if<model::WorkflowDeficiencyIssued>(&decision->front());
    QVERIFY(deficiency != nullptr);
    QVERIFY(!deficiency->cure_deadline_id.has_value());
    QCOMPARE(deficiency->header.operation_id.value, std::string("test.op.appellee.deficiency"));

    run.journal.push_back(model::WorkflowJournalEntry{deficient_command, *decision});
    const auto pending =
        engine::replayWorkflow(definition, case_definition, run.initial_state, run.journal);
    QVERIFY(pending.has_value());
    QCOMPARE(pending->deficiencies.size(), std::size_t{1});
    QVERIFY(!pending->deficiencies.front().cure_deadline_id.has_value());
    QCOMPARE(pending->deadlines.size(), std::size_t{1});

    const auto cure = filing("command.response-cure", "filing.response-cure", "test.actor.appellee",
                             "test.filing.response", date(2026, 9, 10), {appellant}, true,
                             deficiency->deficiency_id);
    const auto cured_events = engine::decideWorkflow(definition, case_definition, *pending, cure);
    QVERIFY(cured_events.has_value());
    run.journal.push_back(model::WorkflowJournalEntry{cure, *cured_events});
    const auto cured =
        engine::replayWorkflow(definition, case_definition, run.initial_state, run.journal);
    QVERIFY(cured.has_value());
    QVERIFY(cured->deficiencies.front().cured);

    auto tampered = run.journal;
    auto* tampered_deficiency =
        std::get_if<model::WorkflowDeficiencyIssued>(&tampered.at(1).events.front());
    QVERIFY(tampered_deficiency != nullptr);
    tampered_deficiency->cure_deadline_id =
        model::WorkflowDeadlineId{"test.deadline.fabricated-cure"};
    const auto rejected =
        engine::replayWorkflow(definition, case_definition, run.initial_state, tampered);
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, engine::WorkflowErrorCode::InvalidEvent);
}

void WorkflowEngineTest::repeatedDeficienciesUseInstanceIdsAndReplay() {
    const auto definition = workflow();
    const auto case_definition = caseDefinition();
    const auto initial = initialState();
    const auto first_command =
        filing("command.deficient.first", "filing.deficient.first", "test.actor.appellant",
               "test.filing.opening", date(2026, 8, 14), {}, false);
    const auto first = engine::decideWorkflow(definition, case_definition, initial, first_command);
    QVERIFY(first.has_value());
    std::vector journal{model::WorkflowJournalEntry{first_command, *first}};
    const auto after_first = engine::replayWorkflow(definition, case_definition, initial, journal);
    QVERIFY(after_first.has_value());

    const auto second_command =
        filing("command.deficient.second", "filing.deficient.second", "test.actor.appellant",
               "test.filing.opening", date(2026, 8, 14), {}, false);
    const auto second =
        engine::decideWorkflow(definition, case_definition, *after_first, second_command);
    QVERIFY(second.has_value());
    journal.push_back(model::WorkflowJournalEntry{second_command, *second});
    const auto after_second = engine::replayWorkflow(definition, case_definition, initial, journal);
    QVERIFY(after_second.has_value());
    QCOMPARE(after_second->deficiencies.size(), std::size_t{2});
    QCOMPARE(after_second->deadlines.size(), std::size_t{2});
    QVERIFY(after_second->deficiencies.at(0).deficiency_id !=
            after_second->deficiencies.at(1).deficiency_id);
    QVERIFY(after_second->deficiencies.at(0).cure_deadline_id !=
            after_second->deficiencies.at(1).cure_deadline_id);

    const auto replayed = engine::replayWorkflow(definition, case_definition, initial, journal);
    QVERIFY(replayed.has_value());
    QVERIFY(*replayed == *after_second);
}

void WorkflowEngineTest::rejectsUnauthorizedIneligibleAndLateFilings() {
    const auto definition = workflow();
    const auto case_definition = caseDefinition();
    const auto initial = initialState();

    const auto unauthorized_command =
        filing("command.unauthorized", "filing.bad-actor", "test.actor.appellee",
               "test.filing.opening", date(2026, 8, 13), {model::ActorId{"test.actor.appellant"}});
    const auto unauthorized =
        engine::decideWorkflow(definition, case_definition, initial, unauthorized_command);
    QVERIFY(unauthorized.has_value());
    const auto* rejected = std::get_if<model::WorkflowFilingRejected>(&unauthorized->front());
    QVERIFY(rejected != nullptr);
    QCOMPARE(rejected->reason, model::WorkflowFilingRejectionReason::UnauthorizedActor);
    QVERIFY(!rejected->header.authority.primary.id.value.empty());
    QVERIFY(engine::replayWorkflow(
                definition, case_definition, initial,
                std::vector{model::WorkflowJournalEntry{unauthorized_command, *unauthorized}})
                .has_value());

    const auto ineligible_command =
        filing("command.ineligible", "filing.wrong", "test.actor.appellant", "test.filing.response",
               date(2026, 8, 13), {model::ActorId{"test.actor.appellee"}});
    const auto ineligible =
        engine::decideWorkflow(definition, case_definition, initial, ineligible_command);
    QVERIFY(!ineligible.has_value());
    QCOMPARE(ineligible.error().code, engine::WorkflowErrorCode::InvalidCommand);

    auto run = emptyRun();
    QVERIFY(execute(definition, case_definition, run,
                    filing("command.opening", "filing.opening", "test.actor.appellant",
                           "test.filing.opening", date(2026, 8, 13),
                           {model::ActorId{"test.actor.appellee"}}))
                .has_value());
    const auto late_command =
        filing("command.late", "filing.response", "test.actor.appellee", "test.filing.response",
               date(2026, 9, 15), {model::ActorId{"test.actor.appellant"}});
    const auto late = engine::decideWorkflow(definition, case_definition, run.state, late_command);
    QVERIFY(late.has_value());
    rejected = std::get_if<model::WorkflowFilingRejected>(&late->front());
    QVERIFY(rejected != nullptr);
    QCOMPARE(rejected->reason, model::WorkflowFilingRejectionReason::DeadlineExpired);
    auto journal = run.journal;
    journal.push_back(model::WorkflowJournalEntry{late_command, *late});
    QVERIFY(engine::replayWorkflow(definition, case_definition, run.initial_state, journal)
                .has_value());
}

void WorkflowEngineTest::usesRouteLocalRejectionAuthoritiesAndExplicitNonconformance() {
    const auto definition = workflow();
    const auto case_definition = caseDefinition();
    auto run = emptyRun();
    const auto appellant = model::ActorId{"test.actor.appellant"};
    QVERIFY(execute(definition, case_definition, run,
                    filing("command.opening", "filing.opening", "test.actor.appellant",
                           "test.filing.opening", date(2026, 8, 13),
                           {model::ActorId{"test.actor.appellee"}}))
                .has_value());

    const auto unauthorized_extension =
        filing("command.extension-unauthorized", "filing.extension-unauthorized",
               "test.actor.appellant", "test.filing.extension", date(2026, 8, 20), {}, true);
    const auto extension_decision =
        engine::decideWorkflow(definition, case_definition, run.state, unauthorized_extension);
    QVERIFY(extension_decision.has_value());
    const auto* extension_rejection =
        std::get_if<model::WorkflowFilingRejected>(&extension_decision->front());
    QVERIFY(extension_rejection != nullptr);
    QCOMPARE(extension_rejection->header.operation_id.value,
             std::string("test.op.extension.reject"));

    const auto unauthorized_response =
        filing("command.response-unauthorized", "filing.response-unauthorized",
               "test.actor.appellant", "test.filing.response", date(2026, 8, 20), {}, true);
    const auto response_decision =
        engine::decideWorkflow(definition, case_definition, run.state, unauthorized_response);
    QVERIFY(response_decision.has_value());
    const auto* response_rejection =
        std::get_if<model::WorkflowFilingRejected>(&response_decision->front());
    QVERIFY(response_rejection != nullptr);
    QCOMPARE(response_rejection->header.operation_id.value, std::string("test.op.response.reject"));
    QVERIFY(extension_rejection->header.authority != response_rejection->header.authority);

    const auto nonconforming = filing(
        "command.extension-nonconforming", "filing.extension-nonconforming", "test.actor.appellee",
        "test.filing.extension", date(2026, 8, 20), {appellant}, false);
    const auto nonconforming_decision =
        engine::decideWorkflow(definition, case_definition, run.state, nonconforming);
    QVERIFY(nonconforming_decision.has_value());
    const auto* nonconforming_rejection =
        std::get_if<model::WorkflowFilingRejected>(&nonconforming_decision->front());
    QVERIFY(nonconforming_rejection != nullptr);
    QCOMPARE(nonconforming_rejection->reason,
             model::WorkflowFilingRejectionReason::NonconformingFiling);
    QCOMPARE(nonconforming_rejection->header.operation_id.value,
             std::string("test.op.extension.reject"));
}

void WorkflowEngineTest::courtRecordsSourcedDeadlineTriggers() {
    const auto definition = workflow();
    const auto case_definition = caseDefinition();
    const auto initial = initialState();
    const auto command = model::CalculateWorkflowDeadline{
        header("command.court-deadline", "test.actor.court", date(2026, 8, 14)),
        model::WorkflowOperationId{"test.op.court.deadline"},
        model::WorkflowDeadlineId{"test.deadline.court-triggered"}};

    const auto decision = engine::decideWorkflow(definition, case_definition, initial,
                                                 model::WorkflowCommand{command});
    QVERIFY(decision.has_value());
    QCOMPARE(decision->size(), std::size_t{1});
    const auto* calculated = std::get_if<model::WorkflowDeadlineCalculated>(&decision->front());
    QVERIFY(calculated != nullptr);
    QCOMPARE(calculated->base_date, date(2026, 8, 14));
    QCOMPARE(calculated->due_date, date(2026, 8, 18));
    QCOMPARE(calculated->purpose, model::WorkflowDeadlinePurpose::Filing);
    QCOMPARE(calculated->header.authority.primary.id.value,
             std::string("test.op.court.deadline.authority"));

    const std::vector journal{
        model::WorkflowJournalEntry{model::WorkflowCommand{command}, *decision}};
    const auto replayed = engine::replayWorkflow(definition, case_definition, initial, journal);
    QVERIFY(replayed.has_value());
    QCOMPARE(replayed->deadlines.size(), std::size_t{1});
    QCOMPARE(replayed->deadlines.front().due_date, date(2026, 8, 18));

    auto unauthorized = command;
    unauthorized.header.command_id = model::WorkflowCommandId{"command.unauthorized-deadline"};
    unauthorized.header.actor_id = model::ActorId{"test.actor.appellant"};
    const auto denied = engine::decideWorkflow(definition, case_definition, initial,
                                               model::WorkflowCommand{unauthorized});
    QVERIFY(!denied.has_value());
    QCOMPARE(denied.error().code, engine::WorkflowErrorCode::UnauthorizedActor);

    auto duplicate = command;
    duplicate.header.command_id = model::WorkflowCommandId{"command.duplicate-deadline"};
    duplicate.header.occurred_at = at(date(2026, 8, 19));
    const auto duplicate_result = engine::decideWorkflow(definition, case_definition, *replayed,
                                                         model::WorkflowCommand{duplicate});
    QVERIFY(!duplicate_result.has_value());
    QCOMPARE(duplicate_result.error().code, engine::WorkflowErrorCode::InvalidCommand);
}

void WorkflowEngineTest::usesExistingDeadlineAsExactBaseAndReachedBoundary() {
    auto definition = canonicalWorkflow();
    auto base_operation = std::ranges::find(definition.operations,
                                            model::WorkflowOperationId{"test.op.court.deadline"},
                                            &model::WorkflowOperation::id);
    QVERIFY(base_operation != definition.operations.end());
    base_operation->produced_deadline_id = model::WorkflowDeadlineId{"test.deadline.base"};
    auto same_date_base = *base_operation;
    same_date_base.id = model::WorkflowOperationId{"test.op.same-date-base"};
    same_date_base.produced_deadline_id = model::WorkflowDeadlineId{"test.deadline.same-date"};
    definition.operations.push_back(same_date_base);
    auto dependent = operation("test.op.dependent-deadline", appellant_stage,
                               model::WorkflowOpcode::CalculateDeadline, std::nullopt, 7,
                               model::DeadlineCounting::CalendarDays, {court_role});
    dependent.authority.primary.provenance = provenance();
    dependent.deadline_base_id = model::WorkflowDeadlineId{"test.deadline.base"};
    dependent.produced_deadline_id = model::WorkflowDeadlineId{"test.deadline.dependent"};
    dependent.preconditions = {
        model::WorkflowDeadlinePrecondition{model::WorkflowDeadlineId{"test.deadline.base"},
                                            model::WorkflowDeadlineCondition::Reached}};
    definition.operations.push_back(dependent);
    const auto case_definition = caseDefinition();
    const auto initial = initialState();

    const auto base_command = model::CalculateWorkflowDeadline{
        header("command.base-deadline", "test.actor.court", date(2026, 8, 14)),
        model::WorkflowOperationId{"test.op.court.deadline"},
        model::WorkflowDeadlineId{"test.deadline.base"}};
    const auto base_decision = engine::decideWorkflow(definition, case_definition, initial,
                                                      model::WorkflowCommand{base_command});
    QVERIFY(base_decision.has_value());
    const auto* base_event =
        std::get_if<model::WorkflowDeadlineCalculated>(&base_decision->front());
    QVERIFY(base_event != nullptr);
    QCOMPARE(base_event->base_date, date(2026, 8, 14));
    QCOMPARE(base_event->due_date, date(2026, 8, 18));

    std::vector base_journal{
        model::WorkflowJournalEntry{model::WorkflowCommand{base_command}, *base_decision}};
    const auto first_base_state =
        engine::replayWorkflow(definition, case_definition, initial, base_journal);
    QVERIFY(first_base_state.has_value());
    const auto same_date_command = model::CalculateWorkflowDeadline{
        header("command.same-date-base", "test.actor.court", date(2026, 8, 14)), same_date_base.id,
        model::WorkflowDeadlineId{"test.deadline.same-date"}};
    const auto same_date_decision = engine::decideWorkflow(
        definition, case_definition, *first_base_state, model::WorkflowCommand{same_date_command});
    QVERIFY(same_date_decision.has_value());
    base_journal.push_back(model::WorkflowJournalEntry{model::WorkflowCommand{same_date_command},
                                                       *same_date_decision});
    const auto base_state =
        engine::replayWorkflow(definition, case_definition, initial, base_journal);
    QVERIFY(base_state.has_value());

    const auto before_reached = model::CalculateWorkflowDeadline{
        header("command.dependent-too-early", "test.actor.court", date(2026, 8, 17)), dependent.id,
        model::WorkflowDeadlineId{"test.deadline.dependent"}};
    const auto rejected = engine::decideWorkflow(definition, case_definition, *base_state,
                                                 model::WorkflowCommand{before_reached});
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, engine::WorkflowErrorCode::UnmetPrecondition);

    const auto at_reached = model::CalculateWorkflowDeadline{
        header("command.dependent-at-boundary", "test.actor.court", date(2026, 8, 18)),
        dependent.id, model::WorkflowDeadlineId{"test.deadline.dependent"}};
    const auto dependent_decision = engine::decideWorkflow(definition, case_definition, *base_state,
                                                           model::WorkflowCommand{at_reached});
    QVERIFY(dependent_decision.has_value());
    const auto* dependent_event =
        std::get_if<model::WorkflowDeadlineCalculated>(&dependent_decision->front());
    QVERIFY(dependent_event != nullptr);
    QCOMPARE(dependent_event->base_date, date(2026, 8, 18));
    QCOMPARE(dependent_event->due_date, date(2026, 8, 25));
    QCOMPARE(dependent_event->deadline_base_id,
             std::optional{model::WorkflowDeadlineId{"test.deadline.base"}});
    QCOMPARE(dependent_event->produced_deadline_id,
             std::optional{model::WorkflowDeadlineId{"test.deadline.dependent"}});

    auto compatible_guards = dependent;
    compatible_guards.id = model::WorkflowOperationId{"test.op.compatible-deadline-guards"};
    compatible_guards.produced_deadline_id = model::WorkflowDeadlineId{"test.deadline.compatible"};
    compatible_guards.preconditions.push_back(
        model::WorkflowDeadlinePrecondition{model::WorkflowDeadlineId{"test.deadline.base"},
                                            model::WorkflowDeadlineCondition::NotElapsed});
    definition.operations.push_back(compatible_guards);
    const auto exact_boundary = model::CalculateWorkflowDeadline{
        header("command.compatible-deadline-guards", "test.actor.court", date(2026, 8, 18)),
        compatible_guards.id, model::WorkflowDeadlineId{"test.deadline.compatible"}};
    const auto compatible_decision = engine::decideWorkflow(
        definition, case_definition, *base_state, model::WorkflowCommand{exact_boundary});
    QVERIFY(compatible_decision.has_value());

    auto elapsed_guard = compatible_guards;
    elapsed_guard.id = model::WorkflowOperationId{"test.op.reached-and-elapsed"};
    elapsed_guard.produced_deadline_id = model::WorkflowDeadlineId{"test.deadline.after-boundary"};
    elapsed_guard.preconditions.back() = model::WorkflowDeadlinePrecondition{
        model::WorkflowDeadlineId{"test.deadline.base"}, model::WorkflowDeadlineCondition::Elapsed};
    definition.operations.push_back(elapsed_guard);
    const auto after_boundary = model::CalculateWorkflowDeadline{
        header("command.reached-and-elapsed", "test.actor.court", date(2026, 8, 19)),
        elapsed_guard.id, model::WorkflowDeadlineId{"test.deadline.after-boundary"}};
    const auto elapsed_decision = engine::decideWorkflow(definition, case_definition, *base_state,
                                                         model::WorkflowCommand{after_boundary});
    QVERIFY(elapsed_decision.has_value());

    auto journal = base_journal;
    journal.push_back(
        model::WorkflowJournalEntry{model::WorkflowCommand{at_reached}, *dependent_decision});
    const auto replayed = engine::replayWorkflow(definition, case_definition, initial, journal);
    QVERIFY(replayed.has_value());
    QCOMPARE(replayed->deadlines.back().due_date, date(2026, 8, 25));

    auto tampered = journal;
    auto& tampered_event =
        std::get<model::WorkflowDeadlineCalculated>(tampered.back().events.front());
    tampered_event.base_date = date(2026, 8, 19);
    tampered_event.due_date = date(2026, 8, 26);
    const auto tampered_result =
        engine::replayWorkflow(definition, case_definition, initial, tampered);
    QVERIFY(!tampered_result.has_value());
    QCOMPARE(tampered_result.error().code, engine::WorkflowErrorCode::InvalidEvent);

    auto binding_tampered = journal;
    auto& binding_event =
        std::get<model::WorkflowDeadlineCalculated>(binding_tampered.back().events.front());
    binding_event.deadline_base_id = model::WorkflowDeadlineId{"test.deadline.same-date"};
    const auto binding_result =
        engine::replayWorkflow(definition, case_definition, initial, binding_tampered);
    QVERIFY(!binding_result.has_value());
    QCOMPARE(binding_result.error().code, engine::WorkflowErrorCode::InvalidEvent);

    auto definition_substituted = definition;
    auto substituted_operation = std::ranges::find(definition_substituted.operations, dependent.id,
                                                   &model::WorkflowOperation::id);
    QVERIFY(substituted_operation != definition_substituted.operations.end());
    substituted_operation->deadline_base_id = model::WorkflowDeadlineId{"test.deadline.same-date"};
    const auto substituted_result =
        engine::replayWorkflow(definition_substituted, case_definition, initial, journal);
    QVERIFY(!substituted_result.has_value());
    QCOMPARE(substituted_result.error().code, engine::WorkflowErrorCode::InvalidEvent);

    const auto missing_base = engine::decideWorkflow(definition, case_definition, initial,
                                                     model::WorkflowCommand{at_reached});
    QVERIFY(!missing_base.has_value());
    QCOMPARE(missing_base.error().code, engine::WorkflowErrorCode::UnmetPrecondition);

    auto forged = definition;
    auto wrong_opcode =
        std::ranges::find(forged.operations, model::WorkflowOperationId{"test.op.opening.accept"},
                          &model::WorkflowOperation::id);
    QVERIFY(wrong_opcode != forged.operations.end());
    wrong_opcode->deadline_base_id = model::WorkflowDeadlineId{"test.deadline.base"};
    const auto forged_result = engine::decideWorkflow(forged, case_definition, initial,
                                                      model::WorkflowCommand{base_command});
    QVERIFY(!forged_result.has_value());
    QCOMPARE(forged_result.error().code, engine::WorkflowErrorCode::InvalidDefinition);

    auto accepted_route_forgery = definition;
    QVERIFY(accepted_route_forgery.filing_routes.front().accepted_deadline.has_value());
    accepted_route_forgery.filing_routes.front().accepted_deadline->operation_id = dependent.id;
    const auto accepted_route_result = engine::decideWorkflow(
        accepted_route_forgery, case_definition, initial, model::WorkflowCommand{base_command});
    QVERIFY(!accepted_route_result.has_value());
    QCOMPARE(accepted_route_result.error().code, engine::WorkflowErrorCode::InvalidDefinition);

    auto deficiency_route_forgery = definition;
    QVERIFY(deficiency_route_forgery.filing_routes.front().deficiency_deadline.has_value());
    deficiency_route_forgery.filing_routes.front().deficiency_deadline->operation_id = dependent.id;
    const auto deficiency_route_result = engine::decideWorkflow(
        deficiency_route_forgery, case_definition, initial, model::WorkflowCommand{base_command});
    QVERIFY(!deficiency_route_result.has_value());
    QCOMPARE(deficiency_route_result.error().code, engine::WorkflowErrorCode::InvalidDefinition);

    auto reserved_definition = definition;
    auto unnamed = operation("test.op.unnamed-deadline", appellant_stage,
                             model::WorkflowOpcode::CalculateDeadline, std::nullopt, 1,
                             model::DeadlineCounting::CalendarDays, {court_role});
    unnamed.authority.primary.provenance = provenance();
    reserved_definition.operations.push_back(unnamed);
    const auto reserved_command = model::CalculateWorkflowDeadline{
        header("command.reserved-deadline", "test.actor.court", date(2026, 8, 14)), unnamed.id,
        model::WorkflowDeadlineId{"test.deadline.base"}};
    const auto reserved_result = engine::decideWorkflow(
        reserved_definition, case_definition, initial, model::WorkflowCommand{reserved_command});
    QVERIFY(!reserved_result.has_value());
    QCOMPARE(reserved_result.error().code, engine::WorkflowErrorCode::InvalidCommand);

    const model::CalculateWorkflowDeadline accepted_squat{
        header("command.squat-accepted", "test.actor.court", date(2026, 8, 14)), unnamed.id,
        model::WorkflowDeadlineId{response_deadline}};
    const auto accepted_squat_result = engine::decideWorkflow(
        reserved_definition, case_definition, initial, model::WorkflowCommand{accepted_squat});
    QVERIFY(!accepted_squat_result.has_value());
    QCOMPARE(accepted_squat_result.error().code, engine::WorkflowErrorCode::InvalidCommand);

    const model::CalculateWorkflowDeadline deficiency_squat{
        header("command.squat-deficiency", "test.actor.court", date(2026, 8, 14)), unnamed.id,
        model::WorkflowDeadlineId{"test.deadline.opening-cure.command.squat-deficiency"}};
    const auto deficiency_squat_result = engine::decideWorkflow(
        reserved_definition, case_definition, initial, model::WorkflowCommand{deficiency_squat});
    QVERIFY(!deficiency_squat_result.has_value());
    QCOMPARE(deficiency_squat_result.error().code, engine::WorkflowErrorCode::InvalidCommand);

    const model::CalculateWorkflowDeadline unreserved{
        header("command.unreserved", "test.actor.court", date(2026, 8, 14)), unnamed.id,
        model::WorkflowDeadlineId{"test.deadline.unreserved"}};
    const auto unreserved_decision = engine::decideWorkflow(
        reserved_definition, case_definition, initial, model::WorkflowCommand{unreserved});
    QVERIFY(unreserved_decision.has_value());
    for (const auto& squatted_id :
         {model::WorkflowDeadlineId{response_deadline},
          model::WorkflowDeadlineId{"test.deadline.opening-cure.command.unreserved"}}) {
        auto squatted_events = *unreserved_decision;
        std::get<model::WorkflowDeadlineCalculated>(squatted_events.front()).deadline_id =
            squatted_id;
        const auto squatted_replay = engine::replayWorkflow(
            reserved_definition, case_definition, initial,
            std::vector{model::WorkflowJournalEntry{model::WorkflowCommand{unreserved},
                                                    std::move(squatted_events)}});
        QVERIFY(!squatted_replay.has_value());
        QCOMPARE(squatted_replay.error().code, engine::WorkflowErrorCode::InvalidEvent);
    }

    auto authorized_route_deadline = definition;
    const auto route_calculation = std::ranges::find(
        authorized_route_deadline.operations,
        model::WorkflowOperationId{"test.op.response.deadline"}, &model::WorkflowOperation::id);
    QVERIFY(route_calculation != authorized_route_deadline.operations.end());
    route_calculation->authorized_roles = {model::ActorRoleId{court_role}};
    const auto deficiency_calculation = std::ranges::find(
        authorized_route_deadline.operations,
        model::WorkflowOperationId{"test.op.opening.cure-deadline"}, &model::WorkflowOperation::id);
    QVERIFY(deficiency_calculation != authorized_route_deadline.operations.end());
    deficiency_calculation->authorized_roles = {model::ActorRoleId{court_role}};

    auto accepted_route_run = emptyRun();
    const auto accepted_route_execution =
        execute(authorized_route_deadline, case_definition, accepted_route_run,
                filing("command.authorized-route-accepted", "filing.authorized-route-accepted",
                       "test.actor.appellant", "test.filing.opening", date(2026, 8, 14),
                       {model::ActorId{"test.actor.appellee"}}));
    QVERIFY2(accepted_route_execution.has_value(),
             accepted_route_execution ? "" : accepted_route_execution.error().c_str());
    QVERIFY(std::ranges::find(accepted_route_run.state.deadlines,
                              model::WorkflowDeadlineId{response_deadline},
                              &model::WorkflowDeadlineRecord::deadline_id) !=
            accepted_route_run.state.deadlines.end());

    auto deficiency_route_run = emptyRun();
    const auto deficiency_route_execution =
        execute(authorized_route_deadline, case_definition, deficiency_route_run,
                filing("command.authorized-route-deficiency", "filing.authorized-route-deficiency",
                       "test.actor.appellant", "test.filing.opening", date(2026, 8, 14),
                       {model::ActorId{"test.actor.appellee"}}, false));
    QVERIFY2(deficiency_route_execution.has_value(),
             deficiency_route_execution ? "" : deficiency_route_execution.error().c_str());
    const auto authorized_deficiency_deadline =
        model::WorkflowDeadlineId{"test.deadline.opening-cure.command.authorized-route-deficiency"};
    QVERIFY(std::ranges::find(deficiency_route_run.state.deadlines, authorized_deficiency_deadline,
                              &model::WorkflowDeadlineRecord::deadline_id) !=
            deficiency_route_run.state.deadlines.end());

    const model::CalculateWorkflowDeadline authorized_unreserved{
        header("command.authorized-route-unreserved", "test.actor.court", date(2026, 8, 14)),
        route_calculation->id, model::WorkflowDeadlineId{"test.deadline.authorized-unreserved"}};
    const auto authorized_unreserved_result =
        engine::decideWorkflow(authorized_route_deadline, case_definition, initial,
                               model::WorkflowCommand{authorized_unreserved});
    QVERIFY(authorized_unreserved_result.has_value());
    const model::CalculateWorkflowDeadline owning_route_squat{
        header("command.owning-route-squat", "test.actor.court", date(2026, 8, 14)),
        route_calculation->id, model::WorkflowDeadlineId{response_deadline}};
    const auto owning_route_squat_result =
        engine::decideWorkflow(authorized_route_deadline, case_definition, initial,
                               model::WorkflowCommand{owning_route_squat});
    QVERIFY(!owning_route_squat_result.has_value());
    QCOMPARE(owning_route_squat_result.error().code, engine::WorkflowErrorCode::InvalidCommand);
    const model::CalculateWorkflowDeadline owning_deficiency_squat{
        header("command.owning-deficiency-squat", "test.actor.court", date(2026, 8, 14)),
        deficiency_calculation->id,
        model::WorkflowDeadlineId{"test.deadline.opening-cure.command.owning-deficiency-squat"}};
    const auto owning_deficiency_squat_result =
        engine::decideWorkflow(authorized_route_deadline, case_definition, initial,
                               model::WorkflowCommand{owning_deficiency_squat});
    QVERIFY(!owning_deficiency_squat_result.has_value());
    QCOMPARE(owning_deficiency_squat_result.error().code,
             engine::WorkflowErrorCode::InvalidCommand);

    auto overlapping_namespace = canonicalWorkflow();
    const auto overlapping_calculation = std::ranges::find(
        overlapping_namespace.operations, model::WorkflowOperationId{"test.op.court.deadline"},
        &model::WorkflowOperation::id);
    QVERIFY(overlapping_calculation != overlapping_namespace.operations.end());
    overlapping_calculation->produced_deadline_id =
        model::WorkflowDeadlineId{"test.deadline.opening-cure.command.future"};
    const model::CalculateWorkflowDeadline overlapping_command{
        header("command.overlapping-namespace", "test.actor.court", date(2026, 8, 14)),
        overlapping_calculation->id, *overlapping_calculation->produced_deadline_id};
    const auto overlapping_result =
        engine::decideWorkflow(overlapping_namespace, case_definition, initial,
                               model::WorkflowCommand{overlapping_command});
    QVERIFY(!overlapping_result.has_value());
    QCOMPARE(overlapping_result.error().code, engine::WorkflowErrorCode::InvalidDefinition);

    auto unnamed_extended_definition = definition;
    auto unnamed_extended = unnamed;
    unnamed_extended.id = model::WorkflowOperationId{"test.op.unnamed-extended-deadline"};
    unnamed_extended.preconditions = {
        model::WorkflowDeadlinePrecondition{model::WorkflowDeadlineId{"test.deadline.base"},
                                            model::WorkflowDeadlineCondition::Reached},
    };
    unnamed_extended_definition.operations.push_back(unnamed_extended);
    const auto unnamed_extended_result = engine::decideWorkflow(
        unnamed_extended_definition, case_definition, *base_state,
        model::WorkflowCommand{model::CalculateWorkflowDeadline{
            header("command.unnamed-extended", "test.actor.court", date(2026, 8, 18)),
            unnamed_extended.id, model::WorkflowDeadlineId{"test.deadline.unnamed-extended"}}});
    QVERIFY(!unnamed_extended_result.has_value());
    QCOMPARE(unnamed_extended_result.error().code, engine::WorkflowErrorCode::InvalidDefinition);
}

void WorkflowEngineTest::usesExactEventDatesAndArgumentDateBoundary() {
    auto definition = canonicalWorkflow();
    auto judgment = std::ranges::find(definition.operations,
                                      model::WorkflowOperationId{"test.op.issue-judgment"},
                                      &model::WorkflowOperation::id);
    QVERIFY(judgment != definition.operations.end());
    judgment->preconditions = {
        model::WorkflowArgumentPrecondition{true},
        model::WorkflowArgumentDatePrecondition{model::WorkflowArgumentDateCondition::Reached},
    };

    auto source_order =
        operation("test.op.clock-source-order", submitted_stage, model::WorkflowOpcode::EnterOrder,
                  std::nullopt, std::nullopt, std::nullopt, {court_role});
    source_order.authority.primary.provenance = provenance();
    auto unrelated_order = source_order;
    unrelated_order.id = model::WorkflowOperationId{"test.op.unrelated-order"};
    unrelated_order.authority = authority(unrelated_order.id.value);
    unrelated_order.authority.primary.provenance = provenance();
    definition.operations.push_back(source_order);
    definition.operations.push_back(unrelated_order);

    auto order_clock = operation("test.op.calculate-order-clock", submitted_stage,
                                 model::WorkflowOpcode::CalculateDeadline, std::nullopt, 3,
                                 model::DeadlineCounting::CalendarDays, {court_role});
    order_clock.authority.primary.provenance = provenance();
    order_clock.produced_deadline_id = model::WorkflowDeadlineId{"test.deadline.order-clock"};
    order_clock.deadline_event_base = model::WorkflowOrderOccurredDeadlineBase{
        model::WorkflowOrderId{"test.order.clock-source"}, source_order.id};
    definition.operations.push_back(order_clock);

    auto judgment_clock = operation("test.op.calculate-judgment-clock", judgment_stage,
                                    model::WorkflowOpcode::CalculateDeadline, std::nullopt, 45,
                                    model::DeadlineCounting::CalendarDays, {court_role});
    judgment_clock.authority.primary.provenance = provenance();
    judgment_clock.produced_deadline_id = model::WorkflowDeadlineId{"test.deadline.judgment-clock"};
    judgment_clock.deadline_event_base = model::WorkflowJudgmentOccurredDeadlineBase{};
    definition.operations.push_back(judgment_clock);

    const auto source = submittedRun();
    QVERIFY2(source.has_value(), source ? "" : source.error().c_str());
    const auto case_definition = caseDefinition();
    auto run = emptyRun();
    for (const auto& entry : source->journal) {
        const auto result = execute(definition, case_definition, run, entry.command);
        QVERIFY2(result.has_value(), result ? "" : result.error().c_str());
    }

    const model::WorkflowCommand order_command = model::EnterWorkflowOrder{
        header("command.clock-source-order", "test.actor.court", date(2026, 10, 7)),
        source_order.id,
        model::WorkflowOrderId{"test.order.clock-source"},
        model::WorkflowOrderDisposition::Other,
        std::string(64, 'e'),
        std::nullopt};
    QVERIFY(execute(definition, case_definition, run, order_command).has_value());
    QCOMPARE(run.state.orders.back().operation_id, std::optional{source_order.id});
    QCOMPARE(run.state.orders.back().entered_at, std::optional{at(date(2026, 10, 7))});

    auto wrong_selector = definition;
    auto wrong_order_clock =
        std::ranges::find(wrong_selector.operations, order_clock.id, &model::WorkflowOperation::id);
    QVERIFY(wrong_order_clock != wrong_selector.operations.end());
    wrong_order_clock->deadline_event_base = model::WorkflowOrderOccurredDeadlineBase{
        model::WorkflowOrderId{"test.order.clock-source"}, unrelated_order.id};
    const model::WorkflowCommand delayed_order_clock = model::CalculateWorkflowDeadline{
        header("command.calculate-order-clock", "test.actor.court", date(2026, 10, 10)),
        order_clock.id, *order_clock.produced_deadline_id};
    const auto selector_rejected =
        engine::decideWorkflow(wrong_selector, case_definition, run.state, delayed_order_clock);
    QVERIFY(!selector_rejected.has_value());
    QCOMPARE(selector_rejected.error().code, engine::WorkflowErrorCode::UnmetPrecondition);

    QVERIFY(execute(definition, case_definition, run, delayed_order_clock).has_value());
    const auto& order_deadline = run.state.deadlines.back();
    QCOMPARE(order_deadline.deadline_id, *order_clock.produced_deadline_id);
    QCOMPARE(order_deadline.due_date, date(2026, 10, 12));
    const auto* order_deadline_event =
        std::get_if<model::WorkflowDeadlineCalculated>(&run.trace.back());
    QVERIFY(order_deadline_event != nullptr);
    QCOMPARE(order_deadline_event->base_date, date(2026, 10, 7));
    QCOMPARE(order_deadline_event->deadline_event_base, order_clock.deadline_event_base);

    const model::WorkflowCommand schedule = model::ScheduleWorkflowArgument{
        header("command.argument-date-guard", "test.actor.court", date(2026, 10, 12)),
        model::WorkflowOperationId{"test.op.schedule-argument"}, date(2026, 10, 20)};
    QVERIFY(execute(definition, case_definition, run, schedule).has_value());

    const model::WorkflowCommand early_judgment = model::IssueWorkflowJudgment{
        header("command.early-judgment", "test.actor.court", date(2026, 10, 19)), judgment->id,
        std::string(64, 'c'), "affirmed"};
    const auto early_result =
        engine::decideWorkflow(definition, case_definition, run.state, early_judgment);
    QVERIFY(!early_result.has_value());
    QCOMPARE(early_result.error().code, engine::WorkflowErrorCode::UnmetPrecondition);

    const model::WorkflowCommand on_date_judgment = model::IssueWorkflowJudgment{
        header("command.on-date-judgment", "test.actor.court", date(2026, 10, 20)), judgment->id,
        std::string(64, 'c'), "affirmed"};
    QVERIFY(execute(definition, case_definition, run, on_date_judgment).has_value());
    QCOMPARE(run.state.judgment_issued_at, std::optional{at(date(2026, 10, 20))});

    const model::WorkflowCommand delayed_judgment_clock = model::CalculateWorkflowDeadline{
        header("command.calculate-judgment-clock", "test.actor.court", date(2026, 11, 2)),
        judgment_clock.id, *judgment_clock.produced_deadline_id};
    const auto deadline_result =
        engine::decideWorkflow(definition, case_definition, run.state, delayed_judgment_clock);
    QVERIFY(deadline_result.has_value());
    const auto* judgment_deadline =
        std::get_if<model::WorkflowDeadlineCalculated>(&deadline_result->front());
    QVERIFY(judgment_deadline != nullptr);
    QCOMPARE(judgment_deadline->base_date, date(2026, 10, 20));
    QCOMPARE(judgment_deadline->due_date, date(2026, 12, 4));
    QCOMPARE(judgment_deadline->deadline_event_base, judgment_clock.deadline_event_base);

    auto journal = run.journal;
    journal.push_back(model::WorkflowJournalEntry{delayed_judgment_clock, *deadline_result});
    const auto replayed =
        engine::replayWorkflow(definition, case_definition, run.initial_state, journal);
    QVERIFY(replayed.has_value());

    auto tampered = journal;
    auto& tampered_deadline =
        std::get<model::WorkflowDeadlineCalculated>(tampered.back().events.front());
    tampered_deadline.deadline_event_base = model::WorkflowOrderOccurredDeadlineBase{
        model::WorkflowOrderId{"test.order.clock-source"}, source_order.id};
    const auto tampered_result =
        engine::replayWorkflow(definition, case_definition, run.initial_state, tampered);
    QVERIFY(!tampered_result.has_value());
    QCOMPARE(tampered_result.error().code, engine::WorkflowErrorCode::InvalidEvent);
}

void WorkflowEngineTest::courtExplicitlyAdvancesSourcedStages() {
    const auto definition = workflow();
    const auto case_definition = caseDefinition();
    const auto initial = initialState();
    const auto command = model::AdvanceWorkflowStage{
        header("command.court-advance", "test.actor.court", date(2026, 8, 14)),
        model::WorkflowOperationId{"test.op.court-advance"}};

    const auto decision = engine::decideWorkflow(definition, case_definition, initial,
                                                 model::WorkflowCommand{command});
    QVERIFY(decision.has_value());
    QCOMPARE(decision->size(), std::size_t{1});
    const auto* advanced = std::get_if<model::WorkflowStageAdvanced>(&decision->front());
    QVERIFY(advanced != nullptr);
    QCOMPARE(advanced->previous_stage_id.value, std::string(appellant_stage));
    QCOMPARE(advanced->next_stage_id.value, std::string(appellee_stage));
    QCOMPARE(advanced->header.operation_id.value, std::string("test.op.court-advance"));
    QCOMPARE(advanced->header.authority.primary.id.value,
             std::string("test.op.court-advance.authority"));

    const std::vector journal{
        model::WorkflowJournalEntry{model::WorkflowCommand{command}, *decision}};
    const auto replayed = engine::replayWorkflow(definition, case_definition, initial, journal);
    QVERIFY(replayed.has_value());
    QCOMPARE(replayed->current_stage_id.value, std::string(appellee_stage));

    auto tampered_journal = journal;
    std::get<model::WorkflowStageAdvanced>(tampered_journal.front().events.front()).next_stage_id =
        model::WorkflowStageId{reply_stage};
    const auto tampered =
        engine::replayWorkflow(definition, case_definition, initial, tampered_journal);
    QVERIFY(!tampered.has_value());
    QCOMPARE(tampered.error().code, engine::WorkflowErrorCode::InvalidEvent);

    auto unauthorized = command;
    unauthorized.header.command_id = model::WorkflowCommandId{"command.party-advance"};
    unauthorized.header.actor_id = model::ActorId{"test.actor.appellant"};
    const auto party_result = engine::decideWorkflow(definition, case_definition, initial,
                                                     model::WorkflowCommand{unauthorized});
    QVERIFY(!party_result.has_value());
    QCOMPARE(party_result.error().code, engine::WorkflowErrorCode::UnauthorizedActor);

    auto wrong_stage = command;
    wrong_stage.header.command_id = model::WorkflowCommandId{"command.wrong-stage-advance"};
    wrong_stage.header.occurred_at = at(date(2026, 8, 15));
    const auto wrong_stage_result = engine::decideWorkflow(definition, case_definition, *replayed,
                                                           model::WorkflowCommand{wrong_stage});
    QVERIFY(!wrong_stage_result.has_value());
    QCOMPARE(wrong_stage_result.error().code, engine::WorkflowErrorCode::InvalidCommand);

    auto wrong_opcode = command;
    wrong_opcode.header.command_id = model::WorkflowCommandId{"command.wrong-opcode-advance"};
    wrong_opcode.operation_id = model::WorkflowOperationId{"test.op.court.deadline"};
    const auto wrong_opcode_result = engine::decideWorkflow(definition, case_definition, initial,
                                                            model::WorkflowCommand{wrong_opcode});
    QVERIFY(!wrong_opcode_result.has_value());
    QCOMPARE(wrong_opcode_result.error().code, engine::WorkflowErrorCode::InvalidCommand);

    const auto deadline_command = model::CalculateWorkflowDeadline{
        header("command.before-advance", "test.actor.court", date(2026, 8, 14)),
        model::WorkflowOperationId{"test.op.court.deadline"},
        model::WorkflowDeadlineId{"test.deadline.before-advance"}};
    const auto deadline_decision = engine::decideWorkflow(definition, case_definition, initial,
                                                          model::WorkflowCommand{deadline_command});
    QVERIFY(deadline_decision.has_value());
    const std::vector deadline_journal{
        model::WorkflowJournalEntry{model::WorkflowCommand{deadline_command}, *deadline_decision}};
    const auto after_deadline =
        engine::replayWorkflow(definition, case_definition, initial, deadline_journal);
    QVERIFY(after_deadline.has_value());
    auto backdated = command;
    backdated.header.command_id = model::WorkflowCommandId{"command.backdated-advance"};
    backdated.header.occurred_at = at(date(2026, 8, 13));
    const auto backdated_result = engine::decideWorkflow(
        definition, case_definition, *after_deadline, model::WorkflowCommand{backdated});
    QVERIFY(!backdated_result.has_value());
    QCOMPARE(backdated_result.error().code, engine::WorkflowErrorCode::BackdatedCommand);

    auto leaking_definition = definition;
    leaking_definition.filing_routes.front().advance_operation_id =
        model::WorkflowOperationId{"test.op.court-advance"};
    const auto party_filing =
        filing("command.leaking-route", "filing.leaking-route", "test.actor.appellant",
               "test.filing.opening", date(2026, 8, 14), {model::ActorId{"test.actor.appellee"}});
    const auto leak_result = engine::decideWorkflow(leaking_definition, case_definition, initial,
                                                    model::WorkflowCommand{party_filing});
    QVERIFY(!leak_result.has_value());
    QCOMPARE(leak_result.error().code, engine::WorkflowErrorCode::InvalidDefinition);
}

void WorkflowEngineTest::rejectsUnauthorizedCourtActionAndMissingAuthority() {
    auto definition = workflow();
    const auto case_definition = caseDefinition();
    const auto submitted = submittedRun();
    QVERIFY2(submitted.has_value(), submitted ? "" : submitted.error().c_str());
    const model::WorkflowCommand command = model::ScheduleWorkflowArgument{
        header("command.bad-argument", "test.actor.appellant", date(2026, 10, 6)),
        model::WorkflowOperationId{"test.op.schedule-argument"}, date(2026, 11, 10)};
    auto result = engine::decideWorkflow(definition, case_definition, submitted->state, command);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::UnauthorizedActor);

    auto operation = std::ranges::find(definition.operations,
                                       model::WorkflowOperationId{"test.op.schedule-argument"},
                                       &model::WorkflowOperation::id);
    QVERIFY(operation != definition.operations.end());
    operation->authority.primary.source_version.clear();
    result = engine::decideWorkflow(definition, case_definition, submitted->state, command);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::MissingAuthority);
}

void WorkflowEngineTest::validatesCompleteProvenanceAndRejectsMutations() {
    auto definition = workflow();
    const auto case_definition = caseDefinition();
    const auto initial = initialState();
    const model::WorkflowCommand command = model::CalculateWorkflowDeadline{
        header("command.provenance", "test.actor.court", date(2026, 8, 14)),
        model::WorkflowOperationId{"test.op.court.deadline"},
        model::WorkflowDeadlineId{"test.deadline.provenance"}};
    for (auto& candidate : definition.operations) {
        candidate.authority.primary.provenance = provenance();
    }
    auto operation = std::ranges::find(definition.operations,
                                       model::WorkflowOperationId{"test.op.court.deadline"},
                                       &model::WorkflowOperation::id);
    QVERIFY(operation != definition.operations.end());
    auto result = engine::decideWorkflow(definition, case_definition, initial, command);
    QVERIFY(result.has_value());
    QVERIFY(eventHeader(result->front()).authority.primary.provenance.has_value());

    operation->authority.primary.provenance->source_url = "javascript:alert(1)";
    result = engine::decideWorkflow(definition, case_definition, initial, command);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::MissingAuthority);

    operation->authority.primary.provenance = provenance();
    operation->authority.primary.provenance->precedential_status =
        static_cast<model::PrecedentialStatus>(99);
    result = engine::decideWorkflow(definition, case_definition, initial, command);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::MissingAuthority);

    operation->authority.primary.provenance = provenance();
    operation->authority.primary.provenance->checked_on = "2025-08-11";
    result = engine::decideWorkflow(definition, case_definition, initial, command);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::MissingAuthority);

    operation->authority.primary.provenance = provenance();
    operation->authority.supporting.push_back(authority("test.op.legacy").primary);
    result = engine::decideWorkflow(definition, case_definition, initial, command);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::MissingAuthority);

    operation->authority.supporting.clear();
    definition.operations.front().authority.primary.provenance.reset();
    result = engine::decideWorkflow(definition, case_definition, initial, command);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::MissingAuthority);

    definition.operations.front().authority.primary.provenance = provenance();
    auto unicode_value = std::string{};
    unicode_value.reserve(2000 * std::string("한").size());
    for (int index = 0; index < 2000; ++index) {
        unicode_value += "한";
    }
    operation->authority.primary.citation = unicode_value;
    operation->authority.primary.proposition = unicode_value;
    operation->authority.primary.provenance->locator = unicode_value;
    result = engine::decideWorkflow(definition, case_definition, initial, command);
    QVERIFY(result.has_value());
}

void WorkflowEngineTest::replayRejectsImpossibleGroupsAndForgedCourtActions() {
    const auto definition = workflow();
    const auto case_definition = caseDefinition();
    const auto initial = initialState();
    const auto opening_command =
        filing("command.opening", "filing.opening", "test.actor.appellant", "test.filing.opening",
               date(2026, 8, 13), {model::ActorId{"test.actor.appellee"}});
    const auto opening =
        engine::decideWorkflow(definition, case_definition, initial, opening_command);
    QVERIFY(opening.has_value());
    auto impossible = *opening;
    const auto reject_id = model::WorkflowOperationId{"test.op.opening.reject"};
    const auto reject_operation =
        std::ranges::find(definition.operations, reject_id, &model::WorkflowOperation::id);
    QVERIFY(reject_operation != definition.operations.end());
    auto reject_header = eventHeader(impossible.front());
    reject_header.operation_id = reject_operation->id;
    reject_header.sequence += 1;
    reject_header.command_event_index = 1;
    reject_header.authority = reject_operation->authority;
    impossible.at(1) = model::WorkflowFilingRejected{
        std::move(reject_header), opening_command.filing_id, opening_command.filing_type,
        opening_command.header.actor_id, model::WorkflowFilingRejectionReason::IneligibleFiling};
    auto replayed = engine::replayWorkflow(
        definition, case_definition, initial,
        std::vector{model::WorkflowJournalEntry{opening_command, std::move(impossible)}});
    QVERIFY(!replayed.has_value());
    QCOMPARE(replayed.error().code, engine::WorkflowErrorCode::InvalidEvent);

    const auto submitted = submittedRun();
    QVERIFY2(submitted.has_value(), submitted ? "" : submitted.error().c_str());
    const model::WorkflowCommand seal_command = model::SetWorkflowSealed{
        header("command.forged-action", "test.actor.court", date(2026, 9, 21)),
        model::WorkflowOperationId{"test.op.set-sealed"}, true};
    const auto schedule_operation = std::ranges::find(
        definition.operations, model::WorkflowOperationId{"test.op.schedule-argument"},
        &model::WorkflowOperation::id);
    QVERIFY(schedule_operation != definition.operations.end());
    const auto& seal_header = std::get<model::SetWorkflowSealed>(seal_command).header;
    const model::WorkflowEvent forged_action = model::WorkflowArgumentScheduled{
        model::WorkflowEventHeader{
            submitted->state.session_id,
            definition.id,
            seal_header.command_id,
            schedule_operation->id,
            submitted->state.next_event_sequence,
            0,
            1,
            seal_header.occurred_at,
            schedule_operation->authority,
        },
        date(2026, 10, 20),
        schedule_operation->next_stage_id,
    };
    auto forged_journal = submitted->journal;
    forged_journal.push_back(model::WorkflowJournalEntry{seal_command, {forged_action}});
    replayed = engine::replayWorkflow(definition, case_definition, initial, forged_journal);
    QVERIFY(!replayed.has_value());
    QCOMPARE(replayed.error().code, engine::WorkflowErrorCode::InvalidEvent);
}

void WorkflowEngineTest::rejectsBackdatedExplicitLegalTime() {
    const auto definition = workflow();
    const auto case_definition = caseDefinition();
    auto run = emptyRun();
    QVERIFY(execute(definition, case_definition, run,
                    filing("command.opening", "filing.opening", "test.actor.appellant",
                           "test.filing.opening", date(2026, 8, 13),
                           {model::ActorId{"test.actor.appellee"}}))
                .has_value());
    QVERIFY(run.state.legal_time_cursor == at(date(2026, 8, 13)));

    auto both_backdated = filing("command.backdated-both", "filing.extension.backdated-both",
                                 "test.actor.appellee", "test.filing.extension", date(2026, 8, 12),
                                 {model::ActorId{"test.actor.appellant"}});
    auto result = engine::decideWorkflow(definition, case_definition, run.state, both_backdated);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::BackdatedCommand);

    auto instant_backdated = filing(
        "command.backdated-instant", "filing.extension.backdated-instant", "test.actor.appellee",
        "test.filing.extension", date(2026, 8, 14), {model::ActorId{"test.actor.appellant"}});
    instant_backdated.header.occurred_at.instant = at(date(2026, 8, 12)).instant;
    result = engine::decideWorkflow(definition, case_definition, run.state, instant_backdated);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::BackdatedCommand);

    auto court_date_backdated =
        filing("command.backdated-court-date", "filing.extension.backdated-court-date",
               "test.actor.appellee", "test.filing.extension", date(2026, 8, 12),
               {model::ActorId{"test.actor.appellant"}});
    court_date_backdated.header.occurred_at.instant = at(date(2026, 8, 14)).instant;
    result = engine::decideWorkflow(definition, case_definition, run.state, court_date_backdated);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::BackdatedCommand);
}

void WorkflowEngineTest::rejectsDirtySnapshotsAndMalformedDefinitions() {
    const auto case_definition = caseDefinition();
    const auto initial = initialState();
    auto dirty = initial;
    dirty.current_stage_id = model::WorkflowStageId{judgment_stage};
    dirty.next_event_sequence = 2;
    dirty.decided_commands.push_back(model::WorkflowCommandId{"command.forged-judgment"});
    dirty.judgment_sha256 = std::string(64, 'f');
    dirty.legal_time_cursor = at(date(2026, 11, 10));
    auto replayed = engine::replayWorkflow(workflow(), case_definition, dirty,
                                           std::vector<model::WorkflowJournalEntry>{});
    QVERIFY(!replayed.has_value());
    QCOMPARE(replayed.error().code, engine::WorkflowErrorCode::InvalidState);

    auto malformed = workflow();
    malformed.id.value = "invalid_workflow";
    auto result = engine::decideWorkflow(
        malformed, case_definition, initial,
        filing("command.opening", "filing.opening", "test.actor.appellant", "test.filing.opening",
               date(2026, 8, 13), {model::ActorId{"test.actor.appellee"}}));
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::InvalidDefinition);

    malformed = workflow();
    malformed.calendar.holidays.push_back(malformed.calendar.holidays.front());
    result = engine::decideWorkflow(
        malformed, case_definition, initial,
        filing("command.opening", "filing.opening", "test.actor.appellant", "test.filing.opening",
               date(2026, 8, 13), {model::ActorId{"test.actor.appellee"}}));
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::InvalidDefinition);

    malformed = workflow();
    malformed.operations.front().authority.primary.source_version = "2026-02-30";
    result = engine::decideWorkflow(
        malformed, case_definition, initial,
        filing("command.opening", "filing.opening", "test.actor.appellant", "test.filing.opening",
               date(2026, 8, 13), {model::ActorId{"test.actor.appellee"}}));
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::MissingAuthority);

    malformed = workflow();
    malformed.filing_routes.front().authorized_role_scope =
        model::WorkflowAuthorizedRoleScope::CatalogSubset;
    result = engine::decideWorkflow(
        malformed, case_definition, initial,
        filing("command.legacy-role-subset", "filing.legacy-role-subset", "test.actor.appellant",
               "test.filing.opening", date(2026, 8, 13), {model::ActorId{"test.actor.appellee"}}));
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::InvalidDefinition);

    malformed = workflow();
    QVERIFY(malformed.filing_routes.front().deficiency_deadline.has_value());
    malformed.filing_routes.front().deficiency_deadline->static_trigger =
        model::WorkflowDeadlinePlan::StaticDeficiencyTrigger{
            model::WorkflowFilingId{"filing.legacy-static"}, model::ActorId{"test.actor.appellant"},
            "test.record.legacy-static", std::string(64, 'a'), date(2026, 8, 13)};
    result = engine::decideWorkflow(
        malformed, case_definition, initial,
        filing("command.legacy-static", "filing.legacy-static", "test.actor.appellant",
               "test.filing.opening", date(2026, 8, 13), {model::ActorId{"test.actor.appellee"}}));
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::InvalidDefinition);

    malformed = workflow();
    for (std::size_t index = malformed.stages.size(); index <= 256; ++index) {
        malformed.stages.push_back(
            model::WorkflowStageId{"test.stage.extra-" + std::to_string(index)});
    }
    result = engine::decideWorkflow(
        malformed, case_definition, initial,
        filing("command.opening", "filing.opening", "test.actor.appellant", "test.filing.opening",
               date(2026, 8, 13), {model::ActorId{"test.actor.appellee"}}));
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::InvalidDefinition);
}

void WorkflowEngineTest::rejectsMalformedRouteOutcomeCombinations() {
    const auto case_definition = caseDefinition();
    const auto initial = initialState();
    const auto command =
        filing("command.opening", "filing.opening", "test.actor.appellant", "test.filing.opening",
               date(2026, 8, 13), {model::ActorId{"test.actor.appellee"}});

    auto malformed = workflow();
    malformed.filing_routes.front().reject_operation_id =
        model::WorkflowOperationId{"test.op.opening.accept"};
    auto result = engine::decideWorkflow(malformed, case_definition, initial, command);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::InvalidDefinition);

    malformed = workflow();
    malformed.filing_routes.front().reject_operation_id =
        model::WorkflowOperationId{"test.op.missing-rejection"};
    result = engine::decideWorkflow(malformed, case_definition, initial, command);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::InvalidDefinition);

    malformed = workflow();
    malformed.filing_routes.front().deficiency_operation_id.reset();
    QVERIFY(malformed.filing_routes.front().deficiency_deadline.has_value());
    result = engine::decideWorkflow(malformed, case_definition, initial, command);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::InvalidDefinition);

    malformed = workflow();
    malformed.filing_routes.clear();
    result = engine::decideWorkflow(malformed, case_definition, initial, command);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::InvalidDefinition);
}

void WorkflowEngineTest::replayRejectsTamperedTrace() {
    const auto run = happyRun();
    QVERIFY(run.has_value());
    auto journal = run->journal;
    auto* deadline = std::get_if<model::WorkflowDeadlineCalculated>(&journal.front().events.at(1));
    QVERIFY(deadline != nullptr);
    deadline->due_date = date(2026, 9, 22);

    const auto replayed =
        engine::replayWorkflow(workflow(), caseDefinition(), initialState(), journal);
    QVERIFY(!replayed.has_value());
    QCOMPARE(replayed.error().code, engine::WorkflowErrorCode::InvalidEvent);
}

void WorkflowEngineTest::usesStructuredDispositionPlanAndCanonicalDigest() {
    const auto definition = structuredWorkflow();
    const auto case_definition = structuredCaseDefinition();
    auto ready = structuredReadyRun();
    QVERIFY2(ready.has_value(), ready ? "" : ready.error().c_str());

    const auto command = model::IssueWorkflowJudgment{
        header("command.structured-judgment", "test.actor.court", date(2026, 11, 10)),
        model::WorkflowOperationId{structured_judgment_operation}, std::string(64, 'e'),
        model::DispositionPlanId{structured_plan_id}};
    const auto decision =
        engine::decideWorkflow(definition, case_definition, ready->state, command);
    QVERIFY2(decision.has_value(), decision ? "" : decision.error().message.c_str());
    QCOMPARE(decision->size(), std::size_t{1});
    const auto* judgment = std::get_if<model::WorkflowJudgmentIssued>(&decision->front());
    QVERIFY(judgment != nullptr);
    const auto* plan = std::get_if<model::DispositionPlan>(&judgment->disposition);
    QVERIFY(plan != nullptr);
    QCOMPARE(*plan, case_definition.disposition_plans.front());
    QCOMPARE(plan->canonical_sha256, std::string(structured_plan_digest));
    QCOMPARE(judgment->header.preconditions,
             std::ranges::find(definition.operations,
                               model::WorkflowOperationId{structured_judgment_operation},
                               &model::WorkflowOperation::id)
                 ->preconditions);

    const auto before_judgment = *ready;
    const auto executed = execute(definition, case_definition, *ready, command);
    QVERIFY2(executed.has_value(), executed ? "" : executed.error().c_str());
    QVERIFY(ready->state.judgment_disposition.has_value());
    const auto* state_plan =
        std::get_if<model::DispositionPlan>(&*ready->state.judgment_disposition);
    QVERIFY(state_plan != nullptr);
    QCOMPARE(*state_plan, case_definition.disposition_plans.front());

    const auto legacy_text = model::IssueWorkflowJudgment{
        header("command.arbitrary-text", "test.actor.court", date(2026, 11, 10)),
        model::WorkflowOperationId{structured_judgment_operation}, std::string(64, 'e'),
        std::string{"affirmed"}};
    auto rejected =
        engine::decideWorkflow(definition, case_definition, before_judgment.state, legacy_text);
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, engine::WorkflowErrorCode::InvalidCommand);

    auto wrong_plan = command;
    wrong_plan.header.command_id = model::WorkflowCommandId{"command.wrong-plan"};
    wrong_plan.disposition = model::DispositionPlanId{"example.disposition.unwritten"};
    rejected =
        engine::decideWorkflow(definition, case_definition, before_judgment.state, wrong_plan);
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, engine::WorkflowErrorCode::InvalidCommand);

    auto tampered_journal = ready->journal;
    auto& recorded_judgment =
        std::get<model::WorkflowJudgmentIssued>(tampered_journal.back().events.front());
    std::get<model::DispositionPlan>(recorded_judgment.disposition).components.front().action =
        model::DispositionAction::Deny;
    const auto tampered = engine::replayWorkflow(definition, case_definition,
                                                 before_judgment.initial_state, tampered_journal);
    QVERIFY(!tampered.has_value());
    QCOMPARE(tampered.error().code, engine::WorkflowErrorCode::InvalidEvent);
}

void WorkflowEngineTest::enforcesOperationDispositionBindings() {
    auto definition = structuredWorkflow();
    auto case_definition = structuredCaseDefinition();
    auto authored = std::ranges::find(definition.operations,
                                      model::WorkflowOperationId{structured_judgment_operation},
                                      &model::WorkflowOperation::id);
    QVERIFY(authored != definition.operations.end());
    authored->disposition_plan_id = model::DispositionPlanId{structured_plan_id};

    auto counterfactual_operation = *authored;
    counterfactual_operation.id = model::WorkflowOperationId{counterfactual_judgment_operation};
    counterfactual_operation.disposition_plan_id = model::DispositionPlanId{counterfactual_plan_id};
    definition.operations.push_back(counterfactual_operation);

    auto counterfactual_plan = case_definition.disposition_plans.front();
    counterfactual_plan.id = model::DispositionPlanId{counterfactual_plan_id};
    counterfactual_plan.canonical_sha256 = counterfactual_plan_digest;
    counterfactual_plan.components.front().action = model::DispositionAction::Affirm;
    counterfactual_plan.components.front().remand = false;
    case_definition.disposition_plans.push_back(counterfactual_plan);

    auto ready = structuredReadyRun();
    QVERIFY2(ready.has_value(), ready ? "" : ready.error().c_str());
    const auto counterfactual_command = model::IssueWorkflowJudgment{
        header("command.counterfactual-judgment", "test.actor.court", date(2026, 11, 10)),
        model::WorkflowOperationId{counterfactual_judgment_operation}, std::string(64, '6'),
        model::DispositionPlanId{counterfactual_plan_id}};
    const auto decision =
        engine::decideWorkflow(definition, case_definition, ready->state, counterfactual_command);
    QVERIFY2(decision.has_value(), decision ? "" : decision.error().message.c_str());
    const auto* issued = std::get_if<model::WorkflowJudgmentIssued>(&decision->front());
    QVERIFY(issued != nullptr);
    QCOMPARE(std::get<model::DispositionPlan>(issued->disposition), counterfactual_plan);

    auto completed = *ready;
    const auto executed = execute(definition, case_definition, completed, counterfactual_command);
    QVERIFY2(executed.has_value(), executed ? "" : executed.error().c_str());
    const auto valid_replay = engine::replayWorkflow(definition, case_definition,
                                                     completed.initial_state, completed.journal);
    QVERIFY2(valid_replay.has_value(), valid_replay ? "" : valid_replay.error().message.c_str());
    QVERIFY(*valid_replay == completed.state);
    const auto mandate = model::IssueWorkflowMandate{
        header("command.counterfactual-mandate", "test.actor.court", date(2026, 11, 11)),
        model::WorkflowOperationId{"test.op.issue-mandate"}, std::string(64, '7')};
    const auto mandate_decision =
        engine::decideWorkflow(definition, case_definition, completed.state, mandate);
    QVERIFY2(mandate_decision.has_value(),
             mandate_decision ? "" : mandate_decision.error().message.c_str());

    auto unreachable_state_definition = definition;
    operationById(unreachable_state_definition, counterfactual_judgment_operation)
        .disposition_plan_id.reset();
    auto rejected_state = engine::decideWorkflow(unreachable_state_definition, case_definition,
                                                 completed.state, mandate);
    QVERIFY(!rejected_state.has_value());
    QCOMPARE(rejected_state.error().code, engine::WorkflowErrorCode::InvalidState);

    auto wrong_plan = counterfactual_command;
    wrong_plan.header.command_id = model::WorkflowCommandId{"command.bound-wrong-plan"};
    wrong_plan.disposition = model::DispositionPlanId{structured_plan_id};
    auto rejected = engine::decideWorkflow(definition, case_definition, ready->state, wrong_plan);
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, engine::WorkflowErrorCode::InvalidCommand);

    auto unbound_authored = definition;
    operationById(unbound_authored, structured_judgment_operation).disposition_plan_id.reset();
    auto wrong_authored_plan = counterfactual_command;
    wrong_authored_plan.header.command_id =
        model::WorkflowCommandId{"command.unbound-authored-wrong-plan"};
    wrong_authored_plan.operation_id = model::WorkflowOperationId{structured_judgment_operation};
    rejected = engine::decideWorkflow(unbound_authored, case_definition, ready->state,
                                      wrong_authored_plan);
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, engine::WorkflowErrorCode::InvalidCommand);

    auto malformed_opcode = definition;
    auto& mandate_operation = operationById(malformed_opcode, "test.op.issue-mandate");
    mandate_operation.disposition_plan_id = model::DispositionPlanId{structured_plan_id};
    rejected = engine::decideWorkflow(malformed_opcode, case_definition, ready->state,
                                      counterfactual_command);
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, engine::WorkflowErrorCode::InvalidDefinition);

    auto unresolved = definition;
    operationById(unresolved, counterfactual_judgment_operation).disposition_plan_id =
        model::DispositionPlanId{"example.disposition.missing"};
    rejected =
        engine::decideWorkflow(unresolved, case_definition, ready->state, counterfactual_command);
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, engine::WorkflowErrorCode::InvalidCase);

    auto mismatched_authored = definition;
    operationById(mismatched_authored, structured_judgment_operation).disposition_plan_id =
        model::DispositionPlanId{counterfactual_plan_id};
    rejected = engine::decideWorkflow(mismatched_authored, case_definition, ready->state,
                                      counterfactual_command);
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, engine::WorkflowErrorCode::InvalidCase);

    auto legacy_definition = canonicalWorkflow();
    operationById(legacy_definition, "test.op.issue-judgment").disposition_plan_id =
        model::DispositionPlanId{structured_plan_id};
    const auto legacy_command = model::IssueWorkflowJudgment{
        header("command.legacy-bound", "test.actor.court", date(2026, 11, 10)),
        model::WorkflowOperationId{"test.op.issue-judgment"}, std::string(64, '8'),
        std::string{"affirmed"}};
    rejected =
        engine::decideWorkflow(legacy_definition, caseDefinition(), ready->state, legacy_command);
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, engine::WorkflowErrorCode::InvalidCase);

    auto tampered_journal = completed.journal;
    auto& tampered_command =
        std::get<model::IssueWorkflowJudgment>(tampered_journal.back().command);
    tampered_command.operation_id = model::WorkflowOperationId{structured_judgment_operation};
    auto& tampered_event =
        std::get<model::WorkflowJudgmentIssued>(tampered_journal.back().events.front());
    tampered_event.header.operation_id = model::WorkflowOperationId{structured_judgment_operation};
    const auto replayed = engine::replayWorkflow(definition, case_definition,
                                                 completed.initial_state, tampered_journal);
    QVERIFY(!replayed.has_value());
    QCOMPARE(replayed.error().code, engine::WorkflowErrorCode::InvalidEvent);
}

void WorkflowEngineTest::enforcesBoundedAllOfPreconditionsAndReplaySnapshots() {
    const auto case_definition = structuredCaseDefinition();
    const auto definition = structuredWorkflow();
    auto ready = structuredReadyRun();
    QVERIFY2(ready.has_value(), ready ? "" : ready.error().c_str());
    const auto command = model::IssueWorkflowJudgment{
        header("command.guarded-judgment", "test.actor.court", date(2026, 11, 10)),
        model::WorkflowOperationId{structured_judgment_operation}, std::string(64, 'f'),
        model::DispositionPlanId{structured_plan_id}};

    auto unmet = definition;
    auto unmet_operation = std::ranges::find(
        unmet.operations, model::WorkflowOperationId{structured_judgment_operation},
        &model::WorkflowOperation::id);
    QVERIFY(unmet_operation != unmet.operations.end());
    auto elapsed = std::ranges::find_if(unmet_operation->preconditions, [](const auto& predicate) {
        const auto* deadline = std::get_if<model::WorkflowDeadlinePrecondition>(&predicate);
        return deadline != nullptr &&
               deadline->condition == model::WorkflowDeadlineCondition::Elapsed;
    });
    QVERIFY(elapsed != unmet_operation->preconditions.end());
    std::get<model::WorkflowDeadlinePrecondition>(*elapsed).condition =
        model::WorkflowDeadlineCondition::NotElapsed;
    auto result = engine::decideWorkflow(unmet, case_definition, ready->state, command);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::UnmetPrecondition);

    auto unresolved = definition;
    auto unresolved_operation = std::ranges::find(
        unresolved.operations, model::WorkflowOperationId{structured_judgment_operation},
        &model::WorkflowOperation::id);
    QVERIFY(unresolved_operation != unresolved.operations.end());
    unresolved_operation->preconditions.front() =
        model::WorkflowFilingPrecondition{model::FilingTypeId{"test.filing.unknown"}, false};
    result = engine::decideWorkflow(unresolved, case_definition, ready->state, command);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::InvalidDefinition);

    const auto opening_command =
        filing("command.guarded-opening", "filing.guarded-opening", "test.actor.appellant",
               "test.filing.opening", date(2026, 8, 13), {model::ActorId{"test.actor.appellee"}});
    auto missing_order = canonicalWorkflow();
    auto guarded_opening = std::ranges::find(missing_order.operations,
                                             model::WorkflowOperationId{"test.op.opening.accept"},
                                             &model::WorkflowOperation::id);
    QVERIFY(guarded_opening != missing_order.operations.end());
    guarded_opening->preconditions = {
        model::WorkflowOrderPrecondition{model::WorkflowOrderId{"test.order.not-entered"},
                                         model::WorkflowOrderDisposition::Granted}};
    result =
        engine::decideWorkflow(missing_order, caseDefinition(), initialState(), opening_command);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::UnmetPrecondition);

    auto missing_deadline = canonicalWorkflow();
    auto named_missing_deadline = operation("test.op.named-missing-deadline", appellant_stage,
                                            model::WorkflowOpcode::CalculateDeadline, std::nullopt,
                                            1, model::DeadlineCounting::CalendarDays, {court_role});
    named_missing_deadline.authority.primary.provenance = provenance();
    named_missing_deadline.produced_deadline_id =
        model::WorkflowDeadlineId{"test.deadline.not-created"};
    missing_deadline.operations.push_back(named_missing_deadline);
    guarded_opening = std::ranges::find(missing_deadline.operations,
                                        model::WorkflowOperationId{"test.op.opening.accept"},
                                        &model::WorkflowOperation::id);
    QVERIFY(guarded_opening != missing_deadline.operations.end());
    guarded_opening->preconditions = {
        model::WorkflowDeadlinePrecondition{model::WorkflowDeadlineId{"test.deadline.not-created"},
                                            model::WorkflowDeadlineCondition::Open}};
    result =
        engine::decideWorkflow(missing_deadline, caseDefinition(), initialState(), opening_command);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::UnmetPrecondition);

    auto command_start_definition = canonicalWorkflow();
    auto second_event_operation = std::ranges::find(
        command_start_definition.operations,
        model::WorkflowOperationId{"test.op.response.deadline"}, &model::WorkflowOperation::id);
    QVERIFY(second_event_operation != command_start_definition.operations.end());
    second_event_operation->preconditions = {
        model::WorkflowFilingPrecondition{model::FilingTypeId{"test.filing.opening"}, true}};
    result = engine::decideWorkflow(command_start_definition, caseDefinition(), initialState(),
                                    opening_command);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::UnmetPrecondition);

    auto bounded = definition;
    auto bounded_operation = std::ranges::find(
        bounded.operations, model::WorkflowOperationId{structured_judgment_operation},
        &model::WorkflowOperation::id);
    QVERIFY(bounded_operation != bounded.operations.end());
    for (std::size_t index = bounded_operation->preconditions.size(); index <= 32; ++index) {
        bounded_operation->preconditions.push_back(model::WorkflowOrderPrecondition{
            model::WorkflowOrderId{"test.order.guard-" + std::to_string(index)},
            model::WorkflowOrderDisposition::Granted});
    }
    result = engine::decideWorkflow(bounded, case_definition, ready->state, command);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::InvalidDefinition);

    auto completed = *ready;
    const auto executed = execute(definition, case_definition, completed, command);
    QVERIFY2(executed.has_value(), executed ? "" : executed.error().c_str());
    auto changed_definition = definition;
    auto changed_operation = std::ranges::find(
        changed_definition.operations, model::WorkflowOperationId{structured_judgment_operation},
        &model::WorkflowOperation::id);
    QVERIFY(changed_operation != changed_definition.operations.end());
    changed_operation->preconditions.front() =
        model::WorkflowFilingPrecondition{model::FilingTypeId{"test.filing.response"}, true};
    const auto changed_replay = engine::replayWorkflow(changed_definition, case_definition,
                                                       completed.initial_state, completed.journal);
    QVERIFY(!changed_replay.has_value());
    QCOMPARE(changed_replay.error().code, engine::WorkflowErrorCode::InvalidEvent);

    auto not_elapsed_definition = canonicalWorkflow();
    auto extension_accept = std::ranges::find(
        not_elapsed_definition.operations, model::WorkflowOperationId{"test.op.extension.accept"},
        &model::WorkflowOperation::id);
    QVERIFY(extension_accept != not_elapsed_definition.operations.end());
    extension_accept->preconditions = {
        model::WorkflowDeadlinePrecondition{model::WorkflowDeadlineId{response_deadline},
                                            model::WorkflowDeadlineCondition::Open},
        model::WorkflowDeadlinePrecondition{model::WorkflowDeadlineId{response_deadline},
                                            model::WorkflowDeadlineCondition::NotElapsed},
    };
    auto not_elapsed_run = emptyRun();
    QVERIFY(execute(not_elapsed_definition, caseDefinition(), not_elapsed_run,
                    filing("command.opening", "filing.opening", "test.actor.appellant",
                           "test.filing.opening", date(2026, 8, 13),
                           {model::ActorId{"test.actor.appellee"}}))
                .has_value());
    const auto timely =
        engine::decideWorkflow(not_elapsed_definition, caseDefinition(), not_elapsed_run.state,
                               filing("command.extension", "filing.extension",
                                      "test.actor.appellee", "test.filing.extension",
                                      date(2026, 8, 20), {model::ActorId{"test.actor.appellant"}}));
    QVERIFY(timely.has_value());
    const auto late =
        engine::decideWorkflow(not_elapsed_definition, caseDefinition(), not_elapsed_run.state,
                               filing("command.late-extension", "filing.late-extension",
                                      "test.actor.appellee", "test.filing.extension",
                                      date(2026, 9, 22), {model::ActorId{"test.actor.appellant"}}));
    QVERIFY(!late.has_value());
    QCOMPARE(late.error().code, engine::WorkflowErrorCode::UnmetPrecondition);

    auto elapsed_definition = canonicalWorkflow();
    extension_accept = std::ranges::find(elapsed_definition.operations,
                                         model::WorkflowOperationId{"test.op.extension.accept"},
                                         &model::WorkflowOperation::id);
    QVERIFY(extension_accept != elapsed_definition.operations.end());
    extension_accept->preconditions = {model::WorkflowDeadlinePrecondition{
        model::WorkflowDeadlineId{response_deadline}, model::WorkflowDeadlineCondition::Elapsed}};
    auto elapsed_run = emptyRun();
    QVERIFY(execute(elapsed_definition, caseDefinition(), elapsed_run,
                    filing("command.elapsed-opening", "filing.elapsed-opening",
                           "test.actor.appellant", "test.filing.opening", date(2026, 8, 13),
                           {model::ActorId{"test.actor.appellee"}}))
                .has_value());
    const auto response =
        std::ranges::find(elapsed_run.state.deadlines, model::WorkflowDeadlineId{response_deadline},
                          &model::WorkflowDeadlineRecord::deadline_id);
    QVERIFY(response != elapsed_run.state.deadlines.end());
    const auto due_date = response->due_date;
    const auto next_day = model::LegalDate{
        std::chrono::year_month_day{std::chrono::sys_days{due_date.value} + std::chrono::days{1}}};
    const auto on_due_date = engine::decideWorkflow(
        elapsed_definition, caseDefinition(), elapsed_run.state,
        filing("command.extension-on-due-date", "filing.extension-on-due-date",
               "test.actor.appellee", "test.filing.extension", due_date,
               {model::ActorId{"test.actor.appellant"}}));
    QVERIFY(!on_due_date.has_value());
    QCOMPARE(on_due_date.error().code, engine::WorkflowErrorCode::UnmetPrecondition);
    const auto after_due_date = engine::decideWorkflow(
        elapsed_definition, caseDefinition(), elapsed_run.state,
        filing("command.extension-after-due-date", "filing.extension-after-due-date",
               "test.actor.appellee", "test.filing.extension", next_day,
               {model::ActorId{"test.actor.appellant"}}));
    QVERIFY(after_due_date.has_value());

    auto legacy_with_guard = workflow();
    auto legacy_guarded_operation = std::ranges::find(
        legacy_with_guard.operations, model::WorkflowOperationId{"test.op.opening.accept"},
        &model::WorkflowOperation::id);
    QVERIFY(legacy_guarded_operation != legacy_with_guard.operations.end());
    legacy_guarded_operation->preconditions = {
        model::WorkflowFilingPrecondition{model::FilingTypeId{"test.filing.opening"}, false},
    };
    const auto legacy_guard_result = engine::decideWorkflow(
        legacy_with_guard, caseDefinition(), initialState(),
        filing("command.legacy-guard", "filing.legacy-guard", "test.actor.appellant",
               "test.filing.opening", date(2026, 8, 13), {model::ActorId{"test.actor.appellee"}}));
    QVERIFY(!legacy_guard_result.has_value());
    QCOMPARE(legacy_guard_result.error().code, engine::WorkflowErrorCode::InvalidDefinition);
}

void WorkflowEngineTest::enforcesExactFilingAndOrderInstancePreconditions() {
    auto definition = canonicalWorkflow();
    auto case_definition = caseDefinition();
    case_definition.actors.push_back(
        {model::ActorId{"test.actor.other-appellant"}, model::ActorRoleId{appellant_role}});
    auto& opening_route = routeByType(definition, "test.filing.opening");
    opening_route.accepted_deadline.reset();
    opening_route.advance_operation_id.reset();
    routeByType(definition, "test.filing.response").satisfies_deadline_id.reset();

    auto order_source = operationById(definition, "test.op.extension.order");
    order_source.id = model::WorkflowOperationId{"test.op.instance-order"};
    order_source.stage_id = model::WorkflowStageId{appellant_stage};
    order_source.deadline_days.reset();
    order_source.deadline_counting.reset();
    order_source.preconditions.clear();
    definition.operations.push_back(order_source);
    order_source.id = model::WorkflowOperationId{"test.op.other-instance-order"};
    definition.operations.push_back(order_source);
    auto& guarded = operationById(definition, "test.op.set-sealed");
    guarded.stage_id = model::WorkflowStageId{appellant_stage};

    auto run = emptyRun();
    const auto first_filing = execute(
        definition, case_definition, run,
        filing("command.instance-first", "filing.instance-first", "test.actor.appellant",
               "test.filing.opening", date(2026, 8, 13), {model::ActorId{"test.actor.appellee"}}));
    QVERIFY2(first_filing.has_value(), first_filing ? "" : first_filing.error().c_str());
    QVERIFY(execute(definition, case_definition, run,
                    filing("command.instance-second", "filing.instance-second",
                           "test.actor.appellant", "test.filing.opening", date(2026, 8, 13),
                           {model::ActorId{"test.actor.appellee"}}))
                .has_value());
    QVERIFY(
        execute(definition, case_definition, run,
                model::EnterWorkflowOrder{
                    header("command.instance-order", "test.actor.court", date(2026, 8, 14)),
                    model::WorkflowOperationId{"test.op.instance-order"},
                    model::WorkflowOrderId{"test.order.instance"},
                    model::WorkflowOrderDisposition::Granted, std::string(64, 'b'), std::nullopt})
            .has_value());

    const auto filing_present = model::WorkflowFilingInstancePrecondition{
        model::FilingTypeId{"test.filing.opening"},
        true,
        model::ActorId{"test.actor.appellant"},
        model::WorkflowFilingId{"filing.instance-first"},
        model::WorkflowOperationId{"test.op.opening.accept"},
        "test.record.entry-first",
        std::string(64, 'a')};
    const auto filing_absent = model::WorkflowFilingInstancePrecondition{
        model::FilingTypeId{"test.filing.opening"},
        false,
        model::ActorId{"test.actor.appellant"},
        model::WorkflowFilingId{"filing.instance-absent"},
        model::WorkflowOperationId{"test.op.opening.accept"},
        "test.record.entry-absent",
        std::string(64, 'c')};
    const auto filing_second = model::WorkflowFilingInstancePrecondition{
        model::FilingTypeId{"test.filing.opening"},
        true,
        model::ActorId{"test.actor.appellant"},
        model::WorkflowFilingId{"filing.instance-second"},
        model::WorkflowOperationId{"test.op.opening.accept"},
        "test.record.entry-second",
        std::string(64, 'a')};
    const auto order_present = model::WorkflowOrderInstancePrecondition{
        model::WorkflowOrderId{"test.order.instance"}, model::WorkflowOrderDisposition::Granted,
        model::WorkflowOperationId{"test.op.instance-order"}, "test.record.entry-order",
        std::string(64, 'b')};
    guarded.preconditions = {filing_present, filing_second, filing_absent, order_present};
    const auto seal = model::SetWorkflowSealed{
        header("command.instance-guard", "test.actor.court", date(2026, 8, 15)),
        model::WorkflowOperationId{"test.op.set-sealed"}, true};
    auto result = engine::decideWorkflow(definition, case_definition, run.state, seal);
    QVERIFY2(result.has_value(), result ? "" : result.error().message.c_str());

    auto absent_only = definition;
    operationById(absent_only, "test.op.set-sealed").preconditions = {filing_absent};
    result = engine::decideWorkflow(absent_only, case_definition, run.state, seal);
    QVERIFY2(result.has_value(), result ? "" : result.error().message.c_str());

    auto deficient_run = run;
    QVERIFY(
        execute(definition, case_definition, deficient_run,
                filing("command.instance-deficient", "filing.instance-deficient",
                       "test.actor.appellant", "test.filing.opening", date(2026, 8, 15), {}, false))
            .has_value());
    const auto deficient_instance = model::WorkflowFilingInstancePrecondition{
        model::FilingTypeId{"test.filing.opening"},
        false,
        model::ActorId{"test.actor.appellant"},
        model::WorkflowFilingId{"filing.instance-deficient"},
        model::WorkflowOperationId{"test.op.opening.accept"},
        "test.record.entry-deficient",
        std::string(64, 'a')};
    for (const auto present : {false, true}) {
        auto deficient_definition = definition;
        auto selector = deficient_instance;
        selector.present = present;
        operationById(deficient_definition, "test.op.set-sealed").preconditions = {selector};
        result = engine::decideWorkflow(deficient_definition, case_definition, deficient_run.state,
                                        seal);
        QVERIFY(!result.has_value());
        QCOMPARE(result.error().code, engine::WorkflowErrorCode::UnmetPrecondition);
    }

    auto substituted_deficiency_state = deficient_run.state;
    QVERIFY(!substituted_deficiency_state.deficiencies.empty());
    substituted_deficiency_state.deficiencies.back().filing_type =
        model::FilingTypeId{"test.filing.reply"};
    substituted_deficiency_state.deficiencies.back().actor_id =
        model::ActorId{"test.actor.other-appellant"};
    for (const auto present : {false, true}) {
        auto deficient_definition = definition;
        auto selector = deficient_instance;
        selector.present = present;
        operationById(deficient_definition, "test.op.set-sealed").preconditions = {selector};
        result = engine::decideWorkflow(deficient_definition, case_definition,
                                        substituted_deficiency_state, seal);
        QVERIFY(!result.has_value());
        QCOMPARE(result.error().code, engine::WorkflowErrorCode::UnmetPrecondition);
    }

    auto alternate_provenance = definition;
    std::get<model::WorkflowFilingInstancePrecondition>(
        operationById(alternate_provenance, "test.op.set-sealed").preconditions.front())
        .record_entry_id = "test.record.same-bytes-other-entry";
    result = engine::decideWorkflow(alternate_provenance, case_definition, run.state, seal);
    QVERIFY2(result.has_value(), result ? "" : result.error().message.c_str());

    auto substituted = definition;
    auto* selector = std::get_if<model::WorkflowFilingInstancePrecondition>(
        &operationById(substituted, "test.op.set-sealed").preconditions.front());
    QVERIFY(selector != nullptr);
    selector->actor_id = model::ActorId{"test.actor.other-appellant"};
    selector->present = false;
    result = engine::decideWorkflow(substituted, case_definition, run.state, seal);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::UnmetPrecondition);

    auto wrong_sha = definition;
    std::get<model::WorkflowFilingInstancePrecondition>(
        operationById(wrong_sha, "test.op.set-sealed").preconditions.front())
        .document_sha256 = std::string(64, 'd');
    result = engine::decideWorkflow(wrong_sha, case_definition, run.state, seal);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::UnmetPrecondition);

    auto wrong_accept = definition;
    wrong_accept.filing_routes.back().filing_type = model::FilingTypeId{"test.filing.opening"};
    auto& wrong_accept_guard = std::get<model::WorkflowFilingInstancePrecondition>(
        operationById(wrong_accept, "test.op.set-sealed").preconditions.front());
    wrong_accept_guard.accept_operation_id = model::WorkflowOperationId{"test.op.reply.accept"};
    result = engine::decideWorkflow(wrong_accept, case_definition, run.state, seal);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::UnmetPrecondition);

    auto wrong_order = definition;
    auto& wrong_order_guard = std::get<model::WorkflowOrderInstancePrecondition>(
        operationById(wrong_order, "test.op.set-sealed").preconditions.back());
    wrong_order_guard.operation_id = model::WorkflowOperationId{"test.op.other-instance-order"};
    result = engine::decideWorkflow(wrong_order, case_definition, run.state, seal);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::UnmetPrecondition);
    wrong_order_guard.operation_id = model::WorkflowOperationId{"test.op.instance-order"};
    wrong_order_guard.document_sha256 = std::string(64, 'e');
    result = engine::decideWorkflow(wrong_order, case_definition, run.state, seal);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::UnmetPrecondition);

    auto allowed_cross_kind = definition;
    operationById(allowed_cross_kind, "test.op.set-sealed").preconditions = {
        model::WorkflowFilingPrecondition{model::FilingTypeId{"test.filing.opening"}, true},
        filing_absent};
    result = engine::decideWorkflow(allowed_cross_kind, case_definition, run.state, seal);
    QVERIFY(result.has_value());

    for (const auto reverse : {false, true}) {
        auto conflict = definition;
        auto impossible = std::vector<model::WorkflowPrecondition>{
            model::WorkflowFilingPrecondition{model::FilingTypeId{"test.filing.opening"}, false},
            filing_present};
        if (reverse) {
            std::ranges::reverse(impossible);
        }
        operationById(conflict, "test.op.set-sealed").preconditions = std::move(impossible);
        result = engine::decideWorkflow(conflict, case_definition, run.state, seal);
        QVERIFY(!result.has_value());
        QCOMPARE(result.error().code, engine::WorkflowErrorCode::InvalidDefinition);
    }

    auto duplicate_instance = definition;
    auto duplicate = filing_present;
    duplicate.document_sha256 = std::string(64, 'f');
    operationById(duplicate_instance, "test.op.set-sealed").preconditions = {filing_present,
                                                                             duplicate};
    result = engine::decideWorkflow(duplicate_instance, case_definition, run.state, seal);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::InvalidDefinition);

    auto legacy_state = run.state;
    legacy_state.accepted_filings.front().accept_operation_id.reset();
    result = engine::decideWorkflow(definition, case_definition, legacy_state, seal);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::UnmetPrecondition);

    auto overlap = run.state;
    overlap.deficiencies.push_back({model::WorkflowDeficiencyId{"test.deficiency.overlap"},
                                    overlap.accepted_filings.front().filing_id,
                                    model::FilingTypeId{"test.filing.opening"},
                                    model::ActorId{"test.actor.appellant"},
                                    {model::WorkflowRequirementId{"test.requirement.signature"}},
                                    std::nullopt,
                                    false});
    result = engine::decideWorkflow(definition, case_definition, overlap, seal);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::InvalidState);

    auto duplicate_deficiencies = run.state;
    duplicate_deficiencies.deficiencies.push_back(
        {model::WorkflowDeficiencyId{"test.deficiency.first"},
         model::WorkflowFilingId{"test.filing.deficient"},
         model::FilingTypeId{"test.filing.opening"},
         model::ActorId{"test.actor.appellant"},
         {model::WorkflowRequirementId{"test.requirement.signature"}},
         std::nullopt,
         false});
    duplicate_deficiencies.deficiencies.push_back(
        {model::WorkflowDeficiencyId{"test.deficiency.second"},
         model::WorkflowFilingId{"test.filing.deficient"},
         model::FilingTypeId{"test.filing.opening"},
         model::ActorId{"test.actor.appellant"},
         {model::WorkflowRequirementId{"test.requirement.service"}},
         std::nullopt,
         false});
    result = engine::decideWorkflow(definition, case_definition, duplicate_deficiencies, seal);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::InvalidState);

    auto impossible_actor = run.state;
    impossible_actor.accepted_filings.front().actor_id = model::ActorId{"test.actor.appellee"};
    result = engine::decideWorkflow(definition, case_definition, impossible_actor, seal);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::InvalidState);
}

void WorkflowEngineTest::enforcesStaticDeficiencyDeadlineIdentityAndReplay() {
    auto definition = canonicalWorkflow();
    auto case_definition = caseDefinition();
    case_definition.actors.push_back(
        {model::ActorId{"test.actor.other-appellant"}, model::ActorRoleId{appellant_role}});
    auto& plan = *routeByType(definition, "test.filing.opening").deficiency_deadline;
    plan.static_trigger = model::WorkflowDeadlinePlan::StaticDeficiencyTrigger{
        model::WorkflowFilingId{"filing.static-trigger"}, model::ActorId{"test.actor.appellant"},
        "test.record.entry-static", std::string(64, 'a'), date(2026, 8, 14)};
    routeByType(definition, "test.filing.opening").satisfies_deadline_id = plan.deadline_id;

    const auto trigger =
        filing("command.static-trigger", "filing.static-trigger", "test.actor.appellant",
               "test.filing.opening", date(2026, 8, 14), {}, false);
    const auto conforming_without_deadline = filing(
        "command.static-conforming", "filing.static-conforming", "test.actor.appellant",
        "test.filing.opening", date(2026, 8, 14), {model::ActorId{"test.actor.appellee"}}, true);
    const auto ineligible = engine::decideWorkflow(definition, case_definition, initialState(),
                                                   conforming_without_deadline);
    QVERIFY(ineligible.has_value());
    QCOMPARE(ineligible->size(), std::size_t{1});
    QCOMPARE(std::get<model::WorkflowFilingRejected>(ineligible->front()).reason,
             model::WorkflowFilingRejectionReason::IneligibleFiling);
    const auto decision =
        engine::decideWorkflow(definition, case_definition, initialState(), trigger);
    QVERIFY2(decision.has_value(), decision ? "" : decision.error().message.c_str());
    QCOMPARE(decision->size(), std::size_t{2});
    const auto* deficiency = std::get_if<model::WorkflowDeficiencyIssued>(&decision->front());
    const auto* deadline = std::get_if<model::WorkflowDeadlineCalculated>(&decision->back());
    QVERIFY(deficiency != nullptr);
    QVERIFY(deadline != nullptr);
    QCOMPARE(*deficiency->cure_deadline_id, plan.deadline_id);
    QCOMPARE(deadline->deadline_id, plan.deadline_id);

    const auto expect_trigger_mismatch = [&](model::SubmitWorkflowFiling changed) {
        const auto rejected =
            engine::decideWorkflow(definition, case_definition, initialState(), changed);
        QVERIFY(!rejected.has_value());
        QCOMPARE(rejected.error().code, engine::WorkflowErrorCode::InvalidCommand);
    };
    auto changed = trigger;
    changed.filing_id = model::WorkflowFilingId{"filing.static-other"};
    expect_trigger_mismatch(changed);
    changed = trigger;
    changed.header.actor_id = model::ActorId{"test.actor.other-appellant"};
    expect_trigger_mismatch(changed);
    changed = trigger;
    changed.document_sha256 = std::string(64, 'b');
    expect_trigger_mismatch(changed);
    changed = trigger;
    changed.header.occurred_at = at(date(2026, 8, 15));
    expect_trigger_mismatch(changed);

    auto independently_gated = definition;
    routeByType(independently_gated, "test.filing.opening").satisfies_deadline_id =
        model::WorkflowDeadlineId{response_deadline};
    changed = trigger;
    changed.filing_id = model::WorkflowFilingId{"filing.static-gated-mismatch"};
    const auto gated =
        engine::decideWorkflow(independently_gated, case_definition, initialState(), changed);
    QVERIFY(gated.has_value());
    QCOMPARE(gated->size(), std::size_t{1});
    QCOMPARE(std::get<model::WorkflowFilingRejected>(gated->front()).reason,
             model::WorkflowFilingRejectionReason::IneligibleFiling);

    std::vector<model::WorkflowJournalEntry> journal{{trigger, *decision}};
    const auto pending =
        engine::replayWorkflow(definition, case_definition, initialState(), journal);
    QVERIFY2(pending.has_value(), pending ? "" : pending.error().message.c_str());
    QCOMPARE(pending->deadlines.size(), std::size_t{1});
    QCOMPARE(pending->deadlines.front().status, model::WorkflowDeadlineStatus::Open);

    auto guarded_definition = definition;
    auto& guarded = operationById(guarded_definition, "test.op.set-sealed");
    guarded.stage_id = model::WorkflowStageId{appellant_stage};
    guarded.preconditions = {model::WorkflowDeadlinePrecondition{
        plan.deadline_id, model::WorkflowDeadlineCondition::Open}};
    auto guarded_result = engine::decideWorkflow(
        guarded_definition, case_definition, *pending,
        model::SetWorkflowSealed{
            header("command.static-guard", "test.actor.court", date(2026, 8, 15)),
            model::WorkflowOperationId{"test.op.set-sealed"}, true});
    QVERIFY(guarded_result.has_value());

    const auto no_cure = filing("command.static-no-cure", "filing.static-no-cure",
                                "test.actor.appellant", "test.filing.opening", date(2026, 8, 16),
                                {model::ActorId{"test.actor.appellee"}}, true);
    const auto no_cure_events =
        engine::decideWorkflow(definition, case_definition, *pending, no_cure);
    QVERIFY(no_cure_events.has_value());
    QCOMPARE(no_cure_events->size(), std::size_t{1});
    QCOMPARE(std::get<model::WorkflowFilingRejected>(no_cure_events->front()).reason,
             model::WorkflowFilingRejectionReason::IneligibleFiling);
    auto no_cure_journal = journal;
    no_cure_journal.push_back({no_cure, *no_cure_events});
    const auto no_cure_replayed =
        engine::replayWorkflow(definition, case_definition, initialState(), no_cure_journal);
    QVERIFY2(no_cure_replayed.has_value(),
             no_cure_replayed ? "" : no_cure_replayed.error().message.c_str());
    QCOMPARE(no_cure_replayed->deadlines.front().status, model::WorkflowDeadlineStatus::Open);
    QVERIFY(!no_cure_replayed->deficiencies.front().cured);

    const auto& cure_definition = definition;
    const auto cure =
        filing("command.static-cure", "filing.static-cure", "test.actor.appellant",
               "test.filing.opening", date(2026, 8, 16), {model::ActorId{"test.actor.appellee"}},
               true, deficiency->deficiency_id);
    const auto cured_events =
        engine::decideWorkflow(cure_definition, case_definition, *pending, cure);
    QVERIFY2(cured_events.has_value(), cured_events ? "" : cured_events.error().message.c_str());
    journal.push_back({cure, *cured_events});
    const auto cured =
        engine::replayWorkflow(cure_definition, case_definition, initialState(), journal);
    QVERIFY2(cured.has_value(), cured ? "" : cured.error().message.c_str());
    QCOMPARE(cured->deadlines.front().status, model::WorkflowDeadlineStatus::Satisfied);
    QVERIFY(cured->deficiencies.front().cured);

    auto late = cure;
    late.header.command_id = model::WorkflowCommandId{"command.static-late-cure"};
    late.filing_id = model::WorkflowFilingId{"filing.static-late-cure"};
    late.header.occurred_at = at(date(2026, 8, 19));
    const auto late_result =
        engine::decideWorkflow(cure_definition, case_definition, *pending, late);
    QVERIFY(late_result.has_value());
    QCOMPARE(late_result->size(), std::size_t{1});
    QCOMPARE(std::get<model::WorkflowFilingRejected>(late_result->front()).reason,
             model::WorkflowFilingRejectionReason::DeadlineExpired);

    auto tampered_events = *decision;
    std::get<model::WorkflowDeficiencyIssued>(tampered_events.front()).actor_id =
        model::ActorId{"test.actor.other-appellant"};
    auto replayed = engine::replayWorkflow(
        definition, case_definition, initialState(),
        std::vector<model::WorkflowJournalEntry>{{trigger, tampered_events}});
    QVERIFY(!replayed.has_value());
    QCOMPARE(replayed.error().code, engine::WorkflowErrorCode::InvalidEvent);

    tampered_events = *decision;
    auto& tampered_header =
        std::get<model::WorkflowDeficiencyIssued>(tampered_events.front()).header;
    tampered_header.occurred_at = at(date(2026, 8, 15));
    replayed = engine::replayWorkflow(
        definition, case_definition, initialState(),
        std::vector<model::WorkflowJournalEntry>{{trigger, tampered_events}});
    QVERIFY(!replayed.has_value());

    auto standalone = std::get<model::WorkflowDeadlineCalculated>(decision->back());
    standalone.header.sequence = 1;
    standalone.header.command_event_index = 0;
    standalone.header.command_event_count = 1;
    replayed = engine::replayWorkflow(
        definition, case_definition, initialState(),
        std::vector<model::WorkflowJournalEntry>{{trigger, {model::WorkflowEvent{standalone}}}});
    QVERIFY(!replayed.has_value());
    QCOMPARE(replayed.error().code, engine::WorkflowErrorCode::InvalidEvent);

    const auto probe = model::SetWorkflowSealed{
        header("command.static-state-probe", "test.actor.court", date(2026, 8, 15)),
        model::WorkflowOperationId{"test.op.set-sealed"}, true};
    auto forged = *pending;
    forged.deficiencies.clear();
    auto state_result = engine::decideWorkflow(definition, case_definition, forged, probe);
    QVERIFY(!state_result.has_value());
    QCOMPARE(state_result.error().code, engine::WorkflowErrorCode::InvalidState);
    forged = *pending;
    forged.deadlines.front().due_date = date(2026, 8, 19);
    state_result = engine::decideWorkflow(definition, case_definition, forged, probe);
    QVERIFY(!state_result.has_value());
    QCOMPARE(state_result.error().code, engine::WorkflowErrorCode::InvalidState);
    forged = *pending;
    forged.accepted_filings.push_back({model::WorkflowFilingId{"filing.static-trigger"},
                                       model::FilingTypeId{"test.filing.opening"},
                                       model::ActorId{"test.actor.appellant"},
                                       std::string(64, 'a'),
                                       at(date(2026, 8, 14)),
                                       {model::ActorId{"test.actor.appellee"}},
                                       model::WorkflowOperationId{"test.op.opening.accept"}});
    state_result = engine::decideWorkflow(definition, case_definition, forged, probe);
    QVERIFY(!state_result.has_value());
    QCOMPARE(state_result.error().code, engine::WorkflowErrorCode::InvalidState);
    forged = *pending;
    forged.deficiencies.push_back({model::WorkflowDeficiencyId{"test.deficiency.borrowed-static"},
                                   model::WorkflowFilingId{"test.filing.borrowed-static"},
                                   model::FilingTypeId{"test.filing.opening"},
                                   model::ActorId{"test.actor.appellant"},
                                   {model::WorkflowRequirementId{"test.requirement.signature"}},
                                   plan.deadline_id,
                                   false});
    state_result = engine::decideWorkflow(definition, case_definition, forged, probe);
    QVERIFY(!state_result.has_value());
    QCOMPARE(state_result.error().code, engine::WorkflowErrorCode::InvalidState);

    auto namespace_conflict = definition;
    auto& static_plan = *routeByType(namespace_conflict, "test.filing.opening").deficiency_deadline;
    static_plan.deadline_id = model::WorkflowDeadlineId{"test.deadline.prefix.child"};
    routeByType(namespace_conflict, "test.filing.opening").satisfies_deadline_id =
        static_plan.deadline_id;
    auto& dynamic_plan = *routeByType(namespace_conflict, "test.filing.reply").deficiency_deadline;
    dynamic_plan.deadline_id = model::WorkflowDeadlineId{"test.deadline.prefix"};
    auto namespace_result =
        engine::decideWorkflow(namespace_conflict, case_definition, initialState(), trigger);
    QVERIFY(!namespace_result.has_value());
    QCOMPARE(namespace_result.error().code, engine::WorkflowErrorCode::InvalidDefinition);
    std::ranges::reverse(namespace_conflict.filing_routes);
    namespace_result =
        engine::decideWorkflow(namespace_conflict, case_definition, initialState(), trigger);
    QVERIFY(!namespace_result.has_value());
    QCOMPARE(namespace_result.error().code, engine::WorkflowErrorCode::InvalidDefinition);

    auto boundary_noncollision = definition;
    auto& boundary_exact =
        *routeByType(boundary_noncollision, "test.filing.opening").deficiency_deadline;
    boundary_exact.deadline_id = model::WorkflowDeadlineId{"test.deadline.segment"};
    routeByType(boundary_noncollision, "test.filing.opening").satisfies_deadline_id =
        boundary_exact.deadline_id;
    auto& adjacent_prefix =
        *routeByType(boundary_noncollision, "test.filing.reply").deficiency_deadline;
    QVERIFY(!adjacent_prefix.static_trigger.has_value());
    adjacent_prefix.deadline_id = model::WorkflowDeadlineId{"test.deadline.segmentx"};
    auto boundary_result =
        engine::decideWorkflow(boundary_noncollision, case_definition, initialState(), trigger);
    QVERIFY2(boundary_result.has_value(),
             boundary_result ? "" : boundary_result.error().message.c_str());
    std::ranges::reverse(boundary_noncollision.filing_routes);
    boundary_result =
        engine::decideWorkflow(boundary_noncollision, case_definition, initialState(), trigger);
    QVERIFY2(boundary_result.has_value(),
             boundary_result ? "" : boundary_result.error().message.c_str());

    auto malformed_accepted = definition;
    auto& accepted_plan = *routeByType(malformed_accepted, "test.filing.opening").accepted_deadline;
    accepted_plan.static_trigger = *plan.static_trigger;
    const auto malformed_accepted_result =
        engine::decideWorkflow(malformed_accepted, case_definition, initialState(), trigger);
    QVERIFY(!malformed_accepted_result.has_value());
    QCOMPARE(malformed_accepted_result.error().code, engine::WorkflowErrorCode::InvalidDefinition);

    auto nested_exacts = definition;
    routeByType(nested_exacts, "test.filing.opening").deficiency_deadline->deadline_id =
        model::WorkflowDeadlineId{"test.deadline.exact"};
    routeByType(nested_exacts, "test.filing.opening").satisfies_deadline_id =
        model::WorkflowDeadlineId{"test.deadline.exact"};
    auto& reply_plan = *routeByType(nested_exacts, "test.filing.reply").deficiency_deadline;
    reply_plan.deadline_id = model::WorkflowDeadlineId{"test.deadline.exact.child"};
    reply_plan.static_trigger = model::WorkflowDeadlinePlan::StaticDeficiencyTrigger{
        model::WorkflowFilingId{"filing.reply-static"}, model::ActorId{"test.actor.appellant"},
        "test.record.reply-static", std::string(64, 'e'), date(2026, 9, 1)};
    auto nested_trigger = trigger;
    nested_trigger.filing_id = model::WorkflowFilingId{"filing.static-trigger"};
    const auto nested_result =
        engine::decideWorkflow(nested_exacts, case_definition, initialState(), nested_trigger);
    QVERIFY2(nested_result.has_value(), nested_result ? "" : nested_result.error().message.c_str());
}

void WorkflowEngineTest::enforcesOperationDocumentAndArgumentBindings() {
    auto definition = canonicalWorkflow();
    auto& order_operation = operationById(definition, "test.op.extension.order");
    order_operation.document_binding = model::WorkflowOperation::DocumentBinding{
        "test.record.entry-order", std::string(64, 'b'), date(2026, 8, 21),
        model::WorkflowOrderId{"test.order.extension"}, model::WorkflowOrderDisposition::Granted};
    auto run = emptyRun();
    const auto case_definition = caseDefinition();
    QVERIFY(execute(definition, case_definition, run,
                    filing("command.binding-opening", "filing.binding-opening",
                           "test.actor.appellant", "test.filing.opening", date(2026, 8, 13),
                           {model::ActorId{"test.actor.appellee"}}))
                .has_value());
    const auto bound_order = model::EnterWorkflowOrder{
        header("command.binding-order", "test.actor.court", date(2026, 8, 21)),
        model::WorkflowOperationId{"test.op.extension.order"},
        model::WorkflowOrderId{"test.order.extension"},
        model::WorkflowOrderDisposition::Granted,
        std::string(64, 'b'),
        model::WorkflowDeadlineId{response_deadline}};
    const auto valid_order =
        engine::decideWorkflow(definition, case_definition, run.state, bound_order);
    QVERIFY(valid_order.has_value());
    for (int variant = 0; variant < 4; ++variant) {
        auto wrong = bound_order;
        if (variant == 0) {
            wrong.order_id = model::WorkflowOrderId{"test.order.other"};
        } else if (variant == 1) {
            wrong.disposition = model::WorkflowOrderDisposition::Denied;
        } else if (variant == 2) {
            wrong.document_sha256 = std::string(64, 'c');
        } else {
            wrong.header.occurred_at = at(date(2026, 8, 22));
        }
        const auto rejected = engine::decideWorkflow(definition, case_definition, run.state, wrong);
        QVERIFY(!rejected.has_value());
        QCOMPARE(rejected.error().code, engine::WorkflowErrorCode::InvalidCommand);
    }

    const auto before_order = run;
    QVERIFY(execute(definition, case_definition, run, bound_order).has_value());
    for (int variant = 0; variant < 4; ++variant) {
        auto events = *valid_order;
        auto& entered = std::get<model::WorkflowOrderEntered>(events.front());
        if (variant == 0) {
            entered.order_id = model::WorkflowOrderId{"test.order.other"};
        } else if (variant == 1) {
            entered.disposition = model::WorkflowOrderDisposition::Denied;
        } else if (variant == 2) {
            entered.document_sha256 = std::string(64, 'c');
        } else {
            entered.header.occurred_at = at(date(2026, 8, 22));
        }
        auto journal = before_order.journal;
        journal.push_back({bound_order, std::move(events)});
        const auto rejected = engine::replayWorkflow(definition, case_definition,
                                                     before_order.initial_state, journal);
        QVERIFY(!rejected.has_value());
    }

    const auto probe = filing("command.binding-probe", "filing.binding-probe",
                              "test.actor.appellee", "test.filing.extension", date(2026, 8, 22),
                              {model::ActorId{"test.actor.appellant"}});
    for (int variant = 0; variant < 4; ++variant) {
        auto forged = run.state;
        if (variant == 0) {
            forged.orders.front().order_id = model::WorkflowOrderId{"test.order.other"};
        } else if (variant == 1) {
            forged.orders.front().disposition = model::WorkflowOrderDisposition::Denied;
        } else if (variant == 2) {
            forged.orders.front().document_sha256 = std::string(64, 'c');
        } else {
            forged.orders.front().entered_at->court_date = date(2026, 8, 22);
        }
        const auto rejected = engine::decideWorkflow(definition, case_definition, forged, probe);
        QVERIFY(!rejected.has_value());
        QCOMPARE(rejected.error().code, engine::WorkflowErrorCode::InvalidState);
    }

    auto inconsistent_selector = definition;
    auto& guarded_accept = operationById(inconsistent_selector, "test.op.extension.accept");
    guarded_accept.preconditions = {model::WorkflowOrderInstancePrecondition{
        model::WorkflowOrderId{"test.order.extension"}, model::WorkflowOrderDisposition::Granted,
        model::WorkflowOperationId{"test.op.extension.order"}, "test.record.entry-other",
        std::string(64, 'b')}};
    const auto inconsistent =
        engine::decideWorkflow(inconsistent_selector, case_definition, run.state, probe);
    QVERIFY(!inconsistent.has_value());
    QCOMPARE(inconsistent.error().code, engine::WorkflowErrorCode::InvalidDefinition);

    auto adjudication = canonicalWorkflow();
    operationById(adjudication, "test.op.schedule-argument").expected_argument_date =
        date(2026, 11, 10);
    operationById(adjudication, "test.op.issue-judgment").document_binding =
        model::WorkflowOperation::DocumentBinding{"test.record.entry-judgment",
                                                  std::string(64, 'c'), date(2026, 11, 10),
                                                  std::nullopt, std::nullopt};
    operationById(adjudication, "test.op.issue-mandate").document_binding =
        model::WorkflowOperation::DocumentBinding{"test.record.entry-mandate", std::string(64, 'd'),
                                                  date(2026, 12, 2), std::nullopt, std::nullopt};
    auto adjudication_run = emptyRun();
    QVERIFY(execute(adjudication, case_definition, adjudication_run,
                    filing("command.binding-opening-2", "filing.binding-opening-2",
                           "test.actor.appellant", "test.filing.opening", date(2026, 8, 13),
                           {model::ActorId{"test.actor.appellee"}}))
                .has_value());
    QVERIFY(execute(adjudication, case_definition, adjudication_run,
                    filing("command.binding-response", "filing.binding-response",
                           "test.actor.appellee", "test.filing.response", date(2026, 9, 10),
                           {model::ActorId{"test.actor.appellant"}}))
                .has_value());
    QVERIFY(execute(adjudication, case_definition, adjudication_run,
                    filing("command.binding-reply", "filing.binding-reply", "test.actor.appellant",
                           "test.filing.reply", date(2026, 9, 20),
                           {model::ActorId{"test.actor.appellee"}}))
                .has_value());

    auto schedule = model::ScheduleWorkflowArgument{
        header("command.binding-argument", "test.actor.court", date(2026, 10, 6)),
        model::WorkflowOperationId{"test.op.schedule-argument"}, date(2026, 11, 11)};
    auto adjudication_result =
        engine::decideWorkflow(adjudication, case_definition, adjudication_run.state, schedule);
    QVERIFY(!adjudication_result.has_value());
    schedule.argument_date = date(2026, 11, 10);
    QVERIFY(execute(adjudication, case_definition, adjudication_run, schedule).has_value());

    auto judgment = model::IssueWorkflowJudgment{
        header("command.binding-judgment", "test.actor.court", date(2026, 11, 10)),
        model::WorkflowOperationId{"test.op.issue-judgment"}, std::string(64, 'e'), "affirmed"};
    adjudication_result =
        engine::decideWorkflow(adjudication, case_definition, adjudication_run.state, judgment);
    QVERIFY(!adjudication_result.has_value());
    judgment.document_sha256 = std::string(64, 'c');
    judgment.header.occurred_at = at(date(2026, 11, 11));
    adjudication_result =
        engine::decideWorkflow(adjudication, case_definition, adjudication_run.state, judgment);
    QVERIFY(!adjudication_result.has_value());
    judgment.header.occurred_at = at(date(2026, 11, 10));
    QVERIFY(execute(adjudication, case_definition, adjudication_run, judgment).has_value());

    auto mandate = model::IssueWorkflowMandate{
        header("command.binding-mandate", "test.actor.court", date(2026, 12, 1)),
        model::WorkflowOperationId{"test.op.issue-mandate"}, std::string(64, 'd')};
    adjudication_result =
        engine::decideWorkflow(adjudication, case_definition, adjudication_run.state, mandate);
    QVERIFY(!adjudication_result.has_value());
    mandate.header.occurred_at = at(date(2026, 12, 2));
    QVERIFY(execute(adjudication, case_definition, adjudication_run, mandate).has_value());
}

void WorkflowEngineTest::rejectsMalformedAndOversizedDispositionInventories() {
    const auto definition = canonicalWorkflow();
    const auto initial = initialState();
    const auto command =
        filing("command.maximum-plan", "filing.maximum-plan", "test.actor.appellant",
               "test.filing.opening", date(2026, 8, 13), {model::ActorId{"test.actor.appellee"}});

    const auto maximum = maximumStructuredCaseDefinition();
    auto result = engine::decideWorkflow(definition, maximum, initial, command);
    QVERIFY2(result.has_value(), result ? "" : result.error().message.c_str());

    const auto legacy_plan_result = engine::decideWorkflow(workflow(), maximum, initial, command);
    QVERIFY(!legacy_plan_result.has_value());
    QCOMPARE(legacy_plan_result.error().code, engine::WorkflowErrorCode::InvalidDefinition);

    auto oversized = maximum;
    oversized.disposition_targets.push_back({model::CaseIssueId{"test.issue.maximum-32"},
                                             model::DispositionTargetId{"test.target.maximum-32"}});
    oversized.disposition_plans.front().components.push_back(
        {model::CaseIssueId{"test.issue.maximum-32"},
         model::DispositionTargetId{"test.target.maximum-32"},
         model::DispositionScope::Whole,
         model::DispositionAction::Affirm,
         false,
         {model::AuthorityId{"test.authority.maximum-32"}},
         {model::RecordAnchorId{"test.anchor.maximum-32"}}});
    result = engine::decideWorkflow(definition, oversized, initial, command);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::InvalidCase);

    auto overlapping = structuredCaseDefinition();
    overlapping.disposition_targets.push_back(
        {model::CaseIssueId{"example.issue.other"},
         model::DispositionTargetId{"example.target.preservation"}});
    result = engine::decideWorkflow(structuredWorkflow(), overlapping, initial, command);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::InvalidCase);

    auto invalid_remand = structuredCaseDefinition();
    invalid_remand.disposition_plans.front().components.front().action =
        model::DispositionAction::Affirm;
    result = engine::decideWorkflow(structuredWorkflow(), invalid_remand, initial, command);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::InvalidCase);

    auto altered_digest = structuredCaseDefinition();
    altered_digest.disposition_plans.front().canonical_sha256.front() = '0';
    result = engine::decideWorkflow(structuredWorkflow(), altered_digest, initial, command);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, engine::WorkflowErrorCode::InvalidCase);
}

} // namespace

QTEST_GUILESS_MAIN(WorkflowEngineTest)

#include "tst_workflow_engine.moc"
