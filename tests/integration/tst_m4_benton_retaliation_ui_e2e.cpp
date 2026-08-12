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

#include <QAction>
#include <QByteArrayView>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QLabel>
#include <QListWidget>
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
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef APPELLATE_M4_BENTON_ROOT
#error "APPELLATE_M4_BENTON_ROOT must name content/m4/benton-retaliation"
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

constexpr auto root_digest = "eaf5f52940d968f33a3b3501e20414081f7f3573d90ba1abb7c3b2f33636ad4e";
constexpr auto federal_digest = "866c90996c15e2076b9508a297ffce1a4e766b1432a9e11d08e8138c57e363c9";
constexpr auto ca4_digest = "449d75c77e5c47883f750377450f2d1ec1fc0e42e20b1f247446b208661d3262";
constexpr auto bench_digest = "cee0bf93309cc9ad800f215a47d734b20a9fdf5dc889f2f440e4382b942d332d";
constexpr auto exclusion_issue = "ca4m4.benton.issue.late-comparator-declaration-exclusion";
constexpr auto actual_session_id = "ca4m4.benton.session.oral.actual";
constexpr auto counterfactual_session_id = "ca4m4.benton.session.oral.counterfactual";
constexpr auto oral_engine_revision = "engine.oral.benton-e2e.1";
constexpr auto workflow_session_id = "ca4m4.benton.session.workflow.e2e";
constexpr auto workflow_engine_revision = "engine.workflow.benton-e2e.1";

[[nodiscard]] QByteArray sha256(const QByteArray& bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
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

[[nodiscard]] QByteArray snapshotBytes(const storage::SessionSnapshot& snapshot) {
    QByteArray encoded;
    appendFrame(encoded, QByteArrayView("appellate-workbench-e2e-session-snapshot-v1"));
    appendFrame(encoded, snapshot.session_id);
    appendFrame(encoded, snapshot.engine_revision);
    appendUint64(encoded, static_cast<std::uint64_t>(snapshot.authority_contract));
    appendUint64(encoded, static_cast<std::uint64_t>(snapshot.sequence));
    appendFrame(encoded, snapshot.created_at_utc);
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
        QStringLiteral("benton-ui-e2e-rows-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    QByteArray encoded;
    QString failure;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name);
        database.setDatabaseName(database_path);
        if (!database.open()) {
            failure = database.lastError().text();
        } else {
            appendFrame(encoded, QByteArrayView("appellate-workbench-benton-ui-workflow-rows-v1"));
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

[[nodiscard]] model::LegalDate date(int year, unsigned month, unsigned day) {
    return model::LegalDate{std::chrono::year{year} / std::chrono::month{month} /
                            std::chrono::day{day}};
}

[[nodiscard]] model::LegalTime at(model::LegalDate court_date) {
    return model::LegalTime{std::chrono::sys_seconds{std::chrono::sys_days{court_date.value}},
                            court_date};
}

[[nodiscard]] model::WorkflowState initialWorkflowState(const packs::RuntimeCase& runtime_case) {
    model::WorkflowState state;
    state.session_id = workflow_session_id;
    state.workflow_id = runtime_case.workflow.id;
    state.current_stage_id = runtime_case.workflow.initial_stage_id;
    return state;
}

class PersistedLaunchProvider final : public ui::OralArgumentLaunchProvider {
  public:
    PersistedLaunchProvider(std::string legal_state_digest, model::PackRevision expected_revision,
                            std::unique_ptr<storage::SessionStore> owner_store)
        : legal_state_digest_(std::move(legal_state_digest)),
          expected_revision_(std::move(expected_revision)), owner_store_(std::move(owner_store)) {}

    [[nodiscard]] auto open(const packs::ResolvedPack& resolved_pack, const model::CaseId& case_id,
                            const packs::RuntimeArgumentConfigId& argument_configuration_id)
        -> std::expected<std::unique_ptr<app::OralArgumentSessionController>,
                         app::OralArgumentSessionError> override {
        if (resolved_pack.root().revision != expected_revision_ ||
            resolved_pack.resourceOwner(argument_configuration_id.value) !=
                std::optional<model::PackRevision>{expected_revision_}) {
            return fail(QStringLiteral("Benton launch did not retain exact root ownership"));
        }
        QString session_id;
        if (argument_configuration_id.value == "ca4m4.benton.argument.actual-record") {
            session_id = QString::fromLatin1(actual_session_id);
        } else if (argument_configuration_id.value ==
                   "ca4m4.benton.argument.no-knowledge-counterfactual") {
            session_id = QString::fromLatin1(counterfactual_session_id);
        } else {
            return fail(QStringLiteral("Unknown exact Benton argument configuration"));
        }

        auto store = owner_store_->forkConnection();
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
    [[nodiscard]] auto forkConnection()
        -> std::expected<std::unique_ptr<storage::SessionStore>, storage::StoreError> {
        return owner_store_->forkConnection();
    }

  private:
    [[nodiscard]] static auto fail(QString message,
                                   app::OralArgumentSessionErrorCode code =
                                       app::OralArgumentSessionErrorCode::InvalidConfiguration)
        -> std::unexpected<app::OralArgumentSessionError> {
        return std::unexpected(app::OralArgumentSessionError{code, std::move(message)});
    }

    std::string legal_state_digest_;
    model::PackRevision expected_revision_;
    std::unique_ptr<storage::SessionStore> owner_store_;
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

class M4BentonRetaliationUiE2eTest final : public QObject {
    Q_OBJECT

  private slots:
    void installsClosureAndExposesRecordArgumentsAndWorkflowBranches();
};

void M4BentonRetaliationUiE2eTest::installsClosureAndExposesRecordArgumentsAndWorkflowBranches() {
    const model::PackRevision expected_root{model::PackId{"us.ca4.m4.benton-retaliation"}, "1.1.0",
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
    const auto root_archive = QDir(state.path()).filePath(QStringLiteral("benton.awpack"));
    const auto database_path = QDir(state.path()).filePath(QStringLiteral("sessions.sqlite"));
    const auto asset_root = QDir(state.path()).filePath(QStringLiteral("workflow-assets"));
    const auto exported = packs::PackArchive::exportDirectory(
        QDir(QStringLiteral(APPELLATE_M4_BENTON_ROOT)).filePath(QStringLiteral("pack")),
        root_archive, {}, packs::PackValidationScope::ResolvedClosure);
    QVERIFY2(exported.has_value(), exported ? "" : qPrintable(exported.error().message));
    QCOMPARE(*exported, expected_root);

    model::WorkflowState workflow_initial;
    model::WorkflowState workflow_state_before;
    std::vector<model::WorkflowJournalEntry> workflow_journal_before;
    storage::SessionSnapshot workflow_snapshot_before;
    QByteArray workflow_rows_before;
    std::string workflow_legal_state_digest;
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

        const auto resolved = (*catalog)->loadResolved(expected_root);
        QVERIFY2(resolved.has_value(), resolved ? "" : qPrintable(resolved.error().message));
        const auto runtime = packs::loadRuntimePack(*resolved);
        QVERIFY2(runtime.has_value(), runtime ? "" : runtime.error().message.c_str());
        QCOMPARE(runtime->cases.size(), std::size_t{1});
        const auto& runtime_case = runtime->cases.front();
        workflow_initial = initialWorkflowState(runtime_case);
        auto workflow_store = storage::SessionStore::open(database_path);
        QVERIFY2(workflow_store.has_value(),
                 workflow_store ? "" : qPrintable(workflow_store.error().message));
        auto workflow = app::WorkflowSessionController::create(
            runtime_case.definition.id, workflow_initial, storage::AssetStore(asset_root),
            std::move(*workflow_store), QString::fromLatin1(workflow_engine_revision),
            QStringLiteral("2026-01-16T12:00:00Z"), *resolved);
        QVERIFY2(workflow.has_value(), workflow ? "" : qPrintable(workflow.error().message));
        const QByteArray notice_document("Benton UI E2E notice of appeal");
        const auto notice = model::SubmitWorkflowFiling{
            model::WorkflowCommandHeader{
                workflow_session_id,
                model::WorkflowCommandId{"ca4m4.benton.command.ui-e2e-notice"},
                model::ActorId{"ca4m4.benton.actor.leora-benton"},
                at(date(2026, 1U, 16U)),
            },
            model::WorkflowFilingId{"ca4m4.benton.filing.ui-e2e-notice"},
            model::FilingTypeId{"us.ca4.filing.civil-notice-of-appeal"},
            sha256(notice_document).toStdString(),
            {model::WorkflowFieldValue{model::FilingFieldId{"us.ca4.field.civil-notice.caption"},
                                       "Leora Benton v. Blue Cedar Compliance"},
             model::WorkflowFieldValue{
                 model::FilingFieldId{"us.ca4.field.civil-notice.appealing-parties"},
                 "Leora Benton"},
             model::WorkflowFieldValue{
                 model::FilingFieldId{"us.ca4.field.civil-notice.originating-docket"},
                 "SYN-EDVA-25-CV-0412"},
             model::WorkflowFieldValue{
                 model::FilingFieldId{"us.ca4.field.civil-notice.judgment-or-order"},
                 "Final summary-judgment order and judgment"},
             model::WorkflowFieldValue{model::FilingFieldId{"us.ca4.field.civil-notice.order-date"},
                                       "2025-12-19"},
             model::WorkflowFieldValue{
                 model::FilingFieldId{"us.ca4.field.civil-notice.destination-court"},
                 "United States Court of Appeals for the Fourth Circuit"}},
            {model::ActorId{"ca4m4.benton.actor.blue-cedar"}},
            std::nullopt,
        };
        const auto filed =
            (*workflow)->submit(model::WorkflowCommand{notice}, QByteArrayView(notice_document),
                                QStringLiteral("2026-01-16T12:01:00Z"));
        QVERIFY2(filed.has_value(), filed ? "" : qPrintable(filed.error().message));
        QVERIFY(filed->asset.has_value());
        const auto notice_digest = sha256(notice_document);
        QCOMPARE(filed->asset->sha256, QString::fromLatin1(notice_digest));
        QCOMPARE(filed->asset->size, static_cast<qint64>(notice_document.size()));
        const auto persisted_notice =
            storage::AssetStore(asset_root).read(QString::fromLatin1(notice_digest));
        QVERIFY2(persisted_notice.has_value(),
                 persisted_notice ? "" : qPrintable(persisted_notice.error().message));
        QCOMPARE(*persisted_notice, notice_document);
        workflow_state_before = (*workflow)->state();
        workflow_journal_before = (*workflow)->journal();
        workflow_snapshot_before = (*workflow)->snapshot();
        QCOMPARE(workflow_snapshot_before.commands.size(), std::size_t{1});
        QCOMPARE(workflow_snapshot_before.events.size(), std::size_t{1});
        QCOMPARE(workflow_snapshot_before.docket.size(), std::size_t{1});
        QCOMPARE(workflow_snapshot_before.asset_references.size(), std::size_t{1});
        (*workflow).reset();
        const auto persisted_rows =
            workflowDatabaseRows(database_path, QString::fromLatin1(workflow_session_id));
        QVERIFY2(persisted_rows.has_value(),
                 persisted_rows ? "" : qPrintable(persisted_rows.error()));
        workflow_rows_before = *persisted_rows;
        workflow_legal_state_digest = sha256(snapshotBytes(workflow_snapshot_before)).toStdString();
        QCOMPARE(workflow_legal_state_digest.size(), std::size_t{64});
    }

    auto owner_store = storage::SessionStore::open(database_path);
    QVERIFY2(owner_store.has_value(), owner_store ? "" : qPrintable(owner_store.error().message));
    storage::AssetStore paired_assets(asset_root);
    const auto recovered_assets = (*owner_store)->recoverAssetStore(paired_assets);
    QVERIFY2(recovered_assets.has_value(),
             recovered_assets ? "" : qPrintable(recovered_assets.error().message));
    auto provider = std::make_shared<PersistedLaunchProvider>(
        workflow_legal_state_digest, expected_root, std::move(*owner_store));
    std::optional<model::OralArgumentState> actual_state;
    std::optional<model::OralArgumentState> counterfactual_state;
    QString actual_transcript;
    QString counterfactual_transcript;

    {
        ui::MainWindow window({}, catalog_root, nullptr, provider);
        const auto loaded = window.loadSource(root_archive);
        QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error()));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.currentRuntime() != nullptr);
        QCOMPARE(window.currentRuntime()->revision, expected_root);
        QCOMPARE(window.currentRuntime()->cases.size(), std::size_t{1});
        QCOMPARE(window.caseList()->count(), 1);
        QCOMPARE(window.argumentConfigurationSelector()->count(), 2);
        QVERIFY(window.procedureSummaryLabel()->text().contains(QStringLiteral("civil appeal"),
                                                                Qt::CaseInsensitive));

        const auto& runtime_case = window.currentRuntime()->cases.front();
        const auto has_operation = [&](std::string_view id) {
            return std::ranges::contains(
                runtime_case.workflow.operations, id,
                [](const auto& operation) { return std::string_view(operation.id.value); });
        };
        for (const auto operation_id : {
                 "ca4m4.benton.operation.schedule-argument",
                 "ca4m4.benton.operation.enter-argument-held",
                 "ca4m4.benton.operation.advance-submitted-on-briefs",
                 "ca4m4.benton.operation.issue-judgment",
                 "ca4m4.benton.operation.issue-judgment-on-briefs",
                 "ca4m4.benton.operation.calculate-mandate-after-rehearing-time",
                 "ca4m4.benton.operation.calculate-mandate-after-rehearing-denial",
                 "ca4m4.benton.operation.issue-mandate-no-petition",
                 "ca4m4.benton.operation.issue-mandate-after-rehearing-denial",
                 "ca4m4.benton.operation.issue-mandate-shortened",
             }) {
            QVERIFY2(has_operation(operation_id), operation_id);
        }

        QString runtime_question_text;
        for (const auto& configuration : runtime_case.argument_configurations) {
            QVERIFY(configuration.grounded_question_bank.has_value());
            for (const auto& question : configuration.grounded_question_bank->questions) {
                runtime_question_text +=
                    QString::fromStdString(question.prompt) + QLatin1Char('\n');
                if (question.issue_id == exclusion_issue) {
                    QVERIFY(!QString::fromStdString(question.prompt)
                                 .contains(QStringLiteral("hypothet"), Qt::CaseInsensitive));
                }
            }
        }
        QVERIFY(runtime_question_text.contains(QStringLiteral("October 15"), Qt::CaseInsensitive));
        QVERIFY(runtime_question_text.contains(QStringLiteral("Benjamin"), Qt::CaseInsensitive));
        QVERIFY(runtime_question_text.contains(QStringLiteral("narrow exclusion"),
                                               Qt::CaseInsensitive));
        for (const auto& forbidden : {
                 QStringLiteral("instruction-to-conceal"),
                 QStringLiteral("directed concealment"),
                 QStringLiteral("affirm exclusion"),
                 QStringLiteral("vacate summary judgment"),
             }) {
            QVERIFY2(!runtime_question_text.contains(forbidden, Qt::CaseInsensitive),
                     qPrintable(forbidden));
        }

        const auto actual_index = configurationIndex(window, "ca4m4.benton.argument.actual-record");
        const auto counterfactual_index =
            configurationIndex(window, "ca4m4.benton.argument.no-knowledge-counterfactual");
        QVERIFY(actual_index >= 0);
        QVERIFY(counterfactual_index >= 0);
        const std::vector<std::string> expected_profiles{
            "us.ca4.bench-profile.rowan",
            "us.ca4.bench-profile.alder",
            "us.ca4.bench-profile.fen",
        };
        window.argumentConfigurationSelector()->setCurrentIndex(actual_index);
        QCOMPARE(window.profileSelector()->count(), 3);
        for (int index = 0; index < window.profileSelector()->count(); ++index) {
            window.profileSelector()->setCurrentIndex(index);
            const auto profile = window.profileEditor()->profile();
            QVERIFY(profile.has_value());
            QCOMPARE(profile->id, expected_profiles.at(static_cast<std::size_t>(index)));
        }
        const auto actual_launch = window.openSelectedOralArgument();
        QVERIFY2(actual_launch.has_value(), actual_launch ? "" : qPrintable(actual_launch.error()));
        QTRY_VERIFY(window.oralArgumentWorkspace()->isReady());
        QCOMPARE(window.oralArgumentWorkspace()
                     ->canonicalDefinition()
                     ->question_bank.argument_configuration_id,
                 std::string("ca4m4.benton.argument.actual-record"));
        QCOMPARE(
            window.oralArgumentWorkspace()->canonicalDefinition()->configuration.legal_state_digest,
            workflow_legal_state_digest);
        submitGroundedAnswer(
            *window.oralArgumentWorkspace(),
            QStringLiteral("The exact current JA pages and authorities frame the actual record."));
        actual_state = *window.oralArgumentWorkspace()->sessionState();
        actual_transcript = window.oralArgumentWorkspace()->transcriptView()->toPlainText();
        QVERIFY(actual_transcript.contains(QStringLiteral("exact current JA pages")));

        const auto opened_record = window.openSelectedRecord();
        QVERIFY2(opened_record.has_value(), opened_record ? "" : qPrintable(opened_record.error()));
        auto* workspace = window.recordWorkspace();
        QVERIFY(workspace != nullptr);
        QCOMPARE(workspace->visibleDocketCount(), qsizetype{37});
        workspace->setDocketFilter(QStringLiteral("ca4m4.benton.docket.district"));
        QCOMPARE(workspace->visibleDocketCount(), qsizetype{37});
        workspace->setDocketFilter(QStringLiteral("ca4m4.benton.docket.appellate"));
        QCOMPARE(workspace->visibleDocketCount(), qsizetype{0});
        workspace->setDocketFilter({});

        const auto workbook_anchor = workspace->navigateToCitation(QStringLiteral("JA70"));
        QVERIFY2(workbook_anchor.has_value(),
                 workbook_anchor ? "" : qPrintable(workbook_anchor.error().message));
        QCOMPARE(workspace->currentDocumentId(),
                 QStringLiteral("ca4m4.benton.record.entry.rif-workbook"));
        QCOMPARE(workspace->loadedPageCount(), 8);
        QCOMPARE(workspace->currentPageIndex(), 4);
        workspace->setDocumentSearch(QStringLiteral("continuity score changed"));
        QTRY_VERIFY_WITH_TIMEOUT(workspace->documentSearchResultCount() > 0, 10'000);

        const auto headcount_anchor = workspace->navigateToCitation(QStringLiteral("JA103"));
        QVERIFY2(headcount_anchor.has_value(),
                 headcount_anchor ? "" : qPrintable(headcount_anchor.error().message));
        QCOMPARE(workspace->currentDocumentId(),
                 QStringLiteral("ca4m4.benton.record.entry.headcount-ledger"));
        QCOMPARE(workspace->currentPageIndex(), 2);
        workspace->setDocumentSearch(QStringLiteral("23 authorized positions"));
        QTRY_VERIFY_WITH_TIMEOUT(workspace->documentSearchResultCount() > 0, 10'000);

        const auto benton_disclosure_anchor =
            workspace->navigateToCitation(QStringLiteral("JA114"));
        QVERIFY2(benton_disclosure_anchor.has_value(),
                 benton_disclosure_anchor ? ""
                                          : qPrintable(benton_disclosure_anchor.error().message));
        QCOMPARE(workspace->currentDocumentId(),
                 QStringLiteral("ca4m4.benton.record.entry.benton-deposition"));
        QCOMPARE(workspace->currentPageIndex(), 8);
        workspace->setDocumentSearch(
            QStringLiteral("materially different discoverable information"));
        QTRY_VERIFY_WITH_TIMEOUT(workspace->documentSearchResultCount() > 0, 10'000);
        const auto pike_disclosure_anchor = workspace->navigateToCitation(QStringLiteral("JA121"));
        QVERIFY2(pike_disclosure_anchor.has_value(),
                 pike_disclosure_anchor ? "" : qPrintable(pike_disclosure_anchor.error().message));
        QCOMPARE(workspace->currentDocumentId(),
                 QStringLiteral("ca4m4.benton.record.entry.pike-deposition"));
        QCOMPARE(workspace->currentPageIndex(), 5);
        workspace->setDocumentSearch(QStringLiteral("No additional subject"));
        QTRY_VERIFY_WITH_TIMEOUT(workspace->documentSearchResultCount() > 0, 10'000);

        const auto declaration_anchor = workspace->navigateToCitation(QStringLiteral("JA194"));
        QVERIFY2(declaration_anchor.has_value(),
                 declaration_anchor ? "" : qPrintable(declaration_anchor.error().message));
        QCOMPARE(workspace->currentDocumentId(),
                 QStringLiteral("ca4m4.benton.record.entry.benton-summary-judgment-opposition"));
        QCOMPARE(workspace->loadedPageCount(), 10);
        QCOMPARE(workspace->currentPageIndex(), 8);
        workspace->setDocumentSearch(QStringLiteral("Leora has made this an HR and EEOC problem"));
        QTRY_VERIFY_WITH_TIMEOUT(workspace->documentSearchResultCount() > 0, 10'000);

        const auto exclusion_order_anchor = workspace->navigateToCitation(QStringLiteral("JA235"));
        QVERIFY2(exclusion_order_anchor.has_value(),
                 exclusion_order_anchor ? "" : qPrintable(exclusion_order_anchor.error().message));
        QCOMPARE(workspace->currentDocumentId(),
                 QStringLiteral("ca4m4.benton.record.entry.order-excluding-wynn-declaration"));
        QCOMPARE(workspace->loadedPageCount(), 4);
        QCOMPARE(workspace->currentPageIndex(), 3);
        workspace->setDocumentSearch(
            QStringLiteral("does not hold that the declaration is a sham"));
        QTRY_VERIFY_WITH_TIMEOUT(workspace->documentSearchResultCount() > 0, 10'000);

        const auto opinion_anchor = workspace->navigateToCitation(QStringLiteral("JA247"));
        QVERIFY2(opinion_anchor.has_value(),
                 opinion_anchor ? "" : qPrintable(opinion_anchor.error().message));
        QCOMPARE(workspace->currentDocumentId(),
                 QStringLiteral("ca4m4.benton.record.entry.summary-judgment-opinion"));
        QCOMPARE(workspace->loadedPageCount(), 12);
        QCOMPARE(workspace->currentPageIndex(), 11);
        workspace->setDocumentSearch(QStringLiteral("ultimate causal finding required by Foster"));
        QTRY_VERIFY_WITH_TIMEOUT(workspace->documentSearchResultCount() > 0, 10'000);

        const auto notice_anchor = workspace->navigateToCitation(QStringLiteral("JA250"));
        QVERIFY2(notice_anchor.has_value(),
                 notice_anchor ? "" : qPrintable(notice_anchor.error().message));
        QCOMPARE(workspace->currentDocumentId(),
                 QStringLiteral("ca4m4.benton.record.entry.notice-of-appeal"));
        QCOMPARE(workspace->loadedPageCount(), 3);
        QCOMPARE(workspace->currentPageIndex(), 0);
        workspace->setDocumentSearch(QStringLiteral("Benton is the only appellant"));
        QTRY_VERIFY_WITH_TIMEOUT(workspace->documentSearchResultCount() > 0, 10'000);

        const auto certificate_anchor = workspace->navigateToCitation(QStringLiteral("JA262"));
        QVERIFY2(certificate_anchor.has_value(),
                 certificate_anchor ? "" : qPrintable(certificate_anchor.error().message));
        QCOMPARE(workspace->currentDocumentId(),
                 QStringLiteral("ca4m4.benton.record.entry.certified-docket-record-certificate"));
        QCOMPARE(workspace->loadedPageCount(), 10);
        QCOMPARE(workspace->currentPageIndex(), 9);
        workspace->setDocumentSearch(
            QStringLiteral("thirty-seven documents, 262 substantive pages"));
        QTRY_VERIFY_WITH_TIMEOUT(workspace->documentSearchResultCount() > 0, 10'000);
    }

    {
        ui::MainWindow window({}, catalog_root, nullptr, provider);
        const auto loaded = window.loadSource(root_archive);
        QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error()));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const auto actual_index = configurationIndex(window, "ca4m4.benton.argument.actual-record");
        QVERIFY(actual_index >= 0);
        window.argumentConfigurationSelector()->setCurrentIndex(actual_index);
        const auto actual_reopen = window.openSelectedOralArgument();
        QVERIFY2(actual_reopen.has_value(), actual_reopen ? "" : qPrintable(actual_reopen.error()));
        QTRY_VERIFY(window.oralArgumentWorkspace()->isReady());
        QCOMPARE(*window.oralArgumentWorkspace()->sessionState(), *actual_state);
        QCOMPARE(window.oralArgumentWorkspace()->transcriptView()->toPlainText(),
                 actual_transcript);

        const auto counterfactual_index =
            configurationIndex(window, "ca4m4.benton.argument.no-knowledge-counterfactual");
        QVERIFY(counterfactual_index >= 0);
        window.argumentConfigurationSelector()->setCurrentIndex(counterfactual_index);
        const auto counterfactual_launch = window.openSelectedOralArgument();
        QVERIFY2(counterfactual_launch.has_value(),
                 counterfactual_launch ? "" : qPrintable(counterfactual_launch.error()));
        QTRY_VERIFY(window.oralArgumentWorkspace()->isReady());
        QVERIFY(window.oralArgumentWorkspace()->isolationNoticeLabel()->text().contains(
            QStringLiteral("isolated from the actual-record workflow")));
        QCOMPARE(window.oralArgumentWorkspace()
                     ->canonicalDefinition()
                     ->question_bank.argument_configuration_id,
                 std::string("ca4m4.benton.argument.no-knowledge-counterfactual"));
        QCOMPARE(
            window.oralArgumentWorkspace()->canonicalDefinition()->configuration.legal_state_digest,
            workflow_legal_state_digest);
        submitGroundedAnswer(
            *window.oralArgumentWorkspace(),
            QStringLiteral("The no-knowledge premise remains isolated from the actual record."));
        counterfactual_state = *window.oralArgumentWorkspace()->sessionState();
        counterfactual_transcript = window.oralArgumentWorkspace()->transcriptView()->toPlainText();
        QVERIFY(counterfactual_transcript.contains(QStringLiteral("no-knowledge premise")));
    }

    {
        ui::MainWindow window({}, catalog_root, nullptr, provider);
        const auto loaded = window.loadSource(root_archive);
        QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error()));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const auto counterfactual_index =
            configurationIndex(window, "ca4m4.benton.argument.no-knowledge-counterfactual");
        QVERIFY(counterfactual_index >= 0);
        window.argumentConfigurationSelector()->setCurrentIndex(counterfactual_index);
        QVERIFY(window.openSelectedOralArgument().has_value());
        QTRY_VERIFY(window.oralArgumentWorkspace()->isReady());
        QCOMPARE(*window.oralArgumentWorkspace()->sessionState(), *counterfactual_state);
        QCOMPARE(window.oralArgumentWorkspace()->transcriptView()->toPlainText(),
                 counterfactual_transcript);

        const auto actual_index = configurationIndex(window, "ca4m4.benton.argument.actual-record");
        QVERIFY(actual_index >= 0);
        window.argumentConfigurationSelector()->setCurrentIndex(actual_index);
        QVERIFY(window.openSelectedOralArgument().has_value());
        QTRY_VERIFY(window.oralArgumentWorkspace()->isReady());
        QCOMPARE(*window.oralArgumentWorkspace()->sessionState(), *actual_state);
        QCOMPARE(window.oralArgumentWorkspace()->transcriptView()->toPlainText(),
                 actual_transcript);
    }

    {
        auto store = provider->forkConnection();
        QVERIFY2(store.has_value(), store ? "" : qPrintable(store.error().message));
        const auto actual_snapshot = (*store)->loadSession(QString::fromLatin1(actual_session_id));
        const auto counterfactual_snapshot =
            (*store)->loadSession(QString::fromLatin1(counterfactual_session_id));
        const auto workflow_snapshot_after =
            (*store)->loadSession(QString::fromLatin1(workflow_session_id));
        QVERIFY2(actual_snapshot.has_value(),
                 actual_snapshot ? "" : qPrintable(actual_snapshot.error().message));
        QVERIFY2(counterfactual_snapshot.has_value(),
                 counterfactual_snapshot ? ""
                                         : qPrintable(counterfactual_snapshot.error().message));
        QVERIFY2(workflow_snapshot_after.has_value(),
                 workflow_snapshot_after ? ""
                                         : qPrintable(workflow_snapshot_after.error().message));
        QCOMPARE(actual_snapshot->authority_contract,
                 storage::SessionAuthorityContract::CanonicalV2);
        QCOMPARE(counterfactual_snapshot->authority_contract,
                 storage::SessionAuthorityContract::CanonicalV2);
        QCOMPARE(actual_snapshot->sequence, qint64{2});
        QCOMPARE(counterfactual_snapshot->sequence, qint64{2});
        QCOMPARE(actual_snapshot->commands.size(), std::size_t{2});
        QCOMPARE(counterfactual_snapshot->commands.size(), std::size_t{2});
        QCOMPARE(actual_snapshot->events.size(), std::size_t{2});
        QCOMPARE(counterfactual_snapshot->events.size(), std::size_t{2});
        QCOMPARE(actual_snapshot->pins.size(), std::size_t{4});
        QCOMPARE(counterfactual_snapshot->pins.size(), std::size_t{4});
        const std::array expected_pins{expected_root, expected_federal, expected_ca4,
                                       expected_bench};
        for (const auto* snapshot : {&*actual_snapshot, &*counterfactual_snapshot}) {
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
        QCOMPARE(workflow_snapshot_after->session_id, workflow_snapshot_before.session_id);
        QCOMPARE(workflow_snapshot_after->engine_revision,
                 workflow_snapshot_before.engine_revision);
        QCOMPARE(workflow_snapshot_after->authority_contract,
                 workflow_snapshot_before.authority_contract);
        QCOMPARE(workflow_snapshot_after->sequence, workflow_snapshot_before.sequence);
        QCOMPARE(workflow_snapshot_after->created_at_utc, workflow_snapshot_before.created_at_utc);
        QVERIFY(workflow_snapshot_after->pins == workflow_snapshot_before.pins);
        QVERIFY(workflow_snapshot_after->commands == workflow_snapshot_before.commands);
        QVERIFY(workflow_snapshot_after->events == workflow_snapshot_before.events);
        QVERIFY(workflow_snapshot_after->docket == workflow_snapshot_before.docket);
        QVERIFY(workflow_snapshot_after->asset_references ==
                workflow_snapshot_before.asset_references);
        QCOMPARE(snapshotBytes(*workflow_snapshot_after), snapshotBytes(workflow_snapshot_before));
        const auto workflow_rows_after =
            workflowDatabaseRows(database_path, QString::fromLatin1(workflow_session_id));
        QVERIFY2(workflow_rows_after.has_value(),
                 workflow_rows_after ? "" : qPrintable(workflow_rows_after.error()));
        QCOMPARE(*workflow_rows_after, workflow_rows_before);
    }

    const auto catalog = packs::PackCatalog::open(catalog_root);
    QVERIFY2(catalog.has_value(), catalog ? "" : qPrintable(catalog.error().message));
    const auto resolved = (*catalog)->loadResolved(expected_root);
    QVERIFY2(resolved.has_value(), resolved ? "" : qPrintable(resolved.error().message));
    const auto runtime = packs::loadRuntimePack(*resolved);
    QVERIFY2(runtime.has_value(), runtime ? "" : runtime.error().message.c_str());
    auto workflow_store = provider->forkConnection();
    QVERIFY2(workflow_store.has_value(),
             workflow_store ? "" : qPrintable(workflow_store.error().message));
    const auto workflow_after = app::WorkflowSessionController::reopen(
        runtime->cases.front().definition.id, workflow_initial, storage::AssetStore(asset_root),
        std::move(*workflow_store), QString::fromLatin1(workflow_engine_revision), *resolved);
    QVERIFY2(workflow_after.has_value(),
             workflow_after ? "" : qPrintable(workflow_after.error().message));
    QCOMPARE((*workflow_after)->state(), workflow_state_before);
    QCOMPARE((*workflow_after)->journal(), workflow_journal_before);
    QCOMPARE(provider->createAttempts(), 2);
    QCOMPARE(provider->reopenAttempts(), 3);
}

} // namespace

QTEST_MAIN(M4BentonRetaliationUiE2eTest)
#include "tst_m4_benton_retaliation_ui_e2e.moc"
