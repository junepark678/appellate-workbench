#pragma once

#include "appellate/model/legal_time.hpp"
#include "appellate/model/workflow.hpp"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace appellate::model {

struct WorkflowCommandHeader final {
    std::string session_id;
    WorkflowCommandId command_id;
    ActorId actor_id;
    LegalTime occurred_at;

    friend bool operator==(const WorkflowCommandHeader&, const WorkflowCommandHeader&) = default;
};

struct WorkflowFieldValue final {
    FilingFieldId id;
    std::string value;

    friend bool operator==(const WorkflowFieldValue&, const WorkflowFieldValue&) = default;
};

struct SubmitWorkflowFiling final {
    WorkflowCommandHeader header;
    WorkflowFilingId filing_id;
    FilingTypeId filing_type;
    std::string document_sha256;
    std::vector<WorkflowFieldValue> fields;
    std::vector<ActorId> served_actors;
    std::optional<WorkflowDeficiencyId> cures_deficiency_id;

    friend bool operator==(const SubmitWorkflowFiling&, const SubmitWorkflowFiling&) = default;
};

struct EnterWorkflowOrder final {
    WorkflowCommandHeader header;
    WorkflowOperationId operation_id;
    WorkflowOrderId order_id;
    WorkflowOrderDisposition disposition{};
    std::string document_sha256;
    std::optional<WorkflowDeadlineId> extension_deadline_id;

    friend bool operator==(const EnterWorkflowOrder&, const EnterWorkflowOrder&) = default;
};

struct SetWorkflowSealed final {
    WorkflowCommandHeader header;
    WorkflowOperationId operation_id;
    bool sealed{};

    friend bool operator==(const SetWorkflowSealed&, const SetWorkflowSealed&) = default;
};

struct ScheduleWorkflowArgument final {
    WorkflowCommandHeader header;
    WorkflowOperationId operation_id;
    LegalDate argument_date;

    friend bool operator==(const ScheduleWorkflowArgument&,
                           const ScheduleWorkflowArgument&) = default;
};

using WorkflowJudgmentSelection = std::variant<std::string, DispositionPlanId>;

struct IssueWorkflowJudgment final {
    WorkflowCommandHeader header;
    WorkflowOperationId operation_id;
    std::string document_sha256;
    WorkflowJudgmentSelection disposition;

    friend bool operator==(const IssueWorkflowJudgment&, const IssueWorkflowJudgment&) = default;
};

struct IssueWorkflowMandate final {
    WorkflowCommandHeader header;
    WorkflowOperationId operation_id;
    std::string document_sha256;

    friend bool operator==(const IssueWorkflowMandate&, const IssueWorkflowMandate&) = default;
};

// Records an explicit court-controlled deadline trigger. The operation supplies the
// versioned authority, day count, and calendar rule; the command supplies only the
// recorded trigger time and the immutable deadline identity.
struct CalculateWorkflowDeadline final {
    WorkflowCommandHeader header;
    WorkflowOperationId operation_id;
    WorkflowDeadlineId deadline_id;

    friend bool operator==(const CalculateWorkflowDeadline&,
                           const CalculateWorkflowDeadline&) = default;
};

// Records a court-controlled transition whose destination and authority are
// supplied exclusively by the pinned workflow definition.
struct AdvanceWorkflowStage final {
    WorkflowCommandHeader header;
    WorkflowOperationId operation_id;

    friend bool operator==(const AdvanceWorkflowStage&, const AdvanceWorkflowStage&) = default;
};

using WorkflowCommand =
    std::variant<SubmitWorkflowFiling, EnterWorkflowOrder, SetWorkflowSealed,
                 ScheduleWorkflowArgument, IssueWorkflowJudgment, IssueWorkflowMandate,
                 CalculateWorkflowDeadline, AdvanceWorkflowStage>;

} // namespace appellate::model
