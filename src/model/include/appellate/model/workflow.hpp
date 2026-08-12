#pragma once

#include "appellate/model/authority_ref.hpp"
#include "appellate/model/case_definition.hpp"
#include "appellate/model/procedure.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
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

enum class WorkflowOrderDisposition {
    Granted,
    Denied,
    Other,
};

struct WorkflowFilingPrecondition final {
    FilingTypeId filing_type;
    bool present{};

    friend bool operator==(const WorkflowFilingPrecondition&,
                           const WorkflowFilingPrecondition&) = default;
};

struct WorkflowOrderPrecondition final {
    WorkflowOrderId order_id;
    WorkflowOrderDisposition disposition{};

    friend bool operator==(const WorkflowOrderPrecondition&,
                           const WorkflowOrderPrecondition&) = default;
};

// record_entry_id is an authored provenance anchor. Workflow commands do not carry
// record-entry identities, so execution compares the remaining persisted fields;
// pack loading proves that the anchor is an unsealed entry with document_sha256.
struct WorkflowFilingInstancePrecondition final {
    FilingTypeId filing_type;
    bool present{};
    ActorId actor_id;
    WorkflowFilingId filing_id;
    WorkflowOperationId accept_operation_id;
    std::string record_entry_id;
    std::string document_sha256;

    friend bool operator==(const WorkflowFilingInstancePrecondition&,
                           const WorkflowFilingInstancePrecondition&) = default;
};

struct WorkflowOrderInstancePrecondition final {
    WorkflowOrderId order_id;
    WorkflowOrderDisposition disposition{};
    WorkflowOperationId operation_id;
    std::string record_entry_id;
    std::string document_sha256;

    friend bool operator==(const WorkflowOrderInstancePrecondition&,
                           const WorkflowOrderInstancePrecondition&) = default;
};

enum class WorkflowDeadlineCondition {
    Open,
    Satisfied,
    Elapsed,
    NotElapsed,
    Reached,
};

struct WorkflowDeadlinePrecondition final {
    WorkflowDeadlineId deadline_id;
    WorkflowDeadlineCondition condition{};

    friend bool operator==(const WorkflowDeadlinePrecondition&,
                           const WorkflowDeadlinePrecondition&) = default;
};

struct WorkflowArgumentPrecondition final {
    bool scheduled{};

    friend bool operator==(const WorkflowArgumentPrecondition&,
                           const WorkflowArgumentPrecondition&) = default;
};

enum class WorkflowArgumentDateCondition {
    Reached,
};

struct WorkflowArgumentDatePrecondition final {
    WorkflowArgumentDateCondition condition{};

    friend bool operator==(const WorkflowArgumentDatePrecondition&,
                           const WorkflowArgumentDatePrecondition&) = default;
};

struct WorkflowJudgmentPrecondition final {
    bool issued{};

    friend bool operator==(const WorkflowJudgmentPrecondition&,
                           const WorkflowJudgmentPrecondition&) = default;
};

using WorkflowPrecondition =
    std::variant<WorkflowFilingPrecondition, WorkflowOrderPrecondition,
                 WorkflowDeadlinePrecondition, WorkflowArgumentPrecondition,
                 WorkflowJudgmentPrecondition, WorkflowArgumentDatePrecondition,
                 WorkflowFilingInstancePrecondition, WorkflowOrderInstancePrecondition>;

struct WorkflowJudgmentOccurredDeadlineBase final {
    friend bool operator==(const WorkflowJudgmentOccurredDeadlineBase&,
                           const WorkflowJudgmentOccurredDeadlineBase&) = default;
};

struct WorkflowOrderOccurredDeadlineBase final {
    WorkflowOrderId order_id;
    WorkflowOperationId operation_id;

    friend bool operator==(const WorkflowOrderOccurredDeadlineBase&,
                           const WorkflowOrderOccurredDeadlineBase&) = default;
};

struct WorkflowOrderOccurredOneOfDeadlineBase final {
    WorkflowOrderId order_id;
    std::vector<WorkflowOperationId> operation_ids;

    friend bool operator==(const WorkflowOrderOccurredOneOfDeadlineBase&,
                           const WorkflowOrderOccurredOneOfDeadlineBase&) = default;
};

using WorkflowDeadlineEventBase =
    std::variant<WorkflowJudgmentOccurredDeadlineBase, WorkflowOrderOccurredDeadlineBase,
                 WorkflowOrderOccurredOneOfDeadlineBase>;

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
    std::vector<WorkflowPrecondition> preconditions{};
    std::optional<WorkflowDeadlineId> deadline_base_id{};
    std::optional<WorkflowDeadlineId> produced_deadline_id{};
    std::optional<WorkflowDeadlineEventBase> deadline_event_base{};

    struct DocumentBinding final {
        std::string record_entry_id;
        std::string document_sha256;
        LegalDate expected_court_date;
        std::optional<WorkflowOrderId> order_id;
        std::optional<WorkflowOrderDisposition> disposition;

        friend bool operator==(const DocumentBinding&, const DocumentBinding&) = default;
    };

    std::optional<DocumentBinding> document_binding{};
    std::optional<LegalDate> expected_argument_date{};
    // Optional schema-v2 binding from this exact judgment operation to one
    // structured disposition plan. Absence preserves the legacy authored
    // operation/plan contract.
    std::optional<DispositionPlanId> disposition_plan_id{};
    std::vector<LegalTime> allowed_legal_times{};

    friend bool operator==(const WorkflowOperation&, const WorkflowOperation&) = default;
};

struct WorkflowDeadlinePlan final {
    WorkflowDeadlineId deadline_id;
    WorkflowOperationId operation_id;

    struct StaticDeficiencyTrigger final {
        WorkflowFilingId filing_id;
        ActorId actor_id;
        std::string record_entry_id;
        std::string document_sha256;
        LegalDate expected_court_date;

        friend bool operator==(const StaticDeficiencyTrigger&,
                               const StaticDeficiencyTrigger&) = default;
    };

    // Absence preserves the legacy dynamic prefix + command-id identity.
    std::optional<StaticDeficiencyTrigger> static_trigger{};

    friend bool operator==(const WorkflowDeadlinePlan&, const WorkflowDeadlinePlan&) = default;
};

enum class WorkflowAuthorizedRoleScope {
    CatalogExact,
    CatalogSubset,
};

struct WorkflowFilingRoute final {
    struct FilingBinding final {
        WorkflowFilingId filing_id;
        ActorId actor_id;
        std::string record_entry_id;
        std::string document_sha256;
        LegalTime expected_legal_time;
        std::vector<WorkflowPrecondition> preconditions{};

        friend bool operator==(const FilingBinding&, const FilingBinding&) = default;
    };

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
    WorkflowAuthorizedRoleScope authorized_role_scope{WorkflowAuthorizedRoleScope::CatalogExact};
    std::vector<FilingBinding> filing_bindings{};

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
