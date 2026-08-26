#include "appellate/engine/oral_argument_engine.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace appellate::engine {
namespace {

constexpr std::size_t maximum_bench_seats = 32;
constexpr std::size_t maximum_compatibility_roles = 8;
constexpr std::size_t maximum_compatible_jurisdictions = 64;
constexpr std::size_t maximum_issue_focus_entries = 256;
constexpr std::size_t maximum_phrases = 8;
constexpr std::size_t maximum_issues = 256;
constexpr std::size_t maximum_grounding_per_issue = 64;
constexpr std::size_t maximum_prompts_per_issue = 32;
constexpr std::size_t maximum_topics_per_issue = 8;
constexpr std::size_t maximum_citations = 32;
constexpr std::size_t maximum_events = 4'096;
constexpr std::size_t maximum_transcript_entries = maximum_events * 2;
constexpr std::size_t maximum_canonical_issue_bindings = 64;
constexpr std::size_t maximum_canonical_questions = 128;
constexpr std::size_t maximum_canonical_questions_per_issue = 16;
constexpr std::size_t maximum_canonical_grounding_per_question = 16;
constexpr std::size_t maximum_canonical_grounding =
    maximum_canonical_questions * maximum_canonical_grounding_per_question;
constexpr std::size_t maximum_canonical_events = 64;
constexpr std::size_t maximum_canonical_transcript_entries = maximum_canonical_events * 2;
constexpr std::size_t maximum_prompt_length = 512;
constexpr std::size_t maximum_answer_length = 16 * 1'024;
constexpr std::size_t maximum_rendered_utterance_length = 32 * 1'024;
constexpr std::size_t maximum_namespaced_id_length = 160;
constexpr std::uint32_t maximum_document_pages = 10'000;
constexpr std::chrono::seconds maximum_argument_time{24 * 60 * 60};

[[nodiscard]] auto fail(ErrorCode code, std::string message) -> std::unexpected<Error> {
    return std::unexpected(Error{code, std::move(message)});
}

[[nodiscard]] bool boundedText(std::string_view value, std::size_t maximum = 256) {
    return !value.empty() && value.size() <= maximum;
}

[[nodiscard]] bool unitInterval(double value) {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

[[nodiscard]] bool namespacedId(std::string_view value) {
    if (value.size() < 3 || value.size() > maximum_namespaced_id_length ||
        value.front() == '.' || value.front() == '-' || value.back() == '.' ||
        value.back() == '-') {
        return false;
    }
    bool has_separator = false;
    bool previous_separator = false;
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        const bool alphanumeric = (character >= 'a' && character <= 'z') ||
                                  (character >= '0' && character <= '9');
        const bool separator = character == '.' || character == '-';
        if ((!alphanumeric && !separator) || (separator && previous_separator)) {
            return false;
        }
        has_separator = has_separator || separator;
        previous_separator = separator;
    }
    return has_separator;
}

[[nodiscard]] bool lowercaseSha256(std::string_view value) {
    return value.size() == 64 && std::ranges::all_of(value, [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

[[nodiscard]] bool canonicalDate(std::string_view value) {
    if (value.size() != 10 || value[4] != '-' || value[7] != '-') {
        return false;
    }
    const auto digit = [](char character) { return character >= '0' && character <= '9'; };
    if (!std::ranges::all_of(value.substr(0, 4), digit) ||
        !std::ranges::all_of(value.substr(5, 2), digit) ||
        !std::ranges::all_of(value.substr(8, 2), digit)) {
        return false;
    }
    const auto number = [&](std::size_t offset, std::size_t count) {
        unsigned result = 0;
        for (std::size_t index = offset; index < offset + count; ++index) {
            result = result * 10U + static_cast<unsigned>(value[index] - '0');
        }
        return result;
    };
    const auto date = std::chrono::year_month_day{
        std::chrono::year{static_cast<int>(number(0, 4))},
        std::chrono::month{number(5, 2)}, std::chrono::day{number(8, 2)}};
    return date.ok();
}

[[nodiscard]] bool validUtf8(std::string_view value) {
    std::size_t index = 0;
    while (index < value.size()) {
        const auto lead = static_cast<std::uint8_t>(value[index]);
        if (lead <= 0x7fU) {
            ++index;
            continue;
        }
        std::size_t continuation_count = 0;
        std::uint32_t code_point = 0;
        std::uint32_t minimum = 0;
        if ((lead & 0xe0U) == 0xc0U) {
            continuation_count = 1;
            code_point = lead & 0x1fU;
            minimum = 0x80U;
        } else if ((lead & 0xf0U) == 0xe0U) {
            continuation_count = 2;
            code_point = lead & 0x0fU;
            minimum = 0x800U;
        } else if ((lead & 0xf8U) == 0xf0U) {
            continuation_count = 3;
            code_point = lead & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (value.size() - index - 1 < continuation_count) {
            return false;
        }
        for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
            const auto continuation = static_cast<std::uint8_t>(value[index + offset]);
            if ((continuation & 0xc0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (continuation & 0x3fU);
        }
        if (code_point < minimum || code_point > 0x10ffffU ||
            (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            return false;
        }
        index += continuation_count + 1;
    }
    return true;
}

[[nodiscard]] constexpr bool asciiWhitespace(unsigned char character) noexcept {
    return character == static_cast<unsigned char>(' ') || character == '\t' ||
           character == '\n' || character == '\r' || character == '\f' || character == '\v';
}

[[nodiscard]] constexpr bool unicodeWhitespace(char32_t scalar) noexcept {
    return (scalar >= U'\t' && scalar <= U'\r') || scalar == U' ' || scalar == U'\u0085' ||
           scalar == U'\u00a0' || scalar == U'\u1680' ||
           (scalar >= U'\u2000' && scalar <= U'\u200a') || scalar == U'\u2028' ||
           scalar == U'\u2029' || scalar == U'\u202f' || scalar == U'\u205f' ||
           scalar == U'\u3000';
}

// Call only after strict UTF-8 validation. This recognizes the Unicode White_Space property so
// an authored question cannot be visually empty while still passing a byte-level check.
[[nodiscard]] bool hasNonWhitespaceScalar(std::string_view value) {
    for (std::size_t index = 0; index < value.size();) {
        const auto lead = static_cast<std::uint8_t>(value[index]);
        std::size_t width = 1;
        char32_t scalar = lead;
        if ((lead & 0xe0U) == 0xc0U) {
            width = 2;
            scalar = lead & 0x1fU;
        } else if ((lead & 0xf0U) == 0xe0U) {
            width = 3;
            scalar = lead & 0x0fU;
        } else if ((lead & 0xf8U) == 0xf0U) {
            width = 4;
            scalar = lead & 0x07U;
        }
        for (std::size_t offset = 1; offset < width; ++offset) {
            scalar = (scalar << 6U) |
                     (static_cast<std::uint8_t>(value[index + offset]) & 0x3fU);
        }
        if (!unicodeWhitespace(scalar)) {
            return true;
        }
        index += width;
    }
    return false;
}

[[nodiscard]] bool validAuthoredPrompt(std::string_view value) {
    return model::isCanonicalAuthorityText(value, maximum_prompt_length) &&
           !asciiWhitespace(static_cast<unsigned char>(value.front())) &&
           !asciiWhitespace(static_cast<unsigned char>(value.back())) &&
           hasNonWhitespaceScalar(value);
}

[[nodiscard]] bool validEnum(model::CourtRole value) {
    switch (value) {
    case model::CourtRole::District:
    case model::CourtRole::Appellate:
        return true;
    }
    return false;
}

[[nodiscard]] bool validEnum(model::ProfileClass value) {
    switch (value) {
    case model::ProfileClass::FictionalComposite:
        return true;
    }
    return false;
}

[[nodiscard]] bool validEnum(model::VoiceRegister value) {
    switch (value) {
    case model::VoiceRegister::Plain:
    case model::VoiceRegister::Formal:
    case model::VoiceRegister::Technical:
        return true;
    }
    return false;
}

[[nodiscard]] bool validEnum(model::VoiceCadence value) {
    switch (value) {
    case model::VoiceCadence::Clipped:
    case model::VoiceCadence::Measured:
    case model::VoiceCadence::Expansive:
        return true;
    }
    return false;
}

[[nodiscard]] bool validEnum(model::GroundingKind value) {
    switch (value) {
    case model::GroundingKind::Authority:
    case model::GroundingKind::BriefPassage:
    case model::GroundingKind::RecordPage:
        return true;
    }
    return false;
}

[[nodiscard]] bool validEnum(model::OralArgumentMode value) {
    switch (value) {
    case model::OralArgumentMode::ActualRecord:
    case model::OralArgumentMode::CounterfactualTraining:
        return true;
    }
    return false;
}

[[nodiscard]] bool validEnum(model::ArgumentFocusTopic value) {
    return !model::argumentFocusTopicId(value).empty();
}

[[nodiscard]] bool validEnum(model::AuthorityType value) {
    switch (value) {
    case model::AuthorityType::Constitution:
    case model::AuthorityType::Statute:
    case model::AuthorityType::Rule:
    case model::AuthorityType::Regulation:
    case model::AuthorityType::Case:
    case model::AuthorityType::Order:
    case model::AuthorityType::AdministrativeDecision:
    case model::AuthorityType::Other:
        return true;
    }
    return false;
}

[[nodiscard]] bool validEnum(model::PrecedentialStatus value) {
    switch (value) {
    case model::PrecedentialStatus::NotApplicable:
    case model::PrecedentialStatus::Precedential:
    case model::PrecedentialStatus::Nonprecedential:
        return true;
    }
    return false;
}

[[nodiscard]] bool validEnum(model::QuestionFraming value) {
    switch (value) {
    case model::QuestionFraming::Direct:
    case model::QuestionFraming::Socratic:
    case model::QuestionFraming::Narrative:
        return true;
    }
    return false;
}

[[nodiscard]] bool validEnum(model::CounselAddress value) {
    switch (value) {
    case model::CounselAddress::Counsel:
    case model::CounselAddress::Advocate:
        return true;
    }
    return false;
}

[[nodiscard]] bool validEnum(model::OralArgumentPhase value) {
    switch (value) {
    case model::OralArgumentPhase::NotStarted:
    case model::OralArgumentPhase::Principal:
    case model::OralArgumentPhase::Rebuttal:
    case model::OralArgumentPhase::Complete:
        return true;
    }
    return false;
}

[[nodiscard]] bool validEnum(model::CounselActKind value) {
    switch (value) {
    case model::CounselActKind::Answer:
    case model::CounselActKind::Concession:
    case model::CounselActKind::RecordClaim:
        return true;
    }
    return false;
}

[[nodiscard]] bool validEnum(model::BenchActKind value) {
    switch (value) {
    case model::BenchActKind::Question:
    case model::BenchActKind::Interruption:
    case model::BenchActKind::FollowUp:
    case model::BenchActKind::Hypothetical:
    case model::BenchActKind::RecordPinDemand:
    case model::BenchActKind::ClarificationRequest:
    case model::BenchActKind::TimeExpired:
        return true;
    }
    return false;
}

[[nodiscard]] bool questionActKind(model::BenchActKind value) {
    switch (value) {
    case model::BenchActKind::Question:
    case model::BenchActKind::Interruption:
    case model::BenchActKind::FollowUp:
    case model::BenchActKind::Hypothetical:
    case model::BenchActKind::RecordPinDemand:
    case model::BenchActKind::ClarificationRequest:
        return true;
    case model::BenchActKind::TimeExpired:
        return false;
    }
    return false;
}

template <typename Range, typename Projection>
[[nodiscard]] bool hasUniqueValues(const Range& range, Projection projection) {
    using Reference = decltype(*range.begin());
    using Key = std::remove_cvref_t<std::invoke_result_t<Projection, Reference>>;
    std::unordered_set<Key> seen;
    seen.reserve(range.size());
    for (const auto& item : range) {
        if (!seen.emplace(std::invoke(projection, item)).second) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool validPhrases(const std::vector<std::string>& phrases) {
    return !phrases.empty() && phrases.size() <= maximum_phrases &&
           hasUniqueValues(
               phrases, [](const std::string& phrase) -> const std::string& { return phrase; }) &&
           std::ranges::all_of(phrases, [](const std::string& phrase) {
               return boundedText(phrase, 128) && validUtf8(phrase) && phrase.front() != ' ' &&
                      phrase.back() != ' ' &&
                      std::ranges::none_of(phrase, [](unsigned char character) {
                          return character < 0x20U || character == 0x7fU || character == '{' ||
                                 character == '}';
                      });
           });
}

[[nodiscard]] bool compatible(const model::JudgeProfile& profile,
                              const model::BenchConfiguration& bench) {
    return std::ranges::find(profile.compatibility.court_roles, bench.court_role) !=
               profile.compatibility.court_roles.end() &&
           std::ranges::find(profile.compatibility.jurisdiction_ids, bench.jurisdiction_id) !=
               profile.compatibility.jurisdiction_ids.end();
}

[[nodiscard]] bool validStyle(const model::JudgeProfile& profile) {
    const auto& compatibility = profile.compatibility;
    const auto& style = profile.interaction;
    const auto& voice = profile.voice;
    return boundedText(profile.id) && validUtf8(profile.id) && boundedText(profile.display_name) &&
           validUtf8(profile.display_name) && validEnum(profile.profile_class) &&
           profile.profile_class == model::ProfileClass::FictionalComposite &&
           !compatibility.court_roles.empty() &&
           compatibility.court_roles.size() <= maximum_compatibility_roles &&
           hasUniqueValues(compatibility.court_roles, [](model::CourtRole role) { return role; }) &&
           std::ranges::all_of(compatibility.court_roles,
                               [](model::CourtRole role) { return validEnum(role); }) &&
           !compatibility.jurisdiction_ids.empty() &&
           compatibility.jurisdiction_ids.size() <= maximum_compatible_jurisdictions &&
           hasUniqueValues(compatibility.jurisdiction_ids,
                           [](const std::string& value) -> const std::string& { return value; }) &&
           std::ranges::all_of(compatibility.jurisdiction_ids,
                               [](const std::string& value) { return boundedText(value); }) &&
           unitInterval(style.directness) && unitInterval(style.formality) &&
           unitInterval(style.question_length) && unitInterval(style.interruption_frequency) &&
           unitInterval(style.follow_up_depth) && unitInterval(style.hypothetical_frequency) &&
           unitInterval(style.concession_recall) && unitInterval(style.record_pin_demand) &&
           unitInterval(style.time_strictness) && !style.issue_focus.empty() &&
           style.issue_focus.size() <= maximum_issue_focus_entries &&
           hasUniqueValues(style.issue_focus, &model::IssueFocus::topic_id) &&
           std::ranges::all_of(style.issue_focus,
                               [](const model::IssueFocus& focus) {
                                   return boundedText(focus.topic_id) && unitInterval(focus.weight);
                               }) &&
           validEnum(voice.register_style) && validEnum(voice.cadence) &&
           validEnum(voice.question_framing) && validEnum(voice.address_convention) &&
           unitInterval(voice.verbosity) && unitInterval(voice.sentence_complexity) &&
           validPhrases(voice.question_phrases) && validPhrases(voice.interruption_phrases) &&
           validPhrases(voice.clarification_phrases);
}

[[nodiscard]] auto validateBench(const model::BenchConfiguration& bench)
    -> std::expected<void, Error> {
    if (!boundedText(bench.jurisdiction_id) || !validEnum(bench.court_role) ||
        bench.seats.empty() || bench.seats.size() > maximum_bench_seats ||
        !boundedText(bench.presiding_seat_id) ||
        !hasUniqueValues(bench.seats, &model::BenchSeat::id)) {
        return fail(ErrorCode::InvalidDefinition, "invalid bench configuration");
    }

    bool has_presiding_seat = false;
    for (const auto& seat : bench.seats) {
        has_presiding_seat = has_presiding_seat || seat.id == bench.presiding_seat_id;
        if (!boundedText(seat.id) || !validStyle(seat.profile) ||
            !compatible(seat.profile, bench)) {
            return fail(ErrorCode::InvalidDefinition,
                        "bench seat is incompatible or has an invalid structured style");
        }
    }
    if (!has_presiding_seat) {
        return fail(ErrorCode::InvalidDefinition, "presiding seat is not on the bench");
    }
    return {};
}

[[nodiscard]] bool validGroundingRef(const model::ArgumentGroundingRef& reference) {
    if (!validEnum(reference.kind) || !boundedText(reference.id)) {
        return false;
    }
    if (reference.kind == model::GroundingKind::RecordPage) {
        return reference.page_number.has_value() && *reference.page_number > 0;
    }
    return !reference.page_number.has_value() || *reference.page_number > 0;
}

[[nodiscard]] auto validateGrounding(const model::ArgumentGrounding& grounding)
    -> std::expected<void, Error> {
    if (grounding.issues.empty() || grounding.issues.size() > maximum_issues ||
        !hasUniqueValues(grounding.issues, &model::ArgumentIssue::id)) {
        return fail(ErrorCode::InvalidDefinition, "argument requires bounded, unique issues");
    }
    for (const auto& issue : grounding.issues) {
        if (!boundedText(issue.id) || !boundedText(issue.label, maximum_prompt_length) ||
            issue.grounding.empty() || issue.grounding.size() > maximum_grounding_per_issue ||
            issue.question_prompts.empty() ||
            issue.question_prompts.size() > maximum_prompts_per_issue ||
            issue.hypothetical_prompts.size() > maximum_prompts_per_issue ||
            !hasUniqueValues(issue.grounding, &model::ArgumentGroundingRef::id) ||
            !std::ranges::all_of(issue.grounding, validGroundingRef) ||
            !std::ranges::all_of(issue.question_prompts,
                                 [](const std::string& prompt) {
                                     return boundedText(prompt, maximum_prompt_length);
                                 }) ||
            !std::ranges::all_of(issue.hypothetical_prompts, [](const std::string& prompt) {
                return boundedText(prompt, maximum_prompt_length);
            })) {
            return fail(ErrorCode::InvalidDefinition,
                        "argument issue, prompt, or grounding is invalid");
        }
    }
    return {};
}

[[nodiscard]] std::string_view
authoredGroundingId(const model::AuthoredArgumentGrounding& grounding) {
    return std::visit([](const auto& reference) -> std::string_view {
        return reference.grounding_id;
    }, grounding);
}

[[nodiscard]] bool validAuthority(const model::AuthorityRef& authority) {
    if (!namespacedId(authority.id.value) ||
        !model::isCanonicalAuthorityText(authority.citation, 4'096) ||
        !canonicalDate(authority.source_version) ||
        !model::isCanonicalAuthorityText(authority.proposition, 4'096) ||
        !authority.provenance.has_value()) {
        return false;
    }
    const auto& provenance = *authority.provenance;
    return validEnum(provenance.type) && namespacedId(provenance.jurisdiction_id) &&
           namespacedId(provenance.issuing_body_id) &&
           validEnum(provenance.precedential_status) && canonicalDate(provenance.checked_on) &&
           model::authorityVerificationNotBeforeSource(authority.source_version,
                                                       provenance.checked_on) &&
           model::isCanonicalAuthorityText(provenance.locator, 1'024) &&
           model::isCanonicalAuthoritySourceUrl(provenance.source_url);
}

[[nodiscard]] bool
validAuthoredGrounding(const model::AuthoredArgumentGrounding& grounding) {
    return std::visit(
        [](const auto& reference) {
            using Reference = std::remove_cvref_t<decltype(reference)>;
            if (!namespacedId(reference.grounding_id)) {
                return false;
            }
            if constexpr (std::is_same_v<Reference, model::AuthorityArgumentGrounding>) {
                return validAuthority(reference.authority);
            } else if constexpr (std::is_same_v<Reference,
                                                model::BriefPageArgumentGrounding>) {
                return namespacedId(reference.record_entry_id) && reference.page_number > 0 &&
                       reference.page_number <= maximum_document_pages &&
                       lowercaseSha256(reference.asset_sha256);
            } else {
                return namespacedId(reference.record_anchor_id) &&
                       namespacedId(reference.record_entry_id) && reference.page_number > 0 &&
                       reference.page_number <= maximum_document_pages &&
                       lowercaseSha256(reference.asset_sha256) &&
                       (!reference.citation_label.has_value() ||
                        model::isCanonicalAuthorityText(*reference.citation_label, 120));
            }
        },
        grounding);
}

[[nodiscard]] auto validateQuestionBank(const model::AuthoredQuestionBank& bank)
    -> std::expected<void, Error> {
    if (!namespacedId(bank.case_id.value) || !namespacedId(bank.argument_configuration_id) ||
        !validEnum(bank.mode) ||
        (!bank.grounding_digest.empty() && !lowercaseSha256(bank.grounding_digest)) ||
        bank.issue_topics.empty() ||
        bank.issue_topics.size() > maximum_canonical_issue_bindings ||
        bank.questions.empty() || bank.questions.size() > maximum_canonical_questions ||
        !hasUniqueValues(bank.issue_topics, &model::ArgumentIssueTopics::issue_id) ||
        !hasUniqueValues(bank.questions, &model::AuthoredArgumentQuestion::id)) {
        return fail(ErrorCode::InvalidDefinition, "invalid authored question-bank shape");
    }

    std::unordered_map<std::string, const model::ArgumentIssueTopics*> bindings;
    bindings.reserve(bank.issue_topics.size());
    for (const auto& binding : bank.issue_topics) {
        if (!namespacedId(binding.issue_id) || binding.topics.empty() ||
            binding.topics.size() > maximum_topics_per_issue ||
            !hasUniqueValues(binding.topics,
                             [](model::ArgumentFocusTopic topic) { return topic; }) ||
            !std::ranges::all_of(binding.topics,
                                 [](model::ArgumentFocusTopic topic) {
                                     return validEnum(topic);
                                 })) {
            return fail(ErrorCode::InvalidDefinition, "invalid authored issue-topic binding");
        }
        bindings.emplace(binding.issue_id, &binding);
    }

    std::unordered_map<std::string, std::size_t> questions_per_issue;
    questions_per_issue.reserve(bindings.size());
    std::unordered_set<std::string> grounding_ids;
    grounding_ids.reserve(maximum_canonical_grounding);
    std::size_t grounding_count = 0;
    for (const auto& question : bank.questions) {
        const auto binding = bindings.find(question.issue_id);
        if (!namespacedId(question.id) || binding == bindings.end() ||
            !validEnum(question.topic) ||
            std::ranges::find(binding->second->topics, question.topic) ==
                binding->second->topics.end() ||
            !validAuthoredPrompt(question.prompt) ||
            question.grounding.empty() ||
            question.grounding.size() > maximum_canonical_grounding_per_question ||
            grounding_count > maximum_canonical_grounding - question.grounding.size()) {
            return fail(ErrorCode::InvalidDefinition, "invalid authored grounded question");
        }
        auto& issue_count = questions_per_issue[question.issue_id];
        ++issue_count;
        if (issue_count > maximum_canonical_questions_per_issue) {
            return fail(ErrorCode::InvalidDefinition,
                        "authored issue exceeds its question limit");
        }
        grounding_count += question.grounding.size();
        for (const auto& grounding : question.grounding) {
            if (!validAuthoredGrounding(grounding) ||
                !grounding_ids.emplace(authoredGroundingId(grounding)).second) {
                return fail(ErrorCode::InvalidDefinition,
                            "authored grounding is invalid or ambiguously identified");
            }
        }
    }
    if (std::ranges::any_of(bank.issue_topics, [&](const auto& binding) {
            return !questions_per_issue.contains(binding.issue_id);
        })) {
        return fail(ErrorCode::InvalidDefinition,
                    "every permitted issue requires an authored grounded question");
    }
    if (std::ranges::any_of(bank.issue_topics, [&](const auto& binding) {
            return std::ranges::any_of(binding.topics, [&](model::ArgumentFocusTopic topic) {
                return std::ranges::none_of(bank.questions, [&](const auto& question) {
                    return question.issue_id == binding.issue_id && question.topic == topic;
                });
            });
        })) {
        return fail(ErrorCode::InvalidDefinition,
                    "every declared issue-topic pair requires an authored question");
    }
    return {};
}

[[nodiscard]] auto validateCanonicalBench(const model::BenchConfiguration& bench,
                                          const model::AuthoredQuestionBank& bank)
    -> std::expected<void, Error> {
    if (const auto valid = validateBench(bench); !valid) {
        return valid;
    }
    std::unordered_set<model::ArgumentFocusTopic> available_topics;
    for (const auto& binding : bank.issue_topics) {
        available_topics.insert(binding.topics.begin(), binding.topics.end());
    }
    for (const auto& seat : bench.seats) {
        bool intersects = false;
        for (const auto& focus : seat.profile.interaction.issue_focus) {
            const auto topic = model::argumentFocusTopicFromId(focus.topic_id);
            if (!topic.has_value()) {
                return fail(ErrorCode::InvalidDefinition,
                            "canonical argument bench uses a case-specific or unknown focus topic");
            }
            intersects = intersects ||
                         (focus.weight > 0.0 && available_topics.contains(*topic));
        }
        if (!intersects) {
            return fail(ErrorCode::InvalidDefinition,
                        "canonical argument bench has no positive focus in the question bank");
        }
    }
    return {};
}

[[nodiscard]] auto validateConfiguration(const model::OralArgumentConfiguration& configuration)
    -> std::expected<void, Error> {
    if (configuration.principal_time <= std::chrono::seconds::zero() ||
        configuration.principal_time > maximum_argument_time ||
        configuration.rebuttal_time < std::chrono::seconds::zero() ||
        configuration.rebuttal_time > maximum_argument_time ||
        !unitInterval(configuration.classification_confidence_threshold) ||
        configuration.maximum_follow_up_depth > 16 ||
        !lowercaseSha256(configuration.behavior_definition_digest) ||
        !lowercaseSha256(configuration.grounding_digest) ||
        !boundedText(configuration.legal_state_digest) ||
        !boundedText(configuration.authored_disposition_id)) {
        return fail(ErrorCode::InvalidDefinition, "invalid oral-argument configuration");
    }
    return {};
}

class Sha256 final {
  public:
    void addByte(std::uint8_t value) {
        block_[block_size_] = value;
        ++block_size_;
        ++byte_count_;
        if (block_size_ == block_.size()) {
            transform();
            block_size_ = 0;
        }
    }

    void addUint64(std::uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8) {
            addByte(static_cast<std::uint8_t>((value >> shift) & 0xffU));
        }
    }

    void addText(std::string_view value) {
        addUint64(static_cast<std::uint64_t>(value.size()));
        for (const char character : value) {
            addByte(static_cast<std::uint8_t>(static_cast<unsigned char>(character)));
        }
    }

    void addDouble(double value) { addUint64(std::bit_cast<std::uint64_t>(value)); }

    [[nodiscard]] std::string finish() {
        const auto bit_count = byte_count_ * 8U;
        addByte(0x80U);
        while (block_size_ != 56) {
            addByte(0U);
        }
        addUint64(bit_count);

        constexpr std::string_view hex = "0123456789abcdef";
        std::string result;
        result.reserve(64);
        for (const auto word : state_) {
            for (int shift = 28; shift >= 0; shift -= 4) {
                const auto index = static_cast<std::size_t>((word >> shift) & 0x0fU);
                result.push_back(hex[index]);
            }
        }
        return result;
    }

  private:
    void transform() {
        constexpr std::array<std::uint32_t, 64> constants{
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
            0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
            0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
            0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
            0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
            0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
            0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
            0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
            0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
            0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
            0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
        };
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            const auto offset = index * 4;
            words[index] = (static_cast<std::uint32_t>(block_[offset]) << 24U) |
                           (static_cast<std::uint32_t>(block_[offset + 1]) << 16U) |
                           (static_cast<std::uint32_t>(block_[offset + 2]) << 8U) |
                           static_cast<std::uint32_t>(block_[offset + 3]);
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            const auto first = std::rotr(words[index - 15], 7) ^ std::rotr(words[index - 15], 18) ^
                               (words[index - 15] >> 3U);
            const auto second = std::rotr(words[index - 2], 17) ^ std::rotr(words[index - 2], 19) ^
                                (words[index - 2] >> 10U);
            words[index] = words[index - 16] + first + words[index - 7] + second;
        }

        auto a = state_[0];
        auto b = state_[1];
        auto c = state_[2];
        auto d = state_[3];
        auto e = state_[4];
        auto f = state_[5];
        auto g = state_[6];
        auto h = state_[7];
        for (std::size_t index = 0; index < words.size(); ++index) {
            const auto sum_one = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const auto choice = (e & f) ^ ((~e) & g);
            const auto temporary_one = h + sum_one + choice + constants[index] + words[index];
            const auto sum_zero = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temporary_two = sum_zero + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary_one;
            d = c;
            c = b;
            b = a;
            a = temporary_one + temporary_two;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    };
    std::array<std::uint8_t, 64> block_{};
    std::size_t block_size_{};
    std::uint64_t byte_count_{};
};

template <typename Enum> void addEnum(Sha256& digest, Enum value) {
    using Underlying = std::underlying_type_t<Enum>;
    digest.addUint64(static_cast<std::uint64_t>(static_cast<Underlying>(value)));
}

void addStrings(Sha256& digest, const std::vector<std::string>& values) {
    digest.addUint64(static_cast<std::uint64_t>(values.size()));
    for (const auto& value : values) {
        digest.addText(value);
    }
}

[[nodiscard]] std::string digestBehaviorUnchecked(const model::BenchConfiguration& bench) {
    Sha256 digest;
    digest.addText("appellate.behavior-definition.v2");
    digest.addText(bench.jurisdiction_id);
    addEnum(digest, bench.court_role);
    digest.addUint64(static_cast<std::uint64_t>(bench.seats.size()));
    for (const auto& seat : bench.seats) {
        digest.addText(seat.id);
        const auto& profile = seat.profile;
        addEnum(digest, profile.profile_class);
        digest.addUint64(static_cast<std::uint64_t>(profile.compatibility.court_roles.size()));
        for (const auto role : profile.compatibility.court_roles) {
            addEnum(digest, role);
        }
        addStrings(digest, profile.compatibility.jurisdiction_ids);

        const auto& interaction = profile.interaction;
        digest.addDouble(interaction.directness);
        digest.addDouble(interaction.formality);
        digest.addDouble(interaction.question_length);
        digest.addDouble(interaction.interruption_frequency);
        digest.addDouble(interaction.follow_up_depth);
        digest.addDouble(interaction.hypothetical_frequency);
        digest.addDouble(interaction.concession_recall);
        digest.addDouble(interaction.record_pin_demand);
        digest.addDouble(interaction.time_strictness);
        digest.addUint64(static_cast<std::uint64_t>(interaction.issue_focus.size()));
        for (const auto& focus : interaction.issue_focus) {
            digest.addText(focus.topic_id);
            digest.addDouble(focus.weight);
        }

        addEnum(digest, profile.voice.register_style);
        addEnum(digest, profile.voice.cadence);
        addEnum(digest, profile.voice.question_framing);
        addEnum(digest, profile.voice.address_convention);
        digest.addDouble(profile.voice.verbosity);
        digest.addDouble(profile.voice.sentence_complexity);
        addStrings(digest, profile.voice.question_phrases);
        addStrings(digest, profile.voice.interruption_phrases);
        addStrings(digest, profile.voice.clarification_phrases);
    }
    digest.addText(bench.presiding_seat_id);
    return digest.finish();
}

void addGroundingRef(Sha256& digest, const model::ArgumentGroundingRef& reference) {
    addEnum(digest, reference.kind);
    digest.addText(reference.id);
    digest.addUint64(reference.page_number.has_value() ? 1U : 0U);
    if (reference.page_number.has_value()) {
        digest.addUint64(*reference.page_number);
    }
}

[[nodiscard]] std::string digestGroundingUnchecked(const model::ArgumentGrounding& grounding) {
    Sha256 digest;
    digest.addText("appellate.argument-grounding.v1");
    digest.addUint64(static_cast<std::uint64_t>(grounding.issues.size()));
    for (const auto& issue : grounding.issues) {
        digest.addText(issue.id);
        digest.addText(issue.label);
        digest.addUint64(static_cast<std::uint64_t>(issue.grounding.size()));
        for (const auto& reference : issue.grounding) {
            addGroundingRef(digest, reference);
        }
        addStrings(digest, issue.question_prompts);
        addStrings(digest, issue.hypothetical_prompts);
    }
    return digest.finish();
}

[[nodiscard]] std::string_view modeName(model::OralArgumentMode mode) {
    switch (mode) {
    case model::OralArgumentMode::ActualRecord:
        return "actual_record";
    case model::OralArgumentMode::CounterfactualTraining:
        return "counterfactual_training";
    }
    return {};
}

[[nodiscard]] std::string_view authorityTypeName(model::AuthorityType type) {
    switch (type) {
    case model::AuthorityType::Constitution:
        return "constitution";
    case model::AuthorityType::Statute:
        return "statute";
    case model::AuthorityType::Rule:
        return "rule";
    case model::AuthorityType::Regulation:
        return "regulation";
    case model::AuthorityType::Case:
        return "case";
    case model::AuthorityType::Order:
        return "order";
    case model::AuthorityType::AdministrativeDecision:
        return "administrative_decision";
    case model::AuthorityType::Other:
        return "other";
    }
    return {};
}

[[nodiscard]] std::string_view precedentialStatusName(model::PrecedentialStatus status) {
    switch (status) {
    case model::PrecedentialStatus::NotApplicable:
        return "not_applicable";
    case model::PrecedentialStatus::Precedential:
        return "precedential";
    case model::PrecedentialStatus::Nonprecedential:
        return "nonprecedential";
    }
    return {};
}

void addAuthority(Sha256& digest, const model::AuthorityRef& authority) {
    digest.addText(authority.id.value);
    digest.addText(authority.citation);
    digest.addText(authority.source_version);
    digest.addText(authority.proposition);
    digest.addUint64(authority.provenance.has_value() ? 1U : 0U);
    if (!authority.provenance.has_value()) {
        return;
    }
    const auto& provenance = *authority.provenance;
    digest.addText(authorityTypeName(provenance.type));
    digest.addText(provenance.jurisdiction_id);
    digest.addText(provenance.issuing_body_id);
    digest.addText(precedentialStatusName(provenance.precedential_status));
    digest.addUint64(provenance.official_source ? 1U : 0U);
    digest.addText(provenance.checked_on);
    digest.addText(provenance.locator);
    digest.addText(provenance.source_url);
}

void addAuthoredGrounding(Sha256& digest,
                          const model::AuthoredArgumentGrounding& grounding) {
    std::visit(
        [&](const auto& reference) {
            using Reference = std::remove_cvref_t<decltype(reference)>;
            digest.addText(reference.grounding_id);
            if constexpr (std::is_same_v<Reference, model::AuthorityArgumentGrounding>) {
                digest.addText("authority");
                addAuthority(digest, reference.authority);
            } else if constexpr (std::is_same_v<Reference,
                                                model::BriefPageArgumentGrounding>) {
                digest.addText("brief_page");
                digest.addText(reference.record_entry_id);
                digest.addUint64(reference.page_number);
                digest.addText(reference.asset_sha256);
            } else {
                digest.addText("record_page");
                digest.addText(reference.record_anchor_id);
                digest.addText(reference.record_entry_id);
                digest.addUint64(reference.page_number);
                digest.addText(reference.asset_sha256);
                digest.addUint64(reference.citation_label.has_value() ? 1U : 0U);
                if (reference.citation_label.has_value()) {
                    digest.addText(*reference.citation_label);
                }
            }
        },
        grounding);
}

[[nodiscard]] std::string
digestQuestionBankUnchecked(const model::AuthoredQuestionBank& bank) {
    Sha256 digest;
    digest.addText("appellate-workbench-grounded-question-bank-v1");
    digest.addText(bank.case_id.value);
    digest.addText(bank.argument_configuration_id);
    digest.addText(modeName(bank.mode));

    std::vector<const model::ArgumentIssueTopics*> bindings;
    bindings.reserve(bank.issue_topics.size());
    for (const auto& binding : bank.issue_topics) {
        bindings.push_back(&binding);
    }
    std::ranges::sort(bindings, {}, [](const auto* binding) -> const std::string& {
        return binding->issue_id;
    });
    digest.addUint64(static_cast<std::uint64_t>(bindings.size()));
    for (const auto* binding : bindings) {
        digest.addText(binding->issue_id);
        auto topics = binding->topics;
        std::ranges::sort(topics, [](model::ArgumentFocusTopic left,
                                    model::ArgumentFocusTopic right) {
            return model::argumentFocusTopicId(left) < model::argumentFocusTopicId(right);
        });
        digest.addUint64(static_cast<std::uint64_t>(topics.size()));
        for (const auto topic : topics) {
            digest.addText(model::argumentFocusTopicId(topic));
        }
    }

    std::vector<const model::AuthoredArgumentQuestion*> questions;
    questions.reserve(bank.questions.size());
    for (const auto& question : bank.questions) {
        questions.push_back(&question);
    }
    std::ranges::sort(questions, {}, [](const auto* question) -> const std::string& {
        return question->id;
    });
    digest.addUint64(static_cast<std::uint64_t>(questions.size()));
    for (const auto* question : questions) {
        digest.addText(question->id);
        digest.addText(question->issue_id);
        digest.addText(model::argumentFocusTopicId(question->topic));
        digest.addText(question->prompt);
        std::vector<const model::AuthoredArgumentGrounding*> grounding;
        grounding.reserve(question->grounding.size());
        for (const auto& reference : question->grounding) {
            grounding.push_back(&reference);
        }
        std::ranges::sort(grounding, [](const auto* left, const auto* right) {
            return authoredGroundingId(*left) < authoredGroundingId(*right);
        });
        digest.addUint64(static_cast<std::uint64_t>(grounding.size()));
        for (const auto* reference : grounding) {
            addAuthoredGrounding(digest, *reference);
        }
    }
    return digest.finish();
}

[[nodiscard]] std::string canonicalDefinitionDigestUnchecked(
    const model::CanonicalOralArgumentDefinition& definition) {
    Sha256 digest;
    digest.addText("appellate-workbench-canonical-oral-argument-definition-v1");
    digest.addText(definition.question_bank.case_id.value);
    digest.addText(definition.question_bank.argument_configuration_id);
    digest.addText(modeName(definition.question_bank.mode));
    digest.addText(definition.question_bank.grounding_digest);

    const auto& configuration = definition.configuration;
    digest.addUint64(static_cast<std::uint64_t>(configuration.principal_time.count()));
    digest.addUint64(static_cast<std::uint64_t>(configuration.rebuttal_time.count()));
    digest.addDouble(configuration.classification_confidence_threshold);
    digest.addUint64(configuration.maximum_follow_up_depth);
    digest.addText(configuration.behavior_definition_digest);
    digest.addText(configuration.grounding_digest);
    digest.addText(configuration.legal_state_digest);
    digest.addText(configuration.authored_disposition_id);
    return digest.finish();
}

[[nodiscard]] model::CanonicalOralArgumentContract
canonicalContract(const model::CanonicalOralArgumentDefinition& definition) {
    const auto& bank = definition.question_bank;
    return model::CanonicalOralArgumentContract{
        bank.case_id,
        bank.argument_configuration_id,
        bank.mode,
        bank.grounding_digest,
        canonicalDefinitionDigestUnchecked(definition),
    };
}

[[nodiscard]] auto validateCanonicalBoundary(
    const model::CanonicalOralArgumentDefinition& definition) -> std::expected<void, Error> {
    if (const auto valid = validateConfiguration(definition.configuration); !valid) {
        return valid;
    }
    if (const auto valid = validateQuestionBank(definition.question_bank); !valid) {
        return valid;
    }
    if (const auto valid = validateCanonicalBench(definition.bench, definition.question_bank);
        !valid) {
        return valid;
    }
    const auto computed = digestQuestionBankUnchecked(definition.question_bank);
    if (definition.configuration.behavior_definition_digest !=
            digestBehaviorUnchecked(definition.bench) ||
        definition.question_bank.grounding_digest != computed ||
        definition.configuration.grounding_digest != computed) {
        return fail(ErrorCode::InvalidDefinition,
                    "canonical oral-argument definitions do not match their digest pins");
    }
    return {};
}

[[nodiscard]] auto validateBoundary(const model::OralArgumentConfiguration& configuration,
                                    const model::BenchConfiguration& bench,
                                    const model::ArgumentGrounding& grounding)
    -> std::expected<void, Error> {
    if (const auto valid = validateConfiguration(configuration); !valid) {
        return std::unexpected(valid.error());
    }
    if (const auto valid = validateBench(bench); !valid) {
        return std::unexpected(valid.error());
    }
    if (const auto valid = validateGrounding(grounding); !valid) {
        return std::unexpected(valid.error());
    }
    if (configuration.behavior_definition_digest != digestBehaviorUnchecked(bench) ||
        configuration.grounding_digest != digestGroundingUnchecked(grounding)) {
        return fail(ErrorCode::InvalidDefinition,
                    "oral-argument definitions do not match their exact digest pins");
    }
    return {};
}

[[nodiscard]] const model::BenchSeat* seatById(const model::BenchConfiguration& bench,
                                               std::string_view id) {
    const auto found = std::ranges::find(bench.seats, id, &model::BenchSeat::id);
    return found == bench.seats.end() ? nullptr : &*found;
}

[[nodiscard]] const model::ArgumentIssue* issueById(const model::ArgumentGrounding& grounding,
                                                    std::string_view id) {
    const auto found = std::ranges::find(grounding.issues, id, &model::ArgumentIssue::id);
    return found == grounding.issues.end() ? nullptr : &*found;
}

[[nodiscard]] const model::BenchSeat* presidingSeat(const model::BenchConfiguration& bench) {
    return seatById(bench, bench.presiding_seat_id);
}

[[nodiscard]] std::uint32_t allowedFollowUps(const model::OralArgumentConfiguration& configuration,
                                             const model::BenchSeat& seat) {
    return static_cast<std::uint32_t>(
        std::lround(seat.profile.interaction.follow_up_depth *
                    static_cast<double>(configuration.maximum_follow_up_depth)));
}

[[nodiscard]] bool groundingAvailable(const model::ArgumentIssue& issue,
                                      std::string_view reference_id) {
    return std::ranges::find(issue.grounding, reference_id, &model::ArgumentGroundingRef::id) !=
           issue.grounding.end();
}

[[nodiscard]] bool groundingAvailableExact(const model::ArgumentIssue& issue,
                                           const model::ArgumentGroundingRef& reference) {
    const auto found =
        std::ranges::find(issue.grounding, reference.id, &model::ArgumentGroundingRef::id);
    return found != issue.grounding.end() && *found == reference;
}

[[nodiscard]] bool hasRecordPage(const model::ArgumentIssue& issue) {
    return std::ranges::find(issue.grounding, model::GroundingKind::RecordPage,
                             &model::ArgumentGroundingRef::kind) != issue.grounding.end();
}

[[nodiscard]] auto validateCounselAnswer(const model::ArgumentGrounding& grounding,
                                         const model::CounselAnswer& answer)
    -> std::expected<void, Error> {
    if (!validEnum(answer.kind) || !boundedText(answer.text, maximum_answer_length) ||
        !boundedText(answer.issue_id) || !unitInterval(answer.classification_confidence) ||
        answer.elapsed <= std::chrono::seconds::zero() || answer.elapsed > maximum_argument_time ||
        answer.cited_grounding_ids.size() > maximum_citations ||
        !hasUniqueValues(answer.cited_grounding_ids,
                         [](const std::string& value) -> const std::string& { return value; }) ||
        !std::ranges::all_of(answer.cited_grounding_ids,
                             [](const std::string& value) { return boundedText(value); })) {
        return fail(ErrorCode::InvalidCommand, "invalid or unbounded counsel answer");
    }
    const auto* issue = issueById(grounding, answer.issue_id);
    if (issue == nullptr ||
        !std::ranges::all_of(answer.cited_grounding_ids, [&](const std::string& id) {
            return groundingAvailable(*issue, id);
        })) {
        return fail(ErrorCode::InvalidCommand,
                    "answer refers to an unavailable issue or grounding");
    }
    return {};
}

[[nodiscard]] auto validateEventShape(const model::BenchConfiguration& bench,
                                      const model::ArgumentGrounding& grounding,
                                      const model::OralArgumentEvent& event)
    -> std::expected<void, Error> {
    if (event.sequence == 0 || event.sequence > maximum_events || !validEnum(event.bench.kind) ||
        seatById(bench, event.bench.seat_id) == nullptr ||
        !boundedText(event.bench.rendered_utterance, maximum_rendered_utterance_length)) {
        return fail(ErrorCode::InvalidEvent, "oral-argument event shape is invalid");
    }
    if (event.counsel.has_value()) {
        if (const auto valid = validateCounselAnswer(grounding, *event.counsel); !valid) {
            return fail(ErrorCode::InvalidEvent, valid.error().message);
        }
    }

    if (!questionActKind(event.bench.kind)) {
        if (event.bench.question.has_value()) {
            return fail(ErrorCode::InvalidEvent, "non-question act unexpectedly has grounding");
        }
        return {};
    }
    if (!event.bench.question.has_value()) {
        return fail(ErrorCode::InvalidEvent, "bench question is missing grounding");
    }
    const auto& question = *event.bench.question;
    const auto* selection = std::get_if<model::LegacyQuestionSelection>(&question.selection);
    const auto* issue = issueById(grounding, question.issue_id);
    if (selection == nullptr || issue == nullptr ||
        !boundedText(selection->prompt, maximum_prompt_length + 128) ||
        selection->grounding.empty() ||
        selection->grounding.size() > maximum_grounding_per_issue ||
        !hasUniqueValues(selection->grounding, &model::ArgumentGroundingRef::id) ||
        !std::ranges::all_of(selection->grounding,
                             [&](const auto& reference) {
                                 return validGroundingRef(reference) &&
                                        groundingAvailableExact(*issue, reference);
                             }) ||
        (question.parent_act_sequence.has_value() &&
         (*question.parent_act_sequence == 0 || *question.parent_act_sequence >= event.sequence)) ||
        (question.recalls_concession && event.bench.kind != model::BenchActKind::FollowUp)) {
        return fail(ErrorCode::InvalidEvent, "bench question grounding is invalid");
    }
    if (event.bench.kind == model::BenchActKind::RecordPinDemand &&
        !std::ranges::any_of(selection->grounding, [](const auto& reference) {
            return reference.kind == model::GroundingKind::RecordPage;
        })) {
        return fail(ErrorCode::InvalidEvent, "record-pin demand lacks a record page");
    }
    return {};
}

[[nodiscard]] model::OralArgumentState
initialStateUnchecked(const model::OralArgumentConfiguration& configuration) {
    return model::OralArgumentState{
        model::OralArgumentPhase::NotStarted,
        configuration.principal_time,
        configuration.rebuttal_time,
        1,
        std::nullopt,
        0,
        {},
        {},
        {},
        configuration.behavior_definition_digest,
        configuration.grounding_digest,
        configuration.legal_state_digest,
        configuration.authored_disposition_id,
        std::nullopt,
    };
}

[[nodiscard]] model::OralArgumentState
applyAcceptedEvent(const model::OralArgumentConfiguration& configuration,
                   const model::OralArgumentState& state, const model::OralArgumentEvent& event) {
    auto next = state;
    if (event.counsel.has_value()) {
        next.transcript.push_back(model::OralArgumentTranscriptEntry{
            event.sequence,
            model::TranscriptSpeaker::Counsel,
            {},
            event.counsel->text,
            std::nullopt,
        });

        auto& remaining = state.phase == model::OralArgumentPhase::Principal
                              ? next.principal_remaining
                              : next.rebuttal_remaining;
        remaining = std::max(std::chrono::seconds::zero(), remaining - event.counsel->elapsed);
        if (event.bench.kind == model::BenchActKind::TimeExpired) {
            if (state.phase == model::OralArgumentPhase::Principal &&
                next.rebuttal_remaining > std::chrono::seconds::zero()) {
                next.phase = model::OralArgumentPhase::Rebuttal;
            } else {
                next.phase = model::OralArgumentPhase::Complete;
            }
        }
        if (event.bench.kind != model::BenchActKind::TimeExpired &&
            event.counsel->kind == model::CounselActKind::Concession &&
            event.counsel->classification_confidence >=
                configuration.classification_confidence_threshold) {
            next.concessions.push_back(model::ArgumentConcession{
                event.sequence,
                event.counsel->issue_id,
                event.counsel->text,
            });
        }
    } else {
        next.phase = model::OralArgumentPhase::Principal;
    }

    next.transcript.push_back(model::OralArgumentTranscriptEntry{
        event.sequence,
        model::TranscriptSpeaker::Bench,
        event.bench.seat_id,
        event.bench.rendered_utterance,
        event.bench.kind,
    });
    const auto same_seat =
        state.last_seat_id.has_value() && *state.last_seat_id == event.bench.seat_id;
    next.follow_up_depth = event.bench.kind == model::BenchActKind::FollowUp && same_seat
                               ? state.follow_up_depth + 1
                               : 0;
    next.last_seat_id = event.bench.seat_id;
    ++next.next_event_sequence;
    next.journal.push_back(event);
    return next;
}

[[nodiscard]] auto planOpeningUnchecked(const model::BenchConfiguration& bench,
                                        const model::ArgumentGrounding& grounding,
                                        const model::OralArgumentState& state)
    -> std::expected<model::OralArgumentEvent, Error>;

[[nodiscard]] auto decideCounselAnswerUnchecked(
    const model::OralArgumentConfiguration& configuration, const model::BenchConfiguration& bench,
    const model::ArgumentGrounding& grounding, const model::OralArgumentState& state,
    const model::CounselAnswer& answer) -> std::expected<model::OralArgumentEvent, Error>;

[[nodiscard]] auto projectJournal(const model::OralArgumentConfiguration& configuration,
                                  const model::BenchConfiguration& bench,
                                  const model::ArgumentGrounding& grounding,
                                  std::span<const model::OralArgumentEvent> journal)
    -> std::expected<model::OralArgumentState, Error> {
    if (journal.size() > maximum_events) {
        return fail(ErrorCode::InvalidSession, "oral-argument journal exceeds its hard limit");
    }
    auto projected = initialStateUnchecked(configuration);
    for (std::size_t index = 0; index < journal.size(); ++index) {
        const auto& event = journal[index];
        if (const auto valid = validateEventShape(bench, grounding, event); !valid) {
            return fail(ErrorCode::InvalidSession, valid.error().message);
        }
        if (event.sequence != static_cast<std::uint64_t>(index) + 1 ||
            (index == 0 && event.counsel.has_value()) ||
            (index != 0 && !event.counsel.has_value()) ||
            (index == 0 && event.bench.kind != model::BenchActKind::Question) ||
            (index == 0 && event.bench.question->parent_act_sequence.has_value()) ||
            (index != 0 && projected.phase != model::OralArgumentPhase::Principal &&
             projected.phase != model::OralArgumentPhase::Rebuttal)) {
            return fail(ErrorCode::InvalidSession, "oral-argument journal ordering is invalid");
        }
        if (event.counsel.has_value()) {
            const auto remaining = projected.phase == model::OralArgumentPhase::Principal
                                       ? projected.principal_remaining
                                       : projected.rebuttal_remaining;
            const auto expires = event.counsel->elapsed >= remaining;
            if ((event.bench.kind == model::BenchActKind::TimeExpired) != expires) {
                return fail(ErrorCode::InvalidSession,
                            "oral-argument journal contradicts its explicit clock");
            }
        }
        if (event.bench.kind == model::BenchActKind::FollowUp) {
            const auto* current = projected.last_seat_id.has_value()
                                      ? seatById(bench, *projected.last_seat_id)
                                      : nullptr;
            if (current == nullptr || current->id != event.bench.seat_id ||
                projected.follow_up_depth >= allowedFollowUps(configuration, *current)) {
                return fail(ErrorCode::InvalidSession,
                            "oral-argument journal exceeds its follow-up chain");
            }
        }
        std::expected<model::OralArgumentEvent, Error> expected =
            index == 0 ? planOpeningUnchecked(bench, grounding, projected)
            : event.counsel.has_value()
                ? decideCounselAnswerUnchecked(configuration, bench, grounding, projected,
                                               *event.counsel)
                : fail(ErrorCode::InvalidSession, "oral-argument journal answer is missing");
        if (!expected) {
            return fail(ErrorCode::InvalidSession, expected.error().message);
        }
        if (*expected != event) {
            return fail(ErrorCode::InvalidSession,
                        "oral-argument journal differs from the deterministic decision");
        }
        projected = applyAcceptedEvent(configuration, projected, event);
    }
    return projected;
}

[[nodiscard]] auto validateState(const model::OralArgumentConfiguration& configuration,
                                 const model::BenchConfiguration& bench,
                                 const model::ArgumentGrounding& grounding,
                                 const model::OralArgumentState& state)
    -> std::expected<void, Error> {
    if (!validEnum(state.phase) ||
        state.behavior_definition_digest != configuration.behavior_definition_digest ||
        state.grounding_digest != configuration.grounding_digest ||
        state.legal_state_digest != configuration.legal_state_digest ||
        state.authored_disposition_id != configuration.authored_disposition_id ||
        state.canonical_contract.has_value() ||
        state.journal.size() > maximum_events ||
        state.transcript.size() > maximum_transcript_entries ||
        state.concessions.size() > maximum_events || state.next_event_sequence == 0 ||
        state.next_event_sequence != static_cast<std::uint64_t>(state.journal.size()) + 1 ||
        state.principal_remaining < std::chrono::seconds::zero() ||
        state.principal_remaining > configuration.principal_time ||
        state.rebuttal_remaining < std::chrono::seconds::zero() ||
        state.rebuttal_remaining > configuration.rebuttal_time ||
        state.follow_up_depth > configuration.maximum_follow_up_depth) {
        return fail(ErrorCode::InvalidSession,
                    "oral argument does not match its pinned definitions and limits");
    }
    const auto projected = projectJournal(configuration, bench, grounding, state.journal);
    if (!projected || *projected != state) {
        return fail(ErrorCode::InvalidSession,
                    projected ? "oral-argument state is not its journal projection"
                              : projected.error().message);
    }
    return {};
}

[[nodiscard]] auto ensureEventCapacity(const model::OralArgumentState& state)
    -> std::expected<void, Error> {
    if (state.journal.size() >= maximum_events) {
        return fail(ErrorCode::InvalidTransition, "oral argument reached its hard event limit");
    }
    return {};
}

[[nodiscard]] auto ensureCanonicalEventCapacity(const model::OralArgumentState& state)
    -> std::expected<void, Error> {
    if (state.journal.size() >= maximum_canonical_events) {
        return fail(ErrorCode::InvalidTransition,
                    "canonical oral argument reached its hard event limit");
    }
    return {};
}

struct SeatTurn final {
    const model::BenchSeat* seat{};
    bool continues_follow_up{};
};

[[nodiscard]] auto selectSeat(const model::OralArgumentConfiguration& configuration,
                              const model::BenchConfiguration& bench,
                              const model::OralArgumentState& state)
    -> std::expected<SeatTurn, Error> {
    if (!state.last_seat_id.has_value()) {
        const auto* presiding = presidingSeat(bench);
        if (presiding == nullptr) {
            return fail(ErrorCode::InvalidDefinition, "presiding seat is unavailable");
        }
        return SeatTurn{presiding, false};
    }
    const auto current = std::ranges::find(bench.seats, *state.last_seat_id, &model::BenchSeat::id);
    if (current == bench.seats.end()) {
        return fail(ErrorCode::InvalidSession, "last argument seat is not on the pinned bench");
    }
    if (state.follow_up_depth < allowedFollowUps(configuration, *current)) {
        return SeatTurn{&*current, true};
    }
    if (bench.seats.size() == 1) {
        return SeatTurn{&*current, false};
    }
    const auto next =
        std::next(current) == bench.seats.end() ? bench.seats.begin() : std::next(current);
    return SeatTurn{&*next, false};
}

[[nodiscard]] const model::ArgumentIssue* chooseIssue(const model::ArgumentGrounding& grounding,
                                                      const model::BenchSeat& seat,
                                                      std::string_view requested_issue,
                                                      std::uint64_t sequence) {
    if (!requested_issue.empty()) {
        return issueById(grounding, requested_issue);
    }

    const model::ArgumentIssue* focused = nullptr;
    double focused_weight = -1.0;
    for (const auto& focus : seat.profile.interaction.issue_focus) {
        const auto* candidate = issueById(grounding, focus.topic_id);
        if (candidate != nullptr && focus.weight > focused_weight) {
            focused = candidate;
            focused_weight = focus.weight;
        }
    }
    if (focused != nullptr) {
        return focused;
    }
    if (grounding.issues.empty() || sequence == 0) {
        return nullptr;
    }
    const auto index = static_cast<std::size_t>((sequence - 1) % grounding.issues.size());
    return &grounding.issues[index];
}

[[nodiscard]] std::optional<std::size_t> structuredChoice(const model::BenchSeat& seat,
                                                          model::BenchActKind kind,
                                                          std::uint64_t sequence,
                                                          std::size_t bound) {
    if (bound == 0) {
        return std::nullopt;
    }
    const auto& interaction = seat.profile.interaction;
    const auto& voice = seat.profile.voice;
    const auto scaled = static_cast<std::uint64_t>(
        std::lround((interaction.directness + interaction.formality + interaction.question_length +
                     voice.verbosity + voice.sentence_complexity) *
                    10.0));
    const auto structure = static_cast<std::uint64_t>(voice.register_style) * 3U +
                           static_cast<std::uint64_t>(voice.cadence) * 5U +
                           static_cast<std::uint64_t>(voice.question_framing) * 7U +
                           static_cast<std::uint64_t>(kind) * 11U;
    return static_cast<std::size_t>((sequence + scaled + structure) % bound);
}

[[nodiscard]] std::vector<model::ArgumentGroundingRef>
selectGrounding(const model::ArgumentIssue& issue, const model::BenchSeat& seat,
                model::BenchActKind kind) {
    if (issue.grounding.empty()) {
        return {};
    }
    const auto requested =
        1 + static_cast<std::size_t>(std::floor(
                (seat.profile.voice.verbosity + seat.profile.interaction.question_length) * 1.5));
    const auto wanted = std::min(requested, issue.grounding.size());
    std::vector<model::ArgumentGroundingRef> selected;
    selected.reserve(wanted);
    if (kind == model::BenchActKind::RecordPinDemand) {
        const auto record = std::ranges::find(issue.grounding, model::GroundingKind::RecordPage,
                                              &model::ArgumentGroundingRef::kind);
        if (record != issue.grounding.end()) {
            selected.push_back(*record);
        }
    }
    for (const auto& reference : issue.grounding) {
        if (selected.size() == wanted) {
            break;
        }
        if (std::ranges::find(selected, reference.id, &model::ArgumentGroundingRef::id) ==
            selected.end()) {
            selected.push_back(reference);
        }
    }
    return selected;
}

[[nodiscard]] const std::vector<std::string>* phrasesFor(const model::BenchSeat& seat,
                                                         model::BenchActKind kind) {
    switch (kind) {
    case model::BenchActKind::Interruption:
        return &seat.profile.voice.interruption_phrases;
    case model::BenchActKind::ClarificationRequest:
        return &seat.profile.voice.clarification_phrases;
    case model::BenchActKind::Question:
    case model::BenchActKind::FollowUp:
    case model::BenchActKind::Hypothetical:
    case model::BenchActKind::RecordPinDemand:
        return &seat.profile.voice.question_phrases;
    case model::BenchActKind::TimeExpired:
        return nullptr;
    }
    return nullptr;
}

[[nodiscard]] std::optional<std::string_view> counselAddress(model::CounselAddress address) {
    switch (address) {
    case model::CounselAddress::Counsel:
        return "Counsel";
    case model::CounselAddress::Advocate:
        return "Advocate";
    }
    return std::nullopt;
}

[[nodiscard]] std::string
groundingSummary(const std::vector<model::ArgumentGroundingRef>& grounding, bool verbose) {
    if (grounding.empty()) {
        return {};
    }
    std::string result = grounding.front().id;
    if (verbose) {
        for (auto iterator = std::next(grounding.begin()); iterator != grounding.end();
             ++iterator) {
            result += ", " + iterator->id;
        }
    }
    return result;
}

[[nodiscard]] std::string groundingId(const model::AuthoredArgumentGrounding& grounding) {
    return std::visit([](const auto& reference) { return reference.grounding_id; }, grounding);
}

[[nodiscard]] std::string
groundingSummary(const std::vector<model::AuthoredArgumentGrounding>& grounding, bool verbose) {
    if (grounding.empty()) {
        return {};
    }
    std::string result = groundingId(grounding.front());
    if (verbose) {
        for (auto iterator = std::next(grounding.begin()); iterator != grounding.end(); ++iterator) {
            result += ", " + groundingId(*iterator);
        }
    }
    return result;
}

[[nodiscard]] auto choosePrompt(const model::ArgumentIssue& issue, const model::BenchSeat& seat,
                                model::BenchActKind kind, std::uint64_t sequence)
    -> std::expected<std::string, Error> {
    if (kind == model::BenchActKind::ClarificationRequest) {
        return "Clarify how your answer addresses " + issue.label;
    }
    if (kind == model::BenchActKind::RecordPinDemand) {
        return "Identify the exact record page supporting the proposition on " + issue.label;
    }
    const auto* prompts = &issue.question_prompts;
    if (kind == model::BenchActKind::Hypothetical && !issue.hypothetical_prompts.empty()) {
        prompts = &issue.hypothetical_prompts;
    }
    const auto index = structuredChoice(seat, kind, sequence, prompts->size());
    if (!index.has_value()) {
        return fail(ErrorCode::InvalidDefinition, "question prompt inventory is empty");
    }
    return (*prompts)[*index];
}

[[nodiscard]] auto renderQuestion(const model::BenchSeat& seat, model::BenchActKind kind,
                                  const model::GroundedQuestion& question, std::uint64_t sequence)
    -> std::expected<std::string, Error> {
    const auto* phrases = phrasesFor(seat, kind);
    const auto address = counselAddress(seat.profile.voice.address_convention);
    if (phrases == nullptr || !address.has_value()) {
        return fail(ErrorCode::InvalidDefinition, "question renderer style is invalid");
    }
    const auto phrase_index = structuredChoice(seat, kind, sequence, phrases->size());
    const auto prompt = std::visit([](const auto& selection) -> std::string_view {
        return selection.prompt;
    }, question.selection);
    const auto grounding_empty = std::visit([](const auto& selection) {
        return selection.grounding.empty();
    }, question.selection);
    if (!phrase_index.has_value() || grounding_empty) {
        return fail(ErrorCode::InvalidDefinition, "question renderer inventory is empty");
    }
    std::string rendered = std::string(*address) + ", " + (*phrases)[*phrase_index];
    switch (seat.profile.voice.question_framing) {
    case model::QuestionFraming::Direct:
        rendered += ": " + std::string(prompt);
        break;
    case model::QuestionFraming::Socratic:
        rendered += "; help the court understand: " + std::string(prompt);
        break;
    case model::QuestionFraming::Narrative:
        rendered += "; walk us through: " + std::string(prompt);
        break;
    }

    const auto& profile_voice = seat.profile.voice;
    const auto sources = std::visit(
        [&](const auto& selection) {
            return groundingSummary(selection.grounding, profile_voice.verbosity >= 0.5);
        },
        question.selection);
    switch (profile_voice.register_style) {
    case model::VoiceRegister::Plain:
        rendered += " [" + sources + "]";
        break;
    case model::VoiceRegister::Formal:
        rendered += "; please address " + sources;
        break;
    case model::VoiceRegister::Technical:
        rendered += "; reconcile your position with " + sources;
        break;
    }
    switch (profile_voice.cadence) {
    case model::VoiceCadence::Clipped:
        rendered += ".";
        break;
    case model::VoiceCadence::Measured:
        rendered += "; answer directly.";
        break;
    case model::VoiceCadence::Expansive:
        rendered += ", and explain each step of the reasoning.";
        break;
    }
    if (!boundedText(rendered, maximum_rendered_utterance_length)) {
        return fail(ErrorCode::InvalidDefinition, "rendered question exceeds its hard limit");
    }
    return rendered;
}

[[nodiscard]] auto makeQuestionAct(const model::BenchSeat& seat, const model::ArgumentIssue& issue,
                                   model::BenchActKind kind, std::uint64_t sequence,
                                   std::optional<std::uint64_t> parent, bool recalls_concession)
    -> std::expected<model::BenchAct, Error> {
    const auto prompt = choosePrompt(issue, seat, kind, sequence);
    auto selected_grounding = selectGrounding(issue, seat, kind);
    if (!prompt || selected_grounding.empty()) {
        return fail(ErrorCode::InvalidDefinition, "cannot construct a grounded bench question");
    }
    auto question = model::GroundedQuestion{issue.id, *prompt, std::move(selected_grounding), parent,
                                             recalls_concession};
    const auto rendered = renderQuestion(seat, kind, question, sequence);
    if (!rendered) {
        return std::unexpected(rendered.error());
    }
    return model::BenchAct{kind, seat.id, std::move(question), *rendered};
}

[[nodiscard]] auto renderExpiration(const model::BenchSeat& seat, model::OralArgumentPhase phase,
                                    bool rebuttal_remains) -> std::expected<std::string, Error> {
    const auto address = counselAddress(seat.profile.voice.address_convention);
    if (!address.has_value()) {
        return fail(ErrorCode::InvalidDefinition, "expiration renderer style is invalid");
    }
    std::string rendered = std::string(*address) + ", your ";
    rendered += phase == model::OralArgumentPhase::Principal ? "principal" : "rebuttal";
    rendered += seat.profile.interaction.time_strictness >= 0.5
                    ? " time has expired"
                    : " allotted time has now concluded";
    rendered += rebuttal_remains ? "; rebuttal time remains." : ".";
    return rendered;
}

[[nodiscard]] auto planOpeningUnchecked(const model::BenchConfiguration& bench,
                                        const model::ArgumentGrounding& grounding,
                                        const model::OralArgumentState& state)
    -> std::expected<model::OralArgumentEvent, Error> {
    if (const auto capacity = ensureEventCapacity(state); !capacity) {
        return std::unexpected(capacity.error());
    }
    if (state.phase != model::OralArgumentPhase::NotStarted) {
        return fail(ErrorCode::InvalidTransition, "opening question is not available");
    }
    const auto* seat = presidingSeat(bench);
    const auto* issue =
        seat == nullptr ? nullptr : chooseIssue(grounding, *seat, {}, state.next_event_sequence);
    if (seat == nullptr || issue == nullptr) {
        return fail(ErrorCode::InvalidDefinition, "opening question has no seat or issue");
    }
    const auto act = makeQuestionAct(*seat, *issue, model::BenchActKind::Question,
                                     state.next_event_sequence, std::nullopt, false);
    if (!act) {
        return std::unexpected(act.error());
    }
    return model::OralArgumentEvent{state.next_event_sequence, std::nullopt, *act};
}

[[nodiscard]] auto decideCounselAnswerUnchecked(
    const model::OralArgumentConfiguration& configuration, const model::BenchConfiguration& bench,
    const model::ArgumentGrounding& grounding, const model::OralArgumentState& state,
    const model::CounselAnswer& answer) -> std::expected<model::OralArgumentEvent, Error> {
    if (const auto capacity = ensureEventCapacity(state); !capacity) {
        return std::unexpected(capacity.error());
    }
    if (state.phase != model::OralArgumentPhase::Principal &&
        state.phase != model::OralArgumentPhase::Rebuttal) {
        return fail(ErrorCode::InvalidTransition, "counsel may not answer in this phase");
    }

    const auto turn = selectSeat(configuration, bench, state);
    if (!turn || turn->seat == nullptr) {
        return fail(ErrorCode::InvalidSession, "no valid bench turn is available");
    }
    const auto* issue =
        chooseIssue(grounding, *turn->seat, answer.issue_id, state.next_event_sequence);
    if (issue == nullptr) {
        return fail(ErrorCode::InvalidCommand, "answer issue is unavailable");
    }
    const auto remaining = state.phase == model::OralArgumentPhase::Principal
                               ? state.principal_remaining
                               : state.rebuttal_remaining;
    if (answer.elapsed >= remaining) {
        const auto* seat = presidingSeat(bench);
        if (seat == nullptr) {
            return fail(ErrorCode::InvalidDefinition, "presiding seat is unavailable");
        }
        const auto rebuttal_remains = state.phase == model::OralArgumentPhase::Principal &&
                                      state.rebuttal_remaining > std::chrono::seconds::zero();
        const auto rendered = renderExpiration(*seat, state.phase, rebuttal_remains);
        if (!rendered) {
            return std::unexpected(rendered.error());
        }
        return model::OralArgumentEvent{
            state.next_event_sequence,
            answer,
            model::BenchAct{
                model::BenchActKind::TimeExpired,
                seat->id,
                std::nullopt,
                *rendered,
            },
        };
    }

    model::BenchActKind kind{};
    bool recalls_concession = false;
    if (answer.classification_confidence < configuration.classification_confidence_threshold) {
        kind = model::BenchActKind::ClarificationRequest;
    } else if (answer.kind == model::CounselActKind::RecordClaim && hasRecordPage(*issue) &&
               turn->seat->profile.interaction.record_pin_demand >= 0.5 &&
               std::ranges::none_of(answer.cited_grounding_ids, [&](const std::string& id) {
                   const auto found =
                       std::ranges::find(issue->grounding, id, &model::ArgumentGroundingRef::id);
                   return found != issue->grounding.end() &&
                          found->kind == model::GroundingKind::RecordPage;
               })) {
        kind = model::BenchActKind::RecordPinDemand;
    } else if (answer.kind == model::CounselActKind::Concession && turn->continues_follow_up &&
               turn->seat->profile.interaction.concession_recall >= 0.5) {
        kind = model::BenchActKind::FollowUp;
        recalls_concession = true;
    } else if (turn->seat->profile.interaction.hypothetical_frequency >= 0.6 &&
               !issue->hypothetical_prompts.empty()) {
        kind = model::BenchActKind::Hypothetical;
    } else if (turn->seat->profile.interaction.interruption_frequency >= 0.6) {
        kind = model::BenchActKind::Interruption;
    } else if (turn->continues_follow_up) {
        kind = model::BenchActKind::FollowUp;
    } else {
        kind = model::BenchActKind::Question;
    }

    const auto parent = state.next_event_sequence > 1
                            ? std::optional<std::uint64_t>{state.next_event_sequence - 1}
                            : std::nullopt;
    const auto act = makeQuestionAct(*turn->seat, *issue, kind, state.next_event_sequence, parent,
                                     recalls_concession);
    if (!act) {
        return std::unexpected(act.error());
    }
    return model::OralArgumentEvent{state.next_event_sequence, answer, *act};
}

[[nodiscard]] const model::ArgumentIssueTopics*
authoredIssueById(const model::AuthoredQuestionBank& bank, std::string_view issue_id) {
    const auto found = std::ranges::find(bank.issue_topics, issue_id,
                                         &model::ArgumentIssueTopics::issue_id);
    return found == bank.issue_topics.end() ? nullptr : &*found;
}

[[nodiscard]] const model::AuthoredArgumentQuestion*
authoredQuestionById(const model::AuthoredQuestionBank& bank, std::string_view question_id) {
    const auto found = std::ranges::find(bank.questions, question_id,
                                         &model::AuthoredArgumentQuestion::id);
    return found == bank.questions.end() ? nullptr : &*found;
}

[[nodiscard]] bool isRecordGrounding(const model::AuthoredArgumentGrounding& grounding) {
    return std::holds_alternative<model::RecordPageArgumentGrounding>(grounding);
}

[[nodiscard]] bool hasAuthoredRecordPage(const model::AuthoredArgumentQuestion& question) {
    return std::ranges::any_of(question.grounding, isRecordGrounding);
}

[[nodiscard]] bool issueHasAuthoredRecordPage(const model::AuthoredQuestionBank& bank,
                                              std::string_view issue_id) {
    return std::ranges::any_of(bank.questions, [&](const auto& question) {
        return question.issue_id == issue_id && hasAuthoredRecordPage(question);
    });
}

[[nodiscard]] bool authoredGroundingAvailable(const model::AuthoredQuestionBank& bank,
                                               std::string_view issue_id,
                                               std::string_view grounding_id) {
    return std::ranges::any_of(bank.questions, [&](const auto& question) {
        return question.issue_id == issue_id &&
               std::ranges::any_of(question.grounding, [&](const auto& grounding) {
                   return authoredGroundingId(grounding) == grounding_id;
               });
    });
}

[[nodiscard]] bool authoredRecordGroundingAvailable(const model::AuthoredQuestionBank& bank,
                                                     std::string_view issue_id,
                                                     std::string_view grounding_id) {
    return std::ranges::any_of(bank.questions, [&](const auto& question) {
        return question.issue_id == issue_id &&
               std::ranges::any_of(question.grounding, [&](const auto& grounding) {
                   return authoredGroundingId(grounding) == grounding_id &&
                          isRecordGrounding(grounding);
               });
    });
}

[[nodiscard]] auto validateCanonicalCounselAnswer(const model::AuthoredQuestionBank& bank,
                                                  const model::CounselAnswer& answer)
    -> std::expected<void, Error> {
    if (!validEnum(answer.kind) || !boundedText(answer.text, maximum_answer_length) ||
        !namespacedId(answer.issue_id) || !unitInterval(answer.classification_confidence) ||
        answer.elapsed <= std::chrono::seconds::zero() || answer.elapsed > maximum_argument_time ||
        answer.cited_grounding_ids.size() > maximum_citations ||
        !hasUniqueValues(answer.cited_grounding_ids,
                         [](const std::string& value) -> const std::string& { return value; }) ||
        !std::ranges::all_of(answer.cited_grounding_ids,
                             [](const std::string& value) { return namespacedId(value); }) ||
        authoredIssueById(bank, answer.issue_id) == nullptr ||
        !std::ranges::all_of(answer.cited_grounding_ids, [&](const std::string& id) {
            return authoredGroundingAvailable(bank, answer.issue_id, id);
        })) {
        return fail(ErrorCode::InvalidCommand,
                    "answer refers to invalid or unavailable authored grounding");
    }
    return {};
}

[[nodiscard]] double focusWeight(const model::BenchSeat& seat,
                                 model::ArgumentFocusTopic topic) {
    const auto id = model::argumentFocusTopicId(topic);
    const auto focus = std::ranges::find(seat.profile.interaction.issue_focus, id,
                                         &model::IssueFocus::topic_id);
    return focus == seat.profile.interaction.issue_focus.end() ? -1.0 : focus->weight;
}

[[nodiscard]] const model::AuthoredArgumentQuestion* chooseAuthoredQuestion(
    const model::AuthoredQuestionBank& bank, const model::BenchSeat& seat,
    std::string_view requested_issue, model::BenchActKind kind, std::uint64_t sequence,
    bool require_record_page = false) {
    std::vector<const model::AuthoredArgumentQuestion*> candidates;
    candidates.reserve(bank.questions.size());
    for (const auto& question : bank.questions) {
        if ((!requested_issue.empty() && question.issue_id != requested_issue) ||
            (require_record_page && !hasAuthoredRecordPage(question))) {
            continue;
        }
        candidates.push_back(&question);
    }
    if (candidates.empty()) {
        return nullptr;
    }
    std::ranges::sort(candidates, {}, [](const auto* question) -> const std::string& {
        return question->id;
    });
    double maximum_weight = -1.0;
    for (const auto* question : candidates) {
        maximum_weight = std::max(maximum_weight, focusWeight(seat, question->topic));
    }
    if (maximum_weight >= 0.0) {
        std::erase_if(candidates, [&](const auto* question) {
            return focusWeight(seat, question->topic) != maximum_weight;
        });
    }
    const auto selected = structuredChoice(seat, kind, sequence, candidates.size());
    return selected.has_value() ? candidates[*selected] : nullptr;
}

[[nodiscard]] model::AuthoredQuestionSelection authoredSelection(
    const model::AuthoredArgumentQuestion& question, model::OralArgumentMode mode) {
    auto grounding = question.grounding;
    std::ranges::sort(grounding, [](const auto& left, const auto& right) {
        return authoredGroundingId(left) < authoredGroundingId(right);
    });
    return model::AuthoredQuestionSelection{question.id, question.topic, mode, question.prompt,
                                            std::move(grounding)};
}

[[nodiscard]] auto makeAuthoredQuestionAct(
    const model::BenchSeat& seat, const model::AuthoredArgumentQuestion& authored,
    model::OralArgumentMode mode, model::BenchActKind kind, std::uint64_t sequence,
    std::optional<std::uint64_t> parent, bool recalls_concession)
    -> std::expected<model::BenchAct, Error> {
    auto question = model::GroundedQuestion{authored.issue_id, authoredSelection(authored, mode),
                                             parent, recalls_concession};
    const auto rendered = renderQuestion(seat, kind, question, sequence);
    if (!rendered) {
        return std::unexpected(rendered.error());
    }
    return model::BenchAct{kind, seat.id, std::move(question), *rendered};
}

[[nodiscard]] auto validateAuthoredEventShape(
    const model::CanonicalOralArgumentDefinition& definition,
    const model::OralArgumentEvent& event) -> std::expected<void, Error> {
    if (event.sequence == 0 || event.sequence > maximum_canonical_events ||
        !validEnum(event.bench.kind) ||
        seatById(definition.bench, event.bench.seat_id) == nullptr ||
        !boundedText(event.bench.rendered_utterance, maximum_rendered_utterance_length)) {
        return fail(ErrorCode::InvalidEvent, "canonical oral-argument event shape is invalid");
    }
    if (event.counsel.has_value()) {
        if (const auto valid =
                validateCanonicalCounselAnswer(definition.question_bank, *event.counsel);
            !valid) {
            return fail(ErrorCode::InvalidEvent, valid.error().message);
        }
    }
    if (!questionActKind(event.bench.kind)) {
        if (event.bench.question.has_value()) {
            return fail(ErrorCode::InvalidEvent,
                        "non-question canonical act carries an authored selection");
        }
        return {};
    }
    if (!event.bench.question.has_value()) {
        return fail(ErrorCode::InvalidEvent, "canonical bench question is missing");
    }
    const auto& question = *event.bench.question;
    const auto* selection = std::get_if<model::AuthoredQuestionSelection>(&question.selection);
    const auto* authored = selection == nullptr
                               ? nullptr
                               : authoredQuestionById(definition.question_bank,
                                                      selection->question_id);
    if (selection == nullptr || authored == nullptr || authored->issue_id != question.issue_id ||
        *selection != authoredSelection(*authored, definition.question_bank.mode) ||
        (question.parent_act_sequence.has_value() &&
         (*question.parent_act_sequence == 0 ||
          *question.parent_act_sequence >= event.sequence)) ||
        (question.recalls_concession && event.bench.kind != model::BenchActKind::FollowUp)) {
        return fail(ErrorCode::InvalidEvent,
                    "canonical bench question is not one exact authored selection");
    }
    if (event.bench.kind == model::BenchActKind::RecordPinDemand &&
        !std::ranges::any_of(selection->grounding, isRecordGrounding)) {
        return fail(ErrorCode::InvalidEvent,
                    "canonical record-pin demand lacks record-page grounding");
    }
    return {};
}

[[nodiscard]] model::OralArgumentState initialCanonicalStateUnchecked(
    const model::CanonicalOralArgumentDefinition& definition) {
    auto state = initialStateUnchecked(definition.configuration);
    state.canonical_contract = canonicalContract(definition);
    return state;
}

[[nodiscard]] auto planAuthoredOpeningUnchecked(
    const model::CanonicalOralArgumentDefinition& definition,
    const model::OralArgumentState& state) -> std::expected<model::OralArgumentEvent, Error> {
    if (const auto capacity = ensureCanonicalEventCapacity(state); !capacity) {
        return std::unexpected(capacity.error());
    }
    if (state.phase != model::OralArgumentPhase::NotStarted) {
        return fail(ErrorCode::InvalidTransition, "opening question is not available");
    }
    const auto* seat = presidingSeat(definition.bench);
    const auto* question = seat == nullptr
                               ? nullptr
                               : chooseAuthoredQuestion(definition.question_bank, *seat, {},
                                                        model::BenchActKind::Question,
                                                        state.next_event_sequence);
    if (seat == nullptr || question == nullptr) {
        return fail(ErrorCode::InvalidDefinition,
                    "opening question has no authored seat or question");
    }
    const auto act = makeAuthoredQuestionAct(
        *seat, *question, definition.question_bank.mode, model::BenchActKind::Question,
        state.next_event_sequence, std::nullopt, false);
    if (!act) {
        return std::unexpected(act.error());
    }
    return model::OralArgumentEvent{state.next_event_sequence, std::nullopt, *act};
}

[[nodiscard]] auto decideAuthoredCounselAnswerUnchecked(
    const model::CanonicalOralArgumentDefinition& definition,
    const model::OralArgumentState& state, const model::CounselAnswer& answer)
    -> std::expected<model::OralArgumentEvent, Error> {
    if (const auto capacity = ensureCanonicalEventCapacity(state); !capacity) {
        return std::unexpected(capacity.error());
    }
    if (state.phase != model::OralArgumentPhase::Principal &&
        state.phase != model::OralArgumentPhase::Rebuttal) {
        return fail(ErrorCode::InvalidTransition, "counsel may not answer in this phase");
    }
    const auto turn = selectSeat(definition.configuration, definition.bench, state);
    if (!turn || turn->seat == nullptr ||
        authoredIssueById(definition.question_bank, answer.issue_id) == nullptr) {
        return fail(ErrorCode::InvalidCommand, "answer issue or bench turn is unavailable");
    }
    const auto remaining = state.phase == model::OralArgumentPhase::Principal
                               ? state.principal_remaining
                               : state.rebuttal_remaining;
    if (answer.elapsed >= remaining) {
        const auto* seat = presidingSeat(definition.bench);
        if (seat == nullptr) {
            return fail(ErrorCode::InvalidDefinition, "presiding seat is unavailable");
        }
        const auto rebuttal_remains = state.phase == model::OralArgumentPhase::Principal &&
                                      state.rebuttal_remaining > std::chrono::seconds::zero();
        const auto rendered = renderExpiration(*seat, state.phase, rebuttal_remains);
        if (!rendered) {
            return std::unexpected(rendered.error());
        }
        return model::OralArgumentEvent{
            state.next_event_sequence,
            answer,
            model::BenchAct{model::BenchActKind::TimeExpired, seat->id, std::nullopt, *rendered},
        };
    }

    model::BenchActKind kind{};
    bool recalls_concession = false;
    bool require_record_page = false;
    if (answer.classification_confidence <
        definition.configuration.classification_confidence_threshold) {
        kind = model::BenchActKind::ClarificationRequest;
    } else if (answer.kind == model::CounselActKind::RecordClaim &&
               issueHasAuthoredRecordPage(definition.question_bank, answer.issue_id) &&
               turn->seat->profile.interaction.record_pin_demand >= 0.5 &&
               std::ranges::none_of(answer.cited_grounding_ids, [&](const std::string& id) {
                   return authoredRecordGroundingAvailable(definition.question_bank,
                                                           answer.issue_id, id);
               })) {
        kind = model::BenchActKind::RecordPinDemand;
        require_record_page = true;
    } else if (answer.kind == model::CounselActKind::Concession && turn->continues_follow_up &&
               turn->seat->profile.interaction.concession_recall >= 0.5) {
        kind = model::BenchActKind::FollowUp;
        recalls_concession = true;
    } else if (turn->seat->profile.interaction.hypothetical_frequency >= 0.6) {
        kind = model::BenchActKind::Hypothetical;
    } else if (turn->seat->profile.interaction.interruption_frequency >= 0.6) {
        kind = model::BenchActKind::Interruption;
    } else if (turn->continues_follow_up) {
        kind = model::BenchActKind::FollowUp;
    } else {
        kind = model::BenchActKind::Question;
    }
    const auto* question = chooseAuthoredQuestion(
        definition.question_bank, *turn->seat, answer.issue_id, kind,
        state.next_event_sequence, require_record_page);
    if (question == nullptr) {
        return fail(ErrorCode::InvalidDefinition,
                    "cannot select an exact authored question for the bench act");
    }
    const auto parent = state.next_event_sequence > 1
                            ? std::optional<std::uint64_t>{state.next_event_sequence - 1}
                            : std::nullopt;
    const auto act = makeAuthoredQuestionAct(*turn->seat, *question,
                                             definition.question_bank.mode, kind,
                                             state.next_event_sequence, parent,
                                             recalls_concession);
    if (!act) {
        return std::unexpected(act.error());
    }
    return model::OralArgumentEvent{state.next_event_sequence, answer, *act};
}

[[nodiscard]] auto projectAuthoredJournal(
    const model::CanonicalOralArgumentDefinition& definition,
    std::span<const model::OralArgumentEvent> journal)
    -> std::expected<model::OralArgumentState, Error> {
    if (journal.size() > maximum_canonical_events) {
        return fail(ErrorCode::InvalidSession,
                    "canonical oral-argument journal exceeds its hard limit");
    }
    auto projected = initialCanonicalStateUnchecked(definition);
    for (std::size_t index = 0; index < journal.size(); ++index) {
        const auto& event = journal[index];
        if (const auto valid = validateAuthoredEventShape(definition, event); !valid) {
            return fail(ErrorCode::InvalidSession, valid.error().message);
        }
        if (event.sequence != static_cast<std::uint64_t>(index) + 1 ||
            (index == 0 && event.counsel.has_value()) ||
            (index != 0 && !event.counsel.has_value()) ||
            (index == 0 && event.bench.kind != model::BenchActKind::Question) ||
            (index == 0 && event.bench.question->parent_act_sequence.has_value()) ||
            (index != 0 && projected.phase != model::OralArgumentPhase::Principal &&
             projected.phase != model::OralArgumentPhase::Rebuttal)) {
            return fail(ErrorCode::InvalidSession,
                        "canonical oral-argument journal ordering is invalid");
        }
        if (event.counsel.has_value()) {
            const auto remaining = projected.phase == model::OralArgumentPhase::Principal
                                       ? projected.principal_remaining
                                       : projected.rebuttal_remaining;
            if ((event.bench.kind == model::BenchActKind::TimeExpired) !=
                (event.counsel->elapsed >= remaining)) {
                return fail(ErrorCode::InvalidSession,
                            "canonical oral-argument journal contradicts its clock");
            }
        }
        if (event.bench.kind == model::BenchActKind::FollowUp) {
            const auto* current = projected.last_seat_id.has_value()
                                      ? seatById(definition.bench, *projected.last_seat_id)
                                      : nullptr;
            if (current == nullptr || current->id != event.bench.seat_id ||
                projected.follow_up_depth >=
                    allowedFollowUps(definition.configuration, *current)) {
                return fail(ErrorCode::InvalidSession,
                            "canonical oral-argument journal exceeds its follow-up chain");
            }
        }
        auto expected = index == 0
                            ? planAuthoredOpeningUnchecked(definition, projected)
                            : decideAuthoredCounselAnswerUnchecked(definition, projected,
                                                                   *event.counsel);
        if (!expected || *expected != event) {
            return fail(ErrorCode::InvalidSession,
                        expected ? "canonical event differs from exact authored decision"
                                 : expected.error().message);
        }
        projected = applyAcceptedEvent(definition.configuration, projected, event);
    }
    return projected;
}

[[nodiscard]] auto validateCanonicalState(
    const model::CanonicalOralArgumentDefinition& definition,
    const model::OralArgumentState& state) -> std::expected<void, Error> {
    if (!validEnum(state.phase) ||
        state.behavior_definition_digest != definition.configuration.behavior_definition_digest ||
        state.grounding_digest != definition.configuration.grounding_digest ||
        state.legal_state_digest != definition.configuration.legal_state_digest ||
        state.authored_disposition_id != definition.configuration.authored_disposition_id ||
        state.canonical_contract !=
            std::optional<model::CanonicalOralArgumentContract>{
                canonicalContract(definition)} ||
        state.journal.size() > maximum_canonical_events ||
        state.transcript.size() > maximum_canonical_transcript_entries ||
        state.concessions.size() > maximum_canonical_events || state.next_event_sequence == 0 ||
        state.next_event_sequence != static_cast<std::uint64_t>(state.journal.size()) + 1 ||
        state.principal_remaining < std::chrono::seconds::zero() ||
        state.principal_remaining > definition.configuration.principal_time ||
        state.rebuttal_remaining < std::chrono::seconds::zero() ||
        state.rebuttal_remaining > definition.configuration.rebuttal_time ||
        state.follow_up_depth > definition.configuration.maximum_follow_up_depth) {
        return fail(ErrorCode::InvalidSession,
                    "canonical oral argument does not match its definition and immutable pins");
    }
    const auto projected = projectAuthoredJournal(definition, state.journal);
    if (!projected || *projected != state) {
        return fail(ErrorCode::InvalidSession,
                    projected ? "canonical oral state is not its journal projection"
                              : projected.error().message);
    }
    return {};
}

} // namespace

std::expected<std::string, Error> behaviorDefinitionDigest(const model::BenchConfiguration& bench) {
    if (const auto valid = validateBench(bench); !valid) {
        return std::unexpected(valid.error());
    }
    return digestBehaviorUnchecked(bench);
}

std::expected<std::string, Error> groundingDigest(const model::ArgumentGrounding& grounding) {
    if (const auto valid = validateGrounding(grounding); !valid) {
        return std::unexpected(valid.error());
    }
    return digestGroundingUnchecked(grounding);
}

std::expected<std::string, Error>
groundingDigest(const model::AuthoredQuestionBank& question_bank) {
    if (const auto valid = validateQuestionBank(question_bank); !valid) {
        return std::unexpected(valid.error());
    }
    return digestQuestionBankUnchecked(question_bank);
}

bool isQuestionAct(model::BenchActKind kind) noexcept { return questionActKind(kind); }

std::expected<model::OralArgumentState, Error>
initializeOralArgument(const model::OralArgumentConfiguration& configuration,
                       const model::BenchConfiguration& bench,
                       const model::ArgumentGrounding& grounding) {
    if (const auto valid = validateBoundary(configuration, bench, grounding); !valid) {
        return std::unexpected(valid.error());
    }
    return initialStateUnchecked(configuration);
}

std::expected<model::OralArgumentEvent, Error> planOpeningQuestion(
    const model::OralArgumentConfiguration& configuration, const model::BenchConfiguration& bench,
    const model::ArgumentGrounding& grounding, const model::OralArgumentState& state) {
    if (const auto valid = validateBoundary(configuration, bench, grounding); !valid) {
        return std::unexpected(valid.error());
    }
    if (const auto valid = validateState(configuration, bench, grounding, state); !valid) {
        return std::unexpected(valid.error());
    }
    return planOpeningUnchecked(bench, grounding, state);
}

std::expected<model::OralArgumentEvent, Error>
decideCounselAnswer(const model::OralArgumentConfiguration& configuration,
                    const model::BenchConfiguration& bench,
                    const model::ArgumentGrounding& grounding,
                    const model::OralArgumentState& state, const model::CounselAnswer& answer) {
    if (const auto valid = validateBoundary(configuration, bench, grounding); !valid) {
        return std::unexpected(valid.error());
    }
    if (const auto valid = validateState(configuration, bench, grounding, state); !valid) {
        return std::unexpected(valid.error());
    }
    if (const auto valid = validateCounselAnswer(grounding, answer); !valid) {
        return std::unexpected(valid.error());
    }
    return decideCounselAnswerUnchecked(configuration, bench, grounding, state, answer);
}

std::expected<model::OralArgumentState, Error> applyOralArgumentEvent(
    const model::OralArgumentConfiguration& configuration, const model::BenchConfiguration& bench,
    const model::ArgumentGrounding& grounding, const model::OralArgumentState& state,
    const model::OralArgumentEvent& event) {
    if (const auto valid = validateBoundary(configuration, bench, grounding); !valid) {
        return std::unexpected(valid.error());
    }
    if (const auto valid = validateState(configuration, bench, grounding, state); !valid) {
        return std::unexpected(valid.error());
    }
    if (const auto capacity = ensureEventCapacity(state); !capacity) {
        return std::unexpected(capacity.error());
    }
    if (const auto valid = validateEventShape(bench, grounding, event); !valid) {
        return std::unexpected(valid.error());
    }
    if (event.sequence != state.next_event_sequence) {
        return fail(ErrorCode::InvalidEvent, "oral-argument event sequence is invalid");
    }

    std::expected<model::OralArgumentEvent, Error> expected =
        state.phase == model::OralArgumentPhase::NotStarted
            ? planOpeningUnchecked(bench, grounding, state)
        : event.counsel.has_value()
            ? decideCounselAnswerUnchecked(configuration, bench, grounding, state, *event.counsel)
            : fail(ErrorCode::InvalidEvent, "counsel answer is missing");
    if (!expected) {
        return std::unexpected(expected.error());
    }
    if (*expected != event) {
        return fail(ErrorCode::InvalidEvent,
                    "oral-argument event differs from the deterministic decision");
    }
    return applyAcceptedEvent(configuration, state, event);
}

std::expected<model::OralArgumentState, Error> replayOralArgument(
    const model::OralArgumentConfiguration& configuration, const model::BenchConfiguration& bench,
    const model::ArgumentGrounding& grounding, const model::OralArgumentState& initial_state,
    std::span<const model::OralArgumentEvent> events) {
    if (const auto valid = validateBoundary(configuration, bench, grounding); !valid) {
        return std::unexpected(valid.error());
    }
    if (events.size() > maximum_events) {
        return fail(ErrorCode::InvalidSession, "replay journal exceeds its hard event limit");
    }
    const auto canonical = initializeOralArgument(configuration, bench, grounding);
    if (!canonical) {
        return std::unexpected(canonical.error());
    }
    if (initial_state != *canonical) {
        return fail(ErrorCode::InvalidSession,
                    "replay seed is not the canonical initialized state");
    }
    return projectJournal(configuration, bench, grounding, events);
}

std::expected<model::OralArgumentState, Error>
initializeOralArgument(const model::CanonicalOralArgumentDefinition& definition) {
    if (const auto valid = validateCanonicalBoundary(definition); !valid) {
        return std::unexpected(valid.error());
    }
    return initialCanonicalStateUnchecked(definition);
}

std::expected<model::OralArgumentEvent, Error>
planOpeningQuestion(const model::CanonicalOralArgumentDefinition& definition,
                    const model::OralArgumentState& state) {
    if (const auto valid = validateCanonicalBoundary(definition); !valid) {
        return std::unexpected(valid.error());
    }
    if (const auto valid = validateCanonicalState(definition, state); !valid) {
        return std::unexpected(valid.error());
    }
    return planAuthoredOpeningUnchecked(definition, state);
}

std::expected<model::OralArgumentEvent, Error>
decideCounselAnswer(const model::CanonicalOralArgumentDefinition& definition,
                    const model::OralArgumentState& state, const model::CounselAnswer& answer) {
    if (const auto valid = validateCanonicalBoundary(definition); !valid) {
        return std::unexpected(valid.error());
    }
    if (const auto valid = validateCanonicalState(definition, state); !valid) {
        return std::unexpected(valid.error());
    }
    if (const auto valid = validateCanonicalCounselAnswer(definition.question_bank, answer);
        !valid) {
        return std::unexpected(valid.error());
    }
    return decideAuthoredCounselAnswerUnchecked(definition, state, answer);
}

std::expected<model::OralArgumentState, Error> applyOralArgumentEvent(
    const model::CanonicalOralArgumentDefinition& definition,
    const model::OralArgumentState& state, const model::OralArgumentEvent& event) {
    if (const auto valid = validateCanonicalBoundary(definition); !valid) {
        return std::unexpected(valid.error());
    }
    if (const auto valid = validateCanonicalState(definition, state); !valid) {
        return std::unexpected(valid.error());
    }
    if (const auto capacity = ensureCanonicalEventCapacity(state); !capacity) {
        return std::unexpected(capacity.error());
    }
    if (const auto valid = validateAuthoredEventShape(definition, event); !valid) {
        return std::unexpected(valid.error());
    }
    if (event.sequence != state.next_event_sequence) {
        return fail(ErrorCode::InvalidEvent,
                    "canonical oral-argument event sequence is invalid");
    }
    auto expected = state.phase == model::OralArgumentPhase::NotStarted
                        ? planAuthoredOpeningUnchecked(definition, state)
                    : event.counsel.has_value()
                        ? decideAuthoredCounselAnswerUnchecked(definition, state, *event.counsel)
                        : fail(ErrorCode::InvalidEvent, "counsel answer is missing");
    if (!expected) {
        return std::unexpected(expected.error());
    }
    if (*expected != event) {
        return fail(ErrorCode::InvalidEvent,
                    "canonical oral event differs from the exact authored decision");
    }
    return applyAcceptedEvent(definition.configuration, state, event);
}

std::expected<model::OralArgumentState, Error> replayOralArgument(
    const model::CanonicalOralArgumentDefinition& definition,
    const model::OralArgumentState& initial_state,
    std::span<const model::OralArgumentEvent> events) {
    if (const auto valid = validateCanonicalBoundary(definition); !valid) {
        return std::unexpected(valid.error());
    }
    if (events.size() > maximum_canonical_events) {
        return fail(ErrorCode::InvalidSession,
                    "canonical replay journal exceeds its hard event limit");
    }
    const auto canonical = initializeOralArgument(definition);
    if (!canonical) {
        return std::unexpected(canonical.error());
    }
    if (initial_state != *canonical) {
        return fail(ErrorCode::InvalidSession,
                    "canonical replay seed is not the exact initialized state");
    }
    return projectAuthoredJournal(definition, events);
}

} // namespace appellate::engine
