#pragma once

#include "workflow_session_controller.hpp"

#include "appellate/model/case_definition.hpp"

#include <expected>
#include <memory>

namespace appellate::packs {
class ResolvedPack;
}

namespace appellate::ui {

// Supplies a workflow controller derived from one exact installed closure. The desktop shell
// never constructs caller-authored workflow definitions or revision pins at this boundary.
class WorkflowLaunchProvider {
  public:
    virtual ~WorkflowLaunchProvider() = default;

    [[nodiscard]] virtual auto openWorkflow(const packs::ResolvedPack& resolved_pack,
                                            const model::CaseId& case_id)
        -> std::expected<std::unique_ptr<app::WorkflowSessionController>,
                         app::WorkflowSessionError> = 0;
};

} // namespace appellate::ui
