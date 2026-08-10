#include "bench_profile_editor.hpp"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStringList>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <functional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace appellate::ui {
namespace {

[[nodiscard]] auto noProfileError() -> std::unexpected<BenchProfileError> {
    return std::unexpected(BenchProfileError{
        BenchProfileErrorCode::InvalidField,
        QStringLiteral("Load a valid fictional/composite profile before editing"),
    });
}

[[nodiscard]] QString utf8(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

} // namespace

BenchProfileEditor::BenchProfileEditor(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("benchProfileEditor"));
    setAccessibleName(QStringLiteral("Fictional composite bench profile editor"));

    auto* outer_layout = new QVBoxLayout(this);
    fictional_composite_label_ = new QLabel(
        QStringLiteral("Fictional/composite profile — interaction and voice only; this profile "
                       "cannot alter facts, legal rules, deadlines, or outcomes."),
        this);
    fictional_composite_label_->setObjectName(QStringLiteral("fictionalCompositeNotice"));
    fictional_composite_label_->setAccessibleName(
        QStringLiteral("Fictional composite profile notice"));
    fictional_composite_label_->setWordWrap(true);
    outer_layout->addWidget(fictional_composite_label_);

    identity_label_ = new QLabel(QStringLiteral("No fictional/composite profile loaded"), this);
    identity_label_->setObjectName(QStringLiteral("profileIdentity"));
    identity_label_->setAccessibleName(QStringLiteral("Loaded profile identity"));
    identity_label_->setTextInteractionFlags(Qt::TextSelectableByKeyboard |
                                             Qt::TextSelectableByMouse);
    outer_layout->addWidget(identity_label_);

    auto* scroll_area = new QScrollArea(this);
    scroll_area->setObjectName(QStringLiteral("profileControlsScrollArea"));
    scroll_area->setAccessibleName(QStringLiteral("Bench profile controls"));
    scroll_area->setWidgetResizable(true);
    scroll_area->setFocusPolicy(Qt::NoFocus);
    auto* controls_widget = new QWidget(scroll_area);
    auto* controls_layout = new QVBoxLayout(controls_widget);

    auto* interaction_group =
        new QGroupBox(QStringLiteral("Interaction behavior"), controls_widget);
    interaction_group->setObjectName(QStringLiteral("interactionBehaviorGroup"));
    auto* interaction_layout = new QFormLayout(interaction_group);
    interaction_layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    directness_ = addUnitControl(*interaction_layout, QStringLiteral("&Directness"),
                                 QStringLiteral("directnessControl"), QStringLiteral("Directness"));
    formality_ = addUnitControl(*interaction_layout, QStringLiteral("&Formality"),
                                QStringLiteral("formalityControl"), QStringLiteral("Formality"));
    question_length_ =
        addUnitControl(*interaction_layout, QStringLiteral("Question &length"),
                       QStringLiteral("questionLengthControl"), QStringLiteral("Question length"));
    interruption_frequency_ = addUnitControl(
        *interaction_layout, QStringLiteral("&Interruption frequency"),
        QStringLiteral("interruptionFrequencyControl"), QStringLiteral("Interruption frequency"));
    follow_up_depth_ =
        addUnitControl(*interaction_layout, QStringLiteral("Follow-up &depth"),
                       QStringLiteral("followUpDepthControl"), QStringLiteral("Follow-up depth"));
    hypothetical_frequency_ = addUnitControl(
        *interaction_layout, QStringLiteral("&Hypothetical frequency"),
        QStringLiteral("hypotheticalFrequencyControl"), QStringLiteral("Hypothetical frequency"));
    concession_recall_ = addUnitControl(*interaction_layout, QStringLiteral("Concession &recall"),
                                        QStringLiteral("concessionRecallControl"),
                                        QStringLiteral("Concession recall"));
    time_strictness_ =
        addUnitControl(*interaction_layout, QStringLiteral("&Time strictness"),
                       QStringLiteral("timeStrictnessControl"), QStringLiteral("Time strictness"));
    controls_layout->addWidget(interaction_group);

    auto* voice_group = new QGroupBox(QStringLiteral("Structured voice"), controls_widget);
    voice_group->setObjectName(QStringLiteral("structuredVoiceGroup"));
    auto* voice_layout = new QFormLayout(voice_group);
    voice_layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    voice_register_ = new QComboBox(voice_group);
    voice_register_->setObjectName(QStringLiteral("voiceRegisterControl"));
    voice_register_->setAccessibleName(QStringLiteral("Voice register"));
    voice_register_->addItem(QStringLiteral("Plain"),
                             static_cast<int>(model::VoiceRegister::Plain));
    voice_register_->addItem(QStringLiteral("Formal"),
                             static_cast<int>(model::VoiceRegister::Formal));
    voice_register_->addItem(QStringLiteral("Technical"),
                             static_cast<int>(model::VoiceRegister::Technical));
    auto* register_label = new QLabel(QStringLiteral("Voice &register"), voice_group);
    register_label->setObjectName(QStringLiteral("voiceRegisterLabel"));
    register_label->setAccessibleName(QStringLiteral("Voice register label"));
    register_label->setBuddy(voice_register_);
    voice_layout->addRow(register_label, voice_register_);

    voice_cadence_ = new QComboBox(voice_group);
    voice_cadence_->setObjectName(QStringLiteral("voiceCadenceControl"));
    voice_cadence_->setAccessibleName(QStringLiteral("Voice cadence"));
    voice_cadence_->addItem(QStringLiteral("Clipped"),
                            static_cast<int>(model::VoiceCadence::Clipped));
    voice_cadence_->addItem(QStringLiteral("Measured"),
                            static_cast<int>(model::VoiceCadence::Measured));
    voice_cadence_->addItem(QStringLiteral("Expansive"),
                            static_cast<int>(model::VoiceCadence::Expansive));
    auto* cadence_label = new QLabel(QStringLiteral("Voice &cadence"), voice_group);
    cadence_label->setObjectName(QStringLiteral("voiceCadenceLabel"));
    cadence_label->setAccessibleName(QStringLiteral("Voice cadence label"));
    cadence_label->setBuddy(voice_cadence_);
    voice_layout->addRow(cadence_label, voice_cadence_);

    verbosity_ =
        addUnitControl(*voice_layout, QStringLiteral("&Verbosity"),
                       QStringLiteral("verbosityControl"), QStringLiteral("Voice verbosity"));
    sentence_complexity_ = addUnitControl(*voice_layout, QStringLiteral("Sentence comple&xity"),
                                          QStringLiteral("sentenceComplexityControl"),
                                          QStringLiteral("Sentence complexity"));
    controls_layout->addWidget(voice_group);

    auto* focus_group = new QGroupBox(QStringLiteral("Issue-focus weights"), controls_widget);
    focus_group->setObjectName(QStringLiteral("issueFocusGroup"));
    issue_focus_layout_ = new QFormLayout(focus_group);
    issue_focus_layout_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    controls_layout->addWidget(focus_group);
    controls_layout->addStretch();
    scroll_area->setWidget(controls_widget);
    outer_layout->addWidget(scroll_area, 1);

    preview_label_ =
        new QLabel(QStringLiteral("Load a profile to preview its structured voice."), this);
    preview_label_->setObjectName(QStringLiteral("structuredVoicePreview"));
    preview_label_->setAccessibleName(QStringLiteral("Structured fictional voice preview"));
    preview_label_->setWordWrap(true);
    preview_label_->setTextInteractionFlags(Qt::TextSelectableByKeyboard |
                                            Qt::TextSelectableByMouse);
    outer_layout->addWidget(preview_label_);

    const std::array<QDoubleSpinBox*, 10> scalar_controls{
        directness_,        formality_,
        question_length_,   interruption_frequency_,
        follow_up_depth_,   hypothetical_frequency_,
        concession_recall_, time_strictness_,
        verbosity_,         sentence_complexity_,
    };
    for (auto* control : scalar_controls) {
        connect(control, &QDoubleSpinBox::valueChanged, this, [this](double) { refreshPreview(); });
    }
    connect(voice_register_, &QComboBox::currentIndexChanged, this,
            [this](int) { refreshPreview(); });
    connect(voice_cadence_, &QComboBox::currentIndexChanged, this,
            [this](int) { refreshPreview(); });
    rebuildTabOrder();
}

QDoubleSpinBox* BenchProfileEditor::addUnitControl(QFormLayout& layout, const QString& label,
                                                   const QString& object_name,
                                                   const QString& accessible_name) {
    auto* control = new QDoubleSpinBox(this);
    control->setObjectName(object_name);
    control->setAccessibleName(accessible_name);
    control->setRange(0.0, 1.0);
    control->setDecimals(3);
    control->setSingleStep(0.05);
    control->setKeyboardTracking(false);
    control->setFocusPolicy(Qt::StrongFocus);
    auto* field_label = new QLabel(label, this);
    field_label->setObjectName(object_name + QStringLiteral("Label"));
    field_label->setAccessibleName(accessible_name + QStringLiteral(" label"));
    field_label->setBuddy(control);
    layout.addRow(field_label, control);
    return control;
}

auto BenchProfileEditor::loadProfile(const model::JudgeProfile& profile)
    -> std::expected<void, BenchProfileError> {
    if (const auto valid = BenchProfileCodec::validate(profile); !valid) {
        return std::unexpected(valid.error());
    }

    loaded_profile_ = profile;
    const std::array blockers{
        QSignalBlocker(directness_),        QSignalBlocker(formality_),
        QSignalBlocker(question_length_),   QSignalBlocker(interruption_frequency_),
        QSignalBlocker(follow_up_depth_),   QSignalBlocker(hypothetical_frequency_),
        QSignalBlocker(concession_recall_), QSignalBlocker(time_strictness_),
        QSignalBlocker(voice_register_),    QSignalBlocker(voice_cadence_),
        QSignalBlocker(verbosity_),         QSignalBlocker(sentence_complexity_),
    };
    static_cast<void>(blockers);

    directness_->setValue(profile.interaction.directness);
    formality_->setValue(profile.interaction.formality);
    question_length_->setValue(profile.interaction.question_length);
    interruption_frequency_->setValue(profile.interaction.interruption_frequency);
    follow_up_depth_->setValue(profile.interaction.follow_up_depth);
    hypothetical_frequency_->setValue(profile.interaction.hypothetical_frequency);
    concession_recall_->setValue(profile.interaction.concession_recall);
    time_strictness_->setValue(profile.interaction.time_strictness);
    voice_register_->setCurrentIndex(
        voice_register_->findData(static_cast<int>(profile.voice.register_style)));
    voice_cadence_->setCurrentIndex(
        voice_cadence_->findData(static_cast<int>(profile.voice.cadence)));
    verbosity_->setValue(profile.voice.verbosity);
    sentence_complexity_->setValue(profile.voice.sentence_complexity);
    rebuildIssueFocusControls(profile.interaction.issue_focus);
    updateIdentityLabel();
    refreshPreview();
    rebuildTabOrder();
    return {};
}

auto BenchProfileEditor::cloneProfile(std::string_view namespaced_id, std::string_view display_name)
    -> std::expected<void, BenchProfileError> {
    const auto edited = profile();
    if (!edited) {
        return std::unexpected(edited.error());
    }
    auto clone = *edited;
    clone.id.assign(namespaced_id);
    clone.display_name.assign(display_name);
    return loadProfile(clone);
}

auto BenchProfileEditor::profile() const -> std::expected<model::JudgeProfile, BenchProfileError> {
    if (!loaded_profile_) {
        return noProfileError();
    }
    auto edited = *loaded_profile_;
    edited.interaction.directness = directness_->value();
    edited.interaction.formality = formality_->value();
    edited.interaction.question_length = question_length_->value();
    edited.interaction.interruption_frequency = interruption_frequency_->value();
    edited.interaction.follow_up_depth = follow_up_depth_->value();
    edited.interaction.hypothetical_frequency = hypothetical_frequency_->value();
    edited.interaction.concession_recall = concession_recall_->value();
    edited.interaction.time_strictness = time_strictness_->value();
    edited.voice.register_style =
        static_cast<model::VoiceRegister>(voice_register_->currentData().toInt());
    edited.voice.cadence = static_cast<model::VoiceCadence>(voice_cadence_->currentData().toInt());
    edited.voice.verbosity = verbosity_->value();
    edited.voice.sentence_complexity = sentence_complexity_->value();
    edited.interaction.issue_focus.clear();
    edited.interaction.issue_focus.reserve(issue_focus_rows_.size());
    for (const auto& row : issue_focus_rows_) {
        edited.interaction.issue_focus.push_back(
            model::IssueFocus{row.topic_id, row.control->value()});
    }
    if (const auto valid = BenchProfileCodec::validate(edited); !valid) {
        return std::unexpected(valid.error());
    }
    return edited;
}

auto BenchProfileEditor::importProfile(QStringView path) -> std::expected<void, BenchProfileError> {
    const auto imported = BenchProfileCodec::importFile(path);
    if (!imported) {
        return std::unexpected(imported.error());
    }
    return loadProfile(*imported);
}

auto BenchProfileEditor::exportProfile(QStringView path) const
    -> std::expected<void, BenchProfileError> {
    const auto edited = profile();
    if (!edited) {
        return std::unexpected(edited.error());
    }
    return BenchProfileCodec::exportFile(*edited, path);
}

QString BenchProfileEditor::previewText() const { return preview_label_->text(); }

QLabel* BenchProfileEditor::fictionalCompositeLabel() const noexcept {
    return fictional_composite_label_;
}

QLabel* BenchProfileEditor::previewLabel() const noexcept { return preview_label_; }

QDoubleSpinBox* BenchProfileEditor::interactionControl(InteractionControl control) const noexcept {
    switch (control) {
    case InteractionControl::Directness:
        return directness_;
    case InteractionControl::Formality:
        return formality_;
    case InteractionControl::QuestionLength:
        return question_length_;
    case InteractionControl::InterruptionFrequency:
        return interruption_frequency_;
    case InteractionControl::FollowUpDepth:
        return follow_up_depth_;
    case InteractionControl::HypotheticalFrequency:
        return hypothetical_frequency_;
    case InteractionControl::ConcessionRecall:
        return concession_recall_;
    case InteractionControl::TimeStrictness:
        return time_strictness_;
    }
    return nullptr;
}

QComboBox* BenchProfileEditor::voiceRegisterControl() const noexcept { return voice_register_; }

QComboBox* BenchProfileEditor::voiceCadenceControl() const noexcept { return voice_cadence_; }

QDoubleSpinBox* BenchProfileEditor::verbosityControl() const noexcept { return verbosity_; }

QDoubleSpinBox* BenchProfileEditor::sentenceComplexityControl() const noexcept {
    return sentence_complexity_;
}

QDoubleSpinBox* BenchProfileEditor::issueFocusControl(std::string_view topic_id) const noexcept {
    const auto found = std::ranges::find_if(issue_focus_rows_, [topic_id](const auto& row) {
        return std::string_view(row.topic_id) == topic_id;
    });
    return found == issue_focus_rows_.end() ? nullptr : found->control;
}

void BenchProfileEditor::rebuildIssueFocusControls(const std::vector<model::IssueFocus>& focus) {
    while (issue_focus_layout_->count() > 0) {
        auto* item = issue_focus_layout_->takeAt(0);
        delete item->widget();
        delete item;
    }
    issue_focus_rows_.clear();
    issue_focus_rows_.reserve(focus.size());
    for (const auto& item : focus) {
        const auto topic = utf8(item.topic_id);
        auto* control = new QDoubleSpinBox(this);
        control->setObjectName(QStringLiteral("issueFocusWeight.%1").arg(topic));
        control->setAccessibleName(QStringLiteral("Issue focus weight for %1").arg(topic));
        control->setRange(0.0, 1.0);
        control->setDecimals(3);
        control->setSingleStep(0.05);
        control->setKeyboardTracking(false);
        control->setFocusPolicy(Qt::StrongFocus);
        control->setValue(item.weight);
        auto* label = new QLabel(QStringLiteral("%1 &weight").arg(topic), this);
        label->setObjectName(QStringLiteral("issueFocusLabel.%1").arg(topic));
        label->setAccessibleName(QStringLiteral("Issue focus label for %1").arg(topic));
        label->setBuddy(control);
        issue_focus_layout_->addRow(label, control);
        connect(control, &QDoubleSpinBox::valueChanged, this, [this](double) { refreshPreview(); });
        issue_focus_rows_.push_back(IssueFocusRow{item.topic_id, control});
    }
}

void BenchProfileEditor::refreshPreview() {
    if (!loaded_profile_) {
        preview_label_->setText(QStringLiteral("Load a profile to preview its structured voice."));
        return;
    }

    const auto direct_opening = directness_->value() >= 0.5
                                    ? QStringLiteral("Identify the record support")
                                    : QStringLiteral("Would you explain the record support");
    const auto formality_phrase =
        formality_->value() >= 0.5 ? QStringLiteral("Counsel, %1") : QStringLiteral("Please %1");
    QString sample = formality_phrase.arg(direct_opening.toLower());
    if (question_length_->value() >= 0.5) {
        sample += QStringLiteral(" and connect it to the governing standard of review");
    }
    sample += u'?';

    QStringList focus;
    focus.reserve(static_cast<qsizetype>(issue_focus_rows_.size()));
    for (const auto& row : issue_focus_rows_) {
        focus.push_back(
            QStringLiteral("%1=%2").arg(utf8(row.topic_id)).arg(row.control->value(), 0, 'f', 3));
    }
    preview_label_->setText(
        QStringLiteral("Fictional/composite structured preview — register=%1; cadence=%2; "
                       "directness=%3; formality=%4; question-length=%5; interruptions=%6; "
                       "follow-up=%7; hypotheticals=%8; concession-recall=%9; "
                       "time-strictness=%10; verbosity=%11; sentence-complexity=%12; "
                       "issue-focus=[%13]. Sample: %14")
            .arg(voice_register_->currentText().toLower(), voice_cadence_->currentText().toLower())
            .arg(directness_->value(), 0, 'f', 3)
            .arg(formality_->value(), 0, 'f', 3)
            .arg(question_length_->value(), 0, 'f', 3)
            .arg(interruption_frequency_->value(), 0, 'f', 3)
            .arg(follow_up_depth_->value(), 0, 'f', 3)
            .arg(hypothetical_frequency_->value(), 0, 'f', 3)
            .arg(concession_recall_->value(), 0, 'f', 3)
            .arg(time_strictness_->value(), 0, 'f', 3)
            .arg(verbosity_->value(), 0, 'f', 3)
            .arg(sentence_complexity_->value(), 0, 'f', 3)
            .arg(focus.join(QStringLiteral(", ")), sample));
}

void BenchProfileEditor::updateIdentityLabel() {
    if (!loaded_profile_) {
        identity_label_->setText(QStringLiteral("No fictional/composite profile loaded"));
        return;
    }
    identity_label_->setText(
        QStringLiteral("Fictional/composite profile: %1 (%2)")
            .arg(utf8(loaded_profile_->display_name), utf8(loaded_profile_->id)));
}

void BenchProfileEditor::rebuildTabOrder() {
    std::vector<QWidget*> controls{
        directness_,        formality_,
        question_length_,   interruption_frequency_,
        follow_up_depth_,   hypothetical_frequency_,
        concession_recall_, time_strictness_,
        voice_register_,    voice_cadence_,
        verbosity_,         sentence_complexity_,
    };
    controls.reserve(controls.size() + issue_focus_rows_.size());
    for (const auto& row : issue_focus_rows_) {
        controls.push_back(row.control);
    }
    for (std::size_t index = 1; index < controls.size(); ++index) {
        QWidget::setTabOrder(controls[index - 1], controls[index]);
    }
}

} // namespace appellate::ui
