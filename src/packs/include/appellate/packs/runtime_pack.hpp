#pragma once

#include "appellate/model/case_definition.hpp"
#include "appellate/model/judge_profile.hpp"
#include "appellate/model/pack_id.hpp"
#include "appellate/model/workflow.hpp"
#include "appellate/packs/pack_reader.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace appellate::packs {

struct RuntimeCourtId final {
    std::string value;
    friend bool operator==(const RuntimeCourtId&, const RuntimeCourtId&) = default;
};

struct RuntimeJurisdictionId final {
    std::string value;
    friend bool operator==(const RuntimeJurisdictionId&, const RuntimeJurisdictionId&) = default;
};

struct RuntimeRecordId final {
    std::string value;
    friend bool operator==(const RuntimeRecordId&, const RuntimeRecordId&) = default;
};

struct RuntimeRecordEntryId final {
    std::string value;
    friend bool operator==(const RuntimeRecordEntryId&, const RuntimeRecordEntryId&) = default;
};

struct RuntimeFilingCatalogId final {
    std::string value;
    friend bool operator==(const RuntimeFilingCatalogId&, const RuntimeFilingCatalogId&) = default;
};

struct RuntimeAuthoritySetId final {
    std::string value;
    friend bool operator==(const RuntimeAuthoritySetId&, const RuntimeAuthoritySetId&) = default;
};

struct RuntimeBenchConfigurationId final {
    std::string value;
    friend bool operator==(const RuntimeBenchConfigurationId&,
                           const RuntimeBenchConfigurationId&) = default;
};

struct RuntimeBenchSeatId final {
    std::string value;
    friend bool operator==(const RuntimeBenchSeatId&, const RuntimeBenchSeatId&) = default;
};

struct RuntimeJudgeProfileId final {
    std::string value;
    friend bool operator==(const RuntimeJudgeProfileId&, const RuntimeJudgeProfileId&) = default;
};

struct RuntimeArgumentConfigId final {
    std::string value;
    friend bool operator==(const RuntimeArgumentConfigId&,
                           const RuntimeArgumentConfigId&) = default;
};

struct RuntimeIssueId final {
    std::string value;
    friend bool operator==(const RuntimeIssueId&, const RuntimeIssueId&) = default;
};

enum class RuntimeProceedingType {
    CivilAppeal,
    CriminalAppeal,
    AgencyReview,
    OriginalWrit,
};

struct RuntimeDocketEntry final {
    RuntimeRecordEntryId id;
    std::uint32_t entry_number{};
    model::LegalDate filed_on;
    std::string title;
    std::string asset_path;
    std::string asset_sha256;
    std::uint32_t page_count{};
    bool sealed{};

    friend bool operator==(const RuntimeDocketEntry&, const RuntimeDocketEntry&) = default;
};

struct RuntimeRecord final {
    RuntimeRecordId id;
    std::string caption;
    std::vector<RuntimeDocketEntry> docket_entries;

    friend bool operator==(const RuntimeRecord&, const RuntimeRecord&) = default;
};

struct RuntimeIssue final {
    RuntimeIssueId id;
    std::string title;
    std::vector<model::AuthorityId> authority_ids;
    std::vector<RuntimeRecordEntryId> record_anchor_ids;

    friend bool operator==(const RuntimeIssue&, const RuntimeIssue&) = default;
};

struct RuntimeProcedure final {
    model::ProcedureId id;
    RuntimeCourtId court_id;
    RuntimeProceedingType proceeding_type{};
    std::vector<model::ActorRoleId> actor_roles;
    RuntimeFilingCatalogId filing_catalog_id;
    model::WorkflowId workflow_id;
    std::vector<RuntimeAuthoritySetId> authority_set_ids;

    friend bool operator==(const RuntimeProcedure&, const RuntimeProcedure&) = default;
};

struct RuntimeCourt final {
    RuntimeCourtId id;
    RuntimeJurisdictionId jurisdiction_id;
    std::string name;
    model::CourtRole role{};
    std::vector<RuntimeAuthoritySetId> authority_set_ids;
    model::CourtCalendar calendar;

    friend bool operator==(const RuntimeCourt&, const RuntimeCourt&) = default;
};

struct RuntimeBenchSeat final {
    RuntimeBenchSeatId id;
    RuntimeJudgeProfileId profile_id;
    model::CourtRole court_role{};
    model::JudgeProfile profile;

    friend bool operator==(const RuntimeBenchSeat&, const RuntimeBenchSeat&) = default;
};

struct RuntimeBenchConfiguration final {
    RuntimeBenchConfigurationId id;
    RuntimeCourtId court_id;
    RuntimeBenchSeatId presiding_seat_id;
    std::vector<RuntimeBenchSeat> seats;

    friend bool operator==(const RuntimeBenchConfiguration&,
                           const RuntimeBenchConfiguration&) = default;
};

struct RuntimeArgumentConfiguration final {
    RuntimeArgumentConfigId id;
    model::CaseId case_id;
    RuntimeBenchConfiguration bench;
    std::uint32_t total_seconds{};
    std::uint32_t rebuttal_seconds{};
    std::vector<RuntimeIssueId> permitted_issue_ids;

    friend bool operator==(const RuntimeArgumentConfiguration&,
                           const RuntimeArgumentConfiguration&) = default;
};

struct RuntimeCase final {
    model::CaseDefinition definition;
    std::string title;
    RuntimeProcedure procedure;
    RuntimeCourt court;
    model::WorkflowDefinition workflow;
    RuntimeRecord record;
    std::vector<RuntimeIssue> issues;
    model::WorkflowOperationId authored_disposition_id;
    std::vector<RuntimeArgumentConfiguration> argument_configurations;

    friend bool operator==(const RuntimeCase&, const RuntimeCase&) = default;
};

struct RuntimePack final {
    model::PackRevision revision;
    std::vector<RuntimeCase> cases;

    friend bool operator==(const RuntimePack&, const RuntimePack&) = default;
};

enum class RuntimePackErrorCode {
    InvalidPack,
    InvalidResource,
    MissingResource,
    DuplicateResource,
    WrongResourceKind,
    MissingArgumentConfiguration,
    CrossReferenceFailure,
};

struct RuntimePackError final {
    RuntimePackErrorCode code{};
    std::string message;

    friend bool operator==(const RuntimePackError&, const RuntimePackError&) = default;
};

[[nodiscard]] std::expected<RuntimePack, RuntimePackError> loadRuntimePack(const LoadedPack& pack);

} // namespace appellate::packs
