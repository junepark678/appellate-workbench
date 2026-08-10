#pragma once

#include "appellate/model/workflow_event.hpp"

#include <vector>

namespace appellate::model {

// A journal record retains the explicit input as well as the resulting event batch. Replay
// re-decides the command and requires this batch to match exactly before reducing any event.
struct WorkflowJournalEntry final {
    WorkflowCommand command;
    std::vector<WorkflowEvent> events;

    friend bool operator==(const WorkflowJournalEntry&, const WorkflowJournalEntry&) = default;
};

} // namespace appellate::model
