#include "offline_self_test.hpp"

#include "appellate/model/workflow_command.hpp"
#include "appellate/packs/pack_catalog.hpp"
#include "appellate/packs/runtime_pack.hpp"
#include "appellate/storage/asset_store.hpp"
#include "local_session_provider.hpp"
#include "main_window.hpp"
#include "oral_argument_workspace.hpp"
#include "workflow_session_controller.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QUuid>
#include <QVariant>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <utility>

namespace appellate::ui {
namespace {

using namespace std::chrono_literals;

constexpr auto asterglen_pack_id = "us.ca4.rule54b.asterglen";
constexpr auto asterglen_initial_stage = "ca4r54b.stage.notice-window";
constexpr auto asterglen_advanced_stage = "ca4r54b.stage.docketed";
constexpr auto asterglen_advance_operation = "ca4r54b.operation.advance-docketed";
constexpr auto asterglen_advance_actor = "ca4r54b.actor.ca4-clerk";
constexpr auto imported_case_id = "example.case.fictional";
constexpr auto imported_argument_id = "example.argument.fictional";
constexpr auto imported_counterfactual_argument_id = "example.argument.counterfactual";
constexpr auto imported_filing_type = "example.filing.notice";
constexpr auto imported_accept_operation = "example.operation.accept-notice";
constexpr auto imported_appellant = "example.actor.appellant";
constexpr auto imported_appellee = "example.actor.appellee";
constexpr auto expected_imported_revision =
    "023008f685d42634a271a626d5df1eb770ee5a6141a1b199eaa6d9945c4f15ce";
constexpr auto expected_bundled_workflow_session =
    "workflow.session.5f62a8255168bf9cabfe35af7e09ad86d368dcbd37683cc5206010f170e8db70";
constexpr auto expected_bundled_workflow_digest =
    "20272ca1834c9738a9d40d97b882d18c6115245856a3d9737096e732fc115fbb";
constexpr auto expected_imported_workflow_session =
    "workflow.session.16a9ac9c6f55f8a2390d031e64de8f2deb23f46e34e37a5a5aa87d5e9e3a0df2";
constexpr auto expected_imported_workflow_digest =
    "8f4f7ed230d52c9ca6dee8e8781ca5a587a5a1b59882c02c81f2a16ff3e0189b";
constexpr auto expected_imported_asset_digest =
    "b45710d93705fc230515730d26e638636005779238f785bfb51dd80006673d4d";
constexpr auto expected_oral_session =
    "oral.argument.session.00ab90968780fc8513550f093a3fb02e9c681b714aeb220ee23f781167ce8991";
constexpr auto expected_imported_workflow_rows =
    "3fa96066cd77a349a9dce45041c94d3af2d5ee282dfec9aeb57e52473b1b61ed";
constexpr auto expected_oral_rows =
    "fc30b2025e94ecf1df82116776b1fe16381e82884a742ac4841b283007d29d5a";
constexpr auto expected_oral_transcript =
    "de2bfe06f60197c7584077d05142915b9dfbff815771b1265f220c51661ddb31";

[[nodiscard]] auto fail(QString message) -> std::unexpected<QString> {
    return std::unexpected(std::move(message));
}

[[nodiscard]] model::LegalTime fixedLegalTime() {
    const auto court_date =
        model::LegalDate{std::chrono::year{2026} / std::chrono::August / std::chrono::day{11}};
    return model::LegalTime{
        std::chrono::sys_seconds{std::chrono::sys_days{court_date.value}} + 11h,
        court_date,
    };
}

[[nodiscard]] QDateTime fixedUtcClock() {
    return QDateTime::fromString(QStringLiteral("2026-08-11T10:00:00Z"), Qt::ISODate);
}

[[nodiscard]] auto fixedWorkflowLegalClock(const QDate& selected_court_date)
    -> std::expected<WorkflowLegalClockReading, QString> {
    return WorkflowLegalClockReading{
        QDateTime::fromString(QStringLiteral("2026-08-11T11:00:00Z"), Qt::ISODate),
        selected_court_date};
}

[[nodiscard]] std::chrono::seconds fixedOralElapsedClock() { return 1s; }

[[nodiscard]] QString fixedOralRecordedAtClock() { return QStringLiteral("2026-08-11T12:00:00Z"); }

[[nodiscard]] QByteArray sha256(QByteArrayView value) {
    return QCryptographicHash::hash(value, QCryptographicHash::Sha256).toHex();
}

void appendUint64(QByteArray& output, std::uint64_t value) {
    std::array<char, 8> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto shift = static_cast<unsigned>((bytes.size() - index - 1U) * 8U);
        bytes.at(index) = static_cast<char>((value >> shift) & 0xffU);
    }
    output.append(bytes.data(), static_cast<qsizetype>(bytes.size()));
}

void appendFrame(QByteArray& output, QByteArrayView value) {
    appendUint64(output, static_cast<std::uint64_t>(value.size()));
    output.append(value.data(), value.size());
}

void appendFrame(QByteArray& output, const QString& value) {
    const auto utf8 = value.toUtf8();
    appendFrame(output, QByteArrayView(utf8));
}

[[nodiscard]] auto databaseRows(const QString& database_path, const QString& session_id)
    -> std::expected<QByteArray, QString> {
    struct Query final {
        QString name;
        QString sql;
    };
    const std::array queries{
        Query{
            QStringLiteral("sessions"),
            QStringLiteral("SELECT session_id, engine_revision, authority_contract, sequence, "
                           "created_at_utc FROM sessions WHERE session_id=? ORDER BY session_id")},
        Query{QStringLiteral("session_pins"),
              QStringLiteral("SELECT session_id, pack_id, version, digest FROM session_pins "
                             "WHERE session_id=? ORDER BY pack_id")},
        Query{QStringLiteral("command_log"),
              QStringLiteral("SELECT session_id, command_id, expected_sequence, payload_json, "
                             "recorded_at_utc FROM command_log WHERE session_id=? "
                             "ORDER BY expected_sequence, command_id")},
        Query{QStringLiteral("event_log"),
              QStringLiteral("SELECT session_id, sequence, event_type, payload_json, authority_id "
                             "FROM event_log WHERE session_id=? ORDER BY sequence")},
        Query{QStringLiteral("docket_projection"),
              QStringLiteral("SELECT session_id, entry_id, event_sequence, title, status FROM "
                             "docket_projection WHERE session_id=? ORDER BY event_sequence, "
                             "entry_id")},
        Query{QStringLiteral("asset_references"),
              QStringLiteral("SELECT session_id, digest, purpose FROM asset_references WHERE "
                             "session_id=? ORDER BY purpose, digest")},
    };
    const auto connection_name =
        QStringLiteral("offline-self-test-rows-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    QByteArray encoded;
    QString failure;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name);
        database.setDatabaseName(database_path);
        if (!database.open()) {
            failure = database.lastError().text();
        } else {
            appendFrame(encoded, QByteArrayView("appellate-workbench-local-session-rows-v1"));
            for (const auto& specification : queries) {
                appendFrame(encoded, specification.name);
                QSqlQuery query(database);
                query.prepare(specification.sql);
                query.addBindValue(session_id);
                if (!query.exec()) {
                    failure = query.lastError().text();
                    break;
                }
                std::uint64_t row_count{};
                QByteArray rows;
                while (query.next()) {
                    ++row_count;
                    const auto record = query.record();
                    appendUint64(rows, static_cast<std::uint64_t>(record.count()));
                    for (int column = 0; column < record.count(); ++column) {
                        const auto value = query.value(column);
                        appendUint64(rows, value.isNull() ? 0U : 1U);
                        if (value.isNull()) {
                            continue;
                        }
                        if (value.metaType().id() == QMetaType::QByteArray) {
                            appendFrame(rows, QByteArrayView(value.toByteArray()));
                        } else {
                            appendFrame(rows, value.toString());
                        }
                    }
                }
                appendUint64(encoded, row_count);
                encoded.append(rows);
            }
        }
        database.close();
        database = QSqlDatabase{};
    }
    QSqlDatabase::removeDatabase(connection_name);
    if (!failure.isEmpty()) {
        return fail(failure);
    }
    return encoded;
}

[[nodiscard]] int argumentIndex(const MainWindow& window, std::string_view id) {
    if (window.currentRuntime() == nullptr || window.currentRuntime()->cases.size() != 1) {
        return -1;
    }
    const auto& arguments = window.currentRuntime()->cases.front().argument_configurations;
    const auto found = std::ranges::find(
        arguments, id, [](const auto& argument) { return std::string_view(argument.id.value); });
    return found == arguments.end() ? -1
                                    : static_cast<int>(std::distance(arguments.begin(), found));
}

[[nodiscard]] auto exactResolved(const QString& catalog_root, const model::PackRevision& revision)
    -> std::expected<packs::ResolvedPack, QString> {
    auto catalog = packs::PackCatalog::open(catalog_root);
    if (!catalog) {
        return fail(QStringLiteral("Self-test catalog cannot be reopened: %1")
                        .arg(catalog.error().message));
    }
    auto resolved = (*catalog)->loadResolved(revision);
    if (!resolved) {
        return fail(QStringLiteral("Self-test closure cannot be exactly resolved: %1")
                        .arg(resolved.error().message));
    }
    return std::move(*resolved);
}

[[nodiscard]] QJsonObject revisionJson(const model::PackRevision& revision) {
    return QJsonObject{
        {QStringLiteral("pack_id"), QString::fromStdString(revision.id.value)},
        {QStringLiteral("version"), QString::fromStdString(revision.version)},
        {QStringLiteral("sha256"), QString::fromStdString(revision.digest)},
    };
}

} // namespace

auto runOfflineSelfTest(QApplication& application, const QString& catalog_root,
                        const QString& bundled_workflow_pack, const QString& imported_grounded_pack)
    -> std::expected<QByteArray, QString> {
    if (catalog_root.isEmpty() || bundled_workflow_pack.isEmpty() ||
        imported_grounded_pack.isEmpty()) {
        return fail(QStringLiteral(
            "Offline self-test requires explicit catalog, bundled workflow pack, and imported "
            "grounded pack paths"));
    }

    model::PackRevision bundled_revision;
    QString bundled_session_id;
    QString bundled_snapshot_digest;
    QString bundled_created_at;
    qsizetype bundled_pin_count{};
    {
        auto provider = LocalSessionProvider::fromStandardPaths(fixedUtcClock);
        if (!provider) {
            return fail(QStringLiteral("Standard-path local sessions unavailable: %1")
                            .arg(provider.error()));
        }
        MainWindow window({}, catalog_root, nullptr, *provider, {}, {}, *provider,
                          fixedWorkflowLegalClock);
        const auto loaded = window.loadSource(bundled_workflow_pack);
        if (!loaded || window.currentRuntime() == nullptr ||
            window.currentRuntime()->cases.size() != 1) {
            return fail(loaded
                            ? QStringLiteral("Bundled pack has no single runtime case")
                            : QStringLiteral("Bundled pack import failed: %1").arg(loaded.error()));
        }
        bundled_revision = window.currentRuntime()->revision;
        if (bundled_revision.id.value != asterglen_pack_id ||
            window.currentRuntime()->cases.front().workflow.initial_stage_id.value !=
                asterglen_initial_stage) {
            return fail(QStringLiteral("Bundled workflow pack identity or initial stage differs"));
        }
        window.workflowCourtDateEditor()->setText(QStringLiteral("2026-08-11"));
        window.openWorkflowButton()->click();
        application.processEvents();
        if (window.workflowSessionController() == nullptr ||
            !window.workflowStatusLabel()->text().contains(
                QString::fromLatin1(asterglen_advance_operation)) ||
            !window.workflowStatusLabel()->text().contains(
                QString::fromLatin1(asterglen_advance_actor))) {
            return fail(QStringLiteral("Bundled exact transition is not visible: %1")
                            .arg(window.errorLabel()->text()));
        }
        bundled_session_id =
            QString::fromStdString(window.workflowSessionController()->state().session_id);
        if (bundled_session_id != QString::fromLatin1(expected_bundled_workflow_session)) {
            return fail(QStringLiteral("Bundled workflow session identity framing changed"));
        }
        if (!window.advanceWorkflowButton()->isEnabled()) {
            return fail(
                QStringLiteral("Bundled transition is disabled with an explicit court date"));
        }
        window.advanceWorkflowButton()->click();
        application.processEvents();
        const auto* controller = window.workflowSessionController();
        if (controller == nullptr ||
            controller->state().current_stage_id.value != asterglen_advanced_stage ||
            controller->snapshot().commands.size() != 1 ||
            controller->snapshot().events.size() != 1) {
            return fail(QStringLiteral("Bundled workflow transition did not persist exactly once"));
        }
        bundled_created_at = controller->snapshot().created_at_utc;
        bundled_pin_count = static_cast<qsizetype>(controller->snapshot().pins.size());
        if (bundled_created_at != QStringLiteral("2026-08-11T10:00:00Z") ||
            controller->snapshot().pins.size() != 1 ||
            controller->snapshot().pins.front() !=
                storage::RevisionPin{QString::fromStdString(bundled_revision.id.value),
                                     QString::fromStdString(bundled_revision.version),
                                     QString::fromStdString(bundled_revision.digest)}) {
            return fail(QStringLiteral("Bundled workflow created-at or exact pin set changed"));
        }
        bundled_snapshot_digest = workflowLegalStateDigest(controller->snapshot());
        if (bundled_snapshot_digest != QString::fromLatin1(expected_bundled_workflow_digest)) {
            return fail(QStringLiteral("Bundled workflow snapshot framing changed: %1")
                            .arg(bundled_snapshot_digest));
        }
    }
    {
        auto provider = LocalSessionProvider::fromStandardPaths(fixedUtcClock);
        if (!provider) {
            return fail(provider.error());
        }
        MainWindow reopened({}, catalog_root, nullptr, *provider, {}, {}, *provider,
                            fixedWorkflowLegalClock);
        const auto loaded = reopened.loadSource(bundled_workflow_pack);
        if (loaded) {
            reopened.openWorkflowAction()->trigger();
            application.processEvents();
        }
        const auto* controller = reopened.workflowSessionController();
        if (!loaded || controller == nullptr ||
            QString::fromStdString(controller->state().session_id) != bundled_session_id ||
            controller->state().current_stage_id.value != asterglen_advanced_stage ||
            workflowLegalStateDigest(controller->snapshot()) != bundled_snapshot_digest) {
            return fail(loaded ? QStringLiteral("Bundled workflow did not exactly reopen")
                               : loaded.error());
        }
    }

    model::PackRevision imported_revision;
    {
        auto provider = LocalSessionProvider::fromStandardPaths(fixedUtcClock);
        if (!provider) {
            return fail(provider.error());
        }
        MainWindow importer({}, catalog_root, nullptr, *provider, {}, {}, *provider,
                            fixedWorkflowLegalClock);
        const auto loaded = importer.loadSource(imported_grounded_pack);
        if (!loaded || importer.currentRuntime() == nullptr ||
            importer.currentRuntime()->cases.size() != 1) {
            return fail(loaded ? QStringLiteral("Imported grounded pack has no single runtime case")
                               : loaded.error());
        }
        imported_revision = importer.currentRuntime()->revision;
        const auto& imported_case = importer.currentRuntime()->cases.front();
        const auto actual = std::ranges::find(
            imported_case.argument_configurations, std::string_view(imported_argument_id),
            [](const auto& argument) { return std::string_view(argument.id.value); });
        const auto counterfactual = std::ranges::find(
            imported_case.argument_configurations,
            std::string_view(imported_counterfactual_argument_id),
            [](const auto& argument) { return std::string_view(argument.id.value); });
        if (imported_revision.digest != expected_imported_revision ||
            imported_case.definition.id.value != imported_case_id ||
            imported_case.argument_configurations.size() != 2 ||
            actual == imported_case.argument_configurations.end() ||
            !actual->grounded_question_bank.has_value() ||
            actual->grounded_question_bank->mode != model::OralArgumentMode::ActualRecord ||
            counterfactual == imported_case.argument_configurations.end() ||
            !counterfactual->grounded_question_bank.has_value() ||
            counterfactual->grounded_question_bank->mode !=
                model::OralArgumentMode::CounterfactualTraining) {
            return fail(
                QStringLiteral("Explicit offline E2E import lacks the exact grounded config pair"));
        }
    }

    auto imported_resolved = exactResolved(catalog_root, imported_revision);
    if (!imported_resolved) {
        return fail(imported_resolved.error());
    }
    const auto imported_runtime = packs::loadRuntimePack(*imported_resolved);
    if (!imported_runtime || imported_runtime->cases.size() != 1) {
        return fail(QStringLiteral("Imported exact closure cannot produce its runtime case"));
    }
    const auto& imported_case = imported_runtime->cases.front();

    QString imported_workflow_session_id;
    QString imported_workflow_digest;
    QString imported_asset_digest;
    QString session_database_path;
    QString imported_workflow_created_at;
    qsizetype imported_workflow_pin_count{};
    qsizetype imported_workflow_event_count{};
    qsizetype imported_workflow_docket_count{};
    {
        auto provider = LocalSessionProvider::fromStandardPaths(fixedUtcClock);
        if (!provider) {
            return fail(provider.error());
        }
        session_database_path = (*provider)->paths().database_path;
        auto workflow = (*provider)->openWorkflow(*imported_resolved, imported_case.definition.id);
        if (!workflow) {
            return fail(QStringLiteral("Imported workflow cannot be created: %1")
                            .arg(workflow.error().message));
        }
        imported_workflow_session_id = QString::fromStdString((*workflow)->state().session_id);
        if (imported_workflow_session_id !=
                QString::fromLatin1(expected_imported_workflow_session) ||
            !(*workflow)->snapshot().commands.empty()) {
            return fail(QStringLiteral("Imported self-test workflow was not pristine"));
        }
        const QByteArray document("installed offline E2E notice of appeal bytes");
        const auto command_id = imported_workflow_session_id.toStdString() + ".command." +
                                std::to_string((*workflow)->state().next_event_sequence) + "." +
                                imported_accept_operation;
        const auto filing = model::SubmitWorkflowFiling{
            model::WorkflowCommandHeader{
                imported_workflow_session_id.toStdString(),
                model::WorkflowCommandId{command_id},
                model::ActorId{imported_appellant},
                fixedLegalTime(),
            },
            model::WorkflowFilingId{"example.filing.offline-self-test"},
            model::FilingTypeId{imported_filing_type},
            sha256(document).toStdString(),
            {model::WorkflowFieldValue{model::FilingFieldId{"example.field.caption"},
                                       "Installed offline E2E caption"}},
            {model::ActorId{imported_appellee}},
            std::nullopt,
        };
        const auto submitted =
            (*workflow)->submit(model::WorkflowCommand{filing}, QByteArrayView(document),
                                QStringLiteral("2026-08-11T11:00:00Z"));
        if (!submitted || !submitted->asset.has_value() ||
            (*workflow)->snapshot().commands.size() != 1 ||
            (*workflow)->snapshot().asset_references.size() != 1) {
            return fail(submitted ? QStringLiteral("Imported workflow/CAS counts differ")
                                  : submitted.error().message);
        }
        imported_asset_digest = submitted->asset->sha256;
        const auto stored =
            storage::AssetStore((*provider)->paths().asset_root).read(imported_asset_digest);
        if (!stored || *stored != document) {
            return fail(stored ? QStringLiteral("Imported workflow CAS bytes differ")
                               : stored.error().message);
        }
        imported_workflow_digest = workflowLegalStateDigest((*workflow)->snapshot());
        const auto& snapshot = (*workflow)->snapshot();
        imported_workflow_created_at = snapshot.created_at_utc;
        imported_workflow_pin_count = static_cast<qsizetype>(snapshot.pins.size());
        imported_workflow_event_count = static_cast<qsizetype>(snapshot.events.size());
        imported_workflow_docket_count = static_cast<qsizetype>(snapshot.docket.size());
        if (snapshot.created_at_utc != QStringLiteral("2026-08-11T10:00:00Z") ||
            snapshot.pins.size() != 1 || snapshot.events.size() != 2 ||
            snapshot.docket.size() != 2 ||
            snapshot.pins.front() !=
                storage::RevisionPin{QString::fromStdString(imported_revision.id.value),
                                     QString::fromStdString(imported_revision.version),
                                     QString::fromStdString(imported_revision.digest)}) {
            return fail(QStringLiteral("Imported workflow created-at or exact pin set changed"));
        }
        if (imported_asset_digest != QString::fromLatin1(expected_imported_asset_digest) ||
            imported_workflow_digest != QString::fromLatin1(expected_imported_workflow_digest)) {
            return fail(QStringLiteral("Imported workflow deterministic evidence changed: "
                                       "asset %1, workflow %2")
                            .arg(imported_asset_digest, imported_workflow_digest));
        }
    }
    {
        auto provider = LocalSessionProvider::fromStandardPaths(fixedUtcClock);
        if (!provider) {
            return fail(provider.error());
        }
        auto reopened = (*provider)->openWorkflow(*imported_resolved, imported_case.definition.id);
        if (!reopened ||
            QString::fromStdString((*reopened)->state().session_id) !=
                imported_workflow_session_id ||
            workflowLegalStateDigest((*reopened)->snapshot()) != imported_workflow_digest ||
            (*reopened)->snapshot().commands.size() != 1 ||
            (*reopened)->snapshot().asset_references.size() != 1) {
            return fail(reopened ? QStringLiteral("Imported workflow/CAS did not exactly reopen")
                                 : reopened.error().message);
        }
    }

    const auto workflow_rows_before_oral =
        databaseRows(session_database_path, imported_workflow_session_id);
    if (!workflow_rows_before_oral) {
        return fail(QStringLiteral("Imported workflow SQL rows cannot be framed: %1")
                        .arg(workflow_rows_before_oral.error()));
    }
    const auto imported_workflow_rows_digest =
        QString::fromLatin1(sha256(QByteArrayView(*workflow_rows_before_oral)));
    if (imported_workflow_rows_digest != QString::fromLatin1(expected_imported_workflow_rows)) {
        return fail(QStringLiteral("Imported workflow raw SQL framing changed: %1")
                        .arg(imported_workflow_rows_digest));
    }

    QString oral_transcript;
    {
        auto provider = LocalSessionProvider::fromStandardPaths(fixedUtcClock);
        if (!provider) {
            return fail(provider.error());
        }
        MainWindow oral_window({}, catalog_root, nullptr, *provider, {}, {}, *provider,
                               fixedWorkflowLegalClock, fixedOralElapsedClock,
                               fixedOralRecordedAtClock);
        const auto loaded = oral_window.loadSource(imported_grounded_pack);
        const auto index = loaded ? argumentIndex(oral_window, imported_argument_id) : -1;
        if (!loaded || index < 0) {
            return fail(loaded ? QStringLiteral("Actual grounded config is not selectable")
                               : loaded.error());
        }
        oral_window.argumentConfigurationSelector()->setCurrentIndex(index);
        oral_window.openOralArgumentAction()->trigger();
        application.processEvents();
        auto* workspace = oral_window.oralArgumentWorkspace();
        if (workspace == nullptr || !workspace->isReady()) {
            return fail(QStringLiteral("Grounded oral workspace is not ready: %1")
                            .arg(oral_window.errorLabel()->text()));
        }
        if (workspace->modeLabel()->text() != QStringLiteral("Mode: Actual record") ||
            workspace->judgeLabel()->text() !=
                QStringLiteral("Fictional/composite judge: Composite Jurist Rowan "
                               "[example.seat.presiding]") ||
            workspace->issueLabel()->text() !=
                QStringLiteral("Issue: example.issue.preservation") ||
            workspace->questionLabel()->text() !=
                QStringLiteral("Authored question example.question.preservation: Where did the "
                               "appellant preserve the claimed error?") ||
            workspace->groundingTable()->rowCount() != 3) {
            return fail(QStringLiteral("Grounded oral opening UI evidence changed"));
        }
        QStringList grounding_ids;
        for (int row = 0; row < workspace->groundingTable()->rowCount(); ++row) {
            auto* item = workspace->groundingTable()->item(row, 0);
            if (item == nullptr) {
                return fail(QStringLiteral("Grounded citation checkbox is missing"));
            }
            grounding_ids.push_back(item->data(Qt::UserRole).toString());
            item->setCheckState(Qt::Checked);
        }
        grounding_ids.sort();
        const QStringList expected_grounding_ids{
            QStringLiteral("example.grounding.preservation-authority"),
            QStringLiteral("example.grounding.preservation-brief"),
            QStringLiteral("example.grounding.preservation-record"),
        };
        if (grounding_ids != expected_grounding_ids) {
            return fail(QStringLiteral("Grounded oral exact citations changed"));
        }
        workspace->answerEditor()->setPlainText(
            QStringLiteral("Installed offline E2E grounded counsel answer"));
        workspace->submitButton()->click();
        application.processEvents();
        if (!workspace->answerEditor()->toPlainText().isEmpty() ||
            workspace->sessionState() == nullptr ||
            workspace->sessionState()->journal.size() != 2) {
            return fail(QStringLiteral("Grounded oral answer did not persist through the UI"));
        }
        oral_transcript = workspace->transcriptView()->toPlainText();
        if (!oral_transcript.contains(QStringLiteral("offline E2E grounded counsel answer"))) {
            return fail(QStringLiteral("Grounded oral transcript omitted the submitted answer"));
        }
    }

    QString oral_session_id;
    qsizetype oral_journal_size{};
    QString oral_created_at;
    qsizetype oral_pin_count{};
    {
        auto provider = LocalSessionProvider::fromStandardPaths(fixedUtcClock);
        if (!provider) {
            return fail(provider.error());
        }
        MainWindow reopened({}, catalog_root, nullptr, *provider, {}, {}, *provider,
                            fixedWorkflowLegalClock, fixedOralElapsedClock,
                            fixedOralRecordedAtClock);
        const auto loaded = reopened.loadSource(imported_grounded_pack);
        const auto index = loaded ? argumentIndex(reopened, imported_argument_id) : -1;
        if (!loaded || index < 0) {
            return fail(loaded ? QStringLiteral("Reopen grounded config is not selectable")
                               : loaded.error());
        }
        reopened.argumentConfigurationSelector()->setCurrentIndex(index);
        if (loaded && index >= 0) {
            reopened.openOralArgumentAction()->trigger();
            application.processEvents();
        }
        const auto* workspace = reopened.oralArgumentWorkspace();
        if (!loaded || workspace == nullptr || !workspace->isReady() ||
            workspace->transcriptView()->toPlainText() != oral_transcript ||
            workspace->sessionState() == nullptr) {
            return fail(loaded ? QStringLiteral("Grounded oral answer did not exactly reopen")
                               : loaded.error());
        }
        oral_journal_size = static_cast<qsizetype>(workspace->sessionState()->journal.size());
    }
    {
        auto provider = LocalSessionProvider::fromStandardPaths(fixedUtcClock);
        if (!provider) {
            return fail(provider.error());
        }
        auto controller = (*provider)->open(*imported_resolved, imported_case.definition.id,
                                            packs::RuntimeArgumentConfigId{imported_argument_id});
        if (!controller) {
            return fail(controller.error().message);
        }
        oral_session_id = (*controller)->sessionId();
        if (oral_session_id != QString::fromLatin1(expected_oral_session)) {
            return fail(QStringLiteral("Grounded oral session identity framing changed: %1")
                            .arg(oral_session_id));
        }
        oral_created_at = (*controller)->snapshot().created_at_utc;
        oral_pin_count = static_cast<qsizetype>((*controller)->snapshot().pins.size());
        if ((*controller)->state().journal.size() != 2 ||
            oral_created_at != QStringLiteral("2026-08-11T10:00:00Z") ||
            (*controller)->snapshot().pins.size() != 1 ||
            (*controller)->snapshot().pins.front() !=
                storage::RevisionPin{QString::fromStdString(imported_revision.id.value),
                                     QString::fromStdString(imported_revision.version),
                                     QString::fromStdString(imported_revision.digest)}) {
            return fail(QStringLiteral("Grounded oral snapshot evidence changed"));
        }
    }
    {
        auto provider = LocalSessionProvider::fromStandardPaths(fixedUtcClock);
        if (!provider) {
            return fail(provider.error());
        }
        auto workflow = (*provider)->openWorkflow(*imported_resolved, imported_case.definition.id);
        if (!workflow ||
            workflowLegalStateDigest((*workflow)->snapshot()) != imported_workflow_digest) {
            return fail(workflow ? QStringLiteral("Oral practice mutated the workflow snapshot")
                                 : workflow.error().message);
        }
    }

    const auto workflow_rows_after_oral =
        databaseRows(session_database_path, imported_workflow_session_id);
    if (!workflow_rows_after_oral || *workflow_rows_after_oral != *workflow_rows_before_oral) {
        return fail(workflow_rows_after_oral
                        ? QStringLiteral("Oral practice mutated exact workflow SQL rows")
                        : QStringLiteral("Workflow SQL rows cannot be reread after oral: %1")
                              .arg(workflow_rows_after_oral.error()));
    }
    const auto oral_rows = databaseRows(session_database_path, oral_session_id);
    if (!oral_rows) {
        return fail(
            QStringLiteral("Grounded oral SQL rows cannot be framed: %1").arg(oral_rows.error()));
    }
    const auto oral_rows_digest = QString::fromLatin1(sha256(QByteArrayView(*oral_rows)));
    const auto oral_transcript_digest =
        QString::fromLatin1(sha256(QByteArrayView(oral_transcript.toUtf8())));
    if (oral_rows_digest != QString::fromLatin1(expected_oral_rows) ||
        oral_transcript_digest != QString::fromLatin1(expected_oral_transcript)) {
        return fail(QStringLiteral("Grounded oral SQL or transcript framing changed: %1, %2")
                        .arg(oral_rows_digest, oral_transcript_digest));
    }

    const QJsonObject output{
        {QStringLiteral("schema_version"), 2},
        {QStringLiteral("status"), QStringLiteral("ok")},
        {QStringLiteral("network_isolation"), QStringLiteral("required-from-caller")},
        {QStringLiteral("bundled_workflow_pack"), revisionJson(bundled_revision)},
        {QStringLiteral("imported_grounded_pack"), revisionJson(imported_revision)},
        {QStringLiteral("bundled_workflow_session_id"), bundled_session_id},
        {QStringLiteral("bundled_workflow_digest"), bundled_snapshot_digest},
        {QStringLiteral("bundled_workflow_created_at_utc"), bundled_created_at},
        {QStringLiteral("bundled_workflow_pins"), static_cast<qint64>(bundled_pin_count)},
        {QStringLiteral("bundled_workflow_commands"), 1},
        {QStringLiteral("bundled_workflow_events"), 1},
        {QStringLiteral("imported_workflow_session_id"), imported_workflow_session_id},
        {QStringLiteral("imported_workflow_digest"), imported_workflow_digest},
        {QStringLiteral("imported_workflow_rows_sha256"), imported_workflow_rows_digest},
        {QStringLiteral("imported_workflow_created_at_utc"), imported_workflow_created_at},
        {QStringLiteral("imported_workflow_pins"),
         static_cast<qint64>(imported_workflow_pin_count)},
        {QStringLiteral("imported_workflow_commands"), 1},
        {QStringLiteral("imported_workflow_events"),
         static_cast<qint64>(imported_workflow_event_count)},
        {QStringLiteral("imported_workflow_docket_entries"),
         static_cast<qint64>(imported_workflow_docket_count)},
        {QStringLiteral("imported_workflow_asset_references"), 1},
        {QStringLiteral("imported_asset_sha256"), imported_asset_digest},
        {QStringLiteral("oral_session_id"), oral_session_id},
        {QStringLiteral("oral_journal_entries"), static_cast<qint64>(oral_journal_size)},
        {QStringLiteral("oral_created_at_utc"), oral_created_at},
        {QStringLiteral("oral_pins"), static_cast<qint64>(oral_pin_count)},
        {QStringLiteral("oral_rows_sha256"), oral_rows_digest},
        {QStringLiteral("oral_transcript_sha256"), oral_transcript_digest},
    };
    return QJsonDocument(output).toJson(QJsonDocument::Compact);
}

} // namespace appellate::ui
