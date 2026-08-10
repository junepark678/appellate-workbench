#pragma once

#include "appellate/model/legal_time.hpp"
#include "appellate/model/session.hpp"

#include <string>
#include <vector>

namespace appellate::model {

struct SubmittedField final {
    FilingFieldId id;
    std::string value;

    friend bool operator==(const SubmittedField&, const SubmittedField&) = default;
};

struct SubmitFiling final {
    SessionId session_id;
    SubmissionId submission_id;
    ActorId actor_id;
    FilingTypeId filing_type;
    LegalTime submitted_at;
    std::string document_sha256;
    std::vector<SubmittedField> fields;

    friend bool operator==(const SubmitFiling&, const SubmitFiling&) = default;
};

} // namespace appellate::model
