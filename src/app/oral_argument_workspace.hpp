#pragma once

#include "oral_argument_session_controller.hpp"

#include "appellate/model/oral_argument.hpp"

#include <QElapsedTimer>
#include <QWidget>

#include <chrono>
#include <expected>
#include <functional>
#include <memory>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;

namespace appellate::ui {

// A narrow presentation seam keeps widget tests deterministic.  The production constructor
// below always wraps the real, persistence-verifying OralArgumentSessionController.
class OralArgumentWorkspaceSession {
  public:
    virtual ~OralArgumentWorkspaceSession() = default;

    [[nodiscard]] virtual const QString& sessionId() const noexcept = 0;
    [[nodiscard]] virtual const model::CanonicalOralArgumentDefinition*
    canonicalDefinition() const noexcept = 0;
    [[nodiscard]] virtual const model::OralArgumentState& state() const noexcept = 0;
    [[nodiscard]] virtual auto submit(QString command_id, const model::CounselAnswer& answer,
                                      const QString& recorded_at_utc)
        -> std::expected<app::OralArgumentSubmissionResult, app::OralArgumentSessionError> = 0;
};

class OralArgumentWorkspace final : public QWidget {
  public:
    using ElapsedClock = std::function<std::chrono::seconds()>;
    using RecordedAtClock = std::function<QString()>;

    explicit OralArgumentWorkspace(QWidget* parent = nullptr);
    explicit OralArgumentWorkspace(std::unique_ptr<app::OralArgumentSessionController> controller,
                                   ElapsedClock elapsed_clock = {},
                                   RecordedAtClock recorded_at_clock = {},
                                   QWidget* parent = nullptr);
    explicit OralArgumentWorkspace(std::unique_ptr<OralArgumentWorkspaceSession> session,
                                   ElapsedClock elapsed_clock = {},
                                   RecordedAtClock recorded_at_clock = {},
                                   QWidget* parent = nullptr);
    ~OralArgumentWorkspace() override;

    OralArgumentWorkspace(const OralArgumentWorkspace&) = delete;
    OralArgumentWorkspace& operator=(const OralArgumentWorkspace&) = delete;
    OralArgumentWorkspace(OralArgumentWorkspace&&) = delete;
    OralArgumentWorkspace& operator=(OralArgumentWorkspace&&) = delete;

    [[nodiscard]] auto submitAnswer() -> std::expected<void, QString>;
    [[nodiscard]] bool isReady() const noexcept;
    [[nodiscard]] QString lastError() const;
    [[nodiscard]] const model::OralArgumentState* sessionState() const noexcept;
    [[nodiscard]] const model::CanonicalOralArgumentDefinition*
    canonicalDefinition() const noexcept;

    [[nodiscard]] QLabel* modeLabel() const noexcept;
    [[nodiscard]] QLabel* isolationNoticeLabel() const noexcept;
    [[nodiscard]] QLabel* judgeLabel() const noexcept;
    [[nodiscard]] QLabel* actLabel() const noexcept;
    [[nodiscard]] QLabel* issueLabel() const noexcept;
    [[nodiscard]] QLabel* topicLabel() const noexcept;
    [[nodiscard]] QLabel* questionLabel() const noexcept;
    [[nodiscard]] QTableWidget* groundingTable() const noexcept;
    [[nodiscard]] QPlainTextEdit* transcriptView() const noexcept;
    [[nodiscard]] QLabel* principalClockLabel() const noexcept;
    [[nodiscard]] QLabel* rebuttalClockLabel() const noexcept;
    [[nodiscard]] QComboBox* answerKindSelector() const noexcept;
    [[nodiscard]] QPlainTextEdit* answerEditor() const noexcept;
    [[nodiscard]] QLabel* statusLabel() const noexcept;
    [[nodiscard]] QPushButton* submitButton() const noexcept;

  private:
    void buildUi();
    void renderSession();
    void renderUnavailable(const QString& message);
    void showError(const QString& message);
    void showStatus(const QString& message);

    std::unique_ptr<OralArgumentWorkspaceSession> session_;
    ElapsedClock elapsed_clock_;
    RecordedAtClock recorded_at_clock_;
    QElapsedTimer answer_elapsed_timer_;
    QString last_error_;
    bool ready_{};

    QLabel* mode_label_{};
    QLabel* isolation_notice_label_{};
    QLabel* judge_label_{};
    QLabel* act_label_{};
    QLabel* issue_label_{};
    QLabel* topic_label_{};
    QLabel* question_label_{};
    QTableWidget* grounding_table_{};
    QPlainTextEdit* transcript_view_{};
    QLabel* principal_clock_label_{};
    QLabel* rebuttal_clock_label_{};
    QComboBox* answer_kind_selector_{};
    QPlainTextEdit* answer_editor_{};
    QLabel* status_label_{};
    QPushButton* submit_button_{};
};

} // namespace appellate::ui
