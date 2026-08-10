#pragma once

#include "bench_profile_codec.hpp"

#include <QWidget>

#include <expected>
#include <optional>
#include <string_view>
#include <vector>

class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;

namespace appellate::ui {

enum class InteractionControl {
    Directness,
    Formality,
    QuestionLength,
    InterruptionFrequency,
    FollowUpDepth,
    HypotheticalFrequency,
    ConcessionRecall,
    TimeStrictness,
};

class BenchProfileEditor final : public QWidget {
  public:
    explicit BenchProfileEditor(QWidget* parent = nullptr);

    [[nodiscard]] auto loadProfile(const model::JudgeProfile& profile)
        -> std::expected<void, BenchProfileError>;
    [[nodiscard]] auto cloneProfile(std::string_view namespaced_id, std::string_view display_name)
        -> std::expected<void, BenchProfileError>;
    [[nodiscard]] auto profile() const -> std::expected<model::JudgeProfile, BenchProfileError>;

    [[nodiscard]] auto importProfile(QStringView path) -> std::expected<void, BenchProfileError>;
    [[nodiscard]] auto exportProfile(QStringView path) const
        -> std::expected<void, BenchProfileError>;

    [[nodiscard]] QString previewText() const;
    [[nodiscard]] QLabel* fictionalCompositeLabel() const noexcept;
    [[nodiscard]] QLabel* previewLabel() const noexcept;
    [[nodiscard]] QDoubleSpinBox* interactionControl(InteractionControl control) const noexcept;
    [[nodiscard]] QComboBox* voiceRegisterControl() const noexcept;
    [[nodiscard]] QComboBox* voiceCadenceControl() const noexcept;
    [[nodiscard]] QDoubleSpinBox* verbosityControl() const noexcept;
    [[nodiscard]] QDoubleSpinBox* sentenceComplexityControl() const noexcept;
    [[nodiscard]] QDoubleSpinBox* issueFocusControl(std::string_view topic_id) const noexcept;

  private:
    struct IssueFocusRow final {
        std::string topic_id;
        QDoubleSpinBox* control{};
    };

    [[nodiscard]] QDoubleSpinBox* addUnitControl(QFormLayout& layout, const QString& label,
                                                 const QString& object_name,
                                                 const QString& accessible_name);
    void rebuildIssueFocusControls(const std::vector<model::IssueFocus>& focus);
    void refreshPreview();
    void updateIdentityLabel();
    void rebuildTabOrder();

    std::optional<model::JudgeProfile> loaded_profile_;
    QLabel* fictional_composite_label_{};
    QLabel* identity_label_{};
    QLabel* preview_label_{};
    QFormLayout* issue_focus_layout_{};
    QDoubleSpinBox* directness_{};
    QDoubleSpinBox* formality_{};
    QDoubleSpinBox* question_length_{};
    QDoubleSpinBox* interruption_frequency_{};
    QDoubleSpinBox* follow_up_depth_{};
    QDoubleSpinBox* hypothetical_frequency_{};
    QDoubleSpinBox* concession_recall_{};
    QDoubleSpinBox* time_strictness_{};
    QComboBox* voice_register_{};
    QComboBox* voice_cadence_{};
    QDoubleSpinBox* verbosity_{};
    QDoubleSpinBox* sentence_complexity_{};
    std::vector<IssueFocusRow> issue_focus_rows_;
};

} // namespace appellate::ui
