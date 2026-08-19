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
#include "workflow_launch_provider.hpp"
#include "workflow_session_controller.hpp"

#include <QAction>
#include <QByteArrayView>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <numeric>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef APPELLATE_M4_CINDER_ROOT
#error "APPELLATE_M4_CINDER_ROOT must name content/m4/cinderlake-writ"
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

constexpr auto root_digest = "020517571a6c15f90765e12b94ab53d8598be3bc3081d47caecdf5950bacd05c";
constexpr auto archive_digest = "eeefbbbe84cf4addbf91a68447281217226c6a08c7e0e3e1294947d5e5dc8956";
constexpr qint64 archive_byte_size = 2'519'053;
constexpr auto federal_digest = "866c90996c15e2076b9508a297ffce1a4e766b1432a9e11d08e8138c57e363c9";
constexpr auto ca4_digest = "449d75c77e5c47883f750377450f2d1ec1fc0e42e20b1f247446b208661d3262";
constexpr auto bench_digest = "cee0bf93309cc9ad800f215a47d734b20a9fdf5dc889f2f440e4382b942d332d";
constexpr auto workflow_engine_revision = "appellate.realism-evidence.codec-replay-multi.v1";
constexpr auto oral_engine_revision = "engine.oral.cinderlake-writ-e2e.1";
constexpr auto actual_argument_session_id = "ca4m4.cinder.session.oral.actual";
constexpr auto counterfactual_argument_session_id = "ca4m4.cinder.session.oral.counterfactual";

[[nodiscard]] QByteArray sha256(QByteArrayView bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

[[nodiscard]] model::LegalDate legalDate(int year, unsigned month, unsigned day) {
    return model::LegalDate{std::chrono::year{year} / std::chrono::month{month} /
                            std::chrono::day{day}};
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

struct TraceContract final {
    QString filename;
    QString trace_id;
    std::string session_id;
    QString terminal_stage_id;
    int command_count{};
    int event_count{};
};

struct FrozenWorkflowStep final {
    model::WorkflowCommand command;
    std::vector<model::WorkflowEvent> expected_events;
    std::optional<QByteArray> document_bytes;
};

struct FrozenWorkflowTrace final {
    TraceContract contract;
    std::vector<FrozenWorkflowStep> steps;
};

[[nodiscard]] auto loadFrozenTrace(const TraceContract& contract,
                                   const packs::RuntimeRecord& record)
    -> std::expected<FrozenWorkflowTrace, QString> {
    const QDir root(QStringLiteral(APPELLATE_M4_CINDER_ROOT));
    QFile file(root.filePath(QStringLiteral("traces/%1").arg(contract.filename)));
    if (!file.open(QIODevice::ReadOnly)) {
        return std::unexpected(file.errorString());
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::unexpected(
            QStringLiteral("Cannot parse canonical Cinder trace %1").arg(contract.filename));
    }
    const auto trace = document.object();
    if (trace.size() != 11 ||
        trace.value(QStringLiteral("trace_id")).toString() != contract.trace_id ||
        trace.value(QStringLiteral("workflow_id")).toString() !=
            QStringLiteral("ca4m4.cinder.workflow.privilege-mandamus") ||
        trace.value(QStringLiteral("engine_revision")).toString() !=
            QString::fromLatin1(workflow_engine_revision) ||
        trace.value(QStringLiteral("command_count")).toInt() != contract.command_count ||
        trace.value(QStringLiteral("event_count")).toInt() != contract.event_count ||
        trace.value(QStringLiteral("terminal_stage_id")).toString() != contract.terminal_stage_id ||
        !trace.value(QStringLiteral("journal")).isArray()) {
        return std::unexpected(QStringLiteral("Canonical Cinder trace metadata drifted for %1")
                                   .arg(contract.trace_id));
    }

    const auto journal = trace.value(QStringLiteral("journal")).toArray();
    if (journal.size() != contract.command_count) {
        return std::unexpected(QStringLiteral("Canonical Cinder journal count drifted"));
    }
    FrozenWorkflowTrace loaded{contract, {}};
    loaded.steps.reserve(static_cast<std::size_t>(journal.size()));
    int decoded_event_count = 0;
    for (const auto& value : journal) {
        if (!value.isObject() || value.toObject().size() != 2) {
            return std::unexpected(QStringLiteral("Canonical Cinder journal shape drifted"));
        }
        const auto entry = value.toObject();
        const auto command_base64 =
            entry.value(QStringLiteral("command_base64")).toString().toLatin1();
        const auto command_bytes = QByteArray::fromBase64(command_base64);
        const auto command = storage::decodeWorkflowCommand(QByteArrayView(command_bytes));
        if (!command || command_bytes.toBase64() != command_base64) {
            return std::unexpected(QStringLiteral("Canonical Cinder command cannot decode"));
        }
        const auto reencoded_command = storage::encodeWorkflowCommand(*command);
        if (!reencoded_command || *reencoded_command != command_bytes) {
            return std::unexpected(QStringLiteral("Canonical Cinder command cannot round-trip"));
        }
        const auto command_session_matches = std::visit(
            [&contract](const auto& concrete) {
                return concrete.header.session_id == contract.session_id;
            },
            *command);
        if (!command_session_matches) {
            return std::unexpected(QStringLiteral("Canonical Cinder command session drifted"));
        }

        const auto encoded_events = entry.value(QStringLiteral("events_base64")).toArray();
        if (encoded_events.isEmpty()) {
            return std::unexpected(QStringLiteral("Canonical Cinder command has no event"));
        }
        std::vector<model::WorkflowEvent> events;
        events.reserve(static_cast<std::size_t>(encoded_events.size()));
        for (const auto& encoded_value : encoded_events) {
            const auto event_base64 = encoded_value.toString().toLatin1();
            const auto event_bytes = QByteArray::fromBase64(event_base64);
            const auto event = storage::decodeWorkflowEvent(QByteArrayView(event_bytes));
            if (!event || event_bytes.toBase64() != event_base64) {
                return std::unexpected(QStringLiteral("Canonical Cinder event cannot decode"));
            }
            const auto reencoded_event = storage::encodeWorkflowEvent(*event);
            if (!reencoded_event || *reencoded_event != event_bytes) {
                return std::unexpected(QStringLiteral("Canonical Cinder event cannot round-trip"));
            }
            const auto event_header_matches = std::visit(
                [&contract](const auto& concrete) {
                    return concrete.header.session_id == contract.session_id &&
                           concrete.header.workflow_id ==
                               model::WorkflowId{"ca4m4.cinder.workflow.privilege-mandamus"};
                },
                *event);
            if (!event_header_matches) {
                return std::unexpected(QStringLiteral("Canonical Cinder event identity drifted"));
            }
            events.push_back(*event);
            ++decoded_event_count;
        }

        std::optional<QByteArray> document_bytes;
        if (const auto digest = commandDocumentDigest(*command); digest.has_value()) {
            const auto docket_entry = std::ranges::find(record.docket_entries, *digest,
                                                        &packs::RuntimeDocketEntry::asset_sha256);
            if (docket_entry == record.docket_entries.end()) {
                return std::unexpected(
                    QStringLiteral("Canonical Cinder document is absent from record"));
            }
            QFile asset(root.filePath(QStringLiteral("pack-candidate/%1")
                                          .arg(QString::fromStdString(docket_entry->asset_path))));
            if (!asset.open(QIODevice::ReadOnly)) {
                return std::unexpected(asset.errorString());
            }
            document_bytes = asset.readAll();
            if (sha256(QByteArrayView(*document_bytes)).toStdString() != *digest) {
                return std::unexpected(
                    QStringLiteral("Canonical Cinder document has wrong digest"));
            }
        }
        loaded.steps.push_back(
            FrozenWorkflowStep{*command, std::move(events), std::move(document_bytes)});
    }
    if (decoded_event_count != contract.event_count) {
        return std::unexpected(QStringLiteral("Canonical Cinder event count drifted"));
    }
    return loaded;
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

struct WorkflowReplay final {
    model::WorkflowState initial;
    model::WorkflowState final_state;
    std::vector<model::WorkflowState> states_after_commands;
    std::vector<model::WorkflowJournalEntry> journal;
    storage::SessionSnapshot snapshot;
};

[[nodiscard]] auto replayFrozenTrace(const packs::ResolvedPack& resolved,
                                     const packs::RuntimeCase& runtime_case,
                                     const FrozenWorkflowTrace& trace, const QString& database_path,
                                     const QString& asset_root, bool probe_wrong_document)
    -> std::expected<WorkflowReplay, QString> {
    const auto initial = initialWorkflowState(runtime_case, trace.steps.front().command);
    auto store = storage::SessionStore::open(database_path);
    if (!store) {
        return std::unexpected(store.error().message);
    }
    auto controller = app::WorkflowSessionController::create(
        runtime_case.definition.id, initial, storage::AssetStore(asset_root), std::move(*store),
        QString::fromLatin1(workflow_engine_revision), QStringLiteral("2026-08-19T00:00:00Z"),
        resolved);
    if (!controller) {
        return std::unexpected(controller.error().message);
    }
    if (probe_wrong_document) {
        if (!trace.steps.front().document_bytes.has_value() ||
            trace.steps.front().document_bytes->isEmpty()) {
            return std::unexpected(QStringLiteral("Wrong-document probe has no document"));
        }
        auto wrong_bytes = *trace.steps.front().document_bytes;
        wrong_bytes[0] = static_cast<char>(wrong_bytes.at(0) ^ 0x01);
        const auto rejected = (*controller)
                                  ->submit(trace.steps.front().command, QByteArrayView(wrong_bytes),
                                           QStringLiteral("2026-08-19T00:00:30Z"));
        if (rejected ||
            rejected.error().code != app::WorkflowSessionErrorCode::DocumentDigestMismatch ||
            (*controller)->state() != initial || !(*controller)->journal().empty() ||
            (*controller)->snapshot().sequence != 0) {
            return std::unexpected(QStringLiteral("Wrong-document probe was not atomic"));
        }
    }

    std::vector<model::WorkflowState> states;
    states.reserve(trace.steps.size());
    for (std::size_t index = 0; index < trace.steps.size(); ++index) {
        const auto& step = trace.steps.at(index);
        std::optional<QByteArrayView> document_view;
        if (step.document_bytes.has_value()) {
            document_view = QByteArrayView(*step.document_bytes);
        }
        const auto submitted =
            (*controller)
                ->submit(step.command, document_view,
                         QStringLiteral("2026-08-19T01:%1:00Z")
                             .arg(static_cast<int>(index), 2, 10, QLatin1Char('0')));
        if (!submitted) {
            return std::unexpected(submitted.error().message);
        }
        if (submitted->events != step.expected_events ||
            submitted->asset.has_value() != step.document_bytes.has_value()) {
            return std::unexpected(QStringLiteral("Cinder replay differs from frozen journal"));
        }
        if (step.document_bytes.has_value()) {
            const auto digest = commandDocumentDigest(step.command);
            if (!digest.has_value() ||
                submitted->asset->sha256 != QString::fromStdString(*digest) ||
                submitted->asset->size != step.document_bytes->size()) {
                return std::unexpected(QStringLiteral("Cinder replay stored wrong asset"));
            }
        }
        states.push_back((*controller)->state());
    }
    if ((*controller)->state().current_stage_id.value !=
            trace.contract.terminal_stage_id.toStdString() ||
        (*controller)->journal().size() != static_cast<std::size_t>(trace.contract.command_count) ||
        (*controller)->snapshot().commands.size() !=
            static_cast<std::size_t>(trace.contract.command_count) ||
        (*controller)->snapshot().events.size() !=
            static_cast<std::size_t>(trace.contract.event_count) ||
        (*controller)->snapshot().sequence != trace.contract.event_count) {
        return std::unexpected(QStringLiteral("Cinder replay final envelope drifted"));
    }
    return WorkflowReplay{initial, (*controller)->state(), std::move(states),
                          (*controller)->journal(), (*controller)->snapshot()};
}

class SessionConnectionOwner final {
  public:
    explicit SessionConnectionOwner(std::unique_ptr<storage::SessionStore> store)
        : store_(std::move(store)) {}

    [[nodiscard]] auto forkConnection() const
        -> std::expected<std::unique_ptr<storage::SessionStore>, storage::StoreError> {
        return store_->forkConnection();
    }

  private:
    std::unique_ptr<storage::SessionStore> store_;
};

class PersistedWorkflowProvider final : public ui::WorkflowLaunchProvider {
  public:
    PersistedWorkflowProvider(model::WorkflowState initial, model::PackRevision expected_revision,
                              QString asset_root,
                              std::shared_ptr<SessionConnectionOwner> connection_owner)
        : initial_(std::move(initial)), expected_revision_(std::move(expected_revision)),
          asset_root_(std::move(asset_root)), connection_owner_(std::move(connection_owner)) {}

    [[nodiscard]] auto openWorkflow(const packs::ResolvedPack& resolved,
                                    const model::CaseId& case_id)
        -> std::expected<std::unique_ptr<app::WorkflowSessionController>,
                         app::WorkflowSessionError> override {
        if (resolved.root().revision != expected_revision_ ||
            case_id != model::CaseId{"ca4m4.case.cinderlake-writ"} ||
            resolved.resourceOwner("ca4m4.cinder.workflow.privilege-mandamus") !=
                std::optional<model::PackRevision>{expected_revision_}) {
            return std::unexpected(app::WorkflowSessionError{
                app::WorkflowSessionErrorCode::InvalidConfiguration,
                QStringLiteral("Workflow launch lost exact Cinder ownership")});
        }
        auto store = connection_owner_->forkConnection();
        if (!store) {
            return std::unexpected(app::WorkflowSessionError{
                app::WorkflowSessionErrorCode::SessionStoreFailure, store.error().message});
        }
        ++open_attempts_;
        return app::WorkflowSessionController::reopen(
            case_id, initial_, storage::AssetStore(asset_root_), std::move(*store),
            QString::fromLatin1(workflow_engine_revision), resolved);
    }

    [[nodiscard]] int openAttempts() const noexcept { return open_attempts_; }

  private:
    model::WorkflowState initial_;
    model::PackRevision expected_revision_;
    QString asset_root_;
    std::shared_ptr<SessionConnectionOwner> connection_owner_;
    int open_attempts_{};
};

class PersistedArgumentProvider final : public ui::OralArgumentLaunchProvider {
  public:
    PersistedArgumentProvider(std::string legal_state_digest, model::PackRevision expected_revision,
                              std::shared_ptr<SessionConnectionOwner> connection_owner)
        : legal_state_digest_(std::move(legal_state_digest)),
          expected_revision_(std::move(expected_revision)),
          connection_owner_(std::move(connection_owner)) {}

    [[nodiscard]] auto open(const packs::ResolvedPack& resolved, const model::CaseId& case_id,
                            const packs::RuntimeArgumentConfigId& configuration_id)
        -> std::expected<std::unique_ptr<app::OralArgumentSessionController>,
                         app::OralArgumentSessionError> override {
        if (resolved.root().revision != expected_revision_ ||
            resolved.resourceOwner(configuration_id.value) !=
                std::optional<model::PackRevision>{expected_revision_}) {
            return failure(QStringLiteral("Oral launch lost exact Cinder ownership"));
        }
        QString session_id;
        if (configuration_id.value == "ca4m4.cinder.argument.actual-record") {
            session_id = QString::fromLatin1(actual_argument_session_id);
        } else if (configuration_id.value ==
                   "ca4m4.cinder.argument.summary-denial-counterfactual") {
            session_id = QString::fromLatin1(counterfactual_argument_session_id);
        } else {
            return failure(QStringLiteral("Unknown Cinder argument configuration"));
        }
        auto connection = connection_owner_->forkConnection();
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
                QString::fromLatin1(oral_engine_revision), resolved);
        }
        ++create_attempts_;
        return app::OralArgumentSessionController::create(
            session_id, case_id, configuration_id, legal_state_digest_, std::move(*connection),
            QString::fromLatin1(oral_engine_revision), QStringLiteral("2026-08-19T06:00:00Z"),
            resolved);
    }

    [[nodiscard]] auto forkConnection()
        -> std::expected<std::unique_ptr<storage::SessionStore>, storage::StoreError> {
        return connection_owner_->forkConnection();
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
    std::shared_ptr<SessionConnectionOwner> connection_owner_;
    int create_attempts_{};
    int reopen_attempts_{};
};

class DeterministicRecordAccessProvider final : public ui::RecordAccessTransitionProvider {
  public:
    struct Transition final {
        QString session_id;
        std::uint64_t sequence{};
        std::string disclosure_id;
        model::RecordAccessAction action{};
    };

    [[nodiscard]] auto createdAtUtc(QStringView session_id)
        -> std::expected<QString, QString> override {
        created_sessions.push_back(session_id.toString());
        return QStringLiteral("2026-08-19T06:10:00Z");
    }

    [[nodiscard]] auto next(QStringView session_id, std::uint64_t next_sequence,
                            const packs::RuntimeRecordDisclosureId& disclosure_id,
                            model::RecordAccessAction action)
        -> std::expected<ui::RecordAccessTransitionStamp, QString> override {
        transitions.push_back(
            Transition{session_id.toString(), next_sequence, disclosure_id.value, action});
        return ui::RecordAccessTransitionStamp{
            QStringLiteral("ca4m4.cinder.record-access.event.%1")
                .arg(static_cast<qulonglong>(next_sequence)),
            QStringLiteral("2026-08-19T06:10:%1Z")
                .arg(static_cast<qulonglong>(next_sequence), 2, 10, QLatin1Char('0')),
        };
    }

    std::vector<QString> created_sessions;
    std::vector<Transition> transitions;
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

class CinderLakeWritUiE2eTest final : public QObject {
    Q_OBJECT

  private slots:
    void exportsInstallsReplaysAndExercisesWorkflowArgumentAndRecordUi();
};

void CinderLakeWritUiE2eTest::exportsInstallsReplaysAndExercisesWorkflowArgumentAndRecordUi() {
    const model::PackRevision expected_root{model::PackId{"us.ca4.m4.cinderlake-writ"}, "1.2.0",
                                            root_digest};
    const model::PackRevision expected_federal{model::PackId{"foundation.us-federal"}, "2025.12.01",
                                               federal_digest};
    const model::PackRevision expected_ca4{model::PackId{"foundation.us-ca4"}, "2026.03.23",
                                           ca4_digest};
    const model::PackRevision expected_bench{model::PackId{"foundation.us-ca4-fictional-bench"},
                                             "1.0.0", bench_digest};
    const QDir root(QStringLiteral(APPELLATE_M4_CINDER_ROOT));
    const QDir foundations(QStringLiteral(APPELLATE_M4_FOUNDATIONS));

    QTemporaryDir state;
    QVERIFY(state.isValid());
    const auto root_archive = QDir(state.path()).filePath(QStringLiteral("cinderlake-writ.awpack"));
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
    const auto session_database = QDir(state.path()).filePath(QStringLiteral("sessions.sqlite"));
    const auto access_database =
        QDir(state.path()).filePath(QStringLiteral("record-access.sqlite"));
    const auto asset_root = QDir(state.path()).filePath(QStringLiteral("workflow-assets"));
    const auto catalog = packs::PackCatalog::open(catalog_root);
    QVERIFY2(catalog.has_value(), catalog ? "" : qPrintable(catalog.error().message));
    const auto federal = (*catalog)->installArchive(
        foundations.filePath(QStringLiteral("us-federal/foundation-us-federal-2025.12.01.awpack")),
        QStringLiteral("2026-08-19T05:59:00Z"));
    const auto ca4 = (*catalog)->installArchive(
        foundations.filePath(QStringLiteral("us-ca4/foundation-us-ca4-2026.03.23.awpack")),
        QStringLiteral("2026-08-19T05:59:01Z"));
    const auto bench = (*catalog)->installArchive(
        foundations.filePath(QStringLiteral(
            "us-ca4-fictional-bench/foundation-us-ca4-fictional-bench-1.0.0.awpack")),
        QStringLiteral("2026-08-19T05:59:02Z"));
    const auto installed_root =
        (*catalog)->installArchive(root_archive, QStringLiteral("2026-08-19T05:59:03Z"));
    QVERIFY2(federal.has_value(), federal ? "" : qPrintable(federal.error().message));
    QVERIFY2(ca4.has_value(), ca4 ? "" : qPrintable(ca4.error().message));
    QVERIFY2(bench.has_value(), bench ? "" : qPrintable(bench.error().message));
    QVERIFY2(installed_root.has_value(),
             installed_root ? "" : qPrintable(installed_root.error().message));
    QCOMPARE(federal->revision, expected_federal);
    QCOMPARE(ca4->revision, expected_ca4);
    QCOMPARE(bench->revision, expected_bench);
    QCOMPARE(installed_root->revision, expected_root);
    QCOMPARE(installed_root->archive_sha256, QString::fromLatin1(archive_digest));
    const auto listed = (*catalog)->list();
    QVERIFY2(listed.has_value(), listed ? "" : qPrintable(listed.error().message));
    QCOMPARE(listed->size(), std::size_t{4});

    const auto resolved = (*catalog)->loadResolved(expected_root);
    QVERIFY2(resolved.has_value(), resolved ? "" : qPrintable(resolved.error().message));
    const auto runtime = packs::loadRuntimePack(*resolved);
    QVERIFY2(runtime.has_value(), runtime ? "" : runtime.error().message.c_str());
    QCOMPARE(runtime->cases.size(), std::size_t{1});
    const auto& runtime_case = runtime->cases.front();
    QCOMPARE(runtime_case.definition.id, model::CaseId{"ca4m4.case.cinderlake-writ"});
    QCOMPARE(runtime_case.workflow.id,
             model::WorkflowId{"ca4m4.cinder.workflow.privilege-mandamus"});
    QCOMPARE(runtime_case.workflow.stages.size(), std::size_t{15});
    QCOMPARE(runtime_case.workflow.operations.size(), std::size_t{45});
    QCOMPARE(runtime_case.workflow.filing_routes.size(), std::size_t{8});
    QCOMPARE(runtime_case.definition.disposition_plans.size(), std::size_t{2});
    QCOMPARE(runtime_case.argument_configurations.size(), std::size_t{2});
    QCOMPARE(runtime_case.record.id.value, std::string("ca4m4.cinder.record"));
    QCOMPARE(runtime_case.record.dockets.size(), std::size_t{4});
    QCOMPARE(runtime_case.record.docket_entries.size(), std::size_t{48});
    QCOMPARE(runtime_case.record.page_anchors.size(), std::size_t{402});
    QCOMPARE(runtime_case.record.sealed_disclosures.size(), std::size_t{4});
    QVERIFY(runtime_case.record.disclosure_policy.has_value());
    QVERIFY(std::ranges::none_of(runtime_case.workflow.operations, [](const auto& operation) {
        return operation.opcode == model::WorkflowOpcode::IssueMandate ||
               operation.id.value.contains("mandate");
    }));

    const std::array contracts{
        TraceContract{QStringLiteral("actual-through-rehearing-expiration.json"),
                      QStringLiteral("ca4m4.cinder.trace.actual-through-rehearing-expiration"),
                      "ca4m4.cinder.session.actual-through-rehearing-expiration",
                      QStringLiteral("ca4m4.cinder.stage.actual-terminal"), 26, 29},
        TraceContract{QStringLiteral("counterfactual-deficiency-cure.json"),
                      QStringLiteral("ca4m4.cinder.trace.counterfactual-deficiency-cure"),
                      "ca4m4.cinder.session.counterfactual-deficiency-cure",
                      QStringLiteral("ca4m4.cinder.stage.submitted"), 4, 6},
        TraceContract{
            QStringLiteral("counterfactual-summary-denial-through-rehearing-expiration.json"),
            QStringLiteral(
                "ca4m4.cinder.trace.counterfactual-summary-denial-through-rehearing-expiration"),
            "ca4m4.cinder.session.counterfactual-summary-denial-through-rehearing-expiration",
            QStringLiteral("ca4m4.cinder.stage.counterfactual-summary-terminal"), 6, 7},
    };
    const auto actual_trace = loadFrozenTrace(contracts.at(0), runtime_case.record);
    const auto deficiency_trace = loadFrozenTrace(contracts.at(1), runtime_case.record);
    const auto summary_trace = loadFrozenTrace(contracts.at(2), runtime_case.record);
    QVERIFY2(actual_trace.has_value(), actual_trace ? "" : qPrintable(actual_trace.error()));
    QVERIFY2(deficiency_trace.has_value(),
             deficiency_trace ? "" : qPrintable(deficiency_trace.error()));
    QVERIFY2(summary_trace.has_value(), summary_trace ? "" : qPrintable(summary_trace.error()));

    const auto actual = replayFrozenTrace(*resolved, runtime_case, *actual_trace, session_database,
                                          asset_root, true);
    const auto deficiency = replayFrozenTrace(*resolved, runtime_case, *deficiency_trace,
                                              session_database, asset_root, false);
    const auto summary = replayFrozenTrace(*resolved, runtime_case, *summary_trace,
                                           session_database, asset_root, false);
    QVERIFY2(actual.has_value(), actual ? "" : qPrintable(actual.error()));
    QVERIFY2(deficiency.has_value(), deficiency ? "" : qPrintable(deficiency.error()));
    QVERIFY2(summary.has_value(), summary ? "" : qPrintable(summary.error()));

    QCOMPARE(actual->states_after_commands.at(18).current_stage_id,
             model::WorkflowStageId{"ca4m4.cinder.stage.actual-argument"});
    QVERIFY(actual->states_after_commands.at(18).argument_date.has_value());
    QCOMPARE(*actual->states_after_commands.at(18).argument_date, legalDate(2026, 4, 21));
    QCOMPARE(actual->final_state.current_stage_id,
             model::WorkflowStageId{"ca4m4.cinder.stage.actual-terminal"});
    QVERIFY(actual->final_state.judgment_sha256.has_value());
    QVERIFY(!actual->final_state.mandate_sha256.has_value());
    QVERIFY(actual->final_state.judgment_disposition.has_value());
    const auto* actual_disposition =
        std::get_if<model::DispositionPlan>(&*actual->final_state.judgment_disposition);
    QVERIFY(actual_disposition != nullptr);
    QCOMPARE(
        actual_disposition->id,
        model::DispositionPlanId{"ca4m4.cinder.disposition.actual-partial-grant-vacatur-remand"});
    QCOMPARE(actual_disposition->canonical_sha256,
             std::string("fb287ac6047317c87ea29e6ae7545829795c25c0ba8480b2e548b1511361d519"));
    QCOMPARE(actual->snapshot.sequence, qint64{29});
    QCOMPARE(actual->snapshot.commands.size(), std::size_t{26});
    QCOMPARE(actual->snapshot.events.size(), std::size_t{29});

    QCOMPARE(deficiency->final_state.current_stage_id,
             model::WorkflowStageId{"ca4m4.cinder.stage.submitted"});
    QCOMPARE(deficiency->final_state.deficiencies.size(), std::size_t{1});
    QVERIFY(deficiency->final_state.deficiencies.front().cured);
    QCOMPARE(
        deficiency->final_state.deficiencies.front().deficiency_id.value,
        std::string("ca4m4.cinder.session.counterfactual-deficiency-cure.command.1.deficiency"));
    const auto cure_deadline =
        std::ranges::find(deficiency->final_state.deadlines,
                          model::WorkflowDeadlineId{"ca4m4.cinder.deadline.b01-petition-cure"},
                          &model::WorkflowDeadlineRecord::deadline_id);
    QVERIFY(cure_deadline != deficiency->final_state.deadlines.end());
    QCOMPARE(cure_deadline->due_date, legalDate(2026, 4, 3));
    QVERIFY(cure_deadline->status == model::WorkflowDeadlineStatus::Satisfied);
    QVERIFY(!deficiency->final_state.argument_date.has_value());
    QVERIFY(!deficiency->final_state.judgment_sha256.has_value());
    QVERIFY(!deficiency->final_state.mandate_sha256.has_value());
    QCOMPARE(deficiency->snapshot.sequence, qint64{6});

    QCOMPARE(summary->final_state.current_stage_id,
             model::WorkflowStageId{"ca4m4.cinder.stage.counterfactual-summary-terminal"});
    QVERIFY(!summary->final_state.argument_date.has_value());
    QVERIFY(summary->final_state.judgment_sha256.has_value());
    QVERIFY(!summary->final_state.mandate_sha256.has_value());
    QVERIFY(summary->final_state.judgment_disposition.has_value());
    const auto* summary_disposition =
        std::get_if<model::DispositionPlan>(&*summary->final_state.judgment_disposition);
    QVERIFY(summary_disposition != nullptr);
    QCOMPARE(summary_disposition->id,
             model::DispositionPlanId{"ca4m4.cinder.disposition.counterfactual-summary-denial"});
    QCOMPARE(summary_disposition->canonical_sha256,
             std::string("c053d31abece1d6c4d14fcafc54d3dd7b8845c8fc0b7fcd67f083c2fd23fe8e9"));
    QCOMPARE(summary->snapshot.sequence, qint64{7});
    QVERIFY(actual->final_state.session_id != deficiency->final_state.session_id);
    QVERIFY(actual->final_state.session_id != summary->final_state.session_id);
    QVERIFY(deficiency->final_state.session_id != summary->final_state.session_id);

    const auto stage_operation_count = [&runtime_case](std::string_view stage_id) {
        return std::ranges::count(
            runtime_case.workflow.operations, stage_id,
            [](const auto& operation) { return std::string_view(operation.stage_id.value); });
    };
    QCOMPARE(stage_operation_count("ca4m4.cinder.stage.actual-terminal"),
             std::ranges::range_difference_t<decltype(runtime_case.workflow.operations)>{0});
    QCOMPARE(stage_operation_count("ca4m4.cinder.stage.counterfactual-summary-terminal"),
             std::ranges::range_difference_t<decltype(runtime_case.workflow.operations)>{0});
    QCOMPARE(stage_operation_count("ca4m4.cinder.stage.submitted"),
             std::ranges::range_difference_t<decltype(runtime_case.workflow.operations)>{2});

    auto argument_store = storage::SessionStore::open(session_database);
    QVERIFY2(argument_store.has_value(),
             argument_store ? "" : qPrintable(argument_store.error().message));
    auto connection_owner = std::make_shared<SessionConnectionOwner>(std::move(*argument_store));
    auto argument_provider =
        std::make_shared<PersistedArgumentProvider>(root_digest, expected_root, connection_owner);
    auto actual_workflow_provider = std::make_shared<PersistedWorkflowProvider>(
        actual->initial, expected_root, asset_root, connection_owner);
    auto deficiency_workflow_provider = std::make_shared<PersistedWorkflowProvider>(
        deficiency->initial, expected_root, asset_root, connection_owner);
    auto summary_workflow_provider = std::make_shared<PersistedWorkflowProvider>(
        summary->initial, expected_root, asset_root, connection_owner);
    auto access_provider = std::make_shared<DeterministicRecordAccessProvider>();
    const ui::WorkflowLegalClock fixed_workflow_clock = [](const QDate& selected_court_date)
        -> std::expected<ui::WorkflowLegalClockReading, QString> {
        return ui::WorkflowLegalClockReading{
            QDateTime(selected_court_date, QTime(0, 0), QTimeZone::UTC), selected_court_date};
    };

    std::optional<model::OralArgumentState> actual_argument_state;
    std::optional<model::OralArgumentState> counterfactual_argument_state;
    {
        ui::MainWindow window({}, catalog_root, nullptr, argument_provider, access_provider,
                              access_database, actual_workflow_provider, fixed_workflow_clock);
        const auto loaded = window.loadSource(root_archive);
        QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error()));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.currentRuntime() != nullptr);
        QCOMPARE(window.currentRuntime()->revision, expected_root);
        QCOMPARE(window.caseList()->count(), 1);
        QCOMPARE(window.argumentConfigurationSelector()->count(), 2);
        QVERIFY(window.recordSummaryLabel()->text().contains(QStringLiteral("44")));

        window.workflowCourtDateEditor()->setText(QStringLiteral("2026-05-19"));
        const auto opened_workflow = window.openSelectedWorkflow();
        QVERIFY2(opened_workflow.has_value(),
                 opened_workflow ? "" : qPrintable(opened_workflow.error()));
        QVERIFY(window.workflowSessionController() != nullptr);
        QCOMPARE(window.workflowSessionController()->state(), actual->final_state);
        QCOMPARE(window.workflowSessionController()->journal(), actual->journal);
        QVERIFY(window.workflowStatusLabel()->text().contains(
            QStringLiteral("ca4m4.cinder.stage.actual-terminal")));
        QVERIFY(
            window.workflowStatusLabel()->text().contains(QStringLiteral("29 persisted events")));
        QVERIFY2(window.workflowStatusLabel()->text().contains(
                     QStringLiteral("No currently eligible authored AdvanceStage")),
                 qPrintable(window.workflowStatusLabel()->text()));
        QVERIFY(!window.advanceWorkflowAction()->isEnabled());

        const auto actual_index = configurationIndex(window, "ca4m4.cinder.argument.actual-record");
        const auto counterfactual_index =
            configurationIndex(window, "ca4m4.cinder.argument.summary-denial-counterfactual");
        QVERIFY(actual_index >= 0);
        QVERIFY(counterfactual_index >= 0);
        window.argumentConfigurationSelector()->setCurrentIndex(actual_index);
        QCOMPARE(window.profileSelector()->count(), 3);
        const std::array expected_profiles{
            std::string("us.ca4.bench-profile.alder"),
            std::string("us.ca4.bench-profile.vale"),
            std::string("us.ca4.bench-profile.fen"),
        };
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
                 std::string("ca4m4.cinder.argument.actual-record"));
        QCOMPARE(
            window.oralArgumentWorkspace()->canonicalDefinition()->question_bank.questions.size(),
            std::size_t{16});
        QCOMPARE(window.oralArgumentWorkspace()
                     ->canonicalDefinition()
                     ->configuration.authored_disposition_id,
                 std::string("ca4m4.cinder.disposition.actual-partial-grant-vacatur-remand"));
        submitGroundedAnswer(
            *window.oralArgumentWorkspace(),
            QStringLiteral("The actual JA and PA record supports narrow mandamus relief."));
        actual_argument_state = *window.oralArgumentWorkspace()->sessionState();

        window.argumentConfigurationSelector()->setCurrentIndex(counterfactual_index);
        QVERIFY(window.openSelectedOralArgument().has_value());
        QTRY_VERIFY(window.oralArgumentWorkspace()->isReady());
        QVERIFY(window.oralArgumentWorkspace()->isolationNoticeLabel()->text().contains(
            QStringLiteral("isolated from the actual-record workflow")));
        QCOMPARE(window.oralArgumentWorkspace()
                     ->canonicalDefinition()
                     ->question_bank.argument_configuration_id,
                 std::string("ca4m4.cinder.argument.summary-denial-counterfactual"));
        QCOMPARE(
            window.oralArgumentWorkspace()->canonicalDefinition()->question_bank.questions.size(),
            std::size_t{8});
        submitGroundedAnswer(
            *window.oralArgumentWorkspace(),
            QStringLiteral("The never-filed summary-denial branch remains counterfactual."));
        counterfactual_argument_state = *window.oralArgumentWorkspace()->sessionState();
        QVERIFY(*actual_argument_state != *counterfactual_argument_state);

        const auto opened_record = window.openSelectedRecord();
        QVERIFY2(opened_record.has_value(), opened_record ? "" : qPrintable(opened_record.error()));
        auto* workspace = window.recordWorkspace();
        QVERIFY(workspace != nullptr);
        QCOMPARE(access_provider->created_sessions.size(), std::size_t{1});
        QCOMPARE(workspace->visibleDocketCount(), qsizetype{44});
        QCOMPARE(window.recordAccessMenu()->actions().size(), 4);
        QVERIFY(workspace->disclosureDeficiencies().empty());

        std::uint64_t public_pages = 0;
        QHash<QString, qsizetype> public_dockets;
        for (const auto& entry : runtime_case.record.docket_entries) {
            if (!entry.sealed) {
                public_pages += entry.page_count;
                if (entry.docket_id.has_value()) {
                    ++public_dockets[QString::fromStdString(entry.docket_id->value)];
                }
            }
        }
        QCOMPARE(public_pages, std::uint64_t{350});
        const std::array docket_partitions{
            std::pair{QStringLiteral("ca4m4.cinder.docket.district"), qsizetype{21}},
            std::pair{QStringLiteral("ca4m4.cinder.docket.appellate"), qsizetype{17}},
            std::pair{QStringLiteral("ca4m4.cinder.docket.counterfactual-deficiency"),
                      qsizetype{3}},
            std::pair{QStringLiteral("ca4m4.cinder.docket.counterfactual-summary-denial"),
                      qsizetype{3}},
        };
        for (const auto& [filter, count] : docket_partitions) {
            QCOMPARE(public_dockets.value(filter), count);
            workspace->setDocketFilter(filter);
            QCOMPARE(workspace->visibleDocketCount(), count);
        }
        workspace->setDocketFilter(QStringLiteral("actual_appellate_docket"));
        QCOMPARE(workspace->visibleDocketCount(), qsizetype{17});
        workspace->setDocketFilter(QStringLiteral("never_occurred_on_actual_docket"));
        QCOMPARE(workspace->visibleDocketCount(), qsizetype{6});
        workspace->setDocketFilter({});
        QCOMPARE(workspace->visibleDocketCount(), qsizetype{44});

        QVERIFY(workspace->navigateToCitation(QStringLiteral("PA132")).has_value());
        QCOMPARE(workspace->currentDocumentId(), QStringLiteral("ca4m4.cinder.record.entry.a17"));
        QCOMPARE(workspace->loadedPageCount(), 18);
        QVERIFY(workspace->navigateToCitation(QStringLiteral("PA164")).has_value());
        QCOMPARE(workspace->currentDocumentId(), QStringLiteral("ca4m4.cinder.record.entry.b02"));
        QCOMPARE(workspace->loadedPageCount(), 3);
        QVERIFY(workspace->navigateToCitation(QStringLiteral("PA199")).has_value());
        QCOMPARE(workspace->currentDocumentId(), QStringLiteral("ca4m4.cinder.record.entry.b05"));
        QCOMPARE(workspace->loadedPageCount(), 6);

        struct DisclosureProbe final {
            const char* disclosure_id;
            const char* stable_anchor;
            const char* sealed_anchor;
            const char* public_entry;
            const char* sealed_entry;
            int pages{};
        };
        const std::array disclosures{
            DisclosureProbe{"ca4m4.cinder.disclosure.lower-investigation-memorandum",
                            "ca4m4.cinder.anchor.stable.lower-investigation-memorandum.page-01",
                            "ca4m4.cinder.anchor.ja67", "ca4m4.cinder.record.entry.l08",
                            "ca4m4.cinder.record.entry.l09", 12},
            DisclosureProbe{"ca4m4.cinder.disclosure.lower-privilege-log",
                            "ca4m4.cinder.anchor.stable.lower-privilege-log.page-01",
                            "ca4m4.cinder.anchor.ja87", "ca4m4.cinder.record.entry.l10",
                            "ca4m4.cinder.record.entry.l11", 8},
            DisclosureProbe{"ca4m4.cinder.disclosure.appellate-petition",
                            "ca4m4.cinder.anchor.stable.appellate-petition.page-01",
                            "ca4m4.cinder.anchor.pa30", "ca4m4.cinder.record.entry.a01",
                            "ca4m4.cinder.record.entry.a05", 18},
            DisclosureProbe{
                "ca4m4.cinder.disclosure.appellate-essential-record-appendix",
                "ca4m4.cinder.anchor.stable.appellate-essential-record-appendix.page-01",
                "ca4m4.cinder.anchor.pa62", "ca4m4.cinder.record.entry.a06",
                "ca4m4.cinder.record.entry.a07", 14},
        };
        for (std::size_t index = 0; index < disclosures.size(); ++index) {
            const auto& disclosure = disclosures.at(index);
            QCOMPARE(runtime_case.record.sealed_disclosures.at(index).disclosure_id.value,
                     std::string(disclosure.disclosure_id));
            QCOMPARE(runtime_case.record.sealed_disclosures.at(index).anchor_mappings.size(),
                     static_cast<std::size_t>(disclosure.pages));
            const auto stable =
                workspace->navigateToAnchor(QString::fromLatin1(disclosure.stable_anchor));
            QVERIFY2(stable.has_value(), stable ? "" : qPrintable(stable.error().message));
            QCOMPARE(workspace->currentDocumentId(), QString::fromLatin1(disclosure.public_entry));
            QCOMPARE(workspace->loadedPageCount(), disclosure.pages);
            const auto hidden_anchor =
                workspace->navigateToAnchor(QString::fromLatin1(disclosure.sealed_anchor));
            QVERIFY(!hidden_anchor.has_value());
            QCOMPARE(hidden_anchor.error().code, ui::RecordWorkspaceErrorCode::InvalidPageAnchor);
            QCOMPARE(workspace->currentDocumentId(), QString::fromLatin1(disclosure.public_entry));
            const auto hidden_document =
                workspace->openDocketEntry(QString::fromLatin1(disclosure.sealed_entry));
            QVERIFY(!hidden_document.has_value());
            QCOMPARE(workspace->currentDocumentId(), QString::fromLatin1(disclosure.public_entry));

            auto* grant =
                window.findChild<QAction*>(QStringLiteral("grantRecordAccessAction.%1")
                                               .arg(QString::fromLatin1(disclosure.disclosure_id)));
            auto* revoke =
                window.findChild<QAction*>(QStringLiteral("revokeRecordAccessAction.%1")
                                               .arg(QString::fromLatin1(disclosure.disclosure_id)));
            QVERIFY(grant != nullptr);
            QVERIFY(revoke != nullptr);
            QVERIFY(grant->isEnabled());
            QVERIFY(!revoke->isEnabled());
            grant->trigger();
            QCOMPARE(access_provider->transitions.size(), index * 2U + 1U);
            QCOMPARE(access_provider->transitions.back().sequence,
                     static_cast<std::uint64_t>(index * 2U + 1U));
            QCOMPARE(access_provider->transitions.back().disclosure_id,
                     std::string(disclosure.disclosure_id));
            QVERIFY(access_provider->transitions.back().action == model::RecordAccessAction::Grant);
            QCOMPARE(workspace->visibleDocketCount(), qsizetype{45});
            QVERIFY(!grant->isEnabled());
            QVERIFY(revoke->isEnabled());

            const auto sealed_stable =
                workspace->navigateToAnchor(QString::fromLatin1(disclosure.stable_anchor));
            QVERIFY2(sealed_stable.has_value(),
                     sealed_stable ? "" : qPrintable(sealed_stable.error().message));
            QCOMPARE(workspace->currentDocumentId(), QString::fromLatin1(disclosure.sealed_entry));
            QCOMPARE(workspace->loadedPageCount(), disclosure.pages);
            QVERIFY(workspace->navigateToAnchor(QString::fromLatin1(disclosure.sealed_anchor))
                        .has_value());
            QVERIFY(workspace->openDocketEntry(QString::fromLatin1(disclosure.sealed_entry))
                        .has_value());

            revoke->trigger();
            QCOMPARE(access_provider->transitions.size(), index * 2U + 2U);
            QCOMPARE(access_provider->transitions.back().sequence,
                     static_cast<std::uint64_t>(index * 2U + 2U));
            QCOMPARE(access_provider->transitions.back().disclosure_id,
                     std::string(disclosure.disclosure_id));
            QVERIFY(access_provider->transitions.back().action ==
                    model::RecordAccessAction::Revoke);
            QCOMPARE(workspace->visibleDocketCount(), qsizetype{44});
            QVERIFY(grant->isEnabled());
            QVERIFY(!revoke->isEnabled());
            QVERIFY(workspace->currentDocumentId().isEmpty());
            QVERIFY(!workspace->openDocketEntry(QString::fromLatin1(disclosure.sealed_entry))
                         .has_value());
            const auto hidden_again =
                workspace->navigateToAnchor(QString::fromLatin1(disclosure.sealed_anchor));
            QVERIFY(!hidden_again.has_value());
            QCOMPARE(hidden_again.error().code, ui::RecordWorkspaceErrorCode::InvalidPageAnchor);
            const auto public_again =
                workspace->navigateToAnchor(QString::fromLatin1(disclosure.stable_anchor));
            QVERIFY2(public_again.has_value(),
                     public_again ? "" : qPrintable(public_again.error().message));
            QCOMPARE(workspace->currentDocumentId(), QString::fromLatin1(disclosure.public_entry));
            QCOMPARE(workspace->loadedPageCount(), disclosure.pages);
        }
    }

    {
        ui::MainWindow window({}, catalog_root, nullptr, argument_provider, {}, {},
                              deficiency_workflow_provider, fixed_workflow_clock);
        QVERIFY(window.loadSource(root_archive).has_value());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.workflowCourtDateEditor()->setText(QStringLiteral("2026-04-01"));
        const auto opened_workflow = window.openSelectedWorkflow();
        QVERIFY2(opened_workflow.has_value(),
                 opened_workflow ? "" : qPrintable(opened_workflow.error()));
        QVERIFY(window.workflowSessionController() != nullptr);
        QCOMPARE(window.workflowSessionController()->state(), deficiency->final_state);
        QCOMPARE(window.workflowSessionController()->journal(), deficiency->journal);
        QVERIFY(window.workflowStatusLabel()->text().contains(
            QStringLiteral("ca4m4.cinder.stage.submitted")));
        QVERIFY(
            window.workflowStatusLabel()->text().contains(QStringLiteral("6 persisted events")));
        QVERIFY2(window.workflowStatusLabel()->text().contains(
                     QStringLiteral("No currently eligible authored AdvanceStage")),
                 qPrintable(window.workflowStatusLabel()->text()));
        QVERIFY(!window.advanceWorkflowAction()->isEnabled());

        const auto counterfactual_index =
            configurationIndex(window, "ca4m4.cinder.argument.summary-denial-counterfactual");
        window.argumentConfigurationSelector()->setCurrentIndex(counterfactual_index);
        QVERIFY(window.openSelectedOralArgument().has_value());
        QTRY_VERIFY(window.oralArgumentWorkspace()->isReady());
        QCOMPARE(*window.oralArgumentWorkspace()->sessionState(), *counterfactual_argument_state);
        const auto actual_index = configurationIndex(window, "ca4m4.cinder.argument.actual-record");
        window.argumentConfigurationSelector()->setCurrentIndex(actual_index);
        QVERIFY(window.openSelectedOralArgument().has_value());
        QTRY_VERIFY(window.oralArgumentWorkspace()->isReady());
        QCOMPARE(*window.oralArgumentWorkspace()->sessionState(), *actual_argument_state);
    }

    {
        ui::MainWindow window({}, catalog_root, nullptr, {}, {}, {}, summary_workflow_provider,
                              fixed_workflow_clock);
        QVERIFY(window.loadSource(root_archive).has_value());
        window.workflowCourtDateEditor()->setText(QStringLiteral("2026-04-15"));
        const auto opened_workflow = window.openSelectedWorkflow();
        QVERIFY2(opened_workflow.has_value(),
                 opened_workflow ? "" : qPrintable(opened_workflow.error()));
        QVERIFY(window.workflowSessionController() != nullptr);
        QCOMPARE(window.workflowSessionController()->state(), summary->final_state);
        QCOMPARE(window.workflowSessionController()->journal(), summary->journal);
        QVERIFY(window.workflowStatusLabel()->text().contains(
            QStringLiteral("ca4m4.cinder.stage.counterfactual-summary-terminal")));
        QVERIFY(
            window.workflowStatusLabel()->text().contains(QStringLiteral("7 persisted events")));
        QVERIFY(!window.advanceWorkflowAction()->isEnabled());
    }

    QCOMPARE(actual_workflow_provider->openAttempts(), 1);
    QCOMPARE(deficiency_workflow_provider->openAttempts(), 1);
    QCOMPARE(summary_workflow_provider->openAttempts(), 1);
    QCOMPARE(argument_provider->createAttempts(), 2);
    QCOMPARE(argument_provider->reopenAttempts(), 2);
    QCOMPARE(access_provider->created_sessions.size(), std::size_t{1});
    QCOMPARE(access_provider->transitions.size(), std::size_t{8});
    for (std::size_t index = 0; index < access_provider->transitions.size(); ++index) {
        QCOMPARE(access_provider->transitions.at(index).session_id,
                 access_provider->created_sessions.front());
        QCOMPARE(access_provider->transitions.at(index).sequence,
                 static_cast<std::uint64_t>(index + 1U));
    }

    auto verification_store = argument_provider->forkConnection();
    QVERIFY2(verification_store.has_value(),
             verification_store ? "" : qPrintable(verification_store.error().message));
    const auto actual_argument_snapshot =
        (*verification_store)->loadSession(QString::fromLatin1(actual_argument_session_id));
    const auto counterfactual_argument_snapshot =
        (*verification_store)->loadSession(QString::fromLatin1(counterfactual_argument_session_id));
    QVERIFY(actual_argument_snapshot.has_value());
    QVERIFY(counterfactual_argument_snapshot.has_value());
    QCOMPARE(actual_argument_snapshot->sequence, qint64{2});
    QCOMPARE(counterfactual_argument_snapshot->sequence, qint64{2});
    QVERIFY(actual_argument_snapshot->session_id != counterfactual_argument_snapshot->session_id);
}

} // namespace

QTEST_MAIN(CinderLakeWritUiE2eTest)

#include "tst_m4_cinderlake_writ_ui_e2e.moc"
