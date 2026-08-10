#include "main_window.hpp"

#include "appellate/packs/pack_catalog.hpp"
#include "appellate/packs/pack_reader.hpp"
#include "bench_profile_codec.hpp"
#include "bench_profile_editor.hpp"

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

[[nodiscard]] auto checkedRuntime(const packs::LoadedPack& loaded)
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
    : QMainWindow(parent) {
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
    auto* container = new QWidget(this);
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
    setCentralWidget(container);

    connect(case_list_, &QListWidget::currentRowChanged, this,
            [this](int row) { updateCaseSelection(row); });
    connect(profile_selector_, &QComboBox::currentIndexChanged, this,
            [this](int row) { updateProfileSelection(row); });
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
    const auto loaded = catalog_->load(installed->revision.id, installed->revision.version);
    if (!loaded) {
        return reject(
            QStringLiteral("Installed pack could not be loaded: %1").arg(loaded.error().message));
    }
    auto runtime = checkedRuntime(*loaded);
    if (!runtime) {
        return reject(runtime.error());
    }
    commitRuntime(std::move(*runtime), absolute_path,
                  QStringLiteral("Installed and loaded pack archive."));
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

void MainWindow::commitRuntime(packs::RuntimePack runtime, const QString& source_path,
                               const QString& success_message) {
    runtime_pack_ = std::move(runtime);
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

void MainWindow::updateCaseSelection(int row) {
    if (!runtime_pack_ || row < 0 || static_cast<std::size_t>(row) >= runtime_pack_->cases.size()) {
        profile_selector_->setEnabled(false);
        profile_editor_->setEnabled(false);
        updateActionStates();
        return;
    }

    const auto& runtime_case = runtime_pack_->cases.at(static_cast<std::size_t>(row));
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
        const QSignalBlocker blocker(profile_selector_);
        profile_selector_->clear();
        for (std::size_t argument_index = 0;
             argument_index < runtime_case.argument_configurations.size(); ++argument_index) {
            const auto& argument = runtime_case.argument_configurations.at(argument_index);
            for (std::size_t seat_index = 0; seat_index < argument.bench.seats.size();
                 ++seat_index) {
                const auto& seat = argument.bench.seats.at(seat_index);
                profile_selector_->addItem(QStringLiteral("%1 — fictional/composite")
                                               .arg(utf8(seat.profile.display_name)));
                const auto item_index = profile_selector_->count() - 1;
                profile_selector_->setItemData(
                    item_index, QVariant::fromValue(static_cast<qulonglong>(argument_index)),
                    Qt::UserRole);
                profile_selector_->setItemData(
                    item_index, QVariant::fromValue(static_cast<qulonglong>(seat_index)),
                    Qt::UserRole + 1);
            }
        }
        profile_selector_->setEnabled(profile_selector_->count() > 0);
        profile_selector_->setCurrentIndex(profile_selector_->count() > 0 ? 0 : -1);
    }
    updateProfileSelection(profile_selector_->currentIndex());
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

BenchProfileEditor* MainWindow::profileEditor() const noexcept { return profile_editor_; }

QAction* MainWindow::openDirectoryAction() const noexcept { return open_directory_action_; }

QAction* MainWindow::installArchiveAction() const noexcept { return install_archive_action_; }

QAction* MainWindow::importProfileAction() const noexcept { return import_profile_action_; }

QAction* MainWindow::cloneProfileAction() const noexcept { return clone_profile_action_; }

QAction* MainWindow::exportProfileAction() const noexcept { return export_profile_action_; }

} // namespace appellate::ui
