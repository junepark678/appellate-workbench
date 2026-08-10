#include "workflow_session_controller.hpp"

#include "appellate/storage/asset_store.hpp"
#include "appellate/storage/session_store.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <QVariant>

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

class WorkflowSessionResumeTest final : public QObject {
    Q_OBJECT

  private slots:
    void deficiencyCureJudgmentAndMandateSurviveReopen();
    void documentContractFailsBeforeMutation();
    void reopenRejectsJournalTampering_data();
    void reopenRejectsJournalTampering();
    void reopenRejectsMissingOrCorruptAsset_data();
    void reopenRejectsMissingOrCorruptAsset();
    void reopenRejectsEnginePinAndSessionMismatch();
    void staleAppendLeavesControllerUnchanged();
};

namespace app = appellate::app;
namespace model = appellate::model;
namespace storage = appellate::storage;

constexpr auto session_id = "test.session.workflow";
constexpr auto filing_stage = "test.stage.filing";
constexpr auto submitted_stage = "test.stage.submitted";
constexpr auto argument_stage = "test.stage.argument";
constexpr auto judgment_stage = "test.stage.judgment";
constexpr auto closed_stage = "test.stage.closed";
constexpr auto appellant_role = "test.role.appellant";
constexpr auto appellee_role = "test.role.appellee";
constexpr auto court_role = "test.role.court";
constexpr auto engine_revision = "engine.workflow.test.1";

[[nodiscard]] model::LegalDate date(int year, unsigned month, unsigned day) {
    return model::LegalDate{std::chrono::year{year} / std::chrono::month{month} /
                            std::chrono::day{day}};
}

[[nodiscard]] model::LegalTime at(int day, int hour = 12) {
    const auto court_date = date(2026, 8, static_cast<unsigned>(day));
    return model::LegalTime{std::chrono::sys_seconds{std::chrono::sys_days{court_date.value}} +
                                std::chrono::hours{hour},
                            court_date};
}

[[nodiscard]] model::AuthorityBasis authority(const std::string& operation_id) {
    return model::AuthorityBasis{
        model::AuthorityRef{model::AuthorityId{operation_id + ".authority"},
                            "Synthetic workflow rule for " + operation_id, "2026-08-11",
                            "Source-grounded proposition for " + operation_id},
        {}};
}

[[nodiscard]] model::WorkflowOperation
operation(std::string id, std::string stage, model::WorkflowOpcode opcode,
          std::optional<std::string> next_stage = std::nullopt,
          std::optional<std::uint32_t> deadline_days = std::nullopt,
          std::optional<model::DeadlineCounting> counting = std::nullopt,
          std::vector<std::string> roles = {}) {
    std::vector<model::ActorRoleId> authorized_roles;
    for (auto& role : roles) {
        authorized_roles.push_back(model::ActorRoleId{std::move(role)});
    }
    const auto operation_id = id;
    return model::WorkflowOperation{
        model::WorkflowOperationId{std::move(id)},
        model::WorkflowStageId{std::move(stage)},
        opcode,
        authority(operation_id),
        next_stage ? std::optional{model::WorkflowStageId{std::move(*next_stage)}} : std::nullopt,
        deadline_days,
        counting,
        std::move(authorized_roles),
    };
}

[[nodiscard]] model::WorkflowDefinition workflow() {
    std::vector<model::WorkflowStageId> stages;
    for (const auto* stage :
         {filing_stage, submitted_stage, argument_stage, judgment_stage, closed_stage}) {
        stages.push_back(model::WorkflowStageId{stage});
    }

    std::vector<model::WorkflowOperation> operations;
    for (const auto* stage :
         {filing_stage, submitted_stage, argument_stage, judgment_stage, closed_stage}) {
        operations.push_back(
            operation(std::string(stage) + ".reject", stage, model::WorkflowOpcode::RejectFiling));
    }
    operations.push_back(
        operation("test.op.filing.accept", filing_stage, model::WorkflowOpcode::AcceptFiling));
    operations.push_back(operation("test.op.filing.deficiency", filing_stage,
                                   model::WorkflowOpcode::IssueDeficiency));
    operations.push_back(operation("test.op.filing.cure-deadline", filing_stage,
                                   model::WorkflowOpcode::CalculateDeadline, std::nullopt, 3,
                                   model::DeadlineCounting::CalendarDays));
    operations.push_back(operation("test.op.filing.advance", filing_stage,
                                   model::WorkflowOpcode::AdvanceStage, submitted_stage));
    operations.push_back(operation("test.op.seal", submitted_stage,
                                   model::WorkflowOpcode::SetSealed, std::nullopt, std::nullopt,
                                   std::nullopt, {court_role}));
    operations.push_back(operation("test.op.order", submitted_stage,
                                   model::WorkflowOpcode::EnterOrder, std::nullopt, std::nullopt,
                                   std::nullopt, {court_role}));
    operations.push_back(operation("test.op.argument", submitted_stage,
                                   model::WorkflowOpcode::ScheduleArgument, argument_stage,
                                   std::nullopt, std::nullopt, {court_role}));
    operations.push_back(operation("test.op.judgment", argument_stage,
                                   model::WorkflowOpcode::IssueJudgment, judgment_stage,
                                   std::nullopt, std::nullopt, {court_role}));
    operations.push_back(operation("test.op.mandate", judgment_stage,
                                   model::WorkflowOpcode::IssueMandate, closed_stage, std::nullopt,
                                   std::nullopt, {court_role}));

    return model::WorkflowDefinition{
        model::WorkflowId{"test.workflow.appeal"},
        model::WorkflowStageId{filing_stage},
        std::move(stages),
        std::move(operations),
        {model::WorkflowFilingRoute{
            model::FilingTypeId{"test.filing.brief"},
            model::WorkflowStageId{filing_stage},
            {model::ActorRoleId{appellant_role}},
            {model::FilingFieldId{"test.field.certificate"}},
            {model::ActorRoleId{appellee_role}},
            model::WorkflowOperationId{"test.op.filing.accept"},
            model::WorkflowOperationId{"test.op.filing.deficiency"},
            model::WorkflowDeadlinePlan{model::WorkflowDeadlineId{"test.deadline.cure"},
                                        model::WorkflowOperationId{"test.op.filing.cure-deadline"}},
            std::nullopt,
            model::WorkflowOperationId{"test.op.filing.advance"},
            std::nullopt,
            true,
        }},
        model::CourtCalendar{},
    };
}

[[nodiscard]] model::CaseDefinition caseDefinition() {
    return model::CaseDefinition{
        model::CaseId{"test.case.workflow"},
        model::ProcedureId{"test.procedure.appeal"},
        {
            {model::ActorId{"test.actor.appellant"}, model::ActorRoleId{appellant_role}},
            {model::ActorId{"test.actor.appellee"}, model::ActorRoleId{appellee_role}},
            {model::ActorId{"test.actor.court"}, model::ActorRoleId{court_role}},
        },
    };
}

[[nodiscard]] model::WorkflowState initialState(std::string id = session_id) {
    return model::WorkflowState{
        std::move(id),
        model::WorkflowId{"test.workflow.appeal"},
        model::WorkflowStageId{filing_stage},
        1,
        std::nullopt,
        {},
        {},
        {},
        {},
        {},
        false,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
    };
}

[[nodiscard]] std::vector<storage::RevisionPin> pins() {
    return {storage::RevisionPin{QStringLiteral("test.pack.workflow"), QStringLiteral("1.0.0"),
                                 QString(64, u'a')}};
}

[[nodiscard]] std::string digest(const QByteArray& bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString();
}

[[nodiscard]] model::WorkflowCommandHeader header(std::string command_id, int day,
                                                  std::string actor = "test.actor.appellant") {
    return model::WorkflowCommandHeader{session_id, model::WorkflowCommandId{std::move(command_id)},
                                        model::ActorId{std::move(actor)}, at(day)};
}

[[nodiscard]] model::SubmitWorkflowFiling deficientFiling(const QByteArray& document) {
    return model::SubmitWorkflowFiling{
        header("test.command.deficient", 11),
        model::WorkflowFilingId{"test.filing.deficient"},
        model::FilingTypeId{"test.filing.brief"},
        digest(document),
        {},
        {model::ActorId{"test.actor.appellee"}},
        std::nullopt,
    };
}

[[nodiscard]] model::SubmitWorkflowFiling cureFiling(const QByteArray& document) {
    return model::SubmitWorkflowFiling{
        header("test.command.cure", 12),
        model::WorkflowFilingId{"test.filing.cure"},
        model::FilingTypeId{"test.filing.brief"},
        digest(document),
        {model::WorkflowFieldValue{model::FilingFieldId{"test.field.certificate"},
                                   "certificate supplied"}},
        {model::ActorId{"test.actor.appellee"}},
        model::WorkflowDeficiencyId{"test.command.deficient.deficiency"},
    };
}

[[nodiscard]] model::ScheduleWorkflowArgument scheduleArgument() {
    return model::ScheduleWorkflowArgument{
        header("test.command.argument", 13, "test.actor.court"),
        model::WorkflowOperationId{"test.op.argument"},
        date(2026, 8, 20),
    };
}

[[nodiscard]] model::IssueWorkflowJudgment judgment(const QByteArray& document) {
    return model::IssueWorkflowJudgment{
        header("test.command.judgment", 20, "test.actor.court"),
        model::WorkflowOperationId{"test.op.judgment"},
        digest(document),
        "Affirmed",
    };
}

[[nodiscard]] model::IssueWorkflowMandate mandate(const QByteArray& document) {
    return model::IssueWorkflowMandate{
        header("test.command.mandate", 21, "test.actor.court"),
        model::WorkflowOperationId{"test.op.mandate"},
        digest(document),
    };
}

[[nodiscard]] auto createController(const QString& database_path, const QString& asset_root)
    -> std::expected<std::unique_ptr<app::WorkflowSessionController>, app::WorkflowSessionError> {
    auto opened = storage::SessionStore::open(database_path);
    if (!opened) {
        return std::unexpected(app::WorkflowSessionError{
            app::WorkflowSessionErrorCode::SessionStoreFailure, opened.error().message});
    }
    return app::WorkflowSessionController::create(
        workflow(), caseDefinition(), initialState(), storage::AssetStore(asset_root, 1024 * 1024),
        std::move(*opened), QString::fromLatin1(engine_revision),
        QStringLiteral("2026-08-11T09:00:00Z"), pins());
}

[[nodiscard]] auto reopenController(const QString& database_path, const QString& asset_root)
    -> std::expected<std::unique_ptr<app::WorkflowSessionController>, app::WorkflowSessionError> {
    auto opened = storage::SessionStore::open(database_path);
    if (!opened) {
        return std::unexpected(app::WorkflowSessionError{
            app::WorkflowSessionErrorCode::SessionStoreFailure, opened.error().message});
    }
    return app::WorkflowSessionController::reopen(
        workflow(), caseDefinition(), initialState(), storage::AssetStore(asset_root, 1024 * 1024),
        std::move(*opened), QString::fromLatin1(engine_revision), pins());
}

[[nodiscard]] bool submitHistory(app::WorkflowSessionController& controller,
                                 bool include_judgment = true) {
    const QByteArray deficient_document("brief missing certificate");
    const QByteArray cure_document("corrected brief with certificate");
    const auto deficient =
        controller.submit(deficientFiling(deficient_document), QByteArrayView(deficient_document),
                          QStringLiteral("2026-08-11T12:01:00Z"));
    if (!deficient) {
        return false;
    }
    const auto cure = controller.submit(cureFiling(cure_document), QByteArrayView(cure_document),
                                        QStringLiteral("2026-08-12T12:01:00Z"));
    if (!cure) {
        return false;
    }
    if (!include_judgment) {
        return true;
    }
    const auto scheduled =
        controller.submit(scheduleArgument(), std::nullopt, QStringLiteral("2026-08-13T12:01:00Z"));
    if (!scheduled) {
        return false;
    }
    const QByteArray judgment_document("synthetic judgment");
    const auto judged =
        controller.submit(judgment(judgment_document), QByteArrayView(judgment_document),
                          QStringLiteral("2026-08-20T12:01:00Z"));
    if (!judged) {
        return false;
    }
    const QByteArray mandate_document("synthetic mandate");
    return controller
        .submit(mandate(mandate_document), QByteArrayView(mandate_document),
                QStringLiteral("2026-08-21T12:01:00Z"))
        .has_value();
}

[[nodiscard]] bool mutateBlob(QSqlDatabase& database, const QString& table,
                              const QString& key_column, const QVariant& key,
                              const QByteArray& before, const QByteArray& after) {
    QSqlQuery select(database);
    select.prepare(
        QStringLiteral("SELECT payload_json FROM %1 WHERE %2 = ?").arg(table, key_column));
    select.addBindValue(key);
    if (!select.exec() || !select.next()) {
        return false;
    }
    auto payload = select.value(0).toByteArray();
    if (!payload.contains(before)) {
        return false;
    }
    payload.replace(before, after);
    select.finish();
    QSqlQuery update(database);
    update.prepare(
        QStringLiteral("UPDATE %1 SET payload_json = ? WHERE %2 = ?").arg(table, key_column));
    update.addBindValue(payload);
    update.addBindValue(key);
    return update.exec() && update.numRowsAffected() == 1;
}

[[nodiscard]] bool tamperDatabase(const QString& database_path, const QString& kind) {
    const auto connection_name = QStringLiteral("workflow-tamper-%1")
                                     .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    bool succeeded = false;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name);
        database.setDatabaseName(database_path);
        if (!database.open()) {
            return false;
        }
        QSqlQuery query(database);
        if (kind == QStringLiteral("command-id")) {
            succeeded = query.exec(
                QStringLiteral("UPDATE command_log SET command_id='test.command.tampered' "
                               "WHERE command_id='test.command.deficient'"));
        } else if (kind == QStringLiteral("command-payload")) {
            succeeded =
                mutateBlob(database, QStringLiteral("command_log"), QStringLiteral("command_id"),
                           QStringLiteral("test.command.deficient"),
                           QByteArray("test.filing.deficient"), QByteArray("test.filing.tampered"));
        } else if (kind == QStringLiteral("event-type")) {
            succeeded = query.exec(QStringLiteral(
                "UPDATE event_log SET event_type='filing.accepted' WHERE sequence=1"));
        } else if (kind == QStringLiteral("event-authority")) {
            succeeded = query.exec(QStringLiteral(
                "UPDATE event_log SET authority_id='test.authority.tampered' WHERE sequence=1"));
        } else if (kind == QStringLiteral("event-payload")) {
            succeeded = mutateBlob(
                database, QStringLiteral("event_log"), QStringLiteral("sequence"), qint64{1},
                QByteArray("test.filing.deficient"), QByteArray("test.filing.tampered"));
        } else if (kind == QStringLiteral("event-group")) {
            succeeded =
                mutateBlob(database, QStringLiteral("event_log"), QStringLiteral("sequence"),
                           qint64{1}, QByteArray("\"command_event_count\":\"2\""),
                           QByteArray("\"command_event_count\":\"1\""));
        } else if (kind == QStringLiteral("event-sequence")) {
            succeeded =
                query.exec(QStringLiteral("UPDATE event_log SET sequence=8 WHERE sequence=7"));
        } else if (kind == QStringLiteral("overlapping-command")) {
            succeeded = query.exec(QStringLiteral("UPDATE command_log SET expected_sequence=0 "
                                                  "WHERE command_id='test.command.cure'"));
        } else if (kind == QStringLiteral("missing-command")) {
            succeeded = query.exec(
                QStringLiteral("DELETE FROM command_log WHERE command_id='test.command.cure'"));
        } else if (kind == QStringLiteral("extra-command")) {
            succeeded = query.exec(QStringLiteral(
                "INSERT INTO command_log(session_id, command_id, expected_sequence, payload_json, "
                "recorded_at_utc) SELECT session_id, 'test.command.extra', 7, payload_json, "
                "recorded_at_utc FROM command_log WHERE command_id='test.command.judgment'"));
        } else if (kind == QStringLiteral("missing-event")) {
            succeeded = query.exec(QStringLiteral("DELETE FROM event_log WHERE sequence=7"));
        } else if (kind == QStringLiteral("docket")) {
            succeeded = query.exec(QStringLiteral(
                "UPDATE docket_projection SET status='tampered' WHERE event_sequence=1"));
        } else if (kind == QStringLiteral("recorded-time")) {
            succeeded = query.exec(
                QStringLiteral("UPDATE command_log SET recorded_at_utc='2026-08-11 12:01:00' "
                               "WHERE command_id='test.command.deficient'"));
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection_name);
    return succeeded;
}

void WorkflowSessionResumeTest::deficiencyCureJudgmentAndMandateSurviveReopen() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto database_path = QDir(directory.path()).filePath(QStringLiteral("sessions.sqlite"));
    const auto asset_root = QDir(directory.path()).filePath(QStringLiteral("assets"));

    auto created = createController(database_path, asset_root);
    if (!created) {
        QFAIL(qPrintable(created.error().message));
    }
    auto controller = std::move(*created);
    QVERIFY(submitHistory(*controller));
    QCOMPARE(controller->journal().size(), std::size_t{5});
    QCOMPARE(controller->snapshot().commands.size(), std::size_t{5});
    QCOMPARE(controller->snapshot().events.size(), std::size_t{7});
    QCOMPARE(controller->snapshot().docket.size(), std::size_t{7});
    QCOMPARE(controller->snapshot().asset_references.size(), std::size_t{4});
    QCOMPARE(controller->state().current_stage_id, model::WorkflowStageId{closed_stage});
    QVERIFY(controller->state().deficiencies.front().cured);
    QVERIFY(controller->state().judgment_sha256.has_value());
    QVERIFY(controller->state().mandate_sha256.has_value());

    const auto state_before_close = controller->state();
    const auto journal_before_close = controller->journal();
    const auto snapshot_before_close = controller->snapshot();
    controller.reset();

    auto reopened = reopenController(database_path, asset_root);
    if (!reopened) {
        QFAIL(qPrintable(reopened.error().message));
    }
    QVERIFY((*reopened)->state() == state_before_close);
    QVERIFY((*reopened)->journal() == journal_before_close);
    QVERIFY((*reopened)->snapshot().pins == snapshot_before_close.pins);
    QVERIFY((*reopened)->snapshot().commands == snapshot_before_close.commands);
    QVERIFY((*reopened)->snapshot().events == snapshot_before_close.events);
    QVERIFY((*reopened)->snapshot().docket == snapshot_before_close.docket);
    QVERIFY((*reopened)->snapshot().asset_references == snapshot_before_close.asset_references);
    for (const auto& reference : (*reopened)->snapshot().asset_references) {
        const auto restored = storage::AssetStore(asset_root, 1024 * 1024).read(reference.digest);
        QVERIFY(restored.has_value());
        QVERIFY(!restored->isEmpty());
    }
}

void WorkflowSessionResumeTest::documentContractFailsBeforeMutation() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto database_path = QDir(directory.path()).filePath(QStringLiteral("sessions.sqlite"));
    const auto asset_root = QDir(directory.path()).filePath(QStringLiteral("assets"));
    auto created = createController(database_path, asset_root);
    if (!created) {
        QFAIL(qPrintable(created.error().message));
    }
    auto controller = std::move(*created);
    const auto pristine_state = controller->state();
    const auto pristine_snapshot = controller->snapshot();

    const QByteArray bytes("document");
    auto filing_command = deficientFiling(bytes);
    auto missing =
        controller->submit(filing_command, std::nullopt, QStringLiteral("2026-08-11T12:01:00Z"));
    QVERIFY(!missing.has_value());
    QCOMPARE(missing.error().code, app::WorkflowSessionErrorCode::UnexpectedDocument);

    filing_command.document_sha256 = std::string(64, 'b');
    auto mismatch = controller->submit(filing_command, QByteArrayView(bytes),
                                       QStringLiteral("2026-08-11T12:01:00Z"));
    QVERIFY(!mismatch.has_value());
    QCOMPARE(mismatch.error().code, app::WorkflowSessionErrorCode::DocumentDigestMismatch);

    auto extra = controller->submit(scheduleArgument(), QByteArrayView(bytes),
                                    QStringLiteral("2026-08-11T12:01:00Z"));
    QVERIFY(!extra.has_value());
    QCOMPARE(extra.error().code, app::WorkflowSessionErrorCode::UnexpectedDocument);
    QVERIFY(controller->state() == pristine_state);
    QVERIFY(controller->journal().empty());
    QVERIFY(controller->snapshot().commands == pristine_snapshot.commands);
    QVERIFY(controller->snapshot().events == pristine_snapshot.events);
}

void WorkflowSessionResumeTest::reopenRejectsJournalTampering_data() {
    QTest::addColumn<QString>("tamperKind");
    for (const auto* kind :
         {"command-id", "command-payload", "event-type", "event-authority", "event-payload",
          "event-group", "event-sequence", "overlapping-command", "missing-command",
          "extra-command", "missing-event", "docket", "recorded-time"}) {
        QTest::newRow(kind) << QString::fromLatin1(kind);
    }
}

void WorkflowSessionResumeTest::reopenRejectsJournalTampering() {
    QFETCH(QString, tamperKind);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto database_path = QDir(directory.path()).filePath(QStringLiteral("sessions.sqlite"));
    const auto asset_root = QDir(directory.path()).filePath(QStringLiteral("assets"));
    {
        auto created = createController(database_path, asset_root);
        if (!created) {
            QFAIL(qPrintable(created.error().message));
        }
        QVERIFY(submitHistory(**created));
    }
    QVERIFY(tamperDatabase(database_path, tamperKind));
    const auto reopened = reopenController(database_path, asset_root);
    QVERIFY(!reopened.has_value());
}

void WorkflowSessionResumeTest::reopenRejectsMissingOrCorruptAsset_data() {
    QTest::addColumn<bool>("removeAsset");
    QTest::newRow("missing") << true;
    QTest::newRow("corrupt") << false;
}

void WorkflowSessionResumeTest::reopenRejectsMissingOrCorruptAsset() {
    QFETCH(bool, removeAsset);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto database_path = QDir(directory.path()).filePath(QStringLiteral("sessions.sqlite"));
    const auto asset_root = QDir(directory.path()).filePath(QStringLiteral("assets"));
    QString digest_to_damage;
    {
        auto created = createController(database_path, asset_root);
        if (!created) {
            QFAIL(qPrintable(created.error().message));
        }
        QVERIFY(submitHistory(**created, false));
        digest_to_damage = (*created)->snapshot().asset_references.front().digest;
    }

    const auto object_path =
        QDir(storage::AssetStore(asset_root).objectsDirectory()).filePath(digest_to_damage);
    if (removeAsset) {
        QVERIFY(QFile::remove(object_path));
    } else {
        QFile object(object_path);
        QVERIFY(object.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(object.write("corrupt"), qint64{7});
        object.close();
    }
    const auto reopened = reopenController(database_path, asset_root);
    QVERIFY(!reopened.has_value());
    QCOMPARE(reopened.error().code, app::WorkflowSessionErrorCode::CorruptSession);
}

void WorkflowSessionResumeTest::reopenRejectsEnginePinAndSessionMismatch() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto database_path = QDir(directory.path()).filePath(QStringLiteral("sessions.sqlite"));
    const auto asset_root = QDir(directory.path()).filePath(QStringLiteral("assets"));
    {
        auto created = createController(database_path, asset_root);
        if (!created) {
            QFAIL(qPrintable(created.error().message));
        }
    }

    auto openFor = [&](QString revision, std::vector<storage::RevisionPin> expected_pins,
                       model::WorkflowState initial)
        -> std::expected<std::unique_ptr<app::WorkflowSessionController>,
                         app::WorkflowSessionError> {
        auto opened = storage::SessionStore::open(database_path);
        if (!opened) {
            return std::unexpected(app::WorkflowSessionError{
                app::WorkflowSessionErrorCode::SessionStoreFailure, opened.error().message});
        }
        return app::WorkflowSessionController::reopen(
            workflow(), caseDefinition(), std::move(initial),
            storage::AssetStore(asset_root, 1024 * 1024), std::move(*opened), std::move(revision),
            std::move(expected_pins));
    };

    QVERIFY(!openFor(QStringLiteral("engine.workflow.test.2"), pins(), initialState()).has_value());
    auto wrong_pins = pins();
    wrong_pins.front().digest = QString(64, u'b');
    QVERIFY(!openFor(QString::fromLatin1(engine_revision), wrong_pins, initialState()).has_value());
    auto invalid_pack_id = pins();
    invalid_pack_id.front().pack_id = QStringLiteral("invalid_pack");
    const auto bad_id =
        openFor(QString::fromLatin1(engine_revision), invalid_pack_id, initialState());
    QVERIFY(!bad_id.has_value());
    QCOMPARE(bad_id.error().code, app::WorkflowSessionErrorCode::InvalidConfiguration);
    auto invalid_version = pins();
    invalid_version.front().version = QStringLiteral("01.0.0");
    const auto bad_version =
        openFor(QString::fromLatin1(engine_revision), invalid_version, initialState());
    QVERIFY(!bad_version.has_value());
    QCOMPARE(bad_version.error().code, app::WorkflowSessionErrorCode::InvalidConfiguration);
    // The alternate initial identity cannot locate or silently adopt the original session.
    QVERIFY(
        !openFor(QString::fromLatin1(engine_revision), pins(), initialState("test.session.other"))
             .has_value());
}

void WorkflowSessionResumeTest::staleAppendLeavesControllerUnchanged() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto database_path = QDir(directory.path()).filePath(QStringLiteral("sessions.sqlite"));
    const auto asset_root = QDir(directory.path()).filePath(QStringLiteral("assets"));
    auto first_result = createController(database_path, asset_root);
    if (!first_result) {
        QFAIL(qPrintable(first_result.error().message));
    }
    auto first = std::move(*first_result);
    auto stale_result = reopenController(database_path, asset_root);
    if (!stale_result) {
        QFAIL(qPrintable(stale_result.error().message));
    }
    auto stale = std::move(*stale_result);
    const auto stale_state = stale->state();
    const auto stale_journal = stale->journal();
    const auto stale_snapshot = stale->snapshot();

    const QByteArray first_document("first concurrent document");
    QVERIFY(first
                ->submit(deficientFiling(first_document), QByteArrayView(first_document),
                         QStringLiteral("2026-08-11T12:01:00Z"))
                .has_value());

    const QByteArray stale_document("stale concurrent document");
    auto stale_command = deficientFiling(stale_document);
    stale_command.header.command_id = model::WorkflowCommandId{"test.command.stale"};
    stale_command.filing_id = model::WorkflowFilingId{"test.filing.stale"};
    const auto submitted = stale->submit(stale_command, QByteArrayView(stale_document),
                                         QStringLiteral("2026-08-11T12:02:00Z"));
    QVERIFY(!submitted.has_value());
    QCOMPARE(submitted.error().code, app::WorkflowSessionErrorCode::SessionStoreFailure);
    QVERIFY(stale->state() == stale_state);
    QVERIFY(stale->journal() == stale_journal);
    QVERIFY(stale->snapshot().pins == stale_snapshot.pins);
    QVERIFY(stale->snapshot().commands == stale_snapshot.commands);
    QVERIFY(stale->snapshot().events == stale_snapshot.events);
    QCOMPARE(stale->snapshot().sequence, stale_snapshot.sequence);
}

} // namespace

QTEST_GUILESS_MAIN(WorkflowSessionResumeTest)

#include "tst_workflow_session_resume.moc"
