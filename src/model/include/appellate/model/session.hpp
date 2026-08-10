#pragma once

#include "appellate/model/case_definition.hpp"
#include "appellate/model/legal_time.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace appellate::model {

struct SessionId final {
    std::string value;

    friend bool operator==(const SessionId&, const SessionId&) = default;
};

struct SubmissionId final {
    std::string value;

    friend bool operator==(const SubmissionId&, const SubmissionId&) = default;
};

enum class SessionPhase {
    AwaitingInitiatingFiling,
    DeficiencyPending,
    Docketed,
};

struct PendingDeficiency final {
    SubmissionId submission_id;
    LegalDate cure_deadline;
    std::vector<FilingFieldId> missing_fields;

    friend bool operator==(const PendingDeficiency&, const PendingDeficiency&) = default;
};

struct AcceptedFiling final {
    SubmissionId submission_id;
    ActorId actor_id;
    FilingTypeId filing_type;
    std::string document_sha256;
    std::uint64_t docket_sequence{};

    friend bool operator==(const AcceptedFiling&, const AcceptedFiling&) = default;
};

struct SessionState final {
    SessionId id;
    ProcedureId procedure_id;
    CaseId case_id;
    SessionPhase phase{SessionPhase::AwaitingInitiatingFiling};
    std::uint64_t next_docket_sequence{1};
    std::optional<PendingDeficiency> pending_deficiency;
    std::optional<AcceptedFiling> accepted_filing;
    std::vector<SubmissionId> decided_submissions;

    friend bool operator==(const SessionState&, const SessionState&) = default;
};

} // namespace appellate::model
