#pragma once

#include "appellate/packs/runtime_pack.hpp"

#include <QMainWindow>

#include <expected>
#include <memory>
#include <optional>
#include <utility>

class QAction;
class QComboBox;
class QLabel;
class QListWidget;
class QTabWidget;

namespace appellate::packs {
class PackCatalog;
}

namespace appellate::ui {

class BenchProfileEditor;
class OralArgumentLaunchProvider;
class OralArgumentWorkspace;
class RecordWorkspace;

class MainWindow final : public QMainWindow {
  public:
    explicit MainWindow(const QString& source_path = {}, const QString& catalog_root = {},
                        QWidget* parent = nullptr);
    MainWindow(const QString& source_path, const QString& catalog_root, QWidget* parent,
               std::shared_ptr<OralArgumentLaunchProvider> oral_argument_launch_provider);
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
    [[nodiscard]] QAction* openOralArgumentAction() const noexcept;

  private:
    void buildUi();
    void buildFileMenu();
    void openCatalog(const QString& injected_root);
    void commitRuntime(packs::RuntimePack runtime, const QString& source_path,
                       const QString& success_message,
                       std::optional<packs::ResolvedPack> installed_pack = std::nullopt);
    void invalidateRecordSelection();
    void invalidateArgumentSelection();
    [[nodiscard]] bool selectedCaseHasLoadedRecord() const;
    [[nodiscard]] bool selectedCaseHasLoadedArgument() const;
    void updateCaseSelection(int row);
    void updateArgumentSelection(int row);
    void updateProfileSelection(int row);
    void updateActionStates();
    void showError(const QString& message);
    void showStatus(const QString& message);

    std::unique_ptr<packs::PackCatalog> catalog_;
    std::optional<packs::RuntimePack> runtime_pack_;
    std::optional<packs::ResolvedPack> installed_pack_;
    std::optional<model::PackRevision> record_revision_;
    std::optional<model::CaseId> record_case_id_;
    std::optional<model::PackRevision> argument_revision_;
    std::optional<model::CaseId> argument_case_id_;
    std::optional<packs::RuntimeArgumentConfigId> argument_configuration_id_;
    std::shared_ptr<OralArgumentLaunchProvider> oral_argument_launch_provider_;
    QString current_source_path_;
    QString catalog_root_;

    QLabel* revision_label_{};
    QLabel* source_label_{};
    QLabel* error_label_{};
    QLabel* court_summary_label_{};
    QLabel* procedure_summary_label_{};
    QLabel* record_summary_label_{};
    QLabel* bench_summary_label_{};
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
    QAction* open_oral_argument_action_{};
};

} // namespace appellate::ui
