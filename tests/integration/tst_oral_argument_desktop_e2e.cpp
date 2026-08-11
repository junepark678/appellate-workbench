#include "appellate/packs/pack_archive.hpp"
#include "appellate/packs/pack_catalog.hpp"
#include "appellate/packs/runtime_pack.hpp"
#include "appellate/storage/asset_store.hpp"
#include "appellate/storage/oral_argument_codec.hpp"
#include "appellate/storage/session_store.hpp"
#include "main_window.hpp"
#include "oral_argument_launch_provider.hpp"
#include "oral_argument_session_controller.hpp"
#include "oral_argument_workspace.hpp"
#include "workflow_session_controller.hpp"

#include <QAction>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
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

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace app = appellate::app;
namespace model = appellate::model;
namespace packs = appellate::packs;
namespace storage = appellate::storage;
namespace ui = appellate::ui;
using namespace std::chrono_literals;

constexpr auto workflow_session_id = "e2e.workflow.canonical";
constexpr auto workflow_engine_revision = "engine.workflow.desktop-e2e.1";
constexpr auto actual_session_id = "e2e.oral.actual";
constexpr auto counterfactual_session_id = "e2e.oral.counterfactual";
constexpr auto oral_engine_revision = "engine.oral.desktop-e2e.2";

[[nodiscard]] QString fixture(const QString& name) {
    return QStringLiteral(APPELLATE_TEST_FIXTURES) + u'/' + name;
}

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
        QStringLiteral("oral-desktop-e2e-rows-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    QByteArray encoded;
    QString failure;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name);
        database.setDatabaseName(database_path);
        if (!database.open()) {
            failure = database.lastError().text();
        } else {
            appendFrame(encoded, QByteArrayView("appellate-workbench-e2e-workflow-rows-v1"));
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

[[nodiscard]] model::LegalTime legalTime(std::chrono::year_month_day day, std::chrono::hours hour) {
    return model::LegalTime{
        std::chrono::sys_seconds{std::chrono::sys_days{day}} + hour,
        model::LegalDate{day},
    };
}

[[nodiscard]] model::WorkflowState initialWorkflowState(const packs::RuntimeCase& runtime_case) {
    model::WorkflowState state;
    state.session_id = workflow_session_id;
    state.workflow_id = runtime_case.workflow.id;
    state.current_stage_id = runtime_case.workflow.initial_stage_id;
    state.next_event_sequence = 1;
    return state;
}

[[nodiscard]] auto createStructuredWorkflow(const packs::RuntimeCase& runtime_case,
                                            const packs::ResolvedPack& resolved_pack,
                                            const QString& database_path, const QString& asset_root)
    -> std::expected<std::unique_ptr<app::WorkflowSessionController>, QString> {
    auto store = storage::SessionStore::open(database_path);
    if (!store) {
        return std::unexpected(store.error().message);
    }
    auto controller = app::WorkflowSessionController::create(
        runtime_case.definition.id, initialWorkflowState(runtime_case),
        storage::AssetStore(asset_root), std::move(*store),
        QString::fromLatin1(workflow_engine_revision), QStringLiteral("2026-01-02T11:59:00Z"),
        resolved_pack);
    if (!controller) {
        return std::unexpected(controller.error().message);
    }

    const QByteArray filing_document("desktop e2e canonical notice");
    const auto filing = model::SubmitWorkflowFiling{
        model::WorkflowCommandHeader{
            workflow_session_id,
            model::WorkflowCommandId{"e2e.command.notice"},
            model::ActorId{"example.actor.appellant"},
            legalTime(std::chrono::year{2026} / std::chrono::January / std::chrono::day{2}, 12h),
        },
        model::WorkflowFilingId{"e2e.filing.notice"},
        model::FilingTypeId{"example.filing.notice"},
        sha256(filing_document).toStdString(),
        {model::WorkflowFieldValue{model::FilingFieldId{"example.field.caption"},
                                   "caption supplied by desktop e2e"}},
        {model::ActorId{"example.actor.appellee"}},
        std::nullopt,
    };
    const auto filed = (*controller)
                           ->submit(model::WorkflowCommand{filing}, QByteArrayView(filing_document),
                                    QStringLiteral("2026-01-02T12:00:00Z"));
    if (!filed) {
        return std::unexpected(filed.error().message);
    }
    if (!runtime_case.definition.authored_disposition_operation_id.has_value() ||
        !runtime_case.definition.authored_disposition_plan_id.has_value()) {
        return std::unexpected(QStringLiteral("Fixture has no authored structured disposition"));
    }

    const QByteArray judgment_document("desktop e2e canonical structured judgment");
    const auto judgment = model::IssueWorkflowJudgment{
        model::WorkflowCommandHeader{
            workflow_session_id,
            model::WorkflowCommandId{"e2e.command.judgment"},
            model::ActorId{"example.actor.court"},
            legalTime(std::chrono::year{2026} / std::chrono::January / std::chrono::day{3}, 14h),
        },
        *runtime_case.definition.authored_disposition_operation_id,
        sha256(judgment_document).toStdString(),
        *runtime_case.definition.authored_disposition_plan_id,
    };
    const auto judged =
        (*controller)
            ->submit(model::WorkflowCommand{judgment}, QByteArrayView(judgment_document),
                     QStringLiteral("2026-01-03T14:00:00Z"));
    if (!judged) {
        return std::unexpected(judged.error().message);
    }
    return std::move(*controller);
}

class PersistedOralArgumentLaunchProvider final : public ui::OralArgumentLaunchProvider {
  public:
    PersistedOralArgumentLaunchProvider(QString database_path, std::string legal_state_digest,
                                        model::PackRevision expected_revision)
        : database_path_(std::move(database_path)),
          legal_state_digest_(std::move(legal_state_digest)),
          expected_revision_(std::move(expected_revision)) {}

    [[nodiscard]] auto open(const packs::ResolvedPack& resolved_pack, const model::CaseId& case_id,
                            const packs::RuntimeArgumentConfigId& argument_configuration_id)
        -> std::expected<std::unique_ptr<app::OralArgumentSessionController>,
                         app::OralArgumentSessionError> override {
        if (resolved_pack.root().revision != expected_revision_) {
            return fail(QStringLiteral("Desktop launch did not retain the exact pack revision"));
        }
        QString session_id;
        if (argument_configuration_id.value == "example.argument.fictional") {
            session_id = QString::fromLatin1(actual_session_id);
        } else if (argument_configuration_id.value == "example.argument.counterfactual") {
            session_id = QString::fromLatin1(counterfactual_session_id);
        } else {
            return fail(QStringLiteral("Unknown exact oral-argument configuration"));
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
            auto reopened = app::OralArgumentSessionController::reopen(
                session_id, case_id, argument_configuration_id, legal_state_digest_,
                std::move(*store), QString::fromLatin1(oral_engine_revision), resolved_pack);
            last_error_ = reopened.has_value()
                              ? std::nullopt
                              : std::optional<app::OralArgumentSessionError>{reopened.error()};
            return reopened;
        }
        ++create_attempts_;
        auto created = app::OralArgumentSessionController::create(
            session_id, case_id, argument_configuration_id, legal_state_digest_, std::move(*store),
            QString::fromLatin1(oral_engine_revision), QStringLiteral("2026-08-11T10:00:00Z"),
            resolved_pack);
        last_error_ = created.has_value()
                          ? std::nullopt
                          : std::optional<app::OralArgumentSessionError>{created.error()};
        return created;
    }

    [[nodiscard]] int createAttempts() const noexcept { return create_attempts_; }
    [[nodiscard]] int reopenAttempts() const noexcept { return reopen_attempts_; }
    [[nodiscard]] const std::optional<app::OralArgumentSessionError>& lastError() const noexcept {
        return last_error_;
    }

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
    std::optional<app::OralArgumentSessionError> last_error_;
};

[[nodiscard]] int configurationIndex(const ui::MainWindow& window, model::OralArgumentMode mode) {
    if (window.currentRuntime() == nullptr || window.currentRuntime()->cases.size() != 1) {
        return -1;
    }
    const auto& configurations = window.currentRuntime()->cases.front().argument_configurations;
    for (std::size_t index = 0; index < configurations.size(); ++index) {
        const auto& bank = configurations.at(index).grounded_question_bank;
        if (bank.has_value() && bank->mode == mode) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

void checkOpeningWorkspace(ui::OralArgumentWorkspace& workspace, model::OralArgumentMode mode) {
    QVERIFY(workspace.isReady());
    QVERIFY(workspace.canonicalDefinition() != nullptr);
    QVERIFY(workspace.sessionState() != nullptr);
    QVERIFY(workspace.sessionState()->canonical_contract.has_value());
    QCOMPARE(workspace.canonicalDefinition()->question_bank.mode, mode);
    QCOMPARE(workspace.sessionState()->canonical_contract->mode, mode);
    QCOMPARE(workspace.groundingTable()->rowCount(), 3);

    const auto actual = mode == model::OralArgumentMode::ActualRecord;
    const auto expected_question =
        actual ? QStringLiteral("example.question.preservation")
               : QStringLiteral("example.question.counterfactual-preservation");
    const auto expected_prompt =
        actual ? QStringLiteral("Where did the appellant preserve the claimed error?")
               : QStringLiteral(
                     "Assume counsel made the same objection one hearing earlier; what would "
                     "change?");
    const auto expected_prefix = actual ? QStringLiteral("example.grounding.preservation-")
                                        : QStringLiteral("example.grounding.counterfactual-");
    QCOMPARE(workspace.canonicalDefinition()->question_bank.argument_configuration_id,
             actual ? std::string("example.argument.fictional")
                    : std::string("example.argument.counterfactual"));
    QVERIFY(workspace.questionLabel()->text().contains(expected_question));
    QVERIFY(workspace.questionLabel()->text().contains(expected_prompt));
    QVERIFY(workspace.issueLabel()->text().contains(QStringLiteral("example.issue.preservation")));
    QVERIFY(workspace.topicLabel()->text().contains(QStringLiteral("Preservation")));
    QVERIFY(
        workspace.topicLabel()->text().contains(QStringLiteral("workbench.topic.preservation")));
    QCOMPARE(workspace.groundingTable()->item(0, 1)->text(), QStringLiteral("Authority"));
    QCOMPARE(workspace.groundingTable()->item(1, 1)->text(), QStringLiteral("Brief page"));
    QCOMPARE(workspace.groundingTable()->item(2, 1)->text(), QStringLiteral("Record page"));
    QCOMPARE(workspace.groundingTable()->item(0, 2)->text(),
             expected_prefix + QStringLiteral("authority"));
    QCOMPARE(workspace.groundingTable()->item(1, 2)->text(),
             expected_prefix + QStringLiteral("brief"));
    QCOMPARE(workspace.groundingTable()->item(2, 2)->text(),
             expected_prefix + QStringLiteral("record"));
    QVERIFY(workspace.groundingTable()->item(0, 3)->text().contains(
        QStringLiteral("example.authority.rule-one")));
    QVERIFY(workspace.groundingTable()->item(1, 3)->text().contains(
        QStringLiteral("example.record.brief-opening")));
    QVERIFY(workspace.groundingTable()->item(2, 3)->text().contains(
        QStringLiteral("example.record.anchor.ja2")));
}

void submitThroughShortcut(ui::OralArgumentWorkspace& workspace, const QString& answer) {
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
    QTRY_VERIFY(workspace.statusLabel()->text().contains(QStringLiteral("event 2")));
    QCOMPARE(workspace.sessionState()->journal.size(), std::size_t{2});
}

[[nodiscard]] auto
substituteStoredOpeningGrounding(const QString& database_path, const QString& target_session_id,
                                 const model::CanonicalOralArgumentDefinition& actual_definition)
    -> std::expected<QByteArray, QString> {
    const model::AuthoredArgumentQuestion* source_question = nullptr;
    for (const auto& question : actual_definition.question_bank.questions) {
        if (question.id == "example.question.record-support") {
            source_question = &question;
            break;
        }
    }
    if (source_question == nullptr) {
        return std::unexpected(
            QStringLiteral("The exact actual bank has no alternate authored question"));
    }
    const auto source_event = model::OralArgumentEvent{
        2,
        model::CounselAnswer{model::CounselActKind::Answer,
                             "Codec-valid source answer",
                             source_question->issue_id,
                             {},
                             1.0,
                             1s},
        model::BenchAct{
            model::BenchActKind::Question,
            actual_definition.bench.presiding_seat_id,
            model::GroundedQuestion{source_question->issue_id,
                                    model::AuthoredQuestionSelection{
                                        source_question->id, source_question->topic,
                                        actual_definition.question_bank.mode,
                                        source_question->prompt, source_question->grounding},
                                    1, false},
            "Codec-valid alternate actual-bank question",
        },
    };
    const auto encoded_source = storage::encodeCanonicalOralArgumentEvent(source_event);
    if (!encoded_source) {
        return std::unexpected(encoded_source.error().message);
    }
    const auto source_question_json = QJsonDocument::fromJson(*encoded_source)
                                          .object()
                                          .value(QStringLiteral("payload"))
                                          .toObject()
                                          .value(QStringLiteral("bench"))
                                          .toObject()
                                          .value(QStringLiteral("question"))
                                          .toObject();
    QJsonObject replacement;
    for (const auto& grounding :
         source_question_json.value(QStringLiteral("grounding")).toArray()) {
        if (grounding.toObject().value(QStringLiteral("kind")).toString() ==
            QStringLiteral("record_page")) {
            replacement = grounding.toObject();
            break;
        }
    }
    if (replacement.isEmpty()) {
        return std::unexpected(
            QStringLiteral("Alternate actual-bank question has no record snapshot"));
    }

    const auto connection_name = QStringLiteral("oral-desktop-e2e-tamper-%1")
                                     .arg(QUuid::createUuid().toString(QUuid::Id128));
    QByteArray mutated;
    QString failure;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name);
        database.setDatabaseName(database_path);
        if (!database.open()) {
            failure = database.lastError().text();
        } else {
            QSqlQuery read(database);
            read.prepare(QStringLiteral("SELECT payload_json FROM event_log WHERE session_id=? "
                                        "AND sequence=1"));
            read.addBindValue(target_session_id);
            if (!read.exec() || !read.next()) {
                failure = read.lastError().text();
            } else {
                auto root = QJsonDocument::fromJson(read.value(0).toByteArray()).object();
                auto payload = root.value(QStringLiteral("payload")).toObject();
                auto bench = payload.value(QStringLiteral("bench")).toObject();
                auto question = bench.value(QStringLiteral("question")).toObject();
                auto grounding = question.value(QStringLiteral("grounding")).toArray();
                bool replacement_is_unique = !replacement.isEmpty();
                qsizetype target_index = -1;
                for (qsizetype index = 0; index < grounding.size(); ++index) {
                    const auto target = grounding.at(index).toObject();
                    if (target.value(QStringLiteral("kind")).toString() ==
                        replacement.value(QStringLiteral("kind")).toString()) {
                        target_index = index;
                        continue;
                    }
                    replacement_is_unique =
                        replacement_is_unique &&
                        target.value(QStringLiteral("grounding_id")).toString() !=
                            replacement.value(QStringLiteral("grounding_id")).toString();
                }
                if (target_index >= 0 && replacement_is_unique &&
                    question.value(QStringLiteral("question_id")).toString() !=
                        source_question_json.value(QStringLiteral("question_id")).toString()) {
                    grounding.replace(target_index, replacement);
                    question.insert(QStringLiteral("grounding"), grounding);
                    bench.insert(QStringLiteral("question"), question);
                    payload.insert(QStringLiteral("bench"), bench);
                    root.insert(QStringLiteral("payload"), payload);
                    mutated = QJsonDocument(root).toJson(QJsonDocument::Compact);
                    read.finish();
                    QSqlQuery write(database);
                    write.prepare(QStringLiteral("UPDATE event_log SET payload_json=? WHERE "
                                                 "session_id=? AND sequence=1"));
                    write.addBindValue(mutated);
                    write.addBindValue(target_session_id);
                    if (!write.exec() || write.numRowsAffected() != 1) {
                        failure = write.lastError().text();
                        mutated.clear();
                    }
                } else {
                    failure = QStringLiteral(
                        "Could not form a unique cross-question actual-bank substitution");
                }
            }
        }
        database.close();
        database = QSqlDatabase{};
    }
    QSqlDatabase::removeDatabase(connection_name);
    if (!failure.isEmpty()) {
        return std::unexpected(failure);
    }
    return mutated;
}

class OralArgumentDesktopE2eTest final : public QObject {
    Q_OBJECT

  private slots:
    void actualAndCounterfactualPersistAcrossDesktopRestartsAndRejectTamper();
};

void OralArgumentDesktopE2eTest::
    actualAndCounterfactualPersistAcrossDesktopRestartsAndRejectTamper() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto archive_path = temporary.filePath(QStringLiteral("full-resource-pack-v2.awpack"));
    const auto catalog_root = temporary.filePath(QStringLiteral("catalog"));
    const auto database_path = temporary.filePath(QStringLiteral("sessions.sqlite"));
    const auto asset_root = temporary.filePath(QStringLiteral("session-assets"));

    const auto exported = packs::PackArchive::exportDirectory(
        fixture(QStringLiteral("full-resource-pack-v2")), archive_path);
    QVERIFY2(exported.has_value(),
             qPrintable(exported.has_value() ? QString{} : exported.error().message));
    auto catalog = packs::PackCatalog::open(catalog_root);
    QVERIFY2(catalog.has_value(),
             qPrintable(catalog.has_value() ? QString{} : catalog.error().message));
    const auto installed =
        (*catalog)->installArchive(archive_path, QStringLiteral("2026-08-11T09:00:00Z"));
    QVERIFY2(installed.has_value(),
             qPrintable(installed.has_value() ? QString{} : installed.error().message));
    QCOMPARE(installed->revision, *exported);
    auto resolved = (*catalog)->loadResolved(installed->revision);
    QVERIFY2(resolved.has_value(),
             qPrintable(resolved.has_value() ? QString{} : resolved.error().message));
    QCOMPARE(resolved->root().revision, *exported);
    const auto runtime = packs::loadRuntimePack(*resolved);
    QVERIFY2(runtime.has_value(), runtime.has_value() ? "" : runtime.error().message.c_str());
    QCOMPARE(runtime->cases.size(), std::size_t{1});
    const auto& runtime_case = runtime->cases.front();

    auto workflow = createStructuredWorkflow(runtime_case, *resolved, database_path, asset_root);
    QVERIFY2(workflow.has_value(), qPrintable(workflow.has_value() ? QString{} : workflow.error()));
    QVERIFY((*workflow)->state().judgment_disposition.has_value());
    const auto* structured_disposition =
        std::get_if<model::DispositionPlan>(&*(*workflow)->state().judgment_disposition);
    QVERIFY(structured_disposition != nullptr);
    QCOMPARE(structured_disposition->id, model::DispositionPlanId{"example.disposition.fictional"});
    QCOMPARE((*workflow)->snapshot().authority_contract,
             storage::SessionAuthorityContract::CanonicalV2);
    QCOMPARE((*workflow)->snapshot().commands.size(), std::size_t{2});
    QCOMPARE((*workflow)->snapshot().events.size(), std::size_t{3});

    const auto workflow_initial = (*workflow)->initialState();
    const auto workflow_state_before = (*workflow)->state();
    const auto workflow_journal_before = (*workflow)->journal();
    (*workflow).reset();
    const auto workflow_snapshot =
        persistedSnapshot(database_path, QString::fromLatin1(workflow_session_id));
    QVERIFY2(workflow_snapshot.has_value(),
             qPrintable(workflow_snapshot.has_value() ? QString{} : workflow_snapshot.error()));
    const auto workflow_snapshot_bytes_before = snapshotBytes(*workflow_snapshot);
    const auto legal_state_digest = sha256(workflow_snapshot_bytes_before).toStdString();
    QCOMPARE(legal_state_digest.size(), std::size_t{64});
    const auto workflow_rows_before =
        workflowDatabaseRows(database_path, QString::fromLatin1(workflow_session_id));
    QVERIFY2(
        workflow_rows_before.has_value(),
        qPrintable(workflow_rows_before.has_value() ? QString{} : workflow_rows_before.error()));

    (*catalog).reset();
    auto provider = std::make_shared<PersistedOralArgumentLaunchProvider>(
        database_path, legal_state_digest, *exported);

    std::optional<model::CanonicalOralArgumentDefinition> actual_definition;
    std::optional<model::OralArgumentState> actual_state;
    QString actual_transcript;
    {
        ui::MainWindow window(archive_path, catalog_root, nullptr, provider);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.currentRuntime() != nullptr);
        QCOMPARE(window.currentRuntime()->revision, *exported);
        const auto actual_index = configurationIndex(window, model::OralArgumentMode::ActualRecord);
        QVERIFY(actual_index >= 0);
        window.argumentConfigurationSelector()->setCurrentIndex(actual_index);
        QVERIFY(window.openOralArgumentAction()->isEnabled());
        window.openOralArgumentAction()->trigger();
        QTRY_VERIFY(window.oralArgumentWorkspace()->isReady());
        checkOpeningWorkspace(*window.oralArgumentWorkspace(),
                              model::OralArgumentMode::ActualRecord);
        QCOMPARE(
            window.oralArgumentWorkspace()->canonicalDefinition()->configuration.legal_state_digest,
            legal_state_digest);
        QCOMPARE(window.oralArgumentWorkspace()
                     ->canonicalDefinition()
                     ->configuration.authored_disposition_id,
                 std::string("example.disposition.fictional"));
        actual_definition = *window.oralArgumentWorkspace()->canonicalDefinition();
        submitThroughShortcut(
            *window.oralArgumentWorkspace(),
            QStringLiteral("The exact authority, opening brief, and joint appendix preserve it."));
        actual_state = *window.oralArgumentWorkspace()->sessionState();
        actual_transcript = window.oralArgumentWorkspace()->transcriptView()->toPlainText();
        QVERIFY(actual_transcript.contains(QStringLiteral("joint appendix preserve it")));
    }

    std::optional<model::OralArgumentState> counterfactual_state;
    QString counterfactual_transcript;
    {
        ui::MainWindow window(archive_path, catalog_root, nullptr, provider);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        const auto actual_index = configurationIndex(window, model::OralArgumentMode::ActualRecord);
        QVERIFY(actual_index >= 0);
        window.argumentConfigurationSelector()->setCurrentIndex(actual_index);
        window.openOralArgumentAction()->trigger();
        QTRY_VERIFY(window.oralArgumentWorkspace()->isReady());
        QCOMPARE(*window.oralArgumentWorkspace()->sessionState(), *actual_state);
        QCOMPARE(window.oralArgumentWorkspace()->transcriptView()->toPlainText(),
                 actual_transcript);

        const auto workflow_rows_immediately_before_counterfactual =
            workflowDatabaseRows(database_path, QString::fromLatin1(workflow_session_id));
        QVERIFY2(workflow_rows_immediately_before_counterfactual.has_value(),
                 qPrintable(workflow_rows_immediately_before_counterfactual.has_value()
                                ? QString{}
                                : workflow_rows_immediately_before_counterfactual.error()));
        auto workflow_store_before_counterfactual = storage::SessionStore::open(database_path);
        QVERIFY2(workflow_store_before_counterfactual.has_value(),
                 qPrintable(workflow_store_before_counterfactual.has_value()
                                ? QString{}
                                : workflow_store_before_counterfactual.error().message));
        auto workflow_before_counterfactual = app::WorkflowSessionController::reopen(
            runtime_case.definition.id, workflow_initial, storage::AssetStore(asset_root),
            std::move(*workflow_store_before_counterfactual),
            QString::fromLatin1(workflow_engine_revision), *resolved);
        QVERIFY2(workflow_before_counterfactual.has_value(),
                 qPrintable(workflow_before_counterfactual.has_value()
                                ? QString{}
                                : workflow_before_counterfactual.error().message));
        const auto workflow_state_immediately_before_counterfactual =
            (*workflow_before_counterfactual)->state();
        const auto workflow_journal_immediately_before_counterfactual =
            (*workflow_before_counterfactual)->journal();
        const auto workflow_snapshot_immediately_before_counterfactual =
            snapshotBytes((*workflow_before_counterfactual)->snapshot());
        QVERIFY(workflow_state_immediately_before_counterfactual.judgment_disposition.has_value());
        (*workflow_before_counterfactual).reset();

        const auto counterfactual_index =
            configurationIndex(window, model::OralArgumentMode::CounterfactualTraining);
        QVERIFY(counterfactual_index >= 0);
        window.argumentConfigurationSelector()->setCurrentIndex(counterfactual_index);
        window.openOralArgumentAction()->trigger();
        QTRY_VERIFY(window.oralArgumentWorkspace()->isReady());
        checkOpeningWorkspace(*window.oralArgumentWorkspace(),
                              model::OralArgumentMode::CounterfactualTraining);
        QVERIFY(window.oralArgumentWorkspace()->isolationNoticeLabel()->text().contains(
            QStringLiteral("isolated from the actual-record workflow")));
        QCOMPARE(
            window.oralArgumentWorkspace()->canonicalDefinition()->configuration.legal_state_digest,
            legal_state_digest);
        QCOMPARE(window.oralArgumentWorkspace()
                     ->canonicalDefinition()
                     ->configuration.authored_disposition_id,
                 std::string("example.disposition.fictional"));
        submitThroughShortcut(
            *window.oralArgumentWorkspace(),
            QStringLiteral("On that counterfactual, timing changes but the legal pin does not."));
        counterfactual_state = *window.oralArgumentWorkspace()->sessionState();
        counterfactual_transcript = window.oralArgumentWorkspace()->transcriptView()->toPlainText();

        const auto workflow_rows_immediately_after_counterfactual =
            workflowDatabaseRows(database_path, QString::fromLatin1(workflow_session_id));
        QVERIFY2(workflow_rows_immediately_after_counterfactual.has_value(),
                 qPrintable(workflow_rows_immediately_after_counterfactual.has_value()
                                ? QString{}
                                : workflow_rows_immediately_after_counterfactual.error()));
        QCOMPARE(*workflow_rows_immediately_after_counterfactual,
                 *workflow_rows_immediately_before_counterfactual);
        auto workflow_store_after_counterfactual = storage::SessionStore::open(database_path);
        QVERIFY2(workflow_store_after_counterfactual.has_value(),
                 qPrintable(workflow_store_after_counterfactual.has_value()
                                ? QString{}
                                : workflow_store_after_counterfactual.error().message));
        auto workflow_after_counterfactual = app::WorkflowSessionController::reopen(
            runtime_case.definition.id, workflow_initial, storage::AssetStore(asset_root),
            std::move(*workflow_store_after_counterfactual),
            QString::fromLatin1(workflow_engine_revision), *resolved);
        QVERIFY2(workflow_after_counterfactual.has_value(),
                 qPrintable(workflow_after_counterfactual.has_value()
                                ? QString{}
                                : workflow_after_counterfactual.error().message));
        QCOMPARE((*workflow_after_counterfactual)->state(),
                 workflow_state_immediately_before_counterfactual);
        QCOMPARE((*workflow_after_counterfactual)->journal(),
                 workflow_journal_immediately_before_counterfactual);
        QCOMPARE(snapshotBytes((*workflow_after_counterfactual)->snapshot()),
                 workflow_snapshot_immediately_before_counterfactual);
        QCOMPARE((*workflow_after_counterfactual)->state().judgment_disposition,
                 workflow_state_immediately_before_counterfactual.judgment_disposition);
    }

    const auto workflow_rows_after_counterfactual =
        workflowDatabaseRows(database_path, QString::fromLatin1(workflow_session_id));
    QVERIFY2(workflow_rows_after_counterfactual.has_value(),
             qPrintable(workflow_rows_after_counterfactual.has_value()
                            ? QString{}
                            : workflow_rows_after_counterfactual.error()));
    QCOMPARE(*workflow_rows_after_counterfactual, *workflow_rows_before);
    auto workflow_store = storage::SessionStore::open(database_path);
    QVERIFY2(workflow_store.has_value(),
             qPrintable(workflow_store.has_value() ? QString{} : workflow_store.error().message));
    auto reopened_workflow = app::WorkflowSessionController::reopen(
        runtime_case.definition.id, workflow_initial, storage::AssetStore(asset_root),
        std::move(*workflow_store), QString::fromLatin1(workflow_engine_revision), *resolved);
    QVERIFY2(
        reopened_workflow.has_value(),
        qPrintable(reopened_workflow.has_value() ? QString{} : reopened_workflow.error().message));
    QCOMPARE((*reopened_workflow)->state(), workflow_state_before);
    QCOMPARE((*reopened_workflow)->journal(), workflow_journal_before);
    QVERIFY((*reopened_workflow)->state().judgment_disposition.has_value());
    QCOMPARE((*reopened_workflow)->state().judgment_disposition,
             workflow_state_before.judgment_disposition);
    QCOMPARE(snapshotBytes((*reopened_workflow)->snapshot()), workflow_snapshot_bytes_before);
    (*reopened_workflow).reset();

    {
        ui::MainWindow window(archive_path, catalog_root, nullptr, provider);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const auto counterfactual_index =
            configurationIndex(window, model::OralArgumentMode::CounterfactualTraining);
        QVERIFY(counterfactual_index >= 0);
        window.argumentConfigurationSelector()->setCurrentIndex(counterfactual_index);
        window.openOralArgumentAction()->trigger();
        QTRY_VERIFY(window.oralArgumentWorkspace()->isReady());
        QCOMPARE(*window.oralArgumentWorkspace()->sessionState(), *counterfactual_state);
        QCOMPARE(window.oralArgumentWorkspace()->transcriptView()->toPlainText(),
                 counterfactual_transcript);

        auto* const last_good_workspace = window.oralArgumentWorkspace();
        const auto last_good_state = *last_good_workspace->sessionState();
        const auto last_good_transcript = last_good_workspace->transcriptView()->toPlainText();
        QVERIFY(actual_definition.has_value());
        const auto mutated = substituteStoredOpeningGrounding(
            database_path, QString::fromLatin1(actual_session_id), *actual_definition);
        QVERIFY2(mutated.has_value(),
                 qPrintable(mutated.has_value() ? QString{} : mutated.error()));
        const auto codec_valid_mutation = storage::decodeCanonicalOralArgumentEvent(*mutated);
        QVERIFY2(codec_valid_mutation.has_value(),
                 qPrintable(codec_valid_mutation.has_value()
                                ? QString{}
                                : codec_valid_mutation.error().message));
        const auto actual_index = configurationIndex(window, model::OralArgumentMode::ActualRecord);
        QVERIFY(actual_index >= 0);
        window.argumentConfigurationSelector()->setCurrentIndex(actual_index);
        window.openOralArgumentAction()->trigger();
        QTRY_VERIFY(!window.errorLabel()->isHidden());
        QVERIFY(window.errorLabel()->text().contains(QStringLiteral("could not be opened")));
        QVERIFY(provider->lastError().has_value());
        QCOMPARE(provider->lastError()->code, app::OralArgumentSessionErrorCode::CorruptSession);
        QCOMPARE(provider->lastError()->message,
                 QStringLiteral("Canonical event differs from exact re-decision"));
        QCOMPARE(window.oralArgumentWorkspace(), last_good_workspace);
        QCOMPARE(*window.oralArgumentWorkspace()->sessionState(), last_good_state);
        QCOMPARE(window.oralArgumentWorkspace()->transcriptView()->toPlainText(),
                 last_good_transcript);
    }

    QCOMPARE(provider->createAttempts(), 2);
    QCOMPARE(provider->reopenAttempts(), 3);
    const auto workflow_rows_after_tamper =
        workflowDatabaseRows(database_path, QString::fromLatin1(workflow_session_id));
    QVERIFY(workflow_rows_after_tamper.has_value());
    QCOMPARE(*workflow_rows_after_tamper, *workflow_rows_before);
}

} // namespace

QTEST_MAIN(OralArgumentDesktopE2eTest)

#include "tst_oral_argument_desktop_e2e.moc"
