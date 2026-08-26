#include "independent_review_publisher_p.hpp"
#include "pack_cli.hpp"
#include "pack_cli_p.hpp"

#include "../../src/packs/src/pack_catalog_p.hpp"

#include "appellate/engine/workflow_engine.hpp"
#include "appellate/packs/pack_archive.hpp"
#include "appellate/packs/pack_catalog.hpp"
#include "appellate/packs/pack_reader.hpp"
#include "appellate/packs/realism_evidence_authoring.hpp"
#include "appellate/packs/runtime_pack.hpp"
#include "appellate/packs/schema_validator.hpp"
#include "appellate/storage/workflow_codec.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#if defined(Q_OS_UNIX)
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

namespace {

using appellate::cli::ExitCode;
using appellate::cli::runPackCli;
using appellate::cli::RunResult;

class PackCliTest final : public QObject {
    Q_OBJECT

  private slots:
    void completePackLifecycle();
    void authorsRealismEvidenceDeterministically();
    void authorsMultiTraceRealismEvidenceDeterministically();
    void preparesIndependentReviewDeterministically();
    void finalizesInterchangeableIndependentReviewHandoffs();
    void finalizesIndependentReviewDeterministically();
    void enforcesIndependentReviewBoundaries_data();
    void enforcesIndependentReviewBoundaries();
    void mapsCatalogBusyWithoutMutatingTheLock();
    void rejectsInvalidArgumentsAndExistingTemplateDestination();
};

[[nodiscard]] QJsonObject responseObject(const QByteArray& bytes) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return {};
    }
    return document.object();
}

void requireSuccess(const RunResult& result, const QString& command) {
    QCOMPARE(result.exit_code, static_cast<int>(ExitCode::Success));
    QVERIFY2(result.standard_error.isEmpty(), result.standard_error.constData());
    QVERIFY(result.standard_output.endsWith('\n'));
    const auto response = responseObject(result.standard_output);
    QCOMPARE(response.value(QStringLiteral("schema_version")).toInt(), 1);
    QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    QCOMPARE(response.value(QStringLiteral("command")).toString(), command);
}

void requireCommandError(const RunResult& result, ExitCode exit_code, const QString& code,
                         const QString& command) {
    QCOMPARE(result.exit_code, static_cast<int>(exit_code));
    QVERIFY(result.standard_output.isEmpty());
    QVERIFY(result.standard_error.endsWith('\n'));
    const auto response = responseObject(result.standard_error);
    QCOMPARE(
        response.keys(),
        QStringList({QStringLiteral("code"), QStringLiteral("command"), QStringLiteral("message"),
                     QStringLiteral("schema_version"), QStringLiteral("status")}));
    QCOMPARE(response.value(QStringLiteral("code")).toString(), code);
    QCOMPARE(response.value(QStringLiteral("command")).toString(), command);
    QVERIFY(!response.value(QStringLiteral("message")).toString().isEmpty());
    QCOMPARE(response.value(QStringLiteral("schema_version")).toInt(), 1);
    QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
    QCOMPARE(result.standard_error, QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
}

[[nodiscard]] QByteArray readAll(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

#if defined(Q_OS_LINUX)
void requireNativeMetadata(const QString& path, mode_t type, mode_t mode, nlink_t link_count) {
    struct stat status{};
    QCOMPARE(::lstat(QFile::encodeName(path).constData(), &status), 0);
    QCOMPARE(status.st_mode & S_IFMT, type);
    QCOMPARE(status.st_mode & 07777, mode);
    QCOMPARE(status.st_uid, ::geteuid());
    QCOMPARE(status.st_nlink, link_count);
}
#endif

[[nodiscard]] bool overwriteAll(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(bytes) == bytes.size();
}

[[nodiscard]] QByteArray jsonBytes(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Indented);
}

[[nodiscard]] QByteArray nestedJsonObject(qsizetype nested_edges) {
    QByteArray bytes("{}");
    for (qsizetype index = 0; index < nested_edges; ++index) {
        bytes.prepend("{\"nested\":");
        bytes.append('}');
    }
    return bytes;
}

[[nodiscard]] QByteArray jsonObjectWithValueCount(qsizetype value_count) {
    if (value_count < 2) {
        return {};
    }
    QByteArray bytes("{\"values\":[");
    bytes.reserve(12 + (value_count - 2) * 5);
    for (qsizetype index = 0; index < value_count - 2; ++index) {
        if (index != 0) {
            bytes.append(',');
        }
        bytes.append("null");
    }
    bytes.append("]}");
    return bytes;
}

#if defined(Q_OS_LINUX)
[[nodiscard]] QByteArray paddedJsonObject(qsizetype byte_count) {
    QByteArray bytes("{}");
    if (byte_count >= bytes.size()) {
        bytes.append(QByteArray(byte_count - bytes.size(), ' '));
    }
    return bytes;
}

[[nodiscard]] QByteArray paddedJsonBytes(QByteArray bytes, qsizetype byte_count) {
    if (byte_count >= bytes.size()) {
        bytes.append(QByteArray(byte_count - bytes.size(), ' '));
    }
    return bytes;
}

class ScopedTimeZone final {
  public:
    ScopedTimeZone() : was_set_(qEnvironmentVariableIsSet("TZ")), original_(qgetenv("TZ")) {}

    ~ScopedTimeZone() {
        if (was_set_) {
            qputenv("TZ", original_);
        } else {
            qunsetenv("TZ");
        }
        ::tzset();
    }

    void set(const QByteArray& value) {
        qputenv("TZ", value);
        ::tzset();
    }

  private:
    bool was_set_{};
    QByteArray original_;
};
#endif

[[nodiscard]] QString sha256(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

void addUint64(QCryptographicHash& hash, std::uint64_t value) {
    std::array<char, 8> bytes{};
    for (int index = 7; index >= 0; --index) {
        bytes[static_cast<std::size_t>(index)] = static_cast<char>(value & 0xffU);
        value >>= 8U;
    }
    hash.addData(QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())));
}

void addFrame(QCryptographicHash& hash, const QString& value) {
    const auto bytes = value.toUtf8();
    addUint64(hash, static_cast<std::uint64_t>(bytes.size()));
    hash.addData(QByteArrayView(bytes));
}

void addFrame(QCryptographicHash& hash, QByteArrayView value) {
    addUint64(hash, static_cast<std::uint64_t>(value.size()));
    hash.addData(value);
}

[[nodiscard]] QString independentHandoffDigest(const QJsonObject& payload) {
    const auto compact = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, QStringLiteral("appellate-workbench-independent-realism-review-handoff-v1"));
    addUint64(hash, 1);
    addFrame(hash, QStringLiteral("independent_realism_review"));
    addFrame(hash, QByteArrayView(compact));
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] QString independentTraceDigest(const QString& case_id, const QJsonObject& trace) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, QStringLiteral("appellate-workbench-executed-trace-evidence-v1"));
    addFrame(hash, case_id);
    addFrame(hash, trace.value(QStringLiteral("evidence_id")).toString());
    addFrame(hash, trace.value(QStringLiteral("trace_id")).toString());
    addFrame(hash, trace.value(QStringLiteral("workflow_id")).toString());
    addFrame(hash, trace.value(QStringLiteral("engine_revision")).toString());
    addUint64(hash,
              static_cast<std::uint64_t>(trace.value(QStringLiteral("command_count")).toInt()));
    addUint64(hash, static_cast<std::uint64_t>(trace.value(QStringLiteral("event_count")).toInt()));
    addFrame(hash, trace.value(QStringLiteral("journal_sha256")).toString());
    const auto operation_ids = trace.value(QStringLiteral("operation_ids")).toArray();
    addUint64(hash, static_cast<std::uint64_t>(operation_ids.size()));
    for (const auto& operation_id : operation_ids) {
        addFrame(hash, operation_id.toString());
    }
    addFrame(hash, trace.value(QStringLiteral("terminal_stage_id")).toString());
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] bool writeNew(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
           file.write(bytes) == bytes.size() && file.flush();
}

[[nodiscard]] bool copyTree(const QString& source, const QString& destination) {
    const QDir source_directory(source);
    if (!source_directory.exists() || !QDir{}.mkpath(destination)) {
        return false;
    }
    QDirIterator iterator(source, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const auto source_path = iterator.next();
        const auto destination_path =
            QDir(destination).filePath(source_directory.relativeFilePath(source_path));
        if (!QDir{}.mkpath(QFileInfo(destination_path).absolutePath()) ||
            !QFile::copy(source_path, destination_path)) {
            return false;
        }
    }
    return true;
}

void addResourceDescriptor(QCryptographicHash& hash, const QJsonObject& resource) {
    addFrame(hash, resource.value(QStringLiteral("owner_pack_id")).toString());
    addFrame(hash, resource.value(QStringLiteral("owner_pack_version")).toString());
    addFrame(hash, resource.value(QStringLiteral("resource_id")).toString());
    addFrame(hash, resource.value(QStringLiteral("resource_kind")).toString());
    addUint64(hash,
              static_cast<std::uint64_t>(resource.value(QStringLiteral("schema_version")).toInt()));
    addFrame(hash, resource.value(QStringLiteral("path")).toString());
    addFrame(hash, resource.value(QStringLiteral("sha256")).toString());
}

[[nodiscard]] QString recomputedClosureDigest(const QJsonObject& review) {
    const auto evidence = review.value(QStringLiteral("evidence")).toObject();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, QStringLiteral("appellate-workbench-case-evidence-closure-v1"));
    addFrame(hash, review.value(QStringLiteral("case_id")).toString());

    const auto packs = evidence.value(QStringLiteral("packs")).toArray();
    addUint64(hash, static_cast<std::uint64_t>(packs.size()));
    for (const auto& value : packs) {
        const auto pack = value.toObject();
        addFrame(hash, pack.value(QStringLiteral("pack_id")).toString());
        addFrame(hash, pack.value(QStringLiteral("version")).toString());
        addUint64(hash, static_cast<std::uint64_t>(
                            pack.value(QStringLiteral("manifest_schema_version")).toInt()));
        const auto capabilities = pack.value(QStringLiteral("required_capabilities")).toArray();
        addUint64(hash, static_cast<std::uint64_t>(capabilities.size()));
        for (const auto& capability_value : capabilities) {
            const auto capability = capability_value.toObject();
            addFrame(hash, capability.value(QStringLiteral("id")).toString());
            addUint64(hash, static_cast<std::uint64_t>(
                                capability.value(QStringLiteral("version")).toInt()));
        }
        const auto dependencies = pack.value(QStringLiteral("dependencies")).toArray();
        addUint64(hash, static_cast<std::uint64_t>(dependencies.size()));
        for (const auto& dependency_value : dependencies) {
            const auto dependency = dependency_value.toObject();
            addFrame(hash, dependency.value(QStringLiteral("pack_id")).toString());
            addFrame(hash, dependency.value(QStringLiteral("version")).toString());
        }
    }

    const auto resources = evidence.value(QStringLiteral("resources")).toArray();
    addUint64(hash, static_cast<std::uint64_t>(resources.size()));
    for (const auto& value : resources) {
        const auto resource = value.toObject();
        addFrame(hash, resource.value(QStringLiteral("evidence_id")).toString());
        addResourceDescriptor(hash, resource);
    }

    const auto blobs = evidence.value(QStringLiteral("blobs")).toArray();
    addUint64(hash, static_cast<std::uint64_t>(blobs.size()));
    for (const auto& value : blobs) {
        const auto blob = value.toObject();
        addFrame(hash, blob.value(QStringLiteral("evidence_id")).toString());
        addFrame(hash, blob.value(QStringLiteral("owner_pack_id")).toString());
        addFrame(hash, blob.value(QStringLiteral("owner_pack_version")).toString());
        addFrame(hash, blob.value(QStringLiteral("path")).toString());
        addFrame(hash, blob.value(QStringLiteral("media_type")).toString());
        addUint64(hash,
                  static_cast<std::uint64_t>(blob.value(QStringLiteral("byte_size")).toDouble()));
        addFrame(hash, blob.value(QStringLiteral("sha256")).toString());
    }
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] QString recomputedPageAnchorCheckDigest(const QJsonObject& review,
                                                      const QJsonObject& check) {
    QJsonObject record;
    const auto resources = review.value(QStringLiteral("evidence"))
                               .toObject()
                               .value(QStringLiteral("resources"))
                               .toArray();
    for (const auto& value : resources) {
        if (value.toObject().value(QStringLiteral("resource_id")) ==
            check.value(QStringLiteral("record_id"))) {
            record = value.toObject();
            break;
        }
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, QStringLiteral("appellate-workbench-record-check-evidence-v1"));
    addFrame(hash, review.value(QStringLiteral("case_id")).toString());
    addFrame(hash, check.value(QStringLiteral("evidence_id")).toString());
    addFrame(hash, check.value(QStringLiteral("check_id")).toString());
    addFrame(hash, check.value(QStringLiteral("record_id")).toString());
    addFrame(hash, check.value(QStringLiteral("check_kind")).toString());
    addResourceDescriptor(hash, record);
    addUint64(hash, 0);
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] const appellate::model::WorkflowEventHeader&
eventHeader(const appellate::model::WorkflowEvent& event) {
    return std::visit(
        [](const auto& concrete) -> const appellate::model::WorkflowEventHeader& {
            return concrete.header;
        },
        event);
}

[[nodiscard]] std::optional<QJsonObject>
executedTrace(const appellate::packs::RuntimeCase& runtime_case, const QString& suffix = {}) {
    const auto suffixed = [&](QLatin1StringView base) {
        return suffix.isEmpty() ? QString{base} : QString{base} + u'.' + suffix;
    };
    const auto session_id = suffixed(QLatin1StringView("example.session.cli-realism"));
    const auto command_id = suffixed(QLatin1StringView("example.command.cli-realism"));
    const auto filing_instance_id =
        suffixed(QLatin1StringView("example.filing-instance.cli-realism"));
    const auto court_date = appellate::model::LegalDate{
        std::chrono::year{2026} / std::chrono::month{1} / std::chrono::day{4}};
    const appellate::model::WorkflowState initial_state{
        session_id.toStdString(),
        runtime_case.workflow.id,
        runtime_case.workflow.initial_stage_id,
        std::uint64_t{1},
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
    const appellate::model::WorkflowCommand command = appellate::model::SubmitWorkflowFiling{
        appellate::model::WorkflowCommandHeader{
            initial_state.session_id,
            appellate::model::WorkflowCommandId{command_id.toStdString()},
            appellate::model::ActorId{"example.actor.appellant"},
            appellate::model::LegalTime{
                std::chrono::sys_seconds{std::chrono::sys_days{court_date.value}}, court_date},
        },
        appellate::model::WorkflowFilingId{filing_instance_id.toStdString()},
        appellate::model::FilingTypeId{"example.filing.notice"},
        std::string(64, 'a'),
        {{appellate::model::FilingFieldId{"example.field.caption"}, "Example caption"}},
        {appellate::model::ActorId{"example.actor.appellee"}},
        std::nullopt,
    };
    const auto events = appellate::engine::decideWorkflow(
        runtime_case.workflow, runtime_case.definition, initial_state, command);
    const auto command_bytes = appellate::storage::encodeWorkflowCommand(command);
    if (!events || events->empty() || !command_bytes) {
        return std::nullopt;
    }
    QJsonArray encoded_events;
    for (const auto& event : *events) {
        const auto bytes = appellate::storage::encodeWorkflowEvent(event);
        if (!bytes || eventHeader(event).operation_id.value.empty()) {
            return std::nullopt;
        }
        encoded_events.push_back(QString::fromLatin1(bytes->toBase64()));
    }
    return QJsonObject{
        {QStringLiteral("evidence_id"),
         suffixed(QLatin1StringView("example.evidence.trace.cli-realism"))},
        {QStringLiteral("trace_id"), suffixed(QLatin1StringView("example.trace.cli-realism"))},
        {QStringLiteral("workflow_id"), QString::fromStdString(runtime_case.workflow.id.value)},
        {QStringLiteral("journal"),
         QJsonArray{QJsonObject{
             {QStringLiteral("command_base64"), QString::fromLatin1(command_bytes->toBase64())},
             {QStringLiteral("events_base64"), encoded_events},
         }}},
    };
}

[[nodiscard]] std::optional<QJsonObject> executedTrace(const appellate::packs::LoadedPack& pack,
                                                       const QString& suffix = {}) {
    const auto runtime = appellate::packs::loadRuntimePack(pack);
    if (!runtime || runtime->cases.empty()) {
        return std::nullopt;
    }
    return executedTrace(runtime->cases.front(), suffix);
}

[[nodiscard]] std::optional<QJsonArray> executedTraces(const appellate::packs::LoadedPack& pack,
                                                       qsizetype count) {
    const auto runtime = appellate::packs::loadRuntimePack(pack);
    if (!runtime || runtime->cases.empty() || count < 0) {
        return std::nullopt;
    }
    QJsonArray traces;
    for (qsizetype index = 0; index < count; ++index) {
        const auto trace =
            executedTrace(runtime->cases.front(), QStringLiteral("n") + QString::number(index));
        if (!trace) {
            return std::nullopt;
        }
        traces.push_back(*trace);
    }
    return traces;
}

[[nodiscard]] bool attachRealismScaffold(const QString& pack_directory, const QJsonObject& review) {
    const auto review_path =
        QDir(pack_directory).filePath(QStringLiteral("resources/realism-review.json"));
    const auto review_bytes = jsonBytes(review);
    if (!overwriteAll(review_path, review_bytes)) {
        QFile review_file(review_path);
        if (!review_file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
            review_file.write(review_bytes) != review_bytes.size()) {
            return false;
        }
    }
    const auto manifest_path = QDir(pack_directory).filePath(QStringLiteral("manifest.json"));
    auto manifest = responseObject(readAll(manifest_path));
    if (manifest.isEmpty()) {
        return false;
    }
    auto capabilities = manifest.value(QStringLiteral("required_capabilities")).toArray();
    capabilities.push_back(QJsonObject{
        {QStringLiteral("id"), QStringLiteral("workbench.pack.realism-evidence")},
        {QStringLiteral("version"), 1},
    });
    manifest.insert(QStringLiteral("required_capabilities"), capabilities);
    auto contents = manifest.value(QStringLiteral("contents")).toArray();
    contents.push_back(QJsonObject{
        {QStringLiteral("id"), review.value(QStringLiteral("resource_id"))},
        {QStringLiteral("kind"), QStringLiteral("realism_review")},
        {QStringLiteral("schema_version"), 2},
        {QStringLiteral("path"), QStringLiteral("resources/realism-review.json")},
        {QStringLiteral("sha256"), sha256(review_bytes)},
    });
    manifest.insert(QStringLiteral("contents"), contents);
    return overwriteAll(manifest_path, jsonBytes(manifest));
}

[[nodiscard]] bool replaceResourceAndDigest(const QString& pack_directory,
                                            const QString& relative_path,
                                            const QString& resource_id,
                                            const QJsonObject& document) {
    const auto bytes = jsonBytes(document);
    if (!overwriteAll(QDir(pack_directory).filePath(relative_path), bytes)) {
        return false;
    }
    const auto manifest_path = QDir(pack_directory).filePath(QStringLiteral("manifest.json"));
    auto manifest = responseObject(readAll(manifest_path));
    auto contents = manifest.value(QStringLiteral("contents")).toArray();
    qsizetype matches = 0;
    for (qsizetype index = 0; index < contents.size(); ++index) {
        auto descriptor = contents.at(index).toObject();
        if (descriptor.value(QStringLiteral("id")).toString() != resource_id) {
            continue;
        }
        ++matches;
        descriptor.insert(QStringLiteral("sha256"), sha256(bytes));
        contents.replace(index, descriptor);
    }
    if (matches != 1) {
        return false;
    }
    manifest.insert(QStringLiteral("contents"), contents);
    return overwriteAll(manifest_path, jsonBytes(manifest));
}

struct IndependentReviewFixture final {
    const char* slug;
    const char* archive_relative_path;
    const char* archive_sha256;
    const char* pack_id;
    const char* version;
    const char* revision;
    const char* case_id;
    const char* review_id;
    const char* reviewed_on;
    int trace_count;
};

constexpr std::array independent_review_fixtures{
    IndependentReviewFixture{
        "asterglen",
        "content/ca4-rule54b/us-ca4-rule54b-asterglen-0.2.0.awpack",
        "10739c149a3bf2617d8af6dd131caee7ea6639a9d97e26cdf2974fa176c82819",
        "us.ca4.rule54b.asterglen",
        "0.2.0",
        "7e77bc0fbe02dc9e108681df73852859d6d0f577acdcb65fcfb7678eac78b728",
        "ca4r54b.case.asterglen",
        "ca4r54b.review.authoring-2026-08-12",
        "2026-08-12",
        8,
    },
    IndependentReviewFixture{
        "cinderlake",
        "content/m4/cinderlake-writ/us-ca4-m4-cinderlake-writ-1.2.0.awpack",
        "eeefbbbe84cf4addbf91a68447281217226c6a08c7e0e3e1294947d5e5dc8956",
        "us.ca4.m4.cinderlake-writ",
        "1.2.0",
        "020517571a6c15f90765e12b94ab53d8598be3bc3081d47caecdf5950bacd05c",
        "ca4m4.case.cinderlake-writ",
        "ca4m4.cinder.review.authoring-2026-08-19",
        "2026-08-19",
        3,
    },
    IndependentReviewFixture{
        "arm-agency",
        "content/m4/arm-agency/us-ca4-m4-arm-agency-1.2.0.awpack",
        "a150903c6c3332d8de582a8ef46e7fd1dd17cee0ac52c93c0ebaf51313cf54d2",
        "us.ca4.m4.arm-agency",
        "1.2.0",
        "ae33933c7cf18f77e662eb302d563afd860e8e900bac8debb081b81b35404edb",
        "ca4m4.case.arm-agency",
        "ca4m4.arm.review.authoring-2026-08-12",
        "2026-08-12",
        7,
    },
    IndependentReviewFixture{
        "benton-retaliation",
        "content/m4/benton-retaliation/us-ca4-m4-benton-retaliation-1.2.0.awpack",
        "9515bdde1e3405e6e82488abd73314a31c33a2062f9e34b4cecdaaff8b634a05",
        "us.ca4.m4.benton-retaliation",
        "1.2.0",
        "59467350af5f381ef429ecf210d38de5503d40fb2e9baf02f56b2ef5023ced28",
        "ca4m4.case.benton-retaliation",
        "ca4m4.benton.review.authoring-2026-08-12",
        "2026-08-12",
        7,
    },
    IndependentReviewFixture{
        "norvale-injunction",
        "content/m4/norvale-injunction/us-ca4-m4-norvale-injunction-1.2.0.awpack",
        "a4b993aa3cc6582d1d0f6ca9a7203109378f4f1c1b2e6ce32efbfe82b6a48e19",
        "us.ca4.m4.norvale-injunction",
        "1.2.0",
        "a51383c0c1edcd56153b36291177425b09846ab607c39c28030820ef700df05f",
        "ca4m4.case.norvale-injunction",
        "ca4m4.norvale.review.authoring-2026-08-12",
        "2026-08-12",
        9,
    },
    IndependentReviewFixture{
        "ellison-immunity",
        "content/m4/ellison-immunity/us-ca4-m4-ellison-immunity-1.2.0.awpack",
        "59f32f521644bac61865cf1e59444fc98dbb9007461a1709272ffe261cbad1d0",
        "us.ca4.m4.ellison-immunity",
        "1.2.0",
        "c2a4f3bc07f05eb1429257320ed839ebaea837da7aa7330f4669bbb157168ce0",
        "ca4m4.case.ellison-immunity",
        "ca4m4.ellison.review.authoring-2026-08-12",
        "2026-08-12",
        6,
    },
    IndependentReviewFixture{
        "blueember-jmol",
        "content/m4/blueember-jmol/us-ca4-m4-blueember-jmol-1.2.0.awpack",
        "c6332ae33e351ccb27ed17b5576b147a47f9f5f0b44583365212b1781a288ed2",
        "us.ca4.m4.blueember-jmol",
        "1.2.0",
        "08d88e4811e8ed8ad6e642cc041365508808f7158862aa93199de867f31431ec",
        "ca4m4.case.blueember-jmol",
        "ca4m4.blueember.review.authoring-2026-08-19",
        "2026-08-19",
        6,
    },
    IndependentReviewFixture{
        "opengrid-foia",
        "content/m4/opengrid-foia/us-ca4-m4-opengrid-foia-1.2.0.awpack",
        "1efa067767f3c729bbd67c40b3faa239673025f421133bddf32ec6b090231b09",
        "us.ca4.m4.opengrid-foia",
        "1.2.0",
        "9cb2879b1cc27e98d8def7c926a38e9f4eb2cbec90785be74c009156b4a1e4c5",
        "ca4m4.case.opengrid-foia",
        "ca4m4.opengrid.review.authoring-2026-08-19",
        "2026-08-19",
        5,
    },
    IndependentReviewFixture{
        "serrano-waiver",
        "content/m4/serrano-waiver/us-ca4-m4-serrano-waiver-1.2.0.awpack",
        "d76686cec2053f78334c73f1c3aac415b637e733f0494b527001368597a1c243",
        "us.ca4.m4.serrano-waiver",
        "1.2.0",
        "9b4941e97292faa0fceda1f1c719f6e38ce8478c82350c7fbbb74a010c27d344",
        "ca4m4.case.serrano-waiver",
        "ca4m4.serrano.review.authoring-2026-08-19",
        "2026-08-19",
        2,
    },
};

[[nodiscard]] QString sourcePath(const char* relative_path) {
    return QDir(QStringLiteral(APPELLATE_SOURCE_DIR)).filePath(QString::fromLatin1(relative_path));
}

struct IndependentReviewDependencyFixture final {
    const char* archive_relative_path;
    const char* pack_id;
    const char* version;
    const char* revision;
};

constexpr std::array independent_review_dependency_fixtures{
    IndependentReviewDependencyFixture{
        "content/foundations/us-federal/foundation-us-federal-2025.12.01.awpack",
        "foundation.us-federal",
        "2025.12.01",
        "866c90996c15e2076b9508a297ffce1a4e766b1432a9e11d08e8138c57e363c9",
    },
    IndependentReviewDependencyFixture{
        "content/foundations/us-ca4/foundation-us-ca4-2026.03.23.awpack",
        "foundation.us-ca4",
        "2026.03.23",
        "449d75c77e5c47883f750377450f2d1ec1fc0e42e20b1f247446b208661d3262",
    },
    IndependentReviewDependencyFixture{
        "content/foundations/us-ca4-fictional-bench/"
        "foundation-us-ca4-fictional-bench-1.0.0.awpack",
        "foundation.us-ca4-fictional-bench",
        "1.0.0",
        "cee0bf93309cc9ad800f215a47d734b20a9fdf5dc889f2f440e4382b942d332d",
    },
};

[[nodiscard]] std::array<QString, 3> independentReviewDependencyArchives() {
    std::array<QString, 3> archives;
    std::ranges::transform(
        independent_review_dependency_fixtures, archives.begin(),
        [](const auto& fixture) { return sourcePath(fixture.archive_relative_path); });
    return archives;
}

[[nodiscard]] QJsonObject
completedIndependentDeclaration(const appellate::packs::PreparedIndependentReview& prepared,
                                const QString& slug, const QString& resource_id = {},
                                const QJsonValue& affiliation = QJsonValue::Null) {
    auto declaration = prepared.declaration_template;
    QJsonObject dimensions;
    for (const auto& name :
         {QStringLiteral("procedural_law"), QStringLiteral("deadlines_authority"),
          QStringLiteral("record_consistency"), QStringLiteral("consequences"),
          QStringLiteral("oral_argument"), QStringLiteral("bench_differentiation"),
          QStringLiteral("provenance")}) {
        dimensions.insert(name, 2);
    }
    declaration.insert(QStringLiteral("dimensions"), dimensions);
    declaration.insert(QStringLiteral("handoff_digest"), prepared.handoff_digest);
    declaration.insert(QStringLiteral("known_uncertainty"), QJsonArray{});
    declaration.insert(QStringLiteral("review_pack_id"),
                       QStringLiteral("test.detached-review.%1").arg(slug));
    declaration.insert(QStringLiteral("review_pack_version"), QStringLiteral("2026.08.20"));
    declaration.insert(QStringLiteral("review_resource_id"),
                       resource_id.isEmpty()
                           ? QStringLiteral("test.detached-review.resource.%1").arg(slug)
                           : resource_id);
    declaration.insert(QStringLiteral("review_state"), QStringLiteral("independently_reviewed"));
    declaration.insert(QStringLiteral("reviewed_on"), QStringLiteral("2026-08-20"));
    declaration.insert(
        QStringLiteral("reviewer"),
        QJsonObject{
            {QStringLiteral("affiliation"), affiliation},
            {QStringLiteral("display_name"), QStringLiteral("TEST-ONLY synthetic reviewer")},
            {QStringLiteral("qualification"),
             QStringLiteral("TEST-ONLY fixture; no human independent review was performed")},
            {QStringLiteral("reviewer_id"), QStringLiteral("test.detached-review.reviewer")},
        });
    declaration.insert(QStringLiteral("reviewer_reference"),
                       QStringLiteral("TEST-ONLY deterministic builder fixture"));
    return declaration;
}

[[nodiscard]] const appellate::packs::ValidatedResource*
findReview(const appellate::packs::LoadedPack& pack, const QString& resource_id) {
    const auto found = std::ranges::find_if(pack.resources, [&](const auto& resource) {
        return resource.descriptor.kind == appellate::model::ResourceKind::RealismReview &&
               QString::fromStdString(resource.descriptor.id) == resource_id;
    });
    return found == pack.resources.end() ? nullptr : &*found;
}

void PackCliTest::completePackLifecycle() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto pack_directory = QDir(temporary.path()).filePath(QStringLiteral("starter-pack"));
    const auto first_archive = QDir(temporary.path()).filePath(QStringLiteral("first.awpack"));
    const auto second_archive = QDir(temporary.path()).filePath(QStringLiteral("second.awpack"));
    const auto catalog_directory = QDir(temporary.path()).filePath(QStringLiteral("catalog"));

    const auto templated = runPackCli({QStringLiteral("template"), pack_directory});
    requireSuccess(templated, QStringLiteral("template"));
    const auto template_response = responseObject(templated.standard_output);
    QCOMPARE(template_response.value(QStringLiteral("resource_count")).toInt(), 12);
    QCOMPARE(template_response.value(QStringLiteral("blob_count")).toInt(), 1);
    QVERIFY(QFileInfo::exists(QDir(pack_directory).filePath(QStringLiteral("manifest.json"))));
    QVERIFY(QFileInfo::exists(
        QDir(pack_directory).filePath(QStringLiteral("objects/final-order.pdf"))));
    const auto templated_manifest =
        responseObject(readAll(QDir(pack_directory).filePath(QStringLiteral("manifest.json"))));
    QCOMPARE(templated_manifest.value(QStringLiteral("schema_version")).toInt(), 2);
    QCOMPARE(templated_manifest.value(QStringLiteral("required_capabilities"))
                 .toArray()
                 .at(0)
                 .toObject()
                 .value(QStringLiteral("version"))
                 .toInt(),
             2);

    const auto validated_directory = runPackCli({QStringLiteral("validate"), pack_directory});
    requireSuccess(validated_directory, QStringLiteral("validate"));
    QCOMPARE(responseObject(validated_directory.standard_output)
                 .value(QStringLiteral("source_kind"))
                 .toString(),
             QStringLiteral("directory"));
    QCOMPARE(responseObject(validated_directory.standard_output)
                 .value(QStringLiteral("blob_count"))
                 .toInt(),
             1);

    const auto first_export = runPackCli({QStringLiteral("export"), pack_directory, first_archive});
    requireSuccess(first_export, QStringLiteral("export"));
    const auto first_revision = responseObject(first_export.standard_output);

    const auto validated_archive = runPackCli({QStringLiteral("validate"), first_archive});
    requireSuccess(validated_archive, QStringLiteral("validate"));
    const auto archive_response = responseObject(validated_archive.standard_output);
    QCOMPARE(archive_response.value(QStringLiteral("source_kind")).toString(),
             QStringLiteral("archive"));
    QCOMPARE(archive_response.value(QStringLiteral("blob_count")).toInt(), 1);
    QCOMPARE(archive_response.value(QStringLiteral("digest")),
             first_revision.value(QStringLiteral("digest")));

    const auto second_export =
        runPackCli({QStringLiteral("export"), pack_directory, second_archive});
    requireSuccess(second_export, QStringLiteral("export"));
    const auto second_revision = responseObject(second_export.standard_output);
    QCOMPARE(second_revision.value(QStringLiteral("digest")),
             first_revision.value(QStringLiteral("digest")));
    QCOMPARE(readAll(first_archive), readAll(second_archive));

    const auto installed =
        runPackCli({QStringLiteral("install"), first_archive, catalog_directory,
                    QStringLiteral("--installed-at"), QStringLiteral("2026-08-11T01:02:03Z")});
    requireSuccess(installed, QStringLiteral("install"));
    const auto install_response = responseObject(installed.standard_output);
    QCOMPARE(install_response.value(QStringLiteral("installed_at_utc")).toString(),
             QStringLiteral("2026-08-11T01:02:03Z"));

    const auto listed = runPackCli({QStringLiteral("list"), catalog_directory});
    requireSuccess(listed, QStringLiteral("list"));
    const auto packs =
        responseObject(listed.standard_output).value(QStringLiteral("packs")).toArray();
    QCOMPARE(packs.size(), 1);
    QCOMPARE(packs.at(0).toObject().value(QStringLiteral("digest")),
             first_revision.value(QStringLiteral("digest")));
    QCOMPARE(packs.at(0).toObject().value(QStringLiteral("pack_id")).toString(),
             QStringLiteral("example.full.fictional"));
}

void PackCliTest::authorsRealismEvidenceDeterministically() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto pack_directory = QDir(temporary.path()).filePath(QStringLiteral("evidence-pack"));
    const auto catalog_directory = QDir(temporary.path()).filePath(QStringLiteral("catalog"));
    const auto trace_path = QDir(temporary.path()).filePath(QStringLiteral("trace.json"));
    requireSuccess(runPackCli({QStringLiteral("template"), pack_directory}),
                   QStringLiteral("template"));

    const auto authority_path = QStringLiteral("resources/authority-set.json");
    auto authority_set = responseObject(readAll(QDir(pack_directory).filePath(authority_path)));
    auto authorities = authority_set.value(QStringLiteral("authorities")).toArray();
    auto unused_authority = authorities.first().toObject();
    unused_authority.insert(QStringLiteral("authority_id"),
                            QStringLiteral("example.authority.unused-filing"));
    unused_authority.insert(QStringLiteral("citation"), QStringLiteral("Fictional Rule 99"));
    unused_authority.insert(QStringLiteral("locator"), QStringLiteral("Rule 99"));
    unused_authority.insert(QStringLiteral("source_url"),
                            QStringLiteral("https://example.invalid/rules/99"));
    unused_authority.insert(
        QStringLiteral("proposition"),
        QStringLiteral("An unused filing type has no bearing on this workflow route."));
    authorities.push_back(unused_authority);
    authority_set.insert(QStringLiteral("authorities"), authorities);
    QVERIFY(replaceResourceAndDigest(pack_directory, authority_path,
                                     QStringLiteral("example.authorities.fictional"),
                                     authority_set));

    const auto filing_catalog_path = QStringLiteral("resources/filing-catalog.json");
    auto filing_catalog =
        responseObject(readAll(QDir(pack_directory).filePath(filing_catalog_path)));
    auto filings = filing_catalog.value(QStringLiteral("filings")).toArray();
    filings.push_back(QJsonObject{
        {QStringLiteral("filing_id"), QStringLiteral("example.filing.unused")},
        {QStringLiteral("title"), QStringLiteral("Unused filing")},
        {QStringLiteral("actor_role_ids"), QJsonArray{QStringLiteral("example.role.appellant")}},
        {QStringLiteral("required_field_ids"), QJsonArray{}},
        {QStringLiteral("authority_id"), QStringLiteral("example.authority.unused-filing")},
    });
    filing_catalog.insert(QStringLiteral("filings"), filings);
    QVERIFY(replaceResourceAndDigest(pack_directory, filing_catalog_path,
                                     QStringLiteral("example.catalog.fictional"), filing_catalog));

    const auto pristine = appellate::packs::PackReader::readDirectory(pack_directory);
    QVERIFY(pristine.has_value());
    const auto trace = executedTrace(*pristine);
    QVERIFY(trace.has_value());
    QVERIFY(overwriteAll(trace_path, jsonBytes(*trace)));

    const auto dependency_directory =
        QDir(temporary.path()).filePath(QStringLiteral("evidence-dependency"));
    const auto dependency_archive =
        QDir(temporary.path()).filePath(QStringLiteral("evidence-dependency.awpack"));
    QVERIFY(QDir{}.mkpath(QDir(dependency_directory).filePath(QStringLiteral("resources"))));
    auto dependency_judge = responseObject(
        readAll(QDir(pack_directory).filePath(QStringLiteral("resources/judge-profile.json"))));
    dependency_judge.insert(QStringLiteral("resource_id"),
                            QStringLiteral("example.judge.realism-dependency"));
    const auto dependency_judge_bytes = jsonBytes(dependency_judge);
    QVERIFY(overwriteAll(
        QDir(dependency_directory).filePath(QStringLiteral("resources/judge-profile.json")),
        dependency_judge_bytes));
    const QJsonObject dependency_manifest{
        {QStringLiteral("schema_version"), 2},
        {QStringLiteral("pack_id"), QStringLiteral("example.realism.dependency")},
        {QStringLiteral("version"), QStringLiteral("2026.08.11")},
        {QStringLiteral("required_capabilities"),
         QJsonArray{
             QJsonObject{
                 {QStringLiteral("id"), QStringLiteral("workbench.pack.declarative-resources")},
                 {QStringLiteral("version"), 2}},
             QJsonObject{{QStringLiteral("id"), QStringLiteral("workbench.pack.judge-profile")},
                         {QStringLiteral("version"), 2}},
             QJsonObject{{QStringLiteral("id"), QStringLiteral("workbench.pack.voice-style")},
                         {QStringLiteral("version"), 2}},
         }},
        {QStringLiteral("dependencies"), QJsonArray{}},
        {QStringLiteral("contents"),
         QJsonArray{QJsonObject{
             {QStringLiteral("id"), QStringLiteral("example.judge.realism-dependency")},
             {QStringLiteral("kind"), QStringLiteral("judge_profile")},
             {QStringLiteral("schema_version"), 2},
             {QStringLiteral("path"), QStringLiteral("resources/judge-profile.json")},
             {QStringLiteral("sha256"), sha256(dependency_judge_bytes)},
         }}},
        {QStringLiteral("blobs"), QJsonArray{}},
    };
    QVERIFY(overwriteAll(QDir(dependency_directory).filePath(QStringLiteral("manifest.json")),
                         jsonBytes(dependency_manifest)));
    const auto exported_dependency =
        runPackCli({QStringLiteral("export"), dependency_directory, dependency_archive});
    requireSuccess(exported_dependency, QStringLiteral("export"));
    const auto dependency_revision = responseObject(exported_dependency.standard_output);
    requireSuccess(
        runPackCli({QStringLiteral("install"), dependency_archive, catalog_directory,
                    QStringLiteral("--installed-at"), QStringLiteral("2026-08-11T02:03:04Z")}),
        QStringLiteral("install"));
    const auto root_manifest_path = QDir(pack_directory).filePath(QStringLiteral("manifest.json"));
    auto root_manifest = responseObject(readAll(root_manifest_path));
    root_manifest.insert(
        QStringLiteral("dependencies"),
        QJsonArray{QJsonObject{
            {QStringLiteral("pack_id"), dependency_revision.value(QStringLiteral("pack_id"))},
            {QStringLiteral("version"), dependency_revision.value(QStringLiteral("version"))},
            {QStringLiteral("sha256"), dependency_revision.value(QStringLiteral("digest"))},
        }});
    QVERIFY(overwriteAll(root_manifest_path, jsonBytes(root_manifest)));

    const QJsonObject dimensions{
        {QStringLiteral("procedural_law"), 1},     {QStringLiteral("deadlines_authority"), 1},
        {QStringLiteral("record_consistency"), 1}, {QStringLiteral("consequences"), 1},
        {QStringLiteral("oral_argument"), 1},      {QStringLiteral("bench_differentiation"), 0},
        {QStringLiteral("provenance"), 1},
    };
    const QJsonObject reviewer{
        {QStringLiteral("reviewer_id"), QStringLiteral("example.reviewer.author")},
        {QStringLiteral("display_name"), QStringLiteral("Example Author")},
        {QStringLiteral("qualification"), QStringLiteral("Repository fixture author")},
        {QStringLiteral("affiliation"), QStringLiteral("Example fixture project")},
    };
    const QJsonObject scaffold{
        {QStringLiteral("schema_version"), 2},
        {QStringLiteral("resource_kind"), QStringLiteral("realism_review")},
        {QStringLiteral("resource_id"), QStringLiteral("example.review.cli-realism")},
        {QStringLiteral("case_id"), QStringLiteral("example.case.fictional")},
        {QStringLiteral("review_state"), QStringLiteral("independent_review_pending")},
        {QStringLiteral("reviewed_on"), QStringLiteral("2026-08-11")},
        {QStringLiteral("reviewer_reference"), QStringLiteral("Authoring test memorandum")},
        {QStringLiteral("reviewer"), reviewer},
        {QStringLiteral("dimensions"), dimensions},
        {QStringLiteral("known_uncertainty"),
         QJsonArray{QJsonObject{
             {QStringLiteral("uncertainty_id"), QStringLiteral("example.uncertainty.cli-realism")},
             {QStringLiteral("summary"), QStringLiteral("Synthetic fixture evidence")},
             {QStringLiteral("blocking"), false},
         }}},
    };
    QVERIFY(attachRealismScaffold(pack_directory, scaffold));
    const auto manifest_path = QDir(pack_directory).filePath(QStringLiteral("manifest.json"));
    const auto review_path =
        QDir(pack_directory).filePath(QStringLiteral("resources/realism-review.json"));
    const auto unrelated_path =
        QDir(pack_directory).filePath(QStringLiteral("resources/judge-profile.json"));
    const auto scaffold_manifest = readAll(manifest_path);
    const auto unrelated_bytes = readAll(unrelated_path);

    const auto missing_catalog =
        QDir(temporary.path()).filePath(QStringLiteral("missing-dependency-catalog"));
    const auto before_missing_review = readAll(review_path);
    const auto before_missing_manifest = readAll(manifest_path);
    const auto missing =
        runPackCli({QStringLiteral("author-realism-evidence"), pack_directory, missing_catalog,
                    QStringLiteral("example.review.cli-realism"), trace_path});
    QCOMPARE(missing.exit_code, static_cast<int>(ExitCode::OperationFailed));
    QCOMPARE(readAll(review_path), before_missing_review);
    QCOMPARE(readAll(manifest_path), before_missing_manifest);

    const auto authored =
        runPackCli({QStringLiteral("author-realism-evidence"), pack_directory, catalog_directory,
                    QStringLiteral("example.review.cli-realism"), trace_path});
    if (authored.exit_code != static_cast<int>(ExitCode::Success)) {
        QFAIL(authored.standard_error.constData());
    }
    requireSuccess(authored, QStringLiteral("author-realism-evidence"));
    const auto authored_response = responseObject(authored.standard_output);
    QCOMPARE(authored_response.value(QStringLiteral("updated")).toBool(), true);
    QCOMPARE(authored_response.value(QStringLiteral("case_id")).toString(),
             QStringLiteral("example.case.fictional"));
    QCOMPARE(authored_response.value(QStringLiteral("evidence_counts"))
                 .toObject()
                 .value(QStringLiteral("packs"))
                 .toInt(),
             2);
    QCOMPARE(authored_response.value(QStringLiteral("evidence_counts"))
                 .toObject()
                 .value(QStringLiteral("record_checks"))
                 .toInt(),
             2);
    QCOMPARE(readAll(unrelated_path), unrelated_bytes);

    const auto authored_review_bytes = readAll(review_path);
    const auto authored_manifest_bytes = readAll(manifest_path);
    auto authored_review = responseObject(authored_review_bytes);
    auto human_fields = authored_review;
    human_fields.remove(QStringLiteral("evidence"));
    QCOMPARE(human_fields, scaffold);
    const auto evidence = authored_review.value(QStringLiteral("evidence")).toObject();
    const auto authored_trace =
        evidence.value(QStringLiteral("traces")).toArray().first().toObject();
    QVERIFY(authored_trace.value(QStringLiteral("command_count")).toInt() > 0);
    QVERIFY(authored_trace.value(QStringLiteral("event_count")).toInt() > 0);
    QCOMPARE(authored_trace.value(QStringLiteral("journal_sha256")).toString().size(), 64);
    QCOMPARE(authored_trace.value(QStringLiteral("digest")).toString().size(), 64);
    QCOMPARE(authored_trace.value(QStringLiteral("engine_revision")).toString(),
             QStringLiteral("appellate.realism-evidence.codec-replay.v1"));
    QVERIFY(!authored_trace.value(QStringLiteral("operation_ids")).toArray().isEmpty());
    QCOMPARE(evidence.value(QStringLiteral("dimension_evidence"))
                 .toObject()
                 .value(QStringLiteral("bench_differentiation"))
                 .toArray()
                 .size(),
             0);
    QCOMPARE(evidence.value(QStringLiteral("authorities")).toArray().size(), 6);
    QHash<QString, QString> authority_evidence_ids;
    for (const auto& value : evidence.value(QStringLiteral("authorities")).toArray()) {
        const auto binding = value.toObject();
        authority_evidence_ids.insert(binding.value(QStringLiteral("authority_id")).toString(),
                                      binding.value(QStringLiteral("evidence_id")).toString());
    }
    QVERIFY(!authority_evidence_ids.contains(QStringLiteral("example.authority.unused-filing")));
    const auto dimension_bindings = evidence.value(QStringLiteral("dimension_evidence")).toObject();
    QSet<QString> deadline_refs;
    for (const auto& value :
         dimension_bindings.value(QStringLiteral("deadlines_authority")).toArray()) {
        deadline_refs.insert(value.toString());
    }
    QVERIFY(deadline_refs.contains(
        authority_evidence_ids.value(QStringLiteral("example.authority.cure-deadline"))));
    QVERIFY(!deadline_refs.contains(
        authority_evidence_ids.value(QStringLiteral("example.authority.judgment"))));
    QVERIFY(!deadline_refs.contains(
        authority_evidence_ids.value(QStringLiteral("example.authority.rule-one"))));

    const auto dependency_for_validation =
        appellate::packs::PackReader::readDirectory(dependency_directory);
    QVERIFY(dependency_for_validation.has_value());
    const auto tamperedHelperReviewError = [&]() {
        const auto root = appellate::packs::PackReader::readDirectory(
            pack_directory, appellate::packs::PackValidationScope::ResolvedClosure);
        if (!root) {
            return root.error().message;
        }
        const std::array<const appellate::packs::LoadedPack*, 1> dependencies{
            &*dependency_for_validation};
        const auto validated = appellate::packs::PackReader::validateResolvedGraph(
            *root, std::span<const appellate::packs::LoadedPack* const>(dependencies));
        return validated ? QString{} : validated.error().message;
    };
    const auto replaceEvidenceReferences = [](QJsonObject& review, const QString& old_id,
                                              const QString& new_id) {
        auto review_evidence = review.value(QStringLiteral("evidence")).toObject();
        auto dimension_references =
            review_evidence.value(QStringLiteral("dimension_evidence")).toObject();
        for (auto iterator = dimension_references.begin(); iterator != dimension_references.end();
             ++iterator) {
            auto references = iterator.value().toArray();
            for (qsizetype index = 0; index < references.size(); ++index) {
                if (references.at(index).toString() == old_id) {
                    references.replace(index, new_id);
                }
            }
            iterator.value() = references;
        }
        review_evidence.insert(QStringLiteral("dimension_evidence"), dimension_references);
        review.insert(QStringLiteral("evidence"), review_evidence);
    };

    auto tampered_resource_review = authored_review;
    auto tampered_resource_evidence =
        tampered_resource_review.value(QStringLiteral("evidence")).toObject();
    auto tampered_resources =
        tampered_resource_evidence.value(QStringLiteral("resources")).toArray();
    auto tampered_resource = tampered_resources.first().toObject();
    const auto original_resource_evidence_id =
        tampered_resource.value(QStringLiteral("evidence_id")).toString();
    const auto tampered_resource_evidence_id = QStringLiteral("example.evidence.tampered-resource");
    tampered_resource.insert(QStringLiteral("evidence_id"), tampered_resource_evidence_id);
    tampered_resources.replace(0, tampered_resource);
    tampered_resource_evidence.insert(QStringLiteral("resources"), tampered_resources);
    tampered_resource_review.insert(QStringLiteral("evidence"), tampered_resource_evidence);
    replaceEvidenceReferences(tampered_resource_review, original_resource_evidence_id,
                              tampered_resource_evidence_id);
    tampered_resource_evidence =
        tampered_resource_review.value(QStringLiteral("evidence")).toObject();
    tampered_resource_evidence.insert(QStringLiteral("closure_digest"),
                                      recomputedClosureDigest(tampered_resource_review));
    tampered_resource_review.insert(QStringLiteral("evidence"), tampered_resource_evidence);
    QVERIFY(replaceResourceAndDigest(
        pack_directory, QStringLiteral("resources/realism-review.json"),
        QStringLiteral("example.review.cli-realism"), tampered_resource_review));
    const auto resource_tamper_error = tamperedHelperReviewError();
    QVERIFY2(resource_tamper_error.contains(
                 QStringLiteral("authoring-profile resource evidence ID is stale")),
             resource_tamper_error.toUtf8().constData());
    QVERIFY(overwriteAll(review_path, authored_review_bytes));
    QVERIFY(overwriteAll(manifest_path, authored_manifest_bytes));

    auto tampered_blob_review = authored_review;
    auto tampered_blob_evidence = tampered_blob_review.value(QStringLiteral("evidence")).toObject();
    auto tampered_blobs = tampered_blob_evidence.value(QStringLiteral("blobs")).toArray();
    QVERIFY(!tampered_blobs.isEmpty());
    auto tampered_blob = tampered_blobs.first().toObject();
    const auto original_blob_evidence_id =
        tampered_blob.value(QStringLiteral("evidence_id")).toString();
    const auto tampered_blob_evidence_id = QStringLiteral("example.evidence.tampered-blob");
    tampered_blob.insert(QStringLiteral("evidence_id"), tampered_blob_evidence_id);
    tampered_blobs.replace(0, tampered_blob);
    tampered_blob_evidence.insert(QStringLiteral("blobs"), tampered_blobs);
    tampered_blob_review.insert(QStringLiteral("evidence"), tampered_blob_evidence);
    replaceEvidenceReferences(tampered_blob_review, original_blob_evidence_id,
                              tampered_blob_evidence_id);
    tampered_blob_evidence = tampered_blob_review.value(QStringLiteral("evidence")).toObject();
    tampered_blob_evidence.insert(QStringLiteral("closure_digest"),
                                  recomputedClosureDigest(tampered_blob_review));
    tampered_blob_review.insert(QStringLiteral("evidence"), tampered_blob_evidence);
    QVERIFY(replaceResourceAndDigest(
        pack_directory, QStringLiteral("resources/realism-review.json"),
        QStringLiteral("example.review.cli-realism"), tampered_blob_review));
    const auto blob_tamper_error = tamperedHelperReviewError();
    QVERIFY2(
        blob_tamper_error.contains(QStringLiteral("authoring-profile blob evidence ID is stale")),
        blob_tamper_error.toUtf8().constData());
    QVERIFY(overwriteAll(review_path, authored_review_bytes));
    QVERIFY(overwriteAll(manifest_path, authored_manifest_bytes));

    auto tampered_check_review = authored_review;
    auto tampered_check_evidence =
        tampered_check_review.value(QStringLiteral("evidence")).toObject();
    auto tampered_checks = tampered_check_evidence.value(QStringLiteral("record_checks")).toArray();
    qsizetype page_anchor_index = -1;
    for (qsizetype index = 0; index < tampered_checks.size(); ++index) {
        if (tampered_checks.at(index).toObject().value(QStringLiteral("check_kind")).toString() ==
            QStringLiteral("page_anchor_resolution")) {
            page_anchor_index = index;
            break;
        }
    }
    QVERIFY(page_anchor_index >= 0);
    auto tampered_check = tampered_checks.at(page_anchor_index).toObject();
    tampered_check.insert(QStringLiteral("check_id"),
                          QStringLiteral("example.check.tampered-page-anchor"));
    tampered_check.insert(QStringLiteral("digest"),
                          recomputedPageAnchorCheckDigest(tampered_check_review, tampered_check));
    tampered_checks.replace(page_anchor_index, tampered_check);
    tampered_check_evidence.insert(QStringLiteral("record_checks"), tampered_checks);
    tampered_check_review.insert(QStringLiteral("evidence"), tampered_check_evidence);
    QVERIFY(replaceResourceAndDigest(
        pack_directory, QStringLiteral("resources/realism-review.json"),
        QStringLiteral("example.review.cli-realism"), tampered_check_review));
    const auto check_tamper_error = tamperedHelperReviewError();
    QVERIFY2(
        check_tamper_error.contains(QStringLiteral("authoring-profile record check_id is stale")),
        check_tamper_error.toUtf8().constData());
    QVERIFY(overwriteAll(review_path, authored_review_bytes));
    QVERIFY(overwriteAll(manifest_path, authored_manifest_bytes));

    const auto manifest = responseObject(authored_manifest_bytes);
    QString declared_review_sha;
    for (const auto& value : manifest.value(QStringLiteral("contents")).toArray()) {
        const auto descriptor = value.toObject();
        if (descriptor.value(QStringLiteral("id")).toString() ==
            QStringLiteral("example.review.cli-realism")) {
            declared_review_sha = descriptor.value(QStringLiteral("sha256")).toString();
        }
    }
    QCOMPARE(declared_review_sha, sha256(authored_review_bytes));
    QCOMPARE(authored_response.value(QStringLiteral("review_sha256")).toString(),
             declared_review_sha);
    const auto resolved_archive =
        QDir(temporary.path()).filePath(QStringLiteral("authored-evidence.awpack"));
    requireSuccess(
        runPackCli({QStringLiteral("export-deferred"), pack_directory, resolved_archive}),
        QStringLiteral("export-deferred"));
    const auto installed_root =
        runPackCli({QStringLiteral("install"), resolved_archive, catalog_directory,
                    QStringLiteral("--installed-at"), QStringLiteral("2026-08-11T03:04:05Z")});
    requireSuccess(installed_root, QStringLiteral("install"));
    const auto installed_root_revision = responseObject(installed_root.standard_output);
    QCOMPARE(installed_root_revision.value(QStringLiteral("digest")),
             authored_response.value(QStringLiteral("digest")));
    const auto resolved_validation =
        runPackCli({QStringLiteral("validate-resolved"), catalog_directory,
                    installed_root_revision.value(QStringLiteral("pack_id")).toString(),
                    installed_root_revision.value(QStringLiteral("version")).toString(),
                    installed_root_revision.value(QStringLiteral("digest")).toString()});
    requireSuccess(resolved_validation, QStringLiteral("validate-resolved"));
    QCOMPARE(responseObject(resolved_validation.standard_output)
                 .value(QStringLiteral("resolved_revision_count"))
                 .toInt(),
             2);

    QVERIFY(overwriteAll(trace_path, jsonBytes(authored_trace)));
    const auto repeated =
        runPackCli({QStringLiteral("author-realism-evidence"), pack_directory, catalog_directory,
                    QStringLiteral("example.review.cli-realism"), trace_path});
    requireSuccess(repeated, QStringLiteral("author-realism-evidence"));
    QCOMPARE(responseObject(repeated.standard_output).value(QStringLiteral("updated")).toBool(),
             false);
    QCOMPARE(readAll(review_path), authored_review_bytes);
    QCOMPARE(readAll(manifest_path), authored_manifest_bytes);

    auto differently_formatted_manifest =
        QJsonDocument(responseObject(authored_manifest_bytes)).toJson(QJsonDocument::Compact) +
        '\n';
    QVERIFY(differently_formatted_manifest != authored_manifest_bytes);
    QVERIFY(overwriteAll(manifest_path, differently_formatted_manifest));
    const auto manifest_only =
        runPackCli({QStringLiteral("author-realism-evidence"), pack_directory, catalog_directory,
                    QStringLiteral("example.review.cli-realism"), trace_path});
    requireSuccess(manifest_only, QStringLiteral("author-realism-evidence"));
    QCOMPARE(
        responseObject(manifest_only.standard_output).value(QStringLiteral("updated")).toBool(),
        true);
    QCOMPARE(readAll(review_path), authored_review_bytes);
    QCOMPARE(readAll(manifest_path), authored_manifest_bytes);

    // A sibling transaction journal makes the review-new/manifest-old interruption recoverable.
    QVERIFY(overwriteAll(manifest_path, scaffold_manifest));
    QVERIFY(!appellate::packs::PackReader::readDirectory(pack_directory).has_value());
    const auto transaction_directory =
        QDir(temporary.path())
            .filePath(QStringLiteral(".evidence-pack.author-realism-evidence.transaction"));
    QVERIFY(QDir{}.mkpath(transaction_directory));
    QVERIFY(QFile::setPermissions(transaction_directory, QFileDevice::ReadOwner |
                                                             QFileDevice::WriteOwner |
                                                             QFileDevice::ExeOwner));
    QVERIFY(overwriteAll(QDir(transaction_directory).filePath(QStringLiteral("review.old")),
                         before_missing_review));
    QVERIFY(overwriteAll(QDir(transaction_directory).filePath(QStringLiteral("review.new")),
                         authored_review_bytes));
    QVERIFY(overwriteAll(QDir(transaction_directory).filePath(QStringLiteral("manifest.old")),
                         scaffold_manifest));
    QVERIFY(overwriteAll(QDir(transaction_directory).filePath(QStringLiteral("manifest.new")),
                         authored_manifest_bytes));
    const QJsonObject transaction_journal{
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("root_directory"), QFileInfo(pack_directory).canonicalFilePath()},
        {QStringLiteral("review_resource_id"), QStringLiteral("example.review.cli-realism")},
        {QStringLiteral("review_path"), QStringLiteral("resources/realism-review.json")},
        {QStringLiteral("pack_id"), authored_response.value(QStringLiteral("pack_id"))},
        {QStringLiteral("version"), authored_response.value(QStringLiteral("version"))},
        {QStringLiteral("final_digest"), authored_response.value(QStringLiteral("digest"))},
        {QStringLiteral("old_review_sha256"), sha256(before_missing_review)},
        {QStringLiteral("new_review_sha256"), sha256(authored_review_bytes)},
        {QStringLiteral("old_manifest_sha256"), sha256(scaffold_manifest)},
        {QStringLiteral("new_manifest_sha256"), sha256(authored_manifest_bytes)},
    };
    QVERIFY(overwriteAll(QDir(transaction_directory).filePath(QStringLiteral("journal.json")),
                         QJsonDocument(transaction_journal).toJson(QJsonDocument::Compact) + '\n'));
    const auto recovered =
        runPackCli({QStringLiteral("author-realism-evidence"), pack_directory, catalog_directory,
                    QStringLiteral("example.review.cli-realism"), trace_path});
    requireSuccess(recovered, QStringLiteral("author-realism-evidence"));
    QCOMPARE(responseObject(recovered.standard_output).value(QStringLiteral("updated")).toBool(),
             true);
    QCOMPARE(readAll(review_path), authored_review_bytes);
    QCOMPARE(readAll(manifest_path), authored_manifest_bytes);
    QVERIFY(!QFileInfo::exists(transaction_directory));

    QVERIFY(overwriteAll(manifest_path, scaffold_manifest));
    QVERIFY(QDir{}.mkpath(transaction_directory));
    QVERIFY(QFile::setPermissions(transaction_directory, QFileDevice::ReadOwner |
                                                             QFileDevice::WriteOwner |
                                                             QFileDevice::ExeOwner));
    QVERIFY(overwriteAll(QDir(transaction_directory).filePath(QStringLiteral("review.old")),
                         before_missing_review));
    QVERIFY(overwriteAll(QDir(transaction_directory).filePath(QStringLiteral("review.new")),
                         authored_review_bytes));
    QVERIFY(overwriteAll(QDir(transaction_directory).filePath(QStringLiteral("manifest.old")),
                         scaffold_manifest));
    QVERIFY(overwriteAll(QDir(transaction_directory).filePath(QStringLiteral("manifest.new")),
                         authored_manifest_bytes));
    auto conflicting_recovery_journal = transaction_journal;
    conflicting_recovery_journal.insert(QStringLiteral("final_digest"),
                                        QString(64, QLatin1Char('f')));
    QVERIFY(overwriteAll(
        QDir(transaction_directory).filePath(QStringLiteral("journal.json")),
        QJsonDocument(conflicting_recovery_journal).toJson(QJsonDocument::Compact) + '\n'));
    const auto recovery_conflict =
        runPackCli({QStringLiteral("author-realism-evidence"), pack_directory, catalog_directory,
                    QStringLiteral("example.review.cli-realism"), trace_path});
    QCOMPARE(recovery_conflict.exit_code, static_cast<int>(ExitCode::OperationFailed));
    QCOMPARE(
        responseObject(recovery_conflict.standard_error).value(QStringLiteral("code")).toString(),
        QStringLiteral("immutable_conflict"));
    QCOMPARE(readAll(review_path), authored_review_bytes);
    QCOMPARE(readAll(manifest_path), scaffold_manifest);
    QVERIFY(QFileInfo::exists(transaction_directory));
    QVERIFY(QDir(transaction_directory).removeRecursively());
    QVERIFY(overwriteAll(manifest_path, authored_manifest_bytes));

    QVERIFY(QDir{}.mkpath(transaction_directory));
    QVERIFY(QFile::setPermissions(transaction_directory, QFileDevice::ReadOwner |
                                                             QFileDevice::WriteOwner |
                                                             QFileDevice::ExeOwner));
    QVERIFY(overwriteAll(QDir(transaction_directory).filePath(QStringLiteral("review.old")),
                         before_missing_review));
    QVERIFY(overwriteAll(QDir(transaction_directory).filePath(QStringLiteral("review.new")),
                         authored_review_bytes));
    QVERIFY(overwriteAll(QDir(transaction_directory).filePath(QStringLiteral("manifest.old")),
                         scaffold_manifest));
    QVERIFY(overwriteAll(QDir(transaction_directory).filePath(QStringLiteral("manifest.new")),
                         authored_manifest_bytes));
    QVERIFY(overwriteAll(QDir(transaction_directory).filePath(QStringLiteral("journal.json")),
                         QJsonDocument(transaction_journal).toJson(QJsonDocument::Compact) + '\n'));
    const QByteArray third_party_review{"third-party bytes\n"};
    QVERIFY(overwriteAll(review_path, third_party_review));
    const auto guarded =
        runPackCli({QStringLiteral("author-realism-evidence"), pack_directory, catalog_directory,
                    QStringLiteral("example.review.cli-realism"), trace_path});
    QCOMPARE(guarded.exit_code, static_cast<int>(ExitCode::OperationFailed));
    QCOMPARE(responseObject(guarded.standard_error).value(QStringLiteral("code")).toString(),
             QStringLiteral("realism_evidence_recovery_conflict"));
    QCOMPARE(readAll(review_path), third_party_review);
    QCOMPARE(readAll(manifest_path), authored_manifest_bytes);
    QVERIFY(QDir(transaction_directory).removeRecursively());
    QVERIFY(overwriteAll(review_path, authored_review_bytes));

#if defined(Q_OS_UNIX)
    auto padded_review_bytes = authored_review_bytes;
    padded_review_bytes.append(QByteArray(7 * 1024 * 1024, ' '));
    QVERIFY(padded_review_bytes.size() < 8 * 1024 * 1024);
    QVERIFY(overwriteAll(review_path, padded_review_bytes));
    auto padded_manifest_object = responseObject(authored_manifest_bytes);
    auto padded_contents = padded_manifest_object.value(QStringLiteral("contents")).toArray();
    for (qsizetype index = 0; index < padded_contents.size(); ++index) {
        auto descriptor = padded_contents.at(index).toObject();
        if (descriptor.value(QStringLiteral("id")).toString() ==
            QStringLiteral("example.review.cli-realism")) {
            descriptor.insert(QStringLiteral("sha256"), sha256(padded_review_bytes));
            padded_contents.replace(index, descriptor);
        }
    }
    padded_manifest_object.insert(QStringLiteral("contents"), padded_contents);
    const auto padded_manifest_bytes = jsonBytes(padded_manifest_object);
    QVERIFY(overwriteAll(manifest_path, padded_manifest_bytes));

    const auto moved_transaction_directory =
        transaction_directory + QStringLiteral(".attacker-moved");
    const auto transaction_victim =
        QDir(temporary.path()).filePath(QStringLiteral("transaction-victim"));
    QVERIFY(QDir{}.mkpath(transaction_victim));
    const auto victim_sentinel = QDir(transaction_victim).filePath(QStringLiteral("sentinel.txt"));
    const auto victim_review_apply =
        QDir(transaction_victim).filePath(QStringLiteral("review.apply"));
    const auto victim_manifest_apply =
        QDir(transaction_victim).filePath(QStringLiteral("manifest.apply"));
    QVERIFY(overwriteAll(victim_sentinel, QByteArray("victim sentinel\n")));
    QVERIFY(overwriteAll(victim_review_apply, QByteArray("victim review apply\n")));
    QVERIFY(overwriteAll(victim_manifest_apply, QByteArray("victim manifest apply\n")));
    const auto victim_entries =
        QDir(transaction_victim).entryList(QDir::Files | QDir::Hidden | QDir::System, QDir::Name);
    std::atomic_bool transaction_substituted{};
    std::jthread transaction_substituter([&] {
        for (int iteration = 0; iteration < 10'000 && !QFileInfo::exists(transaction_directory);
             ++iteration) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto renamed = QDir(temporary.path())
                                 .rename(QFileInfo(transaction_directory).fileName(),
                                         QFileInfo(moved_transaction_directory).fileName());
        const auto linked = renamed && QFile::link(transaction_victim, transaction_directory);
        transaction_substituted.store(linked);
    });
    const auto substituted_transaction =
        runPackCli({QStringLiteral("author-realism-evidence"), pack_directory, catalog_directory,
                    QStringLiteral("example.review.cli-realism"), trace_path});
    transaction_substituter.join();
    QVERIFY(transaction_substituted.load());
    QCOMPARE(substituted_transaction.exit_code, static_cast<int>(ExitCode::OperationFailed));
    QCOMPARE(readAll(review_path), padded_review_bytes);
    QCOMPARE(readAll(manifest_path), padded_manifest_bytes);
    QCOMPARE(
        QDir(transaction_victim).entryList(QDir::Files | QDir::Hidden | QDir::System, QDir::Name),
        victim_entries);
    QCOMPARE(readAll(victim_sentinel), QByteArray("victim sentinel\n"));
    QCOMPARE(readAll(victim_review_apply), QByteArray("victim review apply\n"));
    QCOMPARE(readAll(victim_manifest_apply), QByteArray("victim manifest apply\n"));
    QVERIFY(QFileInfo(transaction_directory).isSymLink());
    QVERIFY(QFile::remove(transaction_directory));
    QVERIFY(QDir(moved_transaction_directory).removeRecursively());
    QVERIFY(overwriteAll(review_path, authored_review_bytes));
    QVERIFY(overwriteAll(manifest_path, authored_manifest_bytes));
#endif

    auto wrong_engine_trace = *trace;
    wrong_engine_trace.insert(QStringLiteral("engine_revision"),
                              QStringLiteral("engine.example.unverifiable.v1"));
    QVERIFY(overwriteAll(trace_path, jsonBytes(wrong_engine_trace)));
    const auto wrong_engine =
        runPackCli({QStringLiteral("author-realism-evidence"), pack_directory, catalog_directory,
                    QStringLiteral("example.review.cli-realism"), trace_path});
    QCOMPARE(wrong_engine.exit_code, static_cast<int>(ExitCode::InvalidPack));
    QCOMPARE(readAll(review_path), authored_review_bytes);
    QCOMPARE(readAll(manifest_path), authored_manifest_bytes);

    QVERIFY(overwriteAll(trace_path, jsonBytes(authored_trace)));
    const auto trace_link = QDir(temporary.path()).filePath(QStringLiteral("trace-link.json"));
    QVERIFY(QFile::link(trace_path, trace_link));
    QVERIFY(QFileInfo(trace_link).isSymLink());
    const auto linked_trace =
        runPackCli({QStringLiteral("author-realism-evidence"), pack_directory, catalog_directory,
                    QStringLiteral("example.review.cli-realism"), trace_link});
    QCOMPARE(linked_trace.exit_code, static_cast<int>(ExitCode::InvalidPack));
    QCOMPARE(responseObject(linked_trace.standard_error).value(QStringLiteral("code")).toString(),
             QStringLiteral("cannot_read_trace"));

    const auto oversized_trace =
        QDir(temporary.path()).filePath(QStringLiteral("oversized-trace.json"));
    QFile oversized(oversized_trace);
    QVERIFY(oversized.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QVERIFY(oversized.resize(64LL * 1024LL * 1024LL + 1LL));
    oversized.close();
    const auto oversized_result =
        runPackCli({QStringLiteral("author-realism-evidence"), pack_directory, catalog_directory,
                    QStringLiteral("example.review.cli-realism"), oversized_trace});
    QCOMPARE(oversized_result.exit_code, static_cast<int>(ExitCode::InvalidPack));
    QCOMPARE(readAll(review_path), authored_review_bytes);
    QCOMPARE(readAll(manifest_path), authored_manifest_bytes);

    const auto growing_trace =
        QDir(temporary.path()).filePath(QStringLiteral("growing-trace.json"));
    QFile growing(growing_trace);
    QVERIFY(growing.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QVERIFY(growing.resize(64LL * 1024LL * 1024LL));
    growing.close();
    std::jthread grower([growing_trace] {
        QFile writer(growing_trace);
        if (writer.open(QIODevice::WriteOnly | QIODevice::Append)) {
            for (int iteration = 0; iteration < 4096; ++iteration) {
                static_cast<void>(writer.write("xx", 2));
                static_cast<void>(writer.flush());
            }
        }
    });
    const auto growing_result =
        runPackCli({QStringLiteral("author-realism-evidence"), pack_directory, catalog_directory,
                    QStringLiteral("example.review.cli-realism"), growing_trace});
    grower.join();
    QCOMPARE(growing_result.exit_code, static_cast<int>(ExitCode::InvalidPack));
    QCOMPARE(responseObject(growing_result.standard_error).value(QStringLiteral("code")).toString(),
             QStringLiteral("cannot_read_trace"));
    QCOMPARE(readAll(review_path), authored_review_bytes);
    QCOMPARE(readAll(manifest_path), authored_manifest_bytes);

    const auto lock_path =
        QDir(temporary.path())
            .filePath(QStringLiteral(".evidence-pack.author-realism-evidence.lock"));
    QLockFile held_lock(lock_path);
    held_lock.setStaleLockTime(0);
    QVERIFY(held_lock.tryLock(0));
    const auto locked =
        runPackCli({QStringLiteral("author-realism-evidence"), pack_directory, catalog_directory,
                    QStringLiteral("example.review.cli-realism"), trace_path});
    QCOMPARE(locked.exit_code, static_cast<int>(ExitCode::OperationFailed));
    QCOMPARE(responseObject(locked.standard_error).value(QStringLiteral("code")).toString(),
             QStringLiteral("authoring_locked"));
    QCOMPARE(readAll(review_path), authored_review_bytes);
    QCOMPARE(readAll(manifest_path), authored_manifest_bytes);
    held_lock.unlock();

#if defined(Q_OS_UNIX)
    const auto crashed_lock_holder = ::fork();
    QVERIFY(crashed_lock_holder >= 0);
    if (crashed_lock_holder == 0) {
        QLockFile abandoned(lock_path);
        abandoned.setStaleLockTime(30'000);
        ::_exit(abandoned.tryLock(0) ? 0 : 1);
    }
    int lock_holder_status{};
    QCOMPARE(::waitpid(crashed_lock_holder, &lock_holder_status, 0), crashed_lock_holder);
    QVERIFY(WIFEXITED(lock_holder_status));
    QCOMPARE(WEXITSTATUS(lock_holder_status), 0);
    QVERIFY(QFileInfo::exists(lock_path));
    const auto recovered_dead_lock =
        runPackCli({QStringLiteral("author-realism-evidence"), pack_directory, catalog_directory,
                    QStringLiteral("example.review.cli-realism"), trace_path});
    requireSuccess(recovered_dead_lock, QStringLiteral("author-realism-evidence"));
    QCOMPARE(responseObject(recovered_dead_lock.standard_output)
                 .value(QStringLiteral("updated"))
                 .toBool(),
             false);
    QVERIFY(!QFileInfo::exists(lock_path));

    const auto redirect_directory =
        QDir(temporary.path()).filePath(QStringLiteral("redirect-pack"));
    requireSuccess(runPackCli({QStringLiteral("template"), redirect_directory}),
                   QStringLiteral("template"));
    const auto redirect_manifest =
        readAll(QDir(redirect_directory).filePath(QStringLiteral("manifest.json")));
    const auto moved_directory =
        QDir(temporary.path()).filePath(QStringLiteral("evidence-pack-moved"));
    const auto padded_trace_path =
        QDir(temporary.path()).filePath(QStringLiteral("padded-trace.json"));
    auto padded_trace = jsonBytes(authored_trace);
    padded_trace.append(QByteArray(32 * 1024 * 1024, ' '));
    QVERIFY(overwriteAll(padded_trace_path, padded_trace));
    std::atomic_bool root_swapped{};
    std::jthread swapper([&] {
        for (int iteration = 0; iteration < 10'000 && !QFileInfo::exists(lock_path); ++iteration) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto renamed =
            QDir(temporary.path())
                .rename(QStringLiteral("evidence-pack"), QStringLiteral("evidence-pack-moved"));
        const auto linked = renamed && QFile::link(redirect_directory, pack_directory);
        root_swapped.store(linked);
    });
    const auto swapped_root =
        runPackCli({QStringLiteral("author-realism-evidence"), pack_directory, catalog_directory,
                    QStringLiteral("example.review.cli-realism"), padded_trace_path});
    swapper.join();
    QVERIFY(root_swapped.load());
    QCOMPARE(swapped_root.exit_code, static_cast<int>(ExitCode::OperationFailed));
    QCOMPARE(responseObject(swapped_root.standard_error).value(QStringLiteral("code")).toString(),
             QStringLiteral("authoring_root_changed"));
    QCOMPARE(
        readAll(QDir(moved_directory).filePath(QStringLiteral("resources/realism-review.json"))),
        authored_review_bytes);
    QCOMPARE(readAll(QDir(moved_directory).filePath(QStringLiteral("manifest.json"))),
             authored_manifest_bytes);
    QCOMPARE(readAll(QDir(redirect_directory).filePath(QStringLiteral("manifest.json"))),
             redirect_manifest);
    QVERIFY(!QFileInfo::exists(
        QDir(redirect_directory).filePath(QStringLiteral("resources/realism-review.json"))));
    QVERIFY(QFileInfo(pack_directory).isSymLink());
    QVERIFY(QFile::remove(pack_directory));
    QVERIFY(QDir(temporary.path())
                .rename(QStringLiteral("evidence-pack-moved"), QStringLiteral("evidence-pack")));

#if defined(Q_OS_LINUX)
    auto final_window_manifest = jsonBytes(responseObject(authored_manifest_bytes));
    final_window_manifest.append(QByteArray(800 * 1024, ' '));
    QVERIFY(final_window_manifest.size() < 1024 * 1024);
    QVERIFY(overwriteAll(manifest_path, final_window_manifest));
    const auto final_window_moved =
        QDir(temporary.path()).filePath(QStringLiteral("evidence-pack-final-window"));
    std::atomic_bool saw_final_transaction{};
    std::atomic_bool final_window_swapped{};
    std::jthread final_window_swapper([&] {
        for (int iteration = 0; iteration < 20'000; ++iteration) {
            if (QFileInfo::exists(transaction_directory)) {
                saw_final_transaction.store(true);
                break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        if (!saw_final_transaction.load()) {
            return;
        }
        for (int iteration = 0; iteration < 20'000; ++iteration) {
            if (readAll(manifest_path) == authored_manifest_bytes) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        if (readAll(manifest_path) != authored_manifest_bytes) {
            return;
        }
        const auto renamed = QDir(temporary.path())
                                 .rename(QStringLiteral("evidence-pack"),
                                         QStringLiteral("evidence-pack-final-window"));
        const auto linked = renamed && QFile::link(redirect_directory, pack_directory);
        final_window_swapped.store(linked);
    });
    const auto final_window =
        runPackCli({QStringLiteral("author-realism-evidence"), pack_directory, catalog_directory,
                    QStringLiteral("example.review.cli-realism"), trace_path});
    final_window_swapper.join();
    QVERIFY(saw_final_transaction.load());
    QVERIFY(final_window_swapped.load());
    QCOMPARE(final_window.exit_code, static_cast<int>(ExitCode::OperationFailed));
    QCOMPARE(responseObject(final_window.standard_error).value(QStringLiteral("code")).toString(),
             QStringLiteral("authoring_root_changed"));
    QCOMPARE(readAll(QDir(final_window_moved).filePath(QStringLiteral("manifest.json"))),
             authored_manifest_bytes);
    QCOMPARE(readAll(QDir(redirect_directory).filePath(QStringLiteral("manifest.json"))),
             redirect_manifest);
    QVERIFY(QFileInfo(pack_directory).isSymLink());
    QVERIFY(QFile::remove(pack_directory));
    QVERIFY(
        QDir(temporary.path())
            .rename(QStringLiteral("evidence-pack-final-window"), QStringLiteral("evidence-pack")));
    QVERIFY(QDir(transaction_directory).removeRecursively());
#endif
#endif

    auto stale_trace = *trace;
    stale_trace.insert(QStringLiteral("command_count"), 99);
    QVERIFY(overwriteAll(trace_path, jsonBytes(stale_trace)));
    const auto before_failed_review = readAll(review_path);
    const auto before_failed_manifest = readAll(manifest_path);
    const auto stale =
        runPackCli({QStringLiteral("author-realism-evidence"), pack_directory, catalog_directory,
                    QStringLiteral("example.review.cli-realism"), trace_path});
    QCOMPARE(stale.exit_code, static_cast<int>(ExitCode::InvalidPack));
    QVERIFY(stale.standard_output.isEmpty());
    QCOMPARE(readAll(review_path), before_failed_review);
    QCOMPARE(readAll(manifest_path), before_failed_manifest);

    QVERIFY(overwriteAll(trace_path, jsonBytes(authored_trace)));
    auto conflicting_review = authored_review;
    conflicting_review.insert(QStringLiteral("reviewer_reference"),
                              QStringLiteral("Changed after immutable installation"));
    const auto conflicting_review_bytes = jsonBytes(conflicting_review);
    QVERIFY(overwriteAll(review_path, conflicting_review_bytes));
    auto conflicting_manifest = responseObject(authored_manifest_bytes);
    auto conflicting_contents = conflicting_manifest.value(QStringLiteral("contents")).toArray();
    for (qsizetype index = 0; index < conflicting_contents.size(); ++index) {
        auto descriptor = conflicting_contents.at(index).toObject();
        if (descriptor.value(QStringLiteral("id")).toString() ==
            QStringLiteral("example.review.cli-realism")) {
            descriptor.insert(QStringLiteral("sha256"), sha256(conflicting_review_bytes));
            conflicting_contents.replace(index, descriptor);
        }
    }
    conflicting_manifest.insert(QStringLiteral("contents"), conflicting_contents);
    const auto conflicting_manifest_bytes = jsonBytes(conflicting_manifest);
    QVERIFY(overwriteAll(manifest_path, conflicting_manifest_bytes));
    const auto conflict =
        runPackCli({QStringLiteral("author-realism-evidence"), pack_directory, catalog_directory,
                    QStringLiteral("example.review.cli-realism"), trace_path});
    QCOMPARE(conflict.exit_code, static_cast<int>(ExitCode::OperationFailed));
    QCOMPARE(responseObject(conflict.standard_error).value(QStringLiteral("code")).toString(),
             QStringLiteral("immutable_conflict"));
    QCOMPARE(readAll(review_path), conflicting_review_bytes);
    QCOMPARE(readAll(manifest_path), conflicting_manifest_bytes);
    QVERIFY(overwriteAll(review_path, authored_review_bytes));
    QVERIFY(overwriteAll(manifest_path, authored_manifest_bytes));

    authored_review.insert(QStringLiteral("review_state"),
                           QStringLiteral("independently_reviewed"));
    const auto independent_bytes = jsonBytes(authored_review);
    QVERIFY(overwriteAll(review_path, independent_bytes));
    auto independent_manifest = responseObject(authored_manifest_bytes);
    auto independent_contents = independent_manifest.value(QStringLiteral("contents")).toArray();
    for (qsizetype index = 0; index < independent_contents.size(); ++index) {
        auto descriptor = independent_contents.at(index).toObject();
        if (descriptor.value(QStringLiteral("id")).toString() ==
            QStringLiteral("example.review.cli-realism")) {
            descriptor.insert(QStringLiteral("sha256"), sha256(independent_bytes));
            independent_contents.replace(index, descriptor);
        }
    }
    independent_manifest.insert(QStringLiteral("contents"), independent_contents);
    QVERIFY(overwriteAll(manifest_path, jsonBytes(independent_manifest)));
    const auto before_independent_review = readAll(review_path);
    const auto before_independent_manifest = readAll(manifest_path);
    const auto independent =
        runPackCli({QStringLiteral("author-realism-evidence"), pack_directory, catalog_directory,
                    QStringLiteral("example.review.cli-realism"), trace_path});
    QCOMPARE(independent.exit_code, static_cast<int>(ExitCode::InvalidPack));
    QCOMPARE(readAll(review_path), before_independent_review);
    QCOMPARE(readAll(manifest_path), before_independent_manifest);

    auto level_two_review = authored_review;
    level_two_review.insert(QStringLiteral("review_state"),
                            QStringLiteral("independent_review_pending"));
    auto level_two_dimensions = level_two_review.value(QStringLiteral("dimensions")).toObject();
    level_two_dimensions.insert(QStringLiteral("procedural_law"), 2);
    level_two_review.insert(QStringLiteral("dimensions"), level_two_dimensions);
    const auto level_two_bytes = jsonBytes(level_two_review);
    QVERIFY(overwriteAll(review_path, level_two_bytes));
    auto level_two_manifest = responseObject(authored_manifest_bytes);
    auto level_two_contents = level_two_manifest.value(QStringLiteral("contents")).toArray();
    for (qsizetype index = 0; index < level_two_contents.size(); ++index) {
        auto descriptor = level_two_contents.at(index).toObject();
        if (descriptor.value(QStringLiteral("id")).toString() ==
            QStringLiteral("example.review.cli-realism")) {
            descriptor.insert(QStringLiteral("sha256"), sha256(level_two_bytes));
            level_two_contents.replace(index, descriptor);
        }
    }
    level_two_manifest.insert(QStringLiteral("contents"), level_two_contents);
    QVERIFY(overwriteAll(manifest_path, jsonBytes(level_two_manifest)));
    const auto before_level_two_review = readAll(review_path);
    const auto before_level_two_manifest = readAll(manifest_path);

    const auto level_two_root = appellate::packs::PackReader::readDirectory(
        pack_directory, appellate::packs::PackValidationScope::ResolvedClosure);
    QVERIFY(level_two_root.has_value());
    auto opened_catalog = appellate::packs::PackCatalog::open(catalog_directory);
    QVERIFY(opened_catalog.has_value());
    const auto dependency =
        (*opened_catalog)
            ->load(
                appellate::model::PackId{
                    dependency_revision.value(QStringLiteral("pack_id")).toString().toStdString()},
                dependency_revision.value(QStringLiteral("version")).toString().toStdString());
    QVERIFY(dependency.has_value());
    const std::array<const appellate::packs::LoadedPack*, 1> dependencies{&*dependency};
    const auto parity = appellate::packs::PackReader::validateResolvedGraph(
        *level_two_root, std::span<const appellate::packs::LoadedPack* const>(dependencies));
    QVERIFY(!parity.has_value());

    const auto level_two =
        runPackCli({QStringLiteral("author-realism-evidence"), pack_directory, catalog_directory,
                    QStringLiteral("example.review.cli-realism"), trace_path});
    QCOMPARE(level_two.exit_code, static_cast<int>(ExitCode::InvalidPack));
    QCOMPARE(readAll(review_path), before_level_two_review);
    QCOMPARE(readAll(manifest_path), before_level_two_manifest);
}

void PackCliTest::authorsMultiTraceRealismEvidenceDeterministically() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto pack_directory =
        QDir(temporary.path()).filePath(QStringLiteral("multi-evidence-pack"));
    const auto catalog_directory = QDir(temporary.path()).filePath(QStringLiteral("catalog"));
    const auto trace_set_path = QDir(temporary.path()).filePath(QStringLiteral("trace-set.json"));
    requireSuccess(runPackCli({QStringLiteral("template"), pack_directory}),
                   QStringLiteral("template"));

    const auto pristine = appellate::packs::PackReader::readDirectory(pack_directory);
    QVERIFY(pristine.has_value());
    const auto all_traces = executedTraces(*pristine, 257);
    QVERIFY(all_traces.has_value());
    QCOMPARE(all_traces->size(), 257);
    const auto trace_zero = all_traces->at(0).toObject();
    const auto trace_one = all_traces->at(1).toObject();

    const QJsonObject dimensions{
        {QStringLiteral("procedural_law"), 2},     {QStringLiteral("deadlines_authority"), 2},
        {QStringLiteral("record_consistency"), 2}, {QStringLiteral("consequences"), 2},
        {QStringLiteral("oral_argument"), 2},      {QStringLiteral("bench_differentiation"), 2},
        {QStringLiteral("provenance"), 2},
    };
    const QJsonObject scaffold{
        {QStringLiteral("schema_version"), 2},
        {QStringLiteral("resource_kind"), QStringLiteral("realism_review")},
        {QStringLiteral("resource_id"), QStringLiteral("example.review.cli-realism-multi")},
        {QStringLiteral("case_id"), QStringLiteral("example.case.fictional")},
        {QStringLiteral("review_state"), QStringLiteral("independent_review_pending")},
        {QStringLiteral("reviewed_on"), QStringLiteral("2026-08-12")},
        {QStringLiteral("reviewer_reference"), QStringLiteral("Multi-trace authoring test")},
        {QStringLiteral("reviewer"),
         QJsonObject{
             {QStringLiteral("reviewer_id"), QStringLiteral("example.reviewer.author")},
             {QStringLiteral("display_name"), QStringLiteral("Example Author")},
             {QStringLiteral("qualification"), QStringLiteral("Repository fixture author")},
             {QStringLiteral("affiliation"), QStringLiteral("Example fixture project")},
         }},
        {QStringLiteral("dimensions"), dimensions},
        {QStringLiteral("known_uncertainty"),
         QJsonArray{QJsonObject{
             {QStringLiteral("uncertainty_id"),
              QStringLiteral("example.uncertainty.cli-realism-multi")},
             {QStringLiteral("summary"), QStringLiteral("Synthetic multi-trace evidence")},
             {QStringLiteral("blocking"), false},
         }}},
    };
    QVERIFY(attachRealismScaffold(pack_directory, scaffold));

    const auto profile = QString::fromLatin1(
        appellate::packs::realism_evidence_multi_trace_authoring_engine_revision.data(),
        static_cast<qsizetype>(
            appellate::packs::realism_evidence_multi_trace_authoring_engine_revision.size()));
    const auto traceBundle = [&](QJsonArray traces) {
        return QJsonObject{
            {QStringLiteral("profile"), profile},
            {QStringLiteral("traces"), std::move(traces)},
        };
    };

    const auto review_path =
        QDir(pack_directory).filePath(QStringLiteral("resources/realism-review.json"));
    const auto manifest_path = QDir(pack_directory).filePath(QStringLiteral("manifest.json"));
    const auto transaction_path =
        QDir(temporary.path())
            .filePath(QStringLiteral(".multi-evidence-pack.author-realism-evidence.transaction"));
    const auto command = QStringLiteral("author-realism-evidence-multi");
    const auto review_id = QStringLiteral("example.review.cli-realism-multi");
    const auto scaffold_review_bytes = readAll(review_path);
    const auto scaffold_manifest_bytes = readAll(manifest_path);

    QVERIFY(
        overwriteAll(trace_set_path, jsonBytes(traceBundle(QJsonArray{trace_zero, trace_one}))));
    const auto legacy_bundle =
        runPackCli({QStringLiteral("author-realism-evidence"), pack_directory, catalog_directory,
                    review_id, trace_set_path});
    QCOMPARE(legacy_bundle.exit_code, static_cast<int>(ExitCode::InvalidPack));
    QVERIFY(legacy_bundle.standard_output.isEmpty());
    QCOMPARE(readAll(review_path), scaffold_review_bytes);
    QCOMPARE(readAll(manifest_path), scaffold_manifest_bytes);

    QVERIFY(overwriteAll(trace_set_path, jsonBytes(traceBundle(QJsonArray{}))));
    const auto empty =
        runPackCli({command, pack_directory, catalog_directory, review_id, trace_set_path});
    QCOMPARE(empty.exit_code, static_cast<int>(ExitCode::InvalidPack));
    QVERIFY(empty.standard_output.isEmpty());
    QCOMPARE(readAll(review_path), scaffold_review_bytes);
    QCOMPARE(readAll(manifest_path), scaffold_manifest_bytes);
    QVERIFY(!QFileInfo::exists(transaction_path));

    QVERIFY(overwriteAll(trace_set_path, jsonBytes(traceBundle(QJsonArray{trace_zero}))));
    const auto single =
        runPackCli({command, pack_directory, catalog_directory, review_id, trace_set_path});
    requireSuccess(single, command);
    const auto single_response = responseObject(single.standard_output);
    QCOMPARE(single_response.value(QStringLiteral("updated")).toBool(), true);
    QCOMPARE(single_response.value(QStringLiteral("evidence_counts"))
                 .toObject()
                 .value(QStringLiteral("traces"))
                 .toInt(),
             1);
    const auto single_review = responseObject(readAll(review_path));
    QCOMPARE(single_review.value(QStringLiteral("dimensions"))
                 .toObject()
                 .value(QStringLiteral("procedural_law"))
                 .toInt(),
             2);
    QCOMPARE(single_review.value(QStringLiteral("evidence"))
                 .toObject()
                 .value(QStringLiteral("traces"))
                 .toArray()
                 .size(),
             1);
    requireSuccess(runPackCli({QStringLiteral("validate"), pack_directory}),
                   QStringLiteral("validate"));

    QJsonArray reverse_traces;
    for (qsizetype index = 255; index >= 0; --index) {
        reverse_traces.push_back(all_traces->at(index));
    }
    QVERIFY(overwriteAll(trace_set_path, jsonBytes(traceBundle(reverse_traces))));
    const auto authored =
        runPackCli({command, pack_directory, catalog_directory, review_id, trace_set_path});
    requireSuccess(authored, command);
    const auto authored_response = responseObject(authored.standard_output);
    QCOMPARE(authored_response.value(QStringLiteral("updated")).toBool(), true);
    QCOMPARE(authored_response.value(QStringLiteral("evidence_counts"))
                 .toObject()
                 .value(QStringLiteral("traces"))
                 .toInt(),
             256);

    const auto authored_review_bytes = readAll(review_path);
    const auto authored_manifest_bytes = readAll(manifest_path);
    const auto evidence =
        responseObject(authored_review_bytes).value(QStringLiteral("evidence")).toObject();
    const auto traces = evidence.value(QStringLiteral("traces")).toArray();
    QCOMPARE(traces.size(), 256);
    for (const auto& value : traces) {
        QCOMPARE(value.toObject().value(QStringLiteral("engine_revision")).toString(), profile);
    }
    const auto dimension_evidence = evidence.value(QStringLiteral("dimension_evidence")).toObject();
    for (const auto& dimension :
         {QStringLiteral("procedural_law"), QStringLiteral("deadlines_authority"),
          QStringLiteral("consequences")}) {
        const auto references = dimension_evidence.value(dimension).toArray();
        QVERIFY(references.size() > 256);
        QVERIFY(references.size() <= 512);
        QVERIFY(references.contains(trace_zero.value(QStringLiteral("evidence_id"))));
        QVERIFY(references.contains(
            all_traces->at(255).toObject().value(QStringLiteral("evidence_id"))));
    }
    requireSuccess(runPackCli({QStringLiteral("validate"), pack_directory}),
                   QStringLiteral("validate"));

    QJsonArray forward_traces;
    for (qsizetype index = 0; index < 256; ++index) {
        forward_traces.push_back(all_traces->at(index));
    }
    QVERIFY(overwriteAll(trace_set_path, jsonBytes(traceBundle(forward_traces))));
    const auto repeated =
        runPackCli({command, pack_directory, catalog_directory, review_id, trace_set_path});
    requireSuccess(repeated, command);
    QCOMPARE(responseObject(repeated.standard_output).value(QStringLiteral("updated")).toBool(),
             false);
    QCOMPARE(readAll(review_path), authored_review_bytes);
    QCOMPARE(readAll(manifest_path), authored_manifest_bytes);

    auto duplicate_evidence_trace = trace_one;
    duplicate_evidence_trace.insert(QStringLiteral("evidence_id"),
                                    trace_zero.value(QStringLiteral("evidence_id")));
    QVERIFY(overwriteAll(trace_set_path,
                         jsonBytes(traceBundle(QJsonArray{trace_zero, duplicate_evidence_trace}))));
    const auto duplicate_evidence =
        runPackCli({command, pack_directory, catalog_directory, review_id, trace_set_path});
    QCOMPARE(duplicate_evidence.exit_code, static_cast<int>(ExitCode::InvalidPack));
    QVERIFY(duplicate_evidence.standard_output.isEmpty());
    QCOMPARE(readAll(review_path), authored_review_bytes);
    QCOMPARE(readAll(manifest_path), authored_manifest_bytes);
    QVERIFY(!QFileInfo::exists(transaction_path));

    auto duplicate_id_trace = trace_one;
    duplicate_id_trace.insert(QStringLiteral("trace_id"),
                              trace_zero.value(QStringLiteral("trace_id")));
    QVERIFY(overwriteAll(trace_set_path,
                         jsonBytes(traceBundle(QJsonArray{trace_zero, duplicate_id_trace}))));
    const auto duplicate_id =
        runPackCli({command, pack_directory, catalog_directory, review_id, trace_set_path});
    QCOMPARE(duplicate_id.exit_code, static_cast<int>(ExitCode::InvalidPack));
    QVERIFY(duplicate_id.standard_output.isEmpty());
    QCOMPARE(readAll(review_path), authored_review_bytes);
    QCOMPARE(readAll(manifest_path), authored_manifest_bytes);
    QVERIFY(!QFileInfo::exists(transaction_path));

    auto tampered_trace = trace_one;
    tampered_trace.insert(QStringLiteral("command_count"), 99);
    QVERIFY(overwriteAll(trace_set_path,
                         jsonBytes(traceBundle(QJsonArray{trace_zero, tampered_trace}))));
    const auto tampered =
        runPackCli({command, pack_directory, catalog_directory, review_id, trace_set_path});
    QCOMPARE(tampered.exit_code, static_cast<int>(ExitCode::InvalidPack));
    QVERIFY(tampered.standard_output.isEmpty());
    QCOMPARE(readAll(review_path), authored_review_bytes);
    QCOMPARE(readAll(manifest_path), authored_manifest_bytes);
    QVERIFY(!QFileInfo::exists(transaction_path));

    QVERIFY(overwriteAll(trace_set_path, jsonBytes(traceBundle(*all_traces))));
    const auto excessive =
        runPackCli({command, pack_directory, catalog_directory, review_id, trace_set_path});
    QCOMPARE(excessive.exit_code, static_cast<int>(ExitCode::InvalidPack));
    QVERIFY(excessive.standard_output.isEmpty());
    QCOMPARE(readAll(review_path), authored_review_bytes);
    QCOMPARE(readAll(manifest_path), authored_manifest_bytes);
    QVERIFY(!QFileInfo::exists(transaction_path));
}

void PackCliTest::preparesIndependentReviewDeterministically() {
    using appellate::model::PackId;
    using appellate::model::PackRevision;
    using appellate::packs::IndependentReviewFinalizeInput;
    using appellate::packs::IndependentReviewPrepareInput;
    using appellate::packs::PackArchive;
    using appellate::packs::PackCatalog;
    using appellate::packs::PackCatalogSnapshot;
    using appellate::packs::PackValidationScope;

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto catalog_path = QDir(temporary.path()).filePath(QStringLiteral("shared-catalog"));
    auto opened_catalog = PackCatalog::open(catalog_path);
    QVERIFY2(opened_catalog.has_value(),
             opened_catalog ? "" : opened_catalog.error().message.toUtf8().constData());
    auto catalog = std::move(*opened_catalog);
    const auto dependency_archives = independentReviewDependencyArchives();
    for (std::size_t index = 0; index < dependency_archives.size(); ++index) {
        const auto installed = catalog->installArchive(dependency_archives.at(index),
                                                       QStringLiteral("2026-08-20T00:00:00Z"));
        QVERIFY2(installed.has_value(),
                 installed ? "" : installed.error().message.toUtf8().constData());
        const auto& fixture = independent_review_dependency_fixtures.at(index);
        QCOMPARE(QString::fromStdString(installed->revision.id.value),
                 QString::fromLatin1(fixture.pack_id));
        QCOMPARE(QString::fromStdString(installed->revision.version),
                 QString::fromLatin1(fixture.version));
        QCOMPARE(QString::fromStdString(installed->revision.digest),
                 QString::fromLatin1(fixture.revision));
    }
    for (const auto& fixture : independent_review_fixtures) {
        const auto archive = sourcePath(fixture.archive_relative_path);
        QCOMPARE(sha256(readAll(archive)), QString::fromLatin1(fixture.archive_sha256));
        const auto installed =
            catalog->installArchive(archive, QStringLiteral("2026-08-20T00:00:00Z"));
        QVERIFY2(installed.has_value(),
                 installed ? "" : installed.error().message.toUtf8().constData());
        QCOMPARE(installed->archive_sha256, QString::fromLatin1(fixture.archive_sha256));
        QCOMPARE(QString::fromStdString(installed->revision.digest),
                 QString::fromLatin1(fixture.revision));
    }
    catalog.reset();

    auto opened_snapshot = PackCatalogSnapshot::openExisting(catalog_path);
    QVERIFY2(opened_snapshot.has_value(),
             opened_snapshot ? "" : opened_snapshot.error().message.toUtf8().constData());
    auto snapshot = std::move(*opened_snapshot);
    const auto list_before = snapshot->list();
    QVERIFY(list_before.has_value());
    QCOMPARE(list_before->size(), std::size_t{12});
    const auto captured_date = QDate(2026, 8, 20);

    for (const auto& fixture : independent_review_fixtures) {
        const auto subject_revision =
            PackRevision{PackId{fixture.pack_id}, fixture.version, fixture.revision};
        const auto subject = snapshot->loadResolved(subject_revision);
        QVERIFY2(subject.has_value(), subject ? "" : subject.error().message.toUtf8().constData());
        const auto* source_review =
            findReview(subject->root(), QString::fromLatin1(fixture.review_id));
        QVERIFY(source_review != nullptr);
        QCOMPARE(source_review->document.value(QStringLiteral("review_state")).toString(),
                 QStringLiteral("independent_review_pending"));
        QCOMPARE(source_review->document.value(QStringLiteral("reviewed_on")).toString(),
                 QString::fromLatin1(fixture.reviewed_on));
        const auto source_dimensions =
            source_review->document.value(QStringLiteral("dimensions")).toObject();
        for (const auto& score : source_dimensions) {
            QCOMPARE(score.toInt(), 2);
        }
        const auto source_traces = source_review->document.value(QStringLiteral("evidence"))
                                       .toObject()
                                       .value(QStringLiteral("traces"))
                                       .toArray();
        QCOMPARE(source_traces.size(), fixture.trace_count);

        const IndependentReviewPrepareInput prepare_input{
            subject_revision, QString::fromLatin1(fixture.case_id), captured_date};
        const auto prepared_a =
            appellate::packs::prepareIndependentReview(*snapshot, prepare_input);
        QVERIFY2(prepared_a.has_value(),
                 prepared_a ? "" : prepared_a.error().message.toUtf8().constData());
        const auto prepared_b =
            appellate::packs::prepareIndependentReview(*snapshot, prepare_input);
        QVERIFY2(prepared_b.has_value(),
                 prepared_b ? "" : prepared_b.error().message.toUtf8().constData());
        QCOMPARE(prepared_a->handoff_bytes, prepared_b->handoff_bytes);
        QCOMPARE(prepared_a->declaration_template_bytes, prepared_b->declaration_template_bytes);
        QCOMPARE(prepared_a->handoff_digest, prepared_b->handoff_digest);
        const auto handoff_payload =
            prepared_a->handoff.value(QStringLiteral("payload")).toObject();
        QCOMPARE(prepared_a->handoff_digest, independentHandoffDigest(handoff_payload));
        QCOMPARE(prepared_a->handoff.value(QStringLiteral("handoff_digest")).toString(),
                 prepared_a->handoff_digest);
        QCOMPARE(handoff_payload.value(QStringLiteral("declaration_template_sha256")).toString(),
                 sha256(prepared_a->declaration_template_bytes));
        QCOMPARE(
            sha256(prepared_a->declaration_template_bytes),
            QStringLiteral("c3749adae144b712301688c86ab5f1d519bcab0b887aecc0a3bde8314409004b"));
        QCOMPARE(prepared_a->source_review_resource_id, QString::fromLatin1(fixture.review_id));
        QCOMPARE(prepared_a->counts.traces, static_cast<std::size_t>(fixture.trace_count));

        const auto detached_traces = prepared_a->handoff.value(QStringLiteral("payload"))
                                         .toObject()
                                         .value(QStringLiteral("mechanical_evidence"))
                                         .toObject()
                                         .value(QStringLiteral("traces"))
                                         .toArray();
        QCOMPARE(detached_traces.size(), source_traces.size());
        for (qsizetype index = 0; index < source_traces.size(); ++index) {
            auto expected = source_traces.at(index).toObject();
            expected.insert(QStringLiteral("engine_revision"),
                            QStringLiteral("appellate.realism-evidence.detached-review-replay.v1"));
            expected.insert(QStringLiteral("digest"),
                            independentTraceDigest(QString::fromLatin1(fixture.case_id), expected));
            QCOMPARE(detached_traces.at(index).toObject(), expected);
        }

        const auto declaration =
            completedIndependentDeclaration(*prepared_a, QString::fromLatin1(fixture.slug));
        const IndependentReviewFinalizeInput finalize_input{
            prepared_a->handoff_bytes,
            prepared_a->declaration_template_bytes,
            jsonBytes(declaration),
            captured_date,
        };
        const auto finalized_a =
            appellate::packs::finalizeIndependentReview(*snapshot, finalize_input);
        QVERIFY2(finalized_a.has_value(),
                 finalized_a ? "" : finalized_a.error().message.toUtf8().constData());
        const auto finalized_b =
            appellate::packs::finalizeIndependentReview(*snapshot, finalize_input);
        QVERIFY2(finalized_b.has_value(),
                 finalized_b ? "" : finalized_b.error().message.toUtf8().constData());
        QCOMPARE(finalized_a->manifest_bytes, finalized_b->manifest_bytes);
        QCOMPARE(finalized_a->review_bytes, finalized_b->review_bytes);
        QVERIFY(finalized_a->revision == finalized_b->revision);
        QCOMPARE(finalized_a->review_document.value(QStringLiteral("review_state")).toString(),
                 QStringLiteral("independently_reviewed"));
        QCOMPARE(finalized_a->review_document.value(QStringLiteral("evidence"))
                     .toObject()
                     .value(QStringLiteral("traces"))
                     .toArray(),
                 detached_traces);

        const auto pack_directory =
            QDir(temporary.path())
                .filePath(QStringLiteral("detached-%1").arg(QString::fromLatin1(fixture.slug)));
        QVERIFY(QDir{}.mkpath(QDir(pack_directory).filePath(QStringLiteral("resources"))));
        QVERIFY(writeNew(QDir(pack_directory).filePath(QStringLiteral("manifest.json")),
                         finalized_a->manifest_bytes));
        QVERIFY(
            writeNew(QDir(pack_directory).filePath(QStringLiteral("resources/realism-review.json")),
                     finalized_a->review_bytes));
        const auto ordinary = appellate::packs::PackReader::readDirectory(
            pack_directory, PackValidationScope::ResolvedClosure);
        QVERIFY2(ordinary.has_value(),
                 ordinary ? "" : ordinary.error().message.toUtf8().constData());
        QVERIFY(ordinary->revision == finalized_a->revision);
        std::vector<const appellate::packs::LoadedPack*> dependencies;
        for (const auto& dependency : subject->dependenciesDependencyFirst()) {
            dependencies.push_back(&dependency);
        }
        dependencies.push_back(&subject->root());
        const auto graph = appellate::packs::PackReader::validateResolvedGraph(
            *ordinary, std::span<const appellate::packs::LoadedPack* const>(dependencies));
        QVERIFY2(graph.has_value(), graph ? "" : graph.error().message.toUtf8().constData());

        const auto verification_catalog_path =
            QDir(temporary.path())
                .filePath(QStringLiteral("verification-%1").arg(QString::fromLatin1(fixture.slug)));
        auto opened_verification = PackCatalog::open(verification_catalog_path);
        QVERIFY(opened_verification.has_value());
        auto verification = std::move(*opened_verification);
        for (const auto& archive : dependency_archives) {
            QVERIFY(verification->installArchive(archive, QStringLiteral("2026-08-20T00:00:00Z"))
                        .has_value());
        }
        QVERIFY(verification
                    ->installArchive(sourcePath(fixture.archive_relative_path),
                                     QStringLiteral("2026-08-20T00:00:00Z"))
                    .has_value());
        const auto detached_archive =
            QDir(temporary.path())
                .filePath(
                    QStringLiteral("detached-%1.awpack").arg(QString::fromLatin1(fixture.slug)));
        const auto exported = PackArchive::exportDirectory(pack_directory, detached_archive, {},
                                                           PackValidationScope::ResolvedClosure);
        QVERIFY(exported.has_value());
        QVERIFY(*exported == finalized_a->revision);
        QVERIFY(
            verification->installArchive(detached_archive, QStringLiteral("2026-08-20T00:00:00Z"))
                .has_value());
        const auto installed_closure = verification->loadResolved(finalized_a->revision);
        QVERIFY(installed_closure.has_value());
        QCOMPARE(installed_closure->revisionsByPackId().size(), std::size_t{5});
    }

#if defined(Q_OS_LINUX)
    const auto& cli_fixture = independent_review_fixtures.back();
    const PackRevision cli_subject{PackId{cli_fixture.pack_id}, cli_fixture.version,
                                   cli_fixture.revision};
    const IndependentReviewPrepareInput cli_input{
        cli_subject, QString::fromLatin1(cli_fixture.case_id), captured_date};
    const auto expected_cli = appellate::packs::prepareIndependentReview(*snapshot, cli_input);
    QVERIFY(expected_cli.has_value());
    const auto handoff_a = QDir(temporary.path()).filePath(QStringLiteral("cli-handoff-a"));
    const auto handoff_b = QDir(temporary.path()).filePath(QStringLiteral("cli-handoff-b"));
    int provider_calls = 0;
    const auto provider = [&provider_calls, captured_date] {
        ++provider_calls;
        return captured_date;
    };
    const auto cli_arguments = [&](const QString& destination) {
        return QStringList{
            QStringLiteral("prepare-independent-review"),
            catalog_path,
            QString::fromLatin1(cli_fixture.pack_id),
            QString::fromLatin1(cli_fixture.version),
            QString::fromLatin1(cli_fixture.revision),
            QString::fromLatin1(cli_fixture.case_id),
            destination,
        };
    };
    const auto prepared_cli_a =
        appellate::cli::detail::runPackCli(cli_arguments(handoff_a), provider);
    const auto prepared_cli_b =
        appellate::cli::detail::runPackCli(cli_arguments(handoff_b), provider);
    requireSuccess(prepared_cli_a, QStringLiteral("prepare-independent-review"));
    requireSuccess(prepared_cli_b, QStringLiteral("prepare-independent-review"));
    QCOMPARE(provider_calls, 2);
    QCOMPARE(prepared_cli_a.standard_output, prepared_cli_b.standard_output);
    const auto response = responseObject(prepared_cli_a.standard_output);
    QCOMPARE(
        response.keys(),
        QStringList({QStringLiteral("case_id"), QStringLiteral("closure_digest"),
                     QStringLiteral("command"), QStringLiteral("evidence_counts"),
                     QStringLiteral("files"), QStringLiteral("handoff_digest"),
                     QStringLiteral("mechanical_trace_revision"), QStringLiteral("schema_version"),
                     QStringLiteral("source_review_resource_id"), QStringLiteral("status"),
                     QStringLiteral("subject_revision")}));
    QCOMPARE(response.value(QStringLiteral("files")).toArray(),
             QJsonArray({QStringLiteral("handoff.json"),
                         QStringLiteral("review-declaration.template.json")}));
    QCOMPARE(response.value(QStringLiteral("subject_revision")).toObject(),
             QJsonObject({{QStringLiteral("digest"), QString::fromLatin1(cli_fixture.revision)},
                          {QStringLiteral("pack_id"), QString::fromLatin1(cli_fixture.pack_id)},
                          {QStringLiteral("version"), QString::fromLatin1(cli_fixture.version)}}));
    const QJsonObject expected_cli_response{
        {QStringLiteral("case_id"), expected_cli->case_id},
        {QStringLiteral("closure_digest"), expected_cli->closure_digest},
        {QStringLiteral("command"), QStringLiteral("prepare-independent-review")},
        {QStringLiteral("evidence_counts"),
         QJsonObject{
             {QStringLiteral("authorities"), static_cast<qint64>(expected_cli->counts.authorities)},
             {QStringLiteral("blobs"), static_cast<qint64>(expected_cli->counts.blobs)},
             {QStringLiteral("packs"), static_cast<qint64>(expected_cli->counts.packs)},
             {QStringLiteral("record_checks"),
              static_cast<qint64>(expected_cli->counts.record_checks)},
             {QStringLiteral("resources"), static_cast<qint64>(expected_cli->counts.resources)},
             {QStringLiteral("traces"), static_cast<qint64>(expected_cli->counts.traces)},
         }},
        {QStringLiteral("files"), QJsonArray{QStringLiteral("handoff.json"),
                                             QStringLiteral("review-declaration.template.json")}},
        {QStringLiteral("handoff_digest"), expected_cli->handoff_digest},
        {QStringLiteral("mechanical_trace_revision"),
         QStringLiteral("appellate.realism-evidence.detached-review-replay.v1")},
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("source_review_resource_id"), expected_cli->source_review_resource_id},
        {QStringLiteral("status"), QStringLiteral("ok")},
        {QStringLiteral("subject_revision"),
         QJsonObject{
             {QStringLiteral("digest"),
              QString::fromStdString(expected_cli->subject_revision.digest)},
             {QStringLiteral("pack_id"),
              QString::fromStdString(expected_cli->subject_revision.id.value)},
             {QStringLiteral("version"),
              QString::fromStdString(expected_cli->subject_revision.version)},
         }},
    };
    QCOMPARE(prepared_cli_a.standard_output,
             QJsonDocument(expected_cli_response).toJson(QJsonDocument::Compact) + '\n');
    const QStringList expected_inventory{QStringLiteral("handoff.json"),
                                         QStringLiteral("review-declaration.template.json")};
    for (const auto& directory : {handoff_a, handoff_b}) {
        QCOMPARE(QDir(directory).entryList(QDir::AllEntries | QDir::Hidden | QDir::System |
                                               QDir::NoDotAndDotDot,
                                           QDir::Name),
                 expected_inventory);
        requireNativeMetadata(directory, S_IFDIR, 0700, 2);
        requireNativeMetadata(QDir(directory).filePath(QStringLiteral("handoff.json")), S_IFREG,
                              0600, 1);
        requireNativeMetadata(
            QDir(directory).filePath(QStringLiteral("review-declaration.template.json")), S_IFREG,
            0600, 1);
        QCOMPARE(readAll(QDir(directory).filePath(QStringLiteral("handoff.json"))),
                 expected_cli->handoff_bytes);
        QCOMPARE(
            readAll(QDir(directory).filePath(QStringLiteral("review-declaration.template.json"))),
            expected_cli->declaration_template_bytes);
        const auto reopened =
            appellate::cli::detail::readIndependentReviewHandoffDirectory(directory);
        QVERIFY(reopened.has_value());
        QCOMPARE(reopened->handoff_bytes, expected_cli->handoff_bytes);
        QCOMPARE(reopened->declaration_template_bytes, expected_cli->declaration_template_bytes);
        QCOMPARE(reopened->protected_entry_count, std::size_t{2});
    }
    QCOMPARE(readAll(QDir(handoff_a).filePath(QStringLiteral("handoff.json"))),
             readAll(QDir(handoff_b).filePath(QStringLiteral("handoff.json"))));
    QCOMPARE(readAll(QDir(handoff_a).filePath(QStringLiteral("review-declaration.template.json"))),
             readAll(QDir(handoff_b).filePath(QStringLiteral("review-declaration.template.json"))));
    const auto staging_residue =
        QDir(temporary.path())
            .entryList({QStringLiteral(".cli-handoff-*.appellate-independent-review-*")},
                       QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    QVERIFY(staging_residue.isEmpty());
#endif

    const auto list_after = snapshot->list();
    QVERIFY(list_after.has_value());
    QCOMPARE(*list_after, *list_before);
}

void PackCliTest::finalizesInterchangeableIndependentReviewHandoffs() {
#if defined(Q_OS_LINUX)
    using appellate::model::PackId;
    using appellate::model::PackRevision;
    using appellate::packs::IndependentReviewPrepareInput;
    using appellate::packs::PackArchive;
    using appellate::packs::PackCatalog;
    using appellate::packs::PackCatalogSnapshot;
    using appellate::packs::PackReader;
    using appellate::packs::PackValidationScope;

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto catalog_path = QDir(temporary.path()).filePath(QStringLiteral("catalog"));
    auto opened_catalog = PackCatalog::open(catalog_path);
    QVERIFY2(opened_catalog.has_value(),
             opened_catalog ? "" : opened_catalog.error().message.toUtf8().constData());
    auto catalog = std::move(*opened_catalog);
    for (const auto& archive : independentReviewDependencyArchives()) {
        const auto installed =
            catalog->installArchive(archive, QStringLiteral("2026-08-20T00:00:00Z"));
        QVERIFY2(installed.has_value(),
                 installed ? "" : installed.error().message.toUtf8().constData());
    }
    const auto& fixture = independent_review_fixtures.back();
    const auto installed_subject = catalog->installArchive(
        sourcePath(fixture.archive_relative_path), QStringLiteral("2026-08-20T00:00:00Z"));
    QVERIFY2(installed_subject.has_value(),
             installed_subject ? "" : installed_subject.error().message.toUtf8().constData());
    const PackRevision subject{PackId{fixture.pack_id}, fixture.version, fixture.revision};
    QVERIFY(installed_subject->revision == subject);
    catalog.reset();

    auto opened_snapshot = PackCatalogSnapshot::openExisting(catalog_path);
    QVERIFY2(opened_snapshot.has_value(),
             opened_snapshot ? "" : opened_snapshot.error().message.toUtf8().constData());
    auto snapshot = std::move(*opened_snapshot);
    const auto prepared = appellate::packs::prepareIndependentReview(
        *snapshot, IndependentReviewPrepareInput{subject, QString::fromLatin1(fixture.case_id),
                                                 QDate(2026, 8, 20)});
    QVERIFY2(prepared.has_value(), prepared ? "" : prepared.error().message.toUtf8().constData());

    const auto handoff_a = QDir(temporary.path()).filePath(QStringLiteral("handoff-a"));
    const auto handoff_b = QDir(temporary.path()).filePath(QStringLiteral("handoff-b"));
    const auto prepare_arguments = [&](const QString& destination) {
        return QStringList{
            QStringLiteral("prepare-independent-review"),
            catalog_path,
            QString::fromLatin1(fixture.pack_id),
            QString::fromLatin1(fixture.version),
            QString::fromLatin1(fixture.revision),
            QString::fromLatin1(fixture.case_id),
            destination,
        };
    };
    int provider_calls = 0;
    const auto provider = [&provider_calls] {
        ++provider_calls;
        return QDate(2026, 8, 20);
    };
    const auto prepared_a =
        appellate::cli::detail::runPackCli(prepare_arguments(handoff_a), provider);
    const auto prepared_b =
        appellate::cli::detail::runPackCli(prepare_arguments(handoff_b), provider);
    requireSuccess(prepared_a, QStringLiteral("prepare-independent-review"));
    requireSuccess(prepared_b, QStringLiteral("prepare-independent-review"));
    QCOMPARE(provider_calls, 2);
    QCOMPARE(prepared_a.standard_output, prepared_b.standard_output);
    QCOMPARE(readAll(QDir(handoff_a).filePath(QStringLiteral("handoff.json"))),
             prepared->handoff_bytes);
    QCOMPARE(readAll(QDir(handoff_b).filePath(QStringLiteral("handoff.json"))),
             prepared->handoff_bytes);
    QCOMPARE(readAll(QDir(handoff_a).filePath(QStringLiteral("review-declaration.template.json"))),
             prepared->declaration_template_bytes);
    QCOMPARE(readAll(QDir(handoff_b).filePath(QStringLiteral("review-declaration.template.json"))),
             prepared->declaration_template_bytes);

    const auto declaration =
        completedIndependentDeclaration(*prepared, QStringLiteral("interchangeable"));
    const auto declaration_path =
        QDir(temporary.path()).filePath(QStringLiteral("declaration.json"));
    QVERIFY(writeNew(declaration_path, jsonBytes(declaration)));
    const auto final_a = QDir(temporary.path()).filePath(QStringLiteral("final-a"));
    const auto final_b = QDir(temporary.path()).filePath(QStringLiteral("final-b"));
    const auto finalize_arguments = [&](const QString& handoff, const QString& destination) {
        return QStringList{QStringLiteral("finalize-independent-review"), handoff, declaration_path,
                           catalog_path, destination};
    };
    const auto finalized_a =
        appellate::cli::detail::runPackCli(finalize_arguments(handoff_a, final_a), provider);
    const auto finalized_b =
        appellate::cli::detail::runPackCli(finalize_arguments(handoff_b, final_b), provider);
    requireSuccess(finalized_a, QStringLiteral("finalize-independent-review"));
    requireSuccess(finalized_b, QStringLiteral("finalize-independent-review"));
    QCOMPARE(provider_calls, 4);
    QCOMPARE(finalized_a.standard_output, finalized_b.standard_output);
    QCOMPARE(readAll(QDir(final_a).filePath(QStringLiteral("manifest.json"))),
             readAll(QDir(final_b).filePath(QStringLiteral("manifest.json"))));
    QCOMPARE(readAll(QDir(final_a).filePath(QStringLiteral("resources/realism-review.json"))),
             readAll(QDir(final_b).filePath(QStringLiteral("resources/realism-review.json"))));

    const auto loaded_a = PackReader::readDirectory(final_a, PackValidationScope::ResolvedClosure);
    const auto loaded_b = PackReader::readDirectory(final_b, PackValidationScope::ResolvedClosure);
    QVERIFY2(loaded_a.has_value(), loaded_a ? "" : loaded_a.error().message.toUtf8().constData());
    QVERIFY2(loaded_b.has_value(), loaded_b ? "" : loaded_b.error().message.toUtf8().constData());
    QVERIFY(loaded_a->revision == loaded_b->revision);
    const auto resolved_subject = snapshot->loadResolved(subject);
    QVERIFY2(resolved_subject.has_value(),
             resolved_subject ? "" : resolved_subject.error().message.toUtf8().constData());
    std::vector<const appellate::packs::LoadedPack*> dependencies;
    for (const auto& dependency : resolved_subject->dependenciesDependencyFirst()) {
        dependencies.push_back(&dependency);
    }
    dependencies.push_back(&resolved_subject->root());
    const auto graph_a = PackReader::validateResolvedGraph(
        *loaded_a, std::span<const appellate::packs::LoadedPack* const>(dependencies));
    const auto graph_b = PackReader::validateResolvedGraph(
        *loaded_b, std::span<const appellate::packs::LoadedPack* const>(dependencies));
    QVERIFY2(graph_a.has_value(), graph_a ? "" : graph_a.error().message.toUtf8().constData());
    QVERIFY2(graph_b.has_value(), graph_b ? "" : graph_b.error().message.toUtf8().constData());

    const auto archive_a = QDir(temporary.path()).filePath(QStringLiteral("final-a.awpack"));
    const auto archive_b = QDir(temporary.path()).filePath(QStringLiteral("final-b.awpack"));
    const auto exported_a =
        PackArchive::exportDirectory(final_a, archive_a, {}, PackValidationScope::ResolvedClosure);
    const auto exported_b =
        PackArchive::exportDirectory(final_b, archive_b, {}, PackValidationScope::ResolvedClosure);
    QVERIFY2(exported_a.has_value(),
             exported_a ? "" : exported_a.error().message.toUtf8().constData());
    QVERIFY2(exported_b.has_value(),
             exported_b ? "" : exported_b.error().message.toUtf8().constData());
    QVERIFY(*exported_a == loaded_a->revision);
    QVERIFY(*exported_b == loaded_b->revision);
    QCOMPARE(readAll(archive_a), readAll(archive_b));

    const auto verification_path =
        QDir(temporary.path()).filePath(QStringLiteral("verification-catalog"));
    auto opened_verification = PackCatalog::open(verification_path);
    QVERIFY2(opened_verification.has_value(),
             opened_verification ? "" : opened_verification.error().message.toUtf8().constData());
    auto verification = std::move(*opened_verification);
    for (const auto& archive : independentReviewDependencyArchives()) {
        QVERIFY(verification->installArchive(archive, QStringLiteral("2026-08-20T00:00:00Z"))
                    .has_value());
    }
    QVERIFY(verification
                ->installArchive(sourcePath(fixture.archive_relative_path),
                                 QStringLiteral("2026-08-20T00:00:00Z"))
                .has_value());
    const auto installed_final =
        verification->installArchive(archive_a, QStringLiteral("2026-08-20T00:00:01Z"));
    QVERIFY2(installed_final.has_value(),
             installed_final ? "" : installed_final.error().message.toUtf8().constData());
    QVERIFY(installed_final->revision == *exported_a);
    const auto installed_closure = verification->loadResolved(*exported_a);
    QVERIFY2(installed_closure.has_value(),
             installed_closure ? "" : installed_closure.error().message.toUtf8().constData());
    QCOMPARE(installed_closure->revisionsByPackId().size(), std::size_t{5});
#endif
}

void PackCliTest::finalizesIndependentReviewDeterministically() {
    using appellate::model::PackId;
    using appellate::model::PackRevision;
    using appellate::packs::IndependentReviewErrorCode;
    using appellate::packs::IndependentReviewFinalizeInput;
    using appellate::packs::IndependentReviewPrepareInput;
    using appellate::packs::PackArchive;
    using appellate::packs::PackCatalog;
    using appellate::packs::PackCatalogSnapshot;

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto catalog_path = QDir(temporary.path()).filePath(QStringLiteral("catalog"));
    auto opened_catalog = PackCatalog::open(catalog_path);
    QVERIFY(opened_catalog.has_value());
    auto catalog = std::move(*opened_catalog);
    for (const auto& archive : independentReviewDependencyArchives()) {
        QVERIFY(
            catalog->installArchive(archive, QStringLiteral("2026-08-20T00:00:00Z")).has_value());
    }
    const auto& fixture = independent_review_fixtures.back();
    const auto score_zero_pack = QDir(temporary.path()).filePath(QStringLiteral("score-zero-pack"));
    QVERIFY(copyTree(sourcePath("content/m4/serrano-waiver/pack-candidate"), score_zero_pack));
    const auto source_review_path = QStringLiteral("resources/realism-review.json");
    auto score_zero_review =
        responseObject(readAll(QDir(score_zero_pack).filePath(source_review_path)));
    QVERIFY(!score_zero_review.isEmpty());
    auto source_dimensions = score_zero_review.value(QStringLiteral("dimensions")).toObject();
    source_dimensions.insert(QStringLiteral("provenance"), 0);
    score_zero_review.insert(QStringLiteral("dimensions"), source_dimensions);
    auto source_evidence = score_zero_review.value(QStringLiteral("evidence")).toObject();
    auto source_dimension_evidence =
        source_evidence.value(QStringLiteral("dimension_evidence")).toObject();
    const auto latent_provenance =
        source_dimension_evidence.value(QStringLiteral("provenance")).toArray();
    QVERIFY(!latent_provenance.isEmpty());
    source_dimension_evidence.insert(QStringLiteral("provenance"), QJsonArray{});
    source_evidence.insert(QStringLiteral("dimension_evidence"), source_dimension_evidence);
    score_zero_review.insert(QStringLiteral("evidence"), source_evidence);
    QVERIFY(replaceResourceAndDigest(score_zero_pack, source_review_path,
                                     QString::fromLatin1(fixture.review_id), score_zero_review));
    const auto score_zero_archive =
        QDir(temporary.path()).filePath(QStringLiteral("score-zero-source.awpack"));
    const auto score_zero_revision =
        PackArchive::exportDirectory(score_zero_pack, score_zero_archive, {},
                                     appellate::packs::PackValidationScope::ResolvedClosure);
    QVERIFY2(score_zero_revision.has_value(),
             score_zero_revision ? "" : score_zero_revision.error().message.toUtf8().constData());
    const auto installed_source =
        catalog->installArchive(score_zero_archive, QStringLiteral("2026-08-20T00:00:00Z"));
    QVERIFY2(installed_source.has_value(),
             installed_source ? "" : installed_source.error().message.toUtf8().constData());
    QVERIFY(installed_source->revision == *score_zero_revision);
    catalog.reset();

    auto opened_snapshot = PackCatalogSnapshot::openExisting(catalog_path);
    QVERIFY(opened_snapshot.has_value());
    auto snapshot = std::move(*opened_snapshot);
    const PackRevision subject_revision = *score_zero_revision;
    const auto prepare_input = IndependentReviewPrepareInput{
        subject_revision, QString::fromLatin1(fixture.case_id), QDate(2026, 8, 20)};
    const auto prepared = appellate::packs::prepareIndependentReview(*snapshot, prepare_input);
    QVERIFY2(prepared.has_value(), prepared ? "" : prepared.error().message.toUtf8().constData());

    const auto prefix = QStringLiteral("test.detached-review.resource.boundary.");
    const auto resource_id_128 = prefix + QString(128 - prefix.toUtf8().size(), u'r');
    QCOMPARE(resource_id_128.toUtf8().size(), 128);
    auto declaration = completedIndependentDeclaration(
        *prepared, QStringLiteral("serrano-boundary"), resource_id_128);
    auto dimensions = declaration.value(QStringLiteral("dimensions")).toObject();
    dimensions.insert(QStringLiteral("provenance"), 3);
    dimensions.insert(QStringLiteral("consequences"), 0);
    declaration.insert(QStringLiteral("dimensions"), dimensions);
    declaration.insert(
        QStringLiteral("known_uncertainty"),
        QJsonArray{
            QJsonObject{
                {QStringLiteral("blocking"), false},
                {QStringLiteral("summary"), QStringLiteral("TEST-ONLY nonblocking uncertainty")},
                {QStringLiteral("uncertainty_id"),
                 QStringLiteral("test.detached-review.uncertainty.nonblocking")},
            },
            QJsonObject{
                {QStringLiteral("blocking"), true},
                {QStringLiteral("remediation_issue"),
                 QStringLiteral("https://example.invalid/review/ISSUE%20ONE")},
                {QStringLiteral("summary"), QStringLiteral("TEST-ONLY blocking uncertainty")},
                {QStringLiteral("uncertainty_id"),
                 QStringLiteral("test.detached-review.uncertainty.blocking")},
            },
        });
    const IndependentReviewFinalizeInput finalize_input{
        prepared->handoff_bytes,
        prepared->declaration_template_bytes,
        jsonBytes(declaration),
        QDate(2026, 8, 20),
    };
    const auto finalized_a = appellate::packs::finalizeIndependentReview(*snapshot, finalize_input);
    QVERIFY2(finalized_a.has_value(),
             finalized_a ? "" : finalized_a.error().message.toUtf8().constData());
    const auto finalized_b = appellate::packs::finalizeIndependentReview(*snapshot, finalize_input);
    QVERIFY(finalized_b.has_value());
    QCOMPARE(finalized_a->manifest_bytes, finalized_b->manifest_bytes);
    QCOMPARE(finalized_a->review_bytes, finalized_b->review_bytes);
    QVERIFY(finalized_a->revision == finalized_b->revision);
    QCOMPARE(finalized_a->review_resource_id, resource_id_128);
    const auto complete_groups = prepared->handoff.value(QStringLiteral("payload"))
                                     .toObject()
                                     .value(QStringLiteral("mechanical_evidence"))
                                     .toObject()
                                     .value(QStringLiteral("dimension_evidence"))
                                     .toObject();
    QCOMPARE(complete_groups.value(QStringLiteral("provenance")).toArray(), latent_provenance);
    const auto final_groups = finalized_a->review_document.value(QStringLiteral("evidence"))
                                  .toObject()
                                  .value(QStringLiteral("dimension_evidence"))
                                  .toObject();
    QVERIFY(!complete_groups.value(QStringLiteral("provenance")).toArray().isEmpty());
    QCOMPARE(final_groups.value(QStringLiteral("provenance")).toArray(),
             complete_groups.value(QStringLiteral("provenance")).toArray());
    QVERIFY(final_groups.value(QStringLiteral("consequences")).toArray().isEmpty());
    const auto reviewer = finalized_a->review_document.value(QStringLiteral("reviewer")).toObject();
    QVERIFY(!reviewer.contains(QStringLiteral("affiliation")));
    QCOMPARE(finalized_a->review_document.value(QStringLiteral("known_uncertainty")).toArray(),
             declaration.value(QStringLiteral("known_uncertainty")).toArray());

#if defined(Q_OS_LINUX)
    const auto cli_handoff =
        QDir(temporary.path()).filePath(QStringLiteral("cli-score-zero-handoff"));
    const auto declaration_path =
        QDir(temporary.path()).filePath(QStringLiteral("completed-declaration.json"));
    const auto cli_pack_a = QDir(temporary.path()).filePath(QStringLiteral("cli-detached-a"));
    const auto cli_pack_b = QDir(temporary.path()).filePath(QStringLiteral("cli-detached-b"));
    int provider_calls = 0;
    const auto provider = [&provider_calls] {
        ++provider_calls;
        return QDate(2026, 8, 20);
    };
    const auto cli_prepare = appellate::cli::detail::runPackCli(
        {QStringLiteral("prepare-independent-review"), catalog_path,
         QString::fromStdString(subject_revision.id.value),
         QString::fromStdString(subject_revision.version),
         QString::fromStdString(subject_revision.digest), QString::fromLatin1(fixture.case_id),
         cli_handoff},
        provider);
    requireSuccess(cli_prepare, QStringLiteral("prepare-independent-review"));
    QCOMPARE(readAll(QDir(cli_handoff).filePath(QStringLiteral("handoff.json"))),
             prepared->handoff_bytes);
    QCOMPARE(
        readAll(QDir(cli_handoff).filePath(QStringLiteral("review-declaration.template.json"))),
        prepared->declaration_template_bytes);
    const auto declaration_bytes = jsonBytes(declaration);
    QVERIFY(writeNew(declaration_path, declaration_bytes));
    const auto reread_declaration =
        appellate::cli::detail::readIndependentReviewDeclaration(declaration_path);
    QVERIFY(reread_declaration.has_value());
    QCOMPARE(*reread_declaration, declaration_bytes);
    const auto finalize_arguments = [&](const QString& destination) {
        return QStringList{QStringLiteral("finalize-independent-review"), cli_handoff,
                           declaration_path, catalog_path, destination};
    };
    const auto finalized_cli_a =
        appellate::cli::detail::runPackCli(finalize_arguments(cli_pack_a), provider);
    const auto finalized_cli_b =
        appellate::cli::detail::runPackCli(finalize_arguments(cli_pack_b), provider);
    requireSuccess(finalized_cli_a, QStringLiteral("finalize-independent-review"));
    requireSuccess(finalized_cli_b, QStringLiteral("finalize-independent-review"));
    QCOMPARE(provider_calls, 3);
    QCOMPARE(finalized_cli_a.standard_output, finalized_cli_b.standard_output);
    const auto response = responseObject(finalized_cli_a.standard_output);
    QCOMPARE(response.keys(),
             QStringList({QStringLiteral("case_id"), QStringLiteral("closure_digest"),
                          QStringLiteral("command"), QStringLiteral("dependency_revision"),
                          QStringLiteral("digest"), QStringLiteral("files"),
                          QStringLiteral("handoff_digest"), QStringLiteral("pack_id"),
                          QStringLiteral("review_resource_id"), QStringLiteral("review_sha256"),
                          QStringLiteral("schema_version"), QStringLiteral("status"),
                          QStringLiteral("version")}));
    QCOMPARE(response.value(QStringLiteral("files")).toArray(),
             QJsonArray({QStringLiteral("manifest.json"),
                         QStringLiteral("resources/realism-review.json")}));
    QCOMPARE(response.value(QStringLiteral("digest")).toString(),
             QString::fromStdString(finalized_a->revision.digest));
    QCOMPARE(response.value(QStringLiteral("pack_id")).toString(),
             QString::fromStdString(finalized_a->revision.id.value));
    QCOMPARE(response.value(QStringLiteral("version")).toString(),
             QString::fromStdString(finalized_a->revision.version));
    const QJsonObject expected_cli_response{
        {QStringLiteral("case_id"), finalized_a->case_id},
        {QStringLiteral("closure_digest"), finalized_a->closure_digest},
        {QStringLiteral("command"), QStringLiteral("finalize-independent-review")},
        {QStringLiteral("dependency_revision"),
         QJsonObject{
             {QStringLiteral("digest"),
              QString::fromStdString(finalized_a->dependency_revision.digest)},
             {QStringLiteral("pack_id"),
              QString::fromStdString(finalized_a->dependency_revision.id.value)},
             {QStringLiteral("version"),
              QString::fromStdString(finalized_a->dependency_revision.version)},
         }},
        {QStringLiteral("digest"), QString::fromStdString(finalized_a->revision.digest)},
        {QStringLiteral("files"), QJsonArray{QStringLiteral("manifest.json"),
                                             QStringLiteral("resources/realism-review.json")}},
        {QStringLiteral("handoff_digest"), finalized_a->handoff_digest},
        {QStringLiteral("pack_id"), QString::fromStdString(finalized_a->revision.id.value)},
        {QStringLiteral("review_resource_id"), finalized_a->review_resource_id},
        {QStringLiteral("review_sha256"), finalized_a->review_sha256},
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("status"), QStringLiteral("ok")},
        {QStringLiteral("version"), QString::fromStdString(finalized_a->revision.version)},
    };
    QCOMPARE(finalized_cli_a.standard_output,
             QJsonDocument(expected_cli_response).toJson(QJsonDocument::Compact) + '\n');
    const QStringList root_inventory{QStringLiteral("manifest.json"), QStringLiteral("resources")};
    for (const auto& directory : {cli_pack_a, cli_pack_b}) {
        QCOMPARE(QDir(directory).entryList(QDir::AllEntries | QDir::Hidden | QDir::System |
                                               QDir::NoDotAndDotDot,
                                           QDir::Name),
                 root_inventory);
        QCOMPARE(
            QDir(QDir(directory).filePath(QStringLiteral("resources")))
                .entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                           QDir::Name),
            QStringList{QStringLiteral("realism-review.json")});
        requireNativeMetadata(directory, S_IFDIR, 0700, 3);
        requireNativeMetadata(QDir(directory).filePath(QStringLiteral("resources")), S_IFDIR, 0700,
                              2);
        requireNativeMetadata(QDir(directory).filePath(QStringLiteral("manifest.json")), S_IFREG,
                              0600, 1);
        requireNativeMetadata(
            QDir(directory).filePath(QStringLiteral("resources/realism-review.json")), S_IFREG,
            0600, 1);
        QCOMPARE(readAll(QDir(directory).filePath(QStringLiteral("manifest.json"))),
                 finalized_a->manifest_bytes);
        QCOMPARE(readAll(QDir(directory).filePath(QStringLiteral("resources/realism-review.json"))),
                 finalized_a->review_bytes);
        const auto ordinary = appellate::packs::PackReader::readDirectory(
            directory, appellate::packs::PackValidationScope::ResolvedClosure);
        QVERIFY(ordinary.has_value());
        QVERIFY(ordinary->revision == finalized_a->revision);
    }
    QCOMPARE(readAll(QDir(cli_pack_a).filePath(QStringLiteral("manifest.json"))),
             readAll(QDir(cli_pack_b).filePath(QStringLiteral("manifest.json"))));
    QCOMPARE(readAll(QDir(cli_pack_a).filePath(QStringLiteral("resources/realism-review.json"))),
             readAll(QDir(cli_pack_b).filePath(QStringLiteral("resources/realism-review.json"))));
    const auto staging_residue =
        QDir(temporary.path())
            .entryList({QStringLiteral(".cli-*.appellate-independent-review-*")},
                       QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    QVERIFY(staging_residue.isEmpty());
#endif

    auto affiliated_declaration = declaration;
    affiliated_declaration.insert(QStringLiteral("review_pack_id"),
                                  QStringLiteral("test.detached-review.serrano-affiliated"));
    affiliated_declaration.insert(
        QStringLiteral("review_resource_id"),
        QStringLiteral("test.detached-review.resource.serrano-affiliated"));
    auto affiliated_reviewer = affiliated_declaration.value(QStringLiteral("reviewer")).toObject();
    affiliated_reviewer.insert(QStringLiteral("affiliation"),
                               QStringLiteral("TEST-ONLY fixture affiliation"));
    affiliated_declaration.insert(QStringLiteral("reviewer"), affiliated_reviewer);
    auto affiliated_input = finalize_input;
    affiliated_input.completed_declaration_bytes = jsonBytes(affiliated_declaration);
    const auto affiliated =
        appellate::packs::finalizeIndependentReview(*snapshot, affiliated_input);
    QVERIFY(affiliated.has_value());
    const auto final_affiliated_reviewer =
        affiliated->review_document.value(QStringLiteral("reviewer")).toObject();
    QCOMPARE(final_affiliated_reviewer.size(), 4);
    QCOMPARE(final_affiliated_reviewer.value(QStringLiteral("affiliation")).toString(),
             QStringLiteral("TEST-ONLY fixture affiliation"));

    const auto too_early = appellate::packs::prepareIndependentReview(
        *snapshot, IndependentReviewPrepareInput{
                       subject_revision, QString::fromLatin1(fixture.case_id), QDate(2026, 8, 18)});
    QVERIFY(!too_early.has_value());
    QCOMPARE(too_early.error().code, IndependentReviewErrorCode::InvalidReviewSource);

    auto wrong_association = declaration;
    wrong_association.insert(QStringLiteral("handoff_digest"), QString(64, u'0'));
    auto wrong_association_input = finalize_input;
    wrong_association_input.completed_declaration_bytes = jsonBytes(wrong_association);
    const auto rejected_association =
        appellate::packs::finalizeIndependentReview(*snapshot, wrong_association_input);
    QVERIFY(!rejected_association.has_value());
    QCOMPARE(rejected_association.error().code, IndependentReviewErrorCode::InvalidDeclaration);

    auto oversized_id = declaration;
    oversized_id.insert(QStringLiteral("review_resource_id"), resource_id_128 + u'r');
    auto oversized_input = finalize_input;
    oversized_input.completed_declaration_bytes = jsonBytes(oversized_id);
    const auto rejected_id =
        appellate::packs::finalizeIndependentReview(*snapshot, oversized_input);
    QVERIFY(!rejected_id.has_value());
    QCOMPARE(rejected_id.error().code, IndependentReviewErrorCode::InvalidDeclaration);

    auto tampered_handoff = prepared->handoff;
    tampered_handoff.insert(QStringLiteral("handoff_digest"), QString(64, u'0'));
    auto tampered_input = finalize_input;
    tampered_input.handoff_bytes = jsonBytes(tampered_handoff);
    const auto rejected_handoff =
        appellate::packs::finalizeIndependentReview(*snapshot, tampered_input);
    QVERIFY(!rejected_handoff.has_value());
    QCOMPARE(rejected_handoff.error().code, IndependentReviewErrorCode::InvalidHandoff);

    auto invalid_template_payload = prepared->handoff.value(QStringLiteral("payload")).toObject();
    invalid_template_payload.insert(QStringLiteral("declaration_template_sha256"),
                                    QString(64, u'0'));
    auto missing_subject =
        invalid_template_payload.value(QStringLiteral("subject_revision")).toObject();
    missing_subject.insert(QStringLiteral("digest"), QString(64, u'0'));
    invalid_template_payload.insert(QStringLiteral("subject_revision"), missing_subject);
    auto invalid_template_handoff = prepared->handoff;
    invalid_template_handoff.insert(QStringLiteral("payload"), invalid_template_payload);
    invalid_template_handoff.insert(QStringLiteral("handoff_digest"),
                                    independentHandoffDigest(invalid_template_payload));
    auto invalid_template_input = finalize_input;
    invalid_template_input.handoff_bytes = jsonBytes(invalid_template_handoff);
    const auto rejected_template =
        appellate::packs::finalizeIndependentReview(*snapshot, invalid_template_input);
    QVERIFY(!rejected_template.has_value());
    QCOMPARE(rejected_template.error().code, IndependentReviewErrorCode::InvalidHandoff);

    auto invalid_revision_payload = prepared->handoff.value(QStringLiteral("payload")).toObject();
    invalid_revision_payload.insert(QStringLiteral("mechanical_trace_revision"),
                                    QStringLiteral("test.invalid.detached-revision"));
    invalid_revision_payload.insert(QStringLiteral("subject_revision"), missing_subject);
    auto invalid_revision_handoff = prepared->handoff;
    invalid_revision_handoff.insert(QStringLiteral("payload"), invalid_revision_payload);
    invalid_revision_handoff.insert(QStringLiteral("handoff_digest"),
                                    independentHandoffDigest(invalid_revision_payload));
    auto invalid_revision_input = finalize_input;
    invalid_revision_input.handoff_bytes = jsonBytes(invalid_revision_handoff);
    const auto rejected_revision =
        appellate::packs::finalizeIndependentReview(*snapshot, invalid_revision_input);
    QVERIFY(!rejected_revision.has_value());
    QCOMPARE(rejected_revision.error().code, IndependentReviewErrorCode::InvalidHandoff);

    auto invalid_subject_payload = prepared->handoff.value(QStringLiteral("payload")).toObject();
    auto invalid_subject =
        invalid_subject_payload.value(QStringLiteral("subject_revision")).toObject();
    invalid_subject.insert(QStringLiteral("pack_id"), QStringLiteral("not_namespaced"));
    invalid_subject_payload.insert(QStringLiteral("subject_revision"), invalid_subject);
    auto invalid_subject_handoff = prepared->handoff;
    invalid_subject_handoff.insert(QStringLiteral("payload"), invalid_subject_payload);
    invalid_subject_handoff.insert(QStringLiteral("handoff_digest"),
                                   independentHandoffDigest(invalid_subject_payload));
    auto invalid_subject_input = finalize_input;
    invalid_subject_input.handoff_bytes = jsonBytes(invalid_subject_handoff);
    const auto rejected_subject =
        appellate::packs::finalizeIndependentReview(*snapshot, invalid_subject_input);
    QVERIFY(!rejected_subject.has_value());
    QCOMPARE(rejected_subject.error().code, IndependentReviewErrorCode::InvalidHandoff);
}

void PackCliTest::enforcesIndependentReviewBoundaries_data() {
    QTest::addColumn<int>("boundary_mode");
    QTest::newRow("core") << 0;
    QTest::newRow("descriptor-9999") << 1;
    QTest::newRow("descriptor-10000") << 2;
}

void PackCliTest::enforcesIndependentReviewBoundaries() {
    QFETCH(int, boundary_mode);
    static_cast<void>(boundary_mode);
    using appellate::cli::detail::IndependentReviewArtifactKind;
    using appellate::cli::detail::IndependentReviewPublicationErrorCode;
    using appellate::packs::ErrorCode;
    using appellate::packs::JsonLimits;
    using appellate::packs::SchemaValidator;
    using appellate::packs::detail::CatalogOperandFailureCode;

    struct ParserBoundary final {
        QString name;
        JsonLimits limits;
    };
    const std::array parser_boundaries{
        ParserBoundary{QStringLiteral("handoff.json"), JsonLimits{64, 500'000}},
        ParserBoundary{QStringLiteral("review-declaration.template.json"), JsonLimits{32, 4'096}},
        ParserBoundary{QStringLiteral("completed declaration"), JsonLimits{32, 4'096}},
        ParserBoundary{QStringLiteral("resources/realism-review.json"), JsonLimits{64, 200'000}},
    };
    for (const auto& boundary : parser_boundaries) {
        const auto exact_depth = SchemaValidator::parseObject(
            nestedJsonObject(boundary.limits.maximum_depth), boundary.name, boundary.limits);
        QVERIFY2(exact_depth.has_value(),
                 exact_depth ? "" : exact_depth.error().message.toUtf8().constData());
        const auto excessive_depth = SchemaValidator::parseObject(
            nestedJsonObject(boundary.limits.maximum_depth + 1), boundary.name, boundary.limits);
        QVERIFY(!excessive_depth.has_value());
        QCOMPARE(excessive_depth.error().code, ErrorCode::InvalidJson);
        QVERIFY(excessive_depth.error().message.contains(
            QStringLiteral("Maximum nesting depth exceeded")));

        const auto exact_values =
            SchemaValidator::parseObject(jsonObjectWithValueCount(boundary.limits.maximum_values),
                                         boundary.name, boundary.limits);
        QVERIFY2(exact_values.has_value(),
                 exact_values ? "" : exact_values.error().message.toUtf8().constData());
        const auto excessive_values = SchemaValidator::parseObject(
            jsonObjectWithValueCount(boundary.limits.maximum_values + 1), boundary.name,
            boundary.limits);
        QVERIFY(!excessive_values.has_value());
        QCOMPARE(excessive_values.error().code, ErrorCode::InvalidJson);
        QVERIFY(excessive_values.error().message.contains(
            QStringLiteral("Maximum JSON value count exceeded")));
    }

    struct StrictParserCase final {
        QByteArray bytes;
        ErrorCode code;
    };
    const std::array strict_parser_cases{
        StrictParserCase{QByteArrayLiteral("{\"key\":1,\"key\":2}"), ErrorCode::DuplicateJsonKey},
        StrictParserCase{QByteArrayLiteral("{\"nested\":{\"key\":1,\"key\":2}}"),
                         ErrorCode::DuplicateJsonKey},
        StrictParserCase{QByteArrayLiteral("{}{}"), ErrorCode::InvalidJson},
        StrictParserCase{QByteArrayLiteral("{} trailing"), ErrorCode::InvalidJson},
    };
    for (const auto& parser_case : strict_parser_cases) {
        const auto parsed = SchemaValidator::parseObject(
            parser_case.bytes, QStringLiteral("strict independent-review representative"),
            JsonLimits{64, 500'000});
        QVERIFY(!parsed.has_value());
        QCOMPARE(parsed.error().code, parser_case.code);
    }

    const auto prepare_arguments = [](const QString& catalog, const QString& destination) {
        return QStringList{
            QStringLiteral("prepare-independent-review"),
            catalog,
            QStringLiteral("test.subject"),
            QStringLiteral("1.0.0"),
            QString(64, u'0'),
            QStringLiteral("test.case"),
            destination,
        };
    };
    const auto finalize_arguments = [](const QString& handoff, const QString& declaration,
                                       const QString& catalog, const QString& destination) {
        return QStringList{QStringLiteral("finalize-independent-review"), handoff, declaration,
                           catalog, destination};
    };

    int provider_calls = 0;
    const auto provider = [&provider_calls] {
        ++provider_calls;
        return QDate(2026, 8, 20);
    };
    const auto expect_invalid_arguments = [&](const QStringList& arguments,
                                              const QString& command) {
        const auto calls_before = provider_calls;
        const auto result = appellate::cli::detail::runPackCli(arguments, provider);
        requireCommandError(result, ExitCode::InvalidArguments, QStringLiteral("invalid_arguments"),
                            command);
        QCOMPARE(provider_calls, calls_before);
    };

    const auto missing_command = appellate::cli::detail::runPackCli({}, provider);
    QCOMPARE(missing_command.exit_code, static_cast<int>(ExitCode::InvalidArguments));
    QVERIFY(missing_command.standard_output.isEmpty());
    const auto missing_command_error = responseObject(missing_command.standard_error);
    QCOMPARE(missing_command_error.keys(),
             QStringList({QStringLiteral("code"), QStringLiteral("message"),
                          QStringLiteral("schema_version"), QStringLiteral("status")}));
    QCOMPARE(missing_command_error.value(QStringLiteral("code")).toString(),
             QStringLiteral("invalid_arguments"));
    QCOMPARE(missing_command_error.value(QStringLiteral("schema_version")).toInt(), 1);
    QCOMPARE(missing_command_error.value(QStringLiteral("status")).toString(),
             QStringLiteral("error"));
    QCOMPARE(missing_command.standard_error,
             QJsonDocument(missing_command_error).toJson(QJsonDocument::Compact) + '\n');
    QCOMPARE(provider_calls, 0);

    for (qsizetype size = 1; size <= 8; ++size) {
        if (size == 7) {
            continue;
        }
        QStringList arguments(size, QStringLiteral("operand"));
        arguments.front() = QStringLiteral("prepare-independent-review");
        expect_invalid_arguments(arguments, QStringLiteral("prepare-independent-review"));
    }
    for (qsizetype size = 1; size <= 7; ++size) {
        if (size == 5) {
            continue;
        }
        QStringList arguments(size, QStringLiteral("operand"));
        arguments.front() = QStringLiteral("finalize-independent-review");
        expect_invalid_arguments(arguments, QStringLiteral("finalize-independent-review"));
    }
    expect_invalid_arguments({QStringLiteral("not-an-independent-review-command")},
                             QStringLiteral("not-an-independent-review-command"));

    const auto valid_prepare =
        prepare_arguments(QStringLiteral("catalog"), QStringLiteral("new-handoff"));
    for (qsizetype index = 1; index < valid_prepare.size(); ++index) {
        auto arguments = valid_prepare;
        arguments[index] = {};
        expect_invalid_arguments(arguments, QStringLiteral("prepare-independent-review"));
    }
    const auto valid_finalize =
        finalize_arguments(QStringLiteral("handoff"), QStringLiteral("declaration.json"),
                           QStringLiteral("catalog"), QStringLiteral("new-pack"));
    for (qsizetype index = 1; index < valid_finalize.size(); ++index) {
        auto arguments = valid_finalize;
        arguments[index] = {};
        expect_invalid_arguments(arguments, QStringLiteral("finalize-independent-review"));
    }

    const std::array invalid_path_fragments{
        QString(1, QChar::Null),
        QString(1, QChar{0xd800}),
        QString(1, QChar{0xdc00}),
        QString(1, QChar::ReplacementCharacter),
    };
    for (const auto& fragment : invalid_path_fragments) {
        const auto invalid_path = QStringLiteral("invalid") + fragment + QStringLiteral("path");
        for (const auto index : {1, 6}) {
            auto arguments = valid_prepare;
            arguments[index] = invalid_path;
            expect_invalid_arguments(arguments, QStringLiteral("prepare-independent-review"));
        }
        for (const auto index : {1, 2, 3, 4}) {
            auto arguments = valid_finalize;
            arguments[index] = invalid_path;
            expect_invalid_arguments(arguments, QStringLiteral("finalize-independent-review"));
        }
    }
    QCOMPARE(provider_calls, 0);

    const auto repeated_path = [](qsizetype count) {
        return QStringList(count, QStringLiteral("p")).join(u'/');
    };
    const auto absolute_ascii_path = [](qsizetype byte_size) {
        QStringList components;
        auto remaining = byte_size - 1;
        while (remaining > 255) {
            components.push_back(QString(255, u'p'));
            remaining -= 256;
        }
        components.push_back(QString(remaining, u'p'));
        return QStringLiteral("/") + components.join(u'/');
    };
    const auto exact_native_path = absolute_ascii_path(4'095);
    const auto oversized_native_path = absolute_ascii_path(4'096);
    QCOMPARE(QFile::encodeName(exact_native_path).size(), 4'095);
    QCOMPARE(QFile::encodeName(oversized_native_path).size(), 4'096);
    QVERIFY(appellate::cli::detail::validateIndependentReviewPathSpelling(exact_native_path, false)
                .has_value());
    QVERIFY(
        !appellate::cli::detail::validateIndependentReviewPathSpelling(oversized_native_path, false)
             .has_value());
    QVERIFY(appellate::cli::detail::validateIndependentReviewPathSpelling(QString(255, u'p'), false)
                .has_value());
    QVERIFY(
        !appellate::cli::detail::validateIndependentReviewPathSpelling(QString(256, u'p'), false)
             .has_value());
    QVERIFY(appellate::cli::detail::validateIndependentReviewPathSpelling(repeated_path(128), false)
                .has_value());
    QVERIFY(
        !appellate::cli::detail::validateIndependentReviewPathSpelling(repeated_path(129), false)
             .has_value());
    for (const auto kind : {IndependentReviewArtifactKind::PreparedHandoff,
                            IndependentReviewArtifactKind::FinalizedPack}) {
        QVERIFY(appellate::cli::detail::validateIndependentReviewDestinationPath(QString(218, u'd'),
                                                                                 kind)
                    .has_value());
        const auto rejected = appellate::cli::detail::validateIndependentReviewDestinationPath(
            QString(219, u'd'), kind);
        QVERIFY(!rejected.has_value());
        QCOMPARE(rejected.error().code, IndependentReviewPublicationErrorCode::InvalidArguments);
    }
    QVERIFY(appellate::cli::detail::validateIndependentReviewDestinationPath(
                repeated_path(127), IndependentReviewArtifactKind::PreparedHandoff)
                .has_value());
    QVERIFY(!appellate::cli::detail::validateIndependentReviewDestinationPath(
                 repeated_path(128), IndependentReviewArtifactKind::PreparedHandoff)
                 .has_value());
    QVERIFY(appellate::cli::detail::validateIndependentReviewDestinationPath(
                repeated_path(126), IndependentReviewArtifactKind::FinalizedPack)
                .has_value());
    QVERIFY(!appellate::cli::detail::validateIndependentReviewDestinationPath(
                 repeated_path(127), IndependentReviewArtifactKind::FinalizedPack)
                 .has_value());
    QVERIFY(appellate::cli::detail::validateIndependentReviewHandoffPath(repeated_path(127))
                .has_value());
    QVERIFY(!appellate::cli::detail::validateIndependentReviewHandoffPath(repeated_path(128))
                 .has_value());
    QVERIFY(appellate::packs::detail::validateCatalogOperandSpelling(QStringLiteral("/") +
                                                                     repeated_path(126))
                .has_value());
    const auto oversized_catalog_spelling =
        appellate::packs::detail::validateCatalogOperandSpelling(QStringLiteral("/") +
                                                                 repeated_path(127));
    QVERIFY(!oversized_catalog_spelling.has_value());
    QCOMPARE(oversized_catalog_spelling.error().code, CatalogOperandFailureCode::InvalidArguments);

    int invalid_date_calls = 0;
    const auto invalid_date_provider = [&invalid_date_calls] {
        ++invalid_date_calls;
        return QDate{};
    };
    const auto expect_date_gate = [&](const QStringList& arguments, const QString& command) {
        const auto calls_before = invalid_date_calls;
        const auto result = appellate::cli::detail::runPackCli(arguments, invalid_date_provider);
        requireCommandError(result, ExitCode::OperationFailed,
                            QStringLiteral("invalid_configuration"), command);
        QCOMPARE(invalid_date_calls, calls_before + 1);
    };
    const auto expect_path_gate = [&](const QStringList& arguments, const QString& command) {
        const auto calls_before = invalid_date_calls;
        const auto result = appellate::cli::detail::runPackCli(arguments, invalid_date_provider);
        requireCommandError(result, ExitCode::InvalidArguments, QStringLiteral("invalid_arguments"),
                            command);
        QCOMPARE(invalid_date_calls, calls_before);
    };

    expect_date_gate(prepare_arguments(QStringLiteral("catalog"), QString(218, u'd')),
                     QStringLiteral("prepare-independent-review"));
    expect_path_gate(prepare_arguments(QStringLiteral("catalog"), QString(219, u'd')),
                     QStringLiteral("prepare-independent-review"));
    expect_date_gate(prepare_arguments(QStringLiteral("catalog"), repeated_path(127)),
                     QStringLiteral("prepare-independent-review"));
    expect_path_gate(prepare_arguments(QStringLiteral("catalog"), repeated_path(128)),
                     QStringLiteral("prepare-independent-review"));
    expect_date_gate(
        prepare_arguments(QStringLiteral("/") + repeated_path(126), QStringLiteral("new-handoff")),
        QStringLiteral("prepare-independent-review"));
    expect_path_gate(
        prepare_arguments(QStringLiteral("/") + repeated_path(127), QStringLiteral("new-handoff")),
        QStringLiteral("prepare-independent-review"));

    expect_date_gate(finalize_arguments(QStringLiteral("handoff"), QString(255, u'p'),
                                        QStringLiteral("catalog"), QStringLiteral("new-pack")),
                     QStringLiteral("finalize-independent-review"));
    expect_path_gate(finalize_arguments(QStringLiteral("handoff"), QString(256, u'p'),
                                        QStringLiteral("catalog"), QStringLiteral("new-pack")),
                     QStringLiteral("finalize-independent-review"));
    expect_date_gate(finalize_arguments(QStringLiteral("handoff"), exact_native_path,
                                        QStringLiteral("catalog"), QStringLiteral("new-pack")),
                     QStringLiteral("finalize-independent-review"));
    expect_path_gate(finalize_arguments(QStringLiteral("handoff"), oversized_native_path,
                                        QStringLiteral("catalog"), QStringLiteral("new-pack")),
                     QStringLiteral("finalize-independent-review"));
    expect_date_gate(finalize_arguments(QStringLiteral("handoff"), repeated_path(128),
                                        QStringLiteral("catalog"), QStringLiteral("new-pack")),
                     QStringLiteral("finalize-independent-review"));
    expect_path_gate(finalize_arguments(QStringLiteral("handoff"), repeated_path(129),
                                        QStringLiteral("catalog"), QStringLiteral("new-pack")),
                     QStringLiteral("finalize-independent-review"));
    expect_date_gate(finalize_arguments(repeated_path(127), QStringLiteral("declaration.json"),
                                        QStringLiteral("catalog"), QStringLiteral("new-pack")),
                     QStringLiteral("finalize-independent-review"));
    expect_path_gate(finalize_arguments(repeated_path(128), QStringLiteral("declaration.json"),
                                        QStringLiteral("catalog"), QStringLiteral("new-pack")),
                     QStringLiteral("finalize-independent-review"));
    expect_date_gate(finalize_arguments(QStringLiteral("handoff"),
                                        QStringLiteral("declaration.json"),
                                        QStringLiteral("catalog"), repeated_path(126)),
                     QStringLiteral("finalize-independent-review"));
    expect_path_gate(finalize_arguments(QStringLiteral("handoff"),
                                        QStringLiteral("declaration.json"),
                                        QStringLiteral("catalog"), repeated_path(127)),
                     QStringLiteral("finalize-independent-review"));
    expect_date_gate(finalize_arguments(QStringLiteral("handoff"),
                                        QStringLiteral("declaration-검증.json"),
                                        QStringLiteral("catalog"), QStringLiteral("new-pack")),
                     QStringLiteral("finalize-independent-review"));

#if defined(Q_OS_LINUX)
    QTemporaryDir relative_environment;
    QVERIFY(relative_environment.isValid());
    auto deep_current_directory = relative_environment.path();
    const auto base_component_count =
        deep_current_directory.sliced(1).split(u'/', Qt::SkipEmptyParts).size();
    QVERIFY(base_component_count < 126);
    for (qsizetype index = base_component_count; index < 126; ++index) {
        deep_current_directory =
            QDir(deep_current_directory).filePath(QStringLiteral("d%1").arg(index));
    }
    QVERIFY(QDir{}.mkpath(deep_current_directory));
    const auto original_current_directory = QDir::currentPath();
    QVERIFY(QDir::setCurrent(deep_current_directory));
    const auto catalog_spelling =
        appellate::packs::detail::validateCatalogOperandSpelling(QStringLiteral("catalog"));
    const auto retained_catalog =
        appellate::packs::detail::retainCatalogOperand(QStringLiteral("catalog"));
    int relative_provider_calls = 0;
    const auto relative_result = appellate::cli::detail::runPackCli(
        prepare_arguments(QStringLiteral("catalog"), QStringLiteral("new-handoff")),
        [&relative_provider_calls] {
            ++relative_provider_calls;
            return QDate(2026, 8, 20);
        });
    const auto restored_current_directory = QDir::setCurrent(original_current_directory);
    QVERIFY(restored_current_directory);
    QVERIFY(catalog_spelling.has_value());
    QVERIFY(!retained_catalog.has_value());
    QCOMPARE(retained_catalog.error().code, CatalogOperandFailureCode::UnsupportedEnvironment);
    QCOMPARE(relative_provider_calls, 1);
    requireCommandError(relative_result, ExitCode::OperationFailed,
                        QStringLiteral("unsupported_authoring_platform"),
                        QStringLiteral("prepare-independent-review"));
    QVERIFY(!QFileInfo::exists(QDir(deep_current_directory).filePath(QStringLiteral("catalog"))));
    QVERIFY(
        !QFileInfo::exists(QDir(deep_current_directory).filePath(QStringLiteral("new-handoff"))));

    QTemporaryDir input_root;
    QVERIFY(input_root.isValid());
    const auto handoff_directory =
        QDir(input_root.path()).filePath(QStringLiteral("valid-handoff"));
    QVERIFY(QDir{}.mkpath(handoff_directory));
    const auto handoff_path = QDir(handoff_directory).filePath(QStringLiteral("handoff.json"));
    const auto template_path =
        QDir(handoff_directory).filePath(QStringLiteral("review-declaration.template.json"));
    const auto declaration_path =
        QDir(input_root.path()).filePath(QStringLiteral("completed-declaration.json"));
    const auto empty_catalog = QDir(input_root.path()).filePath(QStringLiteral("empty-catalog"));
    const auto absent_catalog = QDir(input_root.path()).filePath(QStringLiteral("absent-catalog"));
    QVERIFY(QDir{}.mkpath(empty_catalog));
    QVERIFY(writeNew(handoff_path, QByteArray("{}\n")));
    QVERIFY(writeNew(template_path, QByteArray("{}\n")));
    QVERIFY(writeNew(declaration_path, QByteArray("{}\n")));

    const auto direct_handoff =
        appellate::cli::detail::readIndependentReviewHandoffDirectory(handoff_directory);
    QVERIFY(direct_handoff.has_value());
    QCOMPARE(direct_handoff->handoff_bytes, QByteArray("{}\n"));
    QCOMPARE(direct_handoff->declaration_template_bytes, QByteArray("{}\n"));
    QCOMPARE(direct_handoff->protected_entry_count, std::size_t{2});
    const auto direct_declaration =
        appellate::cli::detail::readIndependentReviewDeclaration(declaration_path);
    QVERIFY(direct_declaration.has_value());
    QCOMPARE(*direct_declaration, QByteArray("{}\n"));

    int input_provider_calls = 0;
    int destination_sequence = 0;
    const auto input_provider = [&input_provider_calls] {
        ++input_provider_calls;
        return input_provider_calls == 1 ? QDate(2026, 8, 20) : QDate(2026, 8, 21);
    };
    struct InputRun final {
        RunResult result;
        QString destination;
        int provider_calls_before{};
    };
    const auto run_input = [&](const QString& handoff, const QString& declaration,
                               const QString& catalog) {
        const auto destination =
            QDir(input_root.path())
                .filePath(QStringLiteral("unused-destination-%1").arg(destination_sequence++));
        const auto calls_before = input_provider_calls;
        const auto result = appellate::cli::detail::runPackCli(
            finalize_arguments(handoff, declaration, catalog, destination), input_provider);
        return InputRun{result, destination, calls_before};
    };
    const auto expect_input_error = [&](const QString& handoff, const QString& declaration,
                                        const QString& code) {
        const auto run = run_input(handoff, declaration, empty_catalog);
        QCOMPARE(input_provider_calls, run.provider_calls_before + 1);
        QVERIFY(!QFileInfo::exists(run.destination));
        requireCommandError(run.result, ExitCode::InvalidPack, code,
                            QStringLiteral("finalize-independent-review"));
    };
    const auto expect_catalog_gate = [&] {
        const auto run = run_input(handoff_directory, declaration_path, empty_catalog);
        QCOMPARE(input_provider_calls, run.provider_calls_before + 1);
        QVERIFY(!QFileInfo::exists(run.destination));
        requireCommandError(run.result, ExitCode::OperationFailed,
                            QStringLiteral("uninitialized_catalog"),
                            QStringLiteral("finalize-independent-review"));
    };
    const auto missing_catalog_run = run_input(handoff_directory, declaration_path, absent_catalog);
    QCOMPARE(input_provider_calls, missing_catalog_run.provider_calls_before + 1);
    QVERIFY(!QFileInfo::exists(missing_catalog_run.destination));
    requireCommandError(missing_catalog_run.result, ExitCode::OperationFailed,
                        QStringLiteral("cannot_open_catalog"),
                        QStringLiteral("finalize-independent-review"));
    QVERIFY(!QFileInfo::exists(absent_catalog));

    expect_input_error(QDir(input_root.path()).filePath(QStringLiteral("missing-handoff")),
                       declaration_path, QStringLiteral("invalid_handoff"));
    const auto extra_path = QDir(handoff_directory).filePath(QStringLiteral("extra.json"));
    QVERIFY(writeNew(extra_path, QByteArray("{}\n")));
    expect_input_error(handoff_directory, declaration_path, QStringLiteral("invalid_handoff"));
    QVERIFY(QFile::remove(extra_path));

    const auto saved_template = readAll(template_path);
    QVERIFY(QFile::remove(template_path));
    expect_input_error(handoff_directory, declaration_path, QStringLiteral("invalid_handoff"));
    QVERIFY(writeNew(template_path, saved_template));

    const auto symlink_handoff =
        QDir(input_root.path()).filePath(QStringLiteral("symlink-handoff"));
    QVERIFY(QDir{}.mkpath(symlink_handoff));
    QVERIFY(writeNew(QDir(symlink_handoff).filePath(QStringLiteral("handoff.json")),
                     QByteArray("{}\n")));
    QCOMPARE(
        ::symlink(
            QFile::encodeName(template_path).constData(),
            QFile::encodeName(
                QDir(symlink_handoff).filePath(QStringLiteral("review-declaration.template.json")))
                .constData()),
        0);
    const auto rejected_symlink_handoff =
        appellate::cli::detail::readIndependentReviewHandoffDirectory(symlink_handoff);
    QVERIFY(!rejected_symlink_handoff.has_value());
    QCOMPARE(rejected_symlink_handoff.error().code,
             IndependentReviewPublicationErrorCode::InvalidStagedArtifact);
    expect_input_error(symlink_handoff, declaration_path, QStringLiteral("invalid_handoff"));

    const auto hardlink_handoff =
        QDir(input_root.path()).filePath(QStringLiteral("hardlink-handoff"));
    QVERIFY(QDir{}.mkpath(hardlink_handoff));
    QVERIFY(writeNew(QDir(hardlink_handoff).filePath(QStringLiteral("handoff.json")),
                     QByteArray("{}\n")));
    const auto hardlink_source =
        QDir(input_root.path()).filePath(QStringLiteral("template-hardlink-source"));
    QVERIFY(writeNew(hardlink_source, QByteArray("{}\n")));
    QCOMPARE(
        ::link(
            QFile::encodeName(hardlink_source).constData(),
            QFile::encodeName(
                QDir(hardlink_handoff).filePath(QStringLiteral("review-declaration.template.json")))
                .constData()),
        0);
    const auto rejected_hardlink_handoff =
        appellate::cli::detail::readIndependentReviewHandoffDirectory(hardlink_handoff);
    QVERIFY(!rejected_hardlink_handoff.has_value());
    QCOMPARE(rejected_hardlink_handoff.error().code,
             IndependentReviewPublicationErrorCode::InvalidStagedArtifact);
    expect_input_error(hardlink_handoff, declaration_path, QStringLiteral("invalid_handoff"));

    expect_input_error(handoff_directory,
                       QDir(input_root.path()).filePath(QStringLiteral("missing-declaration.json")),
                       QStringLiteral("invalid_declaration"));
    const auto declaration_directory =
        QDir(input_root.path()).filePath(QStringLiteral("declaration-directory"));
    QVERIFY(QDir{}.mkpath(declaration_directory));
    expect_input_error(handoff_directory, declaration_directory,
                       QStringLiteral("invalid_declaration"));
    const auto declaration_symlink =
        QDir(input_root.path()).filePath(QStringLiteral("declaration-symlink.json"));
    QCOMPARE(::symlink(QFile::encodeName(declaration_path).constData(),
                       QFile::encodeName(declaration_symlink).constData()),
             0);
    const auto rejected_declaration_symlink =
        appellate::cli::detail::readIndependentReviewDeclaration(declaration_symlink);
    QVERIFY(!rejected_declaration_symlink.has_value());
    QCOMPARE(rejected_declaration_symlink.error().code,
             IndependentReviewPublicationErrorCode::InvalidStagedArtifact);
    expect_input_error(handoff_directory, declaration_symlink,
                       QStringLiteral("invalid_declaration"));
    const auto declaration_hardlink =
        QDir(input_root.path()).filePath(QStringLiteral("declaration-hardlink.json"));
    QCOMPARE(::link(QFile::encodeName(declaration_path).constData(),
                    QFile::encodeName(declaration_hardlink).constData()),
             0);
    const auto rejected_declaration_hardlink =
        appellate::cli::detail::readIndependentReviewDeclaration(declaration_hardlink);
    QVERIFY(!rejected_declaration_hardlink.has_value());
    QCOMPARE(rejected_declaration_hardlink.error().code,
             IndependentReviewPublicationErrorCode::InvalidStagedArtifact);
    expect_input_error(handoff_directory, declaration_hardlink,
                       QStringLiteral("invalid_declaration"));
    QVERIFY(QFile::remove(declaration_hardlink));

    QCOMPARE(::chmod(QFile::encodeName(declaration_path).constData(), 0444), 0);
    QVERIFY(appellate::cli::detail::readIndependentReviewDeclaration(declaration_path).has_value());
    expect_catalog_gate();
    QCOMPARE(::chmod(QFile::encodeName(declaration_path).constData(), 0644), 0);
    QVERIFY(appellate::cli::detail::readIndependentReviewDeclaration(declaration_path).has_value());
    expect_catalog_gate();

    constexpr qsizetype maximum_handoff_bytes = 16 * 1024 * 1024;
    constexpr qsizetype maximum_template_bytes = 1024 * 1024;
    constexpr qsizetype maximum_declaration_bytes = 2 * 1024 * 1024;
    QVERIFY(overwriteAll(handoff_path, QByteArray(maximum_handoff_bytes, ' ')));
    QVERIFY(appellate::cli::detail::readIndependentReviewHandoffDirectory(handoff_directory)
                .has_value());
    expect_catalog_gate();
    QVERIFY(overwriteAll(handoff_path, QByteArray(maximum_handoff_bytes + 1, ' ')));
    const auto rejected_oversized_handoff =
        appellate::cli::detail::readIndependentReviewHandoffDirectory(handoff_directory);
    QVERIFY(!rejected_oversized_handoff.has_value());
    QCOMPARE(rejected_oversized_handoff.error().code,
             IndependentReviewPublicationErrorCode::InvalidStagedArtifact);
    expect_input_error(handoff_directory, declaration_path, QStringLiteral("invalid_handoff"));
    QVERIFY(overwriteAll(handoff_path, QByteArray("{}\n")));

    QVERIFY(overwriteAll(template_path, QByteArray(maximum_template_bytes, ' ')));
    QVERIFY(appellate::cli::detail::readIndependentReviewHandoffDirectory(handoff_directory)
                .has_value());
    expect_catalog_gate();
    QVERIFY(overwriteAll(template_path, QByteArray(maximum_template_bytes + 1, ' ')));
    const auto rejected_oversized_template =
        appellate::cli::detail::readIndependentReviewHandoffDirectory(handoff_directory);
    QVERIFY(!rejected_oversized_template.has_value());
    QCOMPARE(rejected_oversized_template.error().code,
             IndependentReviewPublicationErrorCode::InvalidStagedArtifact);
    expect_input_error(handoff_directory, declaration_path, QStringLiteral("invalid_handoff"));
    QVERIFY(overwriteAll(template_path, QByteArray("{}\n")));

    QVERIFY(overwriteAll(declaration_path, QByteArray(maximum_declaration_bytes, ' ')));
    QVERIFY(appellate::cli::detail::readIndependentReviewDeclaration(declaration_path).has_value());
    expect_catalog_gate();
    QVERIFY(overwriteAll(declaration_path, QByteArray(maximum_declaration_bytes + 1, ' ')));
    const auto rejected_oversized_declaration =
        appellate::cli::detail::readIndependentReviewDeclaration(declaration_path);
    QVERIFY(!rejected_oversized_declaration.has_value());
    QCOMPARE(rejected_oversized_declaration.error().code,
             IndependentReviewPublicationErrorCode::InvalidStagedArtifact);
    expect_input_error(handoff_directory, declaration_path, QStringLiteral("invalid_declaration"));

    using appellate::model::PackId;
    using appellate::model::PackRevision;
    using appellate::packs::IndependentReviewErrorCode;
    using appellate::packs::IndependentReviewFinalizeInput;
    using appellate::packs::IndependentReviewPrepareInput;
    using appellate::packs::PackArchive;
    using appellate::packs::PackCatalog;
    using appellate::packs::PackCatalogSnapshot;
    using appellate::packs::PackReader;
    using appellate::packs::PackValidationScope;

    QTemporaryDir semantic_root;
    QVERIFY(semantic_root.isValid());
    const auto semantic_catalog_path =
        QDir(semantic_root.path()).filePath(QStringLiteral("catalog"));
    auto opened_semantic_catalog = PackCatalog::open(semantic_catalog_path);
    QVERIFY2(opened_semantic_catalog.has_value(),
             opened_semantic_catalog
                 ? ""
                 : opened_semantic_catalog.error().message.toUtf8().constData());
    auto semantic_catalog = std::move(*opened_semantic_catalog);
    for (const auto& archive : independentReviewDependencyArchives()) {
        const auto installed =
            semantic_catalog->installArchive(archive, QStringLiteral("2026-08-20T00:00:00Z"));
        QVERIFY2(installed.has_value(),
                 installed ? "" : installed.error().message.toUtf8().constData());
    }
    const auto& semantic_fixture = independent_review_fixtures.back();
    const auto installed_subject = semantic_catalog->installArchive(
        sourcePath(semantic_fixture.archive_relative_path), QStringLiteral("2026-08-20T00:00:00Z"));
    QVERIFY2(installed_subject.has_value(),
             installed_subject ? "" : installed_subject.error().message.toUtf8().constData());
    const PackRevision semantic_subject{PackId{semantic_fixture.pack_id}, semantic_fixture.version,
                                        semantic_fixture.revision};
    QVERIFY(installed_subject->revision == semantic_subject);

    const auto boundary_template_directory =
        QDir(semantic_root.path()).filePath(QStringLiteral("boundary-template"));
    requireSuccess(runPackCli({QStringLiteral("template"), boundary_template_directory}),
                   QStringLiteral("template"));
    const auto boundary_template = PackReader::readDirectory(boundary_template_directory);
    QVERIFY2(boundary_template.has_value(),
             boundary_template ? "" : boundary_template.error().message.toUtf8().constData());
    const auto boundary_traces = executedTraces(*boundary_template, 257);
    QVERIFY(boundary_traces.has_value());
    QCOMPARE(boundary_traces->size(), 257);
    const auto empty_trace_authoring = appellate::packs::authorRealismEvidence(
        *semantic_catalog,
        appellate::packs::RealismEvidenceTraceSetAuthoringInput{
            boundary_template_directory,
            QStringLiteral("test.boundary.review.empty"),
            {},
            appellate::packs::RealismEvidenceTraceSetProfile::MultiTraceProductionV1});
    QVERIFY(!empty_trace_authoring.has_value());
    QCOMPARE(empty_trace_authoring.error().code,
             appellate::packs::RealismEvidenceAuthoringErrorCode::InvalidInput);
    const auto excessive_trace_authoring = appellate::packs::authorRealismEvidence(
        *semantic_catalog,
        appellate::packs::RealismEvidenceTraceSetAuthoringInput{
            boundary_template_directory, QStringLiteral("test.boundary.review.excessive"),
            *boundary_traces,
            appellate::packs::RealismEvidenceTraceSetProfile::MultiTraceProductionV1});
    QVERIFY(!excessive_trace_authoring.has_value());
    QCOMPARE(excessive_trace_authoring.error().code,
             appellate::packs::RealismEvidenceAuthoringErrorCode::InvalidInput);

    const auto judge_prototype =
        std::ranges::find_if(boundary_template->resources, [](const auto& resource) {
            return resource.descriptor.kind == appellate::model::ResourceKind::JudgeProfile;
        });
    QVERIFY(judge_prototype != boundary_template->resources.end());
    const auto record_prototype =
        std::ranges::find_if(boundary_template->resources, [](const auto& resource) {
            return resource.descriptor.kind == appellate::model::ResourceKind::Record;
        });
    QVERIFY(record_prototype != boundary_template->resources.end());

    const auto export_boundary_directory =
        [&](const QString& fixture_root, const QString& directory,
            const QString& slug) -> std::expected<PackRevision, QString> {
        const auto archive = QDir(fixture_root).filePath(QStringLiteral("%1.awpack").arg(slug));
        const auto exported = PackArchive::exportDirectory(directory, archive, {},
                                                           PackValidationScope::ResolvedClosure);
        if (!exported) {
            return std::unexpected(
                QStringLiteral("Cannot export %1: %2").arg(slug, exported.error().message));
        }
        return *exported;
    };
    const auto install_boundary_directory =
        [&](PackCatalog& catalog, const QString& fixture_root, const QString& directory,
            const QString& slug) -> std::expected<PackRevision, QString> {
        const auto exported = export_boundary_directory(fixture_root, directory, slug);
        if (!exported) {
            return std::unexpected(exported.error());
        }
        const auto archive = QDir(fixture_root).filePath(QStringLiteral("%1.awpack").arg(slug));
        const auto installed =
            catalog.installArchive(archive, QStringLiteral("2026-08-20T00:00:00Z"));
        if (!installed) {
            return std::unexpected(
                QStringLiteral("Cannot install %1: %2").arg(slug, installed.error().message));
        }
        if (installed->revision != *exported) {
            return std::unexpected(QStringLiteral("Installed revision differs for %1").arg(slug));
        }
        return *exported;
    };

    const auto build_filler = [&](PackCatalog* catalog, const QString& fixture_root,
                                  const QString& slug, qsizetype resource_count,
                                  qsizetype blob_count =
                                      0) -> std::expected<PackRevision, QString> {
        const auto directory = QDir(fixture_root).filePath(QStringLiteral("fixture-%1").arg(slug));
        if (!QDir{}.mkpath(directory) ||
            (resource_count > 0 &&
             !QDir{}.mkpath(QDir(directory).filePath(QStringLiteral("resources")))) ||
            (blob_count > 0 &&
             !QDir{}.mkpath(QDir(directory).filePath(QStringLiteral("objects"))))) {
            return std::unexpected(QStringLiteral("Cannot create filler %1").arg(slug));
        }
        const QByteArray blob_bytes("%PDF-1.7\n%%EOF\n");
        const auto blob_digest = sha256(blob_bytes);
        QJsonArray contents;
        for (qsizetype index = 0; index < resource_count; ++index) {
            const auto is_blob_record = blob_count > 0 && index == 0;
            auto document = is_blob_record ? record_prototype->document : judge_prototype->document;
            const auto resource_id = is_blob_record
                                         ? QStringLiteral("test.boundary.record.%1").arg(slug)
                                         : QStringLiteral("test.boundary.judge.%1.j%2")
                                               .arg(slug)
                                               .arg(index, 5, 10, u'0');
            const auto path = is_blob_record
                                  ? QStringLiteral("resources/record.json")
                                  : QStringLiteral("resources/j%1.json").arg(index, 5, 10, u'0');
            document.insert(QStringLiteral("resource_id"), resource_id);
            if (is_blob_record) {
                auto dockets = document.value(QStringLiteral("dockets")).toArray();
                for (qsizetype docket_index = 0; docket_index < dockets.size(); ++docket_index) {
                    auto docket = dockets.at(docket_index).toObject();
                    docket.remove(QStringLiteral("court_id"));
                    dockets.replace(docket_index, docket);
                }
                document.insert(QStringLiteral("dockets"), dockets);
                const auto prototype_entries =
                    record_prototype->document.value(QStringLiteral("docket_entries")).toArray();
                if (prototype_entries.isEmpty()) {
                    return std::unexpected(
                        QStringLiteral("Filler record prototype has no docket entry"));
                }
                const auto prototype_entry = prototype_entries.first().toObject();
                QJsonArray entries;
                for (qsizetype blob_index = 0; blob_index < blob_count; ++blob_index) {
                    auto entry = prototype_entry;
                    entry.remove(QStringLiteral("parent_entry_id"));
                    entry.remove(QStringLiteral("relationship"));
                    entry.insert(QStringLiteral("entry_id"),
                                 QStringLiteral("test.boundary.entry.%1.e%2")
                                     .arg(slug)
                                     .arg(blob_index, 5, 10, u'0'));
                    entry.insert(QStringLiteral("entry_number"), blob_index + 1);
                    entry.insert(QStringLiteral("entry_label"),
                                 QStringLiteral("BOUNDARY-%1").arg(blob_index, 5, 10, u'0'));
                    entry.insert(QStringLiteral("asset_path"),
                                 QStringLiteral("objects/b%1.pdf").arg(blob_index, 5, 10, u'0'));
                    entry.insert(QStringLiteral("asset_sha256"), blob_digest);
                    entries.push_back(entry);
                }
                document.insert(QStringLiteral("docket_entries"), entries);
                document.insert(QStringLiteral("page_anchors"), QJsonArray{});
            }
            const auto bytes = QJsonDocument(document).toJson(QJsonDocument::Compact);
            if (!writeNew(QDir(directory).filePath(path), bytes)) {
                return std::unexpected(
                    QStringLiteral("Cannot write filler %1 resource %2").arg(slug).arg(index));
            }
            contents.push_back(QJsonObject{
                {QStringLiteral("id"), resource_id},
                {QStringLiteral("kind"),
                 is_blob_record ? QStringLiteral("record") : QStringLiteral("judge_profile")},
                {QStringLiteral("schema_version"), 2},
                {QStringLiteral("path"), path},
                {QStringLiteral("sha256"), sha256(bytes)},
            });
        }
        QJsonArray blobs;
        for (qsizetype index = 0; index < blob_count; ++index) {
            const auto path = QStringLiteral("objects/b%1.pdf").arg(index, 5, 10, u'0');
            if (!writeNew(QDir(directory).filePath(path), blob_bytes)) {
                return std::unexpected(
                    QStringLiteral("Cannot write filler %1 blob %2").arg(slug).arg(index));
            }
            blobs.push_back(QJsonObject{
                {QStringLiteral("path"), path},
                {QStringLiteral("media_type"), QStringLiteral("application/pdf")},
                {QStringLiteral("byte_size"), blob_bytes.size()},
                {QStringLiteral("sha256"), blob_digest},
            });
        }
        QJsonArray capabilities{
            QJsonObject{
                {QStringLiteral("id"), QStringLiteral("workbench.pack.declarative-resources")},
                {QStringLiteral("version"), 2},
            },
        };
        if (resource_count > (blob_count > 0 ? 1 : 0)) {
            capabilities.push_back(QJsonObject{
                {QStringLiteral("id"), QStringLiteral("workbench.pack.judge-profile")},
                {QStringLiteral("version"), 2},
            });
            capabilities.push_back(QJsonObject{
                {QStringLiteral("id"), QStringLiteral("workbench.pack.voice-style")},
                {QStringLiteral("version"), 2},
            });
        }
        const QJsonObject manifest{
            {QStringLiteral("schema_version"), 2},
            {QStringLiteral("pack_id"), QStringLiteral("test.boundary.pack.%1").arg(slug)},
            {QStringLiteral("version"), QStringLiteral("1.0.0")},
            {QStringLiteral("required_capabilities"), capabilities},
            {QStringLiteral("dependencies"), QJsonArray{}},
            {QStringLiteral("blobs"), blobs},
            {QStringLiteral("contents"), contents},
        };
        if (!writeNew(QDir(directory).filePath(QStringLiteral("manifest.json")),
                      jsonBytes(manifest))) {
            return std::unexpected(QStringLiteral("Cannot write filler %1 manifest").arg(slug));
        }
        return catalog != nullptr
                   ? install_boundary_directory(*catalog, fixture_root, directory, slug)
                   : export_boundary_directory(fixture_root, directory, slug);
    };

    struct BoundarySubject final {
        QString slug;
        PackRevision revision;
        QString case_id;
        QString review_id;
    };
    const auto build_subject =
        [&](PackCatalog& catalog, const QString& fixture_root, const QString& slug,
            const QJsonArray& traces, const std::vector<PackRevision>& dependencies,
            qsizetype additional_latent_blobs = 0,
            bool zero_provenance = false) -> std::expected<BoundarySubject, QString> {
        const auto directory = QDir(fixture_root).filePath(QStringLiteral("subject-%1").arg(slug));
        if (!copyTree(boundary_template_directory, directory)) {
            return std::unexpected(QStringLiteral("Cannot copy subject %1").arg(slug));
        }
        const auto manifest_path = QDir(directory).filePath(QStringLiteral("manifest.json"));
        auto manifest = responseObject(readAll(manifest_path));
        if (manifest.isEmpty()) {
            return std::unexpected(QStringLiteral("Cannot read subject %1 manifest").arg(slug));
        }
        manifest.insert(QStringLiteral("pack_id"),
                        QStringLiteral("test.boundary.subject.%1").arg(slug));
        manifest.insert(QStringLiteral("version"), QStringLiteral("1.0.0"));
        QJsonArray dependency_values;
        for (const auto& revision : dependencies) {
            dependency_values.push_back(QJsonObject{
                {QStringLiteral("pack_id"), QString::fromStdString(revision.id.value)},
                {QStringLiteral("version"), QString::fromStdString(revision.version)},
                {QStringLiteral("sha256"), QString::fromStdString(revision.digest)},
            });
        }
        manifest.insert(QStringLiteral("dependencies"), dependency_values);
        if (!overwriteAll(manifest_path, jsonBytes(manifest))) {
            return std::unexpected(QStringLiteral("Cannot update subject %1 manifest").arg(slug));
        }

        if (additional_latent_blobs > 0) {
            auto record_document = record_prototype->document;
            auto entries = record_document.value(QStringLiteral("docket_entries")).toArray();
            auto blobs = manifest.value(QStringLiteral("blobs")).toArray();
            if (entries.isEmpty() || blobs.isEmpty()) {
                return std::unexpected(
                    QStringLiteral("Subject %1 record/blob prototype is empty").arg(slug));
            }
            const auto entry_prototype = entries.first().toObject();
            const auto blob_prototype = blobs.first().toObject();
            const auto prototype_path = blob_prototype.value(QStringLiteral("path")).toString();
            for (qsizetype index = 0; index < additional_latent_blobs; ++index) {
                const auto blob_path =
                    QStringLiteral("objects/latent-%1.pdf").arg(index, 4, 10, u'0');
                if (!QFile::copy(QDir(directory).filePath(prototype_path),
                                 QDir(directory).filePath(blob_path))) {
                    return std::unexpected(QStringLiteral("Cannot copy subject %1 latent blob %2")
                                               .arg(slug)
                                               .arg(index));
                }
                auto blob = blob_prototype;
                blob.insert(QStringLiteral("path"), blob_path);
                blobs.push_back(blob);

                auto entry = entry_prototype;
                entry.insert(QStringLiteral("entry_id"),
                             QStringLiteral("test.boundary.record.%1.e%2")
                                 .arg(slug)
                                 .arg(index, 4, 10, u'0'));
                entry.insert(QStringLiteral("entry_number"), 10'000 + index);
                entry.insert(QStringLiteral("entry_label"),
                             QStringLiteral("LATENT-%1").arg(index, 4, 10, u'0'));
                entry.insert(QStringLiteral("asset_path"), blob_path);
                entries.push_back(entry);
            }
            manifest.insert(QStringLiteral("blobs"), blobs);
            if (!overwriteAll(manifest_path, jsonBytes(manifest))) {
                return std::unexpected(
                    QStringLiteral("Cannot update subject %1 blob manifest").arg(slug));
            }
            record_document.insert(QStringLiteral("docket_entries"), entries);
            if (!replaceResourceAndDigest(
                    directory, QString::fromStdString(record_prototype->descriptor.path),
                    QString::fromStdString(record_prototype->descriptor.id), record_document)) {
                return std::unexpected(
                    QStringLiteral("Cannot augment subject %1 record blobs").arg(slug));
            }
        }

        QJsonObject dimensions;
        for (const auto& name :
             {QStringLiteral("procedural_law"), QStringLiteral("deadlines_authority"),
              QStringLiteral("record_consistency"), QStringLiteral("consequences"),
              QStringLiteral("oral_argument"), QStringLiteral("bench_differentiation"),
              QStringLiteral("provenance")}) {
            dimensions.insert(name,
                              name == QStringLiteral("provenance") && zero_provenance ? 0 : 2);
        }
        const auto review_id = QStringLiteral("test.boundary.review.%1").arg(slug);
        const QJsonObject scaffold{
            {QStringLiteral("schema_version"), 2},
            {QStringLiteral("resource_kind"), QStringLiteral("realism_review")},
            {QStringLiteral("resource_id"), review_id},
            {QStringLiteral("case_id"), QStringLiteral("example.case.fictional")},
            {QStringLiteral("review_state"), QStringLiteral("independent_review_pending")},
            {QStringLiteral("reviewed_on"), QStringLiteral("2026-08-20")},
            {QStringLiteral("reviewer_reference"), QStringLiteral("TEST-ONLY boundary source")},
            {QStringLiteral("reviewer"),
             QJsonObject{
                 {QStringLiteral("reviewer_id"), QStringLiteral("test.boundary.source-reviewer")},
                 {QStringLiteral("display_name"), QStringLiteral("TEST-ONLY source reviewer")},
                 {QStringLiteral("qualification"), QStringLiteral("Synthetic boundary fixture")},
                 {QStringLiteral("affiliation"), QStringLiteral("TEST-ONLY fixture project")},
             }},
            {QStringLiteral("dimensions"), dimensions},
            {QStringLiteral("known_uncertainty"), QJsonArray{}},
        };
        if (!attachRealismScaffold(directory, scaffold)) {
            return std::unexpected(QStringLiteral("Cannot scaffold subject %1").arg(slug));
        }
        const auto authored = appellate::packs::authorRealismEvidence(
            catalog, appellate::packs::RealismEvidenceTraceSetAuthoringInput{
                         directory, review_id, traces,
                         appellate::packs::RealismEvidenceTraceSetProfile::MultiTraceProductionV1});
        if (!authored) {
            return std::unexpected(
                QStringLiteral("Cannot author subject %1: %2").arg(slug, authored.error().message));
        }
        if (!overwriteAll(QDir(directory).filePath(authored->review_path),
                          authored->review_bytes) ||
            !overwriteAll(manifest_path, authored->manifest_bytes)) {
            return std::unexpected(QStringLiteral("Cannot publish subject %1 bytes").arg(slug));
        }
        const auto revision = install_boundary_directory(catalog, fixture_root, directory,
                                                         QStringLiteral("source-%1").arg(slug));
        if (!revision) {
            return std::unexpected(revision.error());
        }
        if (*revision != authored->root_revision) {
            return std::unexpected(QStringLiteral("Authored revision differs for %1").arg(slug));
        }
        return BoundarySubject{slug, *revision, QStringLiteral("example.case.fictional"),
                               review_id};
    };

    const QJsonArray one_trace{boundary_traces->first()};
    QJsonArray traces_256;
    for (qsizetype index = 255; index >= 0; --index) {
        traces_256.push_back(boundary_traces->at(index));
    }
    const auto trace_one_subject = build_subject(*semantic_catalog, semantic_root.path(),
                                                 QStringLiteral("trace-1"), one_trace, {});
    QVERIFY2(trace_one_subject.has_value(),
             trace_one_subject ? "" : trace_one_subject.error().toUtf8().constData());
    const auto loaded_trace_one = semantic_catalog->loadResolved(trace_one_subject->revision);
    QVERIFY2(loaded_trace_one.has_value(),
             loaded_trace_one ? "" : loaded_trace_one.error().message.toUtf8().constData());
    const auto* trace_one_review =
        findReview(loaded_trace_one->root(), trace_one_subject->review_id);
    QVERIFY(trace_one_review != nullptr);
    const auto base_provenance = trace_one_review->document.value(QStringLiteral("evidence"))
                                     .toObject()
                                     .value(QStringLiteral("dimension_evidence"))
                                     .toObject()
                                     .value(QStringLiteral("provenance"))
                                     .toArray()
                                     .size();
    QVERIFY(base_provenance > 0);
    QVERIFY(base_provenance < 512);

    const auto trace_256_subject = build_subject(*semantic_catalog, semantic_root.path(),
                                                 QStringLiteral("trace-256"), traces_256, {});
    QVERIFY2(trace_256_subject.has_value(),
             trace_256_subject ? "" : trace_256_subject.error().toUtf8().constData());
    const auto latent_512_subject =
        build_subject(*semantic_catalog, semantic_root.path(), QStringLiteral("latent-512"),
                      one_trace, {}, 512 - base_provenance, true);
    QVERIFY2(latent_512_subject.has_value(),
             latent_512_subject ? "" : latent_512_subject.error().toUtf8().constData());
    const auto latent_513_subject =
        build_subject(*semantic_catalog, semantic_root.path(), QStringLiteral("latent-513"),
                      one_trace, {}, 513 - base_provenance, true);
    QVERIFY2(latent_513_subject.has_value(),
             latent_513_subject ? "" : latent_513_subject.error().toUtf8().constData());

    semantic_catalog.reset();

    auto opened_semantic_snapshot = PackCatalogSnapshot::openExisting(semantic_catalog_path);
    QVERIFY2(opened_semantic_snapshot.has_value(),
             opened_semantic_snapshot
                 ? ""
                 : opened_semantic_snapshot.error().message.toUtf8().constData());
    auto semantic_snapshot = std::move(*opened_semantic_snapshot);

    struct SeedArchive final {
        PackRevision revision;
        QString path;
    };
    const auto empty_blob_set_digest = [](const PackRevision& revision) {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        addFrame(hash, QStringLiteral("appellate-workbench-catalog-blob-set-v1"));
        addFrame(hash, QString::fromStdString(revision.id.value));
        addFrame(hash, QString::fromStdString(revision.version));
        addFrame(hash, QString::fromStdString(revision.digest));
        addUint64(hash, 0);
        return QString::fromLatin1(hash.result().toHex());
    };
    const auto seed_zero_blob_archives =
        [&](const QString& catalog_root,
            const std::vector<SeedArchive>& archives) -> std::expected<void, QString> {
        const auto connection_name = QStringLiteral("independent-boundary-seed-%1")
                                         .arg(QUuid::createUuid().toString(QUuid::Id128));
        bool succeeded = true;
        {
            auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name);
            database.setDatabaseName(QDir(catalog_root).filePath(QStringLiteral("catalog.sqlite")));
            succeeded = database.open();
            QSqlQuery foreign_keys(database);
            succeeded = succeeded && foreign_keys.exec(QStringLiteral("PRAGMA foreign_keys = ON"));
            QSqlQuery transaction(database);
            succeeded = succeeded && transaction.exec(QStringLiteral("BEGIN IMMEDIATE"));
            for (const auto& archive : archives) {
                if (!succeeded) {
                    break;
                }
                const auto archive_bytes = readAll(archive.path);
                const auto archive_sha = sha256(archive_bytes);
                const auto destination =
                    QDir(catalog_root)
                        .filePath(QStringLiteral("archives/%1.awpack").arg(archive_sha));
                if (archive_bytes.isEmpty() ||
                    (!QFileInfo::exists(destination) && !QFile::copy(archive.path, destination))) {
                    succeeded = false;
                    break;
                }
                QSqlQuery pack(database);
                pack.prepare(QStringLiteral(
                    "INSERT INTO pack_revisions(pack_id, version, digest, archive_sha256, "
                    "installed_at_utc) VALUES(?, ?, ?, ?, ?)"));
                pack.addBindValue(QString::fromStdString(archive.revision.id.value));
                pack.addBindValue(QString::fromStdString(archive.revision.version));
                pack.addBindValue(QString::fromStdString(archive.revision.digest));
                pack.addBindValue(archive_sha);
                pack.addBindValue(QStringLiteral("2026-08-20T00:00:00Z"));
                succeeded = pack.exec();
                QSqlQuery blob_set(database);
                blob_set.prepare(QStringLiteral(
                    "INSERT INTO pack_blob_sets(pack_id, version, blob_count, descriptor_sha256) "
                    "VALUES(?, ?, 0, ?)"));
                blob_set.addBindValue(QString::fromStdString(archive.revision.id.value));
                blob_set.addBindValue(QString::fromStdString(archive.revision.version));
                blob_set.addBindValue(empty_blob_set_digest(archive.revision));
                succeeded = succeeded && blob_set.exec();
            }
            QSqlQuery finish(database);
            succeeded = succeeded && finish.exec(QStringLiteral("COMMIT"));
            if (!succeeded) {
                QSqlQuery rollback(database);
                static_cast<void>(rollback.exec(QStringLiteral("ROLLBACK")));
            }
            database.close();
        }
        QSqlDatabase::removeDatabase(connection_name);
        if (!succeeded) {
            return std::unexpected(QStringLiteral("Cannot bulk-seed revision boundary catalog"));
        }
        return {};
    };

    QTemporaryDir revision_root;
    QVERIFY(revision_root.isValid());
    const auto revision_catalog_path =
        QDir(revision_root.path()).filePath(QStringLiteral("catalog"));
    {
        auto initialized = PackCatalog::open(revision_catalog_path);
        QVERIFY2(initialized.has_value(),
                 initialized ? "" : initialized.error().message.toUtf8().constData());
    }
    std::vector<PackRevision> revision_fillers;
    std::vector<SeedArchive> revision_seed_archives;
    revision_fillers.reserve(127);
    revision_seed_archives.reserve(127);
    for (qsizetype index = 0; index < 127; ++index) {
        const auto slug = QStringLiteral("revision-filler-%1").arg(index, 3, 10, u'0');
        const auto filler = build_filler(nullptr, revision_root.path(), slug, 1);
        QVERIFY2(filler.has_value(), filler ? "" : filler.error().toUtf8().constData());
        revision_fillers.push_back(*filler);
        revision_seed_archives.push_back(SeedArchive{
            *filler, QDir(revision_root.path()).filePath(QStringLiteral("%1.awpack").arg(slug))});
    }
    const auto seeded_revisions =
        seed_zero_blob_archives(revision_catalog_path, revision_seed_archives);
    QVERIFY2(seeded_revisions.has_value(),
             seeded_revisions ? "" : seeded_revisions.error().toUtf8().constData());
    auto opened_revision_catalog = PackCatalog::open(revision_catalog_path);
    QVERIFY2(opened_revision_catalog.has_value(),
             opened_revision_catalog
                 ? ""
                 : opened_revision_catalog.error().message.toUtf8().constData());
    auto revision_catalog = std::move(*opened_revision_catalog);
    const std::vector<PackRevision> revisions_126(revision_fillers.begin(),
                                                  revision_fillers.begin() + 126);
    const auto revision_127_subject =
        build_subject(*revision_catalog, revision_root.path(), QStringLiteral("revisions-127"),
                      one_trace, revisions_126);
    QVERIFY2(revision_127_subject.has_value(),
             revision_127_subject ? "" : revision_127_subject.error().toUtf8().constData());
    const auto revision_128_subject =
        build_subject(*revision_catalog, revision_root.path(), QStringLiteral("revisions-128"),
                      one_trace, revision_fillers);
    QVERIFY2(revision_128_subject.has_value(),
             revision_128_subject ? "" : revision_128_subject.error().toUtf8().constData());
    revision_catalog.reset();
    auto opened_revision_snapshot = PackCatalogSnapshot::openExisting(revision_catalog_path);
    QVERIFY2(opened_revision_snapshot.has_value(),
             opened_revision_snapshot
                 ? ""
                 : opened_revision_snapshot.error().message.toUtf8().constData());
    auto revision_snapshot = std::move(*opened_revision_snapshot);

    QTemporaryDir descriptor_root;
    QVERIFY(descriptor_root.isValid());
    const auto descriptor_catalog_path =
        QDir(descriptor_root.path()).filePath(QStringLiteral("catalog"));
    auto opened_descriptor_catalog = PackCatalog::open(descriptor_catalog_path);
    QVERIFY2(opened_descriptor_catalog.has_value(),
             opened_descriptor_catalog
                 ? ""
                 : opened_descriptor_catalog.error().message.toUtf8().constData());
    auto descriptor_catalog = std::move(*opened_descriptor_catalog);
    const auto subject_descriptor_count = static_cast<qsizetype>(
        boundary_template->resources.size() + 1U + boundary_template->blobs.size());
    QVERIFY(subject_descriptor_count < 9'999);
    std::optional<BoundarySubject> descriptor_subject;
    if (boundary_mode != 0) {
        std::vector<PackRevision> descriptor_fillers;
        descriptor_fillers.reserve(5);
        auto remaining_descriptors = 9'999 - subject_descriptor_count;
        for (qsizetype index = 0; index < 4; ++index) {
            const auto filler_count = remaining_descriptors / (4 - index);
            const auto filler =
                build_filler(descriptor_catalog.get(), descriptor_root.path(),
                             QStringLiteral("descriptor-large-%1").arg(index), 1, filler_count - 1);
            QVERIFY2(filler.has_value(), filler ? "" : filler.error().toUtf8().constData());
            descriptor_fillers.push_back(*filler);
            remaining_descriptors -= filler_count;
        }
        QCOMPARE(remaining_descriptors, qsizetype{0});
        if (boundary_mode == 2) {
            const auto descriptor_plus_one =
                build_filler(descriptor_catalog.get(), descriptor_root.path(),
                             QStringLiteral("descriptor-plus-one"), 1);
            QVERIFY2(descriptor_plus_one.has_value(),
                     descriptor_plus_one ? "" : descriptor_plus_one.error().toUtf8().constData());
            descriptor_fillers.push_back(*descriptor_plus_one);
        }
        const auto built = build_subject(*descriptor_catalog, descriptor_root.path(),
                                         boundary_mode == 2 ? QStringLiteral("descriptors-10000")
                                                            : QStringLiteral("descriptors-9999"),
                                         one_trace, descriptor_fillers);
        QVERIFY2(built.has_value(), built ? "" : built.error().toUtf8().constData());
        descriptor_subject = *built;
    }
    descriptor_catalog.reset();
    auto opened_descriptor_snapshot = PackCatalogSnapshot::openExisting(descriptor_catalog_path);
    QVERIFY2(opened_descriptor_snapshot.has_value(),
             opened_descriptor_snapshot
                 ? ""
                 : opened_descriptor_snapshot.error().message.toUtf8().constData());
    auto descriptor_snapshot = std::move(*opened_descriptor_snapshot);

    const auto boundary_descriptor_count = [](const appellate::packs::ResolvedPack& resolved) {
        std::size_t count = resolved.root().resources.size() + resolved.root().blobs.size();
        for (const auto& dependency : resolved.dependenciesDependencyFirst()) {
            count += dependency.resources.size() + dependency.blobs.size();
        }
        return count;
    };
    const auto exercise_boundary_success =
        [&](PackCatalogSnapshot& boundary_snapshot, const QString& boundary_catalog_path,
            const QString& boundary_root, const BoundarySubject& subject,
            qsizetype expected_trace_count, std::size_t expected_revision_count = 0,
            std::size_t expected_descriptor_count = 0,
            qsizetype expected_latent_provenance = -1) -> void {
        const auto resolved = boundary_snapshot.loadResolved(subject.revision);
        QVERIFY2(resolved.has_value(),
                 resolved ? "" : resolved.error().message.toUtf8().constData());
        if (expected_revision_count != 0) {
            QCOMPARE(resolved->revisionsByPackId().size(), expected_revision_count);
        }
        if (expected_descriptor_count != 0) {
            QCOMPARE(boundary_descriptor_count(*resolved), expected_descriptor_count);
        }
        const auto* source_review = findReview(resolved->root(), subject.review_id);
        QVERIFY(source_review != nullptr);
        const auto source_evidence =
            source_review->document.value(QStringLiteral("evidence")).toObject();
        const auto source_traces = source_evidence.value(QStringLiteral("traces")).toArray();
        QCOMPARE(source_traces.size(), expected_trace_count);
        std::optional<std::pair<QString, QString>> previous_trace_key;
        for (const auto& value : source_traces) {
            const auto trace = value.toObject();
            const auto key = std::pair{trace.value(QStringLiteral("trace_id")).toString(),
                                       trace.value(QStringLiteral("evidence_id")).toString()};
            QVERIFY(!previous_trace_key.has_value() || *previous_trace_key < key);
            previous_trace_key = key;
            QCOMPARE(trace.value(QStringLiteral("engine_revision")).toString(),
                     QStringLiteral("appellate.realism-evidence.codec-replay-multi.v1"));
        }
        if (expected_latent_provenance >= 0) {
            QCOMPARE(source_review->document.value(QStringLiteral("dimensions"))
                         .toObject()
                         .value(QStringLiteral("provenance"))
                         .toInt(),
                     0);
            QVERIFY(source_evidence.value(QStringLiteral("dimension_evidence"))
                        .toObject()
                        .value(QStringLiteral("provenance"))
                        .toArray()
                        .isEmpty());
        }

        const auto prepared_boundary = appellate::packs::prepareIndependentReview(
            boundary_snapshot,
            IndependentReviewPrepareInput{subject.revision, subject.case_id, QDate(2026, 8, 20)});
        QVERIFY2(prepared_boundary.has_value(),
                 prepared_boundary ? "" : prepared_boundary.error().message.toUtf8().constData());
        QCOMPARE(prepared_boundary->counts.traces, static_cast<std::size_t>(expected_trace_count));
        if (expected_revision_count != 0) {
            QCOMPARE(prepared_boundary->counts.packs, expected_revision_count);
        }
        const auto mechanical_evidence = prepared_boundary->handoff.value(QStringLiteral("payload"))
                                             .toObject()
                                             .value(QStringLiteral("mechanical_evidence"))
                                             .toObject();
        const auto detached_traces = mechanical_evidence.value(QStringLiteral("traces")).toArray();
        QCOMPARE(detached_traces.size(), expected_trace_count);
        for (qsizetype index = 0; index < source_traces.size(); ++index) {
            const auto source_trace = source_traces.at(index).toObject();
            auto expected_trace = source_trace;
            expected_trace.insert(
                QStringLiteral("engine_revision"),
                QStringLiteral("appellate.realism-evidence.detached-review-replay.v1"));
            expected_trace.insert(QStringLiteral("digest"),
                                  independentTraceDigest(subject.case_id, expected_trace));
            const auto detached_trace = detached_traces.at(index).toObject();
            QCOMPARE(detached_trace, expected_trace);
            QCOMPARE(detached_trace.value(QStringLiteral("evidence_id")),
                     source_trace.value(QStringLiteral("evidence_id")));
            QCOMPARE(detached_trace.value(QStringLiteral("trace_id")),
                     source_trace.value(QStringLiteral("trace_id")));
            QCOMPARE(detached_trace.value(QStringLiteral("journal")),
                     source_trace.value(QStringLiteral("journal")));
        }
        if (expected_latent_provenance >= 0) {
            QCOMPARE(mechanical_evidence.value(QStringLiteral("dimension_evidence"))
                         .toObject()
                         .value(QStringLiteral("provenance"))
                         .toArray()
                         .size(),
                     expected_latent_provenance);
        }

        const auto boundary_declaration =
            completedIndependentDeclaration(*prepared_boundary, subject.slug);
        const IndependentReviewFinalizeInput boundary_finalize_input{
            prepared_boundary->handoff_bytes,
            prepared_boundary->declaration_template_bytes,
            jsonBytes(boundary_declaration),
            QDate(2026, 8, 20),
        };
        const auto finalized_boundary =
            appellate::packs::finalizeIndependentReview(boundary_snapshot, boundary_finalize_input);
        QVERIFY2(finalized_boundary.has_value(),
                 finalized_boundary ? "" : finalized_boundary.error().message.toUtf8().constData());
        QVERIFY(finalized_boundary->dependency_revision == subject.revision);
        QCOMPARE(finalized_boundary->review_document.value(QStringLiteral("evidence"))
                     .toObject()
                     .value(QStringLiteral("traces"))
                     .toArray(),
                 detached_traces);
        if (expected_latent_provenance >= 0) {
            QCOMPARE(finalized_boundary->review_document.value(QStringLiteral("evidence"))
                         .toObject()
                         .value(QStringLiteral("dimension_evidence"))
                         .toObject()
                         .value(QStringLiteral("provenance"))
                         .toArray()
                         .size(),
                     expected_latent_provenance);
        }

        const auto cli_handoff =
            QDir(boundary_root).filePath(QStringLiteral("boundary-handoff-%1").arg(subject.slug));
        const auto cli_declaration =
            QDir(boundary_root)
                .filePath(QStringLiteral("boundary-declaration-%1.json").arg(subject.slug));
        const auto cli_final =
            QDir(boundary_root).filePath(QStringLiteral("boundary-final-%1").arg(subject.slug));
        int prepare_calls = 0;
        const auto cli_prepare = appellate::cli::detail::runPackCli(
            {QStringLiteral("prepare-independent-review"), boundary_catalog_path,
             QString::fromStdString(subject.revision.id.value),
             QString::fromStdString(subject.revision.version),
             QString::fromStdString(subject.revision.digest), subject.case_id, cli_handoff},
            [&prepare_calls] {
                ++prepare_calls;
                return QDate(2026, 8, 20);
            });
        requireSuccess(cli_prepare, QStringLiteral("prepare-independent-review"));
        QCOMPARE(prepare_calls, 1);
        QCOMPARE(responseObject(cli_prepare.standard_output)
                     .value(QStringLiteral("evidence_counts"))
                     .toObject()
                     .value(QStringLiteral("traces"))
                     .toInt(),
                 expected_trace_count);
        QCOMPARE(readAll(QDir(cli_handoff).filePath(QStringLiteral("handoff.json"))),
                 prepared_boundary->handoff_bytes);
        QCOMPARE(
            readAll(QDir(cli_handoff).filePath(QStringLiteral("review-declaration.template.json"))),
            prepared_boundary->declaration_template_bytes);
        QVERIFY(writeNew(cli_declaration, jsonBytes(boundary_declaration)));
        int finalize_calls = 0;
        const auto cli_finalize = appellate::cli::detail::runPackCli(
            finalize_arguments(cli_handoff, cli_declaration, boundary_catalog_path, cli_final),
            [&finalize_calls] {
                ++finalize_calls;
                return QDate(2026, 8, 20);
            });
        requireSuccess(cli_finalize, QStringLiteral("finalize-independent-review"));
        QCOMPARE(finalize_calls, 1);
        QCOMPARE(readAll(QDir(cli_final).filePath(QStringLiteral("manifest.json"))),
                 finalized_boundary->manifest_bytes);
        QCOMPARE(readAll(QDir(cli_final).filePath(QStringLiteral("resources/realism-review.json"))),
                 finalized_boundary->review_bytes);
    };

    const auto exercise_boundary_rejection =
        [&](PackCatalogSnapshot& boundary_snapshot, const QString& boundary_catalog_path,
            const QString& boundary_root, const BoundarySubject& subject,
            const QString& message_fragment, std::size_t expected_revision_count = 0,
            std::size_t expected_descriptor_count = 0) -> void {
        const auto resolved = boundary_snapshot.loadResolved(subject.revision);
        QVERIFY2(resolved.has_value(),
                 resolved ? "" : resolved.error().message.toUtf8().constData());
        if (expected_revision_count != 0) {
            QCOMPARE(resolved->revisionsByPackId().size(), expected_revision_count);
        }
        if (expected_descriptor_count != 0) {
            QCOMPARE(boundary_descriptor_count(*resolved), expected_descriptor_count);
        }
        const auto rejected = appellate::packs::prepareIndependentReview(
            boundary_snapshot,
            IndependentReviewPrepareInput{subject.revision, subject.case_id, QDate(2026, 8, 20)});
        QVERIFY(!rejected.has_value());
        QCOMPARE(rejected.error().code, IndependentReviewErrorCode::InvalidReviewSource);
        QVERIFY2(rejected.error().message.contains(message_fragment),
                 rejected.error().message.toUtf8().constData());

        const auto destination =
            QDir(boundary_root).filePath(QStringLiteral("boundary-rejected-%1").arg(subject.slug));
        int rejection_provider_calls = 0;
        const auto cli_rejected = appellate::cli::detail::runPackCli(
            {QStringLiteral("prepare-independent-review"), boundary_catalog_path,
             QString::fromStdString(subject.revision.id.value),
             QString::fromStdString(subject.revision.version),
             QString::fromStdString(subject.revision.digest), subject.case_id, destination},
            [&rejection_provider_calls] {
                ++rejection_provider_calls;
                return QDate(2026, 8, 20);
            });
        QCOMPARE(rejection_provider_calls, 1);
        QVERIFY(!QFileInfo::exists(destination));
        requireCommandError(cli_rejected, ExitCode::InvalidPack,
                            QStringLiteral("invalid_review_source"),
                            QStringLiteral("prepare-independent-review"));
        QVERIFY(responseObject(cli_rejected.standard_error)
                    .value(QStringLiteral("message"))
                    .toString()
                    .contains(message_fragment));
    };

    if (boundary_mode == 2) {
        QVERIFY(descriptor_subject.has_value());
        exercise_boundary_rejection(*descriptor_snapshot, descriptor_catalog_path,
                                    descriptor_root.path(), *descriptor_subject,
                                    QStringLiteral("descriptor headroom"), 6, 10'000);
        return;
    }
    if (boundary_mode == 1) {
        QVERIFY(descriptor_subject.has_value());
        exercise_boundary_success(*descriptor_snapshot, descriptor_catalog_path,
                                  descriptor_root.path(), *descriptor_subject, 1, 5, 9'999);
        return;
    }

    exercise_boundary_success(*semantic_snapshot, semantic_catalog_path, semantic_root.path(),
                              *trace_one_subject, 1);
    exercise_boundary_success(*semantic_snapshot, semantic_catalog_path, semantic_root.path(),
                              *trace_256_subject, 256);
    exercise_boundary_success(*semantic_snapshot, semantic_catalog_path, semantic_root.path(),
                              *latent_512_subject, 1, 0, 0, 512);
    exercise_boundary_rejection(*semantic_snapshot, semantic_catalog_path, semantic_root.path(),
                                *latent_513_subject, QStringLiteral("exceeds 512 references"));
    exercise_boundary_success(*revision_snapshot, revision_catalog_path, revision_root.path(),
                              *revision_127_subject, 1, 127,
                              static_cast<std::size_t>(subject_descriptor_count + 126));
    exercise_boundary_rejection(*revision_snapshot, revision_catalog_path, revision_root.path(),
                                *revision_128_subject, QStringLiteral("revision headroom"), 128,
                                static_cast<std::size_t>(subject_descriptor_count + 127));
    const auto prepared = appellate::packs::prepareIndependentReview(
        *semantic_snapshot,
        IndependentReviewPrepareInput{
            semantic_subject, QString::fromLatin1(semantic_fixture.case_id), QDate(2026, 8, 20)});
    QVERIFY2(prepared.has_value(), prepared ? "" : prepared.error().message.toUtf8().constData());
    auto declaration = completedIndependentDeclaration(*prepared, QStringLiteral("boundary"));
    const IndependentReviewFinalizeInput baseline_input{
        prepared->handoff_bytes,
        prepared->declaration_template_bytes,
        jsonBytes(declaration),
        QDate(2026, 8, 20),
    };
    const auto baseline =
        appellate::packs::finalizeIndependentReview(*semantic_snapshot, baseline_input);
    QVERIFY2(baseline.has_value(), baseline ? "" : baseline.error().message.toUtf8().constData());

    const auto require_finalize_error = [&](const IndependentReviewFinalizeInput& input,
                                            IndependentReviewErrorCode code,
                                            const QString& message_fragment = {}) -> void {
        const auto finalized =
            appellate::packs::finalizeIndependentReview(*semantic_snapshot, input);
        QVERIFY(!finalized.has_value());
        QCOMPARE(finalized.error().code, code);
        if (!message_fragment.isEmpty()) {
            QVERIFY2(finalized.error().message.contains(message_fragment),
                     finalized.error().message.toUtf8().constData());
        }
    };
    QCOMPARE(baseline->review_document.value(QStringLiteral("reviewed_on")).toString(),
             QStringLiteral("2026-08-20"));
    auto source_date_declaration = declaration;
    source_date_declaration.insert(QStringLiteral("reviewed_on"),
                                   QString::fromLatin1(semantic_fixture.reviewed_on));
    auto source_date_input = baseline_input;
    source_date_input.completed_declaration_bytes = jsonBytes(source_date_declaration);
    const auto source_date_final =
        appellate::packs::finalizeIndependentReview(*semantic_snapshot, source_date_input);
    QVERIFY2(source_date_final.has_value(),
             source_date_final ? "" : source_date_final.error().message.toUtf8().constData());
    QCOMPARE(source_date_final->review_document.value(QStringLiteral("reviewed_on")).toString(),
             QString::fromLatin1(semantic_fixture.reviewed_on));
    auto future_date_declaration = declaration;
    future_date_declaration.insert(QStringLiteral("reviewed_on"), QStringLiteral("2026-08-21"));
    auto future_date_input = baseline_input;
    future_date_input.completed_declaration_bytes = jsonBytes(future_date_declaration);
    require_finalize_error(future_date_input, IndependentReviewErrorCode::InvalidDeclaration,
                           QStringLiteral("source/current UTC bounds"));

    const std::array<QByteArray, 7> strict_artifact_cases{
        QByteArrayLiteral("{\"key\":1,\"key\":2}"),
        QByteArrayLiteral("{\"nested\":{\"key\":1,\"key\":2}}"),
        QByteArray("{\"text\":\"") + QByteArray::fromHex("c328") + QByteArrayLiteral("\"}"),
        QByteArrayLiteral("{}{}"),
        QByteArrayLiteral("{\"text\":\"\\ud800\"}"),
        QByteArrayLiteral("{\"text\":\"\\udc00\"}"),
        QByteArrayLiteral("{\"nested\":{\"\\ud800\":1}}"),
    };
    for (const auto& bytes : strict_artifact_cases) {
        auto handoff_input = baseline_input;
        handoff_input.handoff_bytes = bytes;
        require_finalize_error(handoff_input, IndependentReviewErrorCode::InvalidHandoff);

        auto template_input = baseline_input;
        template_input.declaration_template_bytes = bytes;
        require_finalize_error(template_input, IndependentReviewErrorCode::InvalidHandoff);

        auto declaration_input = baseline_input;
        declaration_input.completed_declaration_bytes = bytes;
        require_finalize_error(declaration_input, IndependentReviewErrorCode::InvalidDeclaration);
    }

    auto noncanonical_handoff = baseline_input;
    noncanonical_handoff.handoff_bytes.append(' ');
    require_finalize_error(noncanonical_handoff, IndependentReviewErrorCode::InvalidHandoff,
                           QStringLiteral("canonical"));
    noncanonical_handoff.handoff_bytes =
        QJsonDocument(prepared->handoff).toJson(QJsonDocument::Compact);
    require_finalize_error(noncanonical_handoff, IndependentReviewErrorCode::InvalidHandoff,
                           QStringLiteral("canonical"));

    auto noncanonical_template = baseline_input;
    noncanonical_template.declaration_template_bytes.append(' ');
    require_finalize_error(noncanonical_template, IndependentReviewErrorCode::InvalidHandoff,
                           QStringLiteral("canonical"));
    noncanonical_template.declaration_template_bytes =
        QJsonDocument(prepared->declaration_template).toJson(QJsonDocument::Compact);
    require_finalize_error(noncanonical_template, IndependentReviewErrorCode::InvalidHandoff,
                           QStringLiteral("canonical"));

    auto compact_declaration_input = baseline_input;
    compact_declaration_input.completed_declaration_bytes =
        QJsonDocument(declaration).toJson(QJsonDocument::Compact);
    const auto compact_declaration =
        appellate::packs::finalizeIndependentReview(*semantic_snapshot, compact_declaration_input);
    QVERIFY2(compact_declaration.has_value(),
             compact_declaration ? "" : compact_declaration.error().message.toUtf8().constData());
    QCOMPARE(compact_declaration->review_bytes, baseline->review_bytes);

    auto scalar_declaration = declaration;
    scalar_declaration.insert(QStringLiteral("reviewer_reference"),
                              QStringLiteral("TEST-ONLY scalar marker"));
    auto scalar_declaration_bytes =
        QJsonDocument(scalar_declaration).toJson(QJsonDocument::Compact);
    QVERIFY(scalar_declaration_bytes.contains("TEST-ONLY scalar marker"));
    scalar_declaration_bytes.replace("TEST-ONLY scalar marker", "TEST-ONLY scalar \\ud83d\\ude00");
    auto scalar_input = baseline_input;
    scalar_input.completed_declaration_bytes = scalar_declaration_bytes;
    const auto scalar_final =
        appellate::packs::finalizeIndependentReview(*semantic_snapshot, scalar_input);
    QVERIFY2(scalar_final.has_value(),
             scalar_final ? "" : scalar_final.error().message.toUtf8().constData());
    const auto supplementary_text = QString::fromUtf8("TEST-ONLY scalar \xf0\x9f\x98\x80");
    QCOMPARE(scalar_final->review_document.value(QStringLiteral("reviewer_reference")).toString(),
             supplementary_text);
    QVERIFY(scalar_final->review_bytes.contains("\xf0\x9f\x98\x80"));

    auto exact_handoff_input = baseline_input;
    exact_handoff_input.handoff_bytes =
        paddedJsonBytes(prepared->handoff_bytes, maximum_handoff_bytes);
    require_finalize_error(exact_handoff_input, IndependentReviewErrorCode::InvalidHandoff,
                           QStringLiteral("canonical"));
    exact_handoff_input.handoff_bytes =
        paddedJsonBytes(prepared->handoff_bytes, maximum_handoff_bytes + 1);
    require_finalize_error(exact_handoff_input, IndependentReviewErrorCode::InvalidHandoff,
                           QStringLiteral("exceeds"));

    auto exact_template_input = baseline_input;
    exact_template_input.declaration_template_bytes =
        paddedJsonBytes(prepared->declaration_template_bytes, maximum_template_bytes);
    require_finalize_error(exact_template_input, IndependentReviewErrorCode::InvalidHandoff,
                           QStringLiteral("canonical"));
    exact_template_input.declaration_template_bytes =
        paddedJsonBytes(prepared->declaration_template_bytes, maximum_template_bytes + 1);
    require_finalize_error(exact_template_input, IndependentReviewErrorCode::InvalidHandoff,
                           QStringLiteral("exceeds"));

    auto exact_declaration_input = baseline_input;
    exact_declaration_input.completed_declaration_bytes =
        paddedJsonBytes(jsonBytes(declaration), maximum_declaration_bytes);
    const auto exact_declaration =
        appellate::packs::finalizeIndependentReview(*semantic_snapshot, exact_declaration_input);
    QVERIFY2(exact_declaration.has_value(),
             exact_declaration ? "" : exact_declaration.error().message.toUtf8().constData());
    QCOMPARE(exact_declaration->review_bytes, baseline->review_bytes);
    exact_declaration_input.completed_declaration_bytes =
        paddedJsonBytes(jsonBytes(declaration), maximum_declaration_bytes + 1);
    require_finalize_error(exact_declaration_input, IndependentReviewErrorCode::InvalidDeclaration,
                           QStringLiteral("exceeds"));

    const auto uncertainties = [](qsizetype count) {
        QJsonArray values;
        for (qsizetype index = 0; index < count; ++index) {
            values.push_back(QJsonObject{
                {QStringLiteral("blocking"), false},
                {QStringLiteral("summary"), QStringLiteral("x")},
                {QStringLiteral("uncertainty_id"),
                 QStringLiteral("test.detached-review.uncertainty.u%1").arg(index, 3, 10, u'0')},
            });
        }
        return values;
    };
    QCOMPARE(baseline->review_document.value(QStringLiteral("known_uncertainty")).toArray().size(),
             0);
    auto declaration_256 = declaration;
    declaration_256.insert(QStringLiteral("known_uncertainty"), uncertainties(256));
    auto input_256 = baseline_input;
    input_256.completed_declaration_bytes = jsonBytes(declaration_256);
    const auto finalized_256 =
        appellate::packs::finalizeIndependentReview(*semantic_snapshot, input_256);
    QVERIFY2(finalized_256.has_value(),
             finalized_256 ? "" : finalized_256.error().message.toUtf8().constData());
    QCOMPARE(finalized_256->review_document.value(QStringLiteral("known_uncertainty")).toArray(),
             uncertainties(256));
    auto declaration_257 = declaration;
    declaration_257.insert(QStringLiteral("known_uncertainty"), uncertainties(257));
    auto input_257 = baseline_input;
    input_257.completed_declaration_bytes = jsonBytes(declaration_257);
    require_finalize_error(input_257, IndependentReviewErrorCode::InvalidDeclaration);

    const auto write_final_pack = [](const QString& directory,
                                     const appellate::packs::FinalizedIndependentReview& final) {
        return QDir{}.mkpath(QDir(directory).filePath(QStringLiteral("resources"))) &&
               writeNew(QDir(directory).filePath(QStringLiteral("manifest.json")),
                        final.manifest_bytes) &&
               writeNew(QDir(directory).filePath(QStringLiteral("resources/realism-review.json")),
                        final.review_bytes);
    };
    const auto natural_pack_path =
        QDir(semantic_root.path()).filePath(QStringLiteral("natural-final-pack"));
    QVERIFY(write_final_pack(natural_pack_path, *baseline));
    const auto natural_pack =
        PackReader::readDirectory(natural_pack_path, PackValidationScope::ResolvedClosure);
    QVERIFY2(natural_pack.has_value(),
             natural_pack ? "" : natural_pack.error().message.toUtf8().constData());

    const auto uncertainty_pack_path =
        QDir(semantic_root.path()).filePath(QStringLiteral("uncertainty-final-pack"));
    QVERIFY(write_final_pack(uncertainty_pack_path, *finalized_256));
    const auto uncertainty_pack =
        PackReader::readDirectory(uncertainty_pack_path, PackValidationScope::ResolvedClosure);
    QVERIFY2(uncertainty_pack.has_value(),
             uncertainty_pack ? "" : uncertainty_pack.error().message.toUtf8().constData());
    const auto resolved_subject = semantic_snapshot->loadResolved(semantic_subject);
    QVERIFY2(resolved_subject.has_value(),
             resolved_subject ? "" : resolved_subject.error().message.toUtf8().constData());
    std::vector<const appellate::packs::LoadedPack*> final_dependencies;
    for (const auto& dependency : resolved_subject->dependenciesDependencyFirst()) {
        final_dependencies.push_back(&dependency);
    }
    final_dependencies.push_back(&resolved_subject->root());
    const auto resolved_final = PackReader::validateResolvedGraph(
        *uncertainty_pack,
        std::span<const appellate::packs::LoadedPack* const>(final_dependencies));
    QVERIFY2(resolved_final.has_value(),
             resolved_final ? "" : resolved_final.error().message.toUtf8().constData());

    const auto raw_pack_path =
        QDir(semantic_root.path()).filePath(QStringLiteral("raw-final-pack"));
    QVERIFY(write_final_pack(raw_pack_path, *baseline));
    const auto write_raw_review = [&](const QByteArray& bytes) {
        if (!overwriteAll(
                QDir(raw_pack_path).filePath(QStringLiteral("resources/realism-review.json")),
                bytes)) {
            return false;
        }
        auto manifest = baseline->manifest;
        auto contents = manifest.value(QStringLiteral("contents")).toArray();
        qsizetype matches = 0;
        for (qsizetype index = 0; index < contents.size(); ++index) {
            auto descriptor = contents.at(index).toObject();
            if (descriptor.value(QStringLiteral("id")).toString() != baseline->review_resource_id) {
                continue;
            }
            ++matches;
            descriptor.insert(QStringLiteral("sha256"), sha256(bytes));
            contents.replace(index, descriptor);
        }
        manifest.insert(QStringLiteral("contents"), contents);
        return matches == 1 &&
               overwriteAll(QDir(raw_pack_path).filePath(QStringLiteral("manifest.json")),
                            jsonBytes(manifest));
    };
    const auto read_raw_review = [&] {
        return PackReader::readDirectory(raw_pack_path, PackValidationScope::ResolvedClosure);
    };
    constexpr qsizetype maximum_final_review_bytes = 8 * 1024 * 1024;
    QVERIFY(write_raw_review(paddedJsonObject(maximum_final_review_bytes)));
    const auto exact_final_bytes = read_raw_review();
    QVERIFY(!exact_final_bytes.has_value());
    QCOMPARE(exact_final_bytes.error().code, ErrorCode::SchemaViolation);
    QVERIFY(write_raw_review(paddedJsonObject(maximum_final_review_bytes + 1)));
    const auto excessive_final_bytes = read_raw_review();
    QVERIFY(!excessive_final_bytes.has_value());
    QCOMPARE(excessive_final_bytes.error().code, ErrorCode::ResourceTooLarge);

    QVERIFY(write_raw_review(nestedJsonObject(64)));
    const auto exact_final_depth = read_raw_review();
    QVERIFY(!exact_final_depth.has_value());
    QCOMPARE(exact_final_depth.error().code, ErrorCode::SchemaViolation);
    QVERIFY(write_raw_review(nestedJsonObject(65)));
    const auto excessive_final_depth = read_raw_review();
    QVERIFY(!excessive_final_depth.has_value());
    QCOMPARE(excessive_final_depth.error().code, ErrorCode::InvalidJson);

    QVERIFY(write_raw_review(jsonObjectWithValueCount(200'000)));
    const auto exact_final_values = read_raw_review();
    QVERIFY(!exact_final_values.has_value());
    QCOMPARE(exact_final_values.error().code, ErrorCode::SchemaViolation);
    QVERIFY(write_raw_review(jsonObjectWithValueCount(200'001)));
    const auto excessive_final_values = read_raw_review();
    QVERIFY(!excessive_final_values.has_value());
    QCOMPARE(excessive_final_values.error().code, ErrorCode::InvalidJson);

    const auto exported_uncertainty_archive =
        QDir(semantic_root.path()).filePath(QStringLiteral("uncertainty-final.awpack"));
    const auto exported_uncertainty =
        PackArchive::exportDirectory(uncertainty_pack_path, exported_uncertainty_archive, {},
                                     PackValidationScope::ResolvedClosure);
    QVERIFY2(exported_uncertainty.has_value(),
             exported_uncertainty ? "" : exported_uncertainty.error().message.toUtf8().constData());
    QVERIFY(*exported_uncertainty == finalized_256->revision);

    const auto cli_handoff_path =
        QDir(semantic_root.path()).filePath(QStringLiteral("clock-handoff"));
    const auto cli_declaration_path =
        QDir(semantic_root.path()).filePath(QStringLiteral("clock-declaration.json"));
    const auto cli_final_path =
        QDir(semantic_root.path()).filePath(QStringLiteral("clock-final-pack"));
    ScopedTimeZone hostile_time_zone;
    hostile_time_zone.set(QByteArrayLiteral("Pacific/Kiritimati"));
    int prepare_clock_calls = 0;
    const auto hostile_prepare = appellate::cli::detail::runPackCli(
        {QStringLiteral("prepare-independent-review"), semantic_catalog_path,
         QString::fromLatin1(semantic_fixture.pack_id),
         QString::fromLatin1(semantic_fixture.version),
         QString::fromLatin1(semantic_fixture.revision),
         QString::fromLatin1(semantic_fixture.case_id), cli_handoff_path},
        [&prepare_clock_calls] {
            ++prepare_clock_calls;
            return prepare_clock_calls == 1 ? QDate(2026, 8, 20) : QDate(2026, 8, 18);
        });
    requireSuccess(hostile_prepare, QStringLiteral("prepare-independent-review"));
    QCOMPARE(prepare_clock_calls, 1);
    QVERIFY(writeNew(cli_declaration_path, jsonBytes(declaration)));
    int finalize_clock_calls = 0;
    const auto hostile_finalize = appellate::cli::detail::runPackCli(
        finalize_arguments(cli_handoff_path, cli_declaration_path, semantic_catalog_path,
                           cli_final_path),
        [&finalize_clock_calls] {
            ++finalize_clock_calls;
            return finalize_clock_calls == 1 ? QDate(2026, 8, 20) : QDate(2026, 8, 18);
        });
    requireSuccess(hostile_finalize, QStringLiteral("finalize-independent-review"));
    QCOMPARE(finalize_clock_calls, 1);
    QCOMPARE(readAll(QDir(cli_final_path).filePath(QStringLiteral("manifest.json"))),
             baseline->manifest_bytes);
    QCOMPARE(
        readAll(QDir(cli_final_path).filePath(QStringLiteral("resources/realism-review.json"))),
        baseline->review_bytes);

    struct RawStagedOverflow final {
        QString slug;
        QByteArray review_bytes;
    };
    const std::array raw_staged_overflows{
        RawStagedOverflow{QStringLiteral("bytes"),
                          paddedJsonObject(maximum_final_review_bytes + 1)},
        RawStagedOverflow{QStringLiteral("depth"), nestedJsonObject(65)},
        RawStagedOverflow{QStringLiteral("values"), jsonObjectWithValueCount(200'001)},
    };
    for (const auto& overflow : raw_staged_overflows) {
        const auto destination =
            QDir(semantic_root.path())
                .filePath(QStringLiteral("raw-staged-%1-rejected").arg(overflow.slug));
        bool members_replaced = false;
        int replacement_calls = 0;
        int raw_clock_calls = 0;
        appellate::cli::detail::IndependentReviewPublisherReport publisher_report;
        appellate::cli::detail::IndependentReviewCliHooks cli_hooks;
        cli_hooks.replace_finalized_publication_members =
            [&baseline, &members_replaced, &overflow,
             &replacement_calls](QByteArray& manifest_bytes, QByteArray& review_bytes) {
                ++replacement_calls;
                QJsonParseError error;
                auto document = QJsonDocument::fromJson(manifest_bytes, &error);
                if (error.error != QJsonParseError::NoError || !document.isObject()) {
                    return;
                }
                auto manifest = document.object();
                auto contents = manifest.value(QStringLiteral("contents")).toArray();
                qsizetype matches = 0;
                for (qsizetype index = 0; index < contents.size(); ++index) {
                    auto descriptor = contents.at(index).toObject();
                    if (descriptor.value(QStringLiteral("id")).toString() !=
                        baseline->review_resource_id) {
                        continue;
                    }
                    ++matches;
                    descriptor.insert(QStringLiteral("sha256"), sha256(overflow.review_bytes));
                    contents.replace(index, descriptor);
                }
                if (matches != 1) {
                    return;
                }
                manifest.insert(QStringLiteral("contents"), contents);
                manifest_bytes = jsonBytes(manifest);
                review_bytes = overflow.review_bytes;
                members_replaced = true;
            };
        cli_hooks.publisher.report = &publisher_report;
        if (overflow.review_bytes.size() > maximum_final_review_bytes) {
            cli_hooks.publisher.injected_final_review_byte_limit = overflow.review_bytes.size();
        }

        const auto rejected = appellate::cli::detail::runPackCli(
            finalize_arguments(cli_handoff_path, cli_declaration_path, semantic_catalog_path,
                               destination),
            [&raw_clock_calls] {
                ++raw_clock_calls;
                return QDate(2026, 8, 20);
            },
            cli_hooks);

        QCOMPARE(raw_clock_calls, 1);
        QCOMPARE(replacement_calls, 1);
        QVERIFY(members_replaced);
        requireCommandError(rejected, ExitCode::InvalidPack,
                            QStringLiteral("invalid_independent_review_pack"),
                            QStringLiteral("finalize-independent-review"));
        QVERIFY(!publisher_report.staging_path.isEmpty());
        QVERIFY(!QFileInfo::exists(publisher_report.staging_path));
        QVERIFY(publisher_report.remaining_ledger_paths.isEmpty());
        QVERIFY(!QFileInfo::exists(destination));
        QVERIFY(std::ranges::any_of(
            publisher_report.observations,
            [](const appellate::cli::detail::IndependentReviewPublisherObservation& observation) {
                return observation.event ==
                       appellate::cli::detail::IndependentReviewPublisherEvent::
                           BeforeStagedValidation;
            }));
        QVERIFY(std::ranges::any_of(
            publisher_report.observations,
            [](const appellate::cli::detail::IndependentReviewPublisherObservation& observation) {
                return observation.event ==
                       appellate::cli::detail::IndependentReviewPublisherEvent::CleanupRemoved;
            }));
    }

    const auto future_cli_destination =
        QDir(semantic_root.path()).filePath(QStringLiteral("clock-future-rejected"));
    QVERIFY(overwriteAll(cli_declaration_path, jsonBytes(future_date_declaration)));
    int future_clock_calls = 0;
    const auto future_clock_rejected = appellate::cli::detail::runPackCli(
        finalize_arguments(cli_handoff_path, cli_declaration_path, semantic_catalog_path,
                           future_cli_destination),
        [&future_clock_calls] {
            ++future_clock_calls;
            return future_clock_calls == 1 ? QDate(2026, 8, 20) : QDate(2026, 8, 21);
        });
    QCOMPARE(future_clock_calls, 1);
    QVERIFY(!QFileInfo::exists(future_cli_destination));
    requireCommandError(future_clock_rejected, ExitCode::InvalidPack,
                        QStringLiteral("invalid_declaration"),
                        QStringLiteral("finalize-independent-review"));
    QVERIFY(overwriteAll(cli_declaration_path, jsonBytes(declaration)));

    int rejected_cli_sequence = 0;
    const auto expect_cli_input_rejection = [&](const QString& code) {
        const auto destination =
            QDir(semantic_root.path())
                .filePath(QStringLiteral("rejected-cli-%1").arg(rejected_cli_sequence++));
        int calls = 0;
        const auto rejected = appellate::cli::detail::runPackCli(
            finalize_arguments(cli_handoff_path, cli_declaration_path, semantic_catalog_path,
                               destination),
            [&calls] {
                ++calls;
                return QDate(2026, 8, 20);
            });
        QCOMPARE(calls, 1);
        QVERIFY(!QFileInfo::exists(destination));
        requireCommandError(rejected, ExitCode::InvalidPack, code,
                            QStringLiteral("finalize-independent-review"));
    };
    const auto cli_handoff_file = QDir(cli_handoff_path).filePath(QStringLiteral("handoff.json"));
    const auto cli_template_file =
        QDir(cli_handoff_path).filePath(QStringLiteral("review-declaration.template.json"));
    const auto saved_cli_handoff = readAll(cli_handoff_file);
    const auto saved_cli_template = readAll(cli_template_file);
    QVERIFY(overwriteAll(cli_handoff_file, saved_cli_handoff + ' '));
    expect_cli_input_rejection(QStringLiteral("invalid_handoff"));
    QVERIFY(overwriteAll(cli_handoff_file, saved_cli_handoff));
    QVERIFY(overwriteAll(cli_template_file, QByteArrayLiteral("{\"x\":1,\"x\":2}")));
    expect_cli_input_rejection(QStringLiteral("invalid_handoff"));
    QVERIFY(overwriteAll(cli_template_file, saved_cli_template));
    QVERIFY(overwriteAll(cli_declaration_path,
                         QByteArrayLiteral("{\"reviewer_reference\":\"\\ud800\"}")));
    expect_cli_input_rejection(QStringLiteral("invalid_declaration"));
    QVERIFY(overwriteAll(cli_declaration_path, jsonBytes(declaration)));

    auto snapshot_expected_declaration =
        completedIndependentDeclaration(*prepared, QStringLiteral("snapshot-conflict"));
    snapshot_expected_declaration.insert(
        QStringLiteral("reviewer_reference"),
        QStringLiteral("TEST-ONLY expected held-snapshot publication"));
    auto snapshot_conflicting_declaration = snapshot_expected_declaration;
    snapshot_conflicting_declaration.insert(
        QStringLiteral("reviewer_reference"),
        QStringLiteral("TEST-ONLY post-snapshot conflicting publication"));
    auto snapshot_expected_input = baseline_input;
    snapshot_expected_input.completed_declaration_bytes = jsonBytes(snapshot_expected_declaration);
    auto snapshot_conflicting_input = baseline_input;
    snapshot_conflicting_input.completed_declaration_bytes =
        jsonBytes(snapshot_conflicting_declaration);
    const auto snapshot_expected =
        appellate::packs::finalizeIndependentReview(*semantic_snapshot, snapshot_expected_input);
    const auto snapshot_conflicting =
        appellate::packs::finalizeIndependentReview(*semantic_snapshot, snapshot_conflicting_input);
    QVERIFY2(snapshot_expected.has_value(),
             snapshot_expected ? "" : snapshot_expected.error().message.toUtf8().constData());
    QVERIFY2(snapshot_conflicting.has_value(),
             snapshot_conflicting ? "" : snapshot_conflicting.error().message.toUtf8().constData());
    QVERIFY(snapshot_expected->revision.id == snapshot_conflicting->revision.id);
    QCOMPARE(snapshot_expected->revision.version, snapshot_conflicting->revision.version);
    QVERIFY(snapshot_expected->revision.digest != snapshot_conflicting->revision.digest);

    const auto conflicting_pack_path =
        QDir(semantic_root.path()).filePath(QStringLiteral("snapshot-conflicting-pack"));
    QVERIFY(write_final_pack(conflicting_pack_path, *snapshot_conflicting));
    const auto conflicting_archive =
        QDir(semantic_root.path()).filePath(QStringLiteral("snapshot-conflicting.awpack"));
    const auto exported_conflicting = PackArchive::exportDirectory(
        conflicting_pack_path, conflicting_archive, {}, PackValidationScope::ResolvedClosure);
    QVERIFY2(exported_conflicting.has_value(),
             exported_conflicting ? "" : exported_conflicting.error().message.toUtf8().constData());
    QVERIFY(*exported_conflicting == snapshot_conflicting->revision);
    auto opened_live_catalog = PackCatalog::open(semantic_catalog_path);
    QVERIFY2(opened_live_catalog.has_value(),
             opened_live_catalog ? "" : opened_live_catalog.error().message.toUtf8().constData());
    auto live_catalog = std::move(*opened_live_catalog);
    const auto installed_conflicting =
        live_catalog->installArchive(conflicting_archive, QStringLiteral("2026-08-20T00:00:00Z"));
    QVERIFY2(installed_conflicting.has_value(),
             installed_conflicting ? ""
                                   : installed_conflicting.error().message.toUtf8().constData());
    QVERIFY(installed_conflicting->revision == snapshot_conflicting->revision);

    const auto rebuilt_after_conflict =
        appellate::packs::finalizeIndependentReview(*semantic_snapshot, snapshot_expected_input);
    QVERIFY2(rebuilt_after_conflict.has_value(),
             rebuilt_after_conflict ? ""
                                    : rebuilt_after_conflict.error().message.toUtf8().constData());
    QVERIFY(rebuilt_after_conflict->revision == snapshot_expected->revision);
    QCOMPARE(rebuilt_after_conflict->manifest_bytes, snapshot_expected->manifest_bytes);
    QCOMPARE(rebuilt_after_conflict->review_bytes, snapshot_expected->review_bytes);

    const auto protected_inputs =
        appellate::packs::detail::inspectProtectedCatalogInputs(*semantic_snapshot);
    QVERIFY2(protected_inputs.has_value(),
             protected_inputs ? "" : protected_inputs.error().message.toUtf8().constData());
    std::vector<appellate::cli::detail::IndependentReviewProtectedDirectory>
        publisher_protected_directories;
    publisher_protected_directories.reserve(protected_inputs->directories.size());
    for (const auto& directory : protected_inputs->directories) {
        publisher_protected_directories.push_back({directory.device, directory.inode});
    }
    const auto held_snapshot_destination =
        QDir(semantic_root.path()).filePath(QStringLiteral("held-snapshot-final"));
    appellate::cli::detail::IndependentReviewPublicationRequest held_snapshot_request{
        IndependentReviewArtifactKind::FinalizedPack,
        held_snapshot_destination,
        std::move(publisher_protected_directories),
        protected_inputs->aggregate_entry_count,
        {},
        {
            {QStringLiteral("manifest.json"), snapshot_expected->manifest_bytes},
            {QStringLiteral("resources/realism-review.json"), snapshot_expected->review_bytes},
        },
        [&](const QString& staging_root)
            -> std::expected<void, appellate::cli::detail::IndependentReviewStagedValidationError> {
            const auto staged =
                PackReader::readDirectory(staging_root, PackValidationScope::ResolvedClosure);
            if (!staged) {
                return std::unexpected(
                    appellate::cli::detail::IndependentReviewStagedValidationError{
                        staged.error().message});
            }
            const auto graph = PackReader::validateResolvedGraph(
                *staged, std::span<const appellate::packs::LoadedPack* const>(final_dependencies));
            if (!graph) {
                return std::unexpected(
                    appellate::cli::detail::IndependentReviewStagedValidationError{
                        graph.error().message});
            }
            const auto rebuilt = appellate::packs::finalizeIndependentReview(
                *semantic_snapshot, snapshot_expected_input);
            if (!rebuilt || rebuilt->revision != snapshot_expected->revision ||
                rebuilt->manifest_bytes != snapshot_expected->manifest_bytes ||
                rebuilt->review_bytes != snapshot_expected->review_bytes) {
                return std::unexpected(
                    appellate::cli::detail::IndependentReviewStagedValidationError{
                        rebuilt ? QStringLiteral("Held-snapshot output changed")
                                : rebuilt.error().message,
                        appellate::cli::detail::IndependentReviewStagedValidationErrorCode::
                            PublicationMismatch});
            }
            return {};
        },
    };
    const auto published_held_snapshot =
        appellate::cli::detail::publishIndependentReviewArtifacts(held_snapshot_request);
    QVERIFY2(published_held_snapshot.has_value(),
             published_held_snapshot
                 ? ""
                 : published_held_snapshot.error().message.toUtf8().constData());
    QCOMPARE(readAll(QDir(held_snapshot_destination).filePath(QStringLiteral("manifest.json"))),
             snapshot_expected->manifest_bytes);
    QCOMPARE(readAll(QDir(held_snapshot_destination)
                         .filePath(QStringLiteral("resources/realism-review.json"))),
             snapshot_expected->review_bytes);
    const auto expected_archive =
        QDir(semantic_root.path()).filePath(QStringLiteral("snapshot-expected.awpack"));
    const auto exported_expected = PackArchive::exportDirectory(
        held_snapshot_destination, expected_archive, {}, PackValidationScope::ResolvedClosure);
    QVERIFY2(exported_expected.has_value(),
             exported_expected ? "" : exported_expected.error().message.toUtf8().constData());
    QVERIFY(*exported_expected == snapshot_expected->revision);
    const auto normal_install_conflict =
        live_catalog->installArchive(expected_archive, QStringLiteral("2026-08-20T00:00:01Z"));
    QVERIFY(!normal_install_conflict.has_value());
    QCOMPARE(normal_install_conflict.error().code,
             appellate::packs::CatalogErrorCode::ImmutableConflict);
    live_catalog.reset();

    semantic_snapshot.reset();
    auto reopened_semantic_catalog = PackCatalog::open(semantic_catalog_path);
    QVERIFY2(reopened_semantic_catalog.has_value(),
             reopened_semantic_catalog
                 ? ""
                 : reopened_semantic_catalog.error().message.toUtf8().constData());
    const auto installed_uncertainty =
        (*reopened_semantic_catalog)
            ->installArchive(exported_uncertainty_archive, QStringLiteral("2026-08-20T00:00:00Z"));
    QVERIFY2(installed_uncertainty.has_value(),
             installed_uncertainty ? ""
                                   : installed_uncertainty.error().message.toUtf8().constData());
    QVERIFY(installed_uncertainty->revision == finalized_256->revision);
    (*reopened_semantic_catalog).reset();
    auto opened_exact_snapshot = PackCatalogSnapshot::openExisting(semantic_catalog_path);
    QVERIFY2(opened_exact_snapshot.has_value(),
             opened_exact_snapshot ? ""
                                   : opened_exact_snapshot.error().message.toUtf8().constData());
    auto exact_snapshot = std::move(*opened_exact_snapshot);
    const auto exact_preexisting =
        appellate::packs::finalizeIndependentReview(*exact_snapshot, input_256);
    QVERIFY2(exact_preexisting.has_value(),
             exact_preexisting ? "" : exact_preexisting.error().message.toUtf8().constData());
    QVERIFY(exact_preexisting->revision == finalized_256->revision);
    QCOMPARE(exact_preexisting->manifest_bytes, finalized_256->manifest_bytes);
    QCOMPARE(exact_preexisting->review_bytes, finalized_256->review_bytes);
    const auto loaded_uncertainty = exact_snapshot->loadResolved(finalized_256->revision);
    QVERIFY2(loaded_uncertainty.has_value(),
             loaded_uncertainty ? "" : loaded_uncertainty.error().message.toUtf8().constData());
    const auto* loaded_review =
        findReview(loaded_uncertainty->root(), finalized_256->review_resource_id);
    QVERIFY(loaded_review != nullptr);
    QCOMPARE(loaded_review->document.value(QStringLiteral("known_uncertainty")).toArray(),
             uncertainties(256));
    QVERIFY(QDir(semantic_root.path())
                .entryList({QStringLiteral(".*.appellate-independent-review-*")},
                           QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)
                .isEmpty());

    const auto staging_residue =
        QDir(input_root.path())
            .entryList({QStringLiteral(".*.appellate-independent-review-*")},
                       QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    QVERIFY(staging_residue.isEmpty());
    QVERIFY(QDir(empty_catalog)
                .entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)
                .isEmpty());
#endif
}

void PackCliTest::mapsCatalogBusyWithoutMutatingTheLock() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto catalog_root = QDir(temporary.path()).filePath(QStringLiteral("catalog"));
    {
        auto catalog = appellate::packs::PackCatalog::open(catalog_root);
        QVERIFY2(catalog.has_value(), catalog ? "" : qPrintable(catalog.error().message));
    }

    const auto lock_path = QDir(catalog_root).filePath(QStringLiteral(".install.lock"));
    QLockFile held_lock(lock_path);
    QVERIFY(held_lock.tryLock());
    const auto lock_bytes = readAll(lock_path);
    QVERIFY(!lock_bytes.isEmpty());
    const auto archives_before = QDir(QDir(catalog_root).filePath(QStringLiteral("archives")))
                                     .entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
    const auto blobs_before = QDir(QDir(catalog_root).filePath(QStringLiteral("blobs")))
                                  .entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);

    const auto result = runPackCli({QStringLiteral("list"), catalog_root});
    QCOMPARE(result.exit_code, static_cast<int>(ExitCode::OperationFailed));
    QVERIFY(result.standard_output.isEmpty());
    QCOMPARE(responseObject(result.standard_error).value(QStringLiteral("code")).toString(),
             QStringLiteral("catalog_busy"));
    QCOMPARE(readAll(lock_path), lock_bytes);
    QCOMPARE(QDir(QDir(catalog_root).filePath(QStringLiteral("archives")))
                 .entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot),
             archives_before);
    QCOMPARE(QDir(QDir(catalog_root).filePath(QStringLiteral("blobs")))
                 .entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot),
             blobs_before);
    QVERIFY(
        !QFileInfo::exists(QDir(catalog_root).filePath(QStringLiteral(".install.lock.rmlock"))));
}

void PackCliTest::rejectsInvalidArgumentsAndExistingTemplateDestination() {
    int provider_calls = 0;
    const auto counting_provider = [&provider_calls] {
        ++provider_calls;
        return QDate(2026, 8, 20);
    };
    const auto wrong_prepare = appellate::cli::detail::runPackCli(
        {QStringLiteral("prepare-independent-review")}, counting_provider);
    QCOMPARE(wrong_prepare.exit_code, static_cast<int>(ExitCode::InvalidArguments));
    QCOMPARE(
        responseObject(wrong_prepare.standard_error).value(QStringLiteral("command")).toString(),
        QStringLiteral("prepare-independent-review"));
    const auto unrelated =
        appellate::cli::detail::runPackCli({QStringLiteral("not-a-command")}, counting_provider);
    QCOMPARE(unrelated.exit_code, static_cast<int>(ExitCode::InvalidArguments));
    QCOMPARE(provider_calls, 0);

    const auto repeated_path = [](qsizetype count) {
        return QStringLiteral("/") + QStringList(count, QStringLiteral("a")).join(u'/');
    };
    const auto deep_catalog = appellate::cli::detail::runPackCli(
        {QStringLiteral("prepare-independent-review"), repeated_path(127),
         QStringLiteral("test.subject"), QStringLiteral("1.0.0"), QString(64, u'0'),
         QStringLiteral("test.case"), QStringLiteral("new-handoff")},
        counting_provider);
    QCOMPARE(deep_catalog.exit_code, static_cast<int>(ExitCode::InvalidArguments));
    const auto deep_handoff = appellate::cli::detail::runPackCli(
        {QStringLiteral("finalize-independent-review"), repeated_path(128),
         QStringLiteral("completed.json"), QStringLiteral("catalog"), QStringLiteral("new-pack")},
        counting_provider);
    QCOMPARE(deep_handoff.exit_code, static_cast<int>(ExitCode::InvalidArguments));
    QCOMPARE(provider_calls, 0);
    QVERIFY(appellate::cli::detail::validateIndependentReviewDestinationPath(
                QString(218, u'd'),
                appellate::cli::detail::IndependentReviewArtifactKind::PreparedHandoff)
                .has_value());
    QVERIFY(!appellate::cli::detail::validateIndependentReviewDestinationPath(
                 QString(219, u'd'),
                 appellate::cli::detail::IndependentReviewArtifactKind::PreparedHandoff)
                 .has_value());

    const auto missing = runPackCli({});
    QCOMPARE(missing.exit_code, static_cast<int>(ExitCode::InvalidArguments));
    QVERIFY(missing.standard_output.isEmpty());
    QCOMPARE(responseObject(missing.standard_error).value(QStringLiteral("code")).toString(),
             QStringLiteral("invalid_arguments"));

    const auto extra_validate =
        runPackCli({QStringLiteral("validate"), QStringLiteral("one"), QStringLiteral("two")});
    QCOMPARE(extra_validate.exit_code, static_cast<int>(ExitCode::InvalidArguments));

    const auto invalid_timestamp = runPackCli(
        {QStringLiteral("install"), QStringLiteral("pack.awpack"), QStringLiteral("catalog"),
         QStringLiteral("--installed-at"), QStringLiteral("2026-08-11T01:02:03+00:00")});
    QCOMPARE(invalid_timestamp.exit_code, static_cast<int>(ExitCode::InvalidArguments));

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto invalid_date_provider = [&provider_calls] {
        ++provider_calls;
        return QDate{};
    };
    const auto missing_catalog = QDir(temporary.path()).filePath(QStringLiteral("missing-catalog"));
    const auto invalid_date_prepare = appellate::cli::detail::runPackCli(
        {QStringLiteral("prepare-independent-review"), missing_catalog,
         QStringLiteral("test.subject"), QStringLiteral("1.0.0"), QString(64, u'0'),
         QStringLiteral("test.case"),
         QDir(temporary.path()).filePath(QStringLiteral("new-handoff"))},
        invalid_date_provider);
    QCOMPARE(invalid_date_prepare.exit_code, static_cast<int>(ExitCode::OperationFailed));
    QCOMPARE(responseObject(invalid_date_prepare.standard_error)
                 .value(QStringLiteral("code"))
                 .toString(),
             QStringLiteral("invalid_configuration"));
    const auto invalid_date_finalize = appellate::cli::detail::runPackCli(
        {QStringLiteral("finalize-independent-review"),
         QDir(temporary.path()).filePath(QStringLiteral("missing-handoff")),
         QDir(temporary.path()).filePath(QStringLiteral("missing-declaration.json")),
         missing_catalog, QDir(temporary.path()).filePath(QStringLiteral("new-pack"))},
        invalid_date_provider);
    QCOMPARE(invalid_date_finalize.exit_code, static_cast<int>(ExitCode::OperationFailed));
    QCOMPARE(responseObject(invalid_date_finalize.standard_error)
                 .value(QStringLiteral("code"))
                 .toString(),
             QStringLiteral("invalid_configuration"));
    QCOMPARE(provider_calls, 2);
    QVERIFY(!QFileInfo::exists(missing_catalog));

    const auto existing = QDir(temporary.path()).filePath(QStringLiteral("existing"));
    QVERIFY(QDir{}.mkpath(existing));
    const auto marker_path = QDir(existing).filePath(QStringLiteral("keep.txt"));
    QFile marker(marker_path);
    QVERIFY(marker.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(marker.write("preserve"), qint64{8});
    marker.close();

    const auto refused = runPackCli({QStringLiteral("template"), existing});
    QCOMPARE(refused.exit_code, static_cast<int>(ExitCode::OperationFailed));
    QCOMPARE(responseObject(refused.standard_error).value(QStringLiteral("code")).toString(),
             QStringLiteral("destination_exists"));
    QCOMPARE(readAll(marker_path), QByteArray("preserve"));
    QVERIFY(!QFileInfo::exists(QDir(existing).filePath(QStringLiteral("manifest.json"))));
}

} // namespace

QTEST_GUILESS_MAIN(PackCliTest)

#include "tst_pack_cli.moc"
