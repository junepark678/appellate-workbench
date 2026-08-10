#include "appellate/engine/workflow_engine.hpp"

#include <QTest>

#include <chrono>
#include <expected>
#include <optional>
#include <string>
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
                                    std::move(authorized)};
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
    void rejectsUnauthorizedCourtActionAndMissingAuthority();
    void replayRejectsImpossibleGroupsAndForgedCourtActions();
    void rejectsBackdatedExplicitLegalTime();
    void rejectsDirtySnapshotsAndMalformedDefinitions();
    void rejectsMalformedRouteOutcomeCombinations();
    void replayRejectsTamperedTrace();
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

} // namespace

QTEST_GUILESS_MAIN(WorkflowEngineTest)

#include "tst_workflow_engine.moc"
