#include "appellate/engine/oral_argument_engine.hpp"

#include <QTest>

#include <algorithm>
#include <array>
#include <chrono>
#include <expected>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace {

class OralArgumentEngineTest final : public QObject {
    Q_OBJECT

  private slots:
    void validatesOneToManyCompatibleBenches();
    void lowConfidenceAnswerProducesGroundedClarificationAndExactReplay();
    void modelsInterruptionsHypotheticalsRecordPinsAndConcessions();
    void expiresPrincipalAndRebuttalClocksAndReplays();
    void structuredStylesDifferWithoutChangingLegalPins();
    void behaviorDigestCoversOperativeProfileDataButNotIdentity();
    void rejectsMutatedDefinitionsAtEveryBoundary();
    void rejectsForgedReplaySeedsAndDefinitionSwaps();
    void enforcesBoundsEnumsAndPositiveElapsed();
    void recordPinDemandControlChangesPlanning();
    void recordClaimsWithoutRecordPagesDoNotDemandPins();
    void boundsSingleSeatFollowUpChains();
    void reusableTopicsSelectWholeAuthoredQuestionsAcrossCases();
    void canonicalDigestBindsTopicsPromptsModesAndResolvedGrounding();
    void rejectsMixedOrInvalidCanonicalDefinitionsAndSwappedSelections();
    void canonicalRecordPinsAndCounterfactualStateRemainBounded();
    void canonicalContractRejectsOperativeConfigurationSwaps();
    void enforcesCanonicalQuestionBankAndPromptBounds();
    void enforcesCanonicalEventLimitAtEveryBoundary();
};

using namespace appellate;
using namespace std::chrono_literals;

[[nodiscard]] model::JudgeProfile clippedProfile(std::string id = "fictional.clipped") {
    return model::JudgeProfile{
        std::move(id),
        "Clipped Composite",
        model::ProfileClass::FictionalComposite,
        model::ProfileCompatibility{{model::CourtRole::Appellate}, {"us.ca4"}},
        model::InteractionStyle{
            0.9,
            0.3,
            0.2,
            0.85,
            0.9,
            0.1,
            0.9,
            0.95,
            0.9,
            {model::IssueFocus{"issue.redressability", 1.0}},
        },
        model::VoiceStyle{
            model::VoiceRegister::Technical,
            model::VoiceCadence::Clipped,
            model::QuestionFraming::Direct,
            model::CounselAddress::Counsel,
            0.25,
            0.3,
            {"state the limiting rule", "answer the point"},
            {"stop there", "before you continue"},
            {"be precise", "clarify that answer"},
        },
    };
}

[[nodiscard]] model::JudgeProfile expansiveProfile(std::string id = "fictional.expansive") {
    return model::JudgeProfile{
        std::move(id),
        "Expansive Composite",
        model::ProfileClass::FictionalComposite,
        model::ProfileCompatibility{{model::CourtRole::Appellate}, {"us.ca4"}},
        model::InteractionStyle{
            0.25,
            0.95,
            0.9,
            0.1,
            0.0,
            0.9,
            0.2,
            0.2,
            0.4,
            {model::IssueFocus{"issue.record", 1.0}},
        },
        model::VoiceStyle{
            model::VoiceRegister::Formal,
            model::VoiceCadence::Expansive,
            model::QuestionFraming::Narrative,
            model::CounselAddress::Advocate,
            0.9,
            0.9,
            {"develop the governing principle", "place the issue in context"},
            {"let us pause at that premise"},
            {"help us understand your position", "draw the distinction carefully"},
        },
    };
}

[[nodiscard]] model::BenchConfiguration twoSeatBench(bool clipped_presides = true) {
    model::BenchSeat presiding{
        "seat.presiding",
        clipped_presides ? clippedProfile() : expansiveProfile(),
    };
    model::BenchSeat second{
        "seat.second",
        clipped_presides ? expansiveProfile() : clippedProfile(),
    };
    return model::BenchConfiguration{
        "us.ca4",
        model::CourtRole::Appellate,
        {std::move(presiding), std::move(second)},
        "seat.presiding",
    };
}

[[nodiscard]] model::BenchConfiguration singleSeatBench(model::JudgeProfile profile) {
    return model::BenchConfiguration{
        "us.ca4",
        model::CourtRole::Appellate,
        {model::BenchSeat{"seat.presiding", std::move(profile)}},
        "seat.presiding",
    };
}

[[nodiscard]] model::ArgumentGrounding grounding() {
    return model::ArgumentGrounding{{
        model::ArgumentIssue{
            "issue.redressability",
            "Article III redressability",
            {
                model::ArgumentGroundingRef{model::GroundingKind::Authority,
                                            "authority.article-iii", std::nullopt},
                model::ArgumentGroundingRef{model::GroundingKind::BriefPassage,
                                            "brief.appellant.12", 12},
                model::ArgumentGroundingRef{model::GroundingKind::RecordPage, "record.45", 45},
            },
            {"What concrete relief remains available?",
             "Which requested remedy redresses the asserted injury?"},
            {"Assume the challenged order expired yesterday; what relief remains?"},
        },
        model::ArgumentIssue{
            "issue.record",
            "record support for causation",
            {
                model::ArgumentGroundingRef{model::GroundingKind::Authority, "authority.rule-10",
                                            std::nullopt},
                model::ArgumentGroundingRef{model::GroundingKind::BriefPassage, "brief.appellee.19",
                                            19},
                model::ArgumentGroundingRef{model::GroundingKind::RecordPage, "record.87", 87},
            },
            {"Where does the record establish the asserted causal link?"},
            {"Assume the cited declaration is excluded; what record support remains?"},
        },
    }};
}

[[nodiscard]] model::OralArgumentConfiguration
configuration(const model::BenchConfiguration& bench, const model::ArgumentGrounding& available,
              std::chrono::seconds principal = 60s, std::chrono::seconds rebuttal = 20s) {
    const auto behavior_digest = engine::behaviorDefinitionDigest(bench);
    const auto grounding_digest = engine::groundingDigest(available);
    Q_ASSERT(behavior_digest.has_value());
    Q_ASSERT(grounding_digest.has_value());
    return model::OralArgumentConfiguration{
        principal,
        rebuttal,
        0.7,
        3,
        *behavior_digest,
        *grounding_digest,
        "legal-state.synthetic.v1",
        "disposition.authored.v1",
    };
}

[[nodiscard]] auto startedState(const model::OralArgumentConfiguration& config,
                                const model::BenchConfiguration& bench,
                                const model::ArgumentGrounding& available)
    -> std::expected<model::OralArgumentState, engine::Error> {
    const auto initial = engine::initializeOralArgument(config, bench, available);
    if (!initial) {
        return std::unexpected(initial.error());
    }
    const auto opening = engine::planOpeningQuestion(config, bench, available, *initial);
    if (!opening) {
        return std::unexpected(opening.error());
    }
    return engine::applyOralArgumentEvent(config, bench, available, *initial, *opening);
}

[[nodiscard]] model::CounselAnswer answer(model::CounselActKind kind, double confidence = 1.0,
                                          std::chrono::seconds elapsed = 5s) {
    return model::CounselAnswer{
        kind,
        "The requested relief remains available under the cited materials.",
        "issue.redressability",
        {"authority.article-iii"},
        confidence,
        elapsed,
    };
}

[[nodiscard]] model::AuthorityRef canonicalAuthority() {
    return model::AuthorityRef{
        model::AuthorityId{"authority.canonical-standard"},
        "Synthetic Authority, 100 F.4th 1",
        "2026-01-15",
        "The court reviews preserved legal questions under the authored standard.",
        model::AuthorityProvenance{
            model::AuthorityType::Case,
            "us.ca4",
            "court.synthetic-appellate",
            model::PrecedentialStatus::Precedential,
            true,
            "2026-01-16",
            "100 F.4th 1",
            "https://court.example.test/opinions/100-f4th-1",
        },
    };
}

[[nodiscard]] model::BenchConfiguration reusableTopicBench() {
    auto reusable = clippedProfile("fictional.reusable-composite");
    reusable.display_name = "Reusable Composite";
    reusable.interaction.issue_focus = {
        model::IssueFocus{"workbench.topic.record-support", 1.0},
        model::IssueFocus{"workbench.topic.remedy", 0.25},
    };
    return singleSeatBench(std::move(reusable));
}

[[nodiscard]] model::AuthoredQuestionBank authoredBank(
    std::string case_id, std::string issue_id,
    model::OralArgumentMode mode = model::OralArgumentMode::ActualRecord) {
    const auto prefix = case_id;
    return model::AuthoredQuestionBank{
        model::CaseId{std::move(case_id)},
        prefix + ".argument",
        mode,
        {},
        {model::ArgumentIssueTopics{
            issue_id,
            {model::ArgumentFocusTopic::RecordSupport, model::ArgumentFocusTopic::Remedy},
        }},
        {
            model::AuthoredArgumentQuestion{
                prefix + ".question-record",
                issue_id,
                model::ArgumentFocusTopic::RecordSupport,
                "Where does the exact record support the asserted proposition?",
                {
                    model::AuthorityArgumentGrounding{
                        prefix + ".grounding-authority", canonicalAuthority()},
                    model::RecordPageArgumentGrounding{
                        prefix + ".grounding-record", prefix + ".anchor-hearing",
                        prefix + ".entry-hearing", 47, std::string(64, 'a'),
                        std::string{"Hearing Tr. 47"}},
                },
            },
            model::AuthoredArgumentQuestion{
                prefix + ".question-remedy",
                issue_id,
                model::ArgumentFocusTopic::Remedy,
                "What relief follows if the court accepts that record proposition?",
                {model::BriefPageArgumentGrounding{
                    prefix + ".grounding-brief", prefix + ".entry-opening-brief", 12,
                    std::string(64, 'b')}},
            },
        },
    };
}

[[nodiscard]] model::CanonicalOralArgumentDefinition canonicalDefinition(
    std::string case_id = "case.alpha", std::string issue_id = "issue.alpha",
    model::OralArgumentMode mode = model::OralArgumentMode::ActualRecord) {
    auto bank = authoredBank(std::move(case_id), std::move(issue_id), mode);
    auto bench = reusableTopicBench();
    const auto behavior = engine::behaviorDefinitionDigest(bench);
    const auto digest = engine::groundingDigest(bank);
    Q_ASSERT(behavior.has_value());
    Q_ASSERT(digest.has_value());
    bank.grounding_digest = *digest;
    return model::CanonicalOralArgumentDefinition{
        model::OralArgumentConfiguration{
            90s,
            20s,
            0.7,
            3,
            *behavior,
            *digest,
            std::string(64, 'c'),
            "operation.authored-judgment",
        },
        std::move(bench),
        std::move(bank),
    };
}

[[nodiscard]] model::AuthoredQuestionBank emptyBoundedBank() {
    return model::AuthoredQuestionBank{
        model::CaseId{"case.bounds"},
        "case.bounds.argument",
        model::OralArgumentMode::ActualRecord,
        {},
        {},
        {},
    };
}

[[nodiscard]] std::string boundedIssueId(std::size_t issue_index) {
    return "case.bounds.issue-" + std::to_string(issue_index);
}

void appendBoundedIssue(model::AuthoredQuestionBank& bank, std::size_t issue_index) {
    bank.issue_topics.push_back(model::ArgumentIssueTopics{
        boundedIssueId(issue_index),
        {model::ArgumentFocusTopic::Merits},
    });
}

void appendBoundedQuestion(model::AuthoredQuestionBank& bank, std::size_t issue_index,
                           std::size_t question_index, std::size_t grounding_count = 1) {
    const auto issue_id = boundedIssueId(issue_index);
    std::vector<model::AuthoredArgumentGrounding> grounding;
    grounding.reserve(grounding_count);
    for (std::size_t grounding_index = 0; grounding_index < grounding_count;
         ++grounding_index) {
        const auto suffix = std::to_string(issue_index) + "-" +
                            std::to_string(question_index) + "-" +
                            std::to_string(grounding_index);
        grounding.push_back(model::BriefPageArgumentGrounding{
            "case.bounds.grounding-" + suffix,
            "case.bounds.entry-" + suffix,
            1,
            std::string(64, 'a'),
        });
    }
    bank.questions.push_back(model::AuthoredArgumentQuestion{
        "case.bounds.question-" + std::to_string(issue_index) + "-" +
            std::to_string(question_index),
        issue_id,
        model::ArgumentFocusTopic::Merits,
        "Which authored source controls issue " + std::to_string(issue_index) + "?",
        std::move(grounding),
    });
}

void OralArgumentEngineTest::validatesOneToManyCompatibleBenches() {
    const auto available = grounding();

    const auto one = singleSeatBench(clippedProfile());
    const auto one_config = configuration(one, available);
    QVERIFY(engine::initializeOralArgument(one_config, one, available).has_value());
    const auto one_behavior_digest = engine::behaviorDefinitionDigest(one);
    const auto available_grounding_digest = engine::groundingDigest(available);
    QVERIFY(one_behavior_digest.has_value());
    QVERIFY(available_grounding_digest.has_value());
    QCOMPARE(*one_behavior_digest,
             std::string("184c5564c5a7e363105d771c58fc36afe8d3feec73b409401254637f544710f6"));
    QCOMPARE(*available_grounding_digest,
             std::string("b1b695e2cd34b689e1b3f39e1530d65e1d598de848c8a9e610405eadfba93ad8"));

    auto district_profile = clippedProfile("fictional.district");
    district_profile.display_name = "District Composite";
    district_profile.compatibility.court_roles = {model::CourtRole::District};
    district_profile.compatibility.jurisdiction_ids = {"us.vaed"};
    const model::BenchConfiguration district{
        "us.vaed",
        model::CourtRole::District,
        {model::BenchSeat{"seat.district", std::move(district_profile)}},
        "seat.district",
    };
    const auto district_config = configuration(district, available);
    QVERIFY(engine::initializeOralArgument(district_config, district, available).has_value());

    auto three = twoSeatBench();
    three.seats.push_back(model::BenchSeat{"seat.third", expansiveProfile("fictional.third")});
    const auto three_config = configuration(three, available);
    QVERIFY(engine::initializeOralArgument(three_config, three, available).has_value());

    auto future_en_banc = one;
    for (int index = 1; index < 9; ++index) {
        future_en_banc.seats.push_back(model::BenchSeat{
            "seat.en-banc-" + std::to_string(index),
            clippedProfile("fictional.en-banc-" + std::to_string(index)),
        });
    }
    const auto en_banc_config = configuration(future_en_banc, available);
    QVERIFY(engine::initializeOralArgument(en_banc_config, future_en_banc, available).has_value());

    auto empty = one;
    empty.seats.clear();
    QVERIFY(!engine::initializeOralArgument(one_config, empty, available).has_value());

    auto missing_presiding = one;
    missing_presiding.presiding_seat_id = "seat.absent";
    QVERIFY(!engine::initializeOralArgument(one_config, missing_presiding, available).has_value());

    auto wrong_jurisdiction = one;
    wrong_jurisdiction.jurisdiction_id = "us.ca9";
    QVERIFY(!engine::initializeOralArgument(one_config, wrong_jurisdiction, available).has_value());

    auto wrong_role = one;
    wrong_role.court_role = model::CourtRole::District;
    QVERIFY(!engine::initializeOralArgument(one_config, wrong_role, available).has_value());
}

void OralArgumentEngineTest::lowConfidenceAnswerProducesGroundedClarificationAndExactReplay() {
    const auto bench = singleSeatBench(clippedProfile());
    const auto available = grounding();
    const auto config = configuration(bench, available);
    const auto initial = engine::initializeOralArgument(config, bench, available);
    QVERIFY(initial.has_value());
    const auto started = startedState(config, bench, available);
    QVERIFY(started.has_value());

    const auto uncertain = answer(model::CounselActKind::Answer, 0.35);
    const auto decision =
        engine::decideCounselAnswer(config, bench, available, *started, uncertain);
    QVERIFY(decision.has_value());
    QCOMPARE(decision->bench.kind, model::BenchActKind::ClarificationRequest);
    QVERIFY(decision->bench.question.has_value());
    QVERIFY(!std::get<model::LegacyQuestionSelection>(decision->bench.question->selection)
                 .grounding.empty());
    QCOMPARE(decision->bench.question->issue_id, std::string("issue.redressability"));
    QVERIFY(!decision->bench.rendered_utterance.empty());

    const auto applied =
        engine::applyOralArgumentEvent(config, bench, available, *started, *decision);
    QVERIFY(applied.has_value());
    QCOMPARE(applied->transcript.back().utterance, decision->bench.rendered_utterance);

    const auto replayed =
        engine::replayOralArgument(config, bench, available, *initial, applied->journal);
    QVERIFY(replayed.has_value());
    QVERIFY(*replayed == *applied);
}

void OralArgumentEngineTest::modelsInterruptionsHypotheticalsRecordPinsAndConcessions() {
    const auto available = grounding();

    const auto clipped = singleSeatBench(clippedProfile());
    const auto clipped_config = configuration(clipped, available);
    const auto clipped_started = startedState(clipped_config, clipped, available);
    QVERIFY(clipped_started.has_value());
    const auto interruption =
        engine::decideCounselAnswer(clipped_config, clipped, available, *clipped_started,
                                    answer(model::CounselActKind::Answer));
    QVERIFY(interruption.has_value());
    QCOMPARE(interruption->bench.kind, model::BenchActKind::Interruption);
    QVERIFY(interruption->bench.question->parent_act_sequence.has_value());

    const auto expansive = singleSeatBench(expansiveProfile());
    const auto expansive_config = configuration(expansive, available);
    const auto expansive_started = startedState(expansive_config, expansive, available);
    QVERIFY(expansive_started.has_value());
    const auto hypothetical =
        engine::decideCounselAnswer(expansive_config, expansive, available, *expansive_started,
                                    answer(model::CounselActKind::Answer));
    QVERIFY(hypothetical.has_value());
    QCOMPARE(hypothetical->bench.kind, model::BenchActKind::Hypothetical);

    const auto record_demand =
        engine::decideCounselAnswer(clipped_config, clipped, available, *clipped_started,
                                    answer(model::CounselActKind::RecordClaim));
    QVERIFY(record_demand.has_value());
    QCOMPARE(record_demand->bench.kind, model::BenchActKind::RecordPinDemand);
    QVERIFY(std::ranges::any_of(
        std::get<model::LegacyQuestionSelection>(record_demand->bench.question->selection).grounding,
        [](const auto& item) {
        return item.kind == model::GroundingKind::RecordPage;
        }));

    const auto concession =
        engine::decideCounselAnswer(clipped_config, clipped, available, *clipped_started,
                                    answer(model::CounselActKind::Concession));
    QVERIFY(concession.has_value());
    QCOMPARE(concession->bench.kind, model::BenchActKind::FollowUp);
    QVERIFY(concession->bench.question->recalls_concession);
    const auto concession_applied = engine::applyOralArgumentEvent(
        clipped_config, clipped, available, *clipped_started, *concession);
    QVERIFY(concession_applied.has_value());
    QCOMPARE(concession_applied->concessions.size(), std::size_t{1});
    QCOMPARE(concession_applied->follow_up_depth, std::uint32_t{1});

    for (const auto* event : {&*interruption, &*hypothetical, &*record_demand, &*concession}) {
        QVERIFY(engine::isQuestionAct(event->bench.kind));
        QVERIFY(event->bench.question.has_value());
        QVERIFY(!std::get<model::LegacyQuestionSelection>(event->bench.question->selection)
                     .grounding.empty());
        QVERIFY(!event->bench.question->issue_id.empty());
    }
}

void OralArgumentEngineTest::expiresPrincipalAndRebuttalClocksAndReplays() {
    const auto bench = singleSeatBench(clippedProfile());
    const auto available = grounding();
    const auto config = configuration(bench, available, 10s, 5s);
    const auto initial = engine::initializeOralArgument(config, bench, available);
    QVERIFY(initial.has_value());
    const auto started = startedState(config, bench, available);
    QVERIFY(started.has_value());

    const auto principal_expiration = engine::decideCounselAnswer(
        config, bench, available, *started, answer(model::CounselActKind::Answer, 1.0, 10s));
    QVERIFY(principal_expiration.has_value());
    QCOMPARE(principal_expiration->bench.kind, model::BenchActKind::TimeExpired);
    const auto rebuttal =
        engine::applyOralArgumentEvent(config, bench, available, *started, *principal_expiration);
    QVERIFY(rebuttal.has_value());
    QCOMPARE(rebuttal->phase, model::OralArgumentPhase::Rebuttal);
    QCOMPARE(rebuttal->principal_remaining, 0s);
    QCOMPARE(rebuttal->rebuttal_remaining, 5s);

    const auto rebuttal_expiration = engine::decideCounselAnswer(
        config, bench, available, *rebuttal, answer(model::CounselActKind::Answer, 1.0, 5s));
    QVERIFY(rebuttal_expiration.has_value());
    const auto complete =
        engine::applyOralArgumentEvent(config, bench, available, *rebuttal, *rebuttal_expiration);
    QVERIFY(complete.has_value());
    QCOMPARE(complete->phase, model::OralArgumentPhase::Complete);
    QCOMPARE(complete->rebuttal_remaining, 0s);
    QVERIFY(!engine::decideCounselAnswer(config, bench, available, *complete,
                                         answer(model::CounselActKind::Answer))
                 .has_value());

    const auto replayed =
        engine::replayOralArgument(config, bench, available, *initial, complete->journal);
    QVERIFY(replayed.has_value());
    QVERIFY(*replayed == *complete);
}

void OralArgumentEngineTest::structuredStylesDifferWithoutChangingLegalPins() {
    const auto available = grounding();
    const auto clipped_bench = twoSeatBench(true);
    const auto expansive_bench = twoSeatBench(false);
    const auto clipped_config = configuration(clipped_bench, available);
    const auto expansive_config = configuration(expansive_bench, available);

    const auto clipped_initial =
        engine::initializeOralArgument(clipped_config, clipped_bench, available);
    const auto expansive_initial =
        engine::initializeOralArgument(expansive_config, expansive_bench, available);
    QVERIFY(clipped_initial.has_value());
    QVERIFY(expansive_initial.has_value());
    const auto clipped_opening =
        engine::planOpeningQuestion(clipped_config, clipped_bench, available, *clipped_initial);
    const auto expansive_opening = engine::planOpeningQuestion(expansive_config, expansive_bench,
                                                               available, *expansive_initial);
    QVERIFY(clipped_opening.has_value());
    QVERIFY(expansive_opening.has_value());
    QVERIFY(clipped_opening->bench.rendered_utterance !=
            expansive_opening->bench.rendered_utterance);

    const auto clipped_started = engine::applyOralArgumentEvent(
        clipped_config, clipped_bench, available, *clipped_initial, *clipped_opening);
    const auto expansive_started = engine::applyOralArgumentEvent(
        expansive_config, expansive_bench, available, *expansive_initial, *expansive_opening);
    QVERIFY(clipped_started.has_value());
    QVERIFY(expansive_started.has_value());
    const auto response = answer(model::CounselActKind::Answer);
    const auto clipped_next = engine::decideCounselAnswer(clipped_config, clipped_bench, available,
                                                          *clipped_started, response);
    const auto expansive_next = engine::decideCounselAnswer(
        expansive_config, expansive_bench, available, *expansive_started, response);
    QVERIFY(clipped_next.has_value());
    QVERIFY(expansive_next.has_value());
    QCOMPARE(clipped_next->bench.seat_id, std::string("seat.presiding"));
    QCOMPARE(expansive_next->bench.seat_id, std::string("seat.second"));

    const auto clipped_after = engine::applyOralArgumentEvent(
        clipped_config, clipped_bench, available, *clipped_started, *clipped_next);
    const auto expansive_after = engine::applyOralArgumentEvent(
        expansive_config, expansive_bench, available, *expansive_started, *expansive_next);
    QVERIFY(clipped_after.has_value());
    QVERIFY(expansive_after.has_value());
    QCOMPARE(clipped_after->phase, expansive_after->phase);
    QCOMPARE(clipped_after->principal_remaining, expansive_after->principal_remaining);
    QCOMPARE(clipped_after->legal_state_digest, expansive_after->legal_state_digest);
    QCOMPARE(clipped_after->authored_disposition_id, expansive_after->authored_disposition_id);
    QVERIFY(clipped_after->transcript != expansive_after->transcript);
    const auto clipped_replay = engine::replayOralArgument(
        clipped_config, clipped_bench, available, *clipped_initial, clipped_after->journal);
    const auto expansive_replay = engine::replayOralArgument(
        expansive_config, expansive_bench, available, *expansive_initial, expansive_after->journal);
    QVERIFY(clipped_replay.has_value());
    QVERIFY(expansive_replay.has_value());
    QCOMPARE(*clipped_replay, *clipped_after);
    QCOMPARE(*expansive_replay, *expansive_after);

    auto renamed = clipped_bench;
    renamed.seats.front().profile.id = "fictional.renamed-with-identical-style";
    renamed.seats.front().profile.display_name = "Renamed Composite";
    const auto renamed_digest = engine::behaviorDefinitionDigest(renamed);
    QVERIFY(renamed_digest.has_value());
    QCOMPARE(*renamed_digest, clipped_config.behavior_definition_digest);
    const auto renamed_initial = engine::initializeOralArgument(clipped_config, renamed, available);
    QVERIFY(renamed_initial.has_value());
    const auto renamed_opening =
        engine::planOpeningQuestion(clipped_config, renamed, available, *renamed_initial);
    QVERIFY(renamed_opening.has_value());
    QVERIFY(*renamed_opening == *clipped_opening);

    auto voice_only = clipped_bench;
    voice_only.seats.front().profile.voice.question_phrases = {
        "focus on the exact boundary",
        "identify the necessary premise",
    };
    voice_only.seats.front().profile.voice.question_framing = model::QuestionFraming::Socratic;
    const auto voice_config = configuration(voice_only, available);
    QCOMPARE(voice_config.legal_state_digest, clipped_config.legal_state_digest);
    QCOMPARE(voice_config.authored_disposition_id, clipped_config.authored_disposition_id);
    QVERIFY(voice_config.behavior_definition_digest != clipped_config.behavior_definition_digest);
    const auto voice_initial = engine::initializeOralArgument(voice_config, voice_only, available);
    QVERIFY(voice_initial.has_value());
    const auto voice_opening =
        engine::planOpeningQuestion(voice_config, voice_only, available, *voice_initial);
    QVERIFY(voice_opening.has_value());
    QVERIFY(voice_opening->bench.rendered_utterance != clipped_opening->bench.rendered_utterance);
    QCOMPARE(voice_initial->legal_state_digest, clipped_initial->legal_state_digest);
    QCOMPARE(voice_initial->authored_disposition_id, clipped_initial->authored_disposition_id);
}

void OralArgumentEngineTest::behaviorDigestCoversOperativeProfileDataButNotIdentity() {
    const auto bench = singleSeatBench(clippedProfile());
    const auto baseline = engine::behaviorDefinitionDigest(bench);
    QVERIFY(baseline.has_value());

    const auto changes_digest = [&bench, &baseline](auto mutate) {
        auto changed = bench;
        mutate(changed.seats.front().profile);
        const auto digest = engine::behaviorDefinitionDigest(changed);
        return digest.has_value() && *digest != *baseline;
    };
    QVERIFY(changes_digest([](model::JudgeProfile& profile) {
        profile.compatibility.court_roles.push_back(model::CourtRole::District);
    }));
    QVERIFY(changes_digest([](model::JudgeProfile& profile) {
        profile.compatibility.jurisdiction_ids.push_back("us.ca9");
    }));
    QVERIFY(
        changes_digest([](model::JudgeProfile& profile) { profile.interaction.directness = 0.1; }));
    QVERIFY(
        changes_digest([](model::JudgeProfile& profile) { profile.interaction.formality = 0.1; }));
    QVERIFY(changes_digest(
        [](model::JudgeProfile& profile) { profile.interaction.question_length = 0.8; }));
    QVERIFY(changes_digest(
        [](model::JudgeProfile& profile) { profile.interaction.interruption_frequency = 0.1; }));
    QVERIFY(changes_digest(
        [](model::JudgeProfile& profile) { profile.interaction.follow_up_depth = 0.1; }));
    QVERIFY(changes_digest(
        [](model::JudgeProfile& profile) { profile.interaction.hypothetical_frequency = 0.8; }));
    QVERIFY(changes_digest(
        [](model::JudgeProfile& profile) { profile.interaction.concession_recall = 0.1; }));
    QVERIFY(changes_digest(
        [](model::JudgeProfile& profile) { profile.interaction.record_pin_demand = 0.1; }));
    QVERIFY(changes_digest(
        [](model::JudgeProfile& profile) { profile.interaction.time_strictness = 0.1; }));
    QVERIFY(changes_digest([](model::JudgeProfile& profile) {
        profile.interaction.issue_focus.front().weight = 0.2;
    }));
    QVERIFY(changes_digest([](model::JudgeProfile& profile) {
        profile.voice.register_style = model::VoiceRegister::Plain;
    }));
    QVERIFY(changes_digest([](model::JudgeProfile& profile) {
        profile.voice.cadence = model::VoiceCadence::Measured;
    }));
    QVERIFY(changes_digest([](model::JudgeProfile& profile) {
        profile.voice.question_framing = model::QuestionFraming::Socratic;
    }));
    QVERIFY(changes_digest([](model::JudgeProfile& profile) {
        profile.voice.address_convention = model::CounselAddress::Advocate;
    }));
    QVERIFY(changes_digest([](model::JudgeProfile& profile) { profile.voice.verbosity = 0.8; }));
    QVERIFY(changes_digest(
        [](model::JudgeProfile& profile) { profile.voice.sentence_complexity = 0.8; }));
    QVERIFY(changes_digest([](model::JudgeProfile& profile) {
        profile.voice.question_phrases.front() = "test the boundary";
    }));
    QVERIFY(changes_digest([](model::JudgeProfile& profile) {
        profile.voice.interruption_phrases.front() = "pause on that premise";
    }));
    QVERIFY(changes_digest([](model::JudgeProfile& profile) {
        profile.voice.clarification_phrases.front() = "clarify the exact point";
    }));

    auto renamed = bench;
    renamed.seats.front().profile.id = "fictional.identity-only-change";
    renamed.seats.front().profile.display_name = "Identity-only Composite";
    const auto renamed_digest = engine::behaviorDefinitionDigest(renamed);
    QVERIFY(renamed_digest.has_value());
    QCOMPARE(*renamed_digest, *baseline);
}

void OralArgumentEngineTest::rejectsMutatedDefinitionsAtEveryBoundary() {
    const auto bench = singleSeatBench(clippedProfile());
    const auto available = grounding();
    const auto config = configuration(bench, available);
    const auto initial = engine::initializeOralArgument(config, bench, available);
    const auto started = startedState(config, bench, available);
    QVERIFY(initial.has_value());
    QVERIFY(started.has_value());
    const auto opening = engine::planOpeningQuestion(config, bench, available, *initial);
    QVERIFY(opening.has_value());

    auto empty_bench = bench;
    empty_bench.seats.clear();
    QVERIFY(!engine::planOpeningQuestion(config, empty_bench, available, *initial).has_value());
    QVERIFY(!engine::decideCounselAnswer(config, empty_bench, available, *started,
                                         answer(model::CounselActKind::Answer))
                 .has_value());
    QVERIFY(!engine::applyOralArgumentEvent(config, empty_bench, available, *initial, *opening)
                 .has_value());
    QVERIFY(!engine::replayOralArgument(config, empty_bench, available, *initial, started->journal)
                 .has_value());

    model::ArgumentGrounding empty_grounding;
    QVERIFY(!engine::planOpeningQuestion(config, bench, empty_grounding, *initial).has_value());
    QVERIFY(!engine::decideCounselAnswer(config, bench, empty_grounding, *started,
                                         answer(model::CounselActKind::Answer))
                 .has_value());
    QVERIFY(!engine::applyOralArgumentEvent(config, bench, empty_grounding, *initial, *opening)
                 .has_value());
    QVERIFY(!engine::replayOralArgument(config, bench, empty_grounding, *initial, started->journal)
                 .has_value());

    auto mutated_bench = bench;
    mutated_bench.seats.front().profile.interaction.directness = 0.1;
    QVERIFY(!engine::decideCounselAnswer(config, mutated_bench, available, *started,
                                         answer(model::CounselActKind::Answer))
                 .has_value());

    auto mutated_grounding = available;
    mutated_grounding.issues.front().label = "A substituted issue definition";
    QVERIFY(!engine::decideCounselAnswer(config, bench, mutated_grounding, *started,
                                         answer(model::CounselActKind::Answer))
                 .has_value());
}

void OralArgumentEngineTest::rejectsForgedReplaySeedsAndDefinitionSwaps() {
    const auto bench = singleSeatBench(clippedProfile());
    const auto available = grounding();
    const auto config = configuration(bench, available);
    const auto initial = engine::initializeOralArgument(config, bench, available);
    const auto started = startedState(config, bench, available);
    QVERIFY(initial.has_value());
    QVERIFY(started.has_value());

    auto forged_seed = *initial;
    forged_seed.principal_remaining -= 1s;
    QVERIFY(!engine::replayOralArgument(config, bench, available, forged_seed, started->journal)
                 .has_value());
    forged_seed = *initial;
    forged_seed.behavior_definition_digest.assign(64, '0');
    QVERIFY(!engine::replayOralArgument(config, bench, available, forged_seed, started->journal)
                 .has_value());

    auto forged_projection = *started;
    forged_projection.journal.front().bench.rendered_utterance += " Forged.";
    forged_projection.transcript.front().utterance =
        forged_projection.journal.front().bench.rendered_utterance;
    QVERIFY(!engine::decideCounselAnswer(config, bench, available, forged_projection,
                                         answer(model::CounselActKind::Answer))
                 .has_value());

    auto changed_bench = bench;
    changed_bench.seats.front().profile.interaction.formality = 0.8;
    auto changed_config = config;
    const auto changed_behavior_digest = engine::behaviorDefinitionDigest(changed_bench);
    QVERIFY(changed_behavior_digest.has_value());
    changed_config.behavior_definition_digest = *changed_behavior_digest;
    QVERIFY(!engine::decideCounselAnswer(changed_config, changed_bench, available, *started,
                                         answer(model::CounselActKind::Answer))
                 .has_value());

    auto changed_grounding = available;
    changed_grounding.issues.front().question_prompts.front() = "A substituted grounded question?";
    auto changed_grounding_config = config;
    const auto changed_grounding_digest = engine::groundingDigest(changed_grounding);
    QVERIFY(changed_grounding_digest.has_value());
    changed_grounding_config.grounding_digest = *changed_grounding_digest;
    QVERIFY(!engine::decideCounselAnswer(changed_grounding_config, bench, changed_grounding,
                                         *started, answer(model::CounselActKind::Answer))
                 .has_value());
}

void OralArgumentEngineTest::enforcesBoundsEnumsAndPositiveElapsed() {
    const auto bench = singleSeatBench(clippedProfile());
    const auto available = grounding();
    const auto config = configuration(bench, available);
    const auto initial = engine::initializeOralArgument(config, bench, available);
    const auto started = startedState(config, bench, available);
    QVERIFY(initial.has_value());
    QVERIFY(started.has_value());

    auto invalid_bench = bench;
    invalid_bench.court_role = static_cast<model::CourtRole>(255);
    QVERIFY(!engine::initializeOralArgument(config, invalid_bench, available).has_value());
    invalid_bench = bench;
    invalid_bench.seats.front().profile.voice.question_framing =
        static_cast<model::QuestionFraming>(255);
    QVERIFY(!engine::behaviorDefinitionDigest(invalid_bench).has_value());
    invalid_bench = bench;
    invalid_bench.seats.front().profile.voice.address_convention =
        static_cast<model::CounselAddress>(255);
    QVERIFY(!engine::behaviorDefinitionDigest(invalid_bench).has_value());
    invalid_bench = bench;
    invalid_bench.seats.front().profile.voice.question_phrases = {"duplicate", "duplicate"};
    QVERIFY(!engine::behaviorDefinitionDigest(invalid_bench).has_value());
    invalid_bench = bench;
    invalid_bench.seats.front().profile.voice.interruption_phrases = {"unknown {template}"};
    QVERIFY(!engine::behaviorDefinitionDigest(invalid_bench).has_value());
    invalid_bench = bench;
    invalid_bench.seats.front().profile.voice.clarification_phrases.clear();
    QVERIFY(!engine::behaviorDefinitionDigest(invalid_bench).has_value());
    invalid_bench = bench;
    invalid_bench.seats.front().profile.profile_class = static_cast<model::ProfileClass>(255);
    QVERIFY(!engine::behaviorDefinitionDigest(invalid_bench).has_value());

    auto invalid_grounding = available;
    invalid_grounding.issues.front().grounding.front().kind =
        static_cast<model::GroundingKind>(255);
    QVERIFY(!engine::groundingDigest(invalid_grounding).has_value());

    auto zero_elapsed = answer(model::CounselActKind::Answer, 1.0, 0s);
    QVERIFY(
        !engine::decideCounselAnswer(config, bench, available, *started, zero_elapsed).has_value());
    auto invalid_answer = answer(static_cast<model::CounselActKind>(255));
    QVERIFY(!engine::decideCounselAnswer(config, bench, available, *started, invalid_answer)
                 .has_value());
    auto duplicate_citations = answer(model::CounselActKind::Answer);
    duplicate_citations.cited_grounding_ids.push_back("authority.article-iii");
    QVERIFY(!engine::decideCounselAnswer(config, bench, available, *started, duplicate_citations)
                 .has_value());
    auto excessive_citations = answer(model::CounselActKind::Answer);
    excessive_citations.cited_grounding_ids.clear();
    for (int index = 0; index < 33; ++index) {
        excessive_citations.cited_grounding_ids.push_back("citation." + std::to_string(index));
    }
    QVERIFY(!engine::decideCounselAnswer(config, bench, available, *started, excessive_citations)
                 .has_value());

    auto invalid_event = engine::decideCounselAnswer(config, bench, available, *started,
                                                     answer(model::CounselActKind::Answer));
    QVERIFY(invalid_event.has_value());
    invalid_event->bench.kind = static_cast<model::BenchActKind>(255);
    QVERIFY(!engine::applyOralArgumentEvent(config, bench, available, *started, *invalid_event)
                 .has_value());
    QVERIFY(!engine::isQuestionAct(static_cast<model::BenchActKind>(255)));

    auto excessive_bench = bench;
    for (int index = 1; index < 33; ++index) {
        excessive_bench.seats.push_back(model::BenchSeat{
            "seat." + std::to_string(index), clippedProfile("fictional." + std::to_string(index))});
    }
    QVERIFY(!engine::behaviorDefinitionDigest(excessive_bench).has_value());

    auto excessive_grounding = available;
    excessive_grounding.issues.clear();
    for (int index = 0; index < 257; ++index) {
        auto issue = available.issues.front();
        issue.id = "issue." + std::to_string(index);
        excessive_grounding.issues.push_back(std::move(issue));
    }
    QVERIFY(!engine::groundingDigest(excessive_grounding).has_value());

    auto excessive_history = *started;
    excessive_history.journal.resize(4'097);
    QVERIFY(!engine::decideCounselAnswer(config, bench, available, excessive_history,
                                         answer(model::CounselActKind::Answer))
                 .has_value());
    const std::vector<model::OralArgumentEvent> excessive_events(4'097);
    QVERIFY(!engine::replayOralArgument(config, bench, available, *initial, excessive_events)
                 .has_value());
    auto invalid_phase = *started;
    invalid_phase.phase = static_cast<model::OralArgumentPhase>(255);
    QVERIFY(!engine::decideCounselAnswer(config, bench, available, invalid_phase,
                                         answer(model::CounselActKind::Answer))
                 .has_value());
}

void OralArgumentEngineTest::recordPinDemandControlChangesPlanning() {
    const auto available = grounding();
    auto demanding_profile = clippedProfile("fictional.record-demanding");
    demanding_profile.interaction.interruption_frequency = 0.0;
    demanding_profile.interaction.hypothetical_frequency = 0.0;
    demanding_profile.interaction.record_pin_demand = 1.0;
    auto restrained_profile = demanding_profile;
    restrained_profile.id = "fictional.record-restrained";
    restrained_profile.display_name = "Record-restrained Composite";
    restrained_profile.interaction.record_pin_demand = 0.0;

    const auto demanding = singleSeatBench(std::move(demanding_profile));
    const auto restrained = singleSeatBench(std::move(restrained_profile));
    const auto demanding_config = configuration(demanding, available);
    const auto restrained_config = configuration(restrained, available);
    const auto demanding_started = startedState(demanding_config, demanding, available);
    const auto restrained_started = startedState(restrained_config, restrained, available);
    QVERIFY(demanding_started.has_value());
    QVERIFY(restrained_started.has_value());

    const auto record_claim = answer(model::CounselActKind::RecordClaim);
    const auto demand = engine::decideCounselAnswer(demanding_config, demanding, available,
                                                    *demanding_started, record_claim);
    const auto restraint = engine::decideCounselAnswer(restrained_config, restrained, available,
                                                       *restrained_started, record_claim);
    QVERIFY(demand.has_value());
    QVERIFY(restraint.has_value());
    QCOMPARE(demand->bench.kind, model::BenchActKind::RecordPinDemand);
    QVERIFY(restraint->bench.kind != model::BenchActKind::RecordPinDemand);
    QCOMPARE(demanding_started->legal_state_digest, restrained_started->legal_state_digest);
    QCOMPARE(demanding_started->authored_disposition_id,
             restrained_started->authored_disposition_id);
}

void OralArgumentEngineTest::recordClaimsWithoutRecordPagesDoNotDemandPins() {
    auto available = grounding();
    available.issues.resize(1);
    auto& references = available.issues.front().grounding;
    std::erase_if(references, [](const model::ArgumentGroundingRef& reference) {
        return reference.kind == model::GroundingKind::RecordPage;
    });
    const auto bench = singleSeatBench(clippedProfile());
    const auto config = configuration(bench, available);
    const auto started = startedState(config, bench, available);
    QVERIFY(started.has_value());

    const auto decision = engine::decideCounselAnswer(config, bench, available, *started,
                                                      answer(model::CounselActKind::RecordClaim));
    QVERIFY(decision.has_value());
    QVERIFY(decision->bench.kind != model::BenchActKind::RecordPinDemand);
    QVERIFY(decision->bench.question.has_value());
    QVERIFY(std::ranges::none_of(
        std::get<model::LegacyQuestionSelection>(decision->bench.question->selection).grounding,
        [](const auto& reference) {
        return reference.kind == model::GroundingKind::RecordPage;
        }));
}

void OralArgumentEngineTest::boundsSingleSeatFollowUpChains() {
    auto profile = clippedProfile("fictional.follow-up");
    profile.display_name = "Follow-up Composite";
    profile.interaction.interruption_frequency = 0.0;
    profile.interaction.hypothetical_frequency = 0.0;
    profile.interaction.concession_recall = 0.0;
    profile.interaction.follow_up_depth = 1.0;
    const auto bench = singleSeatBench(std::move(profile));
    const auto available = grounding();
    auto config = configuration(bench, available);
    config.maximum_follow_up_depth = 2;
    const auto started = startedState(config, bench, available);
    QVERIFY(started.has_value());

    const auto first = engine::decideCounselAnswer(config, bench, available, *started,
                                                   answer(model::CounselActKind::Answer, 1.0, 1s));
    QVERIFY(first.has_value());
    QCOMPARE(first->bench.kind, model::BenchActKind::FollowUp);
    const auto after_first =
        engine::applyOralArgumentEvent(config, bench, available, *started, *first);
    QVERIFY(after_first.has_value());
    QCOMPARE(after_first->follow_up_depth, std::uint32_t{1});

    const auto second = engine::decideCounselAnswer(config, bench, available, *after_first,
                                                    answer(model::CounselActKind::Answer, 1.0, 1s));
    QVERIFY(second.has_value());
    QCOMPARE(second->bench.kind, model::BenchActKind::FollowUp);
    const auto after_second =
        engine::applyOralArgumentEvent(config, bench, available, *after_first, *second);
    QVERIFY(after_second.has_value());
    QCOMPARE(after_second->follow_up_depth, std::uint32_t{2});

    const auto reset_question = engine::decideCounselAnswer(
        config, bench, available, *after_second, answer(model::CounselActKind::Answer, 1.0, 1s));
    QVERIFY(reset_question.has_value());
    QCOMPARE(reset_question->bench.kind, model::BenchActKind::Question);
    const auto reset_state =
        engine::applyOralArgumentEvent(config, bench, available, *after_second, *reset_question);
    QVERIFY(reset_state.has_value());
    QCOMPARE(reset_state->follow_up_depth, std::uint32_t{0});

    const auto restarted_chain = engine::decideCounselAnswer(
        config, bench, available, *reset_state, answer(model::CounselActKind::Answer, 1.0, 1s));
    QVERIFY(restarted_chain.has_value());
    QCOMPARE(restarted_chain->bench.kind, model::BenchActKind::FollowUp);
}

void OralArgumentEngineTest::reusableTopicsSelectWholeAuthoredQuestionsAcrossCases() {
    const auto first = canonicalDefinition("case.alpha", "issue.unrelated-alpha");
    const auto second = canonicalDefinition("case.beta", "issue.unrelated-beta");
    QCOMPARE(first.bench, second.bench);
    QCOMPARE(first.configuration.behavior_definition_digest,
             second.configuration.behavior_definition_digest);

    const auto exercise = [](const model::CanonicalOralArgumentDefinition& definition,
                             std::string_view expected_issue) {
        const auto initial = engine::initializeOralArgument(definition);
        if (!initial) {
            return false;
        }
        const auto opening = engine::planOpeningQuestion(definition, *initial);
        if (!opening || !opening->bench.question.has_value() ||
            opening->bench.question->issue_id != expected_issue) {
            return false;
        }
        const auto* selection = std::get_if<model::AuthoredQuestionSelection>(
            &opening->bench.question->selection);
        if (selection == nullptr || selection->mode != model::OralArgumentMode::ActualRecord ||
            selection->topic != model::ArgumentFocusTopic::RecordSupport ||
            selection->grounding.size() != std::size_t{2}) {
            return false;
        }
        const auto authored = std::ranges::find(definition.question_bank.questions,
                                                selection->question_id,
                                                &model::AuthoredArgumentQuestion::id);
        if (authored == definition.question_bank.questions.end() ||
            authored->prompt != selection->prompt || authored->grounding.size() != 2) {
            return false;
        }
        const auto applied = engine::applyOralArgumentEvent(definition, *initial, *opening);
        if (!applied) {
            return false;
        }
        const std::array events{*opening};
        const auto replayed = engine::replayOralArgument(definition, *initial, events);
        return replayed.has_value() && *replayed == *applied;
    };

    QVERIFY(exercise(first, "issue.unrelated-alpha"));
    QVERIFY(exercise(second, "issue.unrelated-beta"));
}

void OralArgumentEngineTest::canonicalDigestBindsTopicsPromptsModesAndResolvedGrounding() {
    const auto definition = canonicalDefinition();
    const auto baseline = engine::groundingDigest(definition.question_bank);
    QVERIFY(baseline.has_value());
    QCOMPARE(*baseline,
             std::string("47493e7f9156bcd90e018ad8d3c4fd4ffc328df5850ee6f8ca1e59639a6c034e"));

    auto reordered = definition.question_bank;
    std::ranges::reverse(reordered.issue_topics.front().topics);
    std::ranges::reverse(reordered.questions);
    for (auto& question : reordered.questions) {
        std::ranges::reverse(question.grounding);
    }
    QCOMPARE(engine::groundingDigest(reordered), baseline);

    auto mode = definition.question_bank;
    mode.mode = model::OralArgumentMode::CounterfactualTraining;
    QVERIFY(engine::groundingDigest(mode) != baseline);

    auto topic = definition.question_bank;
    topic.issue_topics.front().topics.front() = model::ArgumentFocusTopic::Jurisdiction;
    topic.questions.front().topic = model::ArgumentFocusTopic::Jurisdiction;
    QVERIFY(engine::groundingDigest(topic) != baseline);

    auto prompt = definition.question_bank;
    prompt.questions.front().prompt += " Explain the limiting principle.";
    QVERIFY(engine::groundingDigest(prompt) != baseline);

    auto authority = definition.question_bank;
    auto& authority_ref = std::get<model::AuthorityArgumentGrounding>(
        authority.questions.front().grounding.front());
    authority_ref.authority.provenance->source_url += "?revision=2";
    QVERIFY(engine::groundingDigest(authority) != baseline);

    auto record = definition.question_bank;
    auto& record_ref = std::get<model::RecordPageArgumentGrounding>(
        record.questions.front().grounding.back());
    record_ref.citation_label = "Hearing Tr. 48";
    QVERIFY(engine::groundingDigest(record) != baseline);
}

void OralArgumentEngineTest::rejectsMixedOrInvalidCanonicalDefinitionsAndSwappedSelections() {
    auto definition = canonicalDefinition();
    const auto canonical = engine::initializeOralArgument(definition);
    QVERIFY(canonical.has_value());
    const auto opening = engine::planOpeningQuestion(definition, *canonical);
    QVERIFY(opening.has_value());

    auto swapped = *opening;
    auto& selection = std::get<model::AuthoredQuestionSelection>(
        swapped.bench.question->selection);
    selection.grounding = definition.question_bank.questions.back().grounding;
    QVERIFY(!engine::applyOralArgumentEvent(definition, *canonical, swapped).has_value());

    auto wrong_mode = *opening;
    std::get<model::AuthoredQuestionSelection>(wrong_mode.bench.question->selection).mode =
        model::OralArgumentMode::CounterfactualTraining;
    QVERIFY(!engine::applyOralArgumentEvent(definition, *canonical, wrong_mode).has_value());

    auto case_specific_focus = definition;
    case_specific_focus.bench.seats.front().profile.interaction.issue_focus.front().topic_id =
        "issue.alpha";
    const auto behavior = engine::behaviorDefinitionDigest(case_specific_focus.bench);
    QVERIFY(behavior.has_value());
    case_specific_focus.configuration.behavior_definition_digest = *behavior;
    QVERIFY(!engine::initializeOralArgument(case_specific_focus).has_value());

    auto duplicated_grounding = definition.question_bank;
    std::get<model::BriefPageArgumentGrounding>(
        duplicated_grounding.questions.back().grounding.front())
        .grounding_id = std::string(std::get<model::AuthorityArgumentGrounding>(
                                       duplicated_grounding.questions.front().grounding.front())
                                        .grounding_id);
    QVERIFY(!engine::groundingDigest(duplicated_grounding).has_value());

    auto uncovered_issue = definition.question_bank;
    uncovered_issue.issue_topics.push_back(model::ArgumentIssueTopics{
        "issue.without-question", {model::ArgumentFocusTopic::Merits}});
    QVERIFY(!engine::groundingDigest(uncovered_issue).has_value());

    auto unquestioned_focus = definition.question_bank;
    unquestioned_focus.issue_topics.front().topics.push_back(
        model::ArgumentFocusTopic::Jurisdiction);
    QVERIFY(!engine::groundingDigest(unquestioned_focus).has_value());

    const auto legacy_bench = singleSeatBench(clippedProfile());
    const auto legacy_grounding = grounding();
    const auto legacy_config = configuration(legacy_bench, legacy_grounding);
    const auto legacy = engine::initializeOralArgument(legacy_config, legacy_bench,
                                                       legacy_grounding);
    QVERIFY(legacy.has_value());
    QVERIFY(!engine::planOpeningQuestion(definition, *legacy).has_value());
    QVERIFY(!engine::planOpeningQuestion(legacy_config, legacy_bench, legacy_grounding,
                                         *canonical)
                 .has_value());
}

void OralArgumentEngineTest::canonicalRecordPinsAndCounterfactualStateRemainBounded() {
    const auto definition = canonicalDefinition("case.training", "issue.training",
                                                model::OralArgumentMode::CounterfactualTraining);
    const auto initial = engine::initializeOralArgument(definition);
    QVERIFY(initial.has_value());
    QVERIFY(initial->canonical_contract.has_value());
    QCOMPARE(initial->canonical_contract->mode,
             model::OralArgumentMode::CounterfactualTraining);
    const auto opening = engine::planOpeningQuestion(definition, *initial);
    QVERIFY(opening.has_value());
    const auto started = engine::applyOralArgumentEvent(definition, *initial, *opening);
    QVERIFY(started.has_value());

    const model::CounselAnswer record_claim{
        model::CounselActKind::RecordClaim,
        "The training answer relies on the authored authority but omits the record page.",
        "issue.training",
        {"case.training.grounding-authority"},
        1.0,
        5s,
    };
    const auto demand = engine::decideCounselAnswer(definition, *started, record_claim);
    QVERIFY(demand.has_value());
    QCOMPARE(demand->bench.kind, model::BenchActKind::RecordPinDemand);
    const auto* selection = std::get_if<model::AuthoredQuestionSelection>(
        &demand->bench.question->selection);
    QVERIFY(selection != nullptr);
    QVERIFY(std::ranges::any_of(selection->grounding, [](const auto& grounding) {
        return std::holds_alternative<model::RecordPageArgumentGrounding>(grounding);
    }));
    const auto after_demand =
        engine::applyOralArgumentEvent(definition, *started, *demand);
    QVERIFY(after_demand.has_value());
    QCOMPARE(after_demand->legal_state_digest, definition.configuration.legal_state_digest);
    QCOMPARE(after_demand->authored_disposition_id,
             definition.configuration.authored_disposition_id);
    QCOMPARE(after_demand->canonical_contract, initial->canonical_contract);

    auto forged_legal = *started;
    forged_legal.legal_state_digest.assign(64, 'd');
    QVERIFY(!engine::decideCounselAnswer(definition, forged_legal, record_claim).has_value());
    auto forged_disposition = *started;
    forged_disposition.authored_disposition_id = "operation.forged-judgment";
    QVERIFY(!engine::decideCounselAnswer(definition, forged_disposition, record_claim).has_value());

    const std::array events{*opening, *demand};
    const auto replayed = engine::replayOralArgument(definition, *initial, events);
    QVERIFY(replayed.has_value());
    QCOMPARE(*replayed, *after_demand);
}

void OralArgumentEngineTest::canonicalContractRejectsOperativeConfigurationSwaps() {
    const auto definition = canonicalDefinition();
    const auto initial = engine::initializeOralArgument(definition);
    QVERIFY(initial.has_value());
    QVERIFY(initial->canonical_contract.has_value());
    const auto opening = engine::planOpeningQuestion(definition, *initial);
    QVERIFY(opening.has_value());
    const auto started = engine::applyOralArgumentEvent(definition, *initial, *opening);
    QVERIFY(started.has_value());

    const model::CounselAnswer threshold_probe{
        model::CounselActKind::Answer,
        "The authored record and authority support the requested relief.",
        "issue.alpha",
        {"case.alpha.grounding-authority"},
        0.75,
        5s,
    };
    const auto original_decision =
        engine::decideCounselAnswer(definition, *started, threshold_probe);
    QVERIFY(original_decision.has_value());
    QVERIFY(original_decision->bench.kind != model::BenchActKind::ClarificationRequest);

    const auto rejects_swap = [&](const model::CanonicalOralArgumentDefinition& changed) {
        const auto changed_initial = engine::initializeOralArgument(changed);
        QVERIFY(changed_initial.has_value());
        QVERIFY(changed_initial->canonical_contract.has_value());
        QVERIFY(changed_initial->canonical_contract->definition_digest !=
                initial->canonical_contract->definition_digest);
        QVERIFY(!engine::decideCounselAnswer(changed, *started, threshold_probe).has_value());
        const std::array journal{*opening};
        QVERIFY(!engine::replayOralArgument(changed, *initial, journal).has_value());
    };

    auto changed_threshold = definition;
    changed_threshold.configuration.classification_confidence_threshold = 0.80;
    rejects_swap(changed_threshold);

    auto changed_follow_up_limit = definition;
    ++changed_follow_up_limit.configuration.maximum_follow_up_depth;
    rejects_swap(changed_follow_up_limit);

    auto changed_principal_time = definition;
    changed_principal_time.configuration.principal_time += 30s;
    rejects_swap(changed_principal_time);

    auto changed_rebuttal_time = definition;
    changed_rebuttal_time.configuration.rebuttal_time += 10s;
    rejects_swap(changed_rebuttal_time);
}

void OralArgumentEngineTest::enforcesCanonicalQuestionBankAndPromptBounds() {
    auto issue_limit = emptyBoundedBank();
    for (std::size_t issue_index = 0; issue_index < 64; ++issue_index) {
        appendBoundedIssue(issue_limit, issue_index);
        appendBoundedQuestion(issue_limit, issue_index, 0);
    }
    QVERIFY(engine::groundingDigest(issue_limit).has_value());
    appendBoundedIssue(issue_limit, 64);
    appendBoundedQuestion(issue_limit, 64, 0);
    QVERIFY(!engine::groundingDigest(issue_limit).has_value());

    auto question_limit = emptyBoundedBank();
    for (std::size_t issue_index = 0; issue_index < 8; ++issue_index) {
        appendBoundedIssue(question_limit, issue_index);
        for (std::size_t question_index = 0; question_index < 16; ++question_index) {
            appendBoundedQuestion(question_limit, issue_index, question_index);
        }
    }
    QCOMPARE(question_limit.questions.size(), std::size_t{128});
    QVERIFY(engine::groundingDigest(question_limit).has_value());
    appendBoundedIssue(question_limit, 8);
    appendBoundedQuestion(question_limit, 8, 0);
    QCOMPARE(question_limit.questions.size(), std::size_t{129});
    QVERIFY(!engine::groundingDigest(question_limit).has_value());

    auto per_issue_limit = emptyBoundedBank();
    appendBoundedIssue(per_issue_limit, 0);
    for (std::size_t question_index = 0; question_index < 16; ++question_index) {
        appendBoundedQuestion(per_issue_limit, 0, question_index);
    }
    QVERIFY(engine::groundingDigest(per_issue_limit).has_value());
    appendBoundedQuestion(per_issue_limit, 0, 16);
    QCOMPARE(per_issue_limit.questions.size(), std::size_t{17});
    QVERIFY(!engine::groundingDigest(per_issue_limit).has_value());

    auto grounding_limit = emptyBoundedBank();
    appendBoundedIssue(grounding_limit, 0);
    appendBoundedQuestion(grounding_limit, 0, 0, 16);
    QCOMPARE(grounding_limit.questions.front().grounding.size(), std::size_t{16});
    QVERIFY(engine::groundingDigest(grounding_limit).has_value());
    grounding_limit.questions.front().grounding.push_back(model::BriefPageArgumentGrounding{
        "case.bounds.grounding-0-0-16",
        "case.bounds.entry-0-0-16",
        1,
        std::string(64, 'a'),
    });
    QCOMPARE(grounding_limit.questions.front().grounding.size(), std::size_t{17});
    QVERIFY(!engine::groundingDigest(grounding_limit).has_value());

    const auto rejects_prompt = [](std::string prompt) {
        auto bank = emptyBoundedBank();
        appendBoundedIssue(bank, 0);
        appendBoundedQuestion(bank, 0, 0);
        bank.questions.front().prompt = std::move(prompt);
        return !engine::groundingDigest(bank).has_value();
    };
    QVERIFY(!rejects_prompt(std::string(512, 'a')));
    QVERIFY(rejects_prompt(std::string(513, 'a')));
    QVERIFY(rejects_prompt(" leading ASCII space"));
    QVERIFY(rejects_prompt("trailing ASCII space "));
    QVERIFY(rejects_prompt("   "));
    QVERIFY(rejects_prompt("line\nbreak"));
    QVERIFY(rejects_prompt(std::string{"bad\0prompt", 10}));
    QVERIFY(rejects_prompt(std::string{"bad"} + static_cast<char>(0x7f) + "prompt"));
    QVERIFY(rejects_prompt(std::string{"\xc3\x28", 2}));
    QVERIFY(rejects_prompt(std::string{"\xc2\xa0"}));
    QVERIFY(rejects_prompt(std::string{"\xe3\x80\x80"}));
}

void OralArgumentEngineTest::enforcesCanonicalEventLimitAtEveryBoundary() {
    auto definition = canonicalDefinition();
    definition.configuration.principal_time = 1'000s;
    const auto initial = engine::initializeOralArgument(definition);
    QVERIFY(initial.has_value());
    const auto opening = engine::planOpeningQuestion(definition, *initial);
    QVERIFY(opening.has_value());
    const auto started = engine::applyOralArgumentEvent(definition, *initial, *opening);
    QVERIFY(started.has_value());

    const model::CounselAnswer response{
        model::CounselActKind::Answer,
        "The authored sources support the requested disposition.",
        "issue.alpha",
        {"case.alpha.grounding-authority"},
        1.0,
        1s,
    };
    const auto second = engine::decideCounselAnswer(definition, *started, response);
    QVERIFY(second.has_value());
    const auto after_second =
        engine::applyOralArgumentEvent(definition, *started, *second);
    QVERIFY(after_second.has_value());
    const auto third = engine::decideCounselAnswer(definition, *after_second, response);
    QVERIFY(third.has_value());
    QCOMPARE(second->bench.kind, model::BenchActKind::Interruption);
    QCOMPARE(third->bench.kind, model::BenchActKind::Interruption);

    std::vector<model::OralArgumentEvent> journal;
    journal.reserve(64);
    journal.push_back(*opening);
    for (std::uint64_t sequence = 2; sequence <= 64; ++sequence) {
        auto event = sequence % 2 == 0 ? *second : *third;
        event.sequence = sequence;
        event.bench.question->parent_act_sequence = sequence - 1;
        journal.push_back(std::move(event));
    }

    const auto state = engine::replayOralArgument(definition, *initial, journal);
    QVERIFY(state.has_value());
    QCOMPARE(state->journal.size(), std::size_t{64});
    QCOMPARE(state->transcript.size(), std::size_t{127});
    QCOMPARE(state->next_event_sequence, std::uint64_t{65});
    QVERIFY(!engine::decideCounselAnswer(definition, *state, response).has_value());
    QVERIFY(!engine::applyOralArgumentEvent(definition, *state, journal.back()).has_value());

    auto excessive_journal = journal;
    excessive_journal.push_back(excessive_journal.back());
    QCOMPARE(excessive_journal.size(), std::size_t{65});
    QVERIFY(!engine::replayOralArgument(definition, *initial, excessive_journal).has_value());

    auto excessive_sequence = *opening;
    excessive_sequence.sequence = 65;
    QVERIFY(!engine::applyOralArgumentEvent(definition, *initial, excessive_sequence)
                 .has_value());
}

} // namespace

QTEST_GUILESS_MAIN(OralArgumentEngineTest)

#include "tst_oral_argument_engine.moc"
