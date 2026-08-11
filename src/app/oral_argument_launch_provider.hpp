#pragma once

#include "oral_argument_session_controller.hpp"

#include "appellate/model/case_definition.hpp"
#include "appellate/packs/runtime_pack.hpp"

#include <expected>
#include <memory>

namespace appellate::packs {
class ResolvedPack;
}

namespace appellate::ui {

// The desktop shell deliberately knows neither how a workflow legal-state pin is obtained nor
// where an oral-argument session is stored.  An application boundary that owns those decisions
// must supply this provider and return an already-created or exactly-reopened controller.
class OralArgumentLaunchProvider {
  public:
    virtual ~OralArgumentLaunchProvider() = default;

    [[nodiscard]] virtual auto open(const packs::ResolvedPack& resolved_pack,
                                    const model::CaseId& case_id,
                                    const packs::RuntimeArgumentConfigId& argument_configuration_id)
        -> std::expected<std::unique_ptr<app::OralArgumentSessionController>,
                         app::OralArgumentSessionError> = 0;
};

} // namespace appellate::ui
