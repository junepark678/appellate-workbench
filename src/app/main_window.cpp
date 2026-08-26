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
#include "workflow_action_planner.hpp"
#include "workflow_launch_provider.hpp"
#include "workflow_session_controller.hpp"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringList>
#include <QStyle>
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
#include <type_traits>
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

[[nodiscard]] const model::WorkflowCommandHeader&
workflowCommandHeader(const model::WorkflowCommand& command) {
    return std::visit(
        [](const auto& concrete) -> const model::WorkflowCommandHeader& { return concrete.header; },
        command);
}

[[nodiscard]] QString workflowRejectionName(model::WorkflowFilingRejectionReason reason) {
    switch (reason) {
    case model::WorkflowFilingRejectionReason::UnauthorizedActor:
        return QStringLiteral("unauthorized actor");
    case model::WorkflowFilingRejectionReason::IneligibleFiling:
        return QStringLiteral("ineligible filing");
    case model::WorkflowFilingRejectionReason::NonconformingFiling:
        return QStringLiteral("nonconforming filing");
    case model::WorkflowFilingRejectionReason::DeadlineExpired:
        return QStringLiteral("deadline expired");
    case model::WorkflowFilingRejectionReason::UnknownDeficiency:
        return QStringLiteral("unknown deficiency");
    }
    return QStringLiteral("invalid filing");
}

[[nodiscard]] QString workflowEventPreview(const model::WorkflowEvent& event) {
    return std::visit(
        [](const auto& concrete) -> QString {
            using Event = std::remove_cvref_t<decltype(concrete)>;
            if constexpr (std::same_as<Event, model::WorkflowFilingAccepted>) {
                return QStringLiteral("Accept filing %1")
                    .arg(QString::fromStdString(concrete.filing_id.value));
            } else if constexpr (std::same_as<Event, model::WorkflowFilingRejected>) {
                return QStringLiteral("Reject filing %1: %2")
                    .arg(QString::fromStdString(concrete.filing_id.value),
                         workflowRejectionName(concrete.reason));
            } else if constexpr (std::same_as<Event, model::WorkflowDeficiencyIssued>) {
                QStringList missing;
                for (const auto& requirement : concrete.missing_requirements) {
                    missing.push_back(QString::fromStdString(requirement.value));
                }
                return QStringLiteral("Issue deficiency %1%2")
                    .arg(QString::fromStdString(concrete.deficiency_id.value),
                         missing.empty() ? QString{}
                                         : QStringLiteral("; missing %1").arg(missing.join(u", ")));
            } else if constexpr (std::same_as<Event, model::WorkflowDeadlineCalculated>) {
                return QStringLiteral("Calculate deadline %1")
                    .arg(QString::fromStdString(concrete.deadline_id.value));
            } else if constexpr (std::same_as<Event, model::WorkflowOrderEntered>) {
                return QStringLiteral("Enter order %1")
                    .arg(QString::fromStdString(concrete.order_id.value));
            } else if constexpr (std::same_as<Event, model::WorkflowStageAdvanced>) {
                return QStringLiteral("Advance stage from %1 to %2")
                    .arg(QString::fromStdString(concrete.previous_stage_id.value),
                         QString::fromStdString(concrete.next_stage_id.value));
            } else if constexpr (std::same_as<Event, model::WorkflowSealedSet>) {
                return concrete.sealed ? QStringLiteral("Seal the matter")
                                       : QStringLiteral("Unseal the matter");
            } else if constexpr (std::same_as<Event, model::WorkflowArgumentScheduled>) {
                return QStringLiteral("Schedule oral argument");
            } else if constexpr (std::same_as<Event, model::WorkflowJudgmentIssued>) {
                return QStringLiteral("Issue judgment");
            } else {
                return QStringLiteral("Issue mandate");
            }
        },
        event);
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
    : MainWindow(source_path, catalog_root, parent, {}, {}, {}, {}, {}, {}, {}, {}) {}

MainWindow::MainWindow(
    const QString& source_path, const QString& catalog_root, QWidget* parent,
    std::shared_ptr<OralArgumentLaunchProvider> oral_argument_launch_provider,
    std::shared_ptr<RecordAccessTransitionProvider> record_access_transition_provider,
    QString record_access_database_path,
    std::shared_ptr<WorkflowLaunchProvider> workflow_launch_provider,
    WorkflowLegalClock workflow_legal_clock, OralElapsedClock oral_elapsed_clock,
    OralRecordedAtClock oral_recorded_at_clock, WorkflowRecordedAtClock workflow_recorded_at_clock)
    : QMainWindow(parent), oral_argument_launch_provider_(std::move(oral_argument_launch_provider)),
      workflow_launch_provider_(std::move(workflow_launch_provider)),
      workflow_legal_clock_(std::move(workflow_legal_clock)),
      workflow_recorded_at_clock_(std::move(workflow_recorded_at_clock)),
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
    if (!workflow_recorded_at_clock_) {
        workflow_recorded_at_clock_ = [] { return canonicalUtcNow(); };
    }
    setWindowTitle(QStringLiteral("Appellate Workbench"));
    resize(1180, 780);
    setMinimumSize(840, 600);
    buildFileMenu();
    buildUi();
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

    error_label_ = new QLabel(container);
    configureSummaryLabel(*error_label_, QStringLiteral("packLoadError"),
                          QStringLiteral("Pack and profile error"));
    error_label_->setStyleSheet(QStringLiteral("color: #a32121; font-weight: 600;"));
    error_label_->setVisible(false);
    outer_layout->addWidget(error_label_);

    startup_onboarding_ = new QWidget(container);
    startup_onboarding_->setObjectName(QStringLiteral("startupOnboarding"));
    startup_onboarding_->setAccessibleName(QStringLiteral("Start a local appellate matter"));
    auto* onboarding_outer = new QVBoxLayout(startup_onboarding_);
    onboarding_outer->addStretch(1);
    auto* onboarding_group =
        new QGroupBox(QStringLiteral("Start with a local pack"), startup_onboarding_);
    onboarding_group->setObjectName(QStringLiteral("startupOnboardingGroup"));
    onboarding_group->setAccessibleName(QStringLiteral("Local pack start options"));
    auto* onboarding_layout = new QVBoxLayout(onboarding_group);
    auto* onboarding_summary = new QLabel(
        QStringLiteral("Open an authoring folder or install an .awpack archive. No account or "
                       "network connection is required."),
        onboarding_group);
    configureSummaryLabel(*onboarding_summary, QStringLiteral("startupOnboardingSummary"),
                          QStringLiteral("Local pack onboarding summary"));
    onboarding_layout->addWidget(onboarding_summary);
    auto* onboarding_buttons = new QHBoxLayout();
    welcome_install_archive_button_ =
        new QPushButton(QStringLiteral("&Install pack…"), onboarding_group);
    welcome_install_archive_button_->setObjectName(
        QStringLiteral("welcomeInstallPackArchiveButton"));
    welcome_install_archive_button_->setAccessibleName(QStringLiteral("Install local pack"));
    welcome_install_archive_button_->setAccessibleDescription(
        QStringLiteral("Choose and install a local Appellate Workbench pack archive"));
    welcome_install_archive_button_->setIcon(install_archive_action_->icon());
    welcome_open_directory_button_ =
        new QPushButton(QStringLiteral("Open authoring &folder…"), onboarding_group);
    welcome_open_directory_button_->setObjectName(QStringLiteral("welcomeOpenPackDirectoryButton"));
    welcome_open_directory_button_->setAccessibleName(
        QStringLiteral("Open local authoring pack folder"));
    welcome_open_directory_button_->setAccessibleDescription(
        QStringLiteral("Choose and validate a local authoring pack directory"));
    welcome_open_directory_button_->setIcon(open_directory_action_->icon());
    welcome_import_profile_button_ =
        new QPushButton(QStringLiteral("Import &profile…"), onboarding_group);
    welcome_import_profile_button_->setObjectName(QStringLiteral("welcomeImportProfileButton"));
    welcome_import_profile_button_->setAccessibleName(
        QStringLiteral("Import fictional composite profile"));
    welcome_import_profile_button_->setAccessibleDescription(
        QStringLiteral("Open the profile editor with a local fictional composite profile"));
    welcome_import_profile_button_->setIcon(import_profile_action_->icon());
    onboarding_buttons->addWidget(welcome_install_archive_button_);
    onboarding_buttons->addWidget(welcome_open_directory_button_);
    onboarding_buttons->addWidget(welcome_import_profile_button_);
    onboarding_layout->addLayout(onboarding_buttons);
    onboarding_outer->addWidget(onboarding_group);
    onboarding_outer->addStretch(1);
    outer_layout->addWidget(startup_onboarding_, 1);

    connect(welcome_install_archive_button_, &QPushButton::clicked, install_archive_action_,
            &QAction::trigger);
    connect(welcome_open_directory_button_, &QPushButton::clicked, open_directory_action_,
            &QAction::trigger);
    connect(welcome_import_profile_button_, &QPushButton::clicked, import_profile_action_,
            &QAction::trigger);

    pack_browser_content_ = new QWidget(container);
    pack_browser_content_->setObjectName(QStringLiteral("packBrowserContent"));
    pack_browser_content_->setAccessibleName(QStringLiteral("Loaded pack workspace"));
    auto* content_layout = new QVBoxLayout(pack_browser_content_);

    revision_label_ =
        new QLabel(QStringLiteral("Pack revision: No pack loaded."), pack_browser_content_);
    configureSummaryLabel(*revision_label_, QStringLiteral("packRevision"),
                          QStringLiteral("Loaded pack revision"));
    content_layout->addWidget(revision_label_);

    source_label_ = new QLabel(QStringLiteral("Source: No pack loaded."), pack_browser_content_);
    configureSummaryLabel(*source_label_, QStringLiteral("packSource"),
                          QStringLiteral("Loaded pack source"));
    content_layout->addWidget(source_label_);

    auto* splitter = new QSplitter(Qt::Horizontal, pack_browser_content_);
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

    auto* details_scroll = new QScrollArea(splitter);
    details_scroll->setObjectName(QStringLiteral("caseDetailsScrollArea"));
    details_scroll->setAccessibleName(QStringLiteral("Scrollable selected case controls"));
    details_scroll->setWidgetResizable(true);
    details_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    auto* details = new QWidget(details_scroll);
    details->setObjectName(QStringLiteral("caseDetailsPane"));
    auto* details_layout = new QVBoxLayout(details);
    details_scroll->setWidget(details);

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
        new QLabel(QStringLiteral("Legacy legal &court date (YYYY-MM-DD)"), workflow_group);
    court_date_label->setObjectName(QStringLiteral("workflowCourtDateLabel"));
    court_date_label->setAccessibleName(QStringLiteral("Workflow legal court date label"));
    workflow_court_date_editor_ = new QLineEdit(workflow_group);
    workflow_court_date_editor_->setObjectName(QStringLiteral("workflowCourtDateEditor"));
    workflow_court_date_editor_->setAccessibleName(QStringLiteral("Workflow legal court date"));
    workflow_court_date_editor_->setAccessibleDescription(QStringLiteral(
        "Fallback court-local date for legacy packs without authored legal times; version 2 "
        "actions use their immutable pack-authored timestamp"));
    workflow_court_date_editor_->setPlaceholderText(QStringLiteral("YYYY-MM-DD"));
    workflow_court_date_editor_->setMaxLength(10);
    court_date_label->setBuddy(workflow_court_date_editor_);
    workflow_layout->addWidget(court_date_label);
    workflow_layout->addWidget(workflow_court_date_editor_);
    open_workflow_button_ = new QPushButton(QStringLiteral("Open &workflow"), workflow_group);
    open_workflow_button_->setObjectName(QStringLiteral("openSelectedWorkflowButton"));
    open_workflow_button_->setAccessibleName(QStringLiteral("Open or resume selected workflow"));
    open_workflow_button_->setAccessibleDescription(
        QStringLiteral("Open the exact installed workflow using crash-safe local session storage"));
    open_workflow_button_->setIcon(open_workflow_action_->icon());
    workflow_layout->addWidget(open_workflow_button_);

    workflow_action_workspace_ = new QWidget(workflow_group);
    workflow_action_workspace_->setObjectName(QStringLiteral("workflowActionWorkspace"));
    workflow_action_workspace_->setAccessibleName(
        QStringLiteral("Exact authored workflow action workspace"));
    auto* action_workspace_layout = new QVBoxLayout(workflow_action_workspace_);
    action_workspace_layout->setContentsMargins(0, 0, 0, 0);
    auto* action_selector_label =
        new QLabel(QStringLiteral("Authored workflow &action"), workflow_action_workspace_);
    action_selector_label->setObjectName(QStringLiteral("workflowActionSelectorLabel"));
    action_selector_label->setAccessibleName(QStringLiteral("Authored workflow action label"));
    workflow_action_selector_ = new QComboBox(workflow_action_workspace_);
    workflow_action_selector_->setObjectName(QStringLiteral("workflowActionSelector"));
    workflow_action_selector_->setAccessibleName(QStringLiteral("Exact authored workflow action"));
    workflow_action_selector_->setAccessibleDescription(QStringLiteral(
        "Choose an engine-eligible action bound to the installed pack and current local state"));
    action_selector_label->setBuddy(workflow_action_selector_);
    action_workspace_layout->addWidget(action_selector_label);
    action_workspace_layout->addWidget(workflow_action_selector_);

    workflow_action_description_label_ = new QLabel(workflow_action_workspace_);
    configureSummaryLabel(*workflow_action_description_label_,
                          QStringLiteral("workflowActionDescription"),
                          QStringLiteral("Selected workflow action details"));
    action_workspace_layout->addWidget(workflow_action_description_label_);

    workflow_filing_form_ = new QWidget(workflow_action_workspace_);
    workflow_filing_form_->setObjectName(QStringLiteral("workflowFilingForm"));
    workflow_filing_form_->setAccessibleName(QStringLiteral("Workflow filing details"));
    workflow_filing_form_layout_ = new QFormLayout(workflow_filing_form_);
    workflow_filing_form_layout_->setContentsMargins(0, 0, 0, 0);
    action_workspace_layout->addWidget(workflow_filing_form_);

    workflow_action_preview_label_ = new QLabel(workflow_action_workspace_);
    configureSummaryLabel(*workflow_action_preview_label_, QStringLiteral("workflowActionPreview"),
                          QStringLiteral("Authored workflow consequence preview"));
    action_workspace_layout->addWidget(workflow_action_preview_label_);

    advance_workflow_button_ =
        new QPushButton(QStringLiteral("&Submit workflow action"), workflow_action_workspace_);
    advance_workflow_button_->setObjectName(QStringLiteral("advanceSelectedWorkflowButton"));
    advance_workflow_button_->setAccessibleName(QStringLiteral("Submit authored workflow action"));
    advance_workflow_button_->setAccessibleDescription(QStringLiteral(
        "Persist the selected fresh authored action and its displayed engine consequence"));
    advance_workflow_button_->setIcon(advance_workflow_action_->icon());
    action_workspace_layout->addWidget(advance_workflow_button_);
    for (const auto& sequence : {QKeySequence(QStringLiteral("Ctrl+Return")),
                                 QKeySequence(QStringLiteral("Ctrl+Enter"))}) {
        auto* shortcut = new QShortcut(sequence, workflow_action_workspace_);
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut, &QShortcut::activated, advance_workflow_action_, &QAction::trigger);
    }
    workflow_action_workspace_->setVisible(false);
    workflow_layout->addWidget(workflow_action_workspace_);

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

    argument_launch_boundary_label_ = new QLabel(details);
    configureSummaryLabel(*argument_launch_boundary_label_,
                          QStringLiteral("oralArgumentLaunchBoundary"),
                          QStringLiteral("Oral argument launch boundary"));
    const auto argument_launch_boundary =
        oral_argument_launch_provider_
            ? QStringLiteral("Launch uses only the exact installed closure and selected IDs.")
            : QStringLiteral("Oral argument launch is disabled until the application supplies a "
                             "workflow-authoritative local session provider; this UI never "
                             "invents legal-state or disposition pins.");
    argument_launch_boundary_label_->setText(
        oral_argument_launch_provider_
            ? QStringLiteral("Grounded launch is available for this installed configuration.")
            : QStringLiteral("Oral argument is unavailable for this pack source."));
    argument_launch_boundary_label_->setAccessibleDescription(argument_launch_boundary);
    argument_launch_boundary_label_->setToolTip(argument_launch_boundary);

    profile_selector_ = new QComboBox(details);
    profile_selector_->setObjectName(QStringLiteral("profileSelector"));
    profile_selector_->setAccessibleName(QStringLiteral("Selected fictional/composite profile"));
    profile_selector_->setAccessibleDescription(
        QStringLiteral("Choose a fictional/composite profile from the selected case bench"));
    profile_selector_->setEnabled(false);
    profile_label->setBuddy(profile_selector_);

    profile_editor_ = new BenchProfileEditor(details);
    profile_editor_->setEnabled(false);

    case_details_tabs_ = new QTabWidget(details);
    case_details_tabs_->setObjectName(QStringLiteral("caseDetailsTabs"));
    case_details_tabs_->setAccessibleName(QStringLiteral("Selected case task areas"));
    case_details_tabs_->setDocumentMode(true);

    auto* overview_page = new QWidget(case_details_tabs_);
    overview_page->setObjectName(QStringLiteral("caseOverviewPage"));
    overview_page->setAccessibleName(QStringLiteral("Selected case overview"));
    auto* overview_layout = new QVBoxLayout(overview_page);
    overview_layout->addWidget(summary_group);
    open_record_button_ = new QPushButton(QStringLiteral("Open &record"), overview_page);
    open_record_button_->setObjectName(QStringLiteral("openSelectedRecordButton"));
    open_record_button_->setAccessibleName(QStringLiteral("Open selected installed record"));
    open_record_button_->setAccessibleDescription(
        QStringLiteral("Verify and open the selected case record from local installed storage"));
    open_record_button_->setIcon(open_record_action_->icon());
    overview_layout->addWidget(open_record_button_);
    overview_layout->addStretch(1);
    overview_detail_tab_index_ =
        case_details_tabs_->addTab(overview_page, QStringLiteral("&Overview"));

    auto* workflow_page = new QWidget(case_details_tabs_);
    workflow_page->setObjectName(QStringLiteral("caseWorkflowPage"));
    workflow_page->setAccessibleName(QStringLiteral("Selected case workflow"));
    auto* workflow_page_layout = new QVBoxLayout(workflow_page);
    workflow_page_layout->addWidget(workflow_group);
    workflow_page_layout->addStretch(1);
    workflow_detail_tab_index_ =
        case_details_tabs_->addTab(workflow_page, QStringLiteral("&Workflow"));

    auto* argument_page = new QWidget(case_details_tabs_);
    argument_page->setObjectName(QStringLiteral("caseArgumentPage"));
    argument_page->setAccessibleName(QStringLiteral("Selected case oral argument"));
    auto* argument_page_layout = new QVBoxLayout(argument_page);
    argument_page_layout->addWidget(argument_label);
    argument_page_layout->addWidget(argument_configuration_selector_);
    argument_page_layout->addWidget(argument_launch_boundary_label_);
    open_oral_argument_button_ =
        new QPushButton(QStringLiteral("Start oral &argument"), argument_page);
    open_oral_argument_button_->setObjectName(QStringLiteral("openSelectedOralArgumentButton"));
    open_oral_argument_button_->setAccessibleName(
        QStringLiteral("Start selected grounded oral argument"));
    open_oral_argument_button_->setAccessibleDescription(
        QStringLiteral("Open the exact selected actual-record or counterfactual configuration"));
    open_oral_argument_button_->setIcon(open_oral_argument_action_->icon());
    argument_page_layout->addWidget(open_oral_argument_button_);
    argument_page_layout->addStretch(1);
    argument_detail_tab_index_ =
        case_details_tabs_->addTab(argument_page, QStringLiteral("Oral &Argument"));

    auto* profile_page = new QWidget(case_details_tabs_);
    profile_page->setObjectName(QStringLiteral("caseProfilePage"));
    profile_page->setAccessibleName(QStringLiteral("Fictional composite bench profile"));
    auto* profile_page_layout = new QVBoxLayout(profile_page);
    profile_page_layout->addWidget(profile_label);
    profile_page_layout->addWidget(profile_selector_);
    profile_page_layout->addWidget(profile_editor_, 1);
    profile_detail_tab_index_ =
        case_details_tabs_->addTab(profile_page, QStringLiteral("&Bench Profile"));

    details_layout->addWidget(case_details_tabs_, 1);

    connect(open_record_button_, &QPushButton::clicked, this,
            [this] { static_cast<void>(openSelectedRecord()); });
    connect(open_oral_argument_button_, &QPushButton::clicked, this,
            [this] { static_cast<void>(openSelectedOralArgument()); });

    splitter->addWidget(browser);
    splitter->addWidget(details_scroll);
    splitter->setChildrenCollapsible(false);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    content_layout->addWidget(splitter, 1);
    pack_only_widgets_ = {
        revision_label_,
        source_label_,
        browser,
        summary_group,
        workflow_group,
        argument_label,
        argument_configuration_selector_,
        argument_launch_boundary_label_,
        profile_label,
        profile_selector_,
    };
    pack_browser_content_->setVisible(false);
    outer_layout->addWidget(pack_browser_content_, 1);

    browser_tab_index_ = workspace_tabs_->addTab(container, QStringLiteral("&Start"));
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
    workspace_tabs_->setTabVisible(record_tab_index_, false);

    oral_argument_workspace_ = new OralArgumentWorkspace(workspace_tabs_);
    argument_tab_index_ =
        workspace_tabs_->addTab(oral_argument_workspace_, QStringLiteral("Oral &Argument"));
    workspace_tabs_->setTabToolTip(
        argument_tab_index_,
        QStringLiteral("Practice exact pack-authored questions against a fictional bench"));
    workspace_tabs_->setTabEnabled(argument_tab_index_, false);
    workspace_tabs_->setTabVisible(argument_tab_index_, false);
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
    connect(workflow_action_selector_, &QComboBox::currentIndexChanged, this, [this] {
        rebuildWorkflowFilingForm();
        renderWorkflowActionPreview();
        updateActionStates();
    });
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

    open_directory_action_ = file_menu->addAction(QStringLiteral("Open Authoring &Folder\u2026"));
    configureAction(*open_directory_action_, QStringLiteral("openPackDirectoryAction"),
                    QStringLiteral("Open authoring pack directory"),
                    QStringLiteral("Open and strictly validate an authoring pack directory"));
    open_directory_action_->setShortcut(QKeySequence::Open);
    open_directory_action_->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));

    install_archive_action_ = file_menu->addAction(QStringLiteral("&Install Pack\u2026"));
    configureAction(*install_archive_action_, QStringLiteral("installPackArchiveAction"),
                    QStringLiteral("Install pack archive"),
                    QStringLiteral("Install and load a local .awpack archive"));
    install_archive_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+O")));
    install_archive_action_->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));

    open_record_action_ = file_menu->addAction(QStringLiteral("Open &Record"));
    configureAction(
        *open_record_action_, QStringLiteral("openSelectedRecordAction"),
        QStringLiteral("Open selected installed case record"),
        QStringLiteral("Verify public record PDFs and apply persisted local disclosure access"));
    open_record_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));
    open_record_action_->setIcon(style()->standardIcon(QStyle::SP_FileIcon));

    open_workflow_action_ = file_menu->addAction(QStringLiteral("Open &Workflow"));
    configureAction(
        *open_workflow_action_, QStringLiteral("openSelectedWorkflowAction"),
        QStringLiteral("Open or resume selected workflow"),
        QStringLiteral("Open the exact persisted local workflow for the selected case"));
    open_workflow_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+W")));
    open_workflow_action_->setIcon(style()->standardIcon(QStyle::SP_DialogApplyButton));

    advance_workflow_action_ = file_menu->addAction(QStringLiteral("Submit Workflow &Action"));
    configureAction(*advance_workflow_action_, QStringLiteral("advanceSelectedWorkflowAction"),
                    QStringLiteral("Submit selected authored workflow action"),
                    QStringLiteral("Persist the fresh selected action and displayed consequence"));
    advance_workflow_action_->setIcon(style()->standardIcon(QStyle::SP_ArrowForward));

    open_oral_argument_action_ = file_menu->addAction(QStringLiteral("Start Oral &Argument"));
    configureAction(
        *open_oral_argument_action_, QStringLiteral("openSelectedOralArgumentAction"),
        QStringLiteral("Open selected oral argument configuration"),
        QStringLiteral("Open the exact grounded configuration through the authoritative local "
                       "session provider"));
    open_oral_argument_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+A")));
    open_oral_argument_action_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    file_menu->addSeparator();

    import_profile_action_ = file_menu->addAction(QStringLiteral("&Import Profile\u2026"));
    configureAction(*import_profile_action_, QStringLiteral("importProfileAction"),
                    QStringLiteral("Import fictional/composite profile"),
                    QStringLiteral("Import a strict schema-v1 fictional/composite profile"));
    import_profile_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+I")));
    import_profile_action_->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));

    clone_profile_action_ = file_menu->addAction(QStringLiteral("&Clone Profile\u2026"));
    configureAction(*clone_profile_action_, QStringLiteral("cloneProfileAction"),
                    QStringLiteral("Clone fictional/composite profile"),
                    QStringLiteral("Clone the edited fictional/composite profile under a new ID"));
    clone_profile_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+C")));
    clone_profile_action_->setIcon(style()->standardIcon(QStyle::SP_FileDialogNewFolder));

    export_profile_action_ = file_menu->addAction(QStringLiteral("&Export Profile\u2026"));
    configureAction(*export_profile_action_, QStringLiteral("exportProfileAction"),
                    QStringLiteral("Export fictional/composite profile"),
                    QStringLiteral("Export without overwriting an existing path"));
    export_profile_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")));
    export_profile_action_->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));

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
    if (!runtime_pack_) {
        for (auto* widget : pack_only_widgets_) {
            widget->setVisible(false);
        }
        startup_onboarding_->setVisible(false);
        pack_browser_content_->setVisible(true);
        workspace_tabs_->setTabText(browser_tab_index_, QStringLiteral("&Profile"));
        case_details_tabs_->setTabVisible(overview_detail_tab_index_, false);
        case_details_tabs_->setTabVisible(workflow_detail_tab_index_, false);
        case_details_tabs_->setTabVisible(argument_detail_tab_index_, false);
        case_details_tabs_->setTabVisible(profile_detail_tab_index_, true);
        case_details_tabs_->setCurrentIndex(profile_detail_tab_index_);
    }
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
    workspace_tabs_->setTabVisible(record_tab_index_, true);
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
            "Open the selected installed case's local workflow before submitting an action"));
    }
    const auto selected = selectedWorkflowAction();
    if (!selected) {
        renderWorkflowStatus();
        return reject(QStringLiteral("Select a currently eligible authored workflow action"));
    }

    const auto selected_key = selected->key;
    const auto selected_time = workflowCommandHeader(selected->command).occurred_at;
    const auto case_index = static_cast<std::size_t>(case_list_->currentRow());
    const auto& runtime_case = runtime_pack_->cases.at(case_index);
    const auto fresh_options =
        app::eligibleWorkflowActions(runtime_case, workflow_controller_->state(), selected_time);
    const auto fresh =
        std::ranges::find(fresh_options, selected_key, &app::WorkflowActionOption::key);
    if (fresh == fresh_options.end()) {
        renderWorkflowStatus();
        return reject(QStringLiteral(
            "The selected authored action is stale or no longer eligible; choose it again"));
    }

    const auto command = draftWorkflowCommand(*fresh);
    if (!command) {
        renderWorkflowActionPreview();
        updateActionStates();
        return reject(QStringLiteral("Workflow action draft is invalid: %1").arg(command.error()));
    }
    const auto preview = engine::decideWorkflow(runtime_case.workflow, runtime_case.definition,
                                                workflow_controller_->state(), *command);
    if (!preview) {
        renderWorkflowActionPreview();
        updateActionStates();
        return reject(QStringLiteral("Workflow action is not executable: %1")
                          .arg(QString::fromStdString(preview.error().message)));
    }
    const auto document = materializeWorkflowDocument(runtime_case, *fresh);
    if (!document) {
        return reject(document.error());
    }
    std::optional<QByteArrayView> document_view;
    if (document->has_value()) {
        document_view = QByteArrayView(**document);
    }
    const auto recorded_at_utc = workflow_recorded_at_clock_();
    const auto submitted = workflow_controller_->submit(*command, document_view, recorded_at_utc);
    if (!submitted) {
        renderWorkflowActionPreview();
        updateActionStates();
        return reject(
            QStringLiteral("Workflow action was not persisted: %1").arg(submitted.error().message));
    }
    const auto command_id = workflowCommandHeader(*command).command_id.value;
    renderWorkflowStatus();
    showStatus(QStringLiteral("Persisted workflow command %1 through event sequence %2.")
                   .arg(QString::fromStdString(command_id))
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
    workspace_tabs_->setTabVisible(argument_tab_index_, true);
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
    startup_onboarding_->setVisible(false);
    pack_browser_content_->setVisible(true);
    workspace_tabs_->setTabText(browser_tab_index_, QStringLiteral("&Cases and profiles"));
    for (auto* widget : pack_only_widgets_) {
        widget->setVisible(true);
    }
    case_details_tabs_->setTabVisible(overview_detail_tab_index_, true);
    case_details_tabs_->setTabVisible(workflow_detail_tab_index_, true);
    case_details_tabs_->setTabVisible(argument_detail_tab_index_, true);
    case_details_tabs_->setTabVisible(profile_detail_tab_index_, true);
    case_details_tabs_->setCurrentIndex(overview_detail_tab_index_);

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
    workspace_tabs_->setTabVisible(record_tab_index_, false);
    delete previous_workspace;
}

void MainWindow::clearRecordAccessActions() {
    record_access_action_bindings_.clear();
    if (record_access_menu_ != nullptr) {
        record_access_menu_->clear();
        record_access_menu_->setEnabled(false);
        record_access_menu_->menuAction()->setVisible(false);
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
    workflow_action_options_.clear();
    if (workflow_action_selector_ != nullptr) {
        const QSignalBlocker blocker(workflow_action_selector_);
        workflow_action_selector_->clear();
    }
    clearWorkflowFilingForm();
    if (workflow_action_workspace_ != nullptr) {
        workflow_action_workspace_->setVisible(false);
    }
    if (workflow_action_description_label_ != nullptr) {
        workflow_action_description_label_->clear();
    }
    if (workflow_action_preview_label_ != nullptr) {
        workflow_action_preview_label_->clear();
    }
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
        workspace_tabs_->setTabVisible(argument_tab_index_, false);
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

std::optional<app::WorkflowActionOption> MainWindow::selectedWorkflowAction() const {
    if (workflow_action_selector_ == nullptr || workflow_action_selector_->currentIndex() < 0) {
        return std::nullopt;
    }
    const auto selected_key = workflow_action_selector_->currentData(Qt::UserRole).toString();
    const auto found =
        std::ranges::find(workflow_action_options_, selected_key, &app::WorkflowActionOption::key);
    if (found == workflow_action_options_.end()) {
        return std::nullopt;
    }
    return *found;
}

auto MainWindow::draftWorkflowCommand(const app::WorkflowActionOption& option) const
    -> std::expected<model::WorkflowCommand, QString> {
    auto command = option.command;
    auto* filing = std::get_if<model::SubmitWorkflowFiling>(&command);
    if (filing == nullptr) {
        return command;
    }
    if (workflow_field_editors_.size() != option.required_filing_fields.size()) {
        return std::unexpected(QStringLiteral("Required filing fields are not fully rendered"));
    }
    for (auto& field : filing->fields) {
        const auto binding = std::ranges::find(workflow_field_editors_, field.id,
                                               &WorkflowFieldEditorBinding::field_id);
        if (binding == workflow_field_editors_.end() || binding->editor == nullptr) {
            return std::unexpected(
                QStringLiteral("A required authored filing field is unavailable"));
        }
        const auto value = binding->editor->text();
        if (value.contains(QChar::Null)) {
            return std::unexpected(QStringLiteral("A filing field contains a null character"));
        }
        field.value = value.toUtf8().toStdString();
    }
    std::erase_if(filing->fields, [](const auto& field) { return field.value.empty(); });

    filing->served_actors.clear();
    for (const auto& binding : workflow_service_editors_) {
        if (binding.editor != nullptr && binding.editor->isChecked()) {
            filing->served_actors.push_back(binding.actor_id);
        }
    }
    std::ranges::sort(filing->served_actors, {}, &model::ActorId::value);
    filing->cures_deficiency_id.reset();
    if (workflow_cure_selector_ != nullptr && workflow_cure_selector_->currentIndex() > 0) {
        const auto cure = workflow_cure_selector_->currentData(Qt::UserRole).toString();
        if (!cure.isEmpty()) {
            filing->cures_deficiency_id = model::WorkflowDeficiencyId{cure.toUtf8().toStdString()};
        }
    }
    return command;
}

auto MainWindow::materializeWorkflowDocument(const packs::RuntimeCase& runtime_case,
                                             const app::WorkflowActionOption& option) const
    -> std::expected<std::optional<QByteArray>, QString> {
    if (option.record_entry_id.has_value() != option.document_sha256.has_value()) {
        return std::unexpected(QStringLiteral("Authored workflow document identity is incomplete"));
    }
    if (!option.document_sha256.has_value()) {
        return std::optional<QByteArray>{};
    }
    if (catalog_ == nullptr || !installed_pack_ || !option.record_entry_id.has_value()) {
        return std::unexpected(
            QStringLiteral("Workflow documents require the exact installed pack closure"));
    }
    const auto matches = [&](const packs::RuntimeDocketEntry& candidate) {
        return candidate.id.value == *option.record_entry_id &&
               candidate.asset_sha256 == *option.document_sha256;
    };
    if (std::ranges::count_if(runtime_case.record.docket_entries, matches) != 1) {
        return std::unexpected(QStringLiteral(
            "The authored workflow document does not resolve to one exact record entry"));
    }
    const auto entry = std::ranges::find_if(runtime_case.record.docket_entries, matches);
    if (entry == runtime_case.record.docket_entries.end() || entry->sealed) {
        return std::unexpected(
            QStringLiteral("The authored workflow document is unavailable or sealed"));
    }
    const auto record_owner = installed_pack_->resourceOwner(runtime_case.record.id.value);
    if (!record_owner.has_value()) {
        return std::unexpected(
            QStringLiteral("The installed closure does not own the selected record"));
    }
    const auto materialized =
        catalog_->materializeBlob(*installed_pack_, *record_owner, entry->asset_path);
    if (!materialized) {
        return std::unexpected(QStringLiteral("Workflow document materialization failed: %1")
                                   .arg(materialized.error().message));
    }
    const auto expected_path = QDir(catalog_->blobObjectsDirectory())
                                   .filePath(QString::fromStdString(*option.document_sha256));
    const QFileInfo before(materialized->local_path);
    if (materialized->descriptor.path != entry->asset_path ||
        materialized->descriptor.sha256 != *option.document_sha256 ||
        materialized->descriptor.media_type != "application/pdf" ||
        materialized->descriptor.byte_size == 0 || !before.isFile() || before.isSymLink() ||
        before.size() < 0 ||
        static_cast<std::uint64_t>(before.size()) != materialized->descriptor.byte_size ||
        QDir::cleanPath(before.absoluteFilePath()) !=
            QDir::cleanPath(QFileInfo(expected_path).absoluteFilePath())) {
        return std::unexpected(
            QStringLiteral("Materialized workflow document has an invalid identity"));
    }

    QFile input(materialized->local_path);
    if (!input.open(QIODevice::ReadOnly)) {
        return std::unexpected(QStringLiteral("Materialized workflow document cannot be opened"));
    }
    auto bytes = input.readAll();
    const QFileInfo after(materialized->local_path);
    const auto digest = QString::fromLatin1(
        QCryptographicHash::hash(QByteArrayView(bytes), QCryptographicHash::Sha256).toHex());
    if (input.error() != QFileDevice::NoError ||
        static_cast<std::uint64_t>(bytes.size()) != materialized->descriptor.byte_size ||
        !after.isFile() || after.isSymLink() || after.size() != before.size() ||
        digest.toStdString() != *option.document_sha256) {
        return std::unexpected(
            QStringLiteral("Materialized workflow document failed exact digest verification"));
    }
    return std::optional<QByteArray>{std::move(bytes)};
}

void MainWindow::clearWorkflowFilingForm() {
    workflow_field_editors_.clear();
    workflow_service_editors_.clear();
    workflow_cure_selector_ = nullptr;
    if (workflow_filing_form_layout_ != nullptr) {
        while (workflow_filing_form_layout_->rowCount() > 0) {
            workflow_filing_form_layout_->removeRow(0);
        }
    }
    if (workflow_filing_form_ != nullptr) {
        workflow_filing_form_->setVisible(false);
    }
}

void MainWindow::rebuildWorkflowFilingForm() {
    clearWorkflowFilingForm();
    const auto option = selectedWorkflowAction();
    if (!option || !selectedCaseHasLoadedWorkflow()) {
        return;
    }
    const auto* filing = std::get_if<model::SubmitWorkflowFiling>(&option->command);
    if (filing == nullptr) {
        return;
    }

    for (const auto& field : option->required_filing_fields) {
        auto* editor = new QLineEdit(workflow_filing_form_);
        const auto field_id = QString::fromStdString(field.value);
        editor->setObjectName(QStringLiteral("workflowField.%1").arg(field_id));
        editor->setProperty("workflowFieldId", field_id);
        editor->setAccessibleName(QStringLiteral("Filing field %1").arg(field_id));
        editor->setAccessibleDescription(QStringLiteral(
            "Enter only the filing value supplied by the user; blank is submitted as omitted"));
        editor->setMaxLength(4096);
        auto* label = new QLabel(field_id, workflow_filing_form_);
        label->setBuddy(editor);
        workflow_filing_form_layout_->addRow(label, editor);
        workflow_field_editors_.push_back(WorkflowFieldEditorBinding{field, editor});
        connect(editor, &QLineEdit::textChanged, this, [this] {
            renderWorkflowActionPreview();
            updateActionStates();
        });
    }

    const auto case_index = static_cast<std::size_t>(case_list_->currentRow());
    const auto& runtime_case = runtime_pack_->cases.at(case_index);
    for (const auto& actor : runtime_case.definition.actors) {
        if (actor.id == filing->header.actor_id) {
            continue;
        }
        auto* service =
            new QCheckBox(QString::fromStdString(actor.id.value), workflow_filing_form_);
        const auto actor_id = QString::fromStdString(actor.id.value);
        service->setObjectName(QStringLiteral("workflowService.%1").arg(actor_id));
        service->setProperty("workflowServiceActorId", actor_id);
        service->setAccessibleName(QStringLiteral("Serve filing on %1").arg(actor_id));
        service->setAccessibleDescription(
            QStringLiteral("Include or omit this exact case actor from filing service"));
        service->setChecked(std::ranges::contains(filing->served_actors, actor.id));
        workflow_filing_form_layout_->addRow(QStringLiteral("Service"), service);
        workflow_service_editors_.push_back(WorkflowServiceEditorBinding{actor.id, service});
        connect(service, &QCheckBox::toggled, this, [this] {
            renderWorkflowActionPreview();
            updateActionStates();
        });
    }

    workflow_cure_selector_ = new QComboBox(workflow_filing_form_);
    workflow_cure_selector_->setObjectName(QStringLiteral("workflowCureSelector"));
    workflow_cure_selector_->setAccessibleName(QStringLiteral("Cured workflow deficiency"));
    workflow_cure_selector_->setAccessibleDescription(
        QStringLiteral("Choose an authored open deficiency cured by this filing, or none"));
    workflow_cure_selector_->addItem(QStringLiteral("No deficiency cure"), QString{});
    std::vector<const model::WorkflowDeficiencyRecord*> deficiencies;
    for (const auto& deficiency : workflow_controller_->state().deficiencies) {
        if (!deficiency.cured && deficiency.filing_type == filing->filing_type &&
            deficiency.actor_id == filing->header.actor_id) {
            deficiencies.push_back(&deficiency);
        }
    }
    std::ranges::sort(deficiencies, {}, [](const auto* deficiency) {
        return std::string_view(deficiency->deficiency_id.value);
    });
    for (const auto* deficiency : deficiencies) {
        const auto id = QString::fromStdString(deficiency->deficiency_id.value);
        workflow_cure_selector_->addItem(id, id);
    }
    if (filing->cures_deficiency_id.has_value()) {
        const auto selected = workflow_cure_selector_->findData(
            QString::fromStdString(filing->cures_deficiency_id->value), Qt::UserRole,
            Qt::MatchExactly);
        workflow_cure_selector_->setCurrentIndex(selected >= 0 ? selected : 0);
    }
    workflow_filing_form_layout_->addRow(QStringLiteral("Cures deficiency"),
                                         workflow_cure_selector_);
    connect(workflow_cure_selector_, &QComboBox::currentIndexChanged, this, [this] {
        renderWorkflowActionPreview();
        updateActionStates();
    });
    workflow_filing_form_->setVisible(true);
}

void MainWindow::rebuildWorkflowActions(
    const std::expected<WorkflowLegalClockReading, QString>& preview_reading) {
    if (!selectedCaseHasLoadedWorkflow()) {
        workflow_action_options_.clear();
        if (workflow_action_selector_ != nullptr) {
            const QSignalBlocker blocker(workflow_action_selector_);
            workflow_action_selector_->clear();
        }
        clearWorkflowFilingForm();
        return;
    }
    const auto previous_key = workflow_action_selector_->currentData(Qt::UserRole).toString();
    std::optional<model::SubmitWorkflowFiling> previous_filing;
    if (const auto previous_option = selectedWorkflowAction(); previous_option) {
        const auto previous_command = draftWorkflowCommand(*previous_option);
        if (previous_command) {
            if (const auto* filing = std::get_if<model::SubmitWorkflowFiling>(&*previous_command)) {
                previous_filing = *filing;
            }
        }
    }
    const auto case_index = static_cast<std::size_t>(case_list_->currentRow());
    const auto& runtime_case = runtime_pack_->cases.at(case_index);
    const auto fallback = preview_reading
                              ? std::optional<model::LegalTime>{workflowLegalTime(*preview_reading)}
                              : std::nullopt;
    workflow_action_options_ =
        app::eligibleWorkflowActions(runtime_case, workflow_controller_->state(), fallback);
    {
        const QSignalBlocker blocker(workflow_action_selector_);
        workflow_action_selector_->clear();
        for (const auto& option : workflow_action_options_) {
            workflow_action_selector_->addItem(option.label, option.key);
            workflow_action_selector_->setItemData(workflow_action_selector_->count() - 1,
                                                   option.description, Qt::ToolTipRole);
        }
        auto selected_index =
            previous_key.isEmpty()
                ? -1
                : workflow_action_selector_->findData(previous_key, Qt::UserRole, Qt::MatchExactly);
        if (selected_index < 0 && workflow_action_selector_->count() > 0) {
            selected_index = 0;
        }
        workflow_action_selector_->setCurrentIndex(selected_index);
    }
    rebuildWorkflowFilingForm();
    const auto refreshed = selectedWorkflowAction();
    if (previous_filing.has_value() && refreshed.has_value() && refreshed->key == previous_key) {
        for (const auto& binding : workflow_field_editors_) {
            const auto prior = std::ranges::find(previous_filing->fields, binding.field_id,
                                                 &model::WorkflowFieldValue::id);
            if (binding.editor != nullptr && prior != previous_filing->fields.end()) {
                const QSignalBlocker blocker(binding.editor);
                binding.editor->setText(QString::fromStdString(prior->value));
            }
        }
        for (const auto& binding : workflow_service_editors_) {
            if (binding.editor != nullptr) {
                const QSignalBlocker blocker(binding.editor);
                binding.editor->setChecked(
                    std::ranges::contains(previous_filing->served_actors, binding.actor_id));
            }
        }
        if (workflow_cure_selector_ != nullptr &&
            previous_filing->cures_deficiency_id.has_value()) {
            const auto cure_index = workflow_cure_selector_->findData(
                QString::fromStdString(previous_filing->cures_deficiency_id->value), Qt::UserRole,
                Qt::MatchExactly);
            if (cure_index >= 0) {
                const QSignalBlocker blocker(workflow_cure_selector_);
                workflow_cure_selector_->setCurrentIndex(cure_index);
            }
        }
    }
    renderWorkflowActionPreview();
}

void MainWindow::renderWorkflowActionPreview() {
    const auto option = selectedWorkflowAction();
    if (!option || !selectedCaseHasLoadedWorkflow()) {
        workflow_action_description_label_->setText(
            QStringLiteral("No authored workflow action is selected."));
        workflow_action_preview_label_->setText(
            QStringLiteral("Consequence preview: no currently eligible action."));
        return;
    }
    workflow_action_description_label_->setText(option->description);
    const auto command = draftWorkflowCommand(*option);
    if (!command) {
        workflow_action_preview_label_->setText(
            QStringLiteral("Consequence preview unavailable: %1").arg(command.error()));
        return;
    }
    const auto case_index = static_cast<std::size_t>(case_list_->currentRow());
    const auto& runtime_case = runtime_pack_->cases.at(case_index);
    const auto decision = engine::decideWorkflow(runtime_case.workflow, runtime_case.definition,
                                                 workflow_controller_->state(), *command);
    if (!decision) {
        workflow_action_preview_label_->setText(
            QStringLiteral("Consequence preview unavailable: %1")
                .arg(QString::fromStdString(decision.error().message)));
        return;
    }
    QStringList previews;
    for (const auto& event : *decision) {
        previews.push_back(workflowEventPreview(event));
    }
    workflow_action_preview_label_->setText(
        QStringLiteral("Authored consequence (%1): %2")
            .arg(countText(decision->size(), u"event", u"events"), previews.join(u"; ")));
}

void MainWindow::renderWorkflowStatus() {
    if (!selectedCaseHasLoadedWorkflow()) {
        invalidateWorkflowSelection();
        updateActionStates();
        return;
    }
    const auto preview_reading = sampleWorkflowLegalClock();
    rebuildWorkflowActions(preview_reading);
    const auto& state = workflow_controller_->state();
    const auto& snapshot = workflow_controller_->snapshot();
    const auto case_index = static_cast<std::size_t>(case_list_->currentRow());
    const auto& runtime_case = runtime_pack_->cases.at(case_index);
    const auto stage_has_legacy_operation =
        std::ranges::any_of(runtime_case.workflow.operations, [&](const auto& operation) {
            return operation.stage_id == state.current_stage_id &&
                   operation.allowed_legal_times.empty();
        });
    auto text = QStringLiteral("Workflow session %1 — stage %2; next event %3; %4 persisted "
                               "commands; %5 persisted events; %6 currently eligible actions.")
                    .arg(QString::fromStdString(state.session_id),
                         QString::fromStdString(state.current_stage_id.value))
                    .arg(state.next_event_sequence)
                    .arg(static_cast<qulonglong>(snapshot.commands.size()))
                    .arg(static_cast<qulonglong>(snapshot.events.size()))
                    .arg(static_cast<qulonglong>(workflow_action_options_.size()));
    if (workflow_action_options_.empty()) {
        text += QStringLiteral(" No currently eligible authored workflow action exists.");
        if (stage_has_legacy_operation && !preview_reading) {
            text += QStringLiteral(" Legacy fallback preview is unavailable (%1).")
                        .arg(preview_reading.error());
        }
    }
    workflow_status_label_->setText(text);
    updateActionStates();
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
        workspace_tabs_->setTabVisible(argument_tab_index_, false);
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
    workspace_tabs_->setTabVisible(record_tab_index_, record_matches_selection);
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
        workspace_tabs_->setTabVisible(argument_tab_index_, false);
        updateActionStates();
        return;
    }
    const auto& runtime_case = runtime_pack_->cases.at(static_cast<std::size_t>(case_row));
    const auto argument_index = static_cast<std::size_t>(row);
    if (argument_index >= runtime_case.argument_configurations.size()) {
        profile_selector_->clear();
        profile_selector_->setEnabled(false);
        workspace_tabs_->setTabEnabled(argument_tab_index_, false);
        workspace_tabs_->setTabVisible(argument_tab_index_, false);
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
    workspace_tabs_->setTabVisible(argument_tab_index_, argument_matches_selection);
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
    open_directory_action_->setEnabled(true);
    open_directory_action_->setVisible(true);
    const auto can_install_archive = catalog_ != nullptr;
    install_archive_action_->setEnabled(can_install_archive);
    install_archive_action_->setVisible(can_install_archive);
    import_profile_action_->setEnabled(true);
    import_profile_action_->setVisible(true);
    welcome_open_directory_button_->setEnabled(true);
    welcome_install_archive_button_->setEnabled(can_install_archive);
    welcome_install_archive_button_->setVisible(can_install_archive);
    welcome_import_profile_button_->setEnabled(true);
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
    open_record_button_->setEnabled(can_open_record);
    open_record_button_->setVisible(can_open_record);
    const auto can_open_workflow =
        workflow_launch_provider_ != nullptr && installed_pack_.has_value() &&
        runtime_pack_.has_value() && selected_row >= 0 &&
        static_cast<std::size_t>(selected_row) < runtime_pack_->cases.size();
    open_workflow_action_->setEnabled(can_open_workflow);
    open_workflow_action_->setVisible(can_open_workflow);
    open_workflow_button_->setEnabled(can_open_workflow);
    open_workflow_button_->setVisible(can_open_workflow);
    const auto workflow_loaded = selectedCaseHasLoadedWorkflow();
    bool can_submit_workflow = false;
    if (workflow_loaded) {
        const auto option = selectedWorkflowAction();
        if (option) {
            const auto command = draftWorkflowCommand(*option);
            if (command) {
                const auto case_index = static_cast<std::size_t>(case_list_->currentRow());
                const auto& runtime_case = runtime_pack_->cases.at(case_index);
                can_submit_workflow =
                    engine::decideWorkflow(runtime_case.workflow, runtime_case.definition,
                                           workflow_controller_->state(), *command)
                        .has_value();
            }
        }
    }
    workflow_action_workspace_->setVisible(workflow_loaded);
    workflow_action_selector_->setEnabled(workflow_loaded &&
                                          workflow_action_selector_->count() > 0);
    advance_workflow_action_->setEnabled(can_submit_workflow);
    advance_workflow_action_->setVisible(can_submit_workflow);
    advance_workflow_button_->setEnabled(can_submit_workflow);
    advance_workflow_button_->setVisible(workflow_loaded);
    bool can_open_argument = false;
    bool has_argument_configuration = false;
    if (oral_argument_launch_provider_ && installed_pack_ && runtime_pack_ && selected_row >= 0 &&
        argument_configuration_selector_->currentIndex() >= 0 &&
        static_cast<std::size_t>(selected_row) < runtime_pack_->cases.size()) {
        const auto& runtime_case = runtime_pack_->cases.at(static_cast<std::size_t>(selected_row));
        const auto argument_index =
            static_cast<std::size_t>(argument_configuration_selector_->currentIndex());
        has_argument_configuration = !runtime_case.argument_configurations.empty();
        can_open_argument = argument_index < runtime_case.argument_configurations.size() &&
                            runtime_case.argument_configurations.at(argument_index)
                                .grounded_question_bank.has_value();
    } else if (runtime_pack_ && selected_row >= 0 &&
               static_cast<std::size_t>(selected_row) < runtime_pack_->cases.size()) {
        has_argument_configuration =
            !runtime_pack_->cases.at(static_cast<std::size_t>(selected_row))
                 .argument_configurations.empty();
    }
    open_oral_argument_action_->setEnabled(can_open_argument);
    open_oral_argument_action_->setVisible(can_open_argument);
    open_oral_argument_button_->setEnabled(can_open_argument);
    open_oral_argument_button_->setVisible(can_open_argument);

    const auto has_runtime = runtime_pack_.has_value();
    case_details_tabs_->setTabVisible(overview_detail_tab_index_, has_runtime);
    case_details_tabs_->setTabVisible(workflow_detail_tab_index_, can_open_workflow);
    case_details_tabs_->setTabVisible(argument_detail_tab_index_,
                                      has_runtime && has_argument_configuration);
    case_details_tabs_->setTabVisible(profile_detail_tab_index_, has_profile || has_runtime);
    if (!case_details_tabs_->isTabVisible(case_details_tabs_->currentIndex())) {
        case_details_tabs_->setCurrentIndex(has_runtime ? overview_detail_tab_index_
                                                        : profile_detail_tab_index_);
    }

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

QTabWidget* MainWindow::caseDetailsTabs() const noexcept { return case_details_tabs_; }

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

QComboBox* MainWindow::workflowActionSelector() const noexcept { return workflow_action_selector_; }

QLabel* MainWindow::workflowActionDescriptionLabel() const noexcept {
    return workflow_action_description_label_;
}

QLabel* MainWindow::workflowActionPreviewLabel() const noexcept {
    return workflow_action_preview_label_;
}

QWidget* MainWindow::workflowFilingForm() const noexcept { return workflow_filing_form_; }

QComboBox* MainWindow::workflowCureSelector() const noexcept { return workflow_cure_selector_; }

QMenu* MainWindow::recordAccessMenu() const noexcept { return record_access_menu_; }

QString MainWindow::recordAccessDatabasePath() const { return record_access_database_path_; }

const app::WorkflowSessionController* MainWindow::workflowSessionController() const noexcept {
    return workflow_controller_.get();
}

} // namespace appellate::ui
