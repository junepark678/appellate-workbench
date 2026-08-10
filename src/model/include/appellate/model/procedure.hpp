#pragma once

#include "appellate/model/authority_ref.hpp"
#include "appellate/model/legal_time.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace appellate::model {

struct ProcedureId final {
    std::string value;

    friend bool operator==(const ProcedureId&, const ProcedureId&) = default;
};

struct ActorRoleId final {
    std::string value;

    friend bool operator==(const ActorRoleId&, const ActorRoleId&) = default;
};

struct FilingTypeId final {
    std::string value;

    friend bool operator==(const FilingTypeId&, const FilingTypeId&) = default;
};

struct FilingFieldId final {
    std::string value;

    friend bool operator==(const FilingFieldId&, const FilingFieldId&) = default;
};

enum class DeadlineCounting {
    CalendarDays,
    BusinessDays,
};

struct CureDeadlineRule final {
    std::uint32_t days{};
    DeadlineCounting counting{DeadlineCounting::CalendarDays};
    bool roll_forward_to_business_day{true};

    friend bool operator==(const CureDeadlineRule&, const CureDeadlineRule&) = default;
};

struct CourtCalendar final {
    std::vector<LegalDate> holidays;

    friend bool operator==(const CourtCalendar&, const CourtCalendar&) = default;
};

struct InitiatingFilingRule final {
    FilingTypeId filing_type;
    std::vector<ActorRoleId> authorized_roles;
    std::vector<FilingFieldId> required_fields;
    CureDeadlineRule cure_deadline;
    AuthorityBasis filing_authority;
    AuthorityBasis actor_authority;
    AuthorityBasis deficiency_authority;

    friend bool operator==(const InitiatingFilingRule&, const InitiatingFilingRule&) = default;
};

struct ProcedureDefinition final {
    ProcedureId id;
    CourtCalendar calendar;
    InitiatingFilingRule initiating_filing;

    friend bool operator==(const ProcedureDefinition&, const ProcedureDefinition&) = default;
};

} // namespace appellate::model
