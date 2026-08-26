#pragma once

#include "appellate/model/record_access.hpp"
#include "appellate/model/workflow_command.hpp"
#include "appellate/packs/runtime_pack.hpp"

#include <QDate>
#include <QDateTime>
#include <QMainWindow>
#include <QStringView>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class QAction;
class QComboBox;
class QLabel;
class QListWidget;
class QLineEdit;
class QMenu;
class QPushButton;
class QTabWidget;
class QWidget;

namespace appellate::app {
class RecordAccessSessionController;
class WorkflowSessionController;
} // namespace appellate::app

namespace appellate::packs {
class PackCatalog;
}

namespace appellate::storage {
class SessionStore;
}

namespace appellate::ui {

class BenchProfileEditor;
class OralArgumentLaunchProvider;
class OralArgumentWorkspace;
class RecordWorkspace;
class WorkflowLaunchProvider;

struct RecordAccessTransitionStamp final {
    QString event_id;
    QString recorded_at_utc;

    friend bool operator==(const RecordAccessTransitionStamp&,
                           const RecordAccessTransitionStamp&) = default;
};

struct WorkflowLegalClockReading final {
    QDateTime instant_utc;
    QDate court_date;

    friend bool operator==(const WorkflowLegalClockReading&,
                           const WorkflowLegalClockReading&) = default;
};

using WorkflowLegalClock = std::function<std::expected<WorkflowLegalClockReading, QString>(
    const QDate& selected_court_date)>;
using OralElapsedClock = std::function<std::chrono::seconds()>;
using OralRecordedAtClock = std::function<QString()>;

// Supplies the caller-owned session timestamp and the two fields of each local access
// transition. Tests can inject a deterministic clock/ID source; the production default uses the
// system UTC clock and persisted session sequence. It receives only the public disclosure ID,
// never sealed metadata.
class RecordAccessTransitionProvider {
  public:
    virtual ~RecordAccessTransitionProvider() = default;

    [[nodiscard]] virtual auto createdAtUtc(QStringView session_id)
        -> std::expected<QString, QString> = 0;

    [[nodiscard]] virtual auto next(QStringView session_id, std::uint64_t next_sequence,
                                    const packs::RuntimeRecordDisclosureId& disclosure_id,
                                    model::RecordAccessAction action)
        -> std::expected<RecordAccessTransitionStamp, QString> = 0;
};

class MainWindow final : public QMainWindow {
  public:
    explicit MainWindow(const QString& source_path = {}, const QString& catalog_root = {},
                        QWidget* parent = nullptr);
    MainWindow(
        const QString& source_path, const QString& catalog_root, QWidget* parent,
        std::shared_ptr<OralArgumentLaunchProvider> oral_argument_launch_provider,
        std::shared_ptr<RecordAccessTransitionProvider> record_access_transition_provider = {},
        QString record_access_database_path = {},
        std::shared_ptr<WorkflowLaunchProvider> workflow_launch_provider = {},
        WorkflowLegalClock workflow_legal_clock = {}, OralElapsedClock oral_elapsed_clock = {},
        OralRecordedAtClock oral_recorded_at_clock = {});
    ~MainWindow() override;

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;
    MainWindow(MainWindow&&) = delete;
    MainWindow& operator=(MainWindow&&) = delete;

    [[nodiscard]] auto loadSource(const QString& source_path) -> std::expected<void, QString>;
    [[nodiscard]] auto importProfile(const QString& path) -> std::expected<void, QString>;
    [[nodiscard]] auto cloneProfile(const QString& namespaced_id, const QString& display_name)
        -> std::expected<void, QString>;
    [[nodiscard]] auto exportProfile(const QString& path) -> std::expected<void, QString>;
    [[nodiscard]] auto openSelectedRecord() -> std::expected<void, QString>;
    [[nodiscard]] auto openSelectedWorkflow() -> std::expected<void, QString>;
    [[nodiscard]] auto advanceSelectedWorkflow() -> std::expected<void, QString>;
    [[nodiscard]] auto openSelectedOralArgument() -> std::expected<void, QString>;

    [[nodiscard]] const packs::RuntimePack* currentRuntime() const noexcept;
    [[nodiscard]] QString currentSourcePath() const;
    [[nodiscard]] QString catalogRoot() const;

    [[nodiscard]] QLabel* revisionLabel() const noexcept;
    [[nodiscard]] QLabel* sourceLabel() const noexcept;
    [[nodiscard]] QLabel* errorLabel() const noexcept;
    [[nodiscard]] QLabel* courtSummaryLabel() const noexcept;
    [[nodiscard]] QLabel* procedureSummaryLabel() const noexcept;
    [[nodiscard]] QLabel* recordSummaryLabel() const noexcept;
    [[nodiscard]] QLabel* benchSummaryLabel() const noexcept;
    [[nodiscard]] QLabel* workflowStatusLabel() const noexcept;
    [[nodiscard]] QListWidget* caseList() const noexcept;
    [[nodiscard]] QComboBox* profileSelector() const noexcept;
    [[nodiscard]] QComboBox* argumentConfigurationSelector() const noexcept;
    [[nodiscard]] BenchProfileEditor* profileEditor() const noexcept;
    [[nodiscard]] OralArgumentWorkspace* oralArgumentWorkspace() const noexcept;
    [[nodiscard]] RecordWorkspace* recordWorkspace() const noexcept;
    [[nodiscard]] QTabWidget* workspaceTabs() const noexcept;

    [[nodiscard]] QAction* openDirectoryAction() const noexcept;
    [[nodiscard]] QAction* installArchiveAction() const noexcept;
    [[nodiscard]] QAction* importProfileAction() const noexcept;
    [[nodiscard]] QAction* cloneProfileAction() const noexcept;
    [[nodiscard]] QAction* exportProfileAction() const noexcept;
    [[nodiscard]] QAction* openRecordAction() const noexcept;
    [[nodiscard]] QAction* openWorkflowAction() const noexcept;
    [[nodiscard]] QAction* advanceWorkflowAction() const noexcept;
    [[nodiscard]] QAction* openOralArgumentAction() const noexcept;
    [[nodiscard]] QPushButton* openWorkflowButton() const noexcept;
    [[nodiscard]] QPushButton* advanceWorkflowButton() const noexcept;
    [[nodiscard]] QLineEdit* workflowCourtDateEditor() const noexcept;
    [[nodiscard]] QMenu* recordAccessMenu() const noexcept;
    [[nodiscard]] QString recordAccessDatabasePath() const;
    [[nodiscard]] const app::WorkflowSessionController* workflowSessionController() const noexcept;

  private:
    friend class MainWindowTestAccess;

    struct RecordAccessActionBinding final {
        std::string disclosure_id;
        QMenu* disclosure_menu{};
        QAction* grant_action{};
        QAction* revoke_action{};
    };

    struct WorkflowAdvanceChoice final {
        const model::WorkflowOperation* operation{};
        const model::CaseActor* actor{};
        std::optional<model::AdvanceWorkflowStage> command;
    };

    void buildUi();
    void buildFileMenu();
    void openCatalog(const QString& injected_root);
    void configureRecordAccessDatabase(const QString& injected_path);
    void commitRuntime(packs::RuntimePack runtime, const QString& source_path,
                       const QString& success_message,
                       std::optional<packs::ResolvedPack> installed_pack = std::nullopt);
    void invalidateRecordSelection();
    void invalidateWorkflowSelection();
    void invalidateArgumentSelection();
    void resetRecordWorkspace();
    void clearRecordAccessActions();
    void rebuildRecordAccessActions();
    void transitionRecordAccess(const std::string& disclosure_id, model::RecordAccessAction action);
    [[nodiscard]] auto openRecordAccessSession(const packs::ResolvedPack& resolved_pack,
                                               const model::CaseId& selected_case_id)
        -> std::expected<std::unique_ptr<app::RecordAccessSessionController>, QString>;
    [[nodiscard]] bool selectedCaseHasLoadedRecord() const;
    [[nodiscard]] bool selectedCaseHasLoadedWorkflow() const;
    [[nodiscard]] bool selectedCaseHasLoadedArgument() const;
    [[nodiscard]] WorkflowAdvanceChoice
    currentWorkflowAdvanceChoice(const WorkflowLegalClockReading& reading) const;
    [[nodiscard]] auto sampleWorkflowLegalClock() const
        -> std::expected<WorkflowLegalClockReading, QString>;
    void renderWorkflowStatus();
    void
    renderWorkflowStatus(const std::expected<WorkflowLegalClockReading, QString>& preview_reading);
    void updateCaseSelection(int row);
    void updateArgumentSelection(int row);
    void updateProfileSelection(int row);
    void updateActionStates();
    void
    updateActionStates(const std::expected<WorkflowLegalClockReading, QString>& preview_reading);
    void showError(const QString& message);
    void showStatus(const QString& message);

    std::unique_ptr<packs::PackCatalog> catalog_;
    std::unique_ptr<storage::SessionStore> record_access_owner_store_;
    std::unique_ptr<app::RecordAccessSessionController> record_access_controller_;
    std::unique_ptr<app::WorkflowSessionController> workflow_controller_;
    std::optional<packs::RuntimePack> runtime_pack_;
    std::optional<packs::ResolvedPack> installed_pack_;
    std::optional<model::PackRevision> record_revision_;
    std::optional<model::CaseId> record_case_id_;
    std::optional<model::PackRevision> workflow_revision_;
    std::optional<model::CaseId> workflow_case_id_;
    std::optional<model::PackRevision> argument_revision_;
    std::optional<model::CaseId> argument_case_id_;
    std::optional<packs::RuntimeArgumentConfigId> argument_configuration_id_;
    std::shared_ptr<OralArgumentLaunchProvider> oral_argument_launch_provider_;
    std::shared_ptr<WorkflowLaunchProvider> workflow_launch_provider_;
    WorkflowLegalClock workflow_legal_clock_;
    OralElapsedClock oral_elapsed_clock_;
    OralRecordedAtClock oral_recorded_at_clock_;
    std::shared_ptr<RecordAccessTransitionProvider> record_access_transition_provider_;
    std::vector<RecordAccessActionBinding> record_access_action_bindings_;
    QString current_source_path_;
    QString catalog_root_;
    QString record_access_database_path_;

    QLabel* revision_label_{};
    QLabel* source_label_{};
    QLabel* error_label_{};
    QLabel* court_summary_label_{};
    QLabel* procedure_summary_label_{};
    QLabel* record_summary_label_{};
    QLabel* bench_summary_label_{};
    QLabel* workflow_status_label_{};
    QWidget* startup_onboarding_{};
    QWidget* pack_browser_content_{};
    QPushButton* welcome_open_directory_button_{};
    QPushButton* welcome_install_archive_button_{};
    QPushButton* welcome_import_profile_button_{};
    std::vector<QWidget*> pack_only_widgets_;
    QLineEdit* workflow_court_date_editor_{};
    QPushButton* open_workflow_button_{};
    QPushButton* advance_workflow_button_{};
    QListWidget* case_list_{};
    QComboBox* argument_configuration_selector_{};
    QLabel* argument_launch_boundary_label_{};
    QComboBox* profile_selector_{};
    BenchProfileEditor* profile_editor_{};
    RecordWorkspace* record_workspace_{};
    OralArgumentWorkspace* oral_argument_workspace_{};
    QTabWidget* workspace_tabs_{};
    int browser_tab_index_{};
    int record_tab_index_{};
    int argument_tab_index_{};

    QAction* open_directory_action_{};
    QAction* install_archive_action_{};
    QAction* import_profile_action_{};
    QAction* clone_profile_action_{};
    QAction* export_profile_action_{};
    QAction* open_record_action_{};
    QAction* open_workflow_action_{};
    QAction* advance_workflow_action_{};
    QAction* open_oral_argument_action_{};
    QMenu* record_access_menu_{};
};

} // namespace appellate::ui
