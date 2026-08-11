#pragma once

#include "appellate/model/case_definition.hpp"
#include "appellate/model/workflow.hpp"
#include "appellate/model/workflow_command.hpp"
#include "appellate/model/workflow_event.hpp"
#include "appellate/model/workflow_journal.hpp"
#include "appellate/model/workflow_state.hpp"

#include <expected>
#include <span>
#include <string>
#include <vector>

namespace appellate::engine {

enum class WorkflowErrorCode {
    InvalidDefinition,
    MissingAuthority,
    InvalidCase,
    InvalidState,
    InvalidCommand,
    UnknownActor,
    UnauthorizedActor,
    DuplicateCommand,
    BackdatedCommand,
    InvalidEvent,
    InvalidTransition,
    SequenceOverflow,
    UnmetPrecondition,
};

struct WorkflowError final {
    WorkflowErrorCode code;
    std::string message;

    friend bool operator==(const WorkflowError&, const WorkflowError&) = default;
};

[[nodiscard]] std::expected<std::vector<model::WorkflowEvent>, WorkflowError>
decideWorkflow(const model::WorkflowDefinition& workflow,
               const model::CaseDefinition& case_definition, const model::WorkflowState& state,
               const model::WorkflowCommand& command);

[[nodiscard]] std::expected<model::WorkflowState, WorkflowError>
replayWorkflow(const model::WorkflowDefinition& workflow,
               const model::CaseDefinition& case_definition, model::WorkflowState initial_state,
               std::span<const model::WorkflowJournalEntry> journal);

} // namespace appellate::engine
