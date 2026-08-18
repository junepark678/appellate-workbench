#include "appellate/packs/pack_archive.hpp"
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
#include "workflow_session_controller.hpp"

#include <QByteArrayView>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QPdfSearchModel>
#include <QPlainTextEdit>
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

#ifndef APPELLATE_M4_ELLISON_ROOT
#error "APPELLATE_M4_ELLISON_ROOT must name content/m4/ellison-immunity"
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

constexpr auto root_digest = "c2a4f3bc07f05eb1429257320ed839ebaea837da7aa7330f4669bbb157168ce0";
constexpr auto archive_digest = "59f32f521644bac61865cf1e59444fc98dbb9007461a1709272ffe261cbad1d0";
constexpr qint64 archive_byte_size = 4'230'462;
constexpr auto federal_digest = "866c90996c15e2076b9508a297ffce1a4e766b1432a9e11d08e8138c57e363c9";
constexpr auto ca4_digest = "449d75c77e5c47883f750377450f2d1ec1fc0e42e20b1f247446b208661d3262";
constexpr auto bench_digest = "cee0bf93309cc9ad800f215a47d734b20a9fdf5dc889f2f440e4382b942d332d";
constexpr auto actual_argument_session_id = "ca4m4.ellison.session.oral.actual";
constexpr auto counterfactual_argument_session_id = "ca4m4.ellison.session.oral.counterfactual";
constexpr auto workflow_engine_revision = "appellate.realism-evidence.codec-replay-multi.v1";
constexpr auto oral_engine_revision = "engine.oral.ellison-immunity-e2e.1";

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
    std::optional<QByteArray> document_bytes;
};

[[nodiscard]] auto loadActualTrace(const packs::RuntimeRecord& record)
    -> std::expected<std::vector<FrozenWorkflowStep>, QString> {
    const QDir root(QStringLiteral(APPELLATE_M4_ELLISON_ROOT));
    QFile file(root.filePath(QStringLiteral("traces/actual-ordinary-through-mandate.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        return std::unexpected(file.errorString());
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::unexpected(QStringLiteral("Cannot parse canonical Ellison trace"));
    }
    const auto trace = document.object();
    if (trace.value(QStringLiteral("trace_id")).toString() !=
            QStringLiteral("ca4m4.ellison.trace.actual-ordinary") ||
        trace.value(QStringLiteral("command_count")).toInt() != 41 ||
        trace.value(QStringLiteral("event_count")).toInt() != 41 ||
        trace.value(QStringLiteral("terminal_stage_id")).toString() !=
            QStringLiteral("ca4m4.ellison.stage.terminated")) {
        return std::unexpected(QStringLiteral("Canonical Ellison trace metadata drifted"));
    }

    std::vector<FrozenWorkflowStep> steps;
    for (const auto& value : trace.value(QStringLiteral("journal")).toArray()) {
        const auto command_bytes = QByteArray::fromBase64(
            value.toObject().value(QStringLiteral("command_base64")).toString().toLatin1());
        const auto command = storage::decodeWorkflowCommand(QByteArrayView(command_bytes));
        if (!command || storage::encodeWorkflowCommand(*command) != command_bytes) {
            return std::unexpected(QStringLiteral("Canonical trace command cannot round-trip"));
        }
        std::optional<QByteArray> asset_bytes;
        if (const auto digest = commandDocumentDigest(*command); digest.has_value()) {
            const auto entry = std::ranges::find(record.docket_entries, *digest,
                                                 &packs::RuntimeDocketEntry::asset_sha256);
            if (entry == record.docket_entries.end()) {
                return std::unexpected(QStringLiteral("Trace document is absent from record"));
            }
            QFile asset(root.filePath(QStringLiteral("pack-candidate/%1")
                                          .arg(QString::fromStdString(entry->asset_path))));
            if (!asset.open(QIODevice::ReadOnly)) {
                return std::unexpected(asset.errorString());
            }
            asset_bytes = asset.readAll();
            if (sha256(QByteArrayView(*asset_bytes)).toStdString() != *digest) {
                return std::unexpected(QStringLiteral("Trace document bytes have wrong digest"));
            }
        }
        steps.push_back(FrozenWorkflowStep{*command, std::move(asset_bytes)});
    }
    return steps;
}

[[nodiscard]] model::WorkflowState
initialWorkflowState(const packs::RuntimeCase& runtime_case,
                     const model::WorkflowCommand& first_command) {
    model::WorkflowState state;
    state.session_id =
        std::visit([](const auto& concrete) { return concrete.header.session_id; }, first_command);
    state.workflow_id = runtime_case.workflow.id;
    state.current_stage_id = runtime_case.workflow.initial_stage_id;
    return state;
}

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
            return failure(QStringLiteral("Oral launch lost exact Ellison root ownership"));
        }
        QString session_id;
        if (configuration_id.value == "ca4m4.ellison.argument.actual-record") {
            session_id = QString::fromLatin1(actual_argument_session_id);
        } else if (configuration_id.value == "ca4m4.ellison.argument.adverse-record") {
            session_id = QString::fromLatin1(counterfactual_argument_session_id);
        } else {
            return failure(QStringLiteral("Unknown Ellison argument configuration"));
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

class EllisonImmunityUiE2eTest final : public QObject {
    Q_OBJECT

  private slots:
    void exportsInstallsReplaysAndExercisesActualAndBranchWorkspaces();
};

void EllisonImmunityUiE2eTest::exportsInstallsReplaysAndExercisesActualAndBranchWorkspaces() {
    const model::PackRevision expected_root{model::PackId{"us.ca4.m4.ellison-immunity"}, "1.2.0",
                                            root_digest};
    const model::PackRevision expected_federal{model::PackId{"foundation.us-federal"}, "2025.12.01",
                                               federal_digest};
    const model::PackRevision expected_ca4{model::PackId{"foundation.us-ca4"}, "2026.03.23",
                                           ca4_digest};
    const model::PackRevision expected_bench{model::PackId{"foundation.us-ca4-fictional-bench"},
                                             "1.0.0", bench_digest};
    const QDir root(QStringLiteral(APPELLATE_M4_ELLISON_ROOT));
    const QDir foundations(QStringLiteral(APPELLATE_M4_FOUNDATIONS));

    QTemporaryDir state;
    QVERIFY(state.isValid());
    const auto root_archive =
        QDir(state.path()).filePath(QStringLiteral("ellison-immunity.awpack"));
    const auto exported = packs::PackArchive::exportDirectory(
        root.filePath(QStringLiteral("pack-candidate")), root_archive, {},
        packs::PackValidationScope::ResolvedClosure);
    QVERIFY2(exported.has_value(), exported ? "" : qPrintable(exported.error().message));
    QCOMPARE(*exported, expected_root);
    QFile frozen_root(root_archive);
    QVERIFY2(frozen_root.open(QIODevice::ReadOnly), qPrintable(frozen_root.errorString()));
    QCOMPARE(frozen_root.size(), archive_byte_size);
    QCOMPARE(sha256(QByteArrayView(frozen_root.readAll())), QByteArray(archive_digest));

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
    const auto installed_root =
        (*catalog)->installArchive(root_archive, QStringLiteral("2026-08-12T00:00:03Z"));
    QVERIFY(federal.has_value());
    QVERIFY(ca4.has_value());
    QVERIFY(bench.has_value());
    QVERIFY(installed_root.has_value());
    QCOMPARE(federal->revision, expected_federal);
    QCOMPARE(ca4->revision, expected_ca4);
    QCOMPARE(bench->revision, expected_bench);
    QCOMPARE(installed_root->revision, expected_root);
    QCOMPARE(installed_root->archive_sha256, QString::fromLatin1(archive_digest));
    const auto listed = (*catalog)->list();
    QVERIFY(listed.has_value());
    QCOMPARE(listed->size(), std::size_t{4});

    const auto resolved = (*catalog)->loadResolved(expected_root);
    QVERIFY2(resolved.has_value(), resolved ? "" : qPrintable(resolved.error().message));
    const auto runtime = packs::loadRuntimePack(*resolved);
    QVERIFY2(runtime.has_value(), runtime ? "" : runtime.error().message.c_str());
    QCOMPARE(runtime->cases.size(), std::size_t{1});
    const auto& runtime_case = runtime->cases.front();
    QCOMPARE(runtime_case.record.dockets.size(), std::size_t{3});
    QCOMPARE(runtime_case.record.docket_entries.size(), std::size_t{72});
    QCOMPARE(runtime_case.record.page_anchors.size(), std::size_t{449});
    QCOMPARE(runtime_case.workflow.stages.size(), std::size_t{20});
    QCOMPARE(runtime_case.workflow.operations.size(), std::size_t{77});
    QCOMPARE(runtime_case.workflow.filing_routes.size(), std::size_t{11});
    QCOMPARE(runtime_case.definition.disposition_plans.size(), std::size_t{2});
    const auto former_actor =
        std::ranges::find(runtime_case.definition.actors,
                          model::ActorId{"ca4m4.ellison.actor.alder-creek"}, &model::CaseActor::id);
    const auto active_appellee =
        std::ranges::find(runtime_case.definition.actors,
                          model::ActorId{"ca4m4.ellison.actor.ellison"}, &model::CaseActor::id);
    QVERIFY(former_actor != runtime_case.definition.actors.end());
    QVERIFY(active_appellee != runtime_case.definition.actors.end());
    QCOMPARE(former_actor->role,
             model::ActorRoleId{"ca4m4.ellison.role.former-district-defendant"});
    QCOMPARE(active_appellee->role, model::ActorRoleId{"us.ca4.role.responding-party"});
    QVERIFY(std::ranges::none_of(runtime_case.workflow.filing_routes, [](const auto& route) {
        return std::ranges::find(
                   route.required_service_roles,
                   model::ActorRoleId{"ca4m4.ellison.role.former-district-defendant"}) !=
               route.required_service_roles.end();
    }));

    const auto actual_trace = loadActualTrace(runtime_case.record);
    QVERIFY2(actual_trace.has_value(), actual_trace ? "" : qPrintable(actual_trace.error()));
    QCOMPARE(actual_trace->size(), std::size_t{41});
    const auto initial = initialWorkflowState(runtime_case, actual_trace->front().command);
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

    for (std::size_t index = 0; index < actual_trace->size(); ++index) {
        const auto& step = actual_trace->at(index);
        std::optional<QByteArrayView> document_view;
        if (step.document_bytes.has_value()) {
            document_view = QByteArrayView(*step.document_bytes);
        }
        const auto submitted =
            (*workflow)->submit(step.command, document_view,
                                QStringLiteral("2026-08-12T19:%1:00Z")
                                    .arg(static_cast<int>(index), 2, 10, QLatin1Char('0')));
        QVERIFY2(submitted.has_value(), submitted ? "" : qPrintable(submitted.error().message));
    }
    QCOMPARE((*workflow)->state().current_stage_id,
             model::WorkflowStageId{"ca4m4.ellison.stage.terminated"});
    QVERIFY((*workflow)->state().judgment_disposition.has_value());
    const auto* disposition =
        std::get_if<model::DispositionPlan>(&*(*workflow)->state().judgment_disposition);
    QVERIFY(disposition != nullptr);
    QCOMPARE(disposition->id, model::DispositionPlanId{"ca4m4.ellison.disposition.authored-mixed"});
    QCOMPARE((*workflow)->journal().size(), std::size_t{41});
    QCOMPARE((*workflow)->snapshot().sequence, qint64{41});
    QCOMPARE((*workflow)->snapshot().commands.size(), std::size_t{41});
    QCOMPARE((*workflow)->snapshot().events.size(), std::size_t{41});
    QVERIFY(!(*workflow)->snapshot().asset_references.empty());
    const auto workflow_state_before = (*workflow)->state();
    const auto workflow_journal_before = (*workflow)->journal();
    const std::string legal_state_digest = root_digest;
    (*workflow).reset();

    auto provider_store = storage::SessionStore::open(database_path);
    QVERIFY2(provider_store.has_value(),
             provider_store ? "" : qPrintable(provider_store.error().message));
    auto provider = std::make_shared<PersistedLaunchProvider>(legal_state_digest, expected_root,
                                                              std::move(*provider_store));
    std::optional<model::OralArgumentState> actual_argument_state;
    std::optional<model::OralArgumentState> counterfactual_argument_state;

    {
        ui::MainWindow window({}, catalog_root, nullptr, provider);
        const auto loaded = window.loadSource(root_archive);
        QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error()));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.currentRuntime() != nullptr);
        QCOMPARE(window.currentRuntime()->revision, expected_root);
        QCOMPARE(window.caseList()->count(), 1);
        QCOMPARE(window.argumentConfigurationSelector()->count(), 2);
        QCOMPARE(window.currentRuntime()->cases.front().definition.disposition_plans.size(),
                 std::size_t{2});

        const auto actual_index =
            configurationIndex(window, "ca4m4.ellison.argument.actual-record");
        const auto counterfactual_index =
            configurationIndex(window, "ca4m4.ellison.argument.adverse-record");
        QVERIFY(actual_index >= 0);
        QVERIFY(counterfactual_index >= 0);
        window.argumentConfigurationSelector()->setCurrentIndex(actual_index);
        QCOMPARE(window.profileSelector()->count(), 3);
        const std::vector<std::string> expected_profiles{"us.ca4.bench-profile.alder",
                                                         "us.ca4.bench-profile.reed",
                                                         "us.ca4.bench-profile.march"};
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
                 std::string("ca4m4.ellison.argument.actual-record"));
        QCOMPARE(
            window.oralArgumentWorkspace()->canonicalDefinition()->question_bank.questions.size(),
            std::size_t{12});
        QCOMPARE(window.oralArgumentWorkspace()
                     ->canonicalDefinition()
                     ->configuration.authored_disposition_id,
                 std::string("ca4m4.ellison.disposition.authored-mixed"));
        submitGroundedAnswer(
            *window.oralArgumentWorkspace(),
            QStringLiteral("The actual JA and PA record supports the mixed disposition."));
        actual_argument_state = *window.oralArgumentWorkspace()->sessionState();

        window.argumentConfigurationSelector()->setCurrentIndex(counterfactual_index);
        QVERIFY(window.openSelectedOralArgument().has_value());
        QTRY_VERIFY(window.oralArgumentWorkspace()->isReady());
        QVERIFY(window.oralArgumentWorkspace()->isolationNoticeLabel()->text().contains(
            QStringLiteral("isolated from the actual-record workflow")));
        QCOMPARE(window.oralArgumentWorkspace()
                     ->canonicalDefinition()
                     ->question_bank.argument_configuration_id,
                 std::string("ca4m4.ellison.argument.adverse-record"));
        QCOMPARE(
            window.oralArgumentWorkspace()->canonicalDefinition()->question_bank.questions.size(),
            std::size_t{12});
        submitGroundedAnswer(
            *window.oralArgumentWorkspace(),
            QStringLiteral("The adverse immunity branch remains isolated from actuality."));
        counterfactual_argument_state = *window.oralArgumentWorkspace()->sessionState();

        const auto opened_record = window.openSelectedRecord();
        QVERIFY2(opened_record.has_value(), opened_record ? "" : qPrintable(opened_record.error()));
        auto* workspace = window.recordWorkspace();
        QVERIFY(workspace != nullptr);
        auto* pdf_search = workspace->findChild<QPdfSearchModel*>();
        QVERIFY(pdf_search != nullptr);
        QCOMPARE(workspace->visibleDocketCount(), qsizetype{72});
        const std::array docket_filters{
            std::pair{QStringLiteral("ca4m4.ellison.docket.district"), qsizetype{37}},
            std::pair{QStringLiteral("ca4m4.ellison.docket.appellate"), qsizetype{15}},
            std::pair{QStringLiteral("ca4m4.ellison.docket.counterfactual-branches"),
                      qsizetype{20}},
        };
        for (const auto& [filter, count] : docket_filters) {
            workspace->setDocketFilter(filter);
            QCOMPARE(workspace->visibleDocketCount(), count);
        }
        workspace->setDocketFilter(QStringLiteral("actual_appellate_docket"));
        QCOMPARE(workspace->visibleDocketCount(), qsizetype{15});
        workspace->setDocketFilter(QStringLiteral("never_occurred_on_actual_docket"));
        QCOMPARE(workspace->visibleDocketCount(), qsizetype{20});
        workspace->setDocketFilter({});

        struct Navigation final {
            QString citation;
            QString entry_id;
            int pages{};
            int page_index{};
            QString search;
        };
        const std::array navigations{
            Navigation{QStringLiteral("JA242"), QStringLiteral("ca4m4.ellison.record.entry.l32"),
                       12, 0, QStringLiteral("genuine dispute exists")},
            Navigation{QStringLiteral("PA68"), QStringLiteral("ca4m4.ellison.record.entry.a10"), 2,
                       0, QStringLiteral("calendared for oral argument")},
            Navigation{QStringLiteral("PA72"), QStringLiteral("ca4m4.ellison.record.entry.a12"), 14,
                       0, QStringLiteral("resolving jurisdiction and qualified immunity")},
            Navigation{QStringLiteral("PA168"), QStringLiteral("ca4m4.ellison.record.entry.b18"), 3,
                       0, QStringLiteral("Notice Abandoning Planned Certiorari")},
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
        QVERIFY(window.loadSource(root_archive).has_value());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const auto counterfactual_index =
            configurationIndex(window, "ca4m4.ellison.argument.adverse-record");
        window.argumentConfigurationSelector()->setCurrentIndex(counterfactual_index);
        QVERIFY(window.openSelectedOralArgument().has_value());
        QTRY_VERIFY(window.oralArgumentWorkspace()->isReady());
        QCOMPARE(*window.oralArgumentWorkspace()->sessionState(), *counterfactual_argument_state);
        const auto actual_index =
            configurationIndex(window, "ca4m4.ellison.argument.actual-record");
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

QTEST_MAIN(EllisonImmunityUiE2eTest)
#include "tst_m4_ellison_immunity_ui_e2e.moc"
