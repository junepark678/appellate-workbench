#pragma once

#include "appellate/model/workflow_command.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace appellate::model {

struct WorkflowEventHeader final {
    std::string session_id;
    WorkflowId workflow_id;
    WorkflowCommandId command_id;
    WorkflowOperationId operation_id;
    std::uint64_t sequence{};
    std::uint32_t command_event_index{};
    std::uint32_t command_event_count{};
    LegalTime occurred_at;
    AuthorityBasis authority;
    std::vector<WorkflowPrecondition> preconditions{};

    friend bool operator==(const WorkflowEventHeader&, const WorkflowEventHeader&) = default;
};

struct WorkflowFilingAccepted final {
    WorkflowEventHeader header;
    WorkflowFilingId filing_id;
    FilingTypeId filing_type;
    ActorId actor_id;
    std::string document_sha256;
    std::vector<ActorId> served_actors;
    std::optional<WorkflowDeficiencyId> cured_deficiency_id;
    std::optional<WorkflowDeadlineId> satisfied_deadline_id;

    friend bool operator==(const WorkflowFilingAccepted&, const WorkflowFilingAccepted&) = default;
};

enum class WorkflowFilingRejectionReason {
    UnauthorizedActor,
    IneligibleFiling,
    NonconformingFiling,
    DeadlineExpired,
    UnknownDeficiency,
};

struct WorkflowFilingRejected final {
    WorkflowEventHeader header;
    WorkflowFilingId filing_id;
    FilingTypeId filing_type;
    ActorId actor_id;
    WorkflowFilingRejectionReason reason{};

    friend bool operator==(const WorkflowFilingRejected&, const WorkflowFilingRejected&) = default;
};

struct WorkflowDeficiencyIssued final {
    WorkflowEventHeader header;
    WorkflowDeficiencyId deficiency_id;
    WorkflowFilingId filing_id;
    FilingTypeId filing_type;
    ActorId actor_id;
    std::vector<WorkflowRequirementId> missing_requirements;
    std::optional<WorkflowDeadlineId> cure_deadline_id;

    friend bool operator==(const WorkflowDeficiencyIssued&,
                           const WorkflowDeficiencyIssued&) = default;
};

enum class WorkflowDeadlinePurpose {
    Filing,
    DeficiencyCure,
};

struct WorkflowDeadlineCalculated final {
    WorkflowEventHeader header;
    WorkflowDeadlineId deadline_id;
    WorkflowDeadlinePurpose purpose{};
    LegalDate base_date;
    LegalDate due_date;

    friend bool operator==(const WorkflowDeadlineCalculated&,
                           const WorkflowDeadlineCalculated&) = default;
};

struct WorkflowDeadlineExtension final {
    WorkflowDeadlineId deadline_id;
    LegalDate previous_due_date;
    LegalDate extended_due_date;

    friend bool operator==(const WorkflowDeadlineExtension&,
                           const WorkflowDeadlineExtension&) = default;
};

struct WorkflowOrderEntered final {
    WorkflowEventHeader header;
    WorkflowOrderId order_id;
    WorkflowOrderDisposition disposition{};
    std::string document_sha256;
    std::optional<WorkflowDeadlineExtension> extension;

    friend bool operator==(const WorkflowOrderEntered&, const WorkflowOrderEntered&) = default;
};

struct WorkflowStageAdvanced final {
    WorkflowEventHeader header;
    WorkflowStageId previous_stage_id;
    WorkflowStageId next_stage_id;

    friend bool operator==(const WorkflowStageAdvanced&, const WorkflowStageAdvanced&) = default;
};

struct WorkflowSealedSet final {
    WorkflowEventHeader header;
    bool sealed{};

    friend bool operator==(const WorkflowSealedSet&, const WorkflowSealedSet&) = default;
};

struct WorkflowArgumentScheduled final {
    WorkflowEventHeader header;
    LegalDate argument_date;
    std::optional<WorkflowStageId> next_stage_id;

    friend bool operator==(const WorkflowArgumentScheduled&,
                           const WorkflowArgumentScheduled&) = default;
};

using WorkflowJudgmentDisposition = std::variant<std::string, DispositionPlan>;

struct WorkflowJudgmentIssued final {
    WorkflowEventHeader header;
    std::string document_sha256;
    WorkflowJudgmentDisposition disposition;
    std::optional<WorkflowStageId> next_stage_id;

    friend bool operator==(const WorkflowJudgmentIssued&, const WorkflowJudgmentIssued&) = default;
};

struct WorkflowMandateIssued final {
    WorkflowEventHeader header;
    std::string document_sha256;
    std::optional<WorkflowStageId> next_stage_id;

    friend bool operator==(const WorkflowMandateIssued&, const WorkflowMandateIssued&) = default;
};

using WorkflowEvent =
    std::variant<WorkflowFilingAccepted, WorkflowFilingRejected, WorkflowDeficiencyIssued,
                 WorkflowDeadlineCalculated, WorkflowOrderEntered, WorkflowStageAdvanced,
                 WorkflowSealedSet, WorkflowArgumentScheduled, WorkflowJudgmentIssued,
                 WorkflowMandateIssued>;

} // namespace appellate::model
