#include "pack_cli.hpp"

#include "appellate/engine/workflow_engine.hpp"
#include "appellate/packs/pack_archive.hpp"
#include "appellate/packs/pack_catalog.hpp"
#include "appellate/packs/pack_reader.hpp"
#include "appellate/packs/realism_evidence_authoring.hpp"
#include "appellate/packs/runtime_pack.hpp"
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
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#if defined(Q_OS_UNIX)
#include <sys/wait.h>
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
    void finalizesIndependentReviewDeterministically();
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

[[nodiscard]] QByteArray readAll(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

[[nodiscard]] bool overwriteAll(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(bytes) == bytes.size();
}

[[nodiscard]] QByteArray jsonBytes(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Indented);
}

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

    const auto list_after = snapshot->list();
    QVERIFY(list_after.has_value());
    QCOMPARE(*list_after, *list_before);
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
