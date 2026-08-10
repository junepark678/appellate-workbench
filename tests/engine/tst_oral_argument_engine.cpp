#include "appellate/engine/oral_argument_engine.hpp"

#include <QTest>

#include <algorithm>
#include <chrono>
#include <expected>
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
    QVERIFY(!decision->bench.question->grounding.empty());
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
    QVERIFY(std::ranges::any_of(record_demand->bench.question->grounding, [](const auto& item) {
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
        QVERIFY(!event->bench.question->grounding.empty());
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
    QVERIFY(std::ranges::none_of(decision->bench.question->grounding, [](const auto& reference) {
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

} // namespace

QTEST_GUILESS_MAIN(OralArgumentEngineTest)

#include "tst_oral_argument_engine.moc"
