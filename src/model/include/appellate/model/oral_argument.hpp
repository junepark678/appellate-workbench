#pragma once

#include "appellate/model/judge_profile.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace appellate::model {

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

struct GroundedQuestion final {
    std::string issue_id;
    std::string prompt;
    std::vector<ArgumentGroundingRef> grounding;
    std::optional<std::uint64_t> parent_act_sequence;
    bool recalls_concession{};

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

    friend bool operator==(const OralArgumentState&, const OralArgumentState&) = default;
};

} // namespace appellate::model
