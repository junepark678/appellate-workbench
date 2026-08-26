#pragma once

#include "appellate/model/authority_ref.hpp"

#include <string>
#include <vector>

namespace appellate::model {

struct CaseIssueId final {
    std::string value;

    friend bool operator==(const CaseIssueId&, const CaseIssueId&) = default;
};

struct DispositionTargetId final {
    std::string value;

    friend bool operator==(const DispositionTargetId&, const DispositionTargetId&) = default;
};

struct DispositionPlanId final {
    std::string value;

    friend bool operator==(const DispositionPlanId&, const DispositionPlanId&) = default;
};

struct WorkflowOperationId final {
    std::string value;

    friend bool operator==(const WorkflowOperationId&, const WorkflowOperationId&) = default;
};

struct RecordAnchorId final {
    std::string value;

    friend bool operator==(const RecordAnchorId&, const RecordAnchorId&) = default;
};

enum class DispositionScope {
    Whole,
    Part,
};

enum class DispositionAction {
    Affirm,
    Reverse,
    Vacate,
    Dismiss,
    Grant,
    Deny,
};

enum class DispositionFinality {
    Final,
    Nonfinal,
};

struct DispositionTarget final {
    CaseIssueId issue_id;
    DispositionTargetId target_id;

    friend bool operator==(const DispositionTarget&, const DispositionTarget&) = default;
};

struct DispositionComponent final {
    CaseIssueId issue_id;
    DispositionTargetId target_id;
    DispositionScope scope{};
    DispositionAction action{};
    bool remand{};
    std::vector<AuthorityId> authority_ids;
    std::vector<RecordAnchorId> record_anchor_ids;

    friend bool operator==(const DispositionComponent&, const DispositionComponent&) = default;
};

struct DispositionPlan final {
    DispositionPlanId id;
    DispositionFinality finality{};
    std::string canonical_sha256;
    std::vector<DispositionComponent> components;

    friend bool operator==(const DispositionPlan&, const DispositionPlan&) = default;
};

} // namespace appellate::model
