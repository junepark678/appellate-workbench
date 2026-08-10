#include "bench_profile_editor.hpp"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPlainTextEdit>
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

[[nodiscard]] QString phraseText(const std::vector<std::string>& phrases) {
    QStringList lines;
    lines.reserve(static_cast<qsizetype>(phrases.size()));
    for (const auto& phrase : phrases) {
        lines.push_back(utf8(phrase));
    }
    return lines.join(u'\n');
}

[[nodiscard]] std::vector<std::string> phrasesFrom(const QPlainTextEdit& control) {
    const auto lines = control.toPlainText().split(u'\n', Qt::KeepEmptyParts);
    std::vector<std::string> result;
    result.reserve(static_cast<std::size_t>(lines.size()));
    for (const auto& line : lines) {
        result.push_back(line.toUtf8().toStdString());
    }
    return result;
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
    record_pin_demand_ = addUnitControl(*interaction_layout, QStringLiteral("Record-&pin demand"),
                                        QStringLiteral("recordPinDemandControl"),
                                        QStringLiteral("Record pin demand"));
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

    question_framing_ = new QComboBox(voice_group);
    question_framing_->setObjectName(QStringLiteral("questionFramingControl"));
    question_framing_->setAccessibleName(QStringLiteral("Question framing"));
    question_framing_->addItem(QStringLiteral("Direct"),
                               static_cast<int>(model::QuestionFraming::Direct));
    question_framing_->addItem(QStringLiteral("Socratic"),
                               static_cast<int>(model::QuestionFraming::Socratic));
    question_framing_->addItem(QStringLiteral("Narrative"),
                               static_cast<int>(model::QuestionFraming::Narrative));
    auto* framing_label = new QLabel(QStringLiteral("Question fra&ming"), voice_group);
    framing_label->setObjectName(QStringLiteral("questionFramingLabel"));
    framing_label->setAccessibleName(QStringLiteral("Question framing label"));
    framing_label->setBuddy(question_framing_);
    voice_layout->addRow(framing_label, question_framing_);

    address_convention_ = new QComboBox(voice_group);
    address_convention_->setObjectName(QStringLiteral("addressConventionControl"));
    address_convention_->setAccessibleName(QStringLiteral("Address convention"));
    address_convention_->addItem(QStringLiteral("Counsel"),
                                 static_cast<int>(model::CounselAddress::Counsel));
    address_convention_->addItem(QStringLiteral("Advocate"),
                                 static_cast<int>(model::CounselAddress::Advocate));
    auto* address_label = new QLabel(QStringLiteral("&Address convention"), voice_group);
    address_label->setObjectName(QStringLiteral("addressConventionLabel"));
    address_label->setAccessibleName(QStringLiteral("Address convention label"));
    address_label->setBuddy(address_convention_);
    voice_layout->addRow(address_label, address_convention_);

    verbosity_ =
        addUnitControl(*voice_layout, QStringLiteral("&Verbosity"),
                       QStringLiteral("verbosityControl"), QStringLiteral("Voice verbosity"));
    sentence_complexity_ = addUnitControl(*voice_layout, QStringLiteral("Sentence comple&xity"),
                                          QStringLiteral("sentenceComplexityControl"),
                                          QStringLiteral("Sentence complexity"));
    question_phrases_ = addPhraseControl(*voice_layout, QStringLiteral("Question p&hrases"),
                                         QStringLiteral("questionPhrasesControl"),
                                         QStringLiteral("Question phrases, one per line"));
    interruption_phrases_ = addPhraseControl(*voice_layout, QStringLiteral("Interruption phr&ases"),
                                             QStringLiteral("interruptionPhrasesControl"),
                                             QStringLiteral("Interruption phrases, one per line"));
    clarification_phrases_ =
        addPhraseControl(*voice_layout, QStringLiteral("Clarification phrase&s"),
                         QStringLiteral("clarificationPhrasesControl"),
                         QStringLiteral("Clarification phrases, one per line"));
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

    const std::array<QDoubleSpinBox*, 11> scalar_controls{
        directness_,          formality_,
        question_length_,     interruption_frequency_,
        follow_up_depth_,     hypothetical_frequency_,
        concession_recall_,   record_pin_demand_,
        time_strictness_,     verbosity_,
        sentence_complexity_,
    };
    for (auto* control : scalar_controls) {
        connect(control, &QDoubleSpinBox::valueChanged, this, [this](double) { refreshPreview(); });
    }
    connect(voice_register_, &QComboBox::currentIndexChanged, this,
            [this](int) { refreshPreview(); });
    connect(voice_cadence_, &QComboBox::currentIndexChanged, this,
            [this](int) { refreshPreview(); });
    connect(question_framing_, &QComboBox::currentIndexChanged, this,
            [this](int) { refreshPreview(); });
    connect(address_convention_, &QComboBox::currentIndexChanged, this,
            [this](int) { refreshPreview(); });
    for (auto* phrases : {question_phrases_, interruption_phrases_, clarification_phrases_}) {
        connect(phrases, &QPlainTextEdit::textChanged, this, [this]() { refreshPreview(); });
    }
    rebuildTabOrder();
}

QPlainTextEdit* BenchProfileEditor::addPhraseControl(QFormLayout& layout, const QString& label,
                                                     const QString& object_name,
                                                     const QString& accessible_name) {
    auto* control = new QPlainTextEdit(this);
    control->setObjectName(object_name);
    control->setAccessibleName(accessible_name);
    control->setAccessibleDescription(
        QStringLiteral("Enter between one and eight distinct literal phrases, one per line."));
    control->setTabChangesFocus(true);
    control->setFocusPolicy(Qt::StrongFocus);
    control->setMinimumHeight(72);
    auto* field_label = new QLabel(label, this);
    field_label->setObjectName(object_name + QStringLiteral("Label"));
    field_label->setAccessibleName(accessible_name + QStringLiteral(" label"));
    field_label->setBuddy(control);
    layout.addRow(field_label, control);
    return control;
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
        QSignalBlocker(directness_),           QSignalBlocker(formality_),
        QSignalBlocker(question_length_),      QSignalBlocker(interruption_frequency_),
        QSignalBlocker(follow_up_depth_),      QSignalBlocker(hypothetical_frequency_),
        QSignalBlocker(concession_recall_),    QSignalBlocker(record_pin_demand_),
        QSignalBlocker(time_strictness_),      QSignalBlocker(voice_register_),
        QSignalBlocker(voice_cadence_),        QSignalBlocker(question_framing_),
        QSignalBlocker(address_convention_),   QSignalBlocker(verbosity_),
        QSignalBlocker(sentence_complexity_),  QSignalBlocker(question_phrases_),
        QSignalBlocker(interruption_phrases_), QSignalBlocker(clarification_phrases_),
    };
    static_cast<void>(blockers);

    directness_->setValue(profile.interaction.directness);
    formality_->setValue(profile.interaction.formality);
    question_length_->setValue(profile.interaction.question_length);
    interruption_frequency_->setValue(profile.interaction.interruption_frequency);
    follow_up_depth_->setValue(profile.interaction.follow_up_depth);
    hypothetical_frequency_->setValue(profile.interaction.hypothetical_frequency);
    concession_recall_->setValue(profile.interaction.concession_recall);
    record_pin_demand_->setValue(profile.interaction.record_pin_demand);
    time_strictness_->setValue(profile.interaction.time_strictness);
    voice_register_->setCurrentIndex(
        voice_register_->findData(static_cast<int>(profile.voice.register_style)));
    voice_cadence_->setCurrentIndex(
        voice_cadence_->findData(static_cast<int>(profile.voice.cadence)));
    question_framing_->setCurrentIndex(
        question_framing_->findData(static_cast<int>(profile.voice.question_framing)));
    address_convention_->setCurrentIndex(
        address_convention_->findData(static_cast<int>(profile.voice.address_convention)));
    verbosity_->setValue(profile.voice.verbosity);
    sentence_complexity_->setValue(profile.voice.sentence_complexity);
    question_phrases_->setPlainText(phraseText(profile.voice.question_phrases));
    interruption_phrases_->setPlainText(phraseText(profile.voice.interruption_phrases));
    clarification_phrases_->setPlainText(phraseText(profile.voice.clarification_phrases));
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
    edited.interaction.record_pin_demand = record_pin_demand_->value();
    edited.interaction.time_strictness = time_strictness_->value();
    edited.voice.register_style =
        static_cast<model::VoiceRegister>(voice_register_->currentData().toInt());
    edited.voice.cadence = static_cast<model::VoiceCadence>(voice_cadence_->currentData().toInt());
    edited.voice.question_framing =
        static_cast<model::QuestionFraming>(question_framing_->currentData().toInt());
    edited.voice.address_convention =
        static_cast<model::CounselAddress>(address_convention_->currentData().toInt());
    edited.voice.verbosity = verbosity_->value();
    edited.voice.sentence_complexity = sentence_complexity_->value();
    edited.voice.question_phrases = phrasesFrom(*question_phrases_);
    edited.voice.interruption_phrases = phrasesFrom(*interruption_phrases_);
    edited.voice.clarification_phrases = phrasesFrom(*clarification_phrases_);
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
    case InteractionControl::RecordPinDemand:
        return record_pin_demand_;
    case InteractionControl::TimeStrictness:
        return time_strictness_;
    }
    return nullptr;
}

QComboBox* BenchProfileEditor::voiceRegisterControl() const noexcept { return voice_register_; }

QComboBox* BenchProfileEditor::voiceCadenceControl() const noexcept { return voice_cadence_; }

QComboBox* BenchProfileEditor::questionFramingControl() const noexcept { return question_framing_; }

QComboBox* BenchProfileEditor::addressConventionControl() const noexcept {
    return address_convention_;
}

QDoubleSpinBox* BenchProfileEditor::verbosityControl() const noexcept { return verbosity_; }

QDoubleSpinBox* BenchProfileEditor::sentenceComplexityControl() const noexcept {
    return sentence_complexity_;
}

QPlainTextEdit* BenchProfileEditor::questionPhrasesControl() const noexcept {
    return question_phrases_;
}

QPlainTextEdit* BenchProfileEditor::interruptionPhrasesControl() const noexcept {
    return interruption_phrases_;
}

QPlainTextEdit* BenchProfileEditor::clarificationPhrasesControl() const noexcept {
    return clarification_phrases_;
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

    const auto lead = question_phrases_->toPlainText().section(u'\n', 0, 0);
    QString sample = address_convention_->currentText() + QStringLiteral(", ") + lead;
    switch (static_cast<model::QuestionFraming>(question_framing_->currentData().toInt())) {
    case model::QuestionFraming::Direct:
        sample += QStringLiteral(": Identify the record support");
        break;
    case model::QuestionFraming::Socratic:
        sample += QStringLiteral("; help the court understand: where is the record support");
        break;
    case model::QuestionFraming::Narrative:
        sample += QStringLiteral("; walk us through: where the record supplies support");
        break;
    }
    if (question_length_->value() >= 0.5) {
        sample += QStringLiteral(" and connect it to the governing standard of review");
    }
    sample +=
        voice_cadence_->currentData().toInt() == static_cast<int>(model::VoiceCadence::Clipped)
            ? QStringLiteral(".")
            : QStringLiteral("; answer directly.");

    QStringList focus;
    focus.reserve(static_cast<qsizetype>(issue_focus_rows_.size()));
    for (const auto& row : issue_focus_rows_) {
        focus.push_back(
            QStringLiteral("%1=%2").arg(utf8(row.topic_id)).arg(row.control->value(), 0, 'f', 3));
    }
    auto preview =
        QStringLiteral("Fictional/composite structured preview — register=%1; cadence=%2; "
                       "framing=%3; address=%4; directness=%5; formality=%6; "
                       "question-length=%7; interruptions=%8; follow-up=%9; "
                       "hypotheticals=%10; concession-recall=%11; record-pin=%12; "
                       "time-strictness=%13; verbosity=%14; sentence-complexity=%15; "
                       "phrase-counts=%16/%17/%18; issue-focus=[%19]. Sample: %20")
            .arg(voice_register_->currentText().toLower())
            .arg(voice_cadence_->currentText().toLower())
            .arg(question_framing_->currentText().toLower())
            .arg(address_convention_->currentText().toLower())
            .arg(directness_->value(), 0, 'f', 3)
            .arg(formality_->value(), 0, 'f', 3)
            .arg(question_length_->value(), 0, 'f', 3)
            .arg(interruption_frequency_->value(), 0, 'f', 3)
            .arg(follow_up_depth_->value(), 0, 'f', 3)
            .arg(hypothetical_frequency_->value(), 0, 'f', 3)
            .arg(concession_recall_->value(), 0, 'f', 3)
            .arg(record_pin_demand_->value(), 0, 'f', 3)
            .arg(time_strictness_->value(), 0, 'f', 3)
            .arg(verbosity_->value(), 0, 'f', 3)
            .arg(sentence_complexity_->value(), 0, 'f', 3)
            .arg(question_phrases_->toPlainText().split(u'\n').size())
            .arg(interruption_phrases_->toPlainText().split(u'\n').size())
            .arg(clarification_phrases_->toPlainText().split(u'\n').size())
            .arg(focus.join(QStringLiteral(", ")))
            .arg(sample);
    preview_label_->setText(std::move(preview));
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
        directness_,           formality_,
        question_length_,      interruption_frequency_,
        follow_up_depth_,      hypothetical_frequency_,
        concession_recall_,    record_pin_demand_,
        time_strictness_,      voice_register_,
        voice_cadence_,        question_framing_,
        address_convention_,   verbosity_,
        sentence_complexity_,  question_phrases_,
        interruption_phrases_, clarification_phrases_,
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
