#include "appellate/packs/pack_catalog.hpp"
#include "appellate/packs/runtime_pack.hpp"
#include "appellate/storage/asset_store.hpp"
#include "appellate/storage/session_store.hpp"
#include "appellate/storage/workflow_codec.hpp"
#include "bench_profile_editor.hpp"
#include "main_window.hpp"
#include "oral_argument_launch_provider.hpp"
#include "oral_argument_session_controller.hpp"
#include "oral_argument_workspace.hpp"
#include "record_workspace.hpp"
#include "workflow_action_planner.hpp"
#include "workflow_launch_provider.hpp"
#include "workflow_session_controller.hpp"

#include <QByteArrayView>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPdfSearchModel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <array>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef APPELLATE_CA4_RULE54B_ROOT
#error "APPELLATE_CA4_RULE54B_ROOT must name content/ca4-rule54b"
#endif

#ifndef APPELLATE_M4_FOUNDATIONS
#error "APPELLATE_M4_FOUNDATIONS must name content/foundations"
#endif

namespace {

namespace app = appellate::app;
namespace model = appellate::model;
namespace packs = appellate::packs;
namespace storage = appellate::storage;
namespace ui = appellate::ui;

constexpr auto root_digest = "7e77bc0fbe02dc9e108681df73852859d6d0f577acdcb65fcfb7678eac78b728";
constexpr auto archive_digest = "10739c149a3bf2617d8af6dd131caee7ea6639a9d97e26cdf2974fa176c82819";
constexpr qint64 archive_byte_size = 3'974'147;
constexpr auto v1_root_digest = "ff7a2e1195f9bd006e7df46c19675a3e07a4bd8975b1643a01adbc9cc4fd3424";
constexpr auto federal_digest = "866c90996c15e2076b9508a297ffce1a4e766b1432a9e11d08e8138c57e363c9";
constexpr auto ca4_digest = "449d75c77e5c47883f750377450f2d1ec1fc0e42e20b1f247446b208661d3262";
constexpr auto bench_digest = "cee0bf93309cc9ad800f215a47d734b20a9fdf5dc889f2f440e4382b942d332d";
constexpr auto workflow_session_id = "ca4r54b.session.actual-bare-certification-dismissal-mandate";
constexpr auto actual_argument_session_id = "ca4r54b.session.oral.actual-v02";
constexpr auto counterfactual_argument_session_id = "ca4r54b.session.oral.counterfactual-v02";
constexpr auto workflow_engine_revision = "appellate.realism-evidence.codec-replay-multi.v1";
constexpr auto oral_engine_revision = "engine.oral.asterglen-v02-e2e.1";

[[nodiscard]] QByteArray sha256(QByteArrayView bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

[[nodiscard]] auto commandDocumentDigest(const model::WorkflowCommand& command)
    -> std::optional<std::string> {
    return std::visit(
        [](const auto& concrete) -> std::optional<std::string> {
            if constexpr (requires { concrete.document_sha256; }) {
                return concrete.document_sha256;
            }
            return std::nullopt;
        },
        command);
}

struct FrozenWorkflowStep final {
    model::WorkflowCommand command;
    std::vector<model::WorkflowEvent> events;
    std::optional<QByteArray> document_bytes;
};

[[nodiscard]] auto loadActualTrace(const packs::RuntimeRecord& record)
    -> std::expected<std::vector<FrozenWorkflowStep>, QString> {
    const QDir root(QStringLiteral(APPELLATE_CA4_RULE54B_ROOT));
    QFile file(root.filePath(
        QStringLiteral("traces/v0.2.0/actual-bare-certification-dismissal-mandate.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        return std::unexpected(file.errorString());
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::unexpected(QStringLiteral("Cannot parse canonical Asterglen trace"));
    }
    const auto trace = document.object();
    if (trace.value(QStringLiteral("trace_id")).toString() !=
            QStringLiteral("ca4r54b.trace.actual-bare-certification-dismissal-mandate") ||
        trace.value(QStringLiteral("command_count")).toInt() != 37 ||
        trace.value(QStringLiteral("event_count")).toInt() != 40 ||
        trace.value(QStringLiteral("terminal_stage_id")).toString() !=
            QStringLiteral("ca4r54b.stage.terminated")) {
        return std::unexpected(QStringLiteral("Canonical Asterglen trace metadata drifted"));
    }

    std::vector<FrozenWorkflowStep> steps;
    for (const auto& value : trace.value(QStringLiteral("journal")).toArray()) {
        const auto command_bytes = QByteArray::fromBase64(
            value.toObject().value(QStringLiteral("command_base64")).toString().toLatin1());
        const auto command = storage::decodeWorkflowCommand(QByteArrayView(command_bytes));
        if (!command || storage::encodeWorkflowCommand(*command) != command_bytes) {
            return std::unexpected(QStringLiteral("Canonical trace command cannot round-trip"));
        }
        const auto session_matches = std::visit(
            [](const auto& concrete) { return concrete.header.session_id == workflow_session_id; },
            *command);
        if (!session_matches) {
            return std::unexpected(QStringLiteral("Trace session identity drifted"));
        }
        std::vector<model::WorkflowEvent> events;
        for (const auto& encoded :
             value.toObject().value(QStringLiteral("events_base64")).toArray()) {
            const auto event_bytes = QByteArray::fromBase64(encoded.toString().toLatin1());
            const auto event = storage::decodeWorkflowEvent(QByteArrayView(event_bytes));
            if (!event || storage::encodeWorkflowEvent(*event) != event_bytes) {
                return std::unexpected(QStringLiteral("Canonical trace event cannot round-trip"));
            }
            events.push_back(*event);
        }
        if (events.empty()) {
            return std::unexpected(QStringLiteral("Canonical trace command has no event"));
        }

        std::optional<QByteArray> asset_bytes;
        if (const auto digest = commandDocumentDigest(*command); digest.has_value()) {
            const auto entry = std::ranges::find(record.docket_entries, *digest,
                                                 &packs::RuntimeDocketEntry::asset_sha256);
            if (entry == record.docket_entries.end()) {
                return std::unexpected(QStringLiteral("Trace document is absent from record"));
            }
            QFile asset(root.filePath(
                QStringLiteral("pack-v0.2.0/%1").arg(QString::fromStdString(entry->asset_path))));
            if (!asset.open(QIODevice::ReadOnly)) {
                return std::unexpected(asset.errorString());
            }
            asset_bytes = asset.readAll();
            if (sha256(QByteArrayView(*asset_bytes)).toStdString() != *digest) {
                return std::unexpected(QStringLiteral("Trace document bytes have wrong digest"));
            }
        }
        steps.push_back(FrozenWorkflowStep{*command, std::move(events), std::move(asset_bytes)});
    }
    return steps;
}

[[nodiscard]] model::WorkflowState initialWorkflowState(const packs::RuntimeCase& runtime_case) {
    model::WorkflowState state;
    state.session_id = workflow_session_id;
    state.workflow_id = runtime_case.workflow.id;
    state.current_stage_id = runtime_case.workflow.initial_stage_id;
    return state;
}

class PersistedWorkflowProvider final : public ui::WorkflowLaunchProvider {
  public:
    PersistedWorkflowProvider(model::WorkflowState initial, model::PackRevision expected_revision,
                              QString asset_root, std::unique_ptr<storage::SessionStore> store)
        : initial_(std::move(initial)), expected_revision_(std::move(expected_revision)),
          asset_root_(std::move(asset_root)), store_(std::move(store)) {}

    [[nodiscard]] auto openWorkflow(const packs::ResolvedPack& resolved,
                                    const model::CaseId& case_id)
        -> std::expected<std::unique_ptr<app::WorkflowSessionController>,
                         app::WorkflowSessionError> override {
        if (resolved.root().revision != expected_revision_ ||
            case_id != model::CaseId{"ca4r54b.case.asterglen"} ||
            resolved.resourceOwner(initial_.workflow_id.value) !=
                std::optional<model::PackRevision>{expected_revision_}) {
            return std::unexpected(app::WorkflowSessionError{
                app::WorkflowSessionErrorCode::InvalidConfiguration,
                QStringLiteral("Workflow launch lost exact Asterglen v0.2 ownership")});
        }
        auto connection = store_->forkConnection();
        if (!connection) {
            return std::unexpected(app::WorkflowSessionError{
                app::WorkflowSessionErrorCode::SessionStoreFailure, connection.error().message});
        }
        ++open_attempts_;
        return app::WorkflowSessionController::reopen(
            case_id, initial_, storage::AssetStore(asset_root_), std::move(*connection),
            QString::fromLatin1(workflow_engine_revision), resolved);
    }

    [[nodiscard]] int openAttempts() const noexcept { return open_attempts_; }

  private:
    model::WorkflowState initial_;
    model::PackRevision expected_revision_;
    QString asset_root_;
    std::unique_ptr<storage::SessionStore> store_;
    int open_attempts_{};
};

class PersistedLaunchProvider final : public ui::OralArgumentLaunchProvider {
  public:
    PersistedLaunchProvider(std::string legal_state_digest, model::PackRevision expected_revision,
                            std::unique_ptr<storage::SessionStore> store)
        : legal_state_digest_(std::move(legal_state_digest)),
          expected_revision_(std::move(expected_revision)), store_(std::move(store)) {}

    [[nodiscard]] auto open(const packs::ResolvedPack& resolved_pack, const model::CaseId& case_id,
                            const packs::RuntimeArgumentConfigId& configuration_id)
        -> std::expected<std::unique_ptr<app::OralArgumentSessionController>,
                         app::OralArgumentSessionError> override {
        if (resolved_pack.root().revision != expected_revision_ ||
            resolved_pack.resourceOwner(configuration_id.value) !=
                std::optional<model::PackRevision>{expected_revision_}) {
            return failure(QStringLiteral("Oral launch lost exact v0.2 ownership"));
        }
        QString session_id;
        if (configuration_id.value == "ca4r54b.argument.actual-record") {
            session_id = QString::fromLatin1(actual_argument_session_id);
        } else if (configuration_id.value == "ca4r54b.argument.counterfactual") {
            session_id = QString::fromLatin1(counterfactual_argument_session_id);
        } else {
            return failure(QStringLiteral("Unknown Asterglen argument configuration"));
        }
        auto connection = store_->forkConnection();
        if (!connection) {
            return failure(connection.error().message,
                           app::OralArgumentSessionErrorCode::SessionStoreFailure);
        }
        const auto existing = (*connection)->loadSession(session_id);
        if (!existing && existing.error().code != storage::StoreErrorCode::NotFound) {
            return failure(existing.error().message,
                           app::OralArgumentSessionErrorCode::SessionStoreFailure);
        }
        if (existing) {
            ++reopen_attempts_;
            return app::OralArgumentSessionController::reopen(
                session_id, case_id, configuration_id, legal_state_digest_, std::move(*connection),
                QString::fromLatin1(oral_engine_revision), resolved_pack);
        }
        ++create_attempts_;
        return app::OralArgumentSessionController::create(
            session_id, case_id, configuration_id, legal_state_digest_, std::move(*connection),
            QString::fromLatin1(oral_engine_revision), QStringLiteral("2026-08-12T18:30:00Z"),
            resolved_pack);
    }

    [[nodiscard]] auto forkConnection()
        -> std::expected<std::unique_ptr<storage::SessionStore>, storage::StoreError> {
        return store_->forkConnection();
    }
    [[nodiscard]] int createAttempts() const noexcept { return create_attempts_; }
    [[nodiscard]] int reopenAttempts() const noexcept { return reopen_attempts_; }

  private:
    [[nodiscard]] static auto failure(QString message,
                                      app::OralArgumentSessionErrorCode code =
                                          app::OralArgumentSessionErrorCode::InvalidConfiguration)
        -> std::unexpected<app::OralArgumentSessionError> {
        return std::unexpected(app::OralArgumentSessionError{code, std::move(message)});
    }

    std::string legal_state_digest_;
    model::PackRevision expected_revision_;
    std::unique_ptr<storage::SessionStore> store_;
    int create_attempts_{};
    int reopen_attempts_{};
};

[[nodiscard]] int configurationIndex(const ui::MainWindow& window, std::string_view id) {
    if (window.currentRuntime() == nullptr || window.currentRuntime()->cases.size() != 1) {
        return -1;
    }
    const auto& configurations = window.currentRuntime()->cases.front().argument_configurations;
    const auto found = std::ranges::find(configurations, id, [](const auto& configuration) {
        return std::string_view(configuration.id.value);
    });
    return found == configurations.end()
               ? -1
               : static_cast<int>(std::distance(configurations.begin(), found));
}

void submitGroundedAnswer(ui::OralArgumentWorkspace& workspace, const QString& answer) {
    QVERIFY(workspace.isReady());
    QVERIFY(workspace.groundingTable()->rowCount() > 0);
    for (int row = 0; row < workspace.groundingTable()->rowCount(); ++row) {
        workspace.groundingTable()->item(row, 0)->setCheckState(Qt::Checked);
    }
    workspace.answerKindSelector()->setCurrentIndex(2);
    workspace.answerEditor()->setPlainText(answer);
    workspace.window()->activateWindow();
    workspace.answerEditor()->setFocus();
    QTRY_VERIFY(workspace.answerEditor()->hasFocus());
    QTest::keyClick(workspace.answerEditor(), Qt::Key_Return, Qt::ControlModifier);
    QTRY_VERIFY(workspace.answerEditor()->toPlainText().isEmpty());
    QCOMPARE(workspace.sessionState()->journal.size(), std::size_t{2});
}

class AsterglenRule54bV02UiE2eTest final : public QObject {
    Q_OBJECT

  private slots:
    void coinstallsReplaysAndExercisesActualAndBranchWorkspaces();
};

void AsterglenRule54bV02UiE2eTest::coinstallsReplaysAndExercisesActualAndBranchWorkspaces() {
    const model::PackRevision expected_root{model::PackId{"us.ca4.rule54b.asterglen"}, "0.2.0",
                                            root_digest};
    const model::PackRevision expected_v1{model::PackId{"us.ca4.rule54b.asterglen"}, "0.1.0",
                                          v1_root_digest};
    const model::PackRevision expected_federal{model::PackId{"foundation.us-federal"}, "2025.12.01",
                                               federal_digest};
    const model::PackRevision expected_ca4{model::PackId{"foundation.us-ca4"}, "2026.03.23",
                                           ca4_digest};
    const model::PackRevision expected_bench{model::PackId{"foundation.us-ca4-fictional-bench"},
                                             "1.0.0", bench_digest};
    const QDir root(QStringLiteral(APPELLATE_CA4_RULE54B_ROOT));
    const QDir foundations(QStringLiteral(APPELLATE_M4_FOUNDATIONS));
    const auto v1_archive = root.filePath(QStringLiteral("us-ca4-rule54b-asterglen-0.1.0.awpack"));
    const auto v2_archive = root.filePath(QStringLiteral("us-ca4-rule54b-asterglen-0.2.0.awpack"));
    QFile frozen_v2(v2_archive);
    QVERIFY2(frozen_v2.open(QIODevice::ReadOnly), qPrintable(frozen_v2.errorString()));
    QCOMPARE(frozen_v2.size(), archive_byte_size);
    QCOMPARE(sha256(QByteArrayView(frozen_v2.readAll())), QByteArray(archive_digest));

    QTemporaryDir state;
    QVERIFY(state.isValid());
    const auto catalog_root = QDir(state.path()).filePath(QStringLiteral("catalog"));
    const auto database_path = QDir(state.path()).filePath(QStringLiteral("sessions.sqlite"));
    const auto asset_root = QDir(state.path()).filePath(QStringLiteral("workflow-assets"));
    const auto catalog = packs::PackCatalog::open(catalog_root);
    QVERIFY2(catalog.has_value(), catalog ? "" : qPrintable(catalog.error().message));
    const auto federal = (*catalog)->installArchive(
        foundations.filePath(QStringLiteral("us-federal/foundation-us-federal-2025.12.01.awpack")),
        QStringLiteral("2026-08-12T00:00:00Z"));
    const auto ca4 = (*catalog)->installArchive(
        foundations.filePath(QStringLiteral("us-ca4/foundation-us-ca4-2026.03.23.awpack")),
        QStringLiteral("2026-08-12T00:00:01Z"));
    const auto bench = (*catalog)->installArchive(
        foundations.filePath(QStringLiteral(
            "us-ca4-fictional-bench/foundation-us-ca4-fictional-bench-1.0.0.awpack")),
        QStringLiteral("2026-08-12T00:00:02Z"));
    const auto installed_v1 =
        (*catalog)->installArchive(v1_archive, QStringLiteral("2026-08-12T00:00:03Z"));
    const auto installed_v2 =
        (*catalog)->installArchive(v2_archive, QStringLiteral("2026-08-12T00:00:04Z"));
    QVERIFY(federal.has_value());
    QVERIFY(ca4.has_value());
    QVERIFY(bench.has_value());
    QVERIFY(installed_v1.has_value());
    QVERIFY(installed_v2.has_value());
    QCOMPARE(federal->revision, expected_federal);
    QCOMPARE(ca4->revision, expected_ca4);
    QCOMPARE(bench->revision, expected_bench);
    QCOMPARE(installed_v1->revision, expected_v1);
    QCOMPARE(installed_v2->revision, expected_root);
    QCOMPARE(installed_v2->archive_sha256, QString::fromLatin1(archive_digest));
    const auto listed = (*catalog)->list();
    QVERIFY(listed.has_value());
    QCOMPARE(std::ranges::count_if(*listed,
                                   [](const auto& pack) {
                                       return pack.revision.id.value == "us.ca4.rule54b.asterglen";
                                   }),
             2);

    const auto resolved = (*catalog)->loadResolved(expected_root);
    QVERIFY2(resolved.has_value(), resolved ? "" : qPrintable(resolved.error().message));
    const auto runtime = packs::loadRuntimePack(*resolved);
    QVERIFY2(runtime.has_value(), runtime ? "" : runtime.error().message.c_str());
    QCOMPARE(runtime->cases.size(), std::size_t{1});
    const auto& runtime_case = runtime->cases.front();
    QCOMPARE(runtime_case.record.dockets.size(), std::size_t{4});
    QCOMPARE(runtime_case.record.docket_entries.size(), std::size_t{75});
    QCOMPARE(runtime_case.record.page_anchors.size(), std::size_t{377});
    QCOMPARE(runtime_case.workflow.stages.size(), std::size_t{13});
    QCOMPARE(runtime_case.workflow.operations.size(), std::size_t{81});
    QCOMPARE(runtime_case.workflow.filing_routes.size(), std::size_t{11});
    QCOMPARE(runtime_case.definition.disposition_plans.size(), std::size_t{3});
    const auto initial = initialWorkflowState(runtime_case);
    const auto actual_trace = loadActualTrace(runtime_case.record);
    QVERIFY2(actual_trace.has_value(), actual_trace ? "" : qPrintable(actual_trace.error()));
    QCOMPARE(actual_trace->size(), std::size_t{37});
    auto workflow_store = storage::SessionStore::open(database_path);
    QVERIFY2(workflow_store.has_value(),
             workflow_store ? "" : qPrintable(workflow_store.error().message));
    auto workflow = app::WorkflowSessionController::create(
        runtime_case.definition.id, initial, storage::AssetStore(asset_root),
        std::move(*workflow_store), QString::fromLatin1(workflow_engine_revision),
        QStringLiteral("2026-08-12T18:31:00Z"), *resolved);
    QVERIFY2(workflow.has_value(), workflow ? "" : qPrintable(workflow.error().message));
    QVERIFY(actual_trace->front().document_bytes.has_value());
    auto wrong_bytes = *actual_trace->front().document_bytes;
    wrong_bytes[0] = static_cast<char>(wrong_bytes.at(0) ^ 0x01);
    const auto wrong_document =
        (*workflow)->submit(actual_trace->front().command, QByteArrayView(wrong_bytes),
                            QStringLiteral("2026-08-12T18:31:30Z"));
    QVERIFY(!wrong_document.has_value());
    QCOMPARE(wrong_document.error().code, app::WorkflowSessionErrorCode::DocumentDigestMismatch);
    QCOMPARE((*workflow)->state(), initial);
    QVERIFY((*workflow)->journal().empty());
    (*workflow).reset();

    model::WorkflowState workflow_state_before;
    std::vector<model::WorkflowJournalEntry> workflow_journal_before;
    {
        auto workflow_provider_store = storage::SessionStore::open(database_path);
        QVERIFY2(workflow_provider_store.has_value(),
                 workflow_provider_store ? ""
                                         : qPrintable(workflow_provider_store.error().message));
        auto workflow_provider = std::make_shared<PersistedWorkflowProvider>(
            initial, expected_root, asset_root, std::move(*workflow_provider_store));
        int recorded_at_index = 0;
        const ui::WorkflowLegalClock legal_clock =
            [](const QDate& court_date) -> std::expected<ui::WorkflowLegalClockReading, QString> {
            return ui::WorkflowLegalClockReading{
                QDateTime::fromString(QStringLiteral("2026-02-04T12:00:00Z"), Qt::ISODate),
                court_date};
        };
        const ui::WorkflowRecordedAtClock recorded_at_clock = [&recorded_at_index] {
            return QStringLiteral("2026-08-12T19:%1:00Z")
                .arg(recorded_at_index++, 2, 10, QLatin1Char('0'));
        };
        const auto open_workflow_window =
            [&]() -> std::expected<std::unique_ptr<ui::MainWindow>, QString> {
            auto candidate = std::make_unique<ui::MainWindow>(
                QString{}, catalog_root, nullptr, std::shared_ptr<ui::OralArgumentLaunchProvider>{},
                std::shared_ptr<ui::RecordAccessTransitionProvider>{}, QString{}, workflow_provider,
                legal_clock, ui::OralElapsedClock{}, ui::OralRecordedAtClock{}, recorded_at_clock);
            const auto loaded = candidate->loadSource(v2_archive);
            if (!loaded) {
                return std::unexpected(loaded.error());
            }
            candidate->show();
            if (!QTest::qWaitForWindowExposed(candidate.get())) {
                return std::unexpected(QStringLiteral("Workflow window was not exposed"));
            }
            candidate->workflowCourtDateEditor()->setText(QStringLiteral("2026-02-04"));
            candidate->caseDetailsTabs()->setCurrentIndex(1);
            const auto opened = candidate->openSelectedWorkflow();
            if (!opened) {
                return std::unexpected(opened.error());
            }
            return candidate;
        };

        auto opened_window = open_workflow_window();
        QVERIFY2(opened_window.has_value(), opened_window ? "" : qPrintable(opened_window.error()));
        auto window = std::move(*opened_window);
        QCOMPARE(window->workflowSessionController()->state(), initial);
        int filing_submissions = 0;
        int direct_submissions = 0;
        int rejected_filings = 0;

        for (std::size_t index = 0; index < actual_trace->size(); ++index) {
            const auto& step = actual_trace->at(index);
            const auto expected_action_key = app::workflowActionKey(step.command);
            auto* selector = window->workflowActionSelector();
            const auto action_index =
                selector->findData(expected_action_key, Qt::UserRole, Qt::MatchExactly);
            QVERIFY2(action_index >= 0, qPrintable(expected_action_key));
            selector->setCurrentIndex(action_index);
            QCOMPARE(selector->currentData(Qt::UserRole).toString(), expected_action_key);

            if (const auto* filing = std::get_if<model::SubmitWorkflowFiling>(&step.command)) {
                ++filing_submissions;
                QVERIFY(window->workflowFilingForm()->isVisible());
                for (const auto& field : filing->fields) {
                    const auto field_id = QString::fromStdString(field.id.value);
                    auto* editor = window->workflowFilingForm()->findChild<QLineEdit*>(
                        QStringLiteral("workflowField.%1").arg(field_id));
                    QVERIFY2(editor != nullptr, qPrintable(field_id));
                    editor->setText(QString::fromStdString(field.value));
                }
                for (auto* service : window->workflowFilingForm()->findChildren<QCheckBox*>()) {
                    service->setChecked(false);
                }
                for (const auto& actor : filing->served_actors) {
                    const auto actor_id = QString::fromStdString(actor.value);
                    auto* service = window->workflowFilingForm()->findChild<QCheckBox*>(
                        QStringLiteral("workflowService.%1").arg(actor_id));
                    QVERIFY2(service != nullptr, qPrintable(actor_id));
                    service->setChecked(true);
                }
                auto* cure_selector = window->workflowCureSelector();
                QVERIFY(cure_selector != nullptr);
                const auto cure = filing->cures_deficiency_id.has_value()
                                      ? QString::fromStdString(filing->cures_deficiency_id->value)
                                      : QString{};
                const auto cure_index =
                    cure_selector->findData(cure, Qt::UserRole, Qt::MatchExactly);
                QVERIFY2(cure_index >= 0, qPrintable(cure));
                cure_selector->setCurrentIndex(cure_index);
            } else {
                ++direct_submissions;
            }

            QVERIFY(window->workflowActionPreviewLabel()->text().startsWith(
                QStringLiteral("Authored consequence")));
            QVERIFY(window->advanceWorkflowButton()->isEnabled());
            const auto commands_before = window->workflowSessionController()->journal().size();
            if (index == 0) {
                selector->setFocus();
                QTest::keyClick(selector, Qt::Key_Return, Qt::ControlModifier);
            } else {
                QTest::mouseClick(window->advanceWorkflowButton(), Qt::LeftButton);
            }
            QCOMPARE(window->workflowSessionController()->journal().size(), commands_before + 1);
            const auto& actual_entry = window->workflowSessionController()->journal().back();
            auto normalized_command = actual_entry.command;
            const auto oracle_command_id = std::visit(
                [](const auto& command) { return command.header.command_id; }, step.command);
            std::visit([&](auto& command) { command.header.command_id = oracle_command_id; },
                       normalized_command);
            QCOMPARE(normalized_command, step.command);

            auto normalized_events = actual_entry.events;
            for (auto& event : normalized_events) {
                std::visit([&](auto& concrete) { concrete.header.command_id = oracle_command_id; },
                           event);
            }
            QCOMPARE(normalized_events, step.events);
            rejected_filings +=
                static_cast<int>(std::ranges::count_if(actual_entry.events, [](const auto& event) {
                    return std::holds_alternative<model::WorkflowFilingRejected>(event);
                }));

            if (index == 3 || index == 17) {
                const auto persisted_state = window->workflowSessionController()->state();
                const auto persisted_journal = window->workflowSessionController()->journal();
                window.reset();
                opened_window = open_workflow_window();
                QVERIFY2(opened_window.has_value(),
                         opened_window ? "" : qPrintable(opened_window.error()));
                window = std::move(*opened_window);
                QCOMPARE(window->workflowSessionController()->state(), persisted_state);
                QCOMPARE(window->workflowSessionController()->journal(), persisted_journal);
            }
        }
        QCOMPARE(filing_submissions, 14);
        QCOMPARE(direct_submissions, 23);
        QCOMPARE(rejected_filings, 6);
        QCOMPARE(workflow_provider->openAttempts(), 3);
        QCOMPARE(window->workflowSessionController()->state().current_stage_id,
                 model::WorkflowStageId{"ca4r54b.stage.terminated"});
        QVERIFY(window->workflowSessionController()->state().judgment_disposition.has_value());
        const auto* disposition = std::get_if<model::DispositionPlan>(
            &*window->workflowSessionController()->state().judgment_disposition);
        QVERIFY(disposition != nullptr);
        QCOMPARE(disposition->id,
                 model::DispositionPlanId{"ca4r54b.disposition.authored-dismissal"});
        QCOMPARE(window->workflowSessionController()->journal().size(), std::size_t{37});
        QCOMPARE(window->workflowSessionController()->snapshot().sequence, qint64{40});
        QCOMPARE(window->workflowSessionController()->snapshot().commands.size(), std::size_t{37});
        QCOMPARE(window->workflowSessionController()->snapshot().events.size(), std::size_t{40});
        QCOMPARE(window->workflowSessionController()->snapshot().asset_references.size(),
                 std::size_t{17});
        workflow_state_before = window->workflowSessionController()->state();
        workflow_journal_before = window->workflowSessionController()->journal();
    }
    const std::string legal_state_digest =
        "da2038d9c6d3cc486af66db69b4eeea17de497685856290a439f81bfc0efd715";

    auto provider_store = storage::SessionStore::open(database_path);
    QVERIFY2(provider_store.has_value(),
             provider_store ? "" : qPrintable(provider_store.error().message));
    auto provider = std::make_shared<PersistedLaunchProvider>(legal_state_digest, expected_root,
                                                              std::move(*provider_store));
    std::optional<model::OralArgumentState> actual_argument_state;
    std::optional<model::OralArgumentState> counterfactual_argument_state;

    {
        ui::MainWindow window({}, catalog_root, nullptr, provider);
        const auto loaded = window.loadSource(v2_archive);
        QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error()));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.currentRuntime() != nullptr);
        QCOMPARE(window.currentRuntime()->revision, expected_root);
        QCOMPARE(window.caseList()->count(), 1);
        QCOMPARE(window.argumentConfigurationSelector()->count(), 2);
        QCOMPARE(window.currentRuntime()->cases.front().definition.disposition_plans.size(),
                 std::size_t{3});

        const auto actual_index = configurationIndex(window, "ca4r54b.argument.actual-record");
        const auto counterfactual_index =
            configurationIndex(window, "ca4r54b.argument.counterfactual");
        QVERIFY(actual_index >= 0);
        QVERIFY(counterfactual_index >= 0);
        window.argumentConfigurationSelector()->setCurrentIndex(actual_index);
        QCOMPARE(window.profileSelector()->count(), 3);
        const std::vector<std::string> expected_profiles{"us.ca4.bench-profile.vale",
                                                         "us.ca4.bench-profile.rowan",
                                                         "us.ca4.bench-profile.alder"};
        for (int index = 0; index < window.profileSelector()->count(); ++index) {
            window.profileSelector()->setCurrentIndex(index);
            QVERIFY(window.profileEditor()->profile().has_value());
            QCOMPARE(window.profileEditor()->profile()->id,
                     expected_profiles.at(static_cast<std::size_t>(index)));
        }
        QVERIFY(window.openSelectedOralArgument().has_value());
        QTRY_VERIFY(window.oralArgumentWorkspace()->isReady());
        QCOMPARE(window.oralArgumentWorkspace()
                     ->canonicalDefinition()
                     ->question_bank.argument_configuration_id,
                 std::string("ca4r54b.argument.actual-record"));
        QCOMPARE(
            window.oralArgumentWorkspace()->canonicalDefinition()->question_bank.questions.size(),
            std::size_t{15});
        QCOMPARE(window.oralArgumentWorkspace()
                     ->canonicalDefinition()
                     ->configuration.authored_disposition_id,
                 std::string("ca4r54b.disposition.authored-dismissal"));
        submitGroundedAnswer(
            *window.oralArgumentWorkspace(),
            QStringLiteral("The actual JA and PA record supports dismissal and remand only."));
        actual_argument_state = *window.oralArgumentWorkspace()->sessionState();

        window.argumentConfigurationSelector()->setCurrentIndex(counterfactual_index);
        QVERIFY(window.openSelectedOralArgument().has_value());
        QTRY_VERIFY(window.oralArgumentWorkspace()->isReady());
        QVERIFY(window.oralArgumentWorkspace()->isolationNoticeLabel()->text().contains(
            QStringLiteral("isolated from the actual-record workflow")));
        QCOMPARE(window.oralArgumentWorkspace()
                     ->canonicalDefinition()
                     ->question_bank.argument_configuration_id,
                 std::string("ca4r54b.argument.counterfactual"));
        QCOMPARE(
            window.oralArgumentWorkspace()->canonicalDefinition()->question_bank.questions.size(),
            std::size_t{16});
        submitGroundedAnswer(
            *window.oralArgumentWorkspace(),
            QStringLiteral("The supported-certification branch remains isolated from actuality."));
        counterfactual_argument_state = *window.oralArgumentWorkspace()->sessionState();

        const auto opened_record = window.openSelectedRecord();
        QVERIFY2(opened_record.has_value(), opened_record ? "" : qPrintable(opened_record.error()));
        auto* workspace = window.recordWorkspace();
        QVERIFY(workspace != nullptr);
        auto* pdf_search = workspace->findChild<QPdfSearchModel*>();
        QVERIFY(pdf_search != nullptr);
        QCOMPARE(workspace->visibleDocketCount(), qsizetype{75});
        const std::array docket_filters{
            std::pair{QStringLiteral("ca4r54b.docket.ndwv"), qsizetype{37}},
            std::pair{QStringLiteral("ca4r54b.docket.ca4-v2"), qsizetype{13}},
            std::pair{QStringLiteral("ca4r54b.docket.counterfactual-district"), qsizetype{5}},
            std::pair{QStringLiteral("ca4r54b.docket.counterfactual-appellate"), qsizetype{20}},
        };
        for (const auto& [filter, count] : docket_filters) {
            workspace->setDocketFilter(filter);
            QCOMPARE(workspace->visibleDocketCount(), count);
        }
        workspace->setDocketFilter(QStringLiteral("actual_appellate_docket"));
        QCOMPARE(workspace->visibleDocketCount(), qsizetype{13});
        workspace->setDocketFilter(QStringLiteral("never_occurred_on_actual_docket"));
        QCOMPARE(workspace->visibleDocketCount(), qsizetype{25});
        workspace->setDocketFilter({});

        struct Navigation final {
            QString citation;
            QString entry_id;
            int pages{};
            int page_index{};
            QString search;
        };
        const std::array navigations{
            Navigation{QStringLiteral("JA216"), QStringLiteral("ca4r54b.record.entry.l32"), 1, 0,
                       QStringLiteral("motion is GRANTED")},
            Navigation{QStringLiteral("PA54"), QStringLiteral("ca4r54b.record.entry.a10"), 12, 0,
                       QStringLiteral("no just reason for delay")},
            Navigation{QStringLiteral("PA81"), QStringLiteral("ca4r54b.record.entry.b04"), 12, 0,
                       QStringLiteral("fifteen-day cure opportunity")},
            Navigation{QStringLiteral("PA143"), QStringLiteral("ca4r54b.record.entry.b25"), 2, 1,
                       QStringLiteral("each side to bear its own appellate costs")},
        };
        for (const auto& navigation : navigations) {
            const auto navigated = workspace->navigateToCitation(navigation.citation);
            QVERIFY2(navigated.has_value(), navigated ? "" : qPrintable(navigated.error().message));
            QCOMPARE(workspace->currentDocumentId(), navigation.entry_id);
            QCOMPARE(workspace->loadedPageCount(), navigation.pages);
            QTRY_COMPARE(workspace->currentPageIndex(), navigation.page_index);
            workspace->setDocumentSearch(navigation.search);
            QTRY_VERIFY_WITH_TIMEOUT(workspace->documentSearchResultCount() > 0, 10'000);
            QTRY_VERIFY_WITH_TIMEOUT(
                !pdf_search->resultsOnPage(workspace->currentPageIndex()).isEmpty(), 10'000);
        }
    }

    {
        ui::MainWindow window({}, catalog_root, nullptr, provider);
        QVERIFY(window.loadSource(v2_archive).has_value());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const auto counterfactual_index =
            configurationIndex(window, "ca4r54b.argument.counterfactual");
        window.argumentConfigurationSelector()->setCurrentIndex(counterfactual_index);
        QVERIFY(window.openSelectedOralArgument().has_value());
        QTRY_VERIFY(window.oralArgumentWorkspace()->isReady());
        QCOMPARE(*window.oralArgumentWorkspace()->sessionState(), *counterfactual_argument_state);
        const auto actual_index = configurationIndex(window, "ca4r54b.argument.actual-record");
        window.argumentConfigurationSelector()->setCurrentIndex(actual_index);
        QVERIFY(window.openSelectedOralArgument().has_value());
        QTRY_VERIFY(window.oralArgumentWorkspace()->isReady());
        QCOMPARE(*window.oralArgumentWorkspace()->sessionState(), *actual_argument_state);
    }

    auto verification_store = provider->forkConnection();
    QVERIFY(verification_store.has_value());
    const auto actual_snapshot =
        (*verification_store)->loadSession(QString::fromLatin1(actual_argument_session_id));
    const auto counterfactual_snapshot =
        (*verification_store)->loadSession(QString::fromLatin1(counterfactual_argument_session_id));
    QVERIFY(actual_snapshot.has_value());
    QVERIFY(counterfactual_snapshot.has_value());
    QCOMPARE(actual_snapshot->sequence, qint64{2});
    QCOMPARE(counterfactual_snapshot->sequence, qint64{2});
    QCOMPARE(actual_snapshot->pins.size(), std::size_t{4});
    QCOMPARE(counterfactual_snapshot->pins.size(), std::size_t{4});
    QVERIFY(actual_snapshot->session_id != counterfactual_snapshot->session_id);

    auto reopen_store = provider->forkConnection();
    QVERIFY(reopen_store.has_value());
    const auto reopened_workflow = app::WorkflowSessionController::reopen(
        runtime_case.definition.id, initial, storage::AssetStore(asset_root),
        std::move(*reopen_store), QString::fromLatin1(workflow_engine_revision), *resolved);
    QVERIFY2(reopened_workflow.has_value(),
             reopened_workflow ? "" : qPrintable(reopened_workflow.error().message));
    QCOMPARE((*reopened_workflow)->state(), workflow_state_before);
    QCOMPARE((*reopened_workflow)->journal(), workflow_journal_before);
    QCOMPARE(provider->createAttempts(), 2);
    QCOMPARE(provider->reopenAttempts(), 2);
}

} // namespace

QTEST_MAIN(AsterglenRule54bV02UiE2eTest)
#include "tst_asterglen_rule54b_v02_ui_e2e.moc"
