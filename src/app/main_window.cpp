#include "main_window.hpp"

#include "appellate/engine/workflow_engine.hpp"
#include "appellate/packs/pack_catalog.hpp"
#include "appellate/packs/pack_reader.hpp"
#include "appellate/storage/session_store.hpp"
#include "bench_profile_codec.hpp"
#include "bench_profile_editor.hpp"
#include "installed_record_controller.hpp"
#include "oral_argument_launch_provider.hpp"
#include "oral_argument_workspace.hpp"
#include "record_workspace.hpp"
#include "session_controller.hpp"
#include "workflow_launch_provider.hpp"
#include "workflow_session_controller.hpp"

#include <QAction>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QGroupBox>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringList>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace appellate::ui {
namespace {

constexpr auto record_access_engine_revision = "engine.record-access.v1";
constexpr auto record_access_session_domain = "appellate-workbench-record-access-session-v1";

void addUint64(QCryptographicHash& hash, std::uint64_t value) {
    std::array<char, 8> bytes{};
    for (int index = 7; index >= 0; --index) {
        bytes.at(static_cast<std::size_t>(index)) = static_cast<char>(value & 0xffU);
        value >>= 8U;
    }
    hash.addData(QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())));
}

void addFrame(QCryptographicHash& hash, std::string_view value) {
    addUint64(hash, value.size());
    hash.addData(QByteArrayView(value.data(), static_cast<qsizetype>(value.size())));
}

[[nodiscard]] QString canonicalUtcNow() {
    return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'"));
}

[[nodiscard]] model::LegalTime workflowLegalTime(const WorkflowLegalClockReading& reading) {
    return model::LegalTime{
        std::chrono::sys_seconds{
            std::chrono::seconds{reading.instant_utc.toUTC().toSecsSinceEpoch()}},
        model::LegalDate{std::chrono::year{reading.court_date.year()} /
                         std::chrono::month{static_cast<unsigned>(reading.court_date.month())} /
                         std::chrono::day{static_cast<unsigned>(reading.court_date.day())}},
    };
}

[[nodiscard]] model::AdvanceWorkflowStage
workflowAdvanceCommand(const model::WorkflowState& state, const model::WorkflowOperation& operation,
                       const model::CaseActor& actor,
                       const WorkflowLegalClockReading& occurred_at) {
    return model::AdvanceWorkflowStage{
        model::WorkflowCommandHeader{
            state.session_id,
            model::WorkflowCommandId{state.session_id + ".command." +
                                     std::to_string(state.next_event_sequence) + "." +
                                     operation.id.value},
            actor.id,
            workflowLegalTime(occurred_at),
        },
        operation.id,
    };
}

[[nodiscard]] QString recordAccessSessionId(const model::PackRevision& revision,
                                            const model::CaseId& case_id) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, record_access_session_domain);
    addFrame(hash, revision.id.value);
    addFrame(hash, revision.version);
    addFrame(hash, revision.digest);
    addFrame(hash, case_id.value);
    return QStringLiteral("record.access.session.%1")
        .arg(QString::fromLatin1(hash.result().toHex()));
}

class SystemRecordAccessTransitionProvider final : public RecordAccessTransitionProvider {
  public:
    [[nodiscard]] auto createdAtUtc(QStringView session_id)
        -> std::expected<QString, QString> override {
        if (session_id.isEmpty()) {
            return std::unexpected(QStringLiteral("Record-access session identity is unavailable"));
        }
        return canonicalUtcNow();
    }

    [[nodiscard]] auto next(QStringView session_id, std::uint64_t next_sequence,
                            const packs::RuntimeRecordDisclosureId&, model::RecordAccessAction)
        -> std::expected<RecordAccessTransitionStamp, QString> override {
        if (session_id.isEmpty() || next_sequence == 0) {
            return std::unexpected(
                QStringLiteral("Record-access transition identity is unavailable"));
        }
        return RecordAccessTransitionStamp{
            QStringLiteral("%1.event-%2").arg(session_id).arg(next_sequence),
            canonicalUtcNow(),
        };
    }
};

[[nodiscard]] QString utf8(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

[[nodiscard]] QString courtRoleName(model::CourtRole role) {
    switch (role) {
    case model::CourtRole::District:
        return QStringLiteral("district");
    case model::CourtRole::Appellate:
        return QStringLiteral("appellate");
    }
    return QStringLiteral("unsupported");
}

[[nodiscard]] QString proceedingName(packs::RuntimeProceedingType type) {
    switch (type) {
    case packs::RuntimeProceedingType::CivilAppeal:
        return QStringLiteral("civil appeal");
    case packs::RuntimeProceedingType::CriminalAppeal:
        return QStringLiteral("criminal appeal");
    case packs::RuntimeProceedingType::AgencyReview:
        return QStringLiteral("agency review");
    case packs::RuntimeProceedingType::OriginalWrit:
        return QStringLiteral("original writ");
    }
    return QStringLiteral("unsupported proceeding");
}

[[nodiscard]] QString countText(std::size_t count, QStringView singular, QStringView plural) {
    return QStringLiteral("%1 %2")
        .arg(static_cast<qulonglong>(count))
        .arg(count == 1U ? singular : plural);
}

template <typename Pack>
[[nodiscard]] auto checkedRuntime(const Pack& loaded)
    -> std::expected<packs::RuntimePack, QString> {
    auto runtime = packs::loadRuntimePack(loaded);
    if (!runtime) {
        return std::unexpected(
            QStringLiteral("Runtime pack rejected: %1").arg(utf8(runtime.error().message)));
    }
    for (const auto& runtime_case : runtime->cases) {
        for (const auto& argument : runtime_case.argument_configurations) {
            for (const auto& seat : argument.bench.seats) {
                const auto valid = BenchProfileCodec::validate(seat.profile);
                if (!valid) {
                    return std::unexpected(
                        QStringLiteral("Fictional/composite profile rejected: %1")
                            .arg(valid.error().message));
                }
            }
        }
    }
    return std::move(*runtime);
}

void configureAction(QAction& action, const QString& object_name, const QString& accessible_name,
                     const QString& status_tip) {
    action.setObjectName(object_name);
    action.setProperty("accessibleName", accessible_name);
    action.setToolTip(accessible_name);
    action.setStatusTip(status_tip);
}

void configureSummaryLabel(QLabel& label, const QString& object_name,
                           const QString& accessible_name) {
    label.setObjectName(object_name);
    label.setAccessibleName(accessible_name);
    label.setWordWrap(true);
    label.setTextInteractionFlags(Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
}

} // namespace

MainWindow::MainWindow(const QString& source_path, const QString& catalog_root, QWidget* parent)
    : MainWindow(source_path, catalog_root, parent, {}, {}, {}, {}, {}, {}, {}) {}

MainWindow::MainWindow(
    const QString& source_path, const QString& catalog_root, QWidget* parent,
    std::shared_ptr<OralArgumentLaunchProvider> oral_argument_launch_provider,
    std::shared_ptr<RecordAccessTransitionProvider> record_access_transition_provider,
    QString record_access_database_path,
    std::shared_ptr<WorkflowLaunchProvider> workflow_launch_provider,
    WorkflowLegalClock workflow_legal_clock, OralElapsedClock oral_elapsed_clock,
    OralRecordedAtClock oral_recorded_at_clock)
    : QMainWindow(parent), oral_argument_launch_provider_(std::move(oral_argument_launch_provider)),
      workflow_launch_provider_(std::move(workflow_launch_provider)),
      workflow_legal_clock_(std::move(workflow_legal_clock)),
      oral_elapsed_clock_(std::move(oral_elapsed_clock)),
      oral_recorded_at_clock_(std::move(oral_recorded_at_clock)),
      record_access_transition_provider_(std::move(record_access_transition_provider)) {
    if (!record_access_transition_provider_) {
        record_access_transition_provider_ =
            std::make_shared<SystemRecordAccessTransitionProvider>();
    }
    if (!workflow_legal_clock_) {
        workflow_legal_clock_ = [](const QDate& selected_court_date)
            -> std::expected<WorkflowLegalClockReading, QString> {
            if (!selected_court_date.isValid()) {
                return std::unexpected(QStringLiteral("Select a valid legal court date"));
            }
            return WorkflowLegalClockReading{QDateTime::currentDateTimeUtc(), selected_court_date};
        };
    }
    setWindowTitle(QStringLiteral("Appellate Workbench"));
    resize(1180, 780);
    setMinimumSize(840, 600);
    buildUi();
    buildFileMenu();
    configureRecordAccessDatabase(record_access_database_path);
    openCatalog(catalog_root);
    updateActionStates();

    if (!source_path.isEmpty()) {
        static_cast<void>(loadSource(source_path));
    }
}

MainWindow::~MainWindow() {
    workflow_controller_.reset();
    record_access_controller_.reset();
    record_access_owner_store_.reset();
    clearRecordAccessActions();
    if (workspace_tabs_ != nullptr && record_workspace_ != nullptr) {
        workspace_tabs_->removeTab(workspace_tabs_->indexOf(record_workspace_));
        delete std::exchange(record_workspace_, nullptr);
    }
}

void MainWindow::buildUi() {
    workspace_tabs_ = new QTabWidget(this);
    workspace_tabs_->setObjectName(QStringLiteral("workspaceTabs"));
    workspace_tabs_->setAccessibleName(QStringLiteral("Case browser and verified record tabs"));
    workspace_tabs_->setDocumentMode(true);

    auto* container = new QWidget(workspace_tabs_);
    container->setObjectName(QStringLiteral("packBrowserShell"));
    auto* outer_layout = new QVBoxLayout(container);

    auto* heading = new QLabel(QStringLiteral("Appellate Workbench"), container);
    heading->setObjectName(QStringLiteral("workbenchHeading"));
    heading->setAccessibleName(QStringLiteral("Appellate Workbench heading"));
    QFont heading_font = heading->font();
    heading_font.setPointSize(20);
    heading_font.setBold(true);
    heading->setFont(heading_font);
    outer_layout->addWidget(heading);

    auto* boundary = new QLabel(
        QStringLiteral("Local-first appellate simulation. Authoring directories and installed "
                       "packs remain on this device; no server or account is used."),
        container);
    boundary->setObjectName(QStringLiteral("localOnlyBoundary"));
    boundary->setAccessibleName(QStringLiteral("Local-only application boundary"));
    boundary->setWordWrap(true);
    outer_layout->addWidget(boundary);

    revision_label_ = new QLabel(QStringLiteral("Pack revision: No pack loaded."), container);
    configureSummaryLabel(*revision_label_, QStringLiteral("packRevision"),
                          QStringLiteral("Loaded pack revision"));
    outer_layout->addWidget(revision_label_);

    source_label_ = new QLabel(QStringLiteral("Source: No pack loaded."), container);
    configureSummaryLabel(*source_label_, QStringLiteral("packSource"),
                          QStringLiteral("Loaded pack source"));
    outer_layout->addWidget(source_label_);

    error_label_ = new QLabel(container);
    configureSummaryLabel(*error_label_, QStringLiteral("packLoadError"),
                          QStringLiteral("Pack and profile error"));
    error_label_->setStyleSheet(QStringLiteral("color: #a32121; font-weight: 600;"));
    error_label_->setVisible(false);
    outer_layout->addWidget(error_label_);

    auto* splitter = new QSplitter(Qt::Horizontal, container);
    splitter->setObjectName(QStringLiteral("packBrowserSplitter"));
    splitter->setAccessibleName(QStringLiteral("Pack cases and details"));

    auto* browser = new QWidget(splitter);
    browser->setObjectName(QStringLiteral("caseBrowserPane"));
    auto* browser_layout = new QVBoxLayout(browser);
    auto* case_heading = new QLabel(QStringLiteral("&Cases"), browser);
    case_heading->setObjectName(QStringLiteral("caseListLabel"));
    case_heading->setAccessibleName(QStringLiteral("Cases label"));
    case_list_ = new QListWidget(browser);
    case_list_->setObjectName(QStringLiteral("caseList"));
    case_list_->setAccessibleName(QStringLiteral("Cases in loaded pack"));
    case_list_->setAccessibleDescription(
        QStringLiteral("Select a case to review its court, procedure, record, and bench"));
    case_list_->setSelectionMode(QAbstractItemView::SingleSelection);
    case_list_->setEnabled(false);
    case_heading->setBuddy(case_list_);
    browser_layout->addWidget(case_heading);
    browser_layout->addWidget(case_list_, 1);

    auto* details = new QWidget(splitter);
    details->setObjectName(QStringLiteral("caseDetailsPane"));
    auto* details_layout = new QVBoxLayout(details);

    auto* summary_group = new QGroupBox(QStringLiteral("Selected case"), details);
    summary_group->setObjectName(QStringLiteral("caseSummaryGroup"));
    summary_group->setAccessibleName(QStringLiteral("Selected case summary"));
    auto* summary_layout = new QVBoxLayout(summary_group);
    court_summary_label_ = new QLabel(QStringLiteral("Court: Select a case."), summary_group);
    configureSummaryLabel(*court_summary_label_, QStringLiteral("courtSummary"),
                          QStringLiteral("Court summary"));
    procedure_summary_label_ =
        new QLabel(QStringLiteral("Procedure: Select a case."), summary_group);
    configureSummaryLabel(*procedure_summary_label_, QStringLiteral("procedureSummary"),
                          QStringLiteral("Procedure summary"));
    record_summary_label_ = new QLabel(QStringLiteral("Record: Select a case."), summary_group);
    configureSummaryLabel(*record_summary_label_, QStringLiteral("recordSummary"),
                          QStringLiteral("Record summary"));
    bench_summary_label_ = new QLabel(QStringLiteral("Bench: Select a case."), summary_group);
    configureSummaryLabel(*bench_summary_label_, QStringLiteral("benchSummary"),
                          QStringLiteral("Bench summary"));
    summary_layout->addWidget(court_summary_label_);
    summary_layout->addWidget(procedure_summary_label_);
    summary_layout->addWidget(record_summary_label_);
    summary_layout->addWidget(bench_summary_label_);
    details_layout->addWidget(summary_group);

    auto* workflow_group = new QGroupBox(QStringLiteral("Local workflow session"), details);
    workflow_group->setObjectName(QStringLiteral("localWorkflowSessionGroup"));
    workflow_group->setAccessibleName(QStringLiteral("Local persisted workflow session"));
    auto* workflow_layout = new QVBoxLayout(workflow_group);
    workflow_status_label_ = new QLabel(workflow_group);
    configureSummaryLabel(*workflow_status_label_, QStringLiteral("workflowSessionStatus"),
                          QStringLiteral("Local workflow session status"));
    workflow_status_label_->setText(
        workflow_launch_provider_
            ? QStringLiteral("Workflow: Select an installed case, then open or resume its exact "
                             "local session.")
            : QStringLiteral("Workflow unavailable: no production local session provider is "
                             "configured."));
    workflow_layout->addWidget(workflow_status_label_);
    auto* court_date_label =
        new QLabel(QStringLiteral("Legal &court date (YYYY-MM-DD)"), workflow_group);
    court_date_label->setObjectName(QStringLiteral("workflowCourtDateLabel"));
    court_date_label->setAccessibleName(QStringLiteral("Workflow legal court date label"));
    workflow_court_date_editor_ = new QLineEdit(workflow_group);
    workflow_court_date_editor_->setObjectName(QStringLiteral("workflowCourtDateEditor"));
    workflow_court_date_editor_->setAccessibleName(QStringLiteral("Workflow legal court date"));
    workflow_court_date_editor_->setAccessibleDescription(QStringLiteral(
        "Required explicit court-local legal date in four-digit year, month, and day format"));
    workflow_court_date_editor_->setPlaceholderText(QStringLiteral("YYYY-MM-DD"));
    workflow_court_date_editor_->setMaxLength(10);
    court_date_label->setBuddy(workflow_court_date_editor_);
    workflow_layout->addWidget(court_date_label);
    workflow_layout->addWidget(workflow_court_date_editor_);
    open_workflow_button_ =
        new QPushButton(QStringLiteral("&Open or resume workflow"), workflow_group);
    open_workflow_button_->setObjectName(QStringLiteral("openSelectedWorkflowButton"));
    open_workflow_button_->setAccessibleName(QStringLiteral("Open or resume selected workflow"));
    open_workflow_button_->setAccessibleDescription(
        QStringLiteral("Open the exact installed workflow using crash-safe local session storage"));
    workflow_layout->addWidget(open_workflow_button_);
    advance_workflow_button_ =
        new QPushButton(QStringLiteral("Persist next authored stage &transition"), workflow_group);
    advance_workflow_button_->setObjectName(QStringLiteral("advanceSelectedWorkflowButton"));
    advance_workflow_button_->setAccessibleName(
        QStringLiteral("Persist next authored workflow stage transition"));
    advance_workflow_button_->setAccessibleDescription(QStringLiteral(
        "Submit the exact displayed operation and actor as one persisted workflow command"));
    workflow_layout->addWidget(advance_workflow_button_);
    details_layout->addWidget(workflow_group);

    auto* profile_label =
        new QLabel(QStringLiteral("Selected &fictional/composite profile"), details);
    profile_label->setObjectName(QStringLiteral("profileSelectorLabel"));
    profile_label->setAccessibleName(QStringLiteral("Fictional/composite profile selector label"));
    auto* argument_label = new QLabel(QStringLiteral("Oral-argument &configuration"), details);
    argument_label->setObjectName(QStringLiteral("argumentConfigurationSelectorLabel"));
    argument_label->setAccessibleName(QStringLiteral("Oral argument configuration selector label"));
    argument_configuration_selector_ = new QComboBox(details);
    argument_configuration_selector_->setObjectName(
        QStringLiteral("argumentConfigurationSelector"));
    argument_configuration_selector_->setAccessibleName(
        QStringLiteral("Exact oral argument configuration"));
    argument_configuration_selector_->setAccessibleDescription(QStringLiteral(
        "Choose an exact installed-pack actual-record or counterfactual configuration"));
    argument_configuration_selector_->setEnabled(false);
    argument_label->setBuddy(argument_configuration_selector_);
    details_layout->addWidget(argument_label);
    details_layout->addWidget(argument_configuration_selector_);

    argument_launch_boundary_label_ = new QLabel(details);
    configureSummaryLabel(*argument_launch_boundary_label_,
                          QStringLiteral("oralArgumentLaunchBoundary"),
                          QStringLiteral("Oral argument launch boundary"));
    argument_launch_boundary_label_->setText(
        oral_argument_launch_provider_
            ? QStringLiteral("A workflow-authoritative local session provider is available. "
                             "Launch uses only the exact installed closure and selected IDs.")
            : QStringLiteral("Oral argument launch is disabled until the application supplies a "
                             "workflow-authoritative local session provider; this UI never "
                             "invents legal-state or disposition pins."));
    details_layout->addWidget(argument_launch_boundary_label_);

    profile_selector_ = new QComboBox(details);
    profile_selector_->setObjectName(QStringLiteral("profileSelector"));
    profile_selector_->setAccessibleName(QStringLiteral("Selected fictional/composite profile"));
    profile_selector_->setAccessibleDescription(
        QStringLiteral("Choose a fictional/composite profile from the selected case bench"));
    profile_selector_->setEnabled(false);
    profile_label->setBuddy(profile_selector_);
    details_layout->addWidget(profile_label);
    details_layout->addWidget(profile_selector_);

    profile_editor_ = new BenchProfileEditor(details);
    profile_editor_->setEnabled(false);
    details_layout->addWidget(profile_editor_, 1);

    splitter->addWidget(browser);
    splitter->addWidget(details);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    outer_layout->addWidget(splitter, 1);

    browser_tab_index_ = workspace_tabs_->addTab(container, QStringLiteral("&Cases and profiles"));
    workspace_tabs_->setTabToolTip(
        browser_tab_index_, QStringLiteral("Browse the loaded pack, cases, and bench profiles"));

    record_workspace_ = new RecordWorkspace(workspace_tabs_);
    record_workspace_->setObjectName(QStringLiteral("installedRecordWorkspace"));
    record_workspace_->setAccessibleName(QStringLiteral("Verified installed case record"));
    record_tab_index_ = workspace_tabs_->addTab(record_workspace_, QStringLiteral("&Record"));
    workspace_tabs_->setTabToolTip(
        record_tab_index_,
        QStringLiteral("Review PDFs materialized from the selected installed pack"));
    workspace_tabs_->setTabEnabled(record_tab_index_, false);

    oral_argument_workspace_ = new OralArgumentWorkspace(workspace_tabs_);
    argument_tab_index_ =
        workspace_tabs_->addTab(oral_argument_workspace_, QStringLiteral("Oral &Argument"));
    workspace_tabs_->setTabToolTip(
        argument_tab_index_,
        QStringLiteral("Practice exact pack-authored questions against a fictional bench"));
    workspace_tabs_->setTabEnabled(argument_tab_index_, false);
    setCentralWidget(workspace_tabs_);

    connect(case_list_, &QListWidget::currentRowChanged, this,
            [this](int row) { updateCaseSelection(row); });
    connect(profile_selector_, &QComboBox::currentIndexChanged, this,
            [this](int row) { updateProfileSelection(row); });
    connect(argument_configuration_selector_, &QComboBox::currentIndexChanged, this,
            [this](int row) { updateArgumentSelection(row); });
    connect(open_workflow_button_, &QPushButton::clicked, this,
            [this] { static_cast<void>(openSelectedWorkflow()); });
    connect(advance_workflow_button_, &QPushButton::clicked, this,
            [this] { static_cast<void>(advanceSelectedWorkflow()); });
    connect(workflow_court_date_editor_, &QLineEdit::textChanged, this, [this] {
        if (selectedCaseHasLoadedWorkflow()) {
            renderWorkflowStatus();
        } else {
            updateActionStates();
        }
    });
}

void MainWindow::buildFileMenu() {
    auto* file_menu = menuBar()->addMenu(QStringLiteral("&File"));
    file_menu->setObjectName(QStringLiteral("fileMenu"));
    file_menu->setAccessibleName(QStringLiteral("File menu"));

    open_directory_action_ =
        file_menu->addAction(QStringLiteral("&Open Authoring Pack Directory\u2026"));
    configureAction(*open_directory_action_, QStringLiteral("openPackDirectoryAction"),
                    QStringLiteral("Open authoring pack directory"),
                    QStringLiteral("Open and strictly validate an authoring pack directory"));
    open_directory_action_->setShortcut(QKeySequence::Open);

    install_archive_action_ = file_menu->addAction(QStringLiteral("&Install Pack Archive\u2026"));
    configureAction(*install_archive_action_, QStringLiteral("installPackArchiveAction"),
                    QStringLiteral("Install pack archive"),
                    QStringLiteral("Install and load a local .awpack archive"));
    install_archive_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+O")));

    open_record_action_ = file_menu->addAction(QStringLiteral("Open Selected &Record"));
    configureAction(
        *open_record_action_, QStringLiteral("openSelectedRecordAction"),
        QStringLiteral("Open selected installed case record"),
        QStringLiteral("Verify public record PDFs and apply persisted local disclosure access"));
    open_record_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));

    open_workflow_action_ =
        file_menu->addAction(QStringLiteral("Open or Resume Selected &Workflow"));
    configureAction(
        *open_workflow_action_, QStringLiteral("openSelectedWorkflowAction"),
        QStringLiteral("Open or resume selected workflow"),
        QStringLiteral("Open the exact persisted local workflow for the selected case"));
    open_workflow_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+W")));

    advance_workflow_action_ =
        file_menu->addAction(QStringLiteral("Persist Next Workflow Stage &Transition"));
    configureAction(*advance_workflow_action_, QStringLiteral("advanceSelectedWorkflowAction"),
                    QStringLiteral("Persist next authored workflow stage transition"),
                    QStringLiteral("Submit the exact displayed operation and actor"));
    advance_workflow_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+Right")));

    open_oral_argument_action_ =
        file_menu->addAction(QStringLiteral("Open Selected Oral &Argument"));
    configureAction(
        *open_oral_argument_action_, QStringLiteral("openSelectedOralArgumentAction"),
        QStringLiteral("Open selected oral argument configuration"),
        QStringLiteral("Open the exact grounded configuration through the authoritative local "
                       "session provider"));
    open_oral_argument_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+A")));
    file_menu->addSeparator();

    import_profile_action_ =
        file_menu->addAction(QStringLiteral("&Import Fictional/composite Profile\u2026"));
    configureAction(*import_profile_action_, QStringLiteral("importProfileAction"),
                    QStringLiteral("Import fictional/composite profile"),
                    QStringLiteral("Import a strict schema-v1 fictional/composite profile"));
    import_profile_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+I")));

    clone_profile_action_ =
        file_menu->addAction(QStringLiteral("&Clone Fictional/composite Profile\u2026"));
    configureAction(*clone_profile_action_, QStringLiteral("cloneProfileAction"),
                    QStringLiteral("Clone fictional/composite profile"),
                    QStringLiteral("Clone the edited fictional/composite profile under a new ID"));
    clone_profile_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+C")));

    export_profile_action_ =
        file_menu->addAction(QStringLiteral("&Export Fictional/composite Profile\u2026"));
    configureAction(*export_profile_action_, QStringLiteral("exportProfileAction"),
                    QStringLiteral("Export fictional/composite profile"),
                    QStringLiteral("Export without overwriting an existing path"));
    export_profile_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")));

    connect(open_directory_action_, &QAction::triggered, this, [this] {
        const auto path = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Open Authoring Pack Directory"));
        if (path.isEmpty()) {
            return;
        }
        const auto loaded = loadSource(path);
        if (!loaded) {
            QMessageBox::critical(this, QStringLiteral("Pack Rejected"), loaded.error());
        }
    });
    connect(install_archive_action_, &QAction::triggered, this, [this] {
        const auto path =
            QFileDialog::getOpenFileName(this, QStringLiteral("Install Pack Archive"), {},
                                         QStringLiteral("Appellate Workbench packs (*.awpack)"));
        if (path.isEmpty()) {
            return;
        }
        const auto loaded = loadSource(path);
        if (!loaded) {
            QMessageBox::critical(this, QStringLiteral("Pack Rejected"), loaded.error());
        }
    });
    connect(open_record_action_, &QAction::triggered, this,
            [this] { static_cast<void>(openSelectedRecord()); });
    connect(open_workflow_action_, &QAction::triggered, this,
            [this] { static_cast<void>(openSelectedWorkflow()); });
    connect(advance_workflow_action_, &QAction::triggered, this,
            [this] { static_cast<void>(advanceSelectedWorkflow()); });
    connect(open_oral_argument_action_, &QAction::triggered, this,
            [this] { static_cast<void>(openSelectedOralArgument()); });
    connect(import_profile_action_, &QAction::triggered, this, [this] {
        const auto path =
            QFileDialog::getOpenFileName(this, QStringLiteral("Import Fictional/composite Profile"),
                                         {}, QStringLiteral("JSON profiles (*.json)"));
        if (path.isEmpty()) {
            return;
        }
        const auto imported = importProfile(path);
        if (!imported) {
            QMessageBox::critical(this, QStringLiteral("Profile Rejected"), imported.error());
        }
    });
    connect(clone_profile_action_, &QAction::triggered, this, [this] {
        bool accepted = false;
        const auto id = QInputDialog::getText(
            this, QStringLiteral("Clone Fictional/composite Profile"),
            QStringLiteral("New namespaced profile ID:"), QLineEdit::Normal, {}, &accepted);
        if (!accepted) {
            return;
        }
        const auto display_name = QInputDialog::getText(
            this, QStringLiteral("Clone Fictional/composite Profile"),
            QStringLiteral("New display name:"), QLineEdit::Normal, {}, &accepted);
        if (!accepted) {
            return;
        }
        const auto cloned = cloneProfile(id, display_name);
        if (!cloned) {
            QMessageBox::critical(this, QStringLiteral("Profile Rejected"), cloned.error());
        }
    });
    connect(export_profile_action_, &QAction::triggered, this, [this] {
        const auto edited = profile_editor_->profile();
        if (!edited) {
            showError(edited.error().message);
            return;
        }
        const auto suggested = utf8(edited->id) + QStringLiteral(".json");
        const auto path =
            QFileDialog::getSaveFileName(this, QStringLiteral("Export Fictional/composite Profile"),
                                         suggested, QStringLiteral("JSON profiles (*.json)"));
        if (path.isEmpty()) {
            return;
        }
        const auto exported = exportProfile(path);
        if (!exported) {
            QMessageBox::critical(this, QStringLiteral("Profile Export Failed"), exported.error());
        }
    });

    record_access_menu_ = menuBar()->addMenu(QStringLiteral("Record &Access"));
    record_access_menu_->setObjectName(QStringLiteral("recordAccessMenu"));
    record_access_menu_->setAccessibleName(QStringLiteral("Local record disclosure access"));
    record_access_menu_->setToolTipsVisible(true);
    record_access_menu_->setEnabled(false);
}

void MainWindow::configureRecordAccessDatabase(const QString& injected_path) {
    if (injected_path.contains(QChar::Null)) {
        record_access_database_path_.clear();
        showError(QStringLiteral("Record-access session database path contains a null character"));
        return;
    }
    if (!injected_path.isEmpty()) {
        record_access_database_path_ = QFileInfo(injected_path).absoluteFilePath();
        return;
    }

    const auto application_data =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (application_data.isEmpty()) {
        record_access_database_path_.clear();
        showError(QStringLiteral("Record-access sessions unavailable: app-local data has no path"));
        return;
    }
    record_access_database_path_ =
        QDir(application_data).filePath(QStringLiteral("sessions/record-access.sqlite"));
}

void MainWindow::openCatalog(const QString& injected_root) {
    QString root = injected_root;
    if (root.isEmpty()) {
        const auto application_data =
            QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (application_data.isEmpty()) {
            showError(QStringLiteral("Pack catalog unavailable: app-local data has no path"));
            return;
        }
        root = QDir(application_data).filePath(QStringLiteral("packs"));
    }
    catalog_root_ = QDir(root).absolutePath();
    auto opened = packs::PackCatalog::open(catalog_root_);
    if (!opened) {
        showError(QStringLiteral("Pack catalog unavailable: %1").arg(opened.error().message));
        return;
    }
    catalog_ = std::move(*opened);
}

auto MainWindow::openRecordAccessSession(const packs::ResolvedPack& resolved_pack,
                                         const model::CaseId& selected_case_id)
    -> std::expected<std::unique_ptr<app::RecordAccessSessionController>, QString> {
    if (record_access_database_path_.isEmpty()) {
        return std::unexpected(
            QStringLiteral("Local record-access session storage is unavailable"));
    }
    const QFileInfo database_info(record_access_database_path_);
    if (!QDir{}.mkpath(database_info.absolutePath())) {
        return std::unexpected(
            QStringLiteral("Local record-access session directory cannot be created"));
    }

    if (record_access_owner_store_ == nullptr) {
        auto owner_store = storage::SessionStore::open(record_access_database_path_);
        if (!owner_store) {
            return std::unexpected(
                QStringLiteral("Local record-access session database cannot be opened: %1")
                    .arg(owner_store.error().message));
        }
        record_access_owner_store_ = std::move(*owner_store);
    }
    auto store = record_access_owner_store_->forkConnection();
    if (!store) {
        return std::unexpected(
            QStringLiteral("Local record-access session connection cannot be opened: %1")
                .arg(store.error().message));
    }
    const auto session_id = recordAccessSessionId(resolved_pack.root().revision, selected_case_id);
    const auto existing = (*store)->loadSession(session_id);
    if (existing) {
        auto reopened = app::RecordAccessSessionController::reopen(
            session_id, selected_case_id, std::move(*store),
            QString::fromLatin1(record_access_engine_revision), resolved_pack);
        if (!reopened) {
            return std::unexpected(
                QStringLiteral("Persisted record-access session failed exact replay: %1")
                    .arg(reopened.error().message));
        }
        return std::move(*reopened);
    }
    if (existing.error().code != storage::StoreErrorCode::NotFound) {
        return std::unexpected(
            QStringLiteral("Persisted record-access session cannot be inspected: %1")
                .arg(existing.error().message));
    }

    const auto created_at = record_access_transition_provider_->createdAtUtc(session_id);
    if (!created_at) {
        return std::unexpected(QStringLiteral("Local record-access session time is unavailable: %1")
                                   .arg(created_at.error()));
    }
    auto created = app::RecordAccessSessionController::create(
        session_id, selected_case_id, std::move(*store),
        QString::fromLatin1(record_access_engine_revision), *created_at, resolved_pack);
    if (!created) {
        return std::unexpected(QStringLiteral("Local record-access session cannot be created: %1")
                                   .arg(created.error().message));
    }
    return std::move(*created);
}

auto MainWindow::loadSource(const QString& source_path) -> std::expected<void, QString> {
    const auto reject = [this](QString message) -> std::expected<void, QString> {
        showError(message);
        return std::unexpected(std::move(message));
    };
    if (source_path.isEmpty() || source_path.contains(QChar::Null)) {
        return reject(QStringLiteral("Pack source path is empty or contains a null character"));
    }

    const QFileInfo source_info(source_path);
    const auto absolute_path = source_info.absoluteFilePath();
    if (source_info.isDir()) {
        const auto loaded = packs::PackReader::readDirectory(absolute_path);
        if (!loaded) {
            return reject(
                QStringLiteral("Authoring pack rejected: %1").arg(loaded.error().message));
        }
        auto runtime = checkedRuntime(*loaded);
        if (!runtime) {
            return reject(runtime.error());
        }
        commitRuntime(std::move(*runtime), absolute_path,
                      QStringLiteral("Loaded authoring pack directory."));
        return {};
    }

    if (!source_info.isFile() ||
        source_info.suffix().compare(QStringLiteral("awpack"), Qt::CaseInsensitive) != 0) {
        return reject(QStringLiteral(
            "Pack source must be an authoring-pack directory or a regular .awpack archive"));
    }
    if (catalog_ == nullptr) {
        return reject(QStringLiteral("Pack archive cannot be installed: catalog unavailable"));
    }

    const auto installed = catalog_->installArchive(
        absolute_path, QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    if (!installed) {
        return reject(
            QStringLiteral("Pack archive installation failed: %1").arg(installed.error().message));
    }
    const auto resolved = catalog_->loadResolved(installed->revision);
    if (!resolved) {
        return reject(QStringLiteral("Installed pack closure could not be loaded: %1")
                          .arg(resolved.error().message));
    }
    auto runtime = checkedRuntime(*resolved);
    if (!runtime) {
        return reject(runtime.error());
    }
    commitRuntime(std::move(*runtime), absolute_path,
                  QStringLiteral("Installed and loaded exact pack closure."), std::move(*resolved));
    return {};
}

auto MainWindow::importProfile(const QString& path) -> std::expected<void, QString> {
    const auto imported = profile_editor_->importProfile(path);
    if (!imported) {
        showError(imported.error().message);
        return std::unexpected(imported.error().message);
    }
    {
        const QSignalBlocker blocker(profile_selector_);
        profile_selector_->setCurrentIndex(-1);
    }
    profile_editor_->setEnabled(true);
    updateActionStates();
    showStatus(QStringLiteral("Imported fictional/composite profile."));
    return {};
}

auto MainWindow::cloneProfile(const QString& namespaced_id, const QString& display_name)
    -> std::expected<void, QString> {
    const auto id_bytes = namespaced_id.toUtf8();
    const auto name_bytes = display_name.toUtf8();
    const auto cloned = profile_editor_->cloneProfile(
        std::string_view(id_bytes.constData(), static_cast<std::size_t>(id_bytes.size())),
        std::string_view(name_bytes.constData(), static_cast<std::size_t>(name_bytes.size())));
    if (!cloned) {
        showError(cloned.error().message);
        return std::unexpected(cloned.error().message);
    }
    {
        const QSignalBlocker blocker(profile_selector_);
        profile_selector_->setCurrentIndex(-1);
    }
    updateActionStates();
    showStatus(QStringLiteral("Cloned fictional/composite profile."));
    return {};
}

auto MainWindow::exportProfile(const QString& path) -> std::expected<void, QString> {
    const auto exported = profile_editor_->exportProfile(path);
    if (!exported) {
        showError(exported.error().message);
        return std::unexpected(exported.error().message);
    }
    showStatus(QStringLiteral("Exported fictional/composite profile without overwriting."));
    return {};
}

auto MainWindow::openSelectedRecord() -> std::expected<void, QString> {
    const auto reject = [this](QString message) -> std::expected<void, QString> {
        showError(message);
        return std::unexpected(std::move(message));
    };
    if (catalog_ == nullptr || !installed_pack_ || !runtime_pack_) {
        return reject(QStringLiteral(
            "A case record can be opened only from a pack installed in this device's catalog"));
    }
    const auto selected_row = case_list_->currentRow();
    if (selected_row < 0 || static_cast<std::size_t>(selected_row) >= runtime_pack_->cases.size()) {
        return reject(QStringLiteral("Select an installed-pack case before opening its record"));
    }

    const auto& selected_case = runtime_pack_->cases.at(static_cast<std::size_t>(selected_row));
    auto candidate = std::make_unique<RecordWorkspace>();
    candidate->setObjectName(QStringLiteral("installedRecordWorkspace"));
    candidate->setAccessibleName(QStringLiteral("Verified installed case record"));
    app::InstalledRecordController controller(*catalog_, *candidate);
    const auto loaded =
        controller.load(*installed_pack_, *runtime_pack_, selected_case.definition.id);
    if (!loaded) {
        return reject(
            QStringLiteral("Installed record could not be opened: %1").arg(loaded.error().message));
    }

    std::unique_ptr<app::RecordAccessSessionController> candidate_access_controller;
    if (selected_case.record.disclosure_policy.has_value()) {
        auto opened_access = openRecordAccessSession(*installed_pack_, selected_case.definition.id);
        if (!opened_access) {
            return reject(opened_access.error());
        }
        const auto applied = (*opened_access)->applyCurrentProjection(*candidate);
        if (!applied) {
            return reject(
                QStringLiteral("Persisted record-access projection could not be applied: %1")
                    .arg(applied.error().message));
        }
        candidate_access_controller = std::move(*opened_access);
    }

    const auto first_public_entry =
        std::ranges::find_if(loaded->definition.docket, [&loaded](const RecordDocketEntry& entry) {
            const auto document = std::ranges::find(loaded->definition.documents, entry.document_id,
                                                    &RecordDocument::id);
            return document != loaded->definition.documents.end() && !document->sealed;
        });
    if (first_public_entry == loaded->definition.docket.end()) {
        return reject(QStringLiteral("Installed record contains no public docket entry to open"));
    }
    const auto first_document = candidate->openDocketEntry(first_public_entry->id);
    if (!first_document) {
        return reject(QStringLiteral("Installed record PDF could not be displayed: %1")
                          .arg(first_document.error().message));
    }

    auto* previous_workspace = record_workspace_;
    workspace_tabs_->removeTab(record_tab_index_);
    record_workspace_ = candidate.release();
    record_workspace_->setParent(workspace_tabs_);
    record_tab_index_ = workspace_tabs_->insertTab(
        record_tab_index_, record_workspace_,
        QStringLiteral("&Record — %1").arg(utf8(selected_case.record.caption)));
    workspace_tabs_->setTabToolTip(
        record_tab_index_,
        QStringLiteral("Verified record for %1").arg(utf8(selected_case.record.caption)));
    record_access_controller_ = std::move(candidate_access_controller);
    rebuildRecordAccessActions();
    delete previous_workspace;

    record_revision_ = runtime_pack_->revision;
    record_case_id_ = selected_case.definition.id;
    workspace_tabs_->setTabEnabled(record_tab_index_, true);
    workspace_tabs_->setCurrentIndex(record_tab_index_);
    showStatus(QStringLiteral("Opened the verified public record from installed storage."));
    updateActionStates();
    return {};
}

auto MainWindow::openSelectedWorkflow() -> std::expected<void, QString> {
    const auto reject = [this](QString message) -> std::expected<void, QString> {
        showError(message);
        return std::unexpected(std::move(message));
    };
    if (!workflow_launch_provider_) {
        return reject(QStringLiteral("No production local workflow session provider is available"));
    }
    if (!installed_pack_ || !runtime_pack_) {
        return reject(QStringLiteral(
            "A workflow can open only from an exact pack closure installed on this device"));
    }
    const auto selected_row = case_list_->currentRow();
    if (selected_row < 0 || static_cast<std::size_t>(selected_row) >= runtime_pack_->cases.size()) {
        return reject(QStringLiteral("Select an installed-pack case before opening its workflow"));
    }
    const auto& runtime_case = runtime_pack_->cases.at(static_cast<std::size_t>(selected_row));
    auto opened =
        workflow_launch_provider_->openWorkflow(*installed_pack_, runtime_case.definition.id);
    if (!opened) {
        return reject(
            QStringLiteral("Local workflow could not be opened: %1").arg(opened.error().message));
    }

    workflow_controller_ = std::move(*opened);
    workflow_revision_ = runtime_pack_->revision;
    workflow_case_id_ = runtime_case.definition.id;
    renderWorkflowStatus();
    showStatus(QStringLiteral("Opened exact local workflow session %1.")
                   .arg(QString::fromStdString(workflow_controller_->state().session_id)));
    return {};
}

auto MainWindow::advanceSelectedWorkflow() -> std::expected<void, QString> {
    const auto reject = [this](QString message) -> std::expected<void, QString> {
        showError(message);
        return std::unexpected(std::move(message));
    };
    if (!workflow_controller_ || !selectedCaseHasLoadedWorkflow()) {
        return reject(QStringLiteral(
            "Open the selected installed case's local workflow before submitting a transition"));
    }
    const auto occurred_at = sampleWorkflowLegalClock();
    if (!occurred_at) {
        renderWorkflowStatus(occurred_at);
        return reject(
            QStringLiteral("Workflow legal time is unavailable: %1").arg(occurred_at.error()));
    }
    const auto choice = currentWorkflowAdvanceChoice(*occurred_at);
    if (choice.operation == nullptr || choice.actor == nullptr || !choice.command.has_value()) {
        renderWorkflowStatus(occurred_at);
        return reject(QStringLiteral(
            "No currently eligible authored stage transition and authorized actor are available "
            "for the selected legal court date"));
    }
    const auto& command = *choice.command;
    const auto submitted = workflow_controller_->submit(
        model::WorkflowCommand{command}, std::nullopt,
        occurred_at->instant_utc.toUTC().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'")));
    if (!submitted) {
        renderWorkflowStatus(occurred_at);
        return reject(QStringLiteral("Workflow transition was not persisted: %1")
                          .arg(submitted.error().message));
    }
    renderWorkflowStatus(occurred_at);
    showStatus(QStringLiteral("Persisted workflow command %1 through event sequence %2.")
                   .arg(QString::fromStdString(command.header.command_id.value))
                   .arg(submitted->persisted_sequence));
    return {};
}

auto MainWindow::openSelectedOralArgument() -> std::expected<void, QString> {
    const auto reject = [this](QString message) -> std::expected<void, QString> {
        showError(message);
        return std::unexpected(std::move(message));
    };
    if (!oral_argument_launch_provider_) {
        return reject(QStringLiteral(
            "No workflow-authoritative local oral-argument session provider is available"));
    }
    if (!installed_pack_ || !runtime_pack_) {
        return reject(QStringLiteral(
            "Oral argument can launch only from an exact pack closure installed on this device"));
    }
    const auto case_row = case_list_->currentRow();
    const auto argument_row = argument_configuration_selector_->currentIndex();
    if (case_row < 0 || argument_row < 0 ||
        static_cast<std::size_t>(case_row) >= runtime_pack_->cases.size()) {
        return reject(QStringLiteral(
            "Select an installed-pack case and oral-argument configuration before launching"));
    }
    const auto& selected_case = runtime_pack_->cases.at(static_cast<std::size_t>(case_row));
    if (static_cast<std::size_t>(argument_row) >= selected_case.argument_configurations.size()) {
        return reject(QStringLiteral("Selected oral-argument configuration is outside the case"));
    }
    const auto& selected_argument =
        selected_case.argument_configurations.at(static_cast<std::size_t>(argument_row));
    if (!selected_argument.grounded_question_bank.has_value()) {
        return reject(QStringLiteral(
            "Selected oral-argument configuration has no grounded authored question bank"));
    }

    auto opened = oral_argument_launch_provider_->open(
        *installed_pack_, selected_case.definition.id, selected_argument.id);
    if (!opened) {
        return reject(
            QStringLiteral("Oral argument could not be opened: %1").arg(opened.error().message));
    }
    auto candidate = std::make_unique<OralArgumentWorkspace>(
        std::move(*opened), oral_elapsed_clock_, oral_recorded_at_clock_);
    if (!candidate->isReady()) {
        return reject(
            QStringLiteral("Oral argument could not be opened: %1").arg(candidate->lastError()));
    }

    auto* previous_workspace = oral_argument_workspace_;
    workspace_tabs_->removeTab(argument_tab_index_);
    oral_argument_workspace_ = candidate.release();
    oral_argument_workspace_->setParent(workspace_tabs_);
    argument_tab_index_ = workspace_tabs_->insertTab(
        argument_tab_index_, oral_argument_workspace_,
        QStringLiteral("Oral &Argument — %1").arg(utf8(selected_argument.id.value)));
    workspace_tabs_->setTabToolTip(argument_tab_index_,
                                   QStringLiteral("Exact grounded oral argument configuration %1")
                                       .arg(utf8(selected_argument.id.value)));
    delete previous_workspace;

    argument_revision_ = runtime_pack_->revision;
    argument_case_id_ = selected_case.definition.id;
    argument_configuration_id_ = selected_argument.id;
    workspace_tabs_->setTabEnabled(argument_tab_index_, true);
    workspace_tabs_->setCurrentIndex(argument_tab_index_);
    showStatus(QStringLiteral("Opened exact %1 oral-argument configuration %2.")
                   .arg(selected_argument.grounded_question_bank->mode ==
                                model::OralArgumentMode::ActualRecord
                            ? QStringLiteral("actual-record")
                            : QStringLiteral("counterfactual-training"),
                        utf8(selected_argument.id.value)));
    updateActionStates();
    return {};
}

void MainWindow::commitRuntime(packs::RuntimePack runtime, const QString& source_path,
                               const QString& success_message,
                               std::optional<packs::ResolvedPack> installed_pack) {
    invalidateRecordSelection();
    invalidateWorkflowSelection();
    invalidateArgumentSelection();
    runtime_pack_ = std::move(runtime);
    installed_pack_ = std::move(installed_pack);
    current_source_path_ = source_path;

    const auto& revision = runtime_pack_->revision;
    revision_label_->setText(
        QStringLiteral("Pack revision: %1 @ %2 — SHA-256 %3")
            .arg(utf8(revision.id.value), utf8(revision.version), utf8(revision.digest)));
    source_label_->setText(QStringLiteral("Source: %1").arg(current_source_path_));

    {
        const QSignalBlocker blocker(case_list_);
        case_list_->clear();
        for (const auto& runtime_case : runtime_pack_->cases) {
            auto* item = new QListWidgetItem(utf8(runtime_case.title), case_list_);
            item->setData(Qt::UserRole, utf8(runtime_case.definition.id.value));
            item->setToolTip(QStringLiteral("%1 — %2").arg(utf8(runtime_case.definition.id.value),
                                                           utf8(runtime_case.court.name)));
        }
        case_list_->setEnabled(!runtime_pack_->cases.empty());
        case_list_->setCurrentRow(runtime_pack_->cases.empty() ? -1 : 0);
    }
    updateCaseSelection(case_list_->currentRow());
    showStatus(success_message);
    updateActionStates();
}

void MainWindow::resetRecordWorkspace() {
    if (workspace_tabs_ == nullptr || record_workspace_ == nullptr) {
        return;
    }

    auto* candidate = new RecordWorkspace(workspace_tabs_);
    candidate->setObjectName(QStringLiteral("installedRecordWorkspace"));
    candidate->setAccessibleName(QStringLiteral("Verified installed case record"));

    auto* previous_workspace = record_workspace_;
    workspace_tabs_->removeTab(record_tab_index_);
    record_workspace_ = candidate;
    record_tab_index_ =
        workspace_tabs_->insertTab(record_tab_index_, record_workspace_, QStringLiteral("&Record"));
    workspace_tabs_->setTabToolTip(
        record_tab_index_,
        QStringLiteral("Review PDFs materialized from the selected installed pack"));
    workspace_tabs_->setTabEnabled(record_tab_index_, false);
    delete previous_workspace;
}

void MainWindow::clearRecordAccessActions() {
    record_access_action_bindings_.clear();
    if (record_access_menu_ != nullptr) {
        record_access_menu_->clear();
        record_access_menu_->setEnabled(false);
    }
}

void MainWindow::rebuildRecordAccessActions() {
    clearRecordAccessActions();
    if (record_access_menu_ == nullptr || record_access_controller_ == nullptr) {
        return;
    }

    const auto disclosures = record_access_controller_->disclosures();
    record_access_action_bindings_.reserve(disclosures.size());
    for (const auto& disclosure : disclosures) {
        const auto disclosure_id = utf8(disclosure.disclosure_id);
        auto menu_label = disclosure_id;
        menu_label.replace(u'&', QStringLiteral("&&"));
        auto* disclosure_menu =
            record_access_menu_->addMenu(QStringLiteral("&Disclosure %1").arg(menu_label));
        disclosure_menu->setObjectName(
            QStringLiteral("recordAccessDisclosureMenu.%1").arg(disclosure_id));
        disclosure_menu->setAccessibleName(
            QStringLiteral("Public record disclosure %1").arg(disclosure_id));
        disclosure_menu->setToolTipsVisible(true);

        auto* grant_action = disclosure_menu->addAction(
            QStringLiteral("&Grant local session access — %1").arg(menu_label));
        configureAction(
            *grant_action, QStringLiteral("grantRecordAccessAction.%1").arg(disclosure_id),
            QStringLiteral("Grant local session access for public disclosure %1")
                .arg(disclosure_id),
            disclosure.blocking_deficiencies.empty()
                ? QStringLiteral("Grant local access for public disclosure %1").arg(disclosure_id)
                : QStringLiteral("Grant unavailable because required public disclosure "
                                 "material is incomplete"));

        auto* revoke_action = disclosure_menu->addAction(
            QStringLiteral("&Revoke local session access — %1").arg(menu_label));
        configureAction(*revoke_action,
                        QStringLiteral("revokeRecordAccessAction.%1").arg(disclosure_id),
                        QStringLiteral("Revoke local session access for public disclosure %1")
                            .arg(disclosure_id),
                        QStringLiteral("Immediately revoke local access for public disclosure %1")
                            .arg(disclosure_id));

        const auto public_id = disclosure.disclosure_id;
        connect(grant_action, &QAction::triggered, this, [this, public_id] {
            transitionRecordAccess(public_id, model::RecordAccessAction::Grant);
        });
        connect(revoke_action, &QAction::triggered, this, [this, public_id] {
            transitionRecordAccess(public_id, model::RecordAccessAction::Revoke);
        });
        record_access_action_bindings_.push_back(
            RecordAccessActionBinding{public_id, disclosure_menu, grant_action, revoke_action});
    }
    updateActionStates();
}

void MainWindow::transitionRecordAccess(const std::string& disclosure_id,
                                        model::RecordAccessAction action) {
    if (record_access_controller_ == nullptr || record_access_transition_provider_ == nullptr ||
        record_workspace_ == nullptr || !selectedCaseHasLoadedRecord()) {
        showError(QStringLiteral("Open the matching installed record before changing access"));
        updateActionStates();
        return;
    }

    const auto reconciled_head =
        record_access_controller_->applyCurrentProjection(*record_workspace_);
    if (!reconciled_head) {
        const auto message =
            QStringLiteral("Current record-access journal could not be reconciled: %1")
                .arg(reconciled_head.error().message);
        invalidateRecordSelection();
        showError(message);
        updateActionStates();
        return;
    }

    const auto& snapshot = record_access_controller_->snapshot();
    if (snapshot.sequence < 0 || snapshot.sequence == std::numeric_limits<qint64>::max()) {
        showError(QStringLiteral("Record-access transition sequence is unavailable"));
        updateActionStates();
        return;
    }
    const auto next_sequence = static_cast<std::uint64_t>(snapshot.sequence) + 1U;
    const auto stamp = record_access_transition_provider_->next(
        snapshot.session_id, next_sequence, packs::RuntimeRecordDisclosureId{disclosure_id},
        action);
    if (!stamp) {
        showError(QStringLiteral("Record-access transition could not be identified: %1")
                      .arg(stamp.error()));
        updateActionStates();
        return;
    }

    const auto event_id = stamp->event_id.toUtf8();
    const auto public_id = std::string_view(disclosure_id);
    const auto event_id_view =
        std::string_view(event_id.constData(), static_cast<std::size_t>(event_id.size()));
    auto transitioned =
        action == model::RecordAccessAction::Grant
            ? record_access_controller_->grant(public_id, event_id_view, stamp->recorded_at_utc)
            : record_access_controller_->revoke(public_id, event_id_view, stamp->recorded_at_utc);
    if (!transitioned) {
        const auto reconciled_after_error =
            record_access_controller_->applyCurrentProjection(*record_workspace_);
        if (!reconciled_after_error) {
            const auto message =
                QStringLiteral("Record-access failure could not be reconciled safely: %1")
                    .arg(reconciled_after_error.error().message);
            invalidateRecordSelection();
            showError(message);
            updateActionStates();
            return;
        }
        showError(QStringLiteral("Local record access for public disclosure %1 was unchanged: %2")
                      .arg(utf8(disclosure_id), transitioned.error().message));
        updateActionStates();
        return;
    }

    const auto applied = record_access_controller_->applyCurrentProjection(*record_workspace_);
    if (!applied) {
        const auto message =
            QStringLiteral("Persisted record-access projection could not be applied: %1")
                .arg(applied.error().message);
        invalidateRecordSelection();
        showError(message);
        updateActionStates();
        return;
    }

    showStatus(QStringLiteral("%1 local session access for public disclosure %2.")
                   .arg(action == model::RecordAccessAction::Grant ? QStringLiteral("Granted")
                                                                   : QStringLiteral("Revoked"),
                        utf8(disclosure_id)));
    updateActionStates();
}

void MainWindow::invalidateRecordSelection() {
    record_access_controller_.reset();
    clearRecordAccessActions();
    record_revision_.reset();
    record_case_id_.reset();
    if (workspace_tabs_ != nullptr) {
        resetRecordWorkspace();
        workspace_tabs_->setCurrentIndex(browser_tab_index_);
    }
}

void MainWindow::invalidateWorkflowSelection() {
    workflow_controller_.reset();
    workflow_revision_.reset();
    workflow_case_id_.reset();
    if (workflow_status_label_ != nullptr) {
        workflow_status_label_->setText(
            workflow_launch_provider_
                ? QStringLiteral("Workflow: Select an installed case, then open or resume its "
                                 "exact local session.")
                : QStringLiteral("Workflow unavailable: no production local session provider is "
                                 "configured."));
    }
}

void MainWindow::invalidateArgumentSelection() {
    argument_revision_.reset();
    argument_case_id_.reset();
    argument_configuration_id_.reset();
    if (workspace_tabs_ != nullptr && oral_argument_workspace_ != nullptr) {
        workspace_tabs_->setTabEnabled(argument_tab_index_, false);
        workspace_tabs_->setTabText(argument_tab_index_, QStringLiteral("Oral &Argument"));
        if (workspace_tabs_->currentIndex() == argument_tab_index_) {
            workspace_tabs_->setCurrentIndex(browser_tab_index_);
        }
    }
}

bool MainWindow::selectedCaseHasLoadedRecord() const {
    if (!record_revision_ || !record_case_id_ || !runtime_pack_ ||
        *record_revision_ != runtime_pack_->revision || case_list_->currentRow() < 0) {
        return false;
    }
    const auto selected_index = static_cast<std::size_t>(case_list_->currentRow());
    return selected_index < runtime_pack_->cases.size() &&
           runtime_pack_->cases.at(selected_index).definition.id == *record_case_id_;
}

bool MainWindow::selectedCaseHasLoadedWorkflow() const {
    if (workflow_controller_ == nullptr || !workflow_revision_ || !workflow_case_id_ ||
        !runtime_pack_ || *workflow_revision_ != runtime_pack_->revision ||
        case_list_->currentRow() < 0) {
        return false;
    }
    const auto selected_index = static_cast<std::size_t>(case_list_->currentRow());
    return selected_index < runtime_pack_->cases.size() &&
           runtime_pack_->cases.at(selected_index).definition.id == *workflow_case_id_;
}

MainWindow::WorkflowAdvanceChoice
MainWindow::currentWorkflowAdvanceChoice(const WorkflowLegalClockReading& reading) const {
    if (!selectedCaseHasLoadedWorkflow()) {
        return {};
    }
    const auto case_index = static_cast<std::size_t>(case_list_->currentRow());
    const auto& runtime_case = runtime_pack_->cases.at(case_index);
    const auto& state = workflow_controller_->state();
    std::vector<const model::WorkflowOperation*> operations;
    for (const auto& operation : runtime_case.workflow.operations) {
        if (operation.stage_id == state.current_stage_id &&
            operation.opcode == model::WorkflowOpcode::AdvanceStage &&
            operation.next_stage_id.has_value()) {
            operations.push_back(&operation);
        }
    }
    std::ranges::sort(operations, {},
                      [](const auto* operation) { return std::string_view(operation->id.value); });

    for (const auto* operation : operations) {
        std::vector<const model::CaseActor*> actors;
        for (const auto& actor : runtime_case.definition.actors) {
            if (std::ranges::contains(operation->authorized_roles, actor.role)) {
                actors.push_back(&actor);
            }
        }
        std::ranges::sort(actors, {},
                          [](const auto* actor) { return std::string_view(actor->id.value); });
        for (const auto* actor : actors) {
            auto command = workflowAdvanceCommand(state, *operation, *actor, reading);
            const auto decision =
                engine::decideWorkflow(runtime_case.workflow, runtime_case.definition, state,
                                       model::WorkflowCommand{command});
            if (decision) {
                return WorkflowAdvanceChoice{operation, actor, std::move(command)};
            }
        }
    }
    return {};
}

auto MainWindow::sampleWorkflowLegalClock() const
    -> std::expected<WorkflowLegalClockReading, QString> {
    if (workflow_court_date_editor_ == nullptr || !workflow_legal_clock_) {
        return std::unexpected(QStringLiteral("Workflow legal clock is not configured"));
    }
    const auto text = workflow_court_date_editor_->text().trimmed();
    const auto selected_date = QDate::fromString(text, QStringLiteral("yyyy-MM-dd"));
    if (!selected_date.isValid() || selected_date.toString(QStringLiteral("yyyy-MM-dd")) != text) {
        return std::unexpected(QStringLiteral("Enter the legal court date exactly as YYYY-MM-DD"));
    }
    const auto reading = workflow_legal_clock_(selected_date);
    if (!reading) {
        return std::unexpected(reading.error());
    }
    if (!reading->instant_utc.isValid() || reading->court_date != selected_date) {
        return std::unexpected(
            QStringLiteral("Workflow legal clock did not preserve the selected court date"));
    }
    return WorkflowLegalClockReading{reading->instant_utc.toUTC(), reading->court_date};
}

void MainWindow::renderWorkflowStatus() {
    const auto preview_reading = sampleWorkflowLegalClock();
    renderWorkflowStatus(preview_reading);
}

void MainWindow::renderWorkflowStatus(
    const std::expected<WorkflowLegalClockReading, QString>& preview_reading) {
    if (!selectedCaseHasLoadedWorkflow()) {
        invalidateWorkflowSelection();
        updateActionStates();
        return;
    }
    const auto& state = workflow_controller_->state();
    const auto& snapshot = workflow_controller_->snapshot();
    const auto choice =
        preview_reading ? currentWorkflowAdvanceChoice(*preview_reading) : WorkflowAdvanceChoice{};
    auto text = QStringLiteral("Workflow session %1 — stage %2; next event %3; %4 persisted "
                               "commands; %5 persisted events.")
                    .arg(QString::fromStdString(state.session_id),
                         QString::fromStdString(state.current_stage_id.value))
                    .arg(state.next_event_sequence)
                    .arg(static_cast<qulonglong>(snapshot.commands.size()))
                    .arg(static_cast<qulonglong>(snapshot.events.size()));
    if (!preview_reading) {
        text += QStringLiteral(" Exact transition preview is unavailable (%1); transition "
                               "submission is disabled.")
                    .arg(preview_reading.error());
    } else if (choice.operation != nullptr && choice.actor != nullptr &&
               choice.command.has_value()) {
        text += QStringLiteral(" Next exact transition: operation %1 as actor %2 to stage %3.")
                    .arg(QString::fromStdString(choice.operation->id.value),
                         QString::fromStdString(choice.actor->id.value),
                         QString::fromStdString(choice.operation->next_stage_id->value));
    } else {
        text += QStringLiteral(
            " No currently eligible authored AdvanceStage operation and authorized actor exist "
            "for the selected legal court date.");
    }
    workflow_status_label_->setText(text);
    updateActionStates(preview_reading);
}

bool MainWindow::selectedCaseHasLoadedArgument() const {
    if (!argument_revision_ || !argument_case_id_ || !argument_configuration_id_ ||
        !runtime_pack_ || *argument_revision_ != runtime_pack_->revision ||
        case_list_->currentRow() < 0 || argument_configuration_selector_->currentIndex() < 0) {
        return false;
    }
    const auto case_index = static_cast<std::size_t>(case_list_->currentRow());
    const auto argument_index =
        static_cast<std::size_t>(argument_configuration_selector_->currentIndex());
    return case_index < runtime_pack_->cases.size() &&
           runtime_pack_->cases.at(case_index).definition.id == *argument_case_id_ &&
           argument_index < runtime_pack_->cases.at(case_index).argument_configurations.size() &&
           runtime_pack_->cases.at(case_index).argument_configurations.at(argument_index).id ==
               *argument_configuration_id_;
}

void MainWindow::updateCaseSelection(int row) {
    if (!runtime_pack_ || row < 0 || static_cast<std::size_t>(row) >= runtime_pack_->cases.size()) {
        if (record_case_id_.has_value() || record_access_controller_ != nullptr) {
            invalidateRecordSelection();
        }
        if (workflow_case_id_.has_value() || workflow_controller_ != nullptr) {
            invalidateWorkflowSelection();
        }
        profile_selector_->setEnabled(false);
        argument_configuration_selector_->setEnabled(false);
        workspace_tabs_->setTabEnabled(argument_tab_index_, false);
        profile_editor_->setEnabled(false);
        updateActionStates();
        return;
    }

    const auto& runtime_case = runtime_pack_->cases.at(static_cast<std::size_t>(row));
    if ((record_case_id_.has_value() && *record_case_id_ != runtime_case.definition.id) ||
        (record_access_controller_ != nullptr && !record_case_id_.has_value())) {
        invalidateRecordSelection();
    }
    if ((workflow_case_id_.has_value() && *workflow_case_id_ != runtime_case.definition.id) ||
        (workflow_controller_ != nullptr && !workflow_case_id_.has_value())) {
        invalidateWorkflowSelection();
    }
    const auto record_matches_selection = selectedCaseHasLoadedRecord();
    workspace_tabs_->setTabEnabled(record_tab_index_, record_matches_selection);
    if (!record_matches_selection && workspace_tabs_->currentIndex() == record_tab_index_) {
        workspace_tabs_->setCurrentIndex(browser_tab_index_);
    }
    court_summary_label_->setText(QStringLiteral("Court: %1 — %2; jurisdiction %3")
                                      .arg(utf8(runtime_case.court.name),
                                           courtRoleName(runtime_case.court.role),
                                           utf8(runtime_case.court.jurisdiction_id.value)));
    procedure_summary_label_->setText(
        QStringLiteral("Procedure: %1 — %2; %3")
            .arg(utf8(runtime_case.procedure.id.value),
                 proceedingName(runtime_case.procedure.proceeding_type),
                 countText(runtime_case.procedure.actor_roles.size(), u"actor role",
                           u"actor roles")));
    const auto public_entry_count = static_cast<std::size_t>(std::ranges::count_if(
        runtime_case.record.docket_entries,
        [](const packs::RuntimeDocketEntry& entry) { return !entry.sealed; }));
    record_summary_label_->setText(
        QStringLiteral("Record: %1 (%2) — %3 available publicly")
            .arg(utf8(runtime_case.record.caption), utf8(runtime_case.record.id.value),
                 countText(public_entry_count, u"docket entry", u"docket entries")));

    QStringList benches;
    for (const auto& argument : runtime_case.argument_configurations) {
        QStringList seats;
        for (const auto& seat : argument.bench.seats) {
            auto seat_text = utf8(seat.profile.display_name);
            if (seat.id == argument.bench.presiding_seat_id) {
                seat_text += QStringLiteral(" (presiding)");
            }
            seats.push_back(std::move(seat_text));
        }
        benches.push_back(QStringLiteral("%1: %2").arg(utf8(argument.bench.id.value),
                                                       seats.join(QStringLiteral(", "))));
    }
    bench_summary_label_->setText(
        QStringLiteral("Bench: %1 — %2")
            .arg(countText(runtime_case.argument_configurations.size(),
                           u"oral-argument configuration", u"oral-argument configurations"),
                 benches.join(QStringLiteral("; "))));

    {
        const QSignalBlocker blocker(argument_configuration_selector_);
        argument_configuration_selector_->clear();
        for (const auto& argument : runtime_case.argument_configurations) {
            QString mode = QStringLiteral("ungrounded");
            if (argument.grounded_question_bank.has_value()) {
                mode =
                    argument.grounded_question_bank->mode == model::OralArgumentMode::ActualRecord
                        ? QStringLiteral("actual record")
                        : QStringLiteral("counterfactual training");
            }
            argument_configuration_selector_->addItem(
                QStringLiteral("%1 — %2").arg(utf8(argument.id.value), mode));
        }
        argument_configuration_selector_->setEnabled(argument_configuration_selector_->count() > 0);
        argument_configuration_selector_->setCurrentIndex(
            argument_configuration_selector_->count() > 0 ? 0 : -1);
    }
    updateArgumentSelection(argument_configuration_selector_->currentIndex());
}

void MainWindow::updateArgumentSelection(int row) {
    const auto case_row = case_list_->currentRow();
    if (!runtime_pack_ || case_row < 0 || row < 0 ||
        static_cast<std::size_t>(case_row) >= runtime_pack_->cases.size()) {
        profile_selector_->clear();
        profile_selector_->setEnabled(false);
        workspace_tabs_->setTabEnabled(argument_tab_index_, false);
        updateActionStates();
        return;
    }
    const auto& runtime_case = runtime_pack_->cases.at(static_cast<std::size_t>(case_row));
    const auto argument_index = static_cast<std::size_t>(row);
    if (argument_index >= runtime_case.argument_configurations.size()) {
        profile_selector_->clear();
        profile_selector_->setEnabled(false);
        workspace_tabs_->setTabEnabled(argument_tab_index_, false);
        updateActionStates();
        return;
    }

    {
        const QSignalBlocker blocker(profile_selector_);
        profile_selector_->clear();
        const auto& argument = runtime_case.argument_configurations.at(argument_index);
        for (std::size_t seat_index = 0; seat_index < argument.bench.seats.size(); ++seat_index) {
            const auto& seat = argument.bench.seats.at(seat_index);
            profile_selector_->addItem(
                QStringLiteral("%1 — fictional/composite").arg(utf8(seat.profile.display_name)));
            const auto item_index = profile_selector_->count() - 1;
            profile_selector_->setItemData(
                item_index, QVariant::fromValue(static_cast<qulonglong>(argument_index)),
                Qt::UserRole);
            profile_selector_->setItemData(item_index,
                                           QVariant::fromValue(static_cast<qulonglong>(seat_index)),
                                           Qt::UserRole + 1);
        }
        profile_selector_->setEnabled(profile_selector_->count() > 0);
        profile_selector_->setCurrentIndex(profile_selector_->count() > 0 ? 0 : -1);
    }
    const auto argument_matches_selection = selectedCaseHasLoadedArgument();
    workspace_tabs_->setTabEnabled(argument_tab_index_, argument_matches_selection);
    if (!argument_matches_selection && workspace_tabs_->currentIndex() == argument_tab_index_) {
        workspace_tabs_->setCurrentIndex(browser_tab_index_);
    }
    updateProfileSelection(profile_selector_->currentIndex());
    updateActionStates();
}

void MainWindow::updateProfileSelection(int row) {
    if (!runtime_pack_ || row < 0 || case_list_->currentRow() < 0) {
        updateActionStates();
        return;
    }
    const auto case_index = static_cast<std::size_t>(case_list_->currentRow());
    if (case_index >= runtime_pack_->cases.size()) {
        updateActionStates();
        return;
    }

    const auto argument_value = profile_selector_->itemData(row, Qt::UserRole);
    const auto seat_value = profile_selector_->itemData(row, Qt::UserRole + 1);
    bool argument_ok = false;
    bool seat_ok = false;
    const auto argument_number = argument_value.toULongLong(&argument_ok);
    const auto seat_number = seat_value.toULongLong(&seat_ok);
    if (!argument_ok || !seat_ok) {
        showError(QStringLiteral("Selected fictional/composite profile has invalid UI metadata"));
        updateActionStates();
        return;
    }
    const auto argument_index = static_cast<std::size_t>(argument_number);
    const auto seat_index = static_cast<std::size_t>(seat_number);
    const auto& runtime_case = runtime_pack_->cases.at(case_index);
    if (argument_index >= runtime_case.argument_configurations.size() ||
        seat_index >= runtime_case.argument_configurations.at(argument_index).bench.seats.size()) {
        showError(QStringLiteral("Selected fictional/composite profile is outside the case bench"));
        updateActionStates();
        return;
    }

    const auto& profile =
        runtime_case.argument_configurations.at(argument_index).bench.seats.at(seat_index).profile;
    const auto loaded = profile_editor_->loadProfile(profile);
    if (!loaded) {
        showError(loaded.error().message);
        profile_editor_->setEnabled(false);
        updateActionStates();
        return;
    }
    profile_editor_->setEnabled(true);
    updateActionStates();
    showStatus(QStringLiteral("Selected fictional/composite profile: %1.")
                   .arg(utf8(profile.display_name)));
}

void MainWindow::updateActionStates() {
    const auto preview_reading = sampleWorkflowLegalClock();
    updateActionStates(preview_reading);
}

void MainWindow::updateActionStates(
    const std::expected<WorkflowLegalClockReading, QString>& preview_reading) {
    open_directory_action_->setEnabled(true);
    open_directory_action_->setVisible(true);
    const auto can_install_archive = catalog_ != nullptr;
    install_archive_action_->setEnabled(can_install_archive);
    install_archive_action_->setVisible(can_install_archive);
    import_profile_action_->setEnabled(true);
    import_profile_action_->setVisible(true);
    const auto has_profile = profile_editor_->isEnabled() && profile_editor_->profile().has_value();
    clone_profile_action_->setEnabled(has_profile);
    clone_profile_action_->setVisible(has_profile);
    export_profile_action_->setEnabled(has_profile);
    export_profile_action_->setVisible(has_profile);
    const auto selected_row = case_list_->currentRow();
    const auto can_open_record =
        catalog_ != nullptr && installed_pack_.has_value() && runtime_pack_.has_value() &&
        selected_row >= 0 && static_cast<std::size_t>(selected_row) < runtime_pack_->cases.size();
    open_record_action_->setEnabled(can_open_record);
    open_record_action_->setVisible(can_open_record);
    const auto can_open_workflow =
        workflow_launch_provider_ != nullptr && installed_pack_.has_value() &&
        runtime_pack_.has_value() && selected_row >= 0 &&
        static_cast<std::size_t>(selected_row) < runtime_pack_->cases.size();
    open_workflow_action_->setEnabled(can_open_workflow);
    open_workflow_action_->setVisible(can_open_workflow);
    open_workflow_button_->setEnabled(can_open_workflow);
    const auto workflow_choice =
        preview_reading ? currentWorkflowAdvanceChoice(*preview_reading) : WorkflowAdvanceChoice{};
    const auto can_advance_workflow = workflow_choice.operation != nullptr &&
                                      workflow_choice.actor != nullptr &&
                                      workflow_choice.command.has_value();
    advance_workflow_action_->setEnabled(can_advance_workflow);
    advance_workflow_action_->setVisible(can_advance_workflow);
    advance_workflow_button_->setEnabled(can_advance_workflow);
    bool can_open_argument = false;
    if (oral_argument_launch_provider_ && installed_pack_ && runtime_pack_ && selected_row >= 0 &&
        argument_configuration_selector_->currentIndex() >= 0 &&
        static_cast<std::size_t>(selected_row) < runtime_pack_->cases.size()) {
        const auto& runtime_case = runtime_pack_->cases.at(static_cast<std::size_t>(selected_row));
        const auto argument_index =
            static_cast<std::size_t>(argument_configuration_selector_->currentIndex());
        can_open_argument = argument_index < runtime_case.argument_configurations.size() &&
                            runtime_case.argument_configurations.at(argument_index)
                                .grounded_question_bank.has_value();
    }
    open_oral_argument_action_->setEnabled(can_open_argument);
    open_oral_argument_action_->setVisible(can_open_argument);

    const auto record_access_active =
        record_access_controller_ != nullptr && selectedCaseHasLoadedRecord();
    const auto disclosures = record_access_controller_ != nullptr
                                 ? record_access_controller_->disclosures()
                                 : std::vector<model::RecordAccessDisclosureStatus>{};
    bool has_visible_record_access_action = false;
    for (const auto& binding : record_access_action_bindings_) {
        const auto disclosure =
            std::ranges::find(disclosures, binding.disclosure_id,
                              &model::RecordAccessDisclosureStatus::disclosure_id);
        if (!record_access_active || disclosure == disclosures.end()) {
            binding.disclosure_menu->menuAction()->setVisible(false);
            binding.grant_action->setEnabled(false);
            binding.grant_action->setVisible(false);
            binding.revoke_action->setEnabled(false);
            binding.revoke_action->setVisible(false);
            continue;
        }
        const auto can_grant = !disclosure->authorized && disclosure->blocking_deficiencies.empty();
        binding.grant_action->setEnabled(can_grant);
        binding.grant_action->setVisible(can_grant);
        const auto can_revoke = disclosure->authorized;
        binding.revoke_action->setEnabled(can_revoke);
        binding.revoke_action->setVisible(can_revoke);
        const auto show_disclosure = can_grant || can_revoke;
        binding.disclosure_menu->menuAction()->setVisible(show_disclosure);
        has_visible_record_access_action = has_visible_record_access_action || show_disclosure;
    }
    const auto show_record_access = record_access_active && has_visible_record_access_action;
    record_access_menu_->setEnabled(show_record_access);
    record_access_menu_->menuAction()->setVisible(show_record_access);
}

void MainWindow::showError(const QString& message) {
    error_label_->setText(QStringLiteral("Error: %1").arg(message));
    error_label_->setVisible(true);
    statusBar()->showMessage(message);
}

void MainWindow::showStatus(const QString& message) {
    error_label_->clear();
    error_label_->setVisible(false);
    statusBar()->showMessage(message);
}

const packs::RuntimePack* MainWindow::currentRuntime() const noexcept {
    return runtime_pack_ ? &*runtime_pack_ : nullptr;
}

QString MainWindow::currentSourcePath() const { return current_source_path_; }

QString MainWindow::catalogRoot() const { return catalog_root_; }

QLabel* MainWindow::revisionLabel() const noexcept { return revision_label_; }

QLabel* MainWindow::sourceLabel() const noexcept { return source_label_; }

QLabel* MainWindow::errorLabel() const noexcept { return error_label_; }

QLabel* MainWindow::courtSummaryLabel() const noexcept { return court_summary_label_; }

QLabel* MainWindow::procedureSummaryLabel() const noexcept { return procedure_summary_label_; }

QLabel* MainWindow::recordSummaryLabel() const noexcept { return record_summary_label_; }

QLabel* MainWindow::benchSummaryLabel() const noexcept { return bench_summary_label_; }

QLabel* MainWindow::workflowStatusLabel() const noexcept { return workflow_status_label_; }

QListWidget* MainWindow::caseList() const noexcept { return case_list_; }

QComboBox* MainWindow::profileSelector() const noexcept { return profile_selector_; }

QComboBox* MainWindow::argumentConfigurationSelector() const noexcept {
    return argument_configuration_selector_;
}

BenchProfileEditor* MainWindow::profileEditor() const noexcept { return profile_editor_; }

RecordWorkspace* MainWindow::recordWorkspace() const noexcept { return record_workspace_; }

OralArgumentWorkspace* MainWindow::oralArgumentWorkspace() const noexcept {
    return oral_argument_workspace_;
}

QTabWidget* MainWindow::workspaceTabs() const noexcept { return workspace_tabs_; }

QAction* MainWindow::openDirectoryAction() const noexcept { return open_directory_action_; }

QAction* MainWindow::installArchiveAction() const noexcept { return install_archive_action_; }

QAction* MainWindow::importProfileAction() const noexcept { return import_profile_action_; }

QAction* MainWindow::cloneProfileAction() const noexcept { return clone_profile_action_; }

QAction* MainWindow::exportProfileAction() const noexcept { return export_profile_action_; }

QAction* MainWindow::openRecordAction() const noexcept { return open_record_action_; }

QAction* MainWindow::openWorkflowAction() const noexcept { return open_workflow_action_; }

QAction* MainWindow::advanceWorkflowAction() const noexcept { return advance_workflow_action_; }

QAction* MainWindow::openOralArgumentAction() const noexcept { return open_oral_argument_action_; }

QPushButton* MainWindow::openWorkflowButton() const noexcept { return open_workflow_button_; }

QPushButton* MainWindow::advanceWorkflowButton() const noexcept { return advance_workflow_button_; }
QLineEdit* MainWindow::workflowCourtDateEditor() const noexcept {
    return workflow_court_date_editor_;
}

QMenu* MainWindow::recordAccessMenu() const noexcept { return record_access_menu_; }

QString MainWindow::recordAccessDatabasePath() const { return record_access_database_path_; }

const app::WorkflowSessionController* MainWindow::workflowSessionController() const noexcept {
    return workflow_controller_.get();
}

} // namespace appellate::ui
