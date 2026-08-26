#pragma once

#include "appellate/model/workflow_command.hpp"
#include "appellate/model/workflow_event.hpp"
#include "appellate/model/workflow_state.hpp"
#include "appellate/packs/runtime_pack.hpp"

#include <QString>

#include <optional>
#include <string>
#include <vector>

namespace appellate::app {

struct WorkflowActionOption final {
    QString key;
    QString label;
    QString description;
    model::WorkflowCommand command;
    std::vector<model::WorkflowEvent> preview_events;
    std::optional<std::string> record_entry_id;
    std::optional<std::string> document_sha256;
    std::vector<model::FilingFieldId> required_filing_fields;
};

// Enumerates each exact authored legal-time pair and returns actions whose current draft has an
// engine-authored outcome. Filing options carry empty values for each required field, so their
// initial preview may be an authored rejection; a caller must collect values explicitly rather
// than inventing them. The optional fallback is used only by legacy operations with no authored
// allowed_legal_times.
[[nodiscard]] auto
eligibleWorkflowActions(const packs::RuntimeCase& runtime_case, const model::WorkflowState& state,
                        std::optional<model::LegalTime> legacy_fallback = std::nullopt)
    -> std::vector<WorkflowActionOption>;

// Stable within one workflow state and independent of the generated command ID. This lets a UI
// preserve a user's selected authored action while refreshing the legal-clock preview.
[[nodiscard]] QString workflowActionKey(const model::WorkflowCommand& command);

} // namespace appellate::app
