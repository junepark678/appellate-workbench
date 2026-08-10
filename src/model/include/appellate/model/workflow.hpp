#pragma once

#include "appellate/model/authority_ref.hpp"
#include "appellate/model/case_definition.hpp"
#include "appellate/model/procedure.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace appellate::model {

struct WorkflowId final {
    std::string value;
    friend bool operator==(const WorkflowId&, const WorkflowId&) = default;
};

struct WorkflowStageId final {
    std::string value;
    friend bool operator==(const WorkflowStageId&, const WorkflowStageId&) = default;
};

struct WorkflowOperationId final {
    std::string value;
    friend bool operator==(const WorkflowOperationId&, const WorkflowOperationId&) = default;
};

struct WorkflowDeadlineId final {
    std::string value;
    friend bool operator==(const WorkflowDeadlineId&, const WorkflowDeadlineId&) = default;
};

struct WorkflowFilingId final {
    std::string value;
    friend bool operator==(const WorkflowFilingId&, const WorkflowFilingId&) = default;
};

struct WorkflowDeficiencyId final {
    std::string value;
    friend bool operator==(const WorkflowDeficiencyId&, const WorkflowDeficiencyId&) = default;
};

struct WorkflowOrderId final {
    std::string value;
    friend bool operator==(const WorkflowOrderId&, const WorkflowOrderId&) = default;
};

struct WorkflowCommandId final {
    std::string value;
    friend bool operator==(const WorkflowCommandId&, const WorkflowCommandId&) = default;
};

struct WorkflowRequirementId final {
    std::string value;
    friend bool operator==(const WorkflowRequirementId&, const WorkflowRequirementId&) = default;
};

enum class WorkflowOpcode {
    AcceptFiling,
    RejectFiling,
    IssueDeficiency,
    CalculateDeadline,
    EnterOrder,
    AdvanceStage,
    SetSealed,
    ScheduleArgument,
    IssueJudgment,
    IssueMandate,
};

struct WorkflowOperation final {
    WorkflowOperationId id;
    WorkflowStageId stage_id;
    WorkflowOpcode opcode{};
    AuthorityBasis authority;
    std::optional<WorkflowStageId> next_stage_id;
    std::optional<std::uint32_t> deadline_days;
    std::optional<DeadlineCounting> deadline_counting;
    std::vector<ActorRoleId> authorized_roles;

    friend bool operator==(const WorkflowOperation&, const WorkflowOperation&) = default;
};

struct WorkflowDeadlinePlan final {
    WorkflowDeadlineId deadline_id;
    WorkflowOperationId operation_id;

    friend bool operator==(const WorkflowDeadlinePlan&, const WorkflowDeadlinePlan&) = default;
};

struct WorkflowFilingRoute final {
    FilingTypeId filing_type;
    WorkflowStageId stage_id;
    std::vector<ActorRoleId> authorized_roles;
    std::vector<FilingFieldId> required_fields;
    std::vector<ActorRoleId> required_service_roles;
    WorkflowOperationId accept_operation_id;
    WorkflowOperationId reject_operation_id;
    std::optional<WorkflowOperationId> deficiency_operation_id;
    std::optional<WorkflowDeadlinePlan> deficiency_deadline;
    std::optional<WorkflowDeadlinePlan> accepted_deadline;
    std::optional<WorkflowOperationId> advance_operation_id;
    std::optional<WorkflowDeadlineId> satisfies_deadline_id;
    bool reject_after_deadline{true};

    friend bool operator==(const WorkflowFilingRoute&, const WorkflowFilingRoute&) = default;
};

struct WorkflowDefinition final {
    WorkflowId id;
    WorkflowStageId initial_stage_id;
    std::vector<WorkflowStageId> stages;
    std::vector<WorkflowOperation> operations;
    std::vector<WorkflowFilingRoute> filing_routes;
    CourtCalendar calendar;

    friend bool operator==(const WorkflowDefinition&, const WorkflowDefinition&) = default;
};

} // namespace appellate::model
