#include "appellate/packs/pack_archive.hpp"
#include "appellate/packs/pack_catalog.hpp"
#include "appellate/packs/runtime_pack.hpp"
#include "appellate/storage/asset_store.hpp"
#include "appellate/storage/session_store.hpp"
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
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QPdfSearchModel>
#include <QPlainTextEdit>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <QVariant>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef APPELLATE_M4_ARM_ROOT
#error "APPELLATE_M4_ARM_ROOT must name content/m4/arm-agency"
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

constexpr auto root_digest = "bd1bd37e1e99ecb8239fa41b040aa72a0a856dd012442bcc1061b7d137e6651d";
constexpr auto federal_digest = "866c90996c15e2076b9508a297ffce1a4e766b1432a9e11d08e8138c57e363c9";
constexpr auto ca4_digest = "449d75c77e5c47883f750377450f2d1ec1fc0e42e20b1f247446b208661d3262";
constexpr auto bench_digest = "cee0bf93309cc9ad800f215a47d734b20a9fdf5dc889f2f440e4382b942d332d";
constexpr auto workflow_session_id = "ca4m4.arm.session.ui-workflow";
constexpr auto actual_session_id = "ca4m4.arm.session.ui-actual";
constexpr auto counterfactual_session_id = "ca4m4.arm.session.ui-counterfactual";
constexpr auto workflow_engine_revision = "engine.workflow.arm-ui-e2e.1";
constexpr auto oral_engine_revision = "engine.oral.arm-ui-e2e.1";

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

[[nodiscard]] QByteArray snapshotBytes(const storage::SessionSnapshot& snapshot) {
    QByteArray encoded;
    appendFrame(encoded, QByteArrayView("appellate-workbench-arm-ui-snapshot-v1"));
    appendFrame(encoded, snapshot.session_id);
    appendFrame(encoded, snapshot.engine_revision);
    appendUint64(encoded, static_cast<std::uint64_t>(snapshot.authority_contract));
    appendUint64(encoded, static_cast<std::uint64_t>(snapshot.sequence));
    appendUint64(encoded, static_cast<std::uint64_t>(snapshot.pins.size()));
    for (const auto& pin : snapshot.pins) {
        appendFrame(encoded, pin.pack_id);
        appendFrame(encoded, pin.version);
        appendFrame(encoded, pin.digest);
    }
    appendUint64(encoded, static_cast<std::uint64_t>(snapshot.commands.size()));
    for (const auto& command : snapshot.commands) {
        appendFrame(encoded, command.command_id);
        appendUint64(encoded, static_cast<std::uint64_t>(command.expected_sequence));
        appendFrame(encoded, QByteArrayView(command.payload_json));
        appendFrame(encoded, command.recorded_at_utc);
    }
    appendUint64(encoded, static_cast<std::uint64_t>(snapshot.events.size()));
    for (const auto& event : snapshot.events) {
        appendUint64(encoded, static_cast<std::uint64_t>(event.sequence));
        appendFrame(encoded, event.event_type);
        appendFrame(encoded, QByteArrayView(event.payload_json));
        appendFrame(encoded, event.authority_id);
    }
    appendUint64(encoded, static_cast<std::uint64_t>(snapshot.docket.size()));
    for (const auto& docket : snapshot.docket) {
        appendFrame(encoded, docket.entry_id);
        appendUint64(encoded, static_cast<std::uint64_t>(docket.event_sequence));
        appendFrame(encoded, docket.title);
        appendFrame(encoded, docket.status);
    }
    appendUint64(encoded, static_cast<std::uint64_t>(snapshot.asset_references.size()));
    for (const auto& asset : snapshot.asset_references) {
        appendFrame(encoded, asset.digest);
        appendFrame(encoded, asset.purpose);
    }
    return encoded;
}

[[nodiscard]] auto persistedSnapshot(const QString& database_path, const QString& session_id)
    -> std::expected<storage::SessionSnapshot, QString> {
    auto store = storage::SessionStore::open(database_path);
    if (!store) {
        return std::unexpected(store.error().message);
    }
    auto loaded = (*store)->loadSession(session_id);
    if (!loaded) {
        return std::unexpected(loaded.error().message);
    }
    return *loaded;
}

[[nodiscard]] auto workflowDatabaseRows(const QString& database_path, const QString& session_id)
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
        QStringLiteral("arm-ui-e2e-rows-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    QByteArray encoded;
    QString failure;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name);
        database.setDatabaseName(database_path);
        if (!database.open()) {
            failure = database.lastError().text();
        } else {
            appendFrame(encoded, QByteArrayView("appellate-workbench-arm-ui-workflow-rows-v1"));
            for (const auto& specification : queries) {
                appendFrame(encoded, specification.name);
                QSqlQuery query(database);
                query.prepare(specification.sql);
                query.addBindValue(session_id);
                if (!query.exec()) {
                    failure = query.lastError().text();
                    break;
                }
                std::uint64_t row_count = 0;
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
        return std::unexpected(failure);
    }
    return encoded;
}

[[nodiscard]] model::LegalTime legalTime(int year, unsigned month, unsigned day) {
    const auto date = std::chrono::year{year} / std::chrono::month{month} / std::chrono::day{day};
    return model::LegalTime{std::chrono::sys_seconds{std::chrono::sys_days{date}},
                            model::LegalDate{date}};
}

class PersistedArmLaunchProvider final : public ui::OralArgumentLaunchProvider {
  public:
    struct Call final {
        model::PackRevision root_revision;
        model::CaseId case_id;
        packs::RuntimeArgumentConfigId configuration_id;
        std::optional<model::PackRevision> configuration_owner;
        std::vector<std::string> profile_ids;
    };

    PersistedArmLaunchProvider(QString database_path, std::string legal_state_digest,
                               model::PackRevision expected_revision)
        : database_path_(std::move(database_path)),
          legal_state_digest_(std::move(legal_state_digest)),
          expected_revision_(std::move(expected_revision)) {}

    [[nodiscard]] auto open(const packs::ResolvedPack& resolved_pack, const model::CaseId& case_id,
                            const packs::RuntimeArgumentConfigId& argument_configuration_id)
        -> std::expected<std::unique_ptr<app::OralArgumentSessionController>,
                         app::OralArgumentSessionError> override {
        if (resolved_pack.root().revision != expected_revision_ ||
            case_id.value != "ca4m4.case.arm-agency") {
            return fail(QStringLiteral("Exact ARM revision or case ID changed"));
        }
        const auto runtime = packs::loadRuntimePack(resolved_pack);
        if (!runtime) {
            return fail(QString::fromStdString(runtime.error().message));
        }
        const auto runtime_case =
            std::ranges::find_if(runtime->cases, [&case_id](const auto& candidate) {
                return candidate.definition.id == case_id;
            });
        if (runtime_case == runtime->cases.end()) {
            return fail(QStringLiteral("Exact case ID did not resolve"));
        }
        const auto configuration =
            std::ranges::find(runtime_case->argument_configurations, argument_configuration_id,
                              &packs::RuntimeArgumentConfiguration::id);
        if (configuration == runtime_case->argument_configurations.end()) {
            return fail(QStringLiteral("Exact configuration ID did not resolve"));
        }
        std::vector<std::string> profile_ids;
        profile_ids.reserve(configuration->bench.seats.size());
        for (const auto& seat : configuration->bench.seats) {
            profile_ids.push_back(seat.profile.id);
        }
        calls.push_back(Call{
            resolved_pack.root().revision,
            case_id,
            argument_configuration_id,
            resolved_pack.resourceOwner(argument_configuration_id.value),
            std::move(profile_ids),
        });

        QString session_id;
        if (argument_configuration_id.value == "ca4m4.arm.argument.actual-record") {
            session_id = QString::fromLatin1(actual_session_id);
        } else if (argument_configuration_id.value == "ca4m4.arm.argument.counterfactual") {
            session_id = QString::fromLatin1(counterfactual_session_id);
        } else {
            return fail(QStringLiteral("Unknown exact ARM argument configuration"));
        }

        auto store = storage::SessionStore::open(database_path_);
        if (!store) {
            return fail(store.error().message,
                        app::OralArgumentSessionErrorCode::SessionStoreFailure);
        }
        const auto existing = (*store)->loadSession(session_id);
        if (!existing && existing.error().code != storage::StoreErrorCode::NotFound) {
            return fail(existing.error().message,
                        app::OralArgumentSessionErrorCode::SessionStoreFailure);
        }
        if (existing) {
            ++reopen_attempts_;
            return app::OralArgumentSessionController::reopen(
                session_id, case_id, argument_configuration_id, legal_state_digest_,
                std::move(*store), QString::fromLatin1(oral_engine_revision), resolved_pack);
        }
        ++create_attempts_;
        return app::OralArgumentSessionController::create(
            session_id, case_id, argument_configuration_id, legal_state_digest_, std::move(*store),
            QString::fromLatin1(oral_engine_revision), QStringLiteral("2026-08-11T10:00:00Z"),
            resolved_pack);
    }

    [[nodiscard]] int createAttempts() const noexcept { return create_attempts_; }
    [[nodiscard]] int reopenAttempts() const noexcept { return reopen_attempts_; }

    std::vector<Call> calls;

  private:
    [[nodiscard]] static auto fail(QString message,
                                   app::OralArgumentSessionErrorCode code =
                                       app::OralArgumentSessionErrorCode::InvalidConfiguration)
        -> std::unexpected<app::OralArgumentSessionError> {
        return std::unexpected(app::OralArgumentSessionError{code, std::move(message)});
    }

    QString database_path_;
    std::string legal_state_digest_;
    model::PackRevision expected_revision_;
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
    QVERIFY(workspace.canonicalDefinition() != nullptr);
    QVERIFY(workspace.sessionState() != nullptr);
    QVERIFY(workspace.groundingTable()->rowCount() >= 2);
    for (int row = 0; row < workspace.groundingTable()->rowCount(); ++row) {
        workspace.groundingTable()->item(row, 0)->setCheckState(Qt::Checked);
    }
    workspace.answerKindSelector()->setCurrentIndex(2);
    workspace.answerEditor()->setPlainText(answer);
    const auto submitted = workspace.submitAnswer();
    QVERIFY2(submitted.has_value(), submitted ? "" : qPrintable(submitted.error()));
    QCOMPARE(workspace.sessionState()->journal.size(), std::size_t{2});
    QVERIFY(workspace.transcriptView()->toPlainText().contains(answer));
}

class M4ArmAgencyUiE2eTest final : public QObject {
    Q_OBJECT

  private slots:
    void persistsExactArgumentsAndKeepsWorkflowIsolated();
};

void M4ArmAgencyUiE2eTest::persistsExactArgumentsAndKeepsWorkflowIsolated() {
    const model::PackRevision expected_root{model::PackId{"us.ca4.m4.arm-agency"}, "1.1.0",
                                            root_digest};
    const model::PackRevision expected_federal{model::PackId{"foundation.us-federal"}, "2025.12.01",
                                               federal_digest};
    const model::PackRevision expected_ca4{model::PackId{"foundation.us-ca4"}, "2026.03.23",
                                           ca4_digest};
    const model::PackRevision expected_bench{model::PackId{"foundation.us-ca4-fictional-bench"},
                                             "1.0.0", bench_digest};

    QTemporaryDir state;
    QVERIFY(state.isValid());
    const auto catalog_root = QDir(state.path()).filePath(QStringLiteral("catalog"));
    const auto root_archive = QDir(state.path()).filePath(QStringLiteral("arm.awpack"));
    const auto session_database = QDir(state.path()).filePath(QStringLiteral("sessions.sqlite"));
    const auto asset_root = QDir(state.path()).filePath(QStringLiteral("workflow-assets"));
    const auto exported = packs::PackArchive::exportDirectory(
        QDir(QStringLiteral(APPELLATE_M4_ARM_ROOT)).filePath(QStringLiteral("pack")), root_archive,
        {}, packs::PackValidationScope::ResolvedClosure);
    QVERIFY2(exported.has_value(), exported ? "" : qPrintable(exported.error().message));
    QCOMPARE(*exported, expected_root);

    {
        const auto catalog = packs::PackCatalog::open(catalog_root);
        QVERIFY2(catalog.has_value(), catalog ? "" : qPrintable(catalog.error().message));
        const QDir foundations(QStringLiteral(APPELLATE_M4_FOUNDATIONS));
        const auto federal =
            (*catalog)->installArchive(foundations.filePath(QStringLiteral(
                                           "us-federal/foundation-us-federal-2025.12.01.awpack")),
                                       QStringLiteral("2026-08-11T00:00:00Z"));
        const auto ca4 = (*catalog)->installArchive(
            foundations.filePath(QStringLiteral("us-ca4/foundation-us-ca4-2026.03.23.awpack")),
            QStringLiteral("2026-08-11T00:00:01Z"));
        const auto bench = (*catalog)->installArchive(
            foundations.filePath(QStringLiteral(
                "us-ca4-fictional-bench/foundation-us-ca4-fictional-bench-1.0.0.awpack")),
            QStringLiteral("2026-08-11T00:00:02Z"));
        const auto root =
            (*catalog)->installArchive(root_archive, QStringLiteral("2026-08-11T00:00:03Z"));
        QVERIFY2(federal.has_value(), federal ? "" : qPrintable(federal.error().message));
        QVERIFY2(ca4.has_value(), ca4 ? "" : qPrintable(ca4.error().message));
        QVERIFY2(bench.has_value(), bench ? "" : qPrintable(bench.error().message));
        QVERIFY2(root.has_value(), root ? "" : qPrintable(root.error().message));
        QCOMPARE(federal->revision, expected_federal);
        QCOMPARE(ca4->revision, expected_ca4);
        QCOMPARE(bench->revision, expected_bench);
        QCOMPARE(root->revision, expected_root);
    }

    const auto catalog = packs::PackCatalog::open(catalog_root);
    QVERIFY2(catalog.has_value(), catalog ? "" : qPrintable(catalog.error().message));
    const auto resolved = (*catalog)->loadResolved(expected_root);
    QVERIFY2(resolved.has_value(), resolved ? "" : qPrintable(resolved.error().message));
    const auto runtime = packs::loadRuntimePack(*resolved);
    QVERIFY2(runtime.has_value(), runtime ? "" : runtime.error().message.c_str());
    QCOMPARE(runtime->cases.size(), std::size_t{1});
    const auto& runtime_case = runtime->cases.front();

    model::WorkflowState workflow_initial;
    workflow_initial.session_id = workflow_session_id;
    workflow_initial.workflow_id = runtime_case.workflow.id;
    workflow_initial.current_stage_id = runtime_case.workflow.initial_stage_id;
    auto workflow_store = storage::SessionStore::open(session_database);
    QVERIFY2(workflow_store.has_value(),
             workflow_store ? "" : qPrintable(workflow_store.error().message));
    auto workflow = app::WorkflowSessionController::create(
        runtime_case.definition.id, workflow_initial, storage::AssetStore(asset_root),
        std::move(*workflow_store), QString::fromLatin1(workflow_engine_revision),
        QStringLiteral("2026-08-11T09:00:00Z"), *resolved);
    QVERIFY2(workflow.has_value(), workflow ? "" : qPrintable(workflow.error().message));
    const QByteArray petition_bytes("ARM UI E2E exact agency petition bytes");
    const auto petition_digest =
        QCryptographicHash::hash(petition_bytes, QCryptographicHash::Sha256).toHex().toStdString();
    const auto filed = (*workflow)->submit(
        model::WorkflowCommand{model::SubmitWorkflowFiling{
            model::WorkflowCommandHeader{
                workflow_session_id, model::WorkflowCommandId{"ca4m4.arm.command.ui-file-petition"},
                model::ActorId{"ca4m4.arm.actor.petitioner"}, legalTime(2025, 2U, 11U)},
            model::WorkflowFilingId{"ca4m4.arm.filing.ui-petition"},
            model::FilingTypeId{"us.ca4.filing.agency-petition-for-review"},
            petition_digest,
            {
                {model::FilingFieldId{"us.ca4.field.agency-petition.caption"}, "present"},
                {model::FilingFieldId{"us.ca4.field.agency-petition.parties-seeking-review"},
                 "present"},
                {model::FilingFieldId{"us.ca4.field.agency-petition.agency"}, "present"},
                {model::FilingFieldId{"us.ca4.field.agency-petition.order-reference"}, "present"},
                {model::FilingFieldId{"us.ca4.field.agency-petition.order-copy-attached"},
                 "present"},
                {model::FilingFieldId{"us.ca4.field.agency-petition.respondent-names-addresses"},
                 "present"},
            },
            {model::ActorId{"ca4m4.arm.actor.respondent"}},
            std::nullopt,
        }},
        QByteArrayView(petition_bytes), QStringLiteral("2026-08-11T09:01:00Z"));
    QVERIFY2(filed.has_value(), filed ? "" : qPrintable(filed.error().message));
    QVERIFY(filed->asset.has_value());
    QCOMPARE(filed->asset->sha256, QString::fromStdString(petition_digest));
    QCOMPARE(filed->asset->size, static_cast<qint64>(petition_bytes.size()));
    const auto persisted_petition =
        storage::AssetStore(asset_root).read(QString::fromStdString(petition_digest));
    QVERIFY2(persisted_petition.has_value(),
             persisted_petition ? "" : qPrintable(persisted_petition.error().message));
    QCOMPARE(*persisted_petition, petition_bytes);
    QCOMPARE((*workflow)->state().current_stage_id,
             model::WorkflowStageId{"ca4m4.arm.stage.record"});
    QCOMPARE((*workflow)->journal().size(), std::size_t{1});
    QCOMPARE((*workflow)->snapshot().commands.size(), std::size_t{1});
    QCOMPARE((*workflow)->snapshot().events.size(), std::size_t{2});
    QCOMPARE((*workflow)->snapshot().docket.size(), std::size_t{2});
    QCOMPARE((*workflow)->snapshot().asset_references.size(), std::size_t{1});
    const auto workflow_state_before = (*workflow)->state();
    const auto workflow_journal_before = (*workflow)->journal();
    const auto workflow_snapshot_before = snapshotBytes((*workflow)->snapshot());
    (*workflow).reset();
    const auto workflow_rows_before =
        workflowDatabaseRows(session_database, QString::fromLatin1(workflow_session_id));
    QVERIFY2(workflow_rows_before.has_value(),
             workflow_rows_before ? "" : qPrintable(workflow_rows_before.error()));
    const auto legal_state_digest =
        QCryptographicHash::hash(workflow_snapshot_before, QCryptographicHash::Sha256)
            .toHex()
            .toStdString();
    QCOMPARE(legal_state_digest.size(), std::size_t{64});

    auto provider = std::make_shared<PersistedArmLaunchProvider>(session_database,
                                                                 legal_state_digest, expected_root);
    const std::vector<std::string> expected_profiles{
        "us.ca4.bench-profile.rowan",
        "us.ca4.bench-profile.reed",
        "us.ca4.bench-profile.quill",
    };

    std::optional<model::OralArgumentState> actual_state;
    std::optional<model::OralArgumentState> counterfactual_state;
    QString actual_transcript;
    QString counterfactual_transcript;
    {
        ui::MainWindow window({}, catalog_root, nullptr, provider);
        const auto loaded = window.loadSource(root_archive);
        QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error()));
        QVERIFY(window.currentRuntime() != nullptr);
        QCOMPARE(window.currentRuntime()->revision, expected_root);
        QCOMPARE(window.currentRuntime()->cases.size(), std::size_t{1});
        QCOMPARE(window.caseList()->count(), 1);
        QCOMPARE(window.argumentConfigurationSelector()->count(), 2);
        const auto actual_index = configurationIndex(window, "ca4m4.arm.argument.actual-record");
        const auto counterfactual_index =
            configurationIndex(window, "ca4m4.arm.argument.counterfactual");
        QVERIFY(actual_index >= 0);
        QVERIFY(counterfactual_index >= 0);

        const auto select_profiles = [&](int configuration_index) {
            window.argumentConfigurationSelector()->setCurrentIndex(configuration_index);
            QCOMPARE(window.profileSelector()->count(), 3);
            for (int index = 0; index < window.profileSelector()->count(); ++index) {
                window.profileSelector()->setCurrentIndex(index);
                const auto profile = window.profileEditor()->profile();
                QVERIFY(profile.has_value());
                QCOMPARE(profile->id, expected_profiles.at(static_cast<std::size_t>(index)));
            }
        };

        select_profiles(actual_index);
        const auto actual_launch = window.openSelectedOralArgument();
        QVERIFY2(actual_launch.has_value(), actual_launch ? "" : qPrintable(actual_launch.error()));
        QVERIFY(window.oralArgumentWorkspace() != nullptr);
        QCOMPARE(window.oralArgumentWorkspace()
                     ->canonicalDefinition()
                     ->question_bank.argument_configuration_id,
                 std::string("ca4m4.arm.argument.actual-record"));
        QCOMPARE(
            window.oralArgumentWorkspace()->canonicalDefinition()->configuration.legal_state_digest,
            legal_state_digest);
        submitGroundedAnswer(
            *window.oralArgumentWorkspace(),
            QStringLiteral(
                "The certified AR pages and exact authorities define the present record."));
        actual_state = *window.oralArgumentWorkspace()->sessionState();
        actual_transcript = window.oralArgumentWorkspace()->transcriptView()->toPlainText();

        const auto opened_record = window.openSelectedRecord();
        QVERIFY2(opened_record.has_value(), opened_record ? "" : qPrintable(opened_record.error()));
        auto* record_workspace = window.recordWorkspace();
        QVERIFY(record_workspace != nullptr);
        auto* pdf_search = record_workspace->findChild<QPdfSearchModel*>();
        QVERIFY(pdf_search != nullptr);
        QCOMPARE(record_workspace->visibleDocketCount(), qsizetype{19});
        record_workspace->setDocketFilter(QStringLiteral("ca4m4.arm.docket.agency"));
        QCOMPARE(record_workspace->visibleDocketCount(), qsizetype{18});
        record_workspace->setDocketFilter(QStringLiteral("ca4m4.arm.docket.ca4"));
        QCOMPARE(record_workspace->visibleDocketCount(), qsizetype{1});
        record_workspace->setDocketFilter(QStringLiteral("generated_appellate_filing"));
        QCOMPARE(record_workspace->visibleDocketCount(), qsizetype{1});
        record_workspace->setDocketFilter({});
        QCOMPARE(record_workspace->visibleDocketCount(), qsizetype{19});

        const auto ar_anchor = record_workspace->navigateToCitation(QStringLiteral("AR33"));
        QVERIFY2(ar_anchor.has_value(), ar_anchor ? "" : qPrintable(ar_anchor.error().message));
        QCOMPARE(record_workspace->currentDocumentId(), QStringLiteral("ca4m4.arm.record.ar04"));
        QCOMPARE(record_workspace->loadedPageCount(), 18);
        QCOMPARE(record_workspace->currentPageIndex(), 0);
        record_workspace->setDocumentSearch(QStringLiteral("Agency Exhibit P-7"));
        QTRY_VERIFY_WITH_TIMEOUT(record_workspace->documentSearchResultCount() > 0, 10'000);
        QTRY_VERIFY_WITH_TIMEOUT(
            !pdf_search->resultsOnPage(record_workspace->currentPageIndex()).isEmpty(), 10'000);

        const auto admission_anchor = record_workspace->navigateToCitation(QStringLiteral("AR117"));
        QVERIFY2(admission_anchor.has_value(),
                 admission_anchor ? "" : qPrintable(admission_anchor.error().message));
        QCOMPARE(record_workspace->currentDocumentId(), QStringLiteral("ca4m4.arm.record.ar10"));
        QCOMPARE(record_workspace->loadedPageCount(), 24);
        QCOMPARE(record_workspace->currentPageIndex(), 2);
        record_workspace->setDocumentSearch(QStringLiteral("admits P-7"));
        QTRY_VERIFY_WITH_TIMEOUT(record_workspace->documentSearchResultCount() > 0, 10'000);
        QTRY_VERIFY_WITH_TIMEOUT(
            !pdf_search->resultsOnPage(record_workspace->currentPageIndex()).isEmpty(), 10'000);

        const auto p4_object_anchor = record_workspace->navigateToCitation(QStringLiteral("AR106"));
        QVERIFY2(p4_object_anchor.has_value(),
                 p4_object_anchor ? "" : qPrintable(p4_object_anchor.error().message));
        QCOMPARE(record_workspace->currentDocumentId(), QStringLiteral("ca4m4.arm.record.ar09"));
        QCOMPARE(record_workspace->loadedPageCount(), 10);
        QCOMPARE(record_workspace->currentPageIndex(), 1);
        record_workspace->setDocumentSearch(QStringLiteral("KAL-MSG-1"));
        QTRY_VERIFY_WITH_TIMEOUT(record_workspace->documentSearchResultCount() > 0, 10'000);
        QTRY_VERIFY_WITH_TIMEOUT(
            !pdf_search->resultsOnPage(record_workspace->currentPageIndex()).isEmpty(), 10'000);

        const auto p5_object_anchor = record_workspace->navigateToCitation(QStringLiteral("AR109"));
        QVERIFY2(p5_object_anchor.has_value(),
                 p5_object_anchor ? "" : qPrintable(p5_object_anchor.error().message));
        QCOMPARE(record_workspace->currentDocumentId(), QStringLiteral("ca4m4.arm.record.ar09"));
        QCOMPARE(record_workspace->currentPageIndex(), 4);
        record_workspace->setDocumentSearch(QStringLiteral("KAL-ROUTE-1"));
        QTRY_VERIFY_WITH_TIMEOUT(record_workspace->documentSearchResultCount() > 0, 10'000);
        QTRY_VERIFY_WITH_TIMEOUT(
            !pdf_search->resultsOnPage(record_workspace->currentPageIndex()).isEmpty(), 10'000);

        const auto stipulation_anchor =
            record_workspace->navigateToCitation(QStringLiteral("AR227"));
        QVERIFY2(stipulation_anchor.has_value(),
                 stipulation_anchor ? "" : qPrintable(stipulation_anchor.error().message));
        QCOMPARE(record_workspace->currentDocumentId(), QStringLiteral("ca4m4.arm.record.ar18"));
        QCOMPARE(record_workspace->loadedPageCount(), 12);
        QCOMPARE(record_workspace->currentPageIndex(), 0);
        record_workspace->setDocumentSearch(QStringLiteral("STIPULATION-16B"));
        QTRY_VERIFY_WITH_TIMEOUT(record_workspace->documentSearchResultCount() > 0, 10'000);
        QTRY_VERIFY_WITH_TIMEOUT(
            !pdf_search->resultsOnPage(record_workspace->currentPageIndex()).isEmpty(), 10'000);

        const auto receipt_anchor = record_workspace->navigateToCitation(QStringLiteral("AR229"));
        QVERIFY2(receipt_anchor.has_value(),
                 receipt_anchor ? "" : qPrintable(receipt_anchor.error().message));
        QCOMPARE(record_workspace->currentDocumentId(), QStringLiteral("ca4m4.arm.record.ar18"));
        QCOMPARE(record_workspace->currentPageIndex(), 2);
        record_workspace->setDocumentSearch(
            QStringLiteral("every exhibit P-1 through P-9 admitted"));
        QTRY_VERIFY_WITH_TIMEOUT(record_workspace->documentSearchResultCount() > 0, 10'000);
        QTRY_VERIFY_WITH_TIMEOUT(
            !pdf_search->resultsOnPage(record_workspace->currentPageIndex()).isEmpty(), 10'000);

        const auto correction_anchor =
            record_workspace->navigateToCitation(QStringLiteral("AR232"));
        QVERIFY2(correction_anchor.has_value(),
                 correction_anchor ? "" : qPrintable(correction_anchor.error().message));
        QCOMPARE(record_workspace->currentDocumentId(), QStringLiteral("ca4m4.arm.record.ar18"));
        QCOMPARE(record_workspace->loadedPageCount(), 12);
        QCOMPARE(record_workspace->currentPageIndex(), 5);
        record_workspace->setDocumentSearch(QStringLiteral("without joining the hearing receipt"));
        QTRY_VERIFY_WITH_TIMEOUT(record_workspace->documentSearchResultCount() > 0, 10'000);
        QTRY_VERIFY_WITH_TIMEOUT(
            !pdf_search->resultsOnPage(record_workspace->currentPageIndex()).isEmpty(), 10'000);

        const auto exclusion_anchor = record_workspace->navigateToCitation(QStringLiteral("AR235"));
        QVERIFY2(exclusion_anchor.has_value(),
                 exclusion_anchor ? "" : qPrintable(exclusion_anchor.error().message));
        QCOMPARE(record_workspace->currentDocumentId(), QStringLiteral("ca4m4.arm.record.ar18"));
        QCOMPARE(record_workspace->currentPageIndex(), 8);
        record_workspace->setDocumentSearch(QStringLiteral("appellate proffer pages 1–8"));
        QTRY_VERIFY_WITH_TIMEOUT(record_workspace->documentSearchResultCount() > 0, 10'000);
        QTRY_VERIFY_WITH_TIMEOUT(
            !pdf_search->resultsOnPage(record_workspace->currentPageIndex()).isEmpty(), 10'000);

        const auto pa_anchor = record_workspace->navigateToCitation(QStringLiteral("PA1"));
        QVERIFY2(pa_anchor.has_value(), pa_anchor ? "" : qPrintable(pa_anchor.error().message));
        QCOMPARE(record_workspace->currentDocumentId(), QStringLiteral("ca4m4.arm.record.pa01"));
        QCOMPARE(record_workspace->loadedPageCount(), 8);
        QCOMPARE(record_workspace->currentPageIndex(), 0);
        record_workspace->setDocumentSearch(QStringLiteral("extra-record proffer"));
        QTRY_VERIFY_WITH_TIMEOUT(record_workspace->documentSearchResultCount() > 0, 10'000);
        QTRY_VERIFY_WITH_TIMEOUT(
            !pdf_search->resultsOnPage(record_workspace->currentPageIndex()).isEmpty(), 10'000);
    }

    {
        ui::MainWindow window({}, catalog_root, nullptr, provider);
        const auto loaded = window.loadSource(root_archive);
        QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error()));
        const auto counterfactual_index =
            configurationIndex(window, "ca4m4.arm.argument.counterfactual");
        QVERIFY(counterfactual_index >= 0);
        window.argumentConfigurationSelector()->setCurrentIndex(counterfactual_index);
        QCOMPARE(window.profileSelector()->count(), 3);
        for (int index = 0; index < window.profileSelector()->count(); ++index) {
            window.profileSelector()->setCurrentIndex(index);
            const auto profile = window.profileEditor()->profile();
            QVERIFY(profile.has_value());
            QCOMPARE(profile->id, expected_profiles.at(static_cast<std::size_t>(index)));
        }
        const auto counterfactual_launch = window.openSelectedOralArgument();
        QVERIFY2(counterfactual_launch.has_value(),
                 counterfactual_launch ? "" : qPrintable(counterfactual_launch.error()));
        QCOMPARE(window.oralArgumentWorkspace()
                     ->canonicalDefinition()
                     ->question_bank.argument_configuration_id,
                 std::string("ca4m4.arm.argument.counterfactual"));
        QCOMPARE(
            window.oralArgumentWorkspace()->canonicalDefinition()->configuration.legal_state_digest,
            legal_state_digest);
        QVERIFY(window.oralArgumentWorkspace()->isolationNoticeLabel()->text().contains(
            QStringLiteral("isolated from the actual-record workflow")));
        submitGroundedAnswer(
            *window.oralArgumentWorkspace(),
            QStringLiteral("The counterfactual changes the premise without changing exact pins."));
        counterfactual_state = *window.oralArgumentWorkspace()->sessionState();
        counterfactual_transcript = window.oralArgumentWorkspace()->transcriptView()->toPlainText();
    }

    const auto actual_snapshot =
        persistedSnapshot(session_database, QString::fromLatin1(actual_session_id));
    const auto counterfactual_snapshot =
        persistedSnapshot(session_database, QString::fromLatin1(counterfactual_session_id));
    QVERIFY2(actual_snapshot.has_value(),
             actual_snapshot ? "" : qPrintable(actual_snapshot.error()));
    QVERIFY2(counterfactual_snapshot.has_value(),
             counterfactual_snapshot ? "" : qPrintable(counterfactual_snapshot.error()));
    const std::array expected_pins{expected_root, expected_federal, expected_ca4, expected_bench};
    for (const auto* snapshot : {&*actual_snapshot, &*counterfactual_snapshot}) {
        QCOMPARE(snapshot->authority_contract, storage::SessionAuthorityContract::CanonicalV2);
        QCOMPARE(snapshot->sequence, qint64{2});
        QCOMPARE(snapshot->pins.size(), std::size_t{4});
        QCOMPARE(snapshot->commands.size(), std::size_t{2});
        QCOMPARE(snapshot->events.size(), std::size_t{2});
        QVERIFY(snapshot->docket.empty());
        QVERIFY(snapshot->asset_references.empty());
        for (const auto& expected_pin : expected_pins) {
            QVERIFY(std::ranges::any_of(snapshot->pins, [&](const auto& pin) {
                return pin.pack_id == QString::fromStdString(expected_pin.id.value) &&
                       pin.version == QString::fromStdString(expected_pin.version) &&
                       pin.digest == QString::fromStdString(expected_pin.digest);
            }));
        }
    }
    const auto actual_snapshot_bytes = snapshotBytes(*actual_snapshot);
    const auto counterfactual_snapshot_bytes = snapshotBytes(*counterfactual_snapshot);
    const auto actual_rows =
        workflowDatabaseRows(session_database, QString::fromLatin1(actual_session_id));
    const auto counterfactual_rows =
        workflowDatabaseRows(session_database, QString::fromLatin1(counterfactual_session_id));
    QVERIFY2(actual_rows.has_value(), actual_rows ? "" : qPrintable(actual_rows.error()));
    QVERIFY2(counterfactual_rows.has_value(),
             counterfactual_rows ? "" : qPrintable(counterfactual_rows.error()));
    QVERIFY(QFileInfo(session_database).isFile());
    QVERIFY(QFileInfo(session_database).size() > 0);

    {
        ui::MainWindow window({}, catalog_root, nullptr, provider);
        const auto loaded = window.loadSource(root_archive);
        QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error()));
        const auto actual_index = configurationIndex(window, "ca4m4.arm.argument.actual-record");
        QVERIFY(actual_index >= 0);
        window.argumentConfigurationSelector()->setCurrentIndex(actual_index);
        const auto actual_reopen = window.openSelectedOralArgument();
        QVERIFY2(actual_reopen.has_value(), actual_reopen ? "" : qPrintable(actual_reopen.error()));
        QCOMPARE(*window.oralArgumentWorkspace()->sessionState(), *actual_state);
        QCOMPARE(window.oralArgumentWorkspace()->transcriptView()->toPlainText(),
                 actual_transcript);
    }

    {
        ui::MainWindow window({}, catalog_root, nullptr, provider);
        const auto loaded = window.loadSource(root_archive);
        QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error()));
        const auto counterfactual_index =
            configurationIndex(window, "ca4m4.arm.argument.counterfactual");
        QVERIFY(counterfactual_index >= 0);
        window.argumentConfigurationSelector()->setCurrentIndex(counterfactual_index);
        const auto counterfactual_reopen = window.openSelectedOralArgument();
        QVERIFY2(counterfactual_reopen.has_value(),
                 counterfactual_reopen ? "" : qPrintable(counterfactual_reopen.error()));
        QCOMPARE(*window.oralArgumentWorkspace()->sessionState(), *counterfactual_state);
        QCOMPARE(window.oralArgumentWorkspace()->transcriptView()->toPlainText(),
                 counterfactual_transcript);
    }

    const auto actual_reopened_snapshot =
        persistedSnapshot(session_database, QString::fromLatin1(actual_session_id));
    const auto counterfactual_reopened_snapshot =
        persistedSnapshot(session_database, QString::fromLatin1(counterfactual_session_id));
    QVERIFY(actual_reopened_snapshot.has_value());
    QVERIFY(counterfactual_reopened_snapshot.has_value());
    QCOMPARE(snapshotBytes(*actual_reopened_snapshot), actual_snapshot_bytes);
    QCOMPARE(snapshotBytes(*counterfactual_reopened_snapshot), counterfactual_snapshot_bytes);
    const auto actual_rows_after =
        workflowDatabaseRows(session_database, QString::fromLatin1(actual_session_id));
    const auto counterfactual_rows_after =
        workflowDatabaseRows(session_database, QString::fromLatin1(counterfactual_session_id));
    QVERIFY(actual_rows_after.has_value());
    QVERIFY(counterfactual_rows_after.has_value());
    QCOMPARE(*actual_rows_after, *actual_rows);
    QCOMPARE(*counterfactual_rows_after, *counterfactual_rows);

    auto workflow_reopen_store = storage::SessionStore::open(session_database);
    QVERIFY2(workflow_reopen_store.has_value(),
             workflow_reopen_store ? "" : qPrintable(workflow_reopen_store.error().message));
    auto reopened_workflow = app::WorkflowSessionController::reopen(
        runtime_case.definition.id, workflow_initial, storage::AssetStore(asset_root),
        std::move(*workflow_reopen_store), QString::fromLatin1(workflow_engine_revision),
        *resolved);
    QVERIFY2(reopened_workflow.has_value(),
             reopened_workflow ? "" : qPrintable(reopened_workflow.error().message));
    QCOMPARE((*reopened_workflow)->state(), workflow_state_before);
    QCOMPARE((*reopened_workflow)->journal(), workflow_journal_before);
    QCOMPARE(snapshotBytes((*reopened_workflow)->snapshot()), workflow_snapshot_before);
    (*reopened_workflow).reset();
    const auto workflow_rows_after =
        workflowDatabaseRows(session_database, QString::fromLatin1(workflow_session_id));
    QVERIFY(workflow_rows_after.has_value());
    QCOMPARE(*workflow_rows_after, *workflow_rows_before);

    QCOMPARE(provider->createAttempts(), 2);
    QCOMPARE(provider->reopenAttempts(), 2);
    QCOMPARE(provider->calls.size(), std::size_t{4});
    QCOMPARE(provider->calls.at(0).root_revision, expected_root);
    QCOMPARE(provider->calls.at(0).case_id, runtime_case.definition.id);
    QCOMPARE(provider->calls.at(0).configuration_id.value,
             std::string("ca4m4.arm.argument.actual-record"));
    QCOMPARE(provider->calls.at(1).configuration_id.value,
             std::string("ca4m4.arm.argument.counterfactual"));
    for (const auto& call : provider->calls) {
        QCOMPARE(call.configuration_owner, std::optional<model::PackRevision>{expected_root});
        QVERIFY(call.profile_ids == expected_profiles);
    }
}

} // namespace

QTEST_MAIN(M4ArmAgencyUiE2eTest)
#include "tst_m4_arm_agency_ui_e2e.moc"
