#include "appellate/model/workflow_command.hpp"
#include "appellate/packs/pack_archive.hpp"
#include "appellate/packs/pack_catalog.hpp"
#include "appellate/packs/runtime_pack.hpp"
#include "appellate/storage/asset_store.hpp"
#include "appellate/storage/workflow_codec.hpp"
#include "local_session_provider.hpp"
#include "main_window.hpp"
#include "oral_argument_workspace.hpp"
#include "workflow_session_controller.hpp"

#include <QAction>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <QVariant>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {

namespace app = appellate::app;
namespace model = appellate::model;
namespace packs = appellate::packs;
namespace storage = appellate::storage;
namespace ui = appellate::ui;
using namespace std::chrono_literals;

constexpr auto expected_asterglen_workflow_session =
    "workflow.session.5f62a8255168bf9cabfe35af7e09ad86d368dcbd37683cc5206010f170e8db70";
constexpr auto expected_fixture_workflow_session =
    "workflow.session.16a9ac9c6f55f8a2390d031e64de8f2deb23f46e34e37a5a5aa87d5e9e3a0df2";
constexpr auto expected_fixture_filed_oral_session =
    "oral.argument.session.8d46bd938a24fe7471d7faaff3f440af601dad00073840f165c02507fb63cb22";

[[nodiscard]] QString fixture(QStringView name) {
    return QDir(QStringLiteral(APPELLATE_TEST_FIXTURES)).filePath(name.toString());
}

[[nodiscard]] model::LegalTime at(int year, unsigned month, unsigned day, int hour) {
    const auto court_date = model::LegalDate{std::chrono::year{year} / std::chrono::month{month} /
                                             std::chrono::day{day}};
    return model::LegalTime{
        std::chrono::sys_seconds{std::chrono::sys_days{court_date.value}} +
            std::chrono::hours{hour},
        court_date,
    };
}

[[nodiscard]] QByteArray sha256(QByteArrayView value) {
    return QCryptographicHash::hash(value, QCryptographicHash::Sha256).toHex();
}

[[nodiscard]] auto copyFixtureTree(const QString& source_root, const QString& destination_root)
    -> std::expected<void, QString> {
    if (!QDir{}.mkpath(destination_root)) {
        return std::unexpected(QStringLiteral("Cannot create temporary authoring-pack root"));
    }
    const QDir source(source_root);
    QDirIterator iterator(source_root, QDir::AllEntries | QDir::NoDotAndDotDot,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const auto source_path = iterator.next();
        const auto relative_path = source.relativeFilePath(source_path);
        const auto destination_path = QDir(destination_root).filePath(relative_path);
        const QFileInfo info(source_path);
        if (info.isDir()) {
            if (!QDir{}.mkpath(destination_path)) {
                return std::unexpected(
                    QStringLiteral("Cannot create temporary fixture directory %1")
                        .arg(relative_path));
            }
        } else if (!info.isFile() || !QFile::copy(source_path, destination_path)) {
            return std::unexpected(
                QStringLiteral("Cannot copy temporary fixture entry %1").arg(relative_path));
        }
    }
    return {};
}

[[nodiscard]] auto writeJson(const QString& path, const QJsonObject& object)
    -> std::expected<QByteArray, QString> {
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(bytes) != bytes.size()) {
        return std::unexpected(QStringLiteral("Cannot write temporary JSON %1").arg(path));
    }
    file.close();
    return bytes;
}

[[nodiscard]] auto branchEligibilityPack(const QString& temporary_root)
    -> std::expected<QString, QString> {
    const auto authoring_root = QDir(temporary_root).filePath(QStringLiteral("branch-pack"));
    if (const auto copied = copyFixtureTree(fixture(u"full-resource-pack-v2"), authoring_root);
        !copied) {
        return std::unexpected(copied.error());
    }

    const auto workflow_path =
        QDir(authoring_root).filePath(QStringLiteral("resources/workflow.json"));
    QFile workflow_file(workflow_path);
    if (!workflow_file.open(QIODevice::ReadOnly)) {
        return std::unexpected(QStringLiteral("Cannot read temporary workflow fixture"));
    }
    QJsonParseError workflow_error;
    auto workflow = QJsonDocument::fromJson(workflow_file.readAll(), &workflow_error).object();
    workflow_file.close();
    if (workflow_error.error != QJsonParseError::NoError || workflow.isEmpty()) {
        return std::unexpected(QStringLiteral("Temporary workflow fixture is invalid JSON"));
    }
    auto operations = workflow.value(QStringLiteral("operations")).toArray();
    const auto authority = QJsonObject{
        {QStringLiteral("primary_authority_id"), QStringLiteral("example.authority.submission")},
        {QStringLiteral("supporting_authority_ids"), QJsonArray{}},
    };
    const auto advance = [&](QString id, QJsonObject precondition) {
        return QJsonObject{
            {QStringLiteral("operation_id"), std::move(id)},
            {QStringLiteral("stage_id"), QStringLiteral("example.stage.opened")},
            {QStringLiteral("opcode"), QStringLiteral("advance_stage")},
            {QStringLiteral("authority"), authority},
            {QStringLiteral("next_stage_id"), QStringLiteral("example.stage.submitted")},
            {QStringLiteral("authorized_role_ids"),
             QJsonArray{QStringLiteral("example.role.court")}},
            {QStringLiteral("preconditions"), QJsonArray{std::move(precondition)}},
        };
    };
    operations.push_back(
        advance(QStringLiteral("example.operation.aaa-blocked"),
                QJsonObject{{QStringLiteral("kind"), QStringLiteral("order_disposition")},
                            {QStringLiteral("order_id"), QStringLiteral("example.order.never")},
                            {QStringLiteral("disposition"), QStringLiteral("denied")}}));
    operations.push_back(
        advance(QStringLiteral("example.operation.zzz-eligible"),
                QJsonObject{{QStringLiteral("kind"), QStringLiteral("argument_scheduled")},
                            {QStringLiteral("scheduled"), true}}));
    operations.push_back(QJsonObject{
        {QStringLiteral("operation_id"), QStringLiteral("example.operation.schedule-test")},
        {QStringLiteral("stage_id"), QStringLiteral("example.stage.opened")},
        {QStringLiteral("opcode"), QStringLiteral("schedule_argument")},
        {QStringLiteral("authority"), authority},
        {QStringLiteral("authorized_role_ids"), QJsonArray{QStringLiteral("example.role.court")}},
    });
    workflow.insert(QStringLiteral("operations"), operations);
    const auto workflow_bytes = writeJson(workflow_path, workflow);
    if (!workflow_bytes) {
        return std::unexpected(workflow_bytes.error());
    }

    const auto manifest_path = QDir(authoring_root).filePath(QStringLiteral("manifest.json"));
    QFile manifest_file(manifest_path);
    if (!manifest_file.open(QIODevice::ReadOnly)) {
        return std::unexpected(QStringLiteral("Cannot read temporary manifest fixture"));
    }
    QJsonParseError manifest_error;
    auto manifest = QJsonDocument::fromJson(manifest_file.readAll(), &manifest_error).object();
    manifest_file.close();
    if (manifest_error.error != QJsonParseError::NoError || manifest.isEmpty()) {
        return std::unexpected(QStringLiteral("Temporary manifest fixture is invalid JSON"));
    }
    auto contents = manifest.value(QStringLiteral("contents")).toArray();
    bool replaced = false;
    for (auto index = 0; index < contents.size(); ++index) {
        auto entry = contents.at(index).toObject();
        if (entry.value(QStringLiteral("path")).toString() ==
            QStringLiteral("resources/workflow.json")) {
            entry.insert(QStringLiteral("sha256"),
                         QString::fromLatin1(sha256(QByteArrayView(*workflow_bytes))));
            contents.replace(index, entry);
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        return std::unexpected(QStringLiteral("Temporary manifest has no workflow resource"));
    }
    manifest.insert(QStringLiteral("contents"), contents);
    if (const auto written = writeJson(manifest_path, manifest); !written) {
        return std::unexpected(written.error());
    }

    const auto archive_path = QDir(temporary_root).filePath(QStringLiteral("branch-pack.awpack"));
    const auto exported = packs::PackArchive::exportDirectory(authoring_root, archive_path);
    if (!exported) {
        return std::unexpected(exported.error().message);
    }
    return archive_path;
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
        QStringLiteral("local-session-rows-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
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
        return std::unexpected(failure);
    }
    return encoded;
}

[[nodiscard]] auto executeSql(const QString& database_path, const QString& sql,
                              const QVariantList& bindings = {}) -> std::expected<void, QString> {
    const auto connection_name =
        QStringLiteral("local-session-mutate-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    QString failure;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name);
        database.setDatabaseName(database_path);
        if (!database.open()) {
            failure = database.lastError().text();
        } else {
            QSqlQuery query(database);
            query.prepare(sql);
            for (const auto& binding : bindings) {
                query.addBindValue(binding);
            }
            if (!query.exec()) {
                failure = query.lastError().text();
            }
        }
        database.close();
        database = QSqlDatabase{};
    }
    QSqlDatabase::removeDatabase(connection_name);
    if (!failure.isEmpty()) {
        return std::unexpected(failure);
    }
    return {};
}

[[nodiscard]] std::vector<std::string>
groundingIds(const model::AuthoredQuestionSelection& question) {
    std::vector<std::string> ids;
    ids.reserve(question.grounding.size());
    for (const auto& grounding : question.grounding) {
        ids.push_back(std::visit([](const auto& value) { return value.grounding_id; }, grounding));
    }
    return ids;
}

[[nodiscard]] storage::SessionSnapshot completeSnapshot() {
    return storage::SessionSnapshot{
        QStringLiteral("test.workflow.session"),
        QStringLiteral("engine.workflow.test.v1"),
        storage::SessionAuthorityContract::CanonicalV2,
        7,
        {storage::RevisionPin{QStringLiteral("test.pack"), QStringLiteral("1.2.3"),
                              QString(64, u'a')}},
        {storage::StoredCommand{QStringLiteral("test.command.one"), 5,
                                QByteArrayLiteral("{\"command\":1}"),
                                QStringLiteral("2026-08-11T01:02:03Z")}},
        {storage::StoredEvent{6, QStringLiteral("workflow.stage.advanced"),
                              QByteArrayLiteral("{\"event\":1}"),
                              QStringLiteral("test.authority.one")}},
        {storage::DocketEntry{QStringLiteral("test.docket.one"), 6,
                              QStringLiteral("Test docket title"), QStringLiteral("entered")}},
        {storage::AssetReference{QString(64, u'b'), QStringLiteral("workflow.order-document")}},
        QStringLiteral("2026-08-11T00:00:00Z"),
    };
}

[[nodiscard]] bool sameSnapshot(const storage::SessionSnapshot& left,
                                const storage::SessionSnapshot& right) {
    return left.session_id == right.session_id && left.engine_revision == right.engine_revision &&
           left.authority_contract == right.authority_contract && left.sequence == right.sequence &&
           left.pins == right.pins && left.commands == right.commands &&
           left.events == right.events && left.docket == right.docket &&
           left.asset_references == right.asset_references &&
           left.created_at_utc == right.created_at_utc;
}

class MainWindowLocalSessionsTest final : public QObject {
    Q_OBJECT

  private slots:
    void digestFramesEveryPersistedSnapshotField();
    void providerPersistsWorkflowCasAndDigestBoundOralSessions();
    void asterglenWorkflowActionPersistsAndReopensThroughMainWindow();
    void workflowPreviewSkipsIneligibleBranchAndDisablesWhenNoneEligible();
    void missingProviderIsVisibleAndDisabled();
    void unsafePathsAndNewerSchemaFailWithoutFallback();
    void independentProviderFailsPromptlyAndLaterReopens();
    void pinAndEngineMismatchFailWithoutDatabaseOrCasMutation();
};

void MainWindowLocalSessionsTest::digestFramesEveryPersistedSnapshotField() {
    const auto baseline = completeSnapshot();
    // Independently frozen vector for the v2 domain-separated, length-framed encoding above.
    const auto expected =
        QStringLiteral("408e22d56295fc65c25f10ffebdbe495b9feff31b84bde21189fbf7bf7544704");
    QCOMPARE(ui::workflowLegalStateDigest(baseline), expected);

    const auto changes = [&](auto mutate) {
        auto changed = baseline;
        mutate(changed);
        QVERIFY(ui::workflowLegalStateDigest(changed) != expected);
    };
    changes([](auto& value) { value.session_id += u".changed"; });
    changes([](auto& value) { value.engine_revision += u".changed"; });
    changes([](auto& value) {
        value.authority_contract = storage::SessionAuthorityContract::LegacyV1;
    });
    changes([](auto& value) { ++value.sequence; });
    changes([](auto& value) { value.created_at_utc += u".changed"; });
    changes([](auto& value) { value.pins.clear(); });
    changes([](auto& value) { value.pins.front().pack_id += u".changed"; });
    changes([](auto& value) { value.pins.front().version += u".changed"; });
    changes([](auto& value) { value.pins.front().digest.front() = u'c'; });
    changes([](auto& value) { value.commands.clear(); });
    changes([](auto& value) { value.commands.front().command_id += u".changed"; });
    changes([](auto& value) { ++value.commands.front().expected_sequence; });
    changes([](auto& value) { value.commands.front().payload_json += QByteArrayLiteral(" "); });
    changes([](auto& value) { value.commands.front().recorded_at_utc += u".changed"; });
    changes([](auto& value) { value.events.clear(); });
    changes([](auto& value) { ++value.events.front().sequence; });
    changes([](auto& value) { value.events.front().event_type += u".changed"; });
    changes([](auto& value) { value.events.front().payload_json += QByteArrayLiteral(" "); });
    changes([](auto& value) { value.events.front().authority_id += u".changed"; });
    changes([](auto& value) { value.docket.clear(); });
    changes([](auto& value) { value.docket.front().entry_id += u".changed"; });
    changes([](auto& value) { ++value.docket.front().event_sequence; });
    changes([](auto& value) { value.docket.front().title += u".changed"; });
    changes([](auto& value) { value.docket.front().status += u".changed"; });
    changes([](auto& value) { value.asset_references.clear(); });
    changes([](auto& value) { value.asset_references.front().digest.front() = u'c'; });
    changes([](auto& value) { value.asset_references.front().purpose += u".changed"; });
}

void MainWindowLocalSessionsTest::providerPersistsWorkflowCasAndDigestBoundOralSessions() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto archive_path = temporary.filePath(QStringLiteral("grounded.awpack"));
    const auto exported =
        packs::PackArchive::exportDirectory(fixture(u"full-resource-pack-v2"), archive_path);
    QVERIFY2(exported.has_value(), exported ? "" : qPrintable(exported.error().message));
    const auto catalog = packs::PackCatalog::open(temporary.filePath(QStringLiteral("catalog")));
    QVERIFY2(catalog.has_value(), catalog ? "" : qPrintable(catalog.error().message));
    const auto installed =
        (*catalog)->installArchive(archive_path, QStringLiteral("2026-08-11T00:00:00Z"));
    QVERIFY2(installed.has_value(), installed ? "" : qPrintable(installed.error().message));
    const auto resolved = (*catalog)->loadResolved(installed->revision);
    QVERIFY2(resolved.has_value(), resolved ? "" : qPrintable(resolved.error().message));
    const auto runtime = packs::loadRuntimePack(*resolved);
    QVERIFY2(runtime.has_value(), runtime ? "" : runtime.error().message.c_str());
    QCOMPARE(runtime->cases.size(), std::size_t{1});
    const auto& runtime_case = runtime->cases.front();
    QCOMPARE(runtime_case.argument_configurations.size(), std::size_t{2});
    QVERIFY(runtime_case.argument_configurations.front().grounded_question_bank.has_value());

    const ui::LocalSessionPaths paths{
        temporary.filePath(QStringLiteral("state/sessions.sqlite")),
        temporary.filePath(QStringLiteral("state/assets")),
    };
    const auto fixed_clock = [] {
        return QDateTime::fromString(QStringLiteral("2026-08-11T10:00:00Z"), Qt::ISODate);
    };
    auto provider = ui::LocalSessionProvider::create(paths, fixed_clock);
    QVERIFY2(provider.has_value(), provider ? "" : qPrintable(provider.error()));

    auto workflow = (*provider)->openWorkflow(*resolved, runtime_case.definition.id);
    QVERIFY2(workflow.has_value(), workflow ? "" : qPrintable(workflow.error().message));
    const auto workflow_session_id = QString::fromStdString((*workflow)->state().session_id);
    QCOMPARE(workflow_session_id, QString::fromLatin1(expected_fixture_workflow_session));
    QCOMPARE((*workflow)->snapshot().pins.size(), std::size_t{1});
    QCOMPARE((*workflow)->snapshot().pins.front().pack_id,
             QString::fromStdString(exported->id.value));
    const auto pristine_workflow = (*workflow)->snapshot();
    const auto pristine_workflow_rows = databaseRows(paths.database_path, workflow_session_id);
    QVERIFY2(pristine_workflow_rows.has_value(),
             pristine_workflow_rows ? "" : qPrintable(pristine_workflow_rows.error()));

    const auto argument_id = runtime_case.argument_configurations.front().id;
    auto oral = (*provider)->open(*resolved, runtime_case.definition.id, argument_id);
    QVERIFY2(oral.has_value(), oral ? "" : qPrintable(oral.error().message));
    const auto pristine_oral_id = (*oral)->sessionId();
    QVERIFY(pristine_oral_id.startsWith(QStringLiteral("oral.argument.session.")));
    QVERIFY((*oral)->canonicalDefinition() != nullptr);
    const auto& opening = (*oral)->state().journal.back().bench;
    QVERIFY(opening.question.has_value());
    const auto* authored =
        std::get_if<model::AuthoredQuestionSelection>(&opening.question->selection);
    QVERIFY(authored != nullptr);
    const auto submitted_answer = (*oral)->submit(
        pristine_oral_id + QStringLiteral(".answer-2"),
        model::CounselAnswer{model::CounselActKind::Answer,
                             "Production provider persisted this grounded answer",
                             opening.question->issue_id, groundingIds(*authored), 1.0, 1s},
        QStringLiteral("2026-08-11T10:00:01Z"));
    QVERIFY2(submitted_answer.has_value(),
             submitted_answer ? "" : qPrintable(submitted_answer.error().message));
    const auto answered_state = (*oral)->state();
    (*oral).reset();

    QVERIFY(sameSnapshot((*workflow)->snapshot(), pristine_workflow));
    const auto workflow_rows_after_oral = databaseRows(paths.database_path, workflow_session_id);
    QVERIFY2(workflow_rows_after_oral.has_value(),
             workflow_rows_after_oral ? "" : qPrintable(workflow_rows_after_oral.error()));
    QCOMPARE(*workflow_rows_after_oral, *pristine_workflow_rows);
    (*workflow).reset();
    provider->reset();

    const auto offset_clock = [] {
        return QDateTime::fromString(QStringLiteral("2026-08-11T10:00:00Z"), Qt::ISODate)
            .toOffsetFromUtc(9 * 60 * 60);
    };
    auto second_provider = ui::LocalSessionProvider::create(paths, offset_clock);
    QVERIFY2(second_provider.has_value(),
             second_provider ? "" : qPrintable(second_provider.error()));
    auto reopened_oral =
        (*second_provider)->open(*resolved, runtime_case.definition.id, argument_id);
    QVERIFY2(reopened_oral.has_value(),
             reopened_oral ? "" : qPrintable(reopened_oral.error().message));
    QCOMPARE((*reopened_oral)->sessionId(), pristine_oral_id);
    QCOMPARE((*reopened_oral)->state(), answered_state);
    (*reopened_oral).reset();
    second_provider->reset();

    auto filing_provider = ui::LocalSessionProvider::create(paths, fixed_clock);
    QVERIFY2(filing_provider.has_value(),
             filing_provider ? "" : qPrintable(filing_provider.error()));
    workflow = (*filing_provider)->openWorkflow(*resolved, runtime_case.definition.id);
    QVERIFY2(workflow.has_value(), workflow ? "" : qPrintable(workflow.error().message));

    const QByteArray filing_document("production local provider filing bytes");
    const auto filing = model::SubmitWorkflowFiling{
        model::WorkflowCommandHeader{
            workflow_session_id.toStdString(),
            model::WorkflowCommandId{workflow_session_id.toStdString() +
                                     ".command.1.example.operation.accept-notice"},
            model::ActorId{"example.actor.appellant"},
            at(2026, 8U, 11U, 11),
        },
        model::WorkflowFilingId{"example.filing.production-provider"},
        model::FilingTypeId{"example.filing.notice"},
        sha256(filing_document).toStdString(),
        {model::WorkflowFieldValue{model::FilingFieldId{"example.field.caption"},
                                   "Production provider caption"}},
        {model::ActorId{"example.actor.appellee"}},
        std::nullopt,
    };
    const auto filed =
        (*workflow)->submit(model::WorkflowCommand{filing}, QByteArrayView(filing_document),
                            QStringLiteral("2026-08-11T11:00:00Z"));
    QVERIFY2(filed.has_value(), filed ? "" : qPrintable(filed.error().message));
    QVERIFY(filed->asset.has_value());
    const auto asset = storage::AssetStore(paths.asset_root).read(filed->asset->sha256);
    QVERIFY2(asset.has_value(), asset ? "" : qPrintable(asset.error().message));
    QCOMPARE(*asset, filing_document);
    const auto filed_state = (*workflow)->state();
    const auto filed_snapshot = (*workflow)->snapshot();
    QCOMPARE(filed_snapshot.commands.size(), std::size_t{1});
    QCOMPARE(filed_snapshot.commands.front().command_id,
             workflow_session_id + QStringLiteral(".command.1.example.operation.accept-notice"));
    QCOMPARE(filed_snapshot.asset_references.size(), std::size_t{1});
    QVERIFY(!filed_snapshot.events.empty());
    (*workflow).reset();
    filing_provider->reset();

    const auto final_provider = ui::LocalSessionProvider::create(paths, offset_clock);
    QVERIFY2(final_provider.has_value(), final_provider ? "" : qPrintable(final_provider.error()));

    auto reopened_workflow = (*final_provider)->openWorkflow(*resolved, runtime_case.definition.id);
    QVERIFY2(reopened_workflow.has_value(),
             reopened_workflow ? "" : qPrintable(reopened_workflow.error().message));
    QCOMPARE(QString::fromStdString((*reopened_workflow)->state().session_id), workflow_session_id);
    QCOMPARE((*reopened_workflow)->state(), filed_state);
    QVERIFY(sameSnapshot((*reopened_workflow)->snapshot(), filed_snapshot));
    (*reopened_workflow).reset();

    auto digest_bound_oral =
        (*final_provider)->open(*resolved, runtime_case.definition.id, argument_id);
    QVERIFY2(digest_bound_oral.has_value(),
             digest_bound_oral ? "" : qPrintable(digest_bound_oral.error().message));
    QVERIFY((*digest_bound_oral)->sessionId() != pristine_oral_id);
    QCOMPARE((*digest_bound_oral)->sessionId(),
             QString::fromLatin1(expected_fixture_filed_oral_session));
    QCOMPARE((*digest_bound_oral)->canonicalDefinition()->configuration.legal_state_digest,
             ui::workflowLegalStateDigest(filed_snapshot).toStdString());
}

void MainWindowLocalSessionsTest::asterglenWorkflowActionPersistsAndReopensThroughMainWindow() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const ui::LocalSessionPaths paths{
        temporary.filePath(QStringLiteral("state/sessions.sqlite")),
        temporary.filePath(QStringLiteral("state/assets")),
    };
    const auto fixed_clock = [] {
        return QDateTime::fromString(QStringLiteral("2026-08-11T10:00:00Z"), Qt::ISODate);
    };
    int legal_clock_samples = 0;
    const ui::WorkflowLegalClock cross_midnight_legal_clock =
        [&legal_clock_samples](const QDate& selected_court_date)
        -> std::expected<ui::WorkflowLegalClockReading, QString> {
        ++legal_clock_samples;
        return ui::WorkflowLegalClockReading{
            QDateTime::fromString(QStringLiteral("2026-08-12T00:30:00Z"), Qt::ISODate),
            selected_court_date};
    };
    const auto archive = QStringLiteral(APPELLATE_GOLD_ARCHIVE);
    const auto catalog = temporary.filePath(QStringLiteral("catalog"));
    storage::SessionSnapshot persisted;
    {
        const auto provider = ui::LocalSessionProvider::create(paths, fixed_clock);
        QVERIFY2(provider.has_value(), provider ? "" : qPrintable(provider.error()));
        ui::MainWindow window(archive, catalog, nullptr, *provider, {}, {}, *provider,
                              cross_midnight_legal_clock);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.currentRuntime() != nullptr);
        QVERIFY(window.openWorkflowAction()->isEnabled());
        QVERIFY(!window.advanceWorkflowAction()->isEnabled());
        window.workflowCourtDateEditor()->setText(QStringLiteral("2026-08-11"));
        QTest::mouseClick(window.openWorkflowButton(), Qt::LeftButton);
        QTRY_VERIFY(window.workflowSessionController() != nullptr);
        QVERIFY(window.workflowStatusLabel()->text().contains(
            QStringLiteral("ca4r54b.operation.advance-docketed")));
        QVERIFY(window.workflowStatusLabel()->text().contains(
            QStringLiteral("ca4r54b.actor.ca4-clerk")));
        QVERIFY(window.advanceWorkflowAction()->isEnabled());
        const auto samples_before_transition = legal_clock_samples;
        window.advanceWorkflowAction()->trigger();
        QCOMPARE(legal_clock_samples, samples_before_transition + 1);
        QTRY_COMPARE(window.workflowSessionController()->state().current_stage_id.value,
                     std::string("ca4r54b.stage.docketed"));
        QCOMPARE(window.workflowSessionController()->state().current_stage_id.value,
                 std::string("ca4r54b.stage.docketed"));
        QCOMPARE(window.workflowSessionController()->snapshot().commands.size(), std::size_t{1});
        QCOMPARE(QString::fromStdString(window.workflowSessionController()->state().session_id),
                 QString::fromLatin1(expected_asterglen_workflow_session));
        QCOMPARE(window.workflowSessionController()->snapshot().commands.front().command_id,
                 QString::fromLatin1(expected_asterglen_workflow_session) +
                     QStringLiteral(".command.1.ca4r54b.operation.advance-docketed"));
        QCOMPARE(window.workflowSessionController()->snapshot().events.size(), std::size_t{1});
        const auto decoded = storage::decodeWorkflowCommand(
            window.workflowSessionController()->snapshot().commands.front().payload_json);
        QVERIFY(decoded.has_value());
        const auto* advanced = std::get_if<model::AdvanceWorkflowStage>(&*decoded);
        QVERIFY(advanced != nullptr);
        QCOMPARE(advanced->header.occurred_at.court_date,
                 model::LegalDate{std::chrono::year{2026} / std::chrono::month{8} /
                                  std::chrono::day{11}});
        QCOMPARE(advanced->header.occurred_at.instant,
                 std::chrono::sys_seconds{std::chrono::sys_days{
                     std::chrono::year{2026} / std::chrono::month{8} / std::chrono::day{12}}} +
                     std::chrono::minutes{30});
        persisted = window.workflowSessionController()->snapshot();
        window.caseList()->setCurrentRow(-1);
        QCOMPARE(window.workflowSessionController(), nullptr);
        QVERIFY(!window.advanceWorkflowAction()->isEnabled());
        window.caseList()->setCurrentRow(0);
        window.openWorkflowAction()->trigger();
        QTRY_VERIFY(window.workflowSessionController() != nullptr);
        QVERIFY(sameSnapshot(window.workflowSessionController()->snapshot(), persisted));
    }

    const auto reopened_provider = ui::LocalSessionProvider::create(paths, fixed_clock);
    QVERIFY2(reopened_provider.has_value(),
             reopened_provider ? "" : qPrintable(reopened_provider.error()));
    ui::MainWindow reopened(archive, catalog, nullptr, *reopened_provider, {}, {},
                            *reopened_provider, cross_midnight_legal_clock);
    reopened.workflowCourtDateEditor()->setText(QStringLiteral("2026-08-11"));
    QVERIFY(reopened.openSelectedWorkflow().has_value());
    QVERIFY(reopened.workflowSessionController() != nullptr);
    QVERIFY(sameSnapshot(reopened.workflowSessionController()->snapshot(), persisted));
    QCOMPARE(reopened.workflowSessionController()->state().current_stage_id.value,
             std::string("ca4r54b.stage.docketed"));
    QVERIFY(reopened.workflowStatusLabel()->text().contains(
        QStringLiteral("ca4r54b.operation.advance-initial-requirements")));
    QVERIFY(
        reopened.workflowStatusLabel()->text().contains(QStringLiteral("ca4r54b.actor.ca4-clerk")));
    QVERIFY(reopened.advanceWorkflowAction()->isEnabled());
    QVERIFY(reopened.loadSource(fixture(u"full-resource-pack-v2")).has_value());
    QCOMPARE(reopened.workflowSessionController(), nullptr);
    QVERIFY(!reopened.openWorkflowAction()->isEnabled());
    QVERIFY(!reopened.advanceWorkflowAction()->isEnabled());
}

void MainWindowLocalSessionsTest::
    workflowPreviewSkipsIneligibleBranchAndDisablesWhenNoneEligible() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto archive = branchEligibilityPack(temporary.path());
    QVERIFY2(archive.has_value(), archive ? "" : qPrintable(archive.error()));

    const auto catalog_root = temporary.filePath(QStringLiteral("branch-catalog"));
    const auto catalog = packs::PackCatalog::open(catalog_root);
    QVERIFY2(catalog.has_value(), catalog ? "" : qPrintable(catalog.error().message));
    const auto installed =
        (*catalog)->installArchive(*archive, QStringLiteral("2026-08-11T00:00:00Z"));
    QVERIFY2(installed.has_value(), installed ? "" : qPrintable(installed.error().message));
    const auto resolved = (*catalog)->loadResolved(installed->revision);
    QVERIFY2(resolved.has_value(), resolved ? "" : qPrintable(resolved.error().message));
    const auto runtime = packs::loadRuntimePack(*resolved);
    QVERIFY2(runtime.has_value(), runtime ? "" : runtime.error().message.c_str());
    QCOMPARE(runtime->cases.size(), std::size_t{1});
    const auto& runtime_case = runtime->cases.front();

    const ui::LocalSessionPaths paths{
        temporary.filePath(QStringLiteral("branch-state/sessions.sqlite")),
        temporary.filePath(QStringLiteral("branch-state/assets")),
    };
    const auto fixed_clock = [] {
        return QDateTime::fromString(QStringLiteral("2026-08-11T10:00:00Z"), Qt::ISODate);
    };

    QString session_id;
    {
        const auto provider = ui::LocalSessionProvider::create(paths, fixed_clock);
        QVERIFY2(provider.has_value(), provider ? "" : qPrintable(provider.error()));
        auto workflow = (*provider)->openWorkflow(*resolved, runtime_case.definition.id);
        QVERIFY2(workflow.has_value(), workflow ? "" : qPrintable(workflow.error().message));
        session_id = QString::fromStdString((*workflow)->state().session_id);
        const auto scheduled = (*workflow)->submit(
            model::WorkflowCommand{model::ScheduleWorkflowArgument{
                model::WorkflowCommandHeader{
                    session_id.toStdString(),
                    model::WorkflowCommandId{session_id.toStdString() +
                                             ".command.1.example.operation.schedule-test"},
                    model::ActorId{"example.actor.court"},
                    at(2026, 8U, 11U, 11),
                },
                model::WorkflowOperationId{"example.operation.schedule-test"},
                model::LegalDate{std::chrono::year{2026} / std::chrono::month{8} /
                                 std::chrono::day{20}},
            }},
            std::nullopt, QStringLiteral("2026-08-11T11:00:00Z"));
        QVERIFY2(scheduled.has_value(), scheduled ? "" : qPrintable(scheduled.error().message));
        QCOMPARE((*workflow)->state().current_stage_id.value, std::string("example.stage.opened"));
        QVERIFY((*workflow)->state().argument_date.has_value());
        QCOMPARE((*workflow)->snapshot().commands.size(), std::size_t{1});
    }

    enum class LegalClockMode { Eligible, Failure, Backdated, Alternating };
    auto legal_clock_mode = LegalClockMode::Eligible;
    int legal_clock_samples = 0;
    int alternating_clock_samples = 0;
    const ui::WorkflowLegalClock legal_clock = [&legal_clock_samples, &alternating_clock_samples,
                                                &legal_clock_mode](const QDate& selected_court_date)
        -> std::expected<ui::WorkflowLegalClockReading, QString> {
        ++legal_clock_samples;
        if (legal_clock_mode == LegalClockMode::Failure ||
            (legal_clock_mode == LegalClockMode::Alternating &&
             ++alternating_clock_samples % 2 == 0)) {
            return std::unexpected(QStringLiteral("Injected legal clock failure"));
        }
        return ui::WorkflowLegalClockReading{
            QDateTime::fromString(legal_clock_mode == LegalClockMode::Backdated
                                      ? QStringLiteral("2026-08-11T10:00:00Z")
                                      : QStringLiteral("2026-08-11T12:00:00Z"),
                                  Qt::ISODate),
            selected_court_date};
    };
    const auto provider = ui::LocalSessionProvider::create(paths, fixed_clock);
    QVERIFY2(provider.has_value(), provider ? "" : qPrintable(provider.error()));
    ui::MainWindow window({}, catalog_root, nullptr, *provider, {}, {}, *provider, legal_clock);
    window.workflowCourtDateEditor()->setText(QStringLiteral("2026-08-11"));
    const auto loaded = window.loadSource(*archive);
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error()));

    legal_clock_mode = LegalClockMode::Alternating;
    const auto samples_before_open = legal_clock_samples;
    QTest::mouseClick(window.openWorkflowButton(), Qt::LeftButton);
    QTRY_VERIFY(window.workflowSessionController() != nullptr);
    QCOMPARE(legal_clock_samples, samples_before_open + 1);
    QVERIFY(window.workflowStatusLabel()->text().contains(
        QStringLiteral("example.operation.zzz-eligible")));
    QVERIFY(!window.workflowStatusLabel()->text().contains(
        QStringLiteral("example.operation.aaa-blocked")));
    QVERIFY(window.advanceWorkflowButton()->isEnabled());
    legal_clock_mode = LegalClockMode::Eligible;

    const auto unchanged_snapshot = window.workflowSessionController()->snapshot();
    const auto unchanged_rows = databaseRows(paths.database_path, session_id);
    QVERIFY2(unchanged_rows.has_value(), unchanged_rows ? "" : qPrintable(unchanged_rows.error()));

    legal_clock_mode = LegalClockMode::Failure;
    const auto samples_before_clock_failure = legal_clock_samples;
    QTest::mouseClick(window.advanceWorkflowButton(), Qt::LeftButton);
    QCOMPARE(legal_clock_samples, samples_before_clock_failure + 1);
    QVERIFY(!window.advanceWorkflowButton()->isEnabled());
    QVERIFY(window.workflowStatusLabel()->text().contains(
        QStringLiteral("Injected legal clock failure")));
    QVERIFY(window.errorLabel()->text().contains(QStringLiteral("Injected legal clock failure")));
    QVERIFY(sameSnapshot(window.workflowSessionController()->snapshot(), unchanged_snapshot));
    auto rows_after_rejection = databaseRows(paths.database_path, session_id);
    QVERIFY2(rows_after_rejection.has_value(),
             rows_after_rejection ? "" : qPrintable(rows_after_rejection.error()));
    QCOMPARE(*rows_after_rejection, *unchanged_rows);

    legal_clock_mode = LegalClockMode::Eligible;
    window.workflowCourtDateEditor()->clear();
    window.workflowCourtDateEditor()->setText(QStringLiteral("2026-08-11"));
    QVERIFY(window.advanceWorkflowButton()->isEnabled());
    QVERIFY(window.workflowStatusLabel()->text().contains(
        QStringLiteral("example.operation.zzz-eligible")));

    legal_clock_mode = LegalClockMode::Backdated;
    const auto samples_before_none_eligible = legal_clock_samples;
    QTest::mouseClick(window.advanceWorkflowButton(), Qt::LeftButton);
    QCOMPARE(legal_clock_samples, samples_before_none_eligible + 1);
    QVERIFY(!window.advanceWorkflowButton()->isEnabled());
    QVERIFY(window.workflowStatusLabel()->text().contains(
        QStringLiteral("No currently eligible authored AdvanceStage")));
    QVERIFY(window.errorLabel()->text().contains(QStringLiteral("No currently eligible")));
    QVERIFY(sameSnapshot(window.workflowSessionController()->snapshot(), unchanged_snapshot));
    rows_after_rejection = databaseRows(paths.database_path, session_id);
    QVERIFY2(rows_after_rejection.has_value(),
             rows_after_rejection ? "" : qPrintable(rows_after_rejection.error()));
    QCOMPARE(*rows_after_rejection, *unchanged_rows);

    legal_clock_mode = LegalClockMode::Eligible;
    window.workflowCourtDateEditor()->clear();
    window.workflowCourtDateEditor()->setText(QStringLiteral("2026-08-11"));
    QVERIFY(window.advanceWorkflowButton()->isEnabled());
    const auto samples_before_eligible_click = legal_clock_samples;
    QTest::mouseClick(window.advanceWorkflowButton(), Qt::LeftButton);
    QCOMPARE(legal_clock_samples, samples_before_eligible_click + 1);
    QTRY_COMPARE(window.workflowSessionController()->state().current_stage_id.value,
                 std::string("example.stage.submitted"));
    QCOMPARE(window.workflowSessionController()->snapshot().commands.size(), std::size_t{2});
    const auto decided = storage::decodeWorkflowCommand(
        window.workflowSessionController()->snapshot().commands.back().payload_json);
    QVERIFY(decided.has_value());
    const auto* advance = std::get_if<model::AdvanceWorkflowStage>(&*decided);
    QVERIFY(advance != nullptr);
    QCOMPARE(advance->operation_id.value, std::string("example.operation.zzz-eligible"));

    QVERIFY(!window.advanceWorkflowButton()->isEnabled());
    QVERIFY(window.workflowStatusLabel()->text().contains(
        QStringLiteral("No currently eligible authored AdvanceStage")));
    const auto completed_snapshot = window.workflowSessionController()->snapshot();
    const auto completed_rows = databaseRows(paths.database_path, session_id);
    QVERIFY2(completed_rows.has_value(), completed_rows ? "" : qPrintable(completed_rows.error()));

    const auto samples_before_disabled_click = legal_clock_samples;
    QTest::mouseClick(window.advanceWorkflowButton(), Qt::LeftButton);
    QCOMPARE(legal_clock_samples, samples_before_disabled_click);
    QVERIFY(sameSnapshot(window.workflowSessionController()->snapshot(), completed_snapshot));
    const auto rejected = window.advanceSelectedWorkflow();
    QVERIFY(!rejected.has_value());
    QVERIFY(rejected.error().contains(QStringLiteral("No currently eligible")));
    QCOMPARE(legal_clock_samples, samples_before_disabled_click + 1);
    QVERIFY(sameSnapshot(window.workflowSessionController()->snapshot(), completed_snapshot));
    rows_after_rejection = databaseRows(paths.database_path, session_id);
    QVERIFY2(rows_after_rejection.has_value(),
             rows_after_rejection ? "" : qPrintable(rows_after_rejection.error()));
    QCOMPARE(*rows_after_rejection, *completed_rows);
}

void MainWindowLocalSessionsTest::missingProviderIsVisibleAndDisabled() {
    ui::MainWindow window;
    QVERIFY(window.workflowStatusLabel()->text().contains(QStringLiteral("unavailable"),
                                                          Qt::CaseInsensitive));
    QVERIFY(!window.openWorkflowAction()->isEnabled());
    QVERIFY(!window.openWorkflowButton()->isEnabled());
    QVERIFY(!window.advanceWorkflowAction()->isEnabled());
    QVERIFY(!window.advanceWorkflowButton()->isEnabled());
}

void MainWindowLocalSessionsTest::unsafePathsAndNewerSchemaFailWithoutFallback() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto clock = [] { return QDateTime::currentDateTimeUtc(); };

    const auto relative = ui::LocalSessionProvider::create(
        {QStringLiteral("relative.sqlite"), QStringLiteral("relative-assets")}, clock);
    QVERIFY(!relative.has_value());
    QVERIFY(relative.error().contains(QStringLiteral("absolute"), Qt::CaseInsensitive));

    const auto noncanonical_database =
        temporary.path() + QStringLiteral("/unused/../noncanonical.sqlite");
    const auto noncanonical_asset = temporary.filePath(QStringLiteral("noncanonical-assets"));
    const auto noncanonical =
        ui::LocalSessionProvider::create({noncanonical_database, noncanonical_asset}, clock);
    QVERIFY(!noncanonical.has_value());
    QVERIFY(!QFileInfo::exists(temporary.filePath(QStringLiteral("noncanonical.sqlite"))));
    QVERIFY(!QFileInfo::exists(noncanonical_asset));

    const auto file_parent_database = temporary.filePath(QStringLiteral("file-parent-database"));
    const auto file_parent_asset = QDir(file_parent_database).filePath(QStringLiteral("assets"));
    const auto file_parent =
        ui::LocalSessionProvider::create({file_parent_database, file_parent_asset}, clock);
    QVERIFY(!file_parent.has_value());
    QVERIFY(!QFileInfo::exists(file_parent_database));
    QVERIFY(!QFileInfo::exists(file_parent_asset));

    const auto shared_database_asset = temporary.filePath(QStringLiteral("shared-database-asset"));
    const auto shared_target =
        ui::LocalSessionProvider::create({shared_database_asset, shared_database_asset}, clock);
    QVERIFY(!shared_target.has_value());
    QVERIFY(!QFileInfo::exists(shared_database_asset));

    const auto containing_asset_root =
        temporary.filePath(QStringLiteral("asset-containing-database"));
    const auto contained_database =
        QDir(containing_asset_root).filePath(QStringLiteral("sessions.sqlite"));
    const auto reverse_containment =
        ui::LocalSessionProvider::create({contained_database, containing_asset_root}, clock);
    QVERIFY(!reverse_containment.has_value());
    QVERIFY(!QFileInfo::exists(containing_asset_root));

    const auto nul_path =
        temporary.filePath(QStringLiteral("nul")) + QChar::Null + QStringLiteral("sessions.sqlite");
    const auto nul = ui::LocalSessionProvider::create(
        {nul_path, temporary.filePath(QStringLiteral("nul-assets"))}, clock);
    QVERIFY(!nul.has_value());
    QVERIFY(!QFileInfo::exists(temporary.filePath(QStringLiteral("nul-assets"))));

    const auto database_directory = temporary.filePath(QStringLiteral("database-directory"));
    QVERIFY(QDir{}.mkpath(database_directory));
    const auto nonregular_database = ui::LocalSessionProvider::create(
        {database_directory, temporary.filePath(QStringLiteral("unused-assets"))}, clock);
    QVERIFY(!nonregular_database.has_value());
    QVERIFY(!QFileInfo::exists(temporary.filePath(QStringLiteral("unused-assets"))));

    const auto asset_file_path = temporary.filePath(QStringLiteral("asset-file"));
    QFile asset_file(asset_file_path);
    QVERIFY(asset_file.open(QIODevice::WriteOnly));
    QCOMPARE(asset_file.write("not a directory"), qint64{15});
    asset_file.close();
    const auto rejected_database = temporary.filePath(QStringLiteral("must-not-exist.sqlite"));
    const auto nonregular_asset =
        ui::LocalSessionProvider::create({rejected_database, asset_file_path}, clock);
    QVERIFY(!nonregular_asset.has_value());
    QVERIFY(!QFileInfo::exists(rejected_database));

    const auto database_parent_file = temporary.filePath(QStringLiteral("database-parent-file"));
    QFile database_parent(database_parent_file);
    QVERIFY(database_parent.open(QIODevice::WriteOnly));
    QCOMPARE(database_parent.write("parent sentinel"), qint64{15});
    database_parent.close();
    const auto bad_database_parent =
        ui::LocalSessionProvider::create({database_parent_file + QStringLiteral("/sessions.sqlite"),
                                          temporary.filePath(QStringLiteral("bad-parent-assets"))},
                                         clock);
    QVERIFY(!bad_database_parent.has_value());
    QVERIFY(!QFileInfo::exists(temporary.filePath(QStringLiteral("bad-parent-assets"))));

    const auto asset_parent_file = temporary.filePath(QStringLiteral("asset-parent-file"));
    QFile asset_parent(asset_parent_file);
    QVERIFY(asset_parent.open(QIODevice::WriteOnly));
    QCOMPARE(asset_parent.write("asset parent sentinel"), qint64{21});
    asset_parent.close();
    const auto unused_database = temporary.filePath(QStringLiteral("unused.sqlite"));
    const auto bad_asset_parent = ui::LocalSessionProvider::create(
        {unused_database, asset_parent_file + QStringLiteral("/assets")}, clock);
    QVERIFY(!bad_asset_parent.has_value());
    QVERIFY(!QFileInfo::exists(unused_database));

    const auto database_target = temporary.filePath(QStringLiteral("database-target.sqlite"));
    QFile database_target_file(database_target);
    QVERIFY(database_target_file.open(QIODevice::WriteOnly));
    QCOMPARE(database_target_file.write("database target sentinel"), qint64{24});
    database_target_file.close();
    const auto database_link = temporary.filePath(QStringLiteral("database-link.sqlite"));
    QVERIFY(QFile::link(database_target, database_link));
    const auto linked_database_assets =
        temporary.filePath(QStringLiteral("linked-database-assets"));
    const auto linked_database =
        ui::LocalSessionProvider::create({database_link, linked_database_assets}, clock);
    QVERIFY(!linked_database.has_value());
    QVERIFY(!QFileInfo::exists(linked_database_assets));
    QFile unchanged_database_target(database_target);
    QVERIFY(unchanged_database_target.open(QIODevice::ReadOnly));
    QCOMPARE(unchanged_database_target.readAll(), QByteArrayLiteral("database target sentinel"));

    const auto asset_target = temporary.filePath(QStringLiteral("asset-target"));
    QVERIFY(QDir{}.mkpath(asset_target));
    const auto asset_target_sentinel = QDir(asset_target).filePath(QStringLiteral("sentinel"));
    QFile asset_target_file(asset_target_sentinel);
    QVERIFY(asset_target_file.open(QIODevice::WriteOnly));
    QCOMPARE(asset_target_file.write("asset target sentinel"), qint64{21});
    asset_target_file.close();
    const auto asset_link = temporary.filePath(QStringLiteral("asset-link"));
    QVERIFY(QFile::link(asset_target, asset_link));
    const auto linked_asset_database = temporary.filePath(QStringLiteral("linked-asset.sqlite"));
    const auto linked_asset =
        ui::LocalSessionProvider::create({linked_asset_database, asset_link}, clock);
    QVERIFY(!linked_asset.has_value());
    QVERIFY(!QFileInfo::exists(linked_asset_database));
    QFile unchanged_asset_target(asset_target_sentinel);
    QVERIFY(unchanged_asset_target.open(QIODevice::ReadOnly));
    QCOMPARE(unchanged_asset_target.readAll(), QByteArrayLiteral("asset target sentinel"));

    const auto real_parent = temporary.filePath(QStringLiteral("real-parent"));
    QVERIFY(QDir{}.mkpath(real_parent));
    const auto parent_link = temporary.filePath(QStringLiteral("parent-link"));
    QVERIFY(QFile::link(real_parent, parent_link));
    const auto linked_parent_database =
        QDir(parent_link).filePath(QStringLiteral("sessions.sqlite"));
    const auto linked_parent = ui::LocalSessionProvider::create(
        {linked_parent_database, temporary.filePath(QStringLiteral("parent-link-assets"))}, clock);
    QVERIFY(!linked_parent.has_value());
    QVERIFY(!QFileInfo::exists(QDir(real_parent).filePath(QStringLiteral("sessions.sqlite"))));
    QVERIFY(!QFileInfo::exists(temporary.filePath(QStringLiteral("parent-link-assets"))));

    const auto corrupt_database = temporary.filePath(QStringLiteral("corrupt.sqlite"));
    const QByteArray corrupt_bytes("not a SQLite database\0with fixed bytes", 38);
    QFile corrupt_file(corrupt_database);
    QVERIFY(corrupt_file.open(QIODevice::WriteOnly));
    QCOMPARE(corrupt_file.write(corrupt_bytes), corrupt_bytes.size());
    corrupt_file.close();
    const auto corrupt_assets = temporary.filePath(QStringLiteral("corrupt-assets"));
    const auto corrupt =
        ui::LocalSessionProvider::create({corrupt_database, corrupt_assets}, clock);
    QVERIFY(!corrupt.has_value());
    QVERIFY(!QFileInfo::exists(corrupt_assets));
    QVERIFY(!QFileInfo::exists(corrupt_database + QStringLiteral("-wal")));
    QVERIFY(!QFileInfo::exists(corrupt_database + QStringLiteral("-shm")));
    QFile corrupt_after_file(corrupt_database);
    QVERIFY(corrupt_after_file.open(QIODevice::ReadOnly));
    QCOMPARE(corrupt_after_file.readAll(), corrupt_bytes);

    const auto newer_database = temporary.filePath(QStringLiteral("newer.sqlite"));
    QVERIFY(executeSql(newer_database, QStringLiteral("CREATE TABLE schema_migrations ("
                                                      "version INTEGER PRIMARY KEY, "
                                                      "applied_at_utc TEXT NOT NULL) STRICT"))
                .has_value());
    QVERIFY(executeSql(newer_database,
                       QStringLiteral("INSERT INTO schema_migrations VALUES(3, 'future')"))
                .has_value());
    QVERIFY(executeSql(newer_database, QStringLiteral("CREATE TABLE future_sentinel ("
                                                      "value TEXT NOT NULL) STRICT"))
                .has_value());
    QVERIFY(executeSql(newer_database,
                       QStringLiteral("INSERT INTO future_sentinel VALUES('untouched')"))
                .has_value());
    QFile newer_before_file(newer_database);
    QVERIFY(newer_before_file.open(QIODevice::ReadOnly));
    const auto newer_before = newer_before_file.readAll();
    newer_before_file.close();
    const auto newer_assets = temporary.filePath(QStringLiteral("newer-assets"));
    const auto newer = ui::LocalSessionProvider::create({newer_database, newer_assets}, clock);
    QVERIFY(!newer.has_value());
    QVERIFY(!QFileInfo::exists(newer_assets));
    QVERIFY(!QFileInfo::exists(newer_database + QStringLiteral("-wal")));
    QVERIFY(!QFileInfo::exists(newer_database + QStringLiteral("-shm")));
    QFile newer_after_file(newer_database);
    QVERIFY(newer_after_file.open(QIODevice::ReadOnly));
    QCOMPARE(newer_after_file.readAll(), newer_before);
}

void MainWindowLocalSessionsTest::independentProviderFailsPromptlyAndLaterReopens() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const ui::LocalSessionPaths paths{
        temporary.filePath(QStringLiteral("state/sessions.sqlite")),
        temporary.filePath(QStringLiteral("state/assets")),
    };
    const auto clock = [] {
        return QDateTime::fromString(QStringLiteral("2026-08-11T10:00:00Z"), Qt::ISODate);
    };
    auto owner = ui::LocalSessionProvider::create(paths, clock);
    QVERIFY2(owner.has_value(), owner ? "" : qPrintable(owner.error()));

    QElapsedTimer elapsed;
    elapsed.start();
    const auto contender = ui::LocalSessionProvider::create(paths, clock);
    const auto elapsed_milliseconds = elapsed.elapsed();
    QVERIFY(!contender.has_value());
    QVERIFY2(contender.error().contains(QStringLiteral("in use"), Qt::CaseInsensitive),
             qPrintable(contender.error()));
    QVERIFY2(
        elapsed_milliseconds < 1'000,
        qPrintable(
            QStringLiteral("contending provider blocked for %1 ms").arg(elapsed_milliseconds)));

    owner->reset();
    const auto reopened = ui::LocalSessionProvider::create(paths, clock);
    QVERIFY2(reopened.has_value(), reopened ? "" : qPrintable(reopened.error()));
}

void MainWindowLocalSessionsTest::pinAndEngineMismatchFailWithoutDatabaseOrCasMutation() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto archive_path = temporary.filePath(QStringLiteral("grounded.awpack"));
    const auto exported =
        packs::PackArchive::exportDirectory(fixture(u"full-resource-pack-v2"), archive_path);
    QVERIFY2(exported.has_value(), exported ? "" : qPrintable(exported.error().message));
    const auto catalog = packs::PackCatalog::open(temporary.filePath(QStringLiteral("catalog")));
    QVERIFY2(catalog.has_value(), catalog ? "" : qPrintable(catalog.error().message));
    const auto installed =
        (*catalog)->installArchive(archive_path, QStringLiteral("2026-08-11T00:00:00Z"));
    QVERIFY2(installed.has_value(), installed ? "" : qPrintable(installed.error().message));
    const auto resolved = (*catalog)->loadResolved(installed->revision);
    QVERIFY2(resolved.has_value(), resolved ? "" : qPrintable(resolved.error().message));
    const auto runtime = packs::loadRuntimePack(*resolved);
    QVERIFY2(runtime.has_value(), runtime ? "" : runtime.error().message.c_str());
    const auto& runtime_case = runtime->cases.front();
    const auto argument_id = runtime_case.argument_configurations.front().id;
    const ui::LocalSessionPaths paths{
        temporary.filePath(QStringLiteral("state/sessions.sqlite")),
        temporary.filePath(QStringLiteral("state/assets")),
    };
    const auto clock = [] {
        return QDateTime::fromString(QStringLiteral("2026-08-11T10:00:00Z"), Qt::ISODate);
    };
    const auto provider = ui::LocalSessionProvider::create(paths, clock);
    QVERIFY2(provider.has_value(), provider ? "" : qPrintable(provider.error()));
    auto workflow = (*provider)->openWorkflow(*resolved, runtime_case.definition.id);
    QVERIFY2(workflow.has_value(), workflow ? "" : qPrintable(workflow.error().message));
    const auto workflow_id = QString::fromStdString((*workflow)->state().session_id);
    const QByteArray document("adverse replay CAS sentinel");
    const auto filing = model::SubmitWorkflowFiling{
        model::WorkflowCommandHeader{
            workflow_id.toStdString(),
            model::WorkflowCommandId{workflow_id.toStdString() +
                                     ".command.1.example.operation.accept-notice"},
            model::ActorId{"example.actor.appellant"},
            at(2026, 8U, 11U, 11),
        },
        model::WorkflowFilingId{"example.filing.adverse-replay"},
        model::FilingTypeId{"example.filing.notice"},
        sha256(document).toStdString(),
        {model::WorkflowFieldValue{model::FilingFieldId{"example.field.caption"},
                                   "Adverse replay caption"}},
        {model::ActorId{"example.actor.appellee"}},
        std::nullopt,
    };
    const auto filed = (*workflow)->submit(model::WorkflowCommand{filing}, QByteArrayView(document),
                                           QStringLiteral("2026-08-11T11:00:00Z"));
    QVERIFY2(filed.has_value(), filed ? "" : qPrintable(filed.error().message));
    QVERIFY(filed->asset.has_value());
    const auto asset_digest = filed->asset->sha256;
    (*workflow).reset();

    auto oral = (*provider)->open(*resolved, runtime_case.definition.id, argument_id);
    QVERIFY2(oral.has_value(), oral ? "" : qPrintable(oral.error().message));
    const auto oral_id = (*oral)->sessionId();
    (*oral).reset();

    const auto changed_oral =
        executeSql(paths.database_path,
                   QStringLiteral("UPDATE sessions SET engine_revision=? WHERE session_id=?"),
                   {QStringLiteral("engine.oral.future.v999"), oral_id});
    QVERIFY2(changed_oral.has_value(), changed_oral ? "" : qPrintable(changed_oral.error()));
    const auto oral_rows_before = databaseRows(paths.database_path, oral_id);
    const auto workflow_rows_before_oral_failure = databaseRows(paths.database_path, workflow_id);
    QVERIFY2(oral_rows_before.has_value(),
             oral_rows_before ? "" : qPrintable(oral_rows_before.error()));
    QVERIFY2(workflow_rows_before_oral_failure.has_value(),
             workflow_rows_before_oral_failure
                 ? ""
                 : qPrintable(workflow_rows_before_oral_failure.error()));
    const auto rejected_oral =
        (*provider)->open(*resolved, runtime_case.definition.id, argument_id);
    QVERIFY(!rejected_oral.has_value());
    const auto oral_rows_after = databaseRows(paths.database_path, oral_id);
    const auto workflow_rows_after_oral_failure = databaseRows(paths.database_path, workflow_id);
    QVERIFY(oral_rows_after.has_value());
    QVERIFY(workflow_rows_after_oral_failure.has_value());
    QCOMPARE(*oral_rows_after, *oral_rows_before);
    QCOMPARE(*workflow_rows_after_oral_failure, *workflow_rows_before_oral_failure);
    const auto asset_after_oral_failure = storage::AssetStore(paths.asset_root).read(asset_digest);
    QVERIFY2(asset_after_oral_failure.has_value(),
             asset_after_oral_failure ? "" : qPrintable(asset_after_oral_failure.error().message));
    QCOMPARE(*asset_after_oral_failure, document);

    const auto changed_pin = executeSql(
        paths.database_path, QStringLiteral("UPDATE session_pins SET digest=? WHERE session_id=?"),
        {QString(64, u'c'), workflow_id});
    QVERIFY2(changed_pin.has_value(), changed_pin ? "" : qPrintable(changed_pin.error()));
    const auto workflow_rows_before_pin_failure = databaseRows(paths.database_path, workflow_id);
    QVERIFY(workflow_rows_before_pin_failure.has_value());
    const auto rejected_workflow = (*provider)->openWorkflow(*resolved, runtime_case.definition.id);
    QVERIFY(!rejected_workflow.has_value());
    const auto workflow_rows_after_pin_failure = databaseRows(paths.database_path, workflow_id);
    QVERIFY(workflow_rows_after_pin_failure.has_value());
    QCOMPARE(*workflow_rows_after_pin_failure, *workflow_rows_before_pin_failure);
    const auto asset_after_pin_failure = storage::AssetStore(paths.asset_root).read(asset_digest);
    QVERIFY2(asset_after_pin_failure.has_value(),
             asset_after_pin_failure ? "" : qPrintable(asset_after_pin_failure.error().message));
    QCOMPARE(*asset_after_pin_failure, document);
}

} // namespace

QTEST_MAIN(MainWindowLocalSessionsTest)

#include "tst_main_window_local_sessions.moc"
