#include "appellate/engine/workflow_engine.hpp"
#include "appellate/packs/pack_reader.hpp"
#include "appellate/packs/runtime_pack.hpp"
#include "workflow_action_planner.hpp"

#include <QDir>
#include <QTest>

#include <algorithm>
#include <chrono>
#include <expected>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#ifndef APPELLATE_TEST_FIXTURES
#error "APPELLATE_TEST_FIXTURES must name the test fixture directory"
#endif

namespace {

namespace app = appellate::app;
namespace model = appellate::model;
namespace packs = appellate::packs;

using namespace std::chrono_literals;

constexpr auto submitted_stage = "example.stage.submitted";
constexpr auto court_actor = "example.actor.court";
constexpr auto seal_operation = "example.operation.planner-set-sealed";
constexpr auto argument_operation = "example.operation.planner-schedule-argument";

[[nodiscard]] model::LegalDate date(int year, unsigned month, unsigned day) {
    return model::LegalDate{std::chrono::year{year} / std::chrono::month{month} /
                            std::chrono::day{day}};
}

[[nodiscard]] model::LegalTime authoredTime() {
    const auto court_date = date(2026, 9, 15);
    return model::LegalTime{std::chrono::sys_seconds{std::chrono::sys_days{court_date.value}} + 14h,
                            court_date};
}

[[nodiscard]] auto loadFixtureCase() -> std::expected<packs::RuntimeCase, QString> {
    const auto fixture = QDir(QStringLiteral(APPELLATE_TEST_FIXTURES))
                             .filePath(QStringLiteral("full-resource-pack-v2"));
    const auto loaded = packs::PackReader::readDirectory(fixture);
    if (!loaded) {
        return std::unexpected(loaded.error().message);
    }
    auto runtime = packs::loadRuntimePack(*loaded);
    if (!runtime) {
        return std::unexpected(QString::fromStdString(runtime.error().message));
    }
    if (runtime->cases.size() != 1) {
        return std::unexpected(QStringLiteral("planner fixture must contain exactly one case"));
    }
    return std::move(runtime->cases.front());
}

void clearOpcodeSpecificFields(model::WorkflowOperation& operation) {
    operation.next_stage_id.reset();
    operation.deadline_days.reset();
    operation.deadline_counting.reset();
    operation.preconditions.clear();
    operation.deadline_base_id.reset();
    operation.produced_deadline_id.reset();
    operation.deadline_event_base.reset();
    operation.document_binding.reset();
    operation.expected_argument_date.reset();
    operation.disposition_plan_id.reset();
}

[[nodiscard]] model::WorkflowOperation courtOperation(const model::WorkflowOperation& source,
                                                      std::string id,
                                                      model::WorkflowOpcode opcode) {
    auto operation = source;
    clearOpcodeSpecificFields(operation);
    operation.id = model::WorkflowOperationId{std::move(id)};
    operation.stage_id = model::WorkflowStageId{submitted_stage};
    operation.opcode = opcode;
    operation.allowed_legal_times = {authoredTime()};
    return operation;
}

[[nodiscard]] auto plannerCase(bool include_seal, bool include_argument)
    -> std::expected<packs::RuntimeCase, QString> {
    auto loaded = loadFixtureCase();
    if (!loaded) {
        return std::unexpected(loaded.error());
    }
    auto& workflow = loaded->workflow;
    const auto source = std::ranges::find_if(workflow.operations, [](const auto& operation) {
        return operation.stage_id == model::WorkflowStageId{submitted_stage} &&
               !operation.authorized_roles.empty();
    });
    if (source == workflow.operations.end()) {
        return std::unexpected(QStringLiteral("fixture has no authored court operation"));
    }
    const auto source_operation = *source;

    // Once one operation uses exact legal-time guards, the engine requires every operation in
    // the definition to do so. Keep the fixture internally valid while this focused test adds
    // the two planner-specific operations.
    for (auto& operation : workflow.operations) {
        operation.allowed_legal_times = {authoredTime()};
    }
    if (include_seal) {
        workflow.operations.push_back(
            courtOperation(source_operation, seal_operation, model::WorkflowOpcode::SetSealed));
    }
    if (include_argument) {
        auto schedule = courtOperation(source_operation, argument_operation,
                                       model::WorkflowOpcode::ScheduleArgument);
        schedule.expected_argument_date = authoredTime().court_date;
        workflow.operations.push_back(std::move(schedule));
    }
    return loaded;
}

[[nodiscard]] model::WorkflowState stateFor(const packs::RuntimeCase& runtime_case, bool sealed) {
    model::WorkflowState state;
    state.session_id = "example.session.planner";
    state.workflow_id = runtime_case.workflow.id;
    state.current_stage_id = model::WorkflowStageId{submitted_stage};
    state.next_event_sequence = 2;
    state.decided_commands = {model::WorkflowCommandId{"example.command.prior-stage-event"}};
    state.sealed = sealed;
    state.legal_time_cursor = authoredTime();
    return state;
}

class WorkflowActionPlannerTest final : public QObject {
    Q_OBJECT

  private slots:
    void schedulesArgumentWithExactAuthoredBindings();
    void offersOnlyTheOppositeSealPolarity_data();
    void offersOnlyTheOppositeSealPolarity();
};

void WorkflowActionPlannerTest::schedulesArgumentWithExactAuthoredBindings() {
    const auto loaded = plannerCase(false, true);
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error()));
    const auto state = stateFor(*loaded, false);

    const auto actions = app::eligibleWorkflowActions(*loaded, state);
    const auto found = std::ranges::find_if(actions, [](const auto& option) {
        return std::holds_alternative<model::ScheduleWorkflowArgument>(option.command);
    });
    if (found == actions.end()) {
        const model::WorkflowCommand probe = model::ScheduleWorkflowArgument{
            model::WorkflowCommandHeader{state.session_id,
                                         model::WorkflowCommandId{"example.command.probe"},
                                         model::ActorId{court_actor}, authoredTime()},
            model::WorkflowOperationId{argument_operation}, authoredTime().court_date};
        const auto decision =
            appellate::engine::decideWorkflow(loaded->workflow, loaded->definition, state, probe);
        QVERIFY2(decision.has_value(), decision ? "" : decision.error().message.c_str());
        QFAIL("planner omitted an engine-eligible schedule_argument command");
    }
    QCOMPARE(std::ranges::count_if(
                 actions,
                 [](const auto& option) {
                     return std::holds_alternative<model::ScheduleWorkflowArgument>(option.command);
                 }),
             std::size_t{1});

    const auto& command = std::get<model::ScheduleWorkflowArgument>(found->command);
    QCOMPARE(command.operation_id, model::WorkflowOperationId{argument_operation});
    QCOMPARE(command.header.actor_id, model::ActorId{court_actor});
    QCOMPARE(command.header.occurred_at, authoredTime());
    QCOMPARE(command.argument_date, authoredTime().court_date);
    QCOMPARE(found->key, app::workflowActionKey(found->command));
    QVERIFY(found->key.startsWith(QStringLiteral("argument|")));
    QVERIFY(found->label.startsWith(QStringLiteral("Schedule argument")));
    QVERIFY(!found->record_entry_id.has_value());
    QVERIFY(!found->document_sha256.has_value());
    QVERIFY(found->required_filing_fields.empty());

    QCOMPARE(found->preview_events.size(), std::size_t{1});
    const auto* event =
        std::get_if<model::WorkflowArgumentScheduled>(&found->preview_events.front());
    QVERIFY(event != nullptr);
    QCOMPARE(event->header.operation_id, command.operation_id);
    QCOMPARE(event->header.occurred_at, command.header.occurred_at);
    QCOMPARE(event->argument_date, command.argument_date);
    QVERIFY(!event->next_stage_id.has_value());
}

void WorkflowActionPlannerTest::offersOnlyTheOppositeSealPolarity_data() {
    QTest::addColumn<bool>("currentlySealed");
    QTest::newRow("open-matter-offers-seal") << false;
    QTest::newRow("sealed-matter-offers-unseal") << true;
}

void WorkflowActionPlannerTest::offersOnlyTheOppositeSealPolarity() {
    QFETCH(bool, currentlySealed);
    const auto loaded = plannerCase(true, false);
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error()));
    const auto state = stateFor(*loaded, currentlySealed);

    const auto actions = app::eligibleWorkflowActions(*loaded, state);
    const auto seal_actions = std::ranges::count_if(actions, [](const auto& option) {
        return std::holds_alternative<model::SetWorkflowSealed>(option.command);
    });
    if (seal_actions == 0) {
        const model::WorkflowCommand probe = model::SetWorkflowSealed{
            model::WorkflowCommandHeader{state.session_id,
                                         model::WorkflowCommandId{"example.command.probe"},
                                         model::ActorId{court_actor}, authoredTime()},
            model::WorkflowOperationId{seal_operation}, !currentlySealed};
        const auto decision =
            appellate::engine::decideWorkflow(loaded->workflow, loaded->definition, state, probe);
        QVERIFY2(decision.has_value(), decision ? "" : decision.error().message.c_str());
    }
    QCOMPARE(seal_actions, std::size_t{1});
    const auto found = std::ranges::find_if(actions, [](const auto& option) {
        return std::holds_alternative<model::SetWorkflowSealed>(option.command);
    });
    QVERIFY(found != actions.end());

    const auto& command = std::get<model::SetWorkflowSealed>(found->command);
    QCOMPARE(command.operation_id, model::WorkflowOperationId{seal_operation});
    QCOMPARE(command.header.actor_id, model::ActorId{court_actor});
    QCOMPARE(command.header.occurred_at, authoredTime());
    QCOMPARE(command.sealed, !currentlySealed);
    QVERIFY(command.sealed != state.sealed);
    QCOMPARE(found->key, app::workflowActionKey(found->command));
    QVERIFY(found->key.startsWith(QStringLiteral("sealed|")));
    QVERIFY(found->label.startsWith(currentlySealed ? QStringLiteral("Unseal matter")
                                                    : QStringLiteral("Seal matter")));
    QVERIFY(!found->record_entry_id.has_value());
    QVERIFY(!found->document_sha256.has_value());

    QCOMPARE(found->preview_events.size(), std::size_t{1});
    const auto* event = std::get_if<model::WorkflowSealedSet>(&found->preview_events.front());
    QVERIFY(event != nullptr);
    QCOMPARE(event->header.operation_id, command.operation_id);
    QCOMPARE(event->header.occurred_at, command.header.occurred_at);
    QCOMPARE(event->sealed, command.sealed);
    QVERIFY(event->sealed != state.sealed);
}

} // namespace

QTEST_APPLESS_MAIN(WorkflowActionPlannerTest)

#include "tst_workflow_action_planner.moc"
