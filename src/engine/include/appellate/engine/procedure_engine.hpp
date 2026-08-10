#pragma once

#include "appellate/engine/error.hpp"
#include "appellate/model/case_definition.hpp"
#include "appellate/model/command.hpp"
#include "appellate/model/event.hpp"
#include "appellate/model/procedure.hpp"
#include "appellate/model/session.hpp"

#include <expected>
#include <span>
#include <vector>

namespace appellate::engine {

[[nodiscard]] std::expected<std::vector<model::LegalEvent>, Error>
decide(const model::ProcedureDefinition& procedure, const model::CaseDefinition& case_definition,
       const model::SessionState& state, const model::SubmitFiling& command);

[[nodiscard]] std::expected<model::SessionState, Error>
apply(const model::ProcedureDefinition& procedure, const model::CaseDefinition& case_definition,
      const model::SessionState& state, const model::LegalEvent& event);

[[nodiscard]] std::expected<model::SessionState, Error>
replay(const model::ProcedureDefinition& procedure, const model::CaseDefinition& case_definition,
       model::SessionState initial_state, std::span<const model::LegalEvent> events);

} // namespace appellate::engine
