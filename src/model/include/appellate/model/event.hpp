#pragma once

#include "appellate/model/authority_ref.hpp"
#include "appellate/model/command.hpp"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace appellate::model {

struct FilingAccepted final {
    SessionId session_id;
    SubmissionId submission_id;
    ActorId actor_id;
    FilingTypeId filing_type;
    LegalTime submitted_at;
    std::string document_sha256;
    std::uint64_t docket_sequence{};
    AuthorityBasis authority;

    friend bool operator==(const FilingAccepted&, const FilingAccepted&) = default;
};

struct FilingDeficiencyIssued final {
    SessionId session_id;
    SubmissionId submission_id;
    ActorId actor_id;
    FilingTypeId filing_type;
    LegalTime submitted_at;
    std::string document_sha256;
    std::vector<FilingFieldId> missing_fields;
    LegalDate cure_deadline;
    std::uint64_t docket_sequence{};
    AuthorityBasis authority;

    friend bool operator==(const FilingDeficiencyIssued&, const FilingDeficiencyIssued&) = default;
};

enum class FilingRejectionReason {
    UnauthorizedActor,
    WrongFilingType,
    CureDeadlineExpired,
    DeficiencyNotCured,
    ProceedingAlreadyDocketed,
};

struct FilingRejected final {
    SessionId session_id;
    SubmissionId submission_id;
    ActorId actor_id;
    FilingTypeId filing_type;
    LegalTime submitted_at;
    FilingRejectionReason reason{};
    AuthorityBasis authority;

    friend bool operator==(const FilingRejected&, const FilingRejected&) = default;
};

using LegalEvent = std::variant<FilingAccepted, FilingDeficiencyIssued, FilingRejected>;

} // namespace appellate::model
