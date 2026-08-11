#include "oral_argument_workspace.hpp"

#include <QComboBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTest>

#include <array>
#include <chrono>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace app = appellate::app;
namespace model = appellate::model;
using appellate::ui::OralArgumentWorkspace;
using appellate::ui::OralArgumentWorkspaceSession;
using namespace std::chrono_literals;

class FakeWorkspaceSession final : public OralArgumentWorkspaceSession {
  public:
    explicit FakeWorkspaceSession(
        model::OralArgumentMode mode = model::OralArgumentMode::ActualRecord)
        : definition_(makeDefinition(mode)), state_(makeState(definition_)) {}

    [[nodiscard]] const QString& sessionId() const noexcept override { return session_id_; }

    [[nodiscard]] const model::CanonicalOralArgumentDefinition*
    canonicalDefinition() const noexcept override {
        return canonical_ ? &definition_ : nullptr;
    }

    [[nodiscard]] const model::OralArgumentState& state() const noexcept override { return state_; }

    [[nodiscard]] auto submit(QString command_id, const model::CounselAnswer& answer,
                              const QString& recorded_at_utc)
        -> std::expected<app::OralArgumentSubmissionResult,
                         app::OralArgumentSessionError> override {
        ++submit_count;
        last_command_id = std::move(command_id);
        last_answer = answer;
        last_recorded_at = recorded_at_utc;
        if (fail_submission) {
            return std::unexpected(app::OralArgumentSessionError{
                app::OralArgumentSessionErrorCode::EngineFailure,
                QStringLiteral("Injected deterministic submission failure"),
            });
        }

        const auto& next = definition_.question_bank.questions.at(1);
        const model::OralArgumentEvent event{
            state_.next_event_sequence,
            answer,
            model::BenchAct{
                model::BenchActKind::FollowUp,
                definition_.bench.seats.front().id,
                model::GroundedQuestion{
                    next.issue_id,
                    model::AuthoredQuestionSelection{next.id, next.topic,
                                                     definition_.question_bank.mode, next.prompt,
                                                     next.grounding},
                    state_.journal.back().sequence,
                    false,
                },
                std::string{"Counsel, answer the authored remedy question."},
            },
        };
        state_.principal_remaining -= answer.elapsed;
        state_.next_event_sequence += 1;
        state_.last_seat_id = event.bench.seat_id;
        state_.follow_up_depth += 1;
        state_.journal.push_back(event);
        state_.transcript.push_back(model::OralArgumentTranscriptEntry{
            event.sequence, model::TranscriptSpeaker::Counsel, {}, answer.text, std::nullopt});
        state_.transcript.push_back(model::OralArgumentTranscriptEntry{
            event.sequence, model::TranscriptSpeaker::Bench, event.bench.seat_id,
            event.bench.rendered_utterance, event.bench.kind});
        return app::OralArgumentSubmissionResult{event, 2};
    }

    bool canonical_{true};
    bool fail_submission{};
    int submit_count{};
    QString last_command_id;
    std::optional<model::CounselAnswer> last_answer;
    QString last_recorded_at;

  private:
    [[nodiscard]] static model::JudgeProfile profile() {
        return model::JudgeProfile{
            "fictional.ui-composite",
            "Justice Cedar",
            model::ProfileClass::FictionalComposite,
            model::ProfileCompatibility{{model::CourtRole::Appellate}, {"us.ca4"}},
            model::InteractionStyle{
                0.8,
                0.9,
                0.4,
                0.3,
                0.7,
                0.2,
                0.5,
                0.9,
                0.7,
                {model::IssueFocus{"workbench.topic.record-support", 1.0}},
            },
            model::VoiceStyle{
                model::VoiceRegister::Technical,
                model::VoiceCadence::Clipped,
                model::QuestionFraming::Direct,
                model::CounselAddress::Counsel,
                0.4,
                0.5,
                {"show me the exact source"},
                {"before you move on"},
                {"clarify that answer"},
            },
        };
    }

    [[nodiscard]] static model::AuthorityRef authority() {
        return model::AuthorityRef{
            model::AuthorityId{"authority.ui-standard"},
            "Fictional Authority, 101 F.4th 22",
            "2026-06-01",
            "A preserved issue receives the authored standard of review.",
            model::AuthorityProvenance{
                model::AuthorityType::Case,
                "us.ca4",
                "court.fictional-appellate",
                model::PrecedentialStatus::Precedential,
                true,
                "2026-06-02",
                "101 F.4th 22",
                "https://court.example.test/opinions/101-f4th-22",
            },
        };
    }

    [[nodiscard]] static model::CanonicalOralArgumentDefinition
    makeDefinition(model::OralArgumentMode mode) {
        model::AuthoredQuestionBank bank{
            model::CaseId{"case.ui-fixture"},
            "case.ui-fixture.argument",
            mode,
            "fixture-grounding-digest",
            {model::ArgumentIssueTopics{
                "issue.ui-record",
                {model::ArgumentFocusTopic::RecordSupport, model::ArgumentFocusTopic::Remedy},
            }},
            {
                model::AuthoredArgumentQuestion{
                    "question.ui-record",
                    "issue.ui-record",
                    model::ArgumentFocusTopic::RecordSupport,
                    "Where does the exact record support the dispositive proposition?",
                    {
                        model::AuthorityArgumentGrounding{"grounding.ui-authority", authority()},
                        model::BriefPageArgumentGrounding{"grounding.ui-brief",
                                                          "record.entry-opening-brief", 12,
                                                          std::string(64, 'b')},
                        model::RecordPageArgumentGrounding{
                            "grounding.ui-record", "record.anchor-hearing-47",
                            "record.entry-hearing", 47, std::string(64, 'a'),
                            std::string{"Hearing Tr. 47"}},
                    },
                },
                model::AuthoredArgumentQuestion{
                    "question.ui-remedy",
                    "issue.ui-record",
                    model::ArgumentFocusTopic::Remedy,
                    "What exact relief follows from that record proposition?",
                    {model::RecordPageArgumentGrounding{
                        "grounding.ui-order", "record.anchor-order-3", "record.entry-order", 3,
                        std::string(64, 'c'), std::string{"Order 3"}}},
                },
            },
        };
        return model::CanonicalOralArgumentDefinition{
            model::OralArgumentConfiguration{
                120s,
                30s,
                0.7,
                3,
                "fixture-behavior-digest",
                bank.grounding_digest,
                "fixture-owned-legal-state",
                "fixture-owned-disposition",
            },
            model::BenchConfiguration{
                "us.ca4",
                model::CourtRole::Appellate,
                {model::BenchSeat{"seat.ui-presiding", profile()}},
                "seat.ui-presiding",
            },
            std::move(bank),
        };
    }

    [[nodiscard]] static model::OralArgumentState
    makeState(const model::CanonicalOralArgumentDefinition& definition) {
        const auto& question = definition.question_bank.questions.front();
        const model::OralArgumentEvent opening{
            1,
            std::nullopt,
            model::BenchAct{
                model::BenchActKind::Question,
                definition.bench.seats.front().id,
                model::GroundedQuestion{
                    question.issue_id,
                    model::AuthoredQuestionSelection{question.id, question.topic,
                                                     definition.question_bank.mode, question.prompt,
                                                     question.grounding},
                    std::nullopt,
                    false,
                },
                std::string{"Counsel, where does the record support that proposition?"},
            },
        };
        return model::OralArgumentState{
            model::OralArgumentPhase::Principal,
            definition.configuration.principal_time,
            definition.configuration.rebuttal_time,
            2,
            opening.bench.seat_id,
            0,
            {},
            {model::OralArgumentTranscriptEntry{
                1, model::TranscriptSpeaker::Bench, opening.bench.seat_id,
                opening.bench.rendered_utterance, opening.bench.kind}},
            {opening},
            definition.configuration.behavior_definition_digest,
            definition.configuration.grounding_digest,
            definition.configuration.legal_state_digest,
            definition.configuration.authored_disposition_id,
            model::CanonicalOralArgumentContract{
                definition.question_bank.case_id,
                definition.question_bank.argument_configuration_id,
                definition.question_bank.mode,
                definition.question_bank.grounding_digest,
                "fixture-definition-digest",
            },
        };
    }

    QString session_id_{QStringLiteral("ui.session.argument")};
    model::CanonicalOralArgumentDefinition definition_;
    model::OralArgumentState state_;
};

class OralArgumentWorkspaceTest final : public QObject {
    Q_OBJECT

  private slots:
    void rendersExactActualQuestionAndTypedGrounding();
    void counterfactualModeMakesIsolationExplicit();
    void controlReturnSubmitsExactSelectionsWithInjectedClocks();
    void validationAndControllerErrorsPreserveDraftAndSession();
    void rejectsNoncanonicalSession();
};

void OralArgumentWorkspaceTest::rendersExactActualQuestionAndTypedGrounding() {
    auto session = std::make_unique<FakeWorkspaceSession>();
    OralArgumentWorkspace workspace(std::move(session));

    QVERIFY(workspace.isReady());
    QVERIFY(workspace.modeLabel()->text().contains(QStringLiteral("Actual record")));
    QVERIFY(workspace.isolationNoticeLabel()->text().contains(QStringLiteral("immutable legal")));
    QVERIFY(workspace.isolationNoticeLabel()->text().contains(QStringLiteral("cannot change")));
    QVERIFY(workspace.judgeLabel()->text().contains(QStringLiteral("Justice Cedar")));
    QVERIFY(workspace.judgeLabel()->text().contains(QStringLiteral("fictional/composite"),
                                                    Qt::CaseInsensitive));
    QVERIFY(workspace.actLabel()->text().contains(QStringLiteral("Question")));
    QVERIFY(workspace.issueLabel()->text().contains(QStringLiteral("issue.ui-record")));
    QVERIFY(workspace.topicLabel()->text().contains(QStringLiteral("Record support")));
    QVERIFY(
        workspace.topicLabel()->text().contains(QStringLiteral("workbench.topic.record-support")));
    QVERIFY(workspace.questionLabel()->text().contains(QStringLiteral("question.ui-record")));
    QVERIFY(workspace.questionLabel()->text().contains(
        QStringLiteral("Where does the exact record support the dispositive proposition?")));

    QCOMPARE(workspace.groundingTable()->rowCount(), 3);
    QCOMPARE(workspace.groundingTable()->item(0, 1)->text(), QStringLiteral("Authority"));
    QCOMPARE(workspace.groundingTable()->item(1, 1)->text(), QStringLiteral("Brief page"));
    QCOMPARE(workspace.groundingTable()->item(2, 1)->text(), QStringLiteral("Record page"));
    QCOMPARE(workspace.groundingTable()->item(0, 2)->text(),
             QStringLiteral("grounding.ui-authority"));
    QVERIFY(workspace.groundingTable()->item(0, 3)->text().contains(
        QStringLiteral("Fictional Authority, 101 F.4th 22")));
    QVERIFY(workspace.groundingTable()->item(0, 3)->text().contains(
        QStringLiteral("court.fictional-appellate")));
    QVERIFY(workspace.groundingTable()->item(1, 3)->text().contains(
        QStringLiteral("record.entry-opening-brief, page 12")));
    QVERIFY(workspace.groundingTable()->item(2, 3)->text().contains(
        QStringLiteral("record.anchor-hearing-47")));
    QVERIFY(
        workspace.groundingTable()->item(2, 3)->text().contains(QStringLiteral("Hearing Tr. 47")));
    for (int row = 0; row < workspace.groundingTable()->rowCount(); ++row) {
        QVERIFY(
            workspace.groundingTable()->item(row, 0)->flags().testFlag(Qt::ItemIsUserCheckable));
    }

    QVERIFY(workspace.transcriptView()->toPlainText().contains(QStringLiteral("Justice Cedar")));
    QVERIFY(workspace.transcriptView()->toPlainText().contains(
        QStringLiteral("where does the record support")));
    QCOMPARE(workspace.principalClockLabel()->text(), QStringLiteral("02:00"));
    QCOMPARE(workspace.rebuttalClockLabel()->text(), QStringLiteral("00:30"));

    const std::array accessible_widgets{
        static_cast<QWidget*>(workspace.modeLabel()),
        static_cast<QWidget*>(workspace.isolationNoticeLabel()),
        static_cast<QWidget*>(workspace.judgeLabel()),
        static_cast<QWidget*>(workspace.actLabel()),
        static_cast<QWidget*>(workspace.issueLabel()),
        static_cast<QWidget*>(workspace.topicLabel()),
        static_cast<QWidget*>(workspace.questionLabel()),
        static_cast<QWidget*>(workspace.groundingTable()),
        static_cast<QWidget*>(workspace.transcriptView()),
        static_cast<QWidget*>(workspace.principalClockLabel()),
        static_cast<QWidget*>(workspace.rebuttalClockLabel()),
        static_cast<QWidget*>(workspace.answerKindSelector()),
        static_cast<QWidget*>(workspace.answerEditor()),
        static_cast<QWidget*>(workspace.statusLabel()),
        static_cast<QWidget*>(workspace.submitButton()),
    };
    for (const auto* widget : accessible_widgets) {
        QVERIFY(widget != nullptr);
        QVERIFY(!widget->accessibleName().isEmpty());
    }
    QVERIFY(workspace.answerEditor()->focusPolicy() != Qt::NoFocus);
    QVERIFY(workspace.groundingTable()->focusPolicy() != Qt::NoFocus);
    QVERIFY(workspace.submitButton()->toolTip().contains(QStringLiteral("Ctrl+Return")));
}

void OralArgumentWorkspaceTest::counterfactualModeMakesIsolationExplicit() {
    OralArgumentWorkspace workspace(
        std::make_unique<FakeWorkspaceSession>(model::OralArgumentMode::CounterfactualTraining));
    QVERIFY(workspace.isReady());
    QVERIFY(workspace.modeLabel()->text().contains(QStringLiteral("Counterfactual training")));
    QVERIFY(workspace.isolationNoticeLabel()->text().contains(
        QStringLiteral("isolated from the actual-record workflow")));
    QVERIFY(workspace.isolationNoticeLabel()->text().contains(QStringLiteral("journals")));
    QVERIFY(workspace.isolationNoticeLabel()->text().contains(QStringLiteral("snapshots")));
    QVERIFY(workspace.isolationNoticeLabel()->text().contains(QStringLiteral("disposition")));
}

void OralArgumentWorkspaceTest::controlReturnSubmitsExactSelectionsWithInjectedClocks() {
    auto session = std::make_unique<FakeWorkspaceSession>();
    auto* observed = session.get();
    OralArgumentWorkspace workspace(
        std::move(session), [] { return 7s; },
        [] { return QStringLiteral("2026-08-11T12:34:56Z"); });
    workspace.show();
    QVERIFY(QTest::qWaitForWindowExposed(&workspace));

    workspace.answerKindSelector()->setCurrentIndex(2);
    workspace.answerEditor()->setPlainText(
        QStringLiteral("The record claim is pinned to the hearing and cited authority."));
    workspace.groundingTable()->item(0, 0)->setCheckState(Qt::Checked);
    workspace.groundingTable()->item(2, 0)->setCheckState(Qt::Checked);
    workspace.answerEditor()->setFocus();
    QTest::keyClick(workspace.answerEditor(), Qt::Key_Return, Qt::ControlModifier);

    QTRY_COMPARE(observed->submit_count, 1);
    QCOMPARE(observed->last_command_id, QStringLiteral("ui.session.argument.answer-2"));
    QCOMPARE(observed->last_recorded_at, QStringLiteral("2026-08-11T12:34:56Z"));
    QVERIFY(observed->last_answer.has_value());
    QCOMPARE(observed->last_answer->kind, model::CounselActKind::RecordClaim);
    QCOMPARE(observed->last_answer->text,
             std::string("The record claim is pinned to the hearing and cited authority."));
    QCOMPARE(observed->last_answer->issue_id, std::string("issue.ui-record"));
    QCOMPARE(observed->last_answer->cited_grounding_ids,
             (std::vector<std::string>{"grounding.ui-authority", "grounding.ui-record"}));
    QCOMPARE(observed->last_answer->elapsed, 7s);
    QCOMPARE(workspace.answerEditor()->toPlainText(), QString{});
    QCOMPARE(workspace.principalClockLabel()->text(), QStringLiteral("01:53"));
    QVERIFY(workspace.questionLabel()->text().contains(QStringLiteral("question.ui-remedy")));
    QVERIFY(workspace.questionLabel()->text().contains(
        QStringLiteral("What exact relief follows from that record proposition?")));
    QCOMPARE(workspace.groundingTable()->rowCount(), 1);
    QCOMPARE(workspace.groundingTable()->item(0, 2)->text(), QStringLiteral("grounding.ui-order"));
    QVERIFY(workspace.transcriptView()->toPlainText().contains(
        QStringLiteral("The record claim is pinned")));
    QVERIFY(workspace.transcriptView()->toPlainText().contains(
        QStringLiteral("answer the authored remedy question")));
    QVERIFY(workspace.statusLabel()->text().contains(QStringLiteral("event 2")));
}

void OralArgumentWorkspaceTest::validationAndControllerErrorsPreserveDraftAndSession() {
    auto session = std::make_unique<FakeWorkspaceSession>();
    auto* observed = session.get();
    observed->fail_submission = true;
    OralArgumentWorkspace workspace(
        std::move(session), [] { return 4s; },
        [] { return QStringLiteral("2026-08-11T12:34:56Z"); });

    const auto before_state = *workspace.sessionState();
    const auto before_transcript = workspace.transcriptView()->toPlainText();
    const auto before_principal = workspace.principalClockLabel()->text();
    const auto empty = workspace.submitAnswer();
    QVERIFY(!empty.has_value());
    QCOMPARE(observed->submit_count, 0);
    QCOMPARE(*workspace.sessionState(), before_state);

    const auto draft = QStringLiteral("Preserve this draft after an injected failure.");
    workspace.answerEditor()->setPlainText(draft);
    workspace.groundingTable()->item(1, 0)->setCheckState(Qt::Checked);
    const auto failed = workspace.submitAnswer();
    QVERIFY(!failed.has_value());
    QVERIFY(failed.error().contains(QStringLiteral("Injected deterministic")));
    QCOMPARE(observed->submit_count, 1);
    QCOMPARE(*workspace.sessionState(), before_state);
    QCOMPARE(workspace.answerEditor()->toPlainText(), draft);
    QCOMPARE(workspace.groundingTable()->item(1, 0)->checkState(), Qt::Checked);
    QCOMPARE(workspace.transcriptView()->toPlainText(), before_transcript);
    QCOMPARE(workspace.principalClockLabel()->text(), before_principal);
    QVERIFY(workspace.statusLabel()->text().startsWith(QStringLiteral("Error:")));
}

void OralArgumentWorkspaceTest::rejectsNoncanonicalSession() {
    auto session = std::make_unique<FakeWorkspaceSession>();
    session->canonical_ = false;
    OralArgumentWorkspace workspace(std::move(session));
    QVERIFY(!workspace.isReady());
    QVERIFY(workspace.lastError().contains(QStringLiteral("canonical grounded-question")));
    QVERIFY(!workspace.submitButton()->isEnabled());
    QVERIFY(!workspace.answerEditor()->isEnabled());
    QCOMPARE(workspace.groundingTable()->rowCount(), 0);
}

} // namespace

QTEST_MAIN(OralArgumentWorkspaceTest)

#include "tst_oral_argument_workspace.moc"
