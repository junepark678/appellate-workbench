#include "appellate/engine/procedure_engine.hpp"

#include <QTest>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;

[[nodiscard]] appellate::model::LegalDate date(int year, unsigned month, unsigned day) {
    return appellate::model::LegalDate{std::chrono::year{year} / std::chrono::month{month} /
                                       std::chrono::day{day}};
}

[[nodiscard]] appellate::model::AuthorityBasis authority(std::string id) {
    return appellate::model::AuthorityBasis{
        appellate::model::AuthorityRef{
            appellate::model::AuthorityId{std::move(id)},
            "Test rule 1",
            "2026-08-11",
            "Test-only proposition",
        },
        {},
    };
}

[[nodiscard]] appellate::model::AuthorityProvenance provenance() {
    return appellate::model::AuthorityProvenance{
        appellate::model::AuthorityType::Rule,
        "us.federal",
        "us.ca4",
        appellate::model::PrecedentialStatus::NotApplicable,
        true,
        "2026-08-11",
        "Fed. R. App. P. 3(a)",
        "https://www.ca4.uscourts.gov/rules/Rule03.html",
    };
}

struct Fixture final {
    appellate::model::ProcedureDefinition procedure{
        appellate::model::ProcedureId{"test.procedure.civil"},
        appellate::model::CourtCalendar{{date(2026, 8, 17)}},
        appellate::model::InitiatingFilingRule{
            appellate::model::FilingTypeId{"test.filing.notice"},
            {appellate::model::ActorRoleId{"test.role.appellant"}},
            {appellate::model::FilingFieldId{"test.field.certificate"}},
            appellate::model::CureDeadlineRule{
                2,
                appellate::model::DeadlineCounting::CalendarDays,
                true,
            },
            authority("test.authority.filing"),
            authority("test.authority.actor"),
            authority("test.authority.deficiency"),
        },
    };
    appellate::model::CaseDefinition case_definition{
        appellate::model::CaseId{"test.case.rule54b"},
        procedure.id,
        {
            {appellate::model::ActorId{"test.actor.appellant"},
             appellate::model::ActorRoleId{"test.role.appellant"}},
            {appellate::model::ActorId{"test.actor.appellee"},
             appellate::model::ActorRoleId{"test.role.appellee"}},
        },
    };
    appellate::model::SessionState state{
        appellate::model::SessionId{"test.session.one"},
        procedure.id,
        case_definition.id,
        appellate::model::SessionPhase::AwaitingInitiatingFiling,
        1,
        std::nullopt,
        std::nullopt,
        {},
    };

    [[nodiscard]] appellate::model::SubmitFiling command(std::string submission,
                                                         appellate::model::LegalDate court_date,
                                                         bool complete = true) const {
        std::vector<appellate::model::SubmittedField> fields;
        if (complete) {
            fields.push_back({appellate::model::FilingFieldId{"test.field.certificate"}, "yes"});
        }
        return appellate::model::SubmitFiling{
            state.id,
            appellate::model::SubmissionId{std::move(submission)},
            appellate::model::ActorId{"test.actor.appellant"},
            procedure.initiating_filing.filing_type,
            appellate::model::LegalTime{std::chrono::sys_seconds{0s}, court_date},
            std::string(64, 'a'),
            std::move(fields),
        };
    }
};

class ProcedureEngineTest final : public QObject {
    Q_OBJECT

  private slots:
    void producesDeterministicSourcedAcceptance();
    void rejectsUnauthorizedActorWithAuthority();
    void deficiencyDeadlineSkipsWeekendAndHoliday();
    void cureAndReplayProduceIdenticalState();
    void rejectsExpiredCure();
    void refusesDefinitionWithoutAuthority();
    void validatesCompleteProvenanceAndRejectsMutations();
};

void ProcedureEngineTest::producesDeterministicSourcedAcceptance() {
    const Fixture fixture;
    const auto command = fixture.command("test.submission.one", date(2026, 8, 13));

    const auto first = appellate::engine::decide(fixture.procedure, fixture.case_definition,
                                                 fixture.state, command);
    const auto second = appellate::engine::decide(fixture.procedure, fixture.case_definition,
                                                  fixture.state, command);

    QVERIFY(first.has_value());
    QVERIFY(second.has_value());
    QVERIFY(*first == *second);
    QCOMPARE(first->size(), std::size_t{1});
    const auto* accepted = std::get_if<appellate::model::FilingAccepted>(&first->front());
    QVERIFY(accepted != nullptr);
    QCOMPARE(accepted->docket_sequence, std::uint64_t{1});
    QVERIFY(!accepted->authority.primary.id.value.empty());
}

void ProcedureEngineTest::rejectsUnauthorizedActorWithAuthority() {
    const Fixture fixture;
    auto command = fixture.command("test.submission.unauthorized", date(2026, 8, 13));
    command.actor_id = appellate::model::ActorId{"test.actor.appellee"};

    const auto decision = appellate::engine::decide(fixture.procedure, fixture.case_definition,
                                                    fixture.state, command);

    QVERIFY(decision.has_value());
    const auto* rejected = std::get_if<appellate::model::FilingRejected>(&decision->front());
    QVERIFY(rejected != nullptr);
    QCOMPARE(rejected->reason, appellate::model::FilingRejectionReason::UnauthorizedActor);
    QCOMPARE(rejected->authority.primary.id.value, std::string("test.authority.actor"));
}

void ProcedureEngineTest::deficiencyDeadlineSkipsWeekendAndHoliday() {
    const Fixture fixture;
    const auto command = fixture.command("test.submission.deficient", date(2026, 8, 14), false);

    const auto decision = appellate::engine::decide(fixture.procedure, fixture.case_definition,
                                                    fixture.state, command);

    QVERIFY(decision.has_value());
    const auto* deficiency =
        std::get_if<appellate::model::FilingDeficiencyIssued>(&decision->front());
    QVERIFY(deficiency != nullptr);
    QVERIFY(deficiency->cure_deadline == date(2026, 8, 18));
    QCOMPARE(deficiency->authority.primary.id.value, std::string("test.authority.deficiency"));
}

void ProcedureEngineTest::cureAndReplayProduceIdenticalState() {
    const Fixture fixture;
    const auto deficient = appellate::engine::decide(
        fixture.procedure, fixture.case_definition, fixture.state,
        fixture.command("test.submission.deficient", date(2026, 8, 14), false));
    QVERIFY(deficient.has_value());

    const auto after_deficiency = appellate::engine::apply(
        fixture.procedure, fixture.case_definition, fixture.state, deficient->front());
    QVERIFY(after_deficiency.has_value());

    const auto cure =
        appellate::engine::decide(fixture.procedure, fixture.case_definition, *after_deficiency,
                                  fixture.command("test.submission.cure", date(2026, 8, 18)));
    QVERIFY(cure.has_value());
    const auto live = appellate::engine::apply(fixture.procedure, fixture.case_definition,
                                               *after_deficiency, cure->front());
    QVERIFY(live.has_value());

    std::vector<appellate::model::LegalEvent> trace{deficient->front(), cure->front()};
    const auto replayed =
        appellate::engine::replay(fixture.procedure, fixture.case_definition, fixture.state, trace);
    QVERIFY(replayed.has_value());
    QVERIFY(*live == *replayed);
    QCOMPARE(replayed->phase, appellate::model::SessionPhase::Docketed);
    QCOMPARE(replayed->next_docket_sequence, std::uint64_t{3});
}

void ProcedureEngineTest::rejectsExpiredCure() {
    const Fixture fixture;
    const auto deficient = appellate::engine::decide(
        fixture.procedure, fixture.case_definition, fixture.state,
        fixture.command("test.submission.deficient", date(2026, 8, 14), false));
    QVERIFY(deficient.has_value());
    const auto pending = appellate::engine::apply(fixture.procedure, fixture.case_definition,
                                                  fixture.state, deficient->front());
    QVERIFY(pending.has_value());

    const auto late =
        appellate::engine::decide(fixture.procedure, fixture.case_definition, *pending,
                                  fixture.command("test.submission.late", date(2026, 8, 19)));

    QVERIFY(late.has_value());
    const auto* rejected = std::get_if<appellate::model::FilingRejected>(&late->front());
    QVERIFY(rejected != nullptr);
    QCOMPARE(rejected->reason, appellate::model::FilingRejectionReason::CureDeadlineExpired);
    QVERIFY(!rejected->authority.primary.id.value.empty());
}

void ProcedureEngineTest::refusesDefinitionWithoutAuthority() {
    auto fixture = Fixture{};
    fixture.procedure.initiating_filing.filing_authority.primary.proposition.clear();

    const auto decision = appellate::engine::decide(
        fixture.procedure, fixture.case_definition, fixture.state,
        fixture.command("test.submission.no-authority", date(2026, 8, 13)));

    QVERIFY(!decision.has_value());
    QCOMPARE(decision.error().code, appellate::engine::ErrorCode::MissingAuthority);
}

void ProcedureEngineTest::validatesCompleteProvenanceAndRejectsMutations() {
    auto fixture = Fixture{};
    auto& rule = fixture.procedure.initiating_filing;
    rule.filing_authority.primary.provenance = provenance();
    rule.actor_authority.primary.provenance = provenance();
    rule.deficiency_authority.primary.provenance = provenance();
    auto decision =
        appellate::engine::decide(fixture.procedure, fixture.case_definition, fixture.state,
                                  fixture.command("test.submission.provenance", date(2026, 8, 13)));
    QVERIFY(decision.has_value());
    const auto* accepted = std::get_if<appellate::model::FilingAccepted>(&decision->front());
    QVERIFY(accepted != nullptr);
    QVERIFY(accepted->authority.primary.provenance.has_value());

    rule.filing_authority.primary.provenance->source_url =
        "http://www.ca4.uscourts.gov/rules/Rule03.html";
    decision =
        appellate::engine::decide(fixture.procedure, fixture.case_definition, fixture.state,
                                  fixture.command("test.submission.bad-url", date(2026, 8, 13)));
    QVERIFY(!decision.has_value());
    QCOMPARE(decision.error().code, appellate::engine::ErrorCode::MissingAuthority);

    rule.filing_authority.primary.provenance = provenance();
    rule.filing_authority.primary.provenance->precedential_status =
        static_cast<appellate::model::PrecedentialStatus>(99);
    decision =
        appellate::engine::decide(fixture.procedure, fixture.case_definition, fixture.state,
                                  fixture.command("test.submission.bad-status", date(2026, 8, 13)));
    QVERIFY(!decision.has_value());
    QCOMPARE(decision.error().code, appellate::engine::ErrorCode::MissingAuthority);

    rule.filing_authority.primary.provenance = provenance();
    rule.filing_authority.primary.provenance->checked_on = "2025-08-11";
    decision = appellate::engine::decide(
        fixture.procedure, fixture.case_definition, fixture.state,
        fixture.command("test.submission.bad-chronology", date(2026, 8, 13)));
    QVERIFY(!decision.has_value());
    QCOMPARE(decision.error().code, appellate::engine::ErrorCode::MissingAuthority);

    rule.filing_authority.primary.provenance = provenance();
    rule.filing_authority.supporting.push_back(authority("test.authority.legacy").primary);
    decision =
        appellate::engine::decide(fixture.procedure, fixture.case_definition, fixture.state,
                                  fixture.command("test.submission.mixed", date(2026, 8, 13)));
    QVERIFY(!decision.has_value());
    QCOMPARE(decision.error().code, appellate::engine::ErrorCode::MissingAuthority);

    rule.filing_authority.supporting.clear();
    rule.filing_authority.supporting.push_back(rule.filing_authority.primary);
    decision = appellate::engine::decide(
        fixture.procedure, fixture.case_definition, fixture.state,
        fixture.command("test.submission.duplicate-authority", date(2026, 8, 13)));
    QVERIFY(!decision.has_value());
    QCOMPARE(decision.error().code, appellate::engine::ErrorCode::MissingAuthority);

    rule.filing_authority.supporting.clear();
    rule.actor_authority.primary.provenance.reset();
    decision = appellate::engine::decide(
        fixture.procedure, fixture.case_definition, fixture.state,
        fixture.command("test.submission.mixed-procedure", date(2026, 8, 13)));
    QVERIFY(!decision.has_value());
    QCOMPARE(decision.error().code, appellate::engine::ErrorCode::MissingAuthority);

    rule.actor_authority.primary.provenance = provenance();
    auto unicode_value = std::string{};
    unicode_value.reserve(2000 * std::string("한").size());
    for (int index = 0; index < 2000; ++index) {
        unicode_value += "한";
    }
    rule.filing_authority.primary.citation = unicode_value;
    rule.filing_authority.primary.proposition = unicode_value;
    rule.filing_authority.primary.provenance->locator = unicode_value;
    decision = appellate::engine::decide(
        fixture.procedure, fixture.case_definition, fixture.state,
        fixture.command("test.submission.unicode-authority", date(2026, 8, 13)));
    QVERIFY(decision.has_value());
}

} // namespace

QTEST_GUILESS_MAIN(ProcedureEngineTest)

#include "tst_procedure_engine.moc"
