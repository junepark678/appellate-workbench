#include "oral_argument_workspace.hpp"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDateTime>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QSplitter>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace appellate::ui {
namespace {

class ControllerWorkspaceSession final : public OralArgumentWorkspaceSession {
  public:
    explicit ControllerWorkspaceSession(
        std::unique_ptr<app::OralArgumentSessionController> controller)
        : controller_(std::move(controller)) {}

    [[nodiscard]] const QString& sessionId() const noexcept override {
        return controller_->sessionId();
    }

    [[nodiscard]] const model::CanonicalOralArgumentDefinition*
    canonicalDefinition() const noexcept override {
        return controller_->canonicalDefinition();
    }

    [[nodiscard]] const model::OralArgumentState& state() const noexcept override {
        return controller_->state();
    }

    [[nodiscard]] auto submit(QString command_id, const model::CounselAnswer& answer,
                              const QString& recorded_at_utc)
        -> std::expected<app::OralArgumentSubmissionResult,
                         app::OralArgumentSessionError> override {
        return controller_->submit(std::move(command_id), answer, recorded_at_utc);
    }

  private:
    std::unique_ptr<app::OralArgumentSessionController> controller_;
};

[[nodiscard]] QString utf8(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

[[nodiscard]] QString modeName(model::OralArgumentMode mode) {
    switch (mode) {
    case model::OralArgumentMode::ActualRecord:
        return QStringLiteral("Actual record");
    case model::OralArgumentMode::CounterfactualTraining:
        return QStringLiteral("Counterfactual training");
    }
    return QStringLiteral("Unsupported mode");
}

[[nodiscard]] QString isolationNotice(model::OralArgumentMode mode) {
    switch (mode) {
    case model::OralArgumentMode::ActualRecord:
        return QStringLiteral(
            "Actual-record simulation is pinned to the installed record and immutable legal "
            "state. Practice answers cannot change workflow decisions or the authored "
            "disposition.");
    case model::OralArgumentMode::CounterfactualTraining:
        return QStringLiteral(
            "Counterfactual training is isolated from the actual-record workflow. Practice "
            "answers cannot change workflow decisions, journals, snapshots, or the authored "
            "disposition.");
    }
    return QStringLiteral("Simulation isolation mode is unavailable.");
}

[[nodiscard]] QString topicName(model::ArgumentFocusTopic topic) {
    switch (topic) {
    case model::ArgumentFocusTopic::Jurisdiction:
        return QStringLiteral("Jurisdiction");
    case model::ArgumentFocusTopic::Preservation:
        return QStringLiteral("Preservation");
    case model::ArgumentFocusTopic::StandardOfReview:
        return QStringLiteral("Standard of review");
    case model::ArgumentFocusTopic::RecordSupport:
        return QStringLiteral("Record support");
    case model::ArgumentFocusTopic::GoverningAuthority:
        return QStringLiteral("Governing authority");
    case model::ArgumentFocusTopic::Merits:
        return QStringLiteral("Merits");
    case model::ArgumentFocusTopic::Remedy:
        return QStringLiteral("Remedy");
    case model::ArgumentFocusTopic::PracticalConsequences:
        return QStringLiteral("Practical consequences");
    }
    return QStringLiteral("Unsupported topic");
}

[[nodiscard]] QString actName(model::BenchActKind kind) {
    switch (kind) {
    case model::BenchActKind::Question:
        return QStringLiteral("Question");
    case model::BenchActKind::Interruption:
        return QStringLiteral("Interruption");
    case model::BenchActKind::FollowUp:
        return QStringLiteral("Follow-up");
    case model::BenchActKind::Hypothetical:
        return QStringLiteral("Hypothetical");
    case model::BenchActKind::RecordPinDemand:
        return QStringLiteral("Record-pin demand");
    case model::BenchActKind::ClarificationRequest:
        return QStringLiteral("Clarification request");
    case model::BenchActKind::TimeExpired:
        return QStringLiteral("Time expired");
    }
    return QStringLiteral("Unsupported act");
}

[[nodiscard]] QString authorityTypeName(model::AuthorityType type) {
    switch (type) {
    case model::AuthorityType::Constitution:
        return QStringLiteral("constitution");
    case model::AuthorityType::Statute:
        return QStringLiteral("statute");
    case model::AuthorityType::Rule:
        return QStringLiteral("rule");
    case model::AuthorityType::Regulation:
        return QStringLiteral("regulation");
    case model::AuthorityType::Case:
        return QStringLiteral("case");
    case model::AuthorityType::Order:
        return QStringLiteral("order");
    case model::AuthorityType::AdministrativeDecision:
        return QStringLiteral("administrative decision");
    case model::AuthorityType::Other:
        return QStringLiteral("other");
    }
    return QStringLiteral("unsupported");
}

[[nodiscard]] QString groundingId(const model::AuthoredArgumentGrounding& grounding) {
    return std::visit([](const auto& value) { return utf8(value.grounding_id); }, grounding);
}

struct GroundingDisplay final {
    QString type;
    QString source;
};

[[nodiscard]] GroundingDisplay groundingDisplay(const model::AuthoredArgumentGrounding& grounding) {
    return std::visit(
        [](const auto& value) -> GroundingDisplay {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, model::AuthorityArgumentGrounding>) {
                QString provenance = QStringLiteral("provenance unavailable");
                if (value.authority.provenance.has_value()) {
                    const auto& source = *value.authority.provenance;
                    provenance =
                        QStringLiteral("%1; jurisdiction %2; issuing body %3; checked %4; %5")
                            .arg(authorityTypeName(source.type), utf8(source.jurisdiction_id),
                                 utf8(source.issuing_body_id), utf8(source.checked_on),
                                 utf8(source.locator));
                }
                return {
                    QStringLiteral("Authority"),
                    QStringLiteral("%1 [%2], source version %3 — %4 — %5")
                        .arg(utf8(value.authority.citation), utf8(value.authority.id.value),
                             utf8(value.authority.source_version),
                             utf8(value.authority.proposition), provenance),
                };
            } else if constexpr (std::is_same_v<Value, model::BriefPageArgumentGrounding>) {
                return {
                    QStringLiteral("Brief page"),
                    QStringLiteral("Entry %1, page %2 — asset SHA-256 %3")
                        .arg(utf8(value.record_entry_id))
                        .arg(value.page_number)
                        .arg(utf8(value.asset_sha256)),
                };
            } else {
                const auto citation =
                    value.citation_label.has_value()
                        ? QStringLiteral(" — citation %1").arg(utf8(*value.citation_label))
                        : QString{};
                return {
                    QStringLiteral("Record page"),
                    QStringLiteral("Anchor %1; entry %2, page %3%4 — asset SHA-256 %5")
                        .arg(utf8(value.record_anchor_id), utf8(value.record_entry_id))
                        .arg(value.page_number)
                        .arg(citation, utf8(value.asset_sha256)),
                };
            }
        },
        grounding);
}

[[nodiscard]] QString formattedClock(std::chrono::seconds remaining) {
    const auto total = std::max<std::int64_t>(0, remaining.count());
    const auto hours = total / 3600;
    const auto minutes = (total % 3600) / 60;
    const auto seconds = total % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours, 2, 10, QLatin1Char('0'))
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
}

[[nodiscard]] QString defaultRecordedAt() {
    return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'"));
}

void configureLabel(QLabel& label, const QString& object_name, const QString& accessible_name) {
    label.setObjectName(object_name);
    label.setAccessibleName(accessible_name);
    label.setWordWrap(true);
    label.setTextInteractionFlags(Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
}

} // namespace

OralArgumentWorkspace::OralArgumentWorkspace(QWidget* parent)
    : OralArgumentWorkspace(std::unique_ptr<OralArgumentWorkspaceSession>{}, {}, {}, parent) {}

OralArgumentWorkspace::OralArgumentWorkspace(
    std::unique_ptr<app::OralArgumentSessionController> controller, ElapsedClock elapsed_clock,
    RecordedAtClock recorded_at_clock, QWidget* parent)
    : OralArgumentWorkspace(
          controller ? std::make_unique<ControllerWorkspaceSession>(std::move(controller))
                     : std::unique_ptr<OralArgumentWorkspaceSession>{},
          std::move(elapsed_clock), std::move(recorded_at_clock), parent) {}

OralArgumentWorkspace::OralArgumentWorkspace(std::unique_ptr<OralArgumentWorkspaceSession> session,
                                             ElapsedClock elapsed_clock,
                                             RecordedAtClock recorded_at_clock, QWidget* parent)
    : QWidget(parent), session_(std::move(session)), elapsed_clock_(std::move(elapsed_clock)),
      recorded_at_clock_(std::move(recorded_at_clock)) {
    buildUi();
    renderSession();
}

OralArgumentWorkspace::~OralArgumentWorkspace() = default;

void OralArgumentWorkspace::buildUi() {
    setObjectName(QStringLiteral("oralArgumentWorkspace"));
    setAccessibleName(QStringLiteral("Grounded oral argument workspace"));
    setAccessibleDescription(QStringLiteral(
        "Practice against fictional/composite judges using exact pack-authored questions and "
        "grounding"));

    auto* outer = new QVBoxLayout(this);
    auto* heading = new QLabel(QStringLiteral("Oral Argument"), this);
    heading->setObjectName(QStringLiteral("oralArgumentHeading"));
    heading->setAccessibleName(QStringLiteral("Oral argument heading"));
    QFont heading_font = heading->font();
    heading_font.setPointSize(18);
    heading_font.setBold(true);
    heading->setFont(heading_font);
    outer->addWidget(heading);

    mode_label_ = new QLabel(this);
    configureLabel(*mode_label_, QStringLiteral("oralArgumentMode"),
                   QStringLiteral("Oral argument mode"));
    outer->addWidget(mode_label_);

    isolation_notice_label_ = new QLabel(this);
    configureLabel(*isolation_notice_label_, QStringLiteral("oralArgumentIsolationNotice"),
                   QStringLiteral("Oral argument workflow isolation notice"));
    isolation_notice_label_->setStyleSheet(
        QStringLiteral("background: palette(alternate-base); padding: 6px; font-weight: 600;"));
    outer->addWidget(isolation_notice_label_);

    auto* clocks = new QWidget(this);
    clocks->setObjectName(QStringLiteral("oralArgumentClocks"));
    clocks->setAccessibleName(QStringLiteral("Oral argument clocks"));
    auto* clock_layout = new QFormLayout(clocks);
    principal_clock_label_ = new QLabel(clocks);
    configureLabel(*principal_clock_label_, QStringLiteral("principalClock"),
                   QStringLiteral("Principal argument time remaining"));
    rebuttal_clock_label_ = new QLabel(clocks);
    configureLabel(*rebuttal_clock_label_, QStringLiteral("rebuttalClock"),
                   QStringLiteral("Rebuttal time remaining"));
    clock_layout->addRow(QStringLiteral("Principal remaining:"), principal_clock_label_);
    clock_layout->addRow(QStringLiteral("Rebuttal remaining:"), rebuttal_clock_label_);
    outer->addWidget(clocks);

    auto* split = new QSplitter(Qt::Vertical, this);
    split->setObjectName(QStringLiteral("oralArgumentSplitter"));
    split->setAccessibleName(QStringLiteral("Current question and transcript"));

    auto* question_group = new QGroupBox(QStringLiteral("Current bench act"), split);
    question_group->setObjectName(QStringLiteral("currentBenchActGroup"));
    question_group->setAccessibleName(QStringLiteral("Current fictional judge question"));
    auto* question_layout = new QVBoxLayout(question_group);
    judge_label_ = new QLabel(question_group);
    configureLabel(*judge_label_, QStringLiteral("currentJudge"),
                   QStringLiteral("Current fictional composite judge"));
    act_label_ = new QLabel(question_group);
    configureLabel(*act_label_, QStringLiteral("currentBenchAct"),
                   QStringLiteral("Current bench act kind"));
    issue_label_ = new QLabel(question_group);
    configureLabel(*issue_label_, QStringLiteral("currentIssue"),
                   QStringLiteral("Current authored issue"));
    topic_label_ = new QLabel(question_group);
    configureLabel(*topic_label_, QStringLiteral("currentTopic"),
                   QStringLiteral("Current authored topic"));
    question_label_ = new QLabel(question_group);
    configureLabel(*question_label_, QStringLiteral("currentAuthoredQuestion"),
                   QStringLiteral("Current exact authored question"));
    question_label_->setStyleSheet(QStringLiteral("font-size: 14px; font-weight: 600;"));
    question_layout->addWidget(judge_label_);
    question_layout->addWidget(act_label_);
    question_layout->addWidget(issue_label_);
    question_layout->addWidget(topic_label_);
    question_layout->addWidget(question_label_);

    grounding_table_ = new QTableWidget(question_group);
    grounding_table_->setObjectName(QStringLiteral("authoredGroundingTable"));
    grounding_table_->setAccessibleName(QStringLiteral("Selectable authored grounding citations"));
    grounding_table_->setAccessibleDescription(QStringLiteral(
        "Check exact authority, brief-page, or record-page grounding to cite in the answer"));
    grounding_table_->setColumnCount(4);
    grounding_table_->setHorizontalHeaderLabels({QStringLiteral("Cite"), QStringLiteral("Type"),
                                                 QStringLiteral("Grounding ID"),
                                                 QStringLiteral("Exact source snapshot")});
    grounding_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    grounding_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    grounding_table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    grounding_table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    grounding_table_->verticalHeader()->setVisible(false);
    grounding_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    grounding_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    grounding_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    question_layout->addWidget(grounding_table_, 1);
    split->addWidget(question_group);

    auto* transcript_group = new QGroupBox(QStringLiteral("Transcript"), split);
    transcript_group->setObjectName(QStringLiteral("oralArgumentTranscriptGroup"));
    transcript_group->setAccessibleName(QStringLiteral("Oral argument transcript"));
    auto* transcript_layout = new QVBoxLayout(transcript_group);
    transcript_view_ = new QPlainTextEdit(transcript_group);
    transcript_view_->setObjectName(QStringLiteral("oralArgumentTranscript"));
    transcript_view_->setAccessibleName(QStringLiteral("Persistent oral argument transcript"));
    transcript_view_->setAccessibleDescription(
        QStringLiteral("Read-only transcript of counsel and fictional bench acts"));
    transcript_view_->setReadOnly(true);
    transcript_layout->addWidget(transcript_view_);
    split->addWidget(transcript_group);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    outer->addWidget(split, 1);

    auto* answer_group = new QGroupBox(QStringLiteral("Counsel answer"), this);
    answer_group->setObjectName(QStringLiteral("counselAnswerGroup"));
    answer_group->setAccessibleName(QStringLiteral("Counsel answer controls"));
    auto* answer_layout = new QVBoxLayout(answer_group);
    answer_kind_selector_ = new QComboBox(answer_group);
    answer_kind_selector_->setObjectName(QStringLiteral("answerKindSelector"));
    answer_kind_selector_->setAccessibleName(QStringLiteral("Counsel answer kind"));
    answer_kind_selector_->setAccessibleDescription(
        QStringLiteral("Classify the response as an answer, concession, or record claim"));
    answer_kind_selector_->addItem(QStringLiteral("Answer"),
                                   static_cast<int>(model::CounselActKind::Answer));
    answer_kind_selector_->addItem(QStringLiteral("Concession"),
                                   static_cast<int>(model::CounselActKind::Concession));
    answer_kind_selector_->addItem(QStringLiteral("Record claim"),
                                   static_cast<int>(model::CounselActKind::RecordClaim));
    answer_layout->addWidget(answer_kind_selector_);

    answer_editor_ = new QPlainTextEdit(answer_group);
    answer_editor_->setObjectName(QStringLiteral("counselAnswerEditor"));
    answer_editor_->setAccessibleName(QStringLiteral("Counsel answer text"));
    answer_editor_->setAccessibleDescription(
        QStringLiteral("Enter counsel's answer; press Control Return to submit"));
    answer_editor_->setPlaceholderText(QStringLiteral("Answer the bench's exact question…"));
    answer_layout->addWidget(answer_editor_);

    submit_button_ = new QPushButton(QStringLiteral("&Submit answer"), answer_group);
    submit_button_->setObjectName(QStringLiteral("submitOralArgumentAnswer"));
    submit_button_->setAccessibleName(QStringLiteral("Submit counsel answer"));
    submit_button_->setAccessibleDescription(
        QStringLiteral("Persist this answer and advance the deterministic bench simulation"));
    submit_button_->setToolTip(QStringLiteral("Submit answer (Ctrl+Return)"));
    answer_layout->addWidget(submit_button_);

    status_label_ = new QLabel(answer_group);
    configureLabel(*status_label_, QStringLiteral("oralArgumentStatus"),
                   QStringLiteral("Oral argument submission status"));
    answer_layout->addWidget(status_label_);
    outer->addWidget(answer_group);

    connect(submit_button_, &QPushButton::clicked, this,
            [this] { static_cast<void>(submitAnswer()); });
    auto* submit_shortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Return")), this);
    submit_shortcut->setObjectName(QStringLiteral("submitOralArgumentShortcut"));
    submit_shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(submit_shortcut, &QShortcut::activated, this,
            [this] { static_cast<void>(submitAnswer()); });
}

void OralArgumentWorkspace::renderUnavailable(const QString& message) {
    ready_ = false;
    mode_label_->setText(QStringLiteral("Mode: Unavailable"));
    isolation_notice_label_->setText(QStringLiteral("No canonical oral-argument session is open."));
    judge_label_->setText(QStringLiteral("Fictional/composite judge: Unavailable"));
    act_label_->setText(QStringLiteral("Bench act: Unavailable"));
    issue_label_->setText(QStringLiteral("Issue: Unavailable"));
    topic_label_->setText(QStringLiteral("Topic: Unavailable"));
    question_label_->setText(QStringLiteral("Authored question: Unavailable"));
    grounding_table_->setRowCount(0);
    transcript_view_->clear();
    principal_clock_label_->setText(QStringLiteral("--:--"));
    rebuttal_clock_label_->setText(QStringLiteral("--:--"));
    answer_kind_selector_->setEnabled(false);
    answer_editor_->setEnabled(false);
    submit_button_->setEnabled(false);
    showError(message);
}

void OralArgumentWorkspace::renderSession() {
    ready_ = false;
    if (!session_) {
        renderUnavailable(QStringLiteral("A canonical session controller is required"));
        return;
    }
    const auto* definition = session_->canonicalDefinition();
    if (definition == nullptr) {
        renderUnavailable(
            QStringLiteral("The workspace accepts only a canonical grounded-question session"));
        return;
    }
    const auto& state = session_->state();
    if (state.journal.empty() || !state.canonical_contract.has_value()) {
        renderUnavailable(QStringLiteral("The canonical session has no verified opening act"));
        return;
    }

    const auto mode = definition->question_bank.mode;
    mode_label_->setText(QStringLiteral("Mode: %1").arg(modeName(mode)));
    isolation_notice_label_->setText(isolationNotice(mode));
    principal_clock_label_->setText(formattedClock(state.principal_remaining));
    rebuttal_clock_label_->setText(formattedClock(state.rebuttal_remaining));

    const auto& current_act = state.journal.back().bench;
    const auto seat =
        std::ranges::find(definition->bench.seats, current_act.seat_id, &model::BenchSeat::id);
    judge_label_->setText(seat == definition->bench.seats.end()
                              ? QStringLiteral("Fictional/composite judge: Unknown seat %1")
                                    .arg(utf8(current_act.seat_id))
                              : QStringLiteral("Fictional/composite judge: %1 [%2]")
                                    .arg(utf8(seat->profile.display_name), utf8(seat->id)));
    act_label_->setText(QStringLiteral("Bench act: %1").arg(actName(current_act.kind)));

    grounding_table_->setRowCount(0);
    if (current_act.question.has_value()) {
        const auto& question = *current_act.question;
        issue_label_->setText(QStringLiteral("Issue: %1").arg(utf8(question.issue_id)));
        const auto* authored = std::get_if<model::AuthoredQuestionSelection>(&question.selection);
        if (authored == nullptr) {
            renderUnavailable(
                QStringLiteral("The current act is not one whole authored pack question"));
            return;
        }
        topic_label_->setText(QStringLiteral("Topic: %1 [%2]")
                                  .arg(topicName(authored->topic),
                                       utf8(model::argumentFocusTopicId(authored->topic))));
        question_label_->setText(QStringLiteral("Authored question %1: %2")
                                     .arg(utf8(authored->question_id), utf8(authored->prompt)));
        grounding_table_->setRowCount(static_cast<int>(authored->grounding.size()));
        for (std::size_t index = 0; index < authored->grounding.size(); ++index) {
            const auto row = static_cast<int>(index);
            const auto& grounding = authored->grounding.at(index);
            const auto display = groundingDisplay(grounding);
            auto* cite = new QTableWidgetItem;
            cite->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
            cite->setCheckState(Qt::Unchecked);
            cite->setData(Qt::UserRole, groundingId(grounding));
            cite->setToolTip(QStringLiteral("Cite %1").arg(groundingId(grounding)));
            grounding_table_->setItem(row, 0, cite);
            grounding_table_->setItem(row, 1, new QTableWidgetItem(display.type));
            grounding_table_->setItem(row, 2, new QTableWidgetItem(groundingId(grounding)));
            grounding_table_->setItem(row, 3, new QTableWidgetItem(display.source));
        }
    } else {
        issue_label_->setText(QStringLiteral("Issue: No further issue"));
        topic_label_->setText(QStringLiteral("Topic: No further topic"));
        question_label_->setText(QStringLiteral("Authored question: No further question"));
    }

    QStringList transcript;
    transcript.reserve(static_cast<qsizetype>(state.transcript.size()));
    for (const auto& entry : state.transcript) {
        if (entry.speaker == model::TranscriptSpeaker::Counsel) {
            transcript.push_back(QStringLiteral("[%1] Counsel: %2")
                                     .arg(entry.event_sequence)
                                     .arg(utf8(entry.utterance)));
            continue;
        }
        const auto transcript_seat =
            std::ranges::find(definition->bench.seats, entry.seat_id, &model::BenchSeat::id);
        const auto speaker = transcript_seat == definition->bench.seats.end()
                                 ? utf8(entry.seat_id)
                                 : utf8(transcript_seat->profile.display_name);
        const auto kind = entry.bench_act_kind.has_value() ? actName(*entry.bench_act_kind)
                                                           : QStringLiteral("Bench");
        transcript.push_back(QStringLiteral("[%1] %2 — %3: %4")
                                 .arg(entry.event_sequence)
                                 .arg(speaker, kind, utf8(entry.utterance)));
    }
    transcript_view_->setPlainText(transcript.join(u'\n'));
    answer_kind_selector_->setEnabled(state.phase != model::OralArgumentPhase::Complete);
    answer_editor_->setEnabled(state.phase != model::OralArgumentPhase::Complete);
    submit_button_->setEnabled(state.phase != model::OralArgumentPhase::Complete &&
                               current_act.question.has_value());
    ready_ = true;
    answer_elapsed_timer_.restart();
    last_error_.clear();
    showStatus(state.phase == model::OralArgumentPhase::Complete
                   ? QStringLiteral("Oral argument complete.")
                   : QStringLiteral("Ready for counsel's answer. Ctrl+Return submits."));
}

auto OralArgumentWorkspace::submitAnswer() -> std::expected<void, QString> {
    if (!isReady() || !submit_button_->isEnabled()) {
        const auto message = QStringLiteral("No answerable canonical bench question is open");
        showError(message);
        return std::unexpected(message);
    }
    const auto text = answer_editor_->toPlainText();
    if (text.trimmed().isEmpty()) {
        const auto message = QStringLiteral("Enter a nonempty answer before submitting");
        showError(message);
        return std::unexpected(message);
    }

    const auto& current = session_->state().journal.back().bench;
    if (!current.question.has_value() ||
        std::get_if<model::AuthoredQuestionSelection>(&current.question->selection) == nullptr) {
        const auto message = QStringLiteral("The current exact authored question is unavailable");
        showError(message);
        return std::unexpected(message);
    }

    std::vector<std::string> citations;
    citations.reserve(static_cast<std::size_t>(grounding_table_->rowCount()));
    for (int row = 0; row < grounding_table_->rowCount(); ++row) {
        const auto* item = grounding_table_->item(row, 0);
        if (item != nullptr && item->checkState() == Qt::Checked) {
            citations.push_back(item->data(Qt::UserRole).toString().toStdString());
        }
    }
    const auto elapsed =
        elapsed_clock_
            ? elapsed_clock_()
            : std::chrono::seconds{std::max<qint64>(1, answer_elapsed_timer_.elapsed() / 1'000)};
    const auto recorded_at = recorded_at_clock_ ? recorded_at_clock_() : defaultRecordedAt();
    const auto kind =
        static_cast<model::CounselActKind>(answer_kind_selector_->currentData().toInt());
    const auto answer = model::CounselAnswer{
        kind,    text.toUtf8().toStdString(), current.question->issue_id, std::move(citations), 1.0,
        elapsed,
    };
    const auto command_id =
        session_->sessionId() + QStringLiteral(".answer-") +
        QString::number(static_cast<qulonglong>(session_->state().next_event_sequence));
    const auto submitted = session_->submit(command_id, answer, recorded_at);
    if (!submitted) {
        showError(submitted.error().message);
        return std::unexpected(submitted.error().message);
    }

    answer_editor_->clear();
    renderSession();
    showStatus(QStringLiteral("Answer persisted as event %1.").arg(submitted->persisted_sequence));
    return {};
}

bool OralArgumentWorkspace::isReady() const noexcept { return ready_; }

QString OralArgumentWorkspace::lastError() const { return last_error_; }

const model::OralArgumentState* OralArgumentWorkspace::sessionState() const noexcept {
    return session_ ? &session_->state() : nullptr;
}

const model::CanonicalOralArgumentDefinition*
OralArgumentWorkspace::canonicalDefinition() const noexcept {
    return session_ ? session_->canonicalDefinition() : nullptr;
}

void OralArgumentWorkspace::showError(const QString& message) {
    last_error_ = message;
    status_label_->setText(QStringLiteral("Error: %1").arg(message));
    status_label_->setStyleSheet(QStringLiteral("color: #a32121; font-weight: 600;"));
}

void OralArgumentWorkspace::showStatus(const QString& message) {
    last_error_.clear();
    status_label_->setText(message);
    status_label_->setStyleSheet({});
}

QLabel* OralArgumentWorkspace::modeLabel() const noexcept { return mode_label_; }
QLabel* OralArgumentWorkspace::isolationNoticeLabel() const noexcept {
    return isolation_notice_label_;
}
QLabel* OralArgumentWorkspace::judgeLabel() const noexcept { return judge_label_; }
QLabel* OralArgumentWorkspace::actLabel() const noexcept { return act_label_; }
QLabel* OralArgumentWorkspace::issueLabel() const noexcept { return issue_label_; }
QLabel* OralArgumentWorkspace::topicLabel() const noexcept { return topic_label_; }
QLabel* OralArgumentWorkspace::questionLabel() const noexcept { return question_label_; }
QTableWidget* OralArgumentWorkspace::groundingTable() const noexcept { return grounding_table_; }
QPlainTextEdit* OralArgumentWorkspace::transcriptView() const noexcept { return transcript_view_; }
QLabel* OralArgumentWorkspace::principalClockLabel() const noexcept {
    return principal_clock_label_;
}
QLabel* OralArgumentWorkspace::rebuttalClockLabel() const noexcept { return rebuttal_clock_label_; }
QComboBox* OralArgumentWorkspace::answerKindSelector() const noexcept {
    return answer_kind_selector_;
}
QPlainTextEdit* OralArgumentWorkspace::answerEditor() const noexcept { return answer_editor_; }
QLabel* OralArgumentWorkspace::statusLabel() const noexcept { return status_label_; }
QPushButton* OralArgumentWorkspace::submitButton() const noexcept { return submit_button_; }

} // namespace appellate::ui
