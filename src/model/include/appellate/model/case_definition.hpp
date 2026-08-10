#pragma once

#include "appellate/model/procedure.hpp"

#include <string>
#include <vector>

namespace appellate::model {

struct CaseId final {
    std::string value;

    friend bool operator==(const CaseId&, const CaseId&) = default;
};

struct ActorId final {
    std::string value;

    friend bool operator==(const ActorId&, const ActorId&) = default;
};

struct CaseActor final {
    ActorId id;
    ActorRoleId role;

    friend bool operator==(const CaseActor&, const CaseActor&) = default;
};

struct CaseDefinition final {
    CaseId id;
    ProcedureId procedure_id;
    std::vector<CaseActor> actors;

    friend bool operator==(const CaseDefinition&, const CaseDefinition&) = default;
};

} // namespace appellate::model
