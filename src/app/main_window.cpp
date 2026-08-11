#include "main_window.hpp"

#include "appellate/packs/pack_catalog.hpp"
#include "appellate/packs/pack_reader.hpp"
#include "bench_profile_codec.hpp"
#include "bench_profile_editor.hpp"
#include "installed_record_controller.hpp"
#include "oral_argument_launch_provider.hpp"
#include "oral_argument_workspace.hpp"
#include "record_workspace.hpp"

#include <QAction>
#include <QComboBox>
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
#include <cstddef>
#include <expected>
#include <string_view>
#include <utility>

namespace appellate::ui {
namespace {

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
    : MainWindow(source_path, catalog_root, parent, {}) {}

MainWindow::MainWindow(const QString& source_path, const QString& catalog_root, QWidget* parent,
                       std::shared_ptr<OralArgumentLaunchProvider> oral_argument_launch_provider)
    : QMainWindow(parent),
      oral_argument_launch_provider_(std::move(oral_argument_launch_provider)) {
    setWindowTitle(QStringLiteral("Appellate Workbench"));
    resize(1180, 780);
    setMinimumSize(840, 600);
    buildUi();
    buildFileMenu();
    openCatalog(catalog_root);
    updateActionStates();

    if (!source_path.isEmpty()) {
        static_cast<void>(loadSource(source_path));
    }
}

MainWindow::~MainWindow() = default;

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
        QStringLiteral("Verify and open every record PDF from the selected installed pack"));
    open_record_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));

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
        workspace_tabs_->setCurrentIndex(browser_tab_index_);
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
    if (loaded->definition.docket.empty()) {
        return reject(QStringLiteral("Installed record contains no docket entry to open"));
    }
    const auto first_document = candidate->openDocketEntry(loaded->definition.docket.front().id);
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
    delete previous_workspace;

    record_revision_ = runtime_pack_->revision;
    record_case_id_ = selected_case.definition.id;
    workspace_tabs_->setTabEnabled(record_tab_index_, true);
    workspace_tabs_->setCurrentIndex(record_tab_index_);
    showStatus(QStringLiteral("Opened %1 verified record PDF(s) from installed storage.")
                   .arg(static_cast<qulonglong>(loaded->assets.size())));
    updateActionStates();
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
    auto candidate = std::make_unique<OralArgumentWorkspace>(std::move(*opened));
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

void MainWindow::invalidateRecordSelection() {
    record_revision_.reset();
    record_case_id_.reset();
    if (workspace_tabs_ != nullptr) {
        workspace_tabs_->setTabEnabled(record_tab_index_, false);
        workspace_tabs_->setTabText(record_tab_index_, QStringLiteral("&Record"));
        workspace_tabs_->setCurrentIndex(browser_tab_index_);
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
        workspace_tabs_->setTabEnabled(record_tab_index_, false);
        if (workspace_tabs_->currentIndex() == record_tab_index_) {
            workspace_tabs_->setCurrentIndex(browser_tab_index_);
        }
        profile_selector_->setEnabled(false);
        argument_configuration_selector_->setEnabled(false);
        workspace_tabs_->setTabEnabled(argument_tab_index_, false);
        profile_editor_->setEnabled(false);
        updateActionStates();
        return;
    }

    const auto& runtime_case = runtime_pack_->cases.at(static_cast<std::size_t>(row));
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
    const auto sealed_count = static_cast<std::size_t>(std::ranges::count(
        runtime_case.record.docket_entries, true, &packs::RuntimeDocketEntry::sealed));
    record_summary_label_->setText(QStringLiteral("Record: %1 (%2) — %3; %4 sealed")
                                       .arg(utf8(runtime_case.record.caption),
                                            utf8(runtime_case.record.id.value),
                                            countText(runtime_case.record.docket_entries.size(),
                                                      u"docket entry", u"docket entries"))
                                       .arg(static_cast<qulonglong>(sealed_count)));

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
    open_directory_action_->setEnabled(true);
    install_archive_action_->setEnabled(catalog_ != nullptr);
    import_profile_action_->setEnabled(true);
    const auto has_profile = profile_editor_->isEnabled() && profile_editor_->profile().has_value();
    clone_profile_action_->setEnabled(has_profile);
    export_profile_action_->setEnabled(has_profile);
    const auto selected_row = case_list_->currentRow();
    open_record_action_->setEnabled(
        catalog_ != nullptr && installed_pack_.has_value() && runtime_pack_.has_value() &&
        selected_row >= 0 && static_cast<std::size_t>(selected_row) < runtime_pack_->cases.size());
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

QAction* MainWindow::openOralArgumentAction() const noexcept { return open_oral_argument_action_; }

} // namespace appellate::ui
