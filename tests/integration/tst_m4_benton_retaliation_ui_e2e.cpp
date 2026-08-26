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

#include <QAction>
#include <QByteArrayView>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
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

constexpr auto root_digest = "59467350af5f381ef429ecf210d38de5503d40fb2e9baf02f56b2ef5023ced28";
constexpr auto federal_digest = "866c90996c15e2076b9508a297ffce1a4e766b1432a9e11d08e8138c57e363c9";
constexpr auto ca4_digest = "449d75c77e5c47883f750377450f2d1ec1fc0e42e20b1f247446b208661d3262";
constexpr auto bench_digest = "cee0bf93309cc9ad800f215a47d734b20a9fdf5dc889f2f440e4382b942d332d";
constexpr auto manifest_digest = "2705a970c34c83317a940e00dc11b51ac8ca205424f3a9a30df405bbcb27717a";
constexpr auto archive_digest = "9515bdde1e3405e6e82488abd73314a31c33a2062f9e34b4cecdaaff8b634a05";
constexpr auto archive_byte_size = qint64{3'408'701};
constexpr auto case_digest = "21b068c597c15399aaada912e1e653ca1f53f7325561955976e21fa02576ef8b";
constexpr auto record_digest = "191aac8abba8fb7817cf15c3019429e50dc70b0afa66dfd5e2f2df392a94e875";
constexpr auto workflow_digest = "b4f73bcb6b4451a06a1d1fb6b23fd94549d5968de7bb0f9f0a19bbba7a624e36";
constexpr auto realism_review_digest =
    "fd42415873af8558112a21312eaad87c0020da256f17178fa58554009328dff0";
constexpr auto evidence_closure_digest =
    "cf3538ecc449cc3e8a0a05220a1b8a741c636a17c05fec571c1851ea320aea43";
constexpr auto authored_disposition_digest =
    "40fd60e4fe24ddcbecfd61d72a39db361c2aebc82601521082d2e2e0e472b51e";
constexpr auto adverse_disposition_digest =
    "5a9f06407f6e82dc194f9ba74335573c600cb44a2b9dd5215dc84f90379b819b";
constexpr auto retaliation_issue = "ca4m4.benton.issue.retaliation-summary-judgment";
constexpr auto exclusion_issue = "ca4m4.benton.issue.late-comparator-declaration-exclusion";
constexpr auto actual_session_id = "ca4m4.benton.session.oral.actual";
constexpr auto counterfactual_session_id = "ca4m4.benton.session.oral.counterfactual";
constexpr auto oral_engine_revision = "engine.oral.benton-e2e.1";
constexpr auto workflow_session_id = "ca4m4.benton.session.actual-argued-no-petition-mandate";
constexpr auto workflow_engine_revision = "appellate.realism-evidence.codec-replay-multi.v1";

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

struct FrozenWorkflowStep final {
    model::WorkflowCommand command;
    std::optional<QByteArray> document_bytes;
};

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

[[nodiscard]] auto loadFrozenWorkflowTrace(const QString& trace_path,
                                           std::string_view expected_session_id,
                                           const packs::RuntimeRecord& record)
    -> std::expected<std::vector<FrozenWorkflowStep>, QString> {
    QFile trace_file(trace_path);
    if (!trace_file.open(QIODevice::ReadOnly)) {
        return std::unexpected(trace_file.errorString());
    }
    QJsonParseError parse_error;
    const auto trace_document = QJsonDocument::fromJson(trace_file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !trace_document.isObject()) {
        return std::unexpected(QStringLiteral("Cannot parse frozen workflow trace: %1")
                                   .arg(parse_error.errorString()));
    }
    const auto trace = trace_document.object();
    if (trace.value(QStringLiteral("trace_id")).toString() !=
            QStringLiteral("ca4m4.benton.trace.actual-argued-no-petition-mandate") ||
        trace.value(QStringLiteral("engine_revision")).toString() !=
            QString::fromLatin1(workflow_engine_revision) ||
        trace.value(QStringLiteral("command_count")).toInt() != 26 ||
        trace.value(QStringLiteral("event_count")).toInt() != 27 ||
        trace.value(QStringLiteral("terminal_stage_id")).toString() !=
            QStringLiteral("ca4m4.benton.stage.terminated")) {
        return std::unexpected(QStringLiteral("Frozen workflow trace metadata is stale"));
    }
    const auto journal_value = trace.value(QStringLiteral("journal"));
    if (!journal_value.isArray()) {
        return std::unexpected(QStringLiteral("Frozen workflow trace has no journal"));
    }

    std::vector<FrozenWorkflowStep> steps;
    const auto journal = journal_value.toArray();
    steps.reserve(static_cast<std::size_t>(journal.size()));
    for (const auto& entry_value : journal) {
        if (!entry_value.isObject()) {
            return std::unexpected(
                QStringLiteral("Frozen workflow journal entry is not an object"));
        }
        const auto command_base64 =
            entry_value.toObject().value(QStringLiteral("command_base64")).toString().toLatin1();
        const auto command_bytes = QByteArray::fromBase64(command_base64);
        const auto decoded = storage::decodeWorkflowCommand(QByteArrayView(command_bytes));
        if (!decoded) {
            return std::unexpected(decoded.error().message);
        }
        const auto session_matches = std::visit(
            [expected_session_id](const auto& concrete) {
                return concrete.header.session_id == expected_session_id;
            },
            *decoded);
        if (!session_matches) {
            return std::unexpected(
                QStringLiteral("Frozen workflow command has an unexpected session identity"));
        }

        std::optional<QByteArray> document_bytes;
        if (const auto digest = commandDocumentDigest(*decoded); digest.has_value()) {
            const auto entry = std::ranges::find(record.docket_entries, *digest,
                                                 &packs::RuntimeDocketEntry::asset_sha256);
            if (entry == record.docket_entries.end()) {
                return std::unexpected(
                    QStringLiteral("Frozen workflow document digest is absent from the record"));
            }
            const auto asset_path = QDir(QStringLiteral(APPELLATE_M4_BENTON_ROOT))
                                        .filePath(QStringLiteral("pack/%1").arg(
                                            QString::fromStdString(entry->asset_path)));
            QFile asset(asset_path);
            if (!asset.open(QIODevice::ReadOnly)) {
                return std::unexpected(asset.errorString());
            }
            document_bytes = asset.readAll();
            if (sha256(*document_bytes).toStdString() != *digest) {
                return std::unexpected(
                    QStringLiteral("Frozen workflow document bytes have the wrong digest"));
            }
        }
        steps.push_back(FrozenWorkflowStep{std::move(*decoded), std::move(document_bytes)});
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
    const model::PackRevision expected_root{model::PackId{"us.ca4.m4.benton-retaliation"}, "1.2.0",
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
    QFile manifest(QDir(QStringLiteral(APPELLATE_M4_BENTON_ROOT))
                       .filePath(QStringLiteral("pack/manifest.json")));
    QVERIFY2(manifest.open(QIODevice::ReadOnly), qPrintable(manifest.errorString()));
    QCOMPARE(sha256(manifest.readAll()), QByteArray(manifest_digest));
    const auto exported = packs::PackArchive::exportDirectory(
        QDir(QStringLiteral(APPELLATE_M4_BENTON_ROOT)).filePath(QStringLiteral("pack")),
        root_archive, {}, packs::PackValidationScope::ResolvedClosure);
    QVERIFY2(exported.has_value(), exported ? "" : qPrintable(exported.error().message));
    QCOMPARE(*exported, expected_root);
    QFile archive(root_archive);
    QVERIFY2(archive.open(QIODevice::ReadOnly), qPrintable(archive.errorString()));
    QCOMPARE(archive.size(), archive_byte_size);
    QCOMPARE(sha256(archive.readAll()), QByteArray(archive_digest));

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
        QCOMPARE(root->archive_sha256, QString::fromLatin1(archive_digest));

        const auto resolved = (*catalog)->loadResolved(expected_root);
        QVERIFY2(resolved.has_value(), resolved ? "" : qPrintable(resolved.error().message));
        const auto verify_exact_resource = [&](std::string_view id, std::string_view digest) {
            const auto found =
                std::ranges::find(resolved->root().resources, id, [](const auto& resource) {
                    return std::string_view(resource.descriptor.id);
                });
            QVERIFY2(found != resolved->root().resources.end(), id.data());
            QCOMPARE(found->descriptor.sha256, std::string(digest));
        };
        verify_exact_resource("ca4m4.case.benton-retaliation", case_digest);
        verify_exact_resource("ca4m4.benton.record", record_digest);
        verify_exact_resource("ca4m4.benton.workflow.civil-appeal", workflow_digest);
        verify_exact_resource("ca4m4.benton.review.authoring-2026-08-12", realism_review_digest);
        const auto review = std::ranges::find(
            resolved->root().resources,
            std::string_view{"ca4m4.benton.review.authoring-2026-08-12"},
            [](const auto& resource) { return std::string_view(resource.descriptor.id); });
        QVERIFY(review != resolved->root().resources.end());
        QCOMPARE(review->document.value(QStringLiteral("review_state")).toString(),
                 QStringLiteral("independent_review_pending"));
        const auto dimensions = review->document.value(QStringLiteral("dimensions")).toObject();
        QCOMPARE(dimensions.size(), 7);
        for (auto dimension = dimensions.constBegin(); dimension != dimensions.constEnd();
             ++dimension) {
            QCOMPARE(dimension.value().toInt(), 2);
        }
        const auto evidence = review->document.value(QStringLiteral("evidence")).toObject();
        QCOMPARE(evidence.value(QStringLiteral("closure_digest")).toString(),
                 QString::fromLatin1(evidence_closure_digest));
        const auto traces = evidence.value(QStringLiteral("traces")).toArray();
        QCOMPARE(traces.size(), 7);
        QVERIFY(std::ranges::any_of(traces, [](const auto& value) {
            const auto trace = value.toObject();
            return trace.value(QStringLiteral("trace_id")).toString() ==
                       QStringLiteral("ca4m4.benton.trace.actual-argued-no-petition-mandate") &&
                   trace.value(QStringLiteral("command_count")).toInt() == 26 &&
                   trace.value(QStringLiteral("event_count")).toInt() == 27 &&
                   trace.value(QStringLiteral("terminal_stage_id")).toString() ==
                       QStringLiteral("ca4m4.benton.stage.terminated");
        }));
        const auto runtime = packs::loadRuntimePack(*resolved);
        QVERIFY2(runtime.has_value(), runtime ? "" : runtime.error().message.c_str());
        QCOMPARE(runtime->cases.size(), std::size_t{1});
        const auto& runtime_case = runtime->cases.front();
        QCOMPARE(runtime_case.record.dockets.size(), std::size_t{3});
        QCOMPARE(runtime_case.record.docket_entries.size(), std::size_t{67});
        QCOMPARE(runtime_case.record.page_anchors.size(), std::size_t{389});
        const auto has_tag = [](const packs::RuntimeDocketEntry& entry, std::string_view tag) {
            return std::ranges::find(entry.tags, tag) != entry.tags.end();
        };
        std::size_t district_document_count{};
        std::size_t actual_document_count{};
        std::size_t branch_document_count{};
        std::uint32_t district_page_count{};
        std::uint32_t actual_page_count{};
        std::uint32_t branch_page_count{};
        for (const auto& entry : runtime_case.record.docket_entries) {
            QVERIFY(entry.docket_id.has_value());
            if (entry.docket_id->value == "ca4m4.benton.docket.district") {
                ++district_document_count;
                district_page_count += entry.page_count;
                QVERIFY(!has_tag(entry, "actual_appellate_docket"));
                QVERIFY(!has_tag(entry, "counterfactual_appellate_branch"));
            } else if (entry.docket_id->value == "ca4m4.benton.docket.appellate") {
                ++actual_document_count;
                actual_page_count += entry.page_count;
                QVERIFY(has_tag(entry, "actual_appellate_docket"));
                QVERIFY(!has_tag(entry, "counterfactual_appellate_branch"));
            } else if (entry.docket_id->value == "ca4m4.benton.docket.counterfactual-branches") {
                ++branch_document_count;
                branch_page_count += entry.page_count;
                QVERIFY(has_tag(entry, "counterfactual_appellate_branch"));
                QVERIFY(has_tag(entry, "never_occurred_on_actual_docket"));
                QVERIFY(!has_tag(entry, "actual_appellate_docket"));
            } else {
                QFAIL("Unclassified Benton record document");
            }
        }
        QCOMPARE(district_document_count, std::size_t{37});
        QCOMPARE(district_page_count, std::uint32_t{262});
        QCOMPARE(actual_document_count, std::size_t{13});
        QCOMPARE(actual_page_count, std::uint32_t{70});
        QCOMPARE(branch_document_count, std::size_t{17});
        QCOMPARE(branch_page_count, std::uint32_t{57});

        std::vector<unsigned> ja_labels;
        std::vector<unsigned> pa_labels;
        for (const auto& anchor : runtime_case.record.page_anchors) {
            QVERIFY(anchor.citation_label.has_value());
            const std::string_view label(*anchor.citation_label);
            const auto entry = std::ranges::find(runtime_case.record.docket_entries,
                                                 anchor.entry_id, &packs::RuntimeDocketEntry::id);
            QVERIFY(entry != runtime_case.record.docket_entries.end());
            if (label.starts_with("JA")) {
                ja_labels.push_back(
                    static_cast<unsigned>(std::stoul(std::string(label.substr(2)))));
                QCOMPARE(entry->docket_id->value, std::string("ca4m4.benton.docket.district"));
            } else if (label.starts_with("PA")) {
                const auto page = static_cast<unsigned>(std::stoul(std::string(label.substr(2))));
                pa_labels.push_back(page);
                if (page <= 70U) {
                    QCOMPARE(entry->docket_id->value, std::string("ca4m4.benton.docket.appellate"));
                    QVERIFY(has_tag(*entry, "actual_appellate_docket"));
                } else {
                    QCOMPARE(entry->docket_id->value,
                             std::string("ca4m4.benton.docket.counterfactual-branches"));
                    QVERIFY(has_tag(*entry, "counterfactual_appellate_branch"));
                    QVERIFY(has_tag(*entry, "never_occurred_on_actual_docket"));
                }
            } else {
                QFAIL("Unexpected Benton page-anchor citation series");
            }
        }
        QCOMPARE(ja_labels.size(), std::size_t{262});
        QCOMPARE(pa_labels.size(), std::size_t{127});
        for (std::size_t index = 0; index < ja_labels.size(); ++index) {
            QCOMPARE(ja_labels.at(index), static_cast<unsigned>(index + 1U));
        }
        for (std::size_t index = 0; index < pa_labels.size(); ++index) {
            QCOMPARE(pa_labels.at(index), static_cast<unsigned>(index + 1U));
        }

        QCOMPARE(runtime_case.definition.disposition_plans.size(), std::size_t{2});
        QCOMPARE(runtime_case.definition.authored_disposition_plan_id,
                 std::optional<model::DispositionPlanId>{
                     model::DispositionPlanId{"ca4m4.benton.disposition.authored"}});
        QCOMPARE(runtime_case.definition.authored_disposition_operation_id,
                 std::optional<model::WorkflowOperationId>{
                     model::WorkflowOperationId{"ca4m4.benton.operation.issue-judgment"}});
        const auto authored =
            std::ranges::find(runtime_case.definition.disposition_plans,
                              model::DispositionPlanId{"ca4m4.benton.disposition.authored"},
                              &model::DispositionPlan::id);
        const auto adverse = std::ranges::find(
            runtime_case.definition.disposition_plans,
            model::DispositionPlanId{"ca4m4.benton.disposition.counterfactual-adverse"},
            &model::DispositionPlan::id);
        QVERIFY(authored != runtime_case.definition.disposition_plans.end());
        QVERIFY(adverse != runtime_case.definition.disposition_plans.end());
        QCOMPARE(authored->canonical_sha256, std::string(authored_disposition_digest));
        QCOMPARE(adverse->canonical_sha256, std::string(adverse_disposition_digest));
        QCOMPARE(authored->components.size(), std::size_t{2});
        QCOMPARE(adverse->components.size(), std::size_t{2});
        const auto authored_exclusion =
            std::ranges::find(authored->components, model::CaseIssueId{exclusion_issue},
                              &model::DispositionComponent::issue_id);
        const auto authored_retaliation =
            std::ranges::find(authored->components, model::CaseIssueId{retaliation_issue},
                              &model::DispositionComponent::issue_id);
        const auto adverse_exclusion =
            std::ranges::find(adverse->components, model::CaseIssueId{exclusion_issue},
                              &model::DispositionComponent::issue_id);
        const auto adverse_retaliation =
            std::ranges::find(adverse->components, model::CaseIssueId{retaliation_issue},
                              &model::DispositionComponent::issue_id);
        QVERIFY(authored_exclusion != authored->components.end());
        QVERIFY(authored_retaliation != authored->components.end());
        QVERIFY(adverse_exclusion != adverse->components.end());
        QVERIFY(adverse_retaliation != adverse->components.end());
        QCOMPARE(authored_exclusion->action, model::DispositionAction::Affirm);
        QVERIFY(!authored_exclusion->remand);
        QCOMPARE(authored_retaliation->action, model::DispositionAction::Vacate);
        QVERIFY(authored_retaliation->remand);
        QCOMPARE(adverse_exclusion->action, model::DispositionAction::Affirm);
        QVERIFY(!adverse_exclusion->remand);
        QCOMPARE(adverse_retaliation->action, model::DispositionAction::Affirm);
        QVERIFY(!adverse_retaliation->remand);
        workflow_initial = initialWorkflowState(runtime_case);
        auto workflow_store = storage::SessionStore::open(database_path);
        QVERIFY2(workflow_store.has_value(),
                 workflow_store ? "" : qPrintable(workflow_store.error().message));
        auto workflow = app::WorkflowSessionController::create(
            runtime_case.definition.id, workflow_initial, storage::AssetStore(asset_root),
            std::move(*workflow_store), QString::fromLatin1(workflow_engine_revision),
            QStringLiteral("2026-08-11T09:00:00Z"), *resolved);
        QVERIFY2(workflow.has_value(), workflow ? "" : qPrintable(workflow.error().message));
        const auto actual_trace = loadFrozenWorkflowTrace(
            QDir(QStringLiteral(APPELLATE_M4_BENTON_ROOT))
                .filePath(QStringLiteral("traces/actual-argued-no-petition-mandate.json")),
            workflow_session_id, runtime_case.record);
        QVERIFY2(actual_trace.has_value(), actual_trace ? "" : qPrintable(actual_trace.error()));
        QCOMPARE(actual_trace->size(), std::size_t{26});
        QVERIFY(actual_trace->front().document_bytes.has_value());
        auto wrong_final_order = *actual_trace->front().document_bytes;
        wrong_final_order[0] = static_cast<char>(wrong_final_order.at(0) ^ 0x01);
        const auto wrong_identity =
            (*workflow)->submit(actual_trace->front().command, QByteArrayView(wrong_final_order),
                                QStringLiteral("2026-08-11T09:00:30Z"));
        QVERIFY(!wrong_identity.has_value());
        QVERIFY(wrong_identity.error().code ==
                app::WorkflowSessionErrorCode::DocumentDigestMismatch);
        QCOMPARE((*workflow)->state(), workflow_initial);
        QVERIFY((*workflow)->journal().empty());
        QCOMPARE((*workflow)->snapshot().sequence, qint64{0});

        for (std::size_t index = 0; index < actual_trace->size(); ++index) {
            const auto& step = actual_trace->at(index);
            std::optional<QByteArrayView> document_view;
            if (step.document_bytes.has_value()) {
                document_view = QByteArrayView(*step.document_bytes);
            }
            const auto submitted = (*workflow)->submit(
                step.command, document_view,
                QStringLiteral("2026-08-11T09:%1:00Z")
                    .arg(static_cast<int>(index + 1U), 2, 10, QLatin1Char('0')));
            QVERIFY2(submitted.has_value(), submitted ? "" : qPrintable(submitted.error().message));
            QCOMPARE(submitted->asset.has_value(), step.document_bytes.has_value());
            if (step.document_bytes.has_value()) {
                const auto expected_digest = commandDocumentDigest(step.command);
                QVERIFY(expected_digest.has_value());
                QCOMPARE(submitted->asset->sha256, QString::fromStdString(*expected_digest));
                QCOMPARE(submitted->asset->size, static_cast<qint64>(step.document_bytes->size()));
            }
        }
        QCOMPARE((*workflow)->state().current_stage_id,
                 model::WorkflowStageId{"ca4m4.benton.stage.terminated"});
        QVERIFY((*workflow)->state().judgment_disposition.has_value());
        const auto* structured_disposition =
            std::get_if<model::DispositionPlan>(&*(*workflow)->state().judgment_disposition);
        QVERIFY(structured_disposition != nullptr);
        QCOMPARE(structured_disposition->id,
                 model::DispositionPlanId{"ca4m4.benton.disposition.authored"});
        QCOMPARE(structured_disposition->canonical_sha256,
                 std::string(authored_disposition_digest));
        QCOMPARE((*workflow)->journal().size(), std::size_t{26});
        workflow_state_before = (*workflow)->state();
        workflow_journal_before = (*workflow)->journal();
        workflow_snapshot_before = (*workflow)->snapshot();
        QCOMPARE(workflow_snapshot_before.authority_contract,
                 storage::SessionAuthorityContract::CanonicalV2);
        QCOMPARE(workflow_snapshot_before.sequence, qint64{27});
        QCOMPARE(workflow_snapshot_before.commands.size(), std::size_t{26});
        QCOMPARE(workflow_snapshot_before.events.size(), std::size_t{27});
        QCOMPARE(workflow_snapshot_before.docket.size(), std::size_t{27});
        QCOMPARE(workflow_snapshot_before.asset_references.size(), std::size_t{11});
        constexpr auto mandate_digest =
            "7e2f032d51d648a8cf8e592bf53ff6c5019d9cfebdc95dc74595f2fa8ce75b66";
        const auto mandate_step =
            std::ranges::find_if(*actual_trace, [mandate_digest](const auto& step) {
                return commandDocumentDigest(step.command) == mandate_digest;
            });
        QVERIFY(mandate_step != actual_trace->end());
        QVERIFY(mandate_step->document_bytes.has_value());
        const auto persisted_mandate =
            storage::AssetStore(asset_root).read(QString::fromLatin1(mandate_digest));
        QVERIFY2(persisted_mandate.has_value(),
                 persisted_mandate ? "" : qPrintable(persisted_mandate.error().message));
        QCOMPARE(*persisted_mandate, *mandate_step->document_bytes);
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
                 "ca4m4.benton.operation.calculate-ordinary-mandate",
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
            window.oralArgumentWorkspace()->canonicalDefinition()->question_bank.questions.size(),
            std::size_t{12});
        QCOMPARE(
            window.oralArgumentWorkspace()->canonicalDefinition()->configuration.legal_state_digest,
            workflow_legal_state_digest);
        QCOMPARE(window.oralArgumentWorkspace()
                     ->canonicalDefinition()
                     ->configuration.authored_disposition_id,
                 std::string("ca4m4.benton.disposition.authored"));
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
        auto* pdf_search = workspace->findChild<QPdfSearchModel*>();
        QVERIFY(pdf_search != nullptr);
        QCOMPARE(workspace->visibleDocketCount(), qsizetype{67});
        workspace->setDocketFilter(QStringLiteral("ca4m4.benton.docket.district"));
        QCOMPARE(workspace->visibleDocketCount(), qsizetype{37});
        workspace->setDocketFilter(QStringLiteral("ca4m4.benton.docket.appellate"));
        QCOMPARE(workspace->visibleDocketCount(), qsizetype{13});
        workspace->setDocketFilter(QStringLiteral("ca4m4.benton.docket.counterfactual-branches"));
        QCOMPARE(workspace->visibleDocketCount(), qsizetype{17});
        workspace->setDocketFilter(QStringLiteral("actual_appellate_docket"));
        QCOMPARE(workspace->visibleDocketCount(), qsizetype{13});
        workspace->setDocketFilter(QStringLiteral("counterfactual_appellate_branch"));
        QCOMPARE(workspace->visibleDocketCount(), qsizetype{17});
        workspace->setDocketFilter(QStringLiteral("never_occurred_on_actual_docket"));
        QCOMPARE(workspace->visibleDocketCount(), qsizetype{17});
        workspace->setDocketFilter({});
        QCOMPARE(workspace->visibleDocketCount(), qsizetype{67});

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

        const auto actual_opinion_anchor = workspace->navigateToCitation(QStringLiteral("PA52"));
        QVERIFY2(actual_opinion_anchor.has_value(),
                 actual_opinion_anchor ? "" : qPrintable(actual_opinion_anchor.error().message));
        QCOMPARE(workspace->currentDocumentId(), QStringLiteral("ca4m4.benton.record.entry.a10"));
        QCOMPARE(workspace->loadedPageCount(), 14);
        QCOMPARE(workspace->currentPageIndex(), 0);
        workspace->setDocumentSearch(QStringLiteral("affirm the exclusion"));
        QTRY_VERIFY_WITH_TIMEOUT(workspace->documentSearchResultCount() > 0, 10'000);
        QTRY_VERIFY_WITH_TIMEOUT(
            !pdf_search->resultsOnPage(workspace->currentPageIndex()).isEmpty(), 10'000);

        const auto actual_last_anchor = workspace->navigateToCitation(QStringLiteral("PA70"));
        QVERIFY2(actual_last_anchor.has_value(),
                 actual_last_anchor ? "" : qPrintable(actual_last_anchor.error().message));
        QCOMPARE(workspace->currentDocumentId(), QStringLiteral("ca4m4.benton.record.entry.a13"));
        QCOMPARE(workspace->loadedPageCount(), 2);
        QCOMPARE(workspace->currentPageIndex(), 1);
        workspace->setDocumentSearch(QStringLiteral("district record remains identified"));
        QTRY_VERIFY_WITH_TIMEOUT(workspace->documentSearchResultCount() > 0, 10'000);
        QTRY_VERIFY_WITH_TIMEOUT(
            !pdf_search->resultsOnPage(workspace->currentPageIndex()).isEmpty(), 10'000);

        const auto branch_first_anchor = workspace->navigateToCitation(QStringLiteral("PA71"));
        QVERIFY2(branch_first_anchor.has_value(),
                 branch_first_anchor ? "" : qPrintable(branch_first_anchor.error().message));
        QCOMPARE(workspace->currentDocumentId(), QStringLiteral("ca4m4.benton.record.entry.b01"));
        QCOMPARE(workspace->loadedPageCount(), 2);
        QCOMPARE(workspace->currentPageIndex(), 0);
        workspace->setDocumentSearch(QStringLiteral("oral argument is unnecessary"));
        QTRY_VERIFY_WITH_TIMEOUT(workspace->documentSearchResultCount() > 0, 10'000);
        QTRY_VERIFY_WITH_TIMEOUT(
            !pdf_search->resultsOnPage(workspace->currentPageIndex()).isEmpty(), 10'000);

        const auto branch_last_anchor = workspace->navigateToCitation(QStringLiteral("PA127"));
        QVERIFY2(branch_last_anchor.has_value(),
                 branch_last_anchor ? "" : qPrintable(branch_last_anchor.error().message));
        QCOMPARE(workspace->currentDocumentId(), QStringLiteral("ca4m4.benton.record.entry.b17"));
        QCOMPARE(workspace->loadedPageCount(), 2);
        QCOMPARE(workspace->currentPageIndex(), 1);
        workspace->setDocumentSearch(QStringLiteral("district court may manage"));
        QTRY_VERIFY_WITH_TIMEOUT(workspace->documentSearchResultCount() > 0, 10'000);
        QTRY_VERIFY_WITH_TIMEOUT(
            !pdf_search->resultsOnPage(workspace->currentPageIndex()).isEmpty(), 10'000);
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
            window.oralArgumentWorkspace()->canonicalDefinition()->question_bank.questions.size(),
            std::size_t{12});
        QCOMPARE(
            window.oralArgumentWorkspace()->canonicalDefinition()->configuration.legal_state_digest,
            workflow_legal_state_digest);
        QCOMPARE(window.oralArgumentWorkspace()
                     ->canonicalDefinition()
                     ->configuration.authored_disposition_id,
                 std::string("ca4m4.benton.disposition.authored"));
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
