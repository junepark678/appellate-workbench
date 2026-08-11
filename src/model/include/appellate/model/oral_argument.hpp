#pragma once

#include "appellate/model/authority_ref.hpp"
#include "appellate/model/case_definition.hpp"
#include "appellate/model/judge_profile.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace appellate::model {

enum class OralArgumentMode {
    ActualRecord,
    CounterfactualTraining,
};

enum class ArgumentFocusTopic {
    Jurisdiction,
    Preservation,
    StandardOfReview,
    RecordSupport,
    GoverningAuthority,
    Merits,
    Remedy,
    PracticalConsequences,
};

[[nodiscard]] constexpr std::string_view argumentFocusTopicId(ArgumentFocusTopic topic) noexcept {
    switch (topic) {
    case ArgumentFocusTopic::Jurisdiction:
        return "workbench.topic.jurisdiction";
    case ArgumentFocusTopic::Preservation:
        return "workbench.topic.preservation";
    case ArgumentFocusTopic::StandardOfReview:
        return "workbench.topic.standard-of-review";
    case ArgumentFocusTopic::RecordSupport:
        return "workbench.topic.record-support";
    case ArgumentFocusTopic::GoverningAuthority:
        return "workbench.topic.governing-authority";
    case ArgumentFocusTopic::Merits:
        return "workbench.topic.merits";
    case ArgumentFocusTopic::Remedy:
        return "workbench.topic.remedy";
    case ArgumentFocusTopic::PracticalConsequences:
        return "workbench.topic.practical-consequences";
    }
    return {};
}

[[nodiscard]] constexpr std::optional<ArgumentFocusTopic>
argumentFocusTopicFromId(std::string_view id) noexcept {
    if (id == "workbench.topic.jurisdiction") {
        return ArgumentFocusTopic::Jurisdiction;
    }
    if (id == "workbench.topic.preservation") {
        return ArgumentFocusTopic::Preservation;
    }
    if (id == "workbench.topic.standard-of-review") {
        return ArgumentFocusTopic::StandardOfReview;
    }
    if (id == "workbench.topic.record-support") {
        return ArgumentFocusTopic::RecordSupport;
    }
    if (id == "workbench.topic.governing-authority") {
        return ArgumentFocusTopic::GoverningAuthority;
    }
    if (id == "workbench.topic.merits") {
        return ArgumentFocusTopic::Merits;
    }
    if (id == "workbench.topic.remedy") {
        return ArgumentFocusTopic::Remedy;
    }
    if (id == "workbench.topic.practical-consequences") {
        return ArgumentFocusTopic::PracticalConsequences;
    }
    return std::nullopt;
}

struct AuthorityArgumentGrounding final {
    std::string grounding_id;
    AuthorityRef authority;

    friend bool operator==(const AuthorityArgumentGrounding&,
                           const AuthorityArgumentGrounding&) = default;
};

struct BriefPageArgumentGrounding final {
    std::string grounding_id;
    std::string record_entry_id;
    std::uint32_t page_number{};
    std::string asset_sha256;

    friend bool operator==(const BriefPageArgumentGrounding&,
                           const BriefPageArgumentGrounding&) = default;
};

struct RecordPageArgumentGrounding final {
    std::string grounding_id;
    std::string record_anchor_id;
    std::string record_entry_id;
    std::uint32_t page_number{};
    std::string asset_sha256;
    std::optional<std::string> citation_label;

    friend bool operator==(const RecordPageArgumentGrounding&,
                           const RecordPageArgumentGrounding&) = default;
};

using AuthoredArgumentGrounding =
    std::variant<AuthorityArgumentGrounding, BriefPageArgumentGrounding,
                 RecordPageArgumentGrounding>;

struct ArgumentIssueTopics final {
    std::string issue_id;
    std::vector<ArgumentFocusTopic> topics;

    friend bool operator==(const ArgumentIssueTopics&, const ArgumentIssueTopics&) = default;
};

struct AuthoredArgumentQuestion final {
    std::string id;
    std::string issue_id;
    ArgumentFocusTopic topic{ArgumentFocusTopic::Merits};
    std::string prompt;
    std::vector<AuthoredArgumentGrounding> grounding;

    friend bool operator==(const AuthoredArgumentQuestion&,
                           const AuthoredArgumentQuestion&) = default;
};

struct AuthoredQuestionBank final {
    CaseId case_id;
    std::string argument_configuration_id;
    OralArgumentMode mode{OralArgumentMode::ActualRecord};
    std::string grounding_digest;
    std::vector<ArgumentIssueTopics> issue_topics;
    std::vector<AuthoredArgumentQuestion> questions;

    friend bool operator==(const AuthoredQuestionBank&, const AuthoredQuestionBank&) = default;
};

enum class GroundingKind {
    Authority,
    BriefPassage,
    RecordPage,
};

struct ArgumentGroundingRef final {
    GroundingKind kind{GroundingKind::Authority};
    std::string id;
    std::optional<std::uint32_t> page_number;

    friend bool operator==(const ArgumentGroundingRef&, const ArgumentGroundingRef&) = default;
};

struct ArgumentIssue final {
    std::string id;
    std::string label;
    std::vector<ArgumentGroundingRef> grounding;
    std::vector<std::string> question_prompts;
    std::vector<std::string> hypothetical_prompts;

    friend bool operator==(const ArgumentIssue&, const ArgumentIssue&) = default;
};

struct ArgumentGrounding final {
    std::vector<ArgumentIssue> issues;

    friend bool operator==(const ArgumentGrounding&, const ArgumentGrounding&) = default;
};

struct BenchSeat final {
    std::string id;
    JudgeProfile profile;

    friend bool operator==(const BenchSeat&, const BenchSeat&) = default;
};

struct BenchConfiguration final {
    std::string jurisdiction_id;
    CourtRole court_role{CourtRole::Appellate};
    std::vector<BenchSeat> seats;
    std::string presiding_seat_id;

    friend bool operator==(const BenchConfiguration&, const BenchConfiguration&) = default;
};

struct OralArgumentConfiguration final {
    std::chrono::seconds principal_time{};
    std::chrono::seconds rebuttal_time{};
    double classification_confidence_threshold{0.7};
    std::uint32_t maximum_follow_up_depth{3};
    std::string behavior_definition_digest;
    std::string grounding_digest;
    std::string legal_state_digest;
    std::string authored_disposition_id;

    friend bool operator==(const OralArgumentConfiguration&,
                           const OralArgumentConfiguration&) = default;
};

struct CanonicalOralArgumentContract final {
    CaseId case_id;
    std::string argument_configuration_id;
    OralArgumentMode mode{OralArgumentMode::ActualRecord};
    std::string grounding_digest;
    std::string definition_digest;

    friend bool operator==(const CanonicalOralArgumentContract&,
                           const CanonicalOralArgumentContract&) = default;
};

struct CanonicalOralArgumentDefinition final {
    OralArgumentConfiguration configuration;
    BenchConfiguration bench;
    AuthoredQuestionBank question_bank;

    friend bool operator==(const CanonicalOralArgumentDefinition&,
                           const CanonicalOralArgumentDefinition&) = default;
};

enum class OralArgumentPhase {
    NotStarted,
    Principal,
    Rebuttal,
    Complete,
};

enum class CounselActKind {
    Answer,
    Concession,
    RecordClaim,
};

struct CounselAnswer final {
    CounselActKind kind{CounselActKind::Answer};
    std::string text;
    std::string issue_id;
    std::vector<std::string> cited_grounding_ids;
    double classification_confidence{1.0};
    std::chrono::seconds elapsed{};

    friend bool operator==(const CounselAnswer&, const CounselAnswer&) = default;
};

enum class BenchActKind {
    Question,
    Interruption,
    FollowUp,
    Hypothetical,
    RecordPinDemand,
    ClarificationRequest,
    TimeExpired,
};

struct LegacyQuestionSelection final {
    std::string prompt;
    std::vector<ArgumentGroundingRef> grounding;

    friend bool operator==(const LegacyQuestionSelection&, const LegacyQuestionSelection&) = default;
};

struct AuthoredQuestionSelection final {
    std::string question_id;
    ArgumentFocusTopic topic{ArgumentFocusTopic::Merits};
    OralArgumentMode mode{OralArgumentMode::ActualRecord};
    std::string prompt;
    std::vector<AuthoredArgumentGrounding> grounding;

    friend bool operator==(const AuthoredQuestionSelection&,
                           const AuthoredQuestionSelection&) = default;
};

using GroundedQuestionSelection =
    std::variant<LegacyQuestionSelection, AuthoredQuestionSelection>;

struct GroundedQuestion final {
    std::string issue_id;
    GroundedQuestionSelection selection;
    std::optional<std::uint64_t> parent_act_sequence;
    bool recalls_concession{};

    GroundedQuestion() = default;

    GroundedQuestion(std::string issue, std::string prompt,
                     std::vector<ArgumentGroundingRef> grounding,
                     std::optional<std::uint64_t> parent, bool recalls)
        : issue_id(std::move(issue)),
          selection(LegacyQuestionSelection{std::move(prompt), std::move(grounding)}),
          parent_act_sequence(parent), recalls_concession(recalls) {}

    GroundedQuestion(std::string issue, AuthoredQuestionSelection authored,
                     std::optional<std::uint64_t> parent, bool recalls)
        : issue_id(std::move(issue)), selection(std::move(authored)),
          parent_act_sequence(parent), recalls_concession(recalls) {}

    friend bool operator==(const GroundedQuestion&, const GroundedQuestion&) = default;
};

struct BenchAct final {
    BenchActKind kind{BenchActKind::Question};
    std::string seat_id;
    std::optional<GroundedQuestion> question;
    std::string rendered_utterance;

    friend bool operator==(const BenchAct&, const BenchAct&) = default;
};

struct OralArgumentEvent final {
    std::uint64_t sequence{};
    std::optional<CounselAnswer> counsel;
    BenchAct bench;

    friend bool operator==(const OralArgumentEvent&, const OralArgumentEvent&) = default;
};

struct ArgumentConcession final {
    std::uint64_t event_sequence{};
    std::string issue_id;
    std::string text;

    friend bool operator==(const ArgumentConcession&, const ArgumentConcession&) = default;
};

enum class TranscriptSpeaker {
    Counsel,
    Bench,
};

struct OralArgumentTranscriptEntry final {
    std::uint64_t event_sequence{};
    TranscriptSpeaker speaker{TranscriptSpeaker::Bench};
    std::string seat_id;
    std::string utterance;
    std::optional<BenchActKind> bench_act_kind;

    friend bool operator==(const OralArgumentTranscriptEntry&,
                           const OralArgumentTranscriptEntry&) = default;
};

struct OralArgumentState final {
    OralArgumentPhase phase{OralArgumentPhase::NotStarted};
    std::chrono::seconds principal_remaining{};
    std::chrono::seconds rebuttal_remaining{};
    std::uint64_t next_event_sequence{1};
    std::optional<std::string> last_seat_id;
    std::uint32_t follow_up_depth{};
    std::vector<ArgumentConcession> concessions;
    std::vector<OralArgumentTranscriptEntry> transcript;
    std::vector<OralArgumentEvent> journal;
    std::string behavior_definition_digest;
    std::string grounding_digest;
    std::string legal_state_digest;
    std::string authored_disposition_id;
    std::optional<CanonicalOralArgumentContract> canonical_contract;

    friend bool operator==(const OralArgumentState&, const OralArgumentState&) = default;
};

} // namespace appellate::model
