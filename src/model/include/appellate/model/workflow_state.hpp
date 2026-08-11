#pragma once

#include "appellate/model/workflow_event.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace appellate::model {

struct WorkflowPendingCommand final {
    WorkflowCommandId command_id;
    std::uint32_t next_event_index{};
    std::uint32_t event_count{};

    friend bool operator==(const WorkflowPendingCommand&, const WorkflowPendingCommand&) = default;
};

struct WorkflowFilingRecord final {
    WorkflowFilingId filing_id;
    FilingTypeId filing_type;
    ActorId actor_id;
    std::string document_sha256;
    LegalTime accepted_at;
    std::vector<ActorId> served_actors;

    friend bool operator==(const WorkflowFilingRecord&, const WorkflowFilingRecord&) = default;
};

enum class WorkflowDeadlineStatus {
    Open,
    Satisfied,
};

struct WorkflowDeadlineRecord final {
    WorkflowDeadlineId deadline_id;
    WorkflowDeadlinePurpose purpose{};
    LegalDate due_date;
    WorkflowDeadlineStatus status{WorkflowDeadlineStatus::Open};

    friend bool operator==(const WorkflowDeadlineRecord&, const WorkflowDeadlineRecord&) = default;
};

struct WorkflowDeficiencyRecord final {
    WorkflowDeficiencyId deficiency_id;
    WorkflowFilingId filing_id;
    FilingTypeId filing_type;
    ActorId actor_id;
    std::vector<WorkflowRequirementId> missing_requirements;
    std::optional<WorkflowDeadlineId> cure_deadline_id;
    bool cured{};

    friend bool operator==(const WorkflowDeficiencyRecord&,
                           const WorkflowDeficiencyRecord&) = default;
};

struct WorkflowOrderRecord final {
    WorkflowOrderId order_id;
    WorkflowOrderDisposition disposition{};
    std::string document_sha256;
    std::optional<WorkflowOperationId> operation_id{};
    std::optional<LegalTime> entered_at{};

    friend bool operator==(const WorkflowOrderRecord&, const WorkflowOrderRecord&) = default;
};

struct WorkflowState final {
    std::string session_id;
    WorkflowId workflow_id;
    WorkflowStageId current_stage_id;
    std::uint64_t next_event_sequence{1};
    std::optional<WorkflowPendingCommand> pending_command;
    std::vector<WorkflowCommandId> decided_commands;
    std::vector<WorkflowFilingRecord> accepted_filings;
    std::vector<WorkflowDeadlineRecord> deadlines;
    std::vector<WorkflowDeficiencyRecord> deficiencies;
    std::vector<WorkflowOrderRecord> orders;
    bool sealed{};
    std::optional<LegalDate> argument_date;
    std::optional<std::string> judgment_sha256;
    std::optional<std::string> mandate_sha256;
    std::optional<LegalTime> legal_time_cursor;
    std::optional<WorkflowJudgmentDisposition> judgment_disposition{};
    std::optional<LegalTime> judgment_issued_at{};

    friend bool operator==(const WorkflowState&, const WorkflowState&) = default;
};

} // namespace appellate::model
