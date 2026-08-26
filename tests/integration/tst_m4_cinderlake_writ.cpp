#include "appellate/engine/workflow_engine.hpp"
#include "appellate/packs/pack_archive.hpp"
#include "appellate/packs/pack_catalog.hpp"
#include "appellate/packs/runtime_pack.hpp"
#include "appellate/storage/session_store.hpp"
#include "appellate/storage/workflow_codec.hpp"
#include "installed_record_controller.hpp"
#include "record_workspace.hpp"
#include "session_controller.hpp"

#include <QApplication>
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QPdfDocument>
#include <QSet>
#include <QTemporaryDir>
#include <QTimeZone>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <iostream>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
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

namespace model = appellate::model;
namespace engine = appellate::engine;
namespace packs = appellate::packs;
namespace storage = appellate::storage;

namespace appellate::ui {

class RecordWorkspaceTestAccess final {
  public:
    [[nodiscard]] static const RecordDefinition& fullDefinition(const RecordWorkspace& workspace) {
        return workspace.full_definition_;
    }
};

} // namespace appellate::ui

namespace {

namespace app = appellate::app;
namespace ui = appellate::ui;
using namespace std::chrono_literals;

namespace ReleasePins {
constexpr std::string_view manifest_sha256 =
    "53b2f6769e5e294e8ada426357a1a5eb29e770a9715d9d75ee993b6fd4f5a0ad";
constexpr std::string_view realism_review_sha256 =
    "2ab0d10cef5e67e5dde3684de994d8c838bc566af8925c3686841f14cc82fad7";
constexpr std::string_view record_sha256 =
    "dfd9e3e48cb97aeb5846ce8173143bd69192ea35127e07f40079ae3ce70f32e6";
constexpr std::string_view workflow_sha256 =
    "337c0eb859c43ec1296a7e4cb0e595cb20bbb061265dd6ce86f02a5524851d60";
constexpr std::string_view actual_argument_sha256 =
    "b6ff4404d140de641bcae080e65b7d94a4486f263834fa43a2828303baec5f21";
constexpr std::string_view counterfactual_argument_sha256 =
    "a9771f82c8b9348c5ab0b8c0667b32895484cb50d06399fa11b84ca886ce77a4";
constexpr std::string_view evidence_closure =
    "f6f573db90136f950b34055f4ae9baa18c7b3135733ff1d803dc686da7463439";
constexpr std::string_view trace_closure =
    "e589e247823dd02b51a03069d3724a0e9f3827480be81a95ef750e665e4f6988";
constexpr std::string_view root_revision =
    "020517571a6c15f90765e12b94ab53d8598be3bc3081d47caecdf5950bacd05c";
constexpr std::string_view archive_sha256 =
    "eeefbbbe84cf4addbf91a68447281217226c6a08c7e0e3e1294947d5e5dc8956";
constexpr std::uint64_t archive_byte_size = 2'519'053;
} // namespace ReleasePins

constexpr std::string_view case_id = "ca4m4.case.cinderlake-writ";
constexpr std::string_view workflow_id = "ca4m4.cinder.workflow.privilege-mandamus";

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

[[nodiscard]] QByteArray readAll(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        fail("cannot read " + path.toStdString());
    return file.readAll();
}

[[nodiscard]] bool writeAll(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(bytes) == bytes.size() && file.flush();
}

[[nodiscard]] QString asQString(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

[[nodiscard]] QByteArray sha256(QByteArrayView bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

[[nodiscard]] QByteArray sha256(const QByteArray& bytes) { return sha256(QByteArrayView(bytes)); }

[[nodiscard]] QJsonObject parseObject(const QByteArray& bytes, const QString& path) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        fail("invalid JSON object " + path.toStdString());
    return document.object();
}

[[nodiscard]] QSet<QString> strings(const QJsonArray& values) {
    QSet<QString> result;
    for (const auto& value : values)
        result.insert(value.toString());
    return result;
}

[[nodiscard]] QJsonObject objectAt(const QHash<QString, QJsonObject>& objects, const QString& id,
                                   std::string_view label) {
    const auto found = objects.constFind(id);
    if (found == objects.constEnd())
        fail(std::string(label) + " is missing: " + id.toStdString());
    return *found;
}

[[nodiscard]] const model::WorkflowCommandHeader&
commandHeader(const model::WorkflowCommand& command) {
    return std::visit([](const auto& concrete) -> const auto& { return concrete.header; }, command);
}

[[nodiscard]] model::WorkflowCommandHeader& commandHeader(model::WorkflowCommand& command) {
    return std::visit([](auto& concrete) -> auto& { return concrete.header; }, command);
}

[[nodiscard]] const model::WorkflowEventHeader& eventHeader(const model::WorkflowEvent& event) {
    return std::visit([](const auto& concrete) -> const auto& { return concrete.header; }, event);
}

[[nodiscard]] model::WorkflowEventHeader& eventHeader(model::WorkflowEvent& event) {
    return std::visit([](auto& concrete) -> auto& { return concrete.header; }, event);
}

[[nodiscard]] std::optional<std::string>
commandDocumentDigest(const model::WorkflowCommand& command) {
    return std::visit(
        [](const auto& concrete) -> std::optional<std::string> {
            if constexpr (requires { concrete.document_sha256; })
                return concrete.document_sha256;
            return std::nullopt;
        },
        command);
}

[[nodiscard]] std::string dateText(const model::LegalDate& date) {
    if (!date.value.ok())
        fail("Invalid legal date");
    const auto year = static_cast<int>(date.value.year());
    if (year < 1 || year > 9999)
        fail("Legal date year must be between 0001 and 9999");
    return QStringLiteral("%1-%2-%3")
        .arg(year, 4, 10, QLatin1Char('0'))
        .arg(static_cast<unsigned>(date.value.month()), 2, 10, QLatin1Char('0'))
        .arg(static_cast<unsigned>(date.value.day()), 2, 10, QLatin1Char('0'))
        .toStdString();
}

void addUint64(QCryptographicHash& hash, std::uint64_t value) {
    std::array<char, 8> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto shift = static_cast<unsigned>((bytes.size() - index - 1U) * 8U);
        bytes.at(index) = static_cast<char>((value >> shift) & 0xffU);
    }
    hash.addData(QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())));
}

void addFrame(QCryptographicHash& hash, QByteArrayView value) {
    addUint64(hash, static_cast<std::uint64_t>(value.size()));
    hash.addData(value);
}

void addFrame(QCryptographicHash& hash, QStringView value) {
    const auto utf8 = value.toUtf8();
    addFrame(hash, QByteArrayView(utf8));
}

[[nodiscard]] std::optional<QString> journalDigest(const QJsonArray& journal) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, QByteArrayView("appellate-workbench-executed-workflow-journal-v1"));
    addUint64(hash, static_cast<std::uint64_t>(journal.size()));
    for (const auto& value : journal) {
        const auto entry = value.toObject();
        const auto encoded_command =
            entry.value(QStringLiteral("command_base64")).toString().toLatin1();
        const auto command = QByteArray::fromBase64(encoded_command);
        const auto events = entry.value(QStringLiteral("events_base64")).toArray();
        if (entry.isEmpty() || command.isEmpty() || command.toBase64() != encoded_command)
            return std::nullopt;
        addFrame(hash, QByteArrayView(command));
        addUint64(hash, static_cast<std::uint64_t>(events.size()));
        for (const auto& event : events) {
            const auto encoded = event.toString().toLatin1();
            const auto decoded = QByteArray::fromBase64(encoded);
            if (decoded.isEmpty() || decoded.toBase64() != encoded)
                return std::nullopt;
            addFrame(hash, QByteArrayView(decoded));
        }
    }
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] QString traceDigest(const QJsonObject& trace) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, QByteArrayView("appellate-workbench-executed-trace-evidence-v1"));
    addFrame(hash, QStringLiteral("ca4m4.case.cinderlake-writ"));
    addFrame(hash, trace.value(QStringLiteral("evidence_id")).toString());
    addFrame(hash, trace.value(QStringLiteral("trace_id")).toString());
    addFrame(hash, trace.value(QStringLiteral("workflow_id")).toString());
    addFrame(hash, trace.value(QStringLiteral("engine_revision")).toString());
    addUint64(hash,
              static_cast<std::uint64_t>(trace.value(QStringLiteral("command_count")).toInteger()));
    addUint64(hash,
              static_cast<std::uint64_t>(trace.value(QStringLiteral("event_count")).toInteger()));
    addFrame(hash, trace.value(QStringLiteral("journal_sha256")).toString());
    const auto operation_ids = trace.value(QStringLiteral("operation_ids")).toArray();
    addUint64(hash, static_cast<std::uint64_t>(operation_ids.size()));
    for (const auto& value : operation_ids)
        addFrame(hash, value.toString());
    addFrame(hash, trace.value(QStringLiteral("terminal_stage_id")).toString());
    return QString::fromLatin1(hash.result().toHex());
}

struct TraceMeta final {
    QString file;
    std::string label;
    std::string trace_id;
    std::string evidence_id;
    std::size_t commands;
    std::size_t events;
    std::string terminal;
    std::string file_sha256;
    std::string journal_sha256;
    std::string digest;
};

const std::array<TraceMeta, 3> trace_metas{
    TraceMeta{QStringLiteral("actual-through-rehearing-expiration.json"), "actual",
              "ca4m4.cinder.trace.actual-through-rehearing-expiration",
              "ca4m4.cinder.evidence.trace.actual-through-rehearing-expiration", 26, 29,
              "ca4m4.cinder.stage.actual-terminal",
              "46b28dfe93f2944061210b9af26e042125b4bfe073c952cbc12a0c6bcf6093a2",
              "2c9710ea8cd9fcf29d625bba317a714fabb59188eb691f113293b3dacd827116",
              "363b21bb154d03dda4353f4516d7f730cebe0b1ec52c1887b408dd0895d66c6e"},
    TraceMeta{QStringLiteral("counterfactual-deficiency-cure.json"), "deficiency",
              "ca4m4.cinder.trace.counterfactual-deficiency-cure",
              "ca4m4.cinder.evidence.trace.counterfactual-deficiency-cure", 4, 6,
              "ca4m4.cinder.stage.submitted",
              "ca8a32d23c1b92628ccc926720aa5e690f7c4cd04c0128ee877a5270c4f8a823",
              "6e066d0dfe0283f2a5538d63f17562e1a5dbb5c1f3cddf7154bcb1b8205289f6",
              "d8eacd84fadacff1b2c251de0a8d8152d41ce176f27bd1c38c5b863e26610100"},
    TraceMeta{QStringLiteral("counterfactual-summary-denial-through-rehearing-expiration.json"),
              "summary",
              "ca4m4.cinder.trace.counterfactual-summary-denial-through-rehearing-expiration",
              "ca4m4.cinder.evidence.trace.counterfactual-summary-denial-through-rehearing-"
              "expiration",
              6, 7, "ca4m4.cinder.stage.counterfactual-summary-terminal",
              "58d1bb00a4ff5042fbeb1e644320efb7ac924e9bab4f66c709eb95bd2f02749b",
              "88f16fca0493f5e1dccabc15ca8011bb3ced423c85d09a466a9d7b43f5ce8f77",
              "666f7721612ce47073a1df879c5d532798b05ee0b9f9ad2c25d0e620314148a7"},
};

[[nodiscard]] bool hasFilingPrecondition(const QJsonObject& operation, QStringView filing_id,
                                         bool present) {
    return std::ranges::any_of(
        operation.value(QStringLiteral("preconditions")).toArray(), [&](const auto& value) {
            const auto condition = value.toObject();
            return condition.value(QStringLiteral("kind")).toString() ==
                       QStringLiteral("filing_instance") &&
                   condition.value(QStringLiteral("filing_id")).toString() == filing_id &&
                   condition.value(QStringLiteral("present")).toBool() == present;
        });
}

[[nodiscard]] bool hasDeadlineStatus(const QJsonObject& operation, QStringView deadline_id,
                                     QStringView status) {
    return std::ranges::any_of(
        operation.value(QStringLiteral("preconditions")).toArray(), [&](const auto& value) {
            const auto condition = value.toObject();
            return condition.value(QStringLiteral("kind")).toString() ==
                       QStringLiteral("deadline_status") &&
                   condition.value(QStringLiteral("deadline_id")).toString() == deadline_id &&
                   condition.value(QStringLiteral("status")).toString() == status;
        });
}

void auditStaticContent(const QDir& root) {
    const QDir pack_root(root.filePath(QStringLiteral("pack-candidate")));
    const auto manifest_bytes = readAll(pack_root.filePath(QStringLiteral("manifest.json")));
    const auto review_bytes =
        readAll(pack_root.filePath(QStringLiteral("resources/realism-review.json")));
    const auto record_bytes = readAll(pack_root.filePath(QStringLiteral("resources/record.json")));
    const auto workflow_bytes =
        readAll(pack_root.filePath(QStringLiteral("resources/workflow.json")));
    const auto actual_argument_bytes =
        readAll(pack_root.filePath(QStringLiteral("resources/argument-actual.json")));
    const auto counterfactual_argument_bytes =
        readAll(pack_root.filePath(QStringLiteral("resources/argument-counterfactual.json")));
    if (sha256(manifest_bytes).toStdString() != ReleasePins::manifest_sha256 ||
        sha256(review_bytes).toStdString() != ReleasePins::realism_review_sha256 ||
        sha256(record_bytes).toStdString() != ReleasePins::record_sha256 ||
        sha256(workflow_bytes).toStdString() != ReleasePins::workflow_sha256 ||
        sha256(actual_argument_bytes).toStdString() != ReleasePins::actual_argument_sha256 ||
        sha256(counterfactual_argument_bytes).toStdString() !=
            ReleasePins::counterfactual_argument_sha256) {
        fail("frozen final resource digest drift");
    }

    const auto manifest = parseObject(manifest_bytes, QStringLiteral("manifest.json"));
    const auto review = parseObject(review_bytes, QStringLiteral("realism-review.json"));
    const auto record = parseObject(record_bytes, QStringLiteral("record.json"));
    const auto workflow = parseObject(workflow_bytes, QStringLiteral("workflow.json"));
    const auto case_definition =
        parseObject(readAll(pack_root.filePath(QStringLiteral("resources/case.json"))),
                    QStringLiteral("case.json"));
    const auto actual_argument =
        parseObject(actual_argument_bytes, QStringLiteral("argument-actual.json"));
    const auto counterfactual_argument =
        parseObject(counterfactual_argument_bytes, QStringLiteral("argument-counterfactual.json"));

    const auto contents = manifest.value(QStringLiteral("contents")).toArray();
    const auto blobs = manifest.value(QStringLiteral("blobs")).toArray();
    const auto capabilities = manifest.value(QStringLiteral("required_capabilities")).toArray();
    const auto dependencies = manifest.value(QStringLiteral("dependencies")).toArray();
    if (manifest.value(QStringLiteral("schema_version")).toInt() != 2 ||
        manifest.value(QStringLiteral("pack_id")).toString() !=
            QStringLiteral("us.ca4.m4.cinderlake-writ") ||
        manifest.value(QStringLiteral("version")).toString() != QStringLiteral("1.2.0") ||
        contents.size() != 9 || blobs.size() != 48 || capabilities.size() != 18 ||
        dependencies.size() != 3) {
        fail("final 9-resource/48-blob/18-capability/3-dependency envelope drift");
    }

    const QSet<QString> expected_content_ids{
        QStringLiteral("ca4m4.case.cinderlake-writ"),
        QStringLiteral("ca4m4.cinder.argument.actual-record"),
        QStringLiteral("ca4m4.cinder.argument.summary-denial-counterfactual"),
        QStringLiteral("ca4m4.cinder.authorities.case-specific"),
        QStringLiteral("ca4m4.cinder.bench.three-judge"),
        QStringLiteral("ca4m4.cinder.procedure.privilege-mandamus"),
        QStringLiteral("ca4m4.cinder.record"),
        QStringLiteral("ca4m4.cinder.workflow.privilege-mandamus"),
        QStringLiteral("ca4m4.cinder.review.authoring-2026-08-19"),
    };
    QSet<QString> content_ids;
    for (const auto& value : contents) {
        const auto descriptor = value.toObject();
        const auto path = descriptor.value(QStringLiteral("path")).toString();
        const auto bytes = readAll(pack_root.filePath(path));
        const auto resource = parseObject(bytes, path);
        const auto id = descriptor.value(QStringLiteral("id")).toString();
        if (!expected_content_ids.contains(id) || content_ids.contains(id) ||
            descriptor.value(QStringLiteral("schema_version")).toInt() != 2 ||
            descriptor.value(QStringLiteral("sha256")).toString().toLatin1() != sha256(bytes) ||
            resource.value(QStringLiteral("resource_id")).toString() != id ||
            resource.value(QStringLiteral("resource_kind")).toString() !=
                descriptor.value(QStringLiteral("kind")).toString()) {
            fail("resource descriptor drift: " + path.toStdString());
        }
        content_ids.insert(id);
    }
    if (content_ids != expected_content_ids)
        fail("exact nine-resource ID set drift");

    QSet<QString> capability_ids;
    for (const auto& value : capabilities) {
        const auto capability = value.toObject();
        const auto id = capability.value(QStringLiteral("id")).toString();
        if (id.isEmpty() || capability_ids.contains(id) ||
            capability.value(QStringLiteral("version")).toInt() !=
                (id == QStringLiteral("workbench.pack.declarative-resources") ? 2 : 1)) {
            fail("capability identity/version drift: " + id.toStdString());
        }
        capability_ids.insert(id);
    }
    if (capability_ids.size() != 18 ||
        !capability_ids.contains(QStringLiteral("workbench.pack.realism-evidence")) ||
        !capability_ids.contains(QStringLiteral("workbench.pack.static-deficiency-deadlines"))) {
        fail("exact Cinder capability set drift");
    }

    const QHash<QString, QString> dependency_digests{
        {QStringLiteral("foundation.us-federal"),
         QStringLiteral("866c90996c15e2076b9508a297ffce1a4e766b1432a9e11d08e8138c57e363c9")},
        {QStringLiteral("foundation.us-ca4"),
         QStringLiteral("449d75c77e5c47883f750377450f2d1ec1fc0e42e20b1f247446b208661d3262")},
        {QStringLiteral("foundation.us-ca4-fictional-bench"),
         QStringLiteral("cee0bf93309cc9ad800f215a47d734b20a9fdf5dc889f2f440e4382b942d332d")},
    };
    QSet<QString> dependency_ids;
    for (const auto& value : dependencies) {
        const auto dependency = value.toObject();
        const auto id = dependency.value(QStringLiteral("pack_id")).toString();
        if (!dependency_digests.contains(id) || dependency_ids.contains(id) ||
            dependency.value(QStringLiteral("sha256")).toString() != dependency_digests.value(id)) {
            fail("dependency revision drift: " + id.toStdString());
        }
        dependency_ids.insert(id);
    }
    if (dependency_ids.size() != 3)
        fail("three-dependency closure drift");

    const auto evidence = review.value(QStringLiteral("evidence")).toObject();
    const auto dimension_evidence = evidence.value(QStringLiteral("dimension_evidence")).toObject();
    const auto dimensions = review.value(QStringLiteral("dimensions")).toObject();
    if (review.value(QStringLiteral("review_state")).toString() !=
            QStringLiteral("independent_review_pending") ||
        review.value(QStringLiteral("known_uncertainty")).toArray().size() != 16 ||
        std::ranges::any_of(review.value(QStringLiteral("known_uncertainty")).toArray(),
                            [](const auto& value) {
                                return value.toObject().value(QStringLiteral("blocking")).toBool();
                            }) ||
        dimensions.size() != 7 ||
        std::ranges::any_of(dimensions.keys(),
                            [&](const auto& key) { return dimensions.value(key).toInt() != 2; }) ||
        evidence.value(QStringLiteral("closure_digest")).toString().toStdString() !=
            ReleasePins::evidence_closure ||
        evidence.value(QStringLiteral("packs")).toArray().size() != 4 ||
        evidence.value(QStringLiteral("resources")).toArray().size() != 44 ||
        evidence.value(QStringLiteral("blobs")).toArray().size() != 48 ||
        evidence.value(QStringLiteral("traces")).toArray().size() != 3 ||
        evidence.value(QStringLiteral("record_checks")).toArray().size() != 2 ||
        evidence.value(QStringLiteral("authorities")).toArray().size() != 30) {
        fail("realism review/evidence 4/44/48/3/2/30 envelope drift");
    }

    const QHash<QString, int> expected_dimension_counts{
        {QStringLiteral("bench_differentiation"), 4}, {QStringLiteral("consequences"), 28},
        {QStringLiteral("deadlines_authority"), 15},  {QStringLiteral("oral_argument"), 24},
        {QStringLiteral("procedural_law"), 40},       {QStringLiteral("provenance"), 85},
        {QStringLiteral("record_consistency"), 51},
    };
    QSet<QString> evidence_ids;
    for (const auto& group :
         {QStringLiteral("resources"), QStringLiteral("blobs"), QStringLiteral("traces"),
          QStringLiteral("record_checks"), QStringLiteral("authorities")}) {
        for (const auto& value : evidence.value(group).toArray()) {
            const auto id = value.toObject().value(QStringLiteral("evidence_id")).toString();
            if (id.isEmpty() || evidence_ids.contains(id))
                fail("empty or duplicate realism evidence ID");
            evidence_ids.insert(id);
        }
    }
    if (evidence_ids.size() != 127 || dimension_evidence.size() != 7)
        fail("127-ID/seven-dimension evidence closure drift");
    for (auto iterator = expected_dimension_counts.constBegin();
         iterator != expected_dimension_counts.constEnd(); ++iterator) {
        const auto references = dimension_evidence.value(iterator.key()).toArray();
        const auto unique = strings(references);
        if (references.size() != iterator.value() || unique.size() != references.size() ||
            std::ranges::any_of(unique,
                                [&](const auto& id) { return !evidence_ids.contains(id); })) {
            fail("dimension evidence drift: " + iterator.key().toStdString());
        }
    }

    QSet<QString> record_checks;
    for (const auto& value : evidence.value(QStringLiteral("record_checks")).toArray()) {
        const auto check = value.toObject();
        record_checks.insert(check.value(QStringLiteral("evidence_id")).toString() + u'|' +
                             check.value(QStringLiteral("check_id")).toString() + u'|' +
                             check.value(QStringLiteral("digest")).toString());
    }
    const QSet<QString> expected_record_checks{
        QStringLiteral(
            "workbench.evidence.record-check.fa0f7ede6989048f1a8a6571e9d4c718dddbb3d36c0da9f637"
            "dc45fe4cc9178b|workbench.check.asset-resolution."
            "c38692ac34e2560471cec47f9a65c14f896749507690d4a735565162479e11f3|"
            "1b449ac773d73d573564b36c749e34b6242735afee8bdc7721ac7918b2f00238"),
        QStringLiteral(
            "workbench.evidence.record-check.dfed9cf253108ac5bdd4c4c1640b7e68cca6103247d26cf3209"
            "c499d5e45f3cf|workbench.check.page-anchor-resolution."
            "faaaba0f6e52a58844c44ab26f9c2f4a4f66df42d39d9561cc5862158585eb8a|"
            "71e7a7d9ef03d87d8b279679e25b26795c1027bb26986409d67cd631d41612ee"),
    };
    if (record_checks != expected_record_checks)
        fail("asset/page-anchor evidence formula pins drift");

    int case_authorities = 0;
    int ca4_authorities = 0;
    int federal_authorities = 0;
    for (const auto& value : evidence.value(QStringLiteral("authorities")).toArray()) {
        const auto id = value.toObject().value(QStringLiteral("authority_id")).toString();
        case_authorities += id.startsWith(QStringLiteral("ca4m4.cinder.")) ? 1 : 0;
        ca4_authorities += id.startsWith(QStringLiteral("us.ca4.")) ? 1 : 0;
        federal_authorities += id.startsWith(QStringLiteral("us.federal.")) ? 1 : 0;
    }
    if (case_authorities != 15 || ca4_authorities != 7 || federal_authorities != 8)
        fail("30-authority 15+7+8 partition drift");

    const auto entries = record.value(QStringLiteral("docket_entries")).toArray();
    const auto anchors = record.value(QStringLiteral("page_anchors")).toArray();
    const auto disclosures = record.value(QStringLiteral("sealed_disclosures")).toArray();
    if (record.value(QStringLiteral("dockets")).toArray().size() != 4 || entries.size() != 48 ||
        anchors.size() != 402 || disclosures.size() != 4 ||
        record.value(QStringLiteral("disclosure_policy")).toObject().isEmpty()) {
        fail("record 4-docket/48-entry/402-anchor/4-disclosure envelope drift");
    }

    QHash<QString, QJsonObject> entries_by_id;
    QHash<QString, QJsonObject> entries_by_path;
    QHash<QString, int> docket_documents;
    QHash<QString, int> docket_pages;
    QHash<QString, int> public_documents;
    int sealed_documents = 0;
    for (const auto& value : entries) {
        const auto entry = value.toObject();
        const auto id = entry.value(QStringLiteral("entry_id")).toString();
        const auto path = entry.value(QStringLiteral("asset_path")).toString();
        const auto docket = entry.value(QStringLiteral("docket_id")).toString();
        if (id.isEmpty() || entries_by_id.contains(id) || entries_by_path.contains(path))
            fail("record entry identity drift: " + id.toStdString());
        const auto counterfactual = docket.contains(QStringLiteral(".counterfactual-"));
        const auto tags = strings(entry.value(QStringLiteral("tags")).toArray());
        if (counterfactual != tags.contains(QStringLiteral("never_filed")) ||
            counterfactual != tags.contains(QStringLiteral("never_occurred_on_actual_docket"))) {
            fail("actual/counterfactual record tag isolation drift: " + id.toStdString());
        }
        entries_by_id.insert(id, entry);
        entries_by_path.insert(path, entry);
        ++docket_documents[docket];
        docket_pages[docket] += entry.value(QStringLiteral("page_count")).toInt();
        if (entry.value(QStringLiteral("sealed")).toBool())
            ++sealed_documents;
        else
            ++public_documents[docket];
    }
    const auto district = QStringLiteral("ca4m4.cinder.docket.district");
    const auto actual = QStringLiteral("ca4m4.cinder.docket.appellate");
    const auto deficiency = QStringLiteral("ca4m4.cinder.docket.counterfactual-deficiency");
    const auto summary = QStringLiteral("ca4m4.cinder.docket.counterfactual-summary-denial");
    if (docket_documents.value(district) != 23 || docket_pages.value(district) != 196 ||
        docket_documents.value(actual) != 19 || docket_pages.value(actual) != 153 ||
        docket_documents.value(deficiency) != 3 || docket_pages.value(deficiency) != 31 ||
        docket_documents.value(summary) != 3 || docket_pages.value(summary) != 22 ||
        public_documents.value(district) != 21 || public_documents.value(actual) != 17 ||
        public_documents.value(deficiency) != 3 || public_documents.value(summary) != 3 ||
        sealed_documents != 4) {
        fail("record split is not exact 23/196 + 19/153 + 3/31 + 3/22, public44");
    }

    QHash<QString, int> anchors_by_entry;
    QHash<QString, QString> entry_by_anchor;
    for (const auto& value : anchors) {
        const auto anchor = value.toObject();
        const auto anchor_id = anchor.value(QStringLiteral("anchor_id")).toString();
        const auto entry_id = anchor.value(QStringLiteral("entry_id")).toString();
        const auto entry = objectAt(entries_by_id, entry_id, "anchor entry");
        const auto page = anchor.value(QStringLiteral("page_number")).toInt();
        if (anchor_id.isEmpty() || entry_by_anchor.contains(anchor_id) || page < 1 ||
            page > entry.value(QStringLiteral("page_count")).toInt()) {
            fail("record anchor identity/page drift: " + anchor_id.toStdString());
        }
        entry_by_anchor.insert(anchor_id, entry_id);
        ++anchors_by_entry[entry_id];
    }
    for (auto iterator = entries_by_id.constBegin(); iterator != entries_by_id.constEnd();
         ++iterator) {
        if (anchors_by_entry.value(iterator.key()) !=
            iterator.value().value(QStringLiteral("page_count")).toInt()) {
            fail("record entry/page-anchor count drift: " + iterator.key().toStdString());
        }
    }

    QSet<QString> sealed_twins;
    QSet<QString> stable_anchors;
    QSet<QString> sealed_anchors;
    QSet<QString> public_anchors;
    int mapping_count = 0;
    for (const auto& value : disclosures) {
        const auto disclosure = value.toObject();
        const auto sealed_id = disclosure.value(QStringLiteral("sealed_entry_id")).toString();
        const auto public_id = disclosure.value(QStringLiteral("public_entry_id")).toString();
        const auto sealed_entry = objectAt(entries_by_id, sealed_id, "sealed twin");
        const auto public_entry = objectAt(entries_by_id, public_id, "public twin");
        if (sealed_twins.contains(sealed_id) ||
            !sealed_entry.value(QStringLiteral("sealed")).toBool() ||
            public_entry.value(QStringLiteral("sealed")).toBool() ||
            sealed_entry.value(QStringLiteral("page_count")).toInt() !=
                public_entry.value(QStringLiteral("page_count")).toInt()) {
            fail("sealed/public twin drift: " + sealed_id.toStdString());
        }
        sealed_twins.insert(sealed_id);
        for (const auto& mapping_value :
             disclosure.value(QStringLiteral("anchor_mappings")).toArray()) {
            const auto mapping = mapping_value.toObject();
            const auto stable = mapping.value(QStringLiteral("stable_anchor_id")).toString();
            const auto sealed_anchor = mapping.value(QStringLiteral("sealed_anchor_id")).toString();
            const auto public_anchor = mapping.value(QStringLiteral("public_anchor_id")).toString();
            if (stable.isEmpty() || stable_anchors.contains(stable) ||
                sealed_anchors.contains(sealed_anchor) || public_anchors.contains(public_anchor) ||
                entry_by_anchor.value(sealed_anchor) != sealed_id ||
                entry_by_anchor.value(public_anchor) != public_id) {
                fail("sealed stable-anchor mapping drift: " + stable.toStdString());
            }
            stable_anchors.insert(stable);
            sealed_anchors.insert(sealed_anchor);
            public_anchors.insert(public_anchor);
            entry_by_anchor.insert(stable, public_id);
            ++mapping_count;
        }
    }
    if (sealed_twins.size() != 4 || stable_anchors.size() != 52 || sealed_anchors.size() != 52 ||
        public_anchors.size() != 52 || mapping_count != 52) {
        fail("sealed-record four-twin/52-mapping closure drift");
    }

    QSet<QString> blob_paths;
    for (const auto& value : blobs) {
        const auto descriptor = value.toObject();
        const auto path = descriptor.value(QStringLiteral("path")).toString();
        const auto bytes = readAll(pack_root.filePath(path));
        const auto entry = objectAt(entries_by_path, path, "blob entry");
        if (blob_paths.contains(path) ||
            descriptor.value(QStringLiteral("media_type")).toString() !=
                QStringLiteral("application/pdf") ||
            descriptor.value(QStringLiteral("byte_size")).toInteger() != bytes.size() ||
            descriptor.value(QStringLiteral("sha256")).toString().toLatin1() != sha256(bytes) ||
            entry.value(QStringLiteral("asset_sha256")).toString() !=
                descriptor.value(QStringLiteral("sha256")).toString()) {
            fail("manifest PDF descriptor drift: " + path.toStdString());
        }
        QPdfDocument pdf;
        if (pdf.load(pack_root.filePath(path)) != QPdfDocument::Error::None ||
            pdf.status() != QPdfDocument::Status::Ready ||
            pdf.pageCount() != entry.value(QStringLiteral("page_count")).toInt()) {
            fail("manifest PDF/page count cannot be loaded: " + path.toStdString());
        }
        blob_paths.insert(path);
    }
    if (blob_paths.size() != 48 || entries_by_path.size() != 48)
        fail("48-PDF manifest/record path closure drift");

    const auto argumentEntries = [&](const QJsonObject& configuration) {
        QSet<QString> result;
        const auto bank = configuration.value(QStringLiteral("grounded_question_bank")).toObject();
        for (const auto& question_value : bank.value(QStringLiteral("questions")).toArray()) {
            for (const auto& grounding_value :
                 question_value.toObject().value(QStringLiteral("grounding")).toArray()) {
                const auto grounding = grounding_value.toObject();
                if (grounding.value(QStringLiteral("kind")).toString() !=
                    QStringLiteral("record_page")) {
                    continue;
                }
                const auto anchor = grounding.value(QStringLiteral("anchor_id")).toString();
                if (!entry_by_anchor.contains(anchor))
                    fail("argument grounding anchor is absent: " + anchor.toStdString());
                result.insert(entry_by_anchor.value(anchor));
            }
        }
        return result;
    };
    const auto actual_entries = argumentEntries(actual_argument);
    const auto counterfactual_entries = argumentEntries(counterfactual_argument);
    const auto actual_bank =
        actual_argument.value(QStringLiteral("grounded_question_bank")).toObject();
    const auto counterfactual_bank =
        counterfactual_argument.value(QStringLiteral("grounded_question_bank")).toObject();
    if (actual_bank.value(QStringLiteral("mode")).toString() != QStringLiteral("actual_record") ||
        actual_bank.value(QStringLiteral("grounding_digest")).toString() !=
            QStringLiteral("6800db5538c47b4a2940dec743e30bd463986e9f741944b639e7446f3451ce0d") ||
        actual_bank.value(QStringLiteral("questions")).toArray().size() != 16 ||
        actual_entries.size() != 14 ||
        std::ranges::any_of(actual_entries,
                            [&](const auto& id) {
                                return entries_by_id.value(id)
                                    .value(QStringLiteral("docket_id"))
                                    .toString()
                                    .contains(QStringLiteral(".counterfactual-"));
                            }) ||
        counterfactual_bank.value(QStringLiteral("mode")).toString() !=
            QStringLiteral("counterfactual_training") ||
        counterfactual_bank.value(QStringLiteral("grounding_digest")).toString() !=
            QStringLiteral("19e61fe1ead8a35fb5d3a1688e04b7e9eb46966bd33dace6c9195b2fdc5c430d") ||
        counterfactual_bank.value(QStringLiteral("questions")).toArray().size() != 8 ||
        counterfactual_entries.size() != 6 ||
        std::ranges::any_of(counterfactual_entries, [&](const auto& id) {
            return !entries_by_id.value(id)
                        .value(QStringLiteral("docket_id"))
                        .toString()
                        .contains(QStringLiteral(".counterfactual-"));
        })) {
        fail("actual/counterfactual argument grounding isolation drift");
    }

    const auto plans = case_definition.value(QStringLiteral("disposition_plans")).toArray();
    QHash<QString, QJsonObject> plans_by_id;
    for (const auto& value : plans) {
        const auto plan = value.toObject();
        plans_by_id.insert(plan.value(QStringLiteral("plan_id")).toString(), plan);
    }
    const auto actual_plan = plans_by_id.value(
        QStringLiteral("ca4m4.cinder.disposition.actual-partial-grant-vacatur-remand"));
    const auto counterfactual_plan =
        plans_by_id.value(QStringLiteral("ca4m4.cinder.disposition.counterfactual-summary-denial"));
    const auto actions = [](const QJsonObject& plan) {
        QStringList result;
        for (const auto& value : plan.value(QStringLiteral("components")).toArray())
            result.push_back(value.toObject().value(QStringLiteral("action")).toString());
        return result;
    };
    if (case_definition.value(QStringLiteral("actors")).toArray().size() != 5 ||
        case_definition.value(QStringLiteral("issues")).toArray().size() != 5 ||
        plans.size() != 2 || plans_by_id.size() != 2 ||
        case_definition.value(QStringLiteral("authored_disposition_id")).toString() !=
            QStringLiteral("ca4m4.cinder.operation.issue-actual-judgment") ||
        case_definition.value(QStringLiteral("authored_disposition_plan_id")).toString() !=
            QStringLiteral("ca4m4.cinder.disposition.actual-partial-grant-vacatur-remand") ||
        actual_plan.value(QStringLiteral("digest")).toString() !=
            QStringLiteral("fb287ac6047317c87ea29e6ae7545829795c25c0ba8480b2e548b1511361d519") ||
        counterfactual_plan.value(QStringLiteral("digest")).toString() !=
            QStringLiteral("c053d31abece1d6c4d14fcafc54d3dd7b8845c8fc0b7fcd67f083c2fd23fe8e9") ||
        actions(actual_plan) != QStringList{QStringLiteral("grant"), QStringLiteral("vacate"),
                                            QStringLiteral("vacate"), QStringLiteral("deny")} ||
        actions(counterfactual_plan) != QStringList{QStringLiteral("deny")}) {
        fail("five-actor/five-issue/two-disposition contract drift");
    }

    const auto stages = workflow.value(QStringLiteral("stages")).toArray();
    const auto operations = workflow.value(QStringLiteral("operations")).toArray();
    const auto routes = workflow.value(QStringLiteral("filing_routes")).toArray();
    if (workflow.value(QStringLiteral("resource_id")).toString().toStdString() != workflow_id ||
        stages.size() != 15 || operations.size() != 45 || routes.size() != 8) {
        fail("workflow 15-stage/45-operation/8-route envelope drift");
    }
    const auto stage_ids = strings(stages);
    QHash<QString, QJsonObject> operations_by_id;
    QHash<QString, int> opcode_counts;
    QSet<QString> bound_entries;
    int document_bindings = 0;
    int disposition_bindings = 0;
    for (const auto& value : operations) {
        const auto operation = value.toObject();
        const auto id = operation.value(QStringLiteral("operation_id")).toString();
        const auto opcode = operation.value(QStringLiteral("opcode")).toString();
        if (id.isEmpty() || operations_by_id.contains(id) ||
            !stage_ids.contains(operation.value(QStringLiteral("stage_id")).toString()) ||
            operation.value(QStringLiteral("allowed_legal_times")).toArray().isEmpty()) {
            fail("invalid or unguarded operation: " + id.toStdString());
        }
        ++opcode_counts[opcode];
        if (operation.contains(QStringLiteral("document_binding"))) {
            const auto binding = operation.value(QStringLiteral("document_binding")).toObject();
            const auto entry_id = binding.value(QStringLiteral("record_entry_id")).toString();
            const auto entry = objectAt(entries_by_id, entry_id, "document-bound entry");
            if (entry.value(QStringLiteral("sealed")).toBool() ||
                binding.value(QStringLiteral("document_sha256")).toString() !=
                    entry.value(QStringLiteral("asset_sha256")).toString()) {
                fail("operation document binding drift: " + id.toStdString());
            }
            bound_entries.insert(entry_id);
            ++document_bindings;
        }
        disposition_bindings += operation.contains(QStringLiteral("disposition_plan_id")) ? 1 : 0;
        operations_by_id.insert(id, operation);
    }
    QSet<QString> filing_ids;
    int filing_bindings = 0;
    for (const auto& value : routes) {
        const auto route = value.toObject();
        if (route.value(QStringLiteral("authorized_role_scope")).toString() !=
                QStringLiteral("catalog_subset") ||
            !operations_by_id.contains(
                route.value(QStringLiteral("accept_operation_id")).toString()) ||
            !operations_by_id.contains(
                route.value(QStringLiteral("reject_operation_id")).toString())) {
            fail("filing route role/operation reference drift");
        }
        for (const auto& binding_value : route.value(QStringLiteral("filing_bindings")).toArray()) {
            const auto binding = binding_value.toObject();
            const auto filing_id = binding.value(QStringLiteral("filing_id")).toString();
            const auto entry_id = binding.value(QStringLiteral("record_entry_id")).toString();
            const auto entry = objectAt(entries_by_id, entry_id, "filing-bound entry");
            if (filing_id.isEmpty() || filing_ids.contains(filing_id) ||
                entry.value(QStringLiteral("sealed")).toBool() ||
                binding.value(QStringLiteral("document_sha256")).toString() !=
                    entry.value(QStringLiteral("asset_sha256")).toString()) {
                fail("filing binding identity/digest drift: " + filing_id.toStdString());
            }
            filing_ids.insert(filing_id);
            bound_entries.insert(entry_id);
            ++filing_bindings;
        }
    }
    const QHash<QString, int> expected_opcodes{
        {QStringLiteral("accept_filing"), 8},     {QStringLiteral("reject_filing"), 8},
        {QStringLiteral("issue_deficiency"), 1},  {QStringLiteral("calculate_deadline"), 5},
        {QStringLiteral("enter_order"), 8},       {QStringLiteral("advance_stage"), 12},
        {QStringLiteral("schedule_argument"), 1}, {QStringLiteral("issue_judgment"), 2},
    };
    if (stage_ids.size() != 15 || operations_by_id.size() != 45 ||
        opcode_counts != expected_opcodes || filing_bindings != 10 || filing_ids.size() != 10 ||
        document_bindings != 10 || disposition_bindings != 2 || bound_entries.size() != 20) {
        fail("workflow 15/45/8/10-filing/10-document topology drift");
    }

    const auto actual_selector = operations_by_id.value(
        QStringLiteral("ca4m4.cinder.operation.advance-a01-to-actual-sealing-support"));
    const auto summary_selector = operations_by_id.value(
        QStringLiteral("ca4m4.cinder.operation.advance-b04-to-counterfactual-summary"));
    if (!hasFilingPrecondition(actual_selector, u"ca4m4.cinder.filing.a01-public-writ-petition",
                               true) ||
        !hasFilingPrecondition(actual_selector, u"ca4m4.cinder.filing.b03-corrected-petition",
                               false) ||
        !hasFilingPrecondition(actual_selector,
                               u"ca4m4.cinder.filing.b04-conforming-incomplete-petition", false) ||
        !hasFilingPrecondition(summary_selector,
                               u"ca4m4.cinder.filing.b04-conforming-incomplete-petition", true) ||
        !hasFilingPrecondition(summary_selector, u"ca4m4.cinder.filing.a01-public-writ-petition",
                               false) ||
        !hasFilingPrecondition(summary_selector, u"ca4m4.cinder.filing.b03-corrected-petition",
                               false)) {
        fail("submitted-stage three-petition branch mutex drift");
    }

    const auto opened_route = routes.at(0).toObject();
    const auto cure_route = routes.at(1).toObject();
    const auto deficiency_deadline =
        opened_route.value(QStringLiteral("deficiency_deadline")).toObject();
    const auto trigger = deficiency_deadline.value(QStringLiteral("trigger_filing")).toObject();
    const auto b02 = operations_by_id.value(
        QStringLiteral("ca4m4.cinder.operation.enter-b02-deficiency-notice"));
    const auto b03_bindings = cure_route.value(QStringLiteral("filing_bindings")).toArray();
    if (opened_route.value(QStringLiteral("filing_bindings")).toArray().size() != 0 ||
        opened_route.value(QStringLiteral("deficiency_operation_id")).toString() !=
            QStringLiteral("ca4m4.cinder.operation.issue-b01-petition-deficiency") ||
        deficiency_deadline.value(QStringLiteral("deadline_id")).toString() !=
            QStringLiteral("ca4m4.cinder.deadline.b01-petition-cure") ||
        deficiency_deadline.value(QStringLiteral("id_mode")).toString() !=
            QStringLiteral("exact") ||
        trigger.value(QStringLiteral("filing_id")).toString() !=
            QStringLiteral("ca4m4.cinder.filing.b01-defective-petition") ||
        b02.value(QStringLiteral("document_binding"))
                .toObject()
                .value(QStringLiteral("disposition"))
                .toString() != QStringLiteral("other") ||
        !hasDeadlineStatus(b02, u"ca4m4.cinder.deadline.b01-petition-cure", u"open") ||
        b03_bindings.size() != 1 ||
        b03_bindings.at(0).toObject().value(QStringLiteral("filing_id")).toString() !=
            QStringLiteral("ca4m4.cinder.filing.b03-corrected-petition") ||
        cure_route.value(QStringLiteral("satisfies_deadline_id")).toString() !=
            QStringLiteral("ca4m4.cinder.deadline.b01-petition-cure") ||
        !cure_route.value(QStringLiteral("reject_after_deadline")).toBool()) {
        fail("B01 deficiency/B02 notice/B03 exact-cure contract drift");
    }

    const auto a12 = operations_by_id.value(
        QStringLiteral("ca4m4.cinder.operation.enter-a12-answer-invitation"));
    const auto a15 = operations_by_id.value(
        QStringLiteral("ca4m4.cinder.operation.enter-a15-expedited-argument-order"));
    const auto schedule =
        operations_by_id.value(QStringLiteral("ca4m4.cinder.operation.schedule-a15-oral-argument"));
    const auto actual_judgment =
        operations_by_id.value(QStringLiteral("ca4m4.cinder.operation.issue-actual-judgment"));
    const auto summary_judgment = operations_by_id.value(
        QStringLiteral("ca4m4.cinder.operation.issue-counterfactual-summary-judgment"));
    const auto actual_rule40 = operations_by_id.value(
        QStringLiteral("ca4m4.cinder.operation.calculate-actual-rule40-deadline"));
    const auto summary_rule40 = operations_by_id.value(
        QStringLiteral("ca4m4.cinder.operation.calculate-counterfactual-summary-rule40-deadline"));
    const auto actual_terminal = operations_by_id.value(
        QStringLiteral("ca4m4.cinder.operation.advance-actual-rehearing-expired-to-terminal"));
    const auto summary_terminal = operations_by_id.value(QStringLiteral(
        "ca4m4.cinder.operation.advance-counterfactual-summary-rehearing-expired-to-terminal"));
    if (a12.value(QStringLiteral("document_binding"))
                .toObject()
                .value(QStringLiteral("disposition"))
                .toString() != QStringLiteral("other") ||
        a15.value(QStringLiteral("document_binding"))
                .toObject()
                .value(QStringLiteral("disposition"))
                .toString() != QStringLiteral("other") ||
        schedule.value(QStringLiteral("expected_argument_date")).toString() !=
            QStringLiteral("2026-04-21") ||
        schedule.value(QStringLiteral("allowed_legal_times")).toArray().size() != 2 ||
        actual_judgment.value(QStringLiteral("disposition_plan_id")).toString() !=
            QStringLiteral("ca4m4.cinder.disposition.actual-partial-grant-vacatur-remand") ||
        summary_judgment.value(QStringLiteral("disposition_plan_id")).toString() !=
            QStringLiteral("ca4m4.cinder.disposition.counterfactual-summary-denial") ||
        actual_rule40.value(QStringLiteral("deadline_days")).toInt() != 14 ||
        summary_rule40.value(QStringLiteral("deadline_days")).toInt() != 14 ||
        actual_rule40.value(QStringLiteral("deadline_event_base"))
                .toObject()
                .value(QStringLiteral("kind"))
                .toString() != QStringLiteral("judgment_occurred") ||
        summary_rule40.value(QStringLiteral("deadline_event_base"))
                .toObject()
                .value(QStringLiteral("kind"))
                .toString() != QStringLiteral("judgment_occurred") ||
        !hasDeadlineStatus(actual_terminal, u"ca4m4.cinder.deadline.actual-rule40", u"reached") ||
        !hasDeadlineStatus(summary_terminal, u"ca4m4.cinder.deadline.counterfactual-summary-rule40",
                           u"reached")) {
        fail("answer/argument/disposition/Rule40 contract drift");
    }

    QCryptographicHash trace_closure(QCryptographicHash::Sha256);
    QHash<QString, QJsonObject> authored_traces;
    for (const auto& value : evidence.value(QStringLiteral("traces")).toArray()) {
        const auto trace = value.toObject();
        authored_traces.insert(trace.value(QStringLiteral("trace_id")).toString(), trace);
    }
    for (const auto& meta : trace_metas) {
        const auto relative_path = QStringLiteral("traces/%1").arg(meta.file);
        const auto bytes = readAll(root.filePath(relative_path));
        const auto repo_path =
            QStringLiteral("content/m4/cinderlake-writ/%1").arg(relative_path).toUtf8();
        trace_closure.addData(QByteArrayView(repo_path));
        trace_closure.addData(QByteArrayView("\0", 1));
        trace_closure.addData(QByteArrayView(bytes));
        trace_closure.addData(QByteArrayView("\0", 1));
        const auto trace = parseObject(bytes, relative_path);
        if (authored_traces.value(trace.value(QStringLiteral("trace_id")).toString()) != trace)
            fail("authored realism trace differs from committed trace: " +
                 relative_path.toStdString());
    }
    if (trace_closure.result().toHex().toStdString() != ReleasePins::trace_closure ||
        authored_traces.size() != 3) {
        fail("three-trace framed closure/evidence binding drift");
    }

    const std::array candidate_pairs{
        std::pair{QStringLiteral("resources/argument-actual.candidate.json"),
                  QStringLiteral("pack-candidate/resources/argument-actual.json")},
        std::pair{QStringLiteral("resources/argument-counterfactual.candidate.json"),
                  QStringLiteral("pack-candidate/resources/argument-counterfactual.json")},
        std::pair{QStringLiteral("resources/authority-set.candidate.json"),
                  QStringLiteral("pack-candidate/resources/authority-set.json")},
        std::pair{QStringLiteral("resources/bench-configuration.candidate.json"),
                  QStringLiteral("pack-candidate/resources/bench-configuration.json")},
        std::pair{QStringLiteral("resources/case.candidate.json"),
                  QStringLiteral("pack-candidate/resources/case.json")},
        std::pair{QStringLiteral("resources/procedure-profile.candidate.json"),
                  QStringLiteral("pack-candidate/resources/procedure-profile.json")},
        std::pair{QStringLiteral("resources/record.candidate.json"),
                  QStringLiteral("pack-candidate/resources/record.json")},
        std::pair{QStringLiteral("resources/workflow.candidate.json"),
                  QStringLiteral("pack-candidate/resources/workflow.json")},
    };
    for (const auto& [candidate, promoted] : candidate_pairs) {
        if (readAll(root.filePath(candidate)) != readAll(root.filePath(promoted)))
            fail("candidate/promoted resource drift: " + candidate.toStdString());
    }
}

[[nodiscard]] std::set<std::string> expectedTraceEntries(std::string_view label) {
    std::string_view codes;
    if (label == "actual") {
        codes = "a01 a02 a03 a04 a06 a08 a09 a10 a11 a12 a13 a14 a15 a16 a17 a18 a19";
    } else if (label == "deficiency") {
        codes = "b01 b02 b03";
    } else if (label == "summary") {
        codes = "b04 b05 b06";
    } else {
        fail("unknown Cinder trace label");
    }

    std::set<std::string> result;
    std::size_t start{};
    while (start < codes.size()) {
        const auto end = codes.find(' ', start);
        result.emplace("ca4m4.cinder.record.entry." +
                       std::string(codes.substr(start, end == codes.npos ? codes.size() - start
                                                                         : end - start)));
        if (end == codes.npos)
            break;
        start = end + 1U;
    }
    return result;
}

[[nodiscard]] std::vector<model::WorkflowJournalEntry> decodeJournal(const QJsonArray& values,
                                                                     const std::string& label) {
    std::vector<model::WorkflowJournalEntry> result;
    result.reserve(static_cast<std::size_t>(values.size()));
    for (qsizetype index = 0; index < values.size(); ++index) {
        const auto object = values.at(index).toObject();
        if (object.keys() !=
            QStringList{QStringLiteral("command_base64"), QStringLiteral("events_base64")}) {
            fail(label + " journal entry envelope drift at " + std::to_string(index));
        }
        const auto encoded_command =
            object.value(QStringLiteral("command_base64")).toString().toLatin1();
        const auto command_bytes = QByteArray::fromBase64(encoded_command);
        const auto command = storage::decodeWorkflowCommand(QByteArrayView(command_bytes));
        if (command_bytes.isEmpty() || command_bytes.toBase64() != encoded_command || !command ||
            storage::encodeWorkflowCommand(*command) != command_bytes) {
            fail(label + " command codec/base64 noncanonical at " + std::to_string(index));
        }
        std::vector<model::WorkflowEvent> events;
        const auto encoded_events = object.value(QStringLiteral("events_base64")).toArray();
        events.reserve(static_cast<std::size_t>(encoded_events.size()));
        for (const auto& value : encoded_events) {
            const auto encoded = value.toString().toLatin1();
            const auto event_bytes = QByteArray::fromBase64(encoded);
            const auto event = storage::decodeWorkflowEvent(QByteArrayView(event_bytes));
            if (event_bytes.isEmpty() || event_bytes.toBase64() != encoded || !event ||
                storage::encodeWorkflowEvent(*event) != event_bytes) {
                fail(label + " event codec/base64 noncanonical at " + std::to_string(index));
            }
            events.push_back(*event);
        }
        result.push_back(model::WorkflowJournalEntry{*command, std::move(events)});
    }
    return result;
}

[[nodiscard]] model::WorkflowState initialState(const model::WorkflowDefinition& workflow,
                                                const model::WorkflowCommand& first) {
    model::WorkflowState state;
    state.session_id = commandHeader(first).session_id;
    state.workflow_id = workflow.id;
    state.current_stage_id = workflow.initial_stage_id;
    return state;
}

template <typename Mutator>
void rejectTamper(const TraceMeta& meta, const packs::RuntimeCase& runtime_case,
                  const model::WorkflowState& initial,
                  const std::vector<model::WorkflowJournalEntry>& source, std::string_view name,
                  Mutator mutate) {
    auto copy = source;
    if (!mutate(copy))
        fail(meta.label + " had no target for " + std::string(name) + " tamper");
    const auto replayed =
        engine::replayWorkflow(runtime_case.workflow, runtime_case.definition, initial, copy);
    if (replayed || replayed.error().code != engine::WorkflowErrorCode::InvalidEvent)
        fail(meta.label + " did not fail closed as InvalidEvent for " + std::string(name));
}

[[nodiscard]] std::size_t auditTampers(const TraceMeta& meta,
                                       const packs::RuntimeCase& runtime_case,
                                       const model::WorkflowState& initial,
                                       const std::vector<model::WorkflowJournalEntry>& source) {
    rejectTamper(meta, runtime_case, initial, source, "event-sequence", [](auto& journal) {
        ++eventHeader(journal.front().events.front()).sequence;
        return true;
    });
    rejectTamper(meta, runtime_case, initial, source, "LegalTime-instant", [](auto& journal) {
        commandHeader(journal.front().command).occurred_at.instant += 1s;
        return true;
    });
    rejectTamper(meta, runtime_case, initial, source, "document-SHA", [](auto& journal) {
        for (auto& entry : journal) {
            bool changed = false;
            std::visit(
                [&](auto& command) {
                    if constexpr (requires { command.document_sha256; }) {
                        if (!command.document_sha256.empty()) {
                            command.document_sha256.front() =
                                command.document_sha256.front() == '0' ? '1' : '0';
                            changed = true;
                        }
                    }
                },
                entry.command);
            if (changed)
                return true;
        }
        return false;
    });
    rejectTamper(meta, runtime_case, initial, source, "operation-ID", [](auto& journal) {
        eventHeader(journal.back().events.back()).operation_id =
            model::WorkflowOperationId{"ca4m4.cinder.operation.hostile-tamper"};
        return true;
    });
    if (meta.label != "actual")
        return 4U;

    rejectTamper(meta, runtime_case, initial, source, "actor-substitution", [&](auto& journal) {
        for (auto& entry : journal) {
            auto* filing = std::get_if<model::SubmitWorkflowFiling>(&entry.command);
            if (!filing || entry.events.empty() ||
                !std::holds_alternative<model::WorkflowFilingAccepted>(entry.events.front())) {
                continue;
            }
            const auto other =
                std::ranges::find_if(runtime_case.definition.actors, [&](const auto& actor) {
                    return actor.id != filing->header.actor_id;
                });
            if (other == runtime_case.definition.actors.end())
                return false;
            filing->header.actor_id = other->id;
            return true;
        }
        return false;
    });
    rejectTamper(meta, runtime_case, initial, source, "required-service", [](auto& journal) {
        for (auto& entry : journal) {
            auto* filing = std::get_if<model::SubmitWorkflowFiling>(&entry.command);
            if (filing && !entry.events.empty() &&
                std::holds_alternative<model::WorkflowFilingAccepted>(entry.events.front())) {
                filing->served_actors.clear();
                return true;
            }
        }
        return false;
    });
    rejectTamper(meta, runtime_case, initial, source, "order-ID", [](auto& journal) {
        for (auto& entry : journal) {
            if (auto* order = std::get_if<model::EnterWorkflowOrder>(&entry.command)) {
                order->order_id = model::WorkflowOrderId{"ca4m4.cinder.order.hostile-tamper"};
                return true;
            }
        }
        return false;
    });
    rejectTamper(meta, runtime_case, initial, source, "order-disposition", [](auto& journal) {
        for (auto& entry : journal) {
            if (auto* order = std::get_if<model::EnterWorkflowOrder>(&entry.command)) {
                order->disposition = order->disposition == model::WorkflowOrderDisposition::Granted
                                         ? model::WorkflowOrderDisposition::Denied
                                         : model::WorkflowOrderDisposition::Granted;
                return true;
            }
        }
        return false;
    });
    rejectTamper(meta, runtime_case, initial, source, "deadline-date", [](auto& journal) {
        for (auto& entry : journal) {
            for (auto& event : entry.events) {
                if (auto* deadline = std::get_if<model::WorkflowDeadlineCalculated>(&event)) {
                    deadline->due_date.value = std::chrono::year_month_day{
                        std::chrono::sys_days{deadline->due_date.value} + std::chrono::days{1}};
                    return true;
                }
            }
        }
        return false;
    });
    rejectTamper(meta, runtime_case, initial, source, "deadline-ID", [](auto& journal) {
        for (auto& entry : journal) {
            for (auto& event : entry.events) {
                if (auto* deadline = std::get_if<model::WorkflowDeadlineCalculated>(&event)) {
                    deadline->deadline_id =
                        model::WorkflowDeadlineId{"ca4m4.cinder.deadline.hostile-tamper"};
                    return true;
                }
            }
        }
        return false;
    });
    rejectTamper(meta, runtime_case, initial, source, "deadline-event-base", [](auto& journal) {
        for (auto& entry : journal) {
            for (auto& event : entry.events) {
                auto* deadline = std::get_if<model::WorkflowDeadlineCalculated>(&event);
                if (!deadline || !deadline->deadline_event_base)
                    continue;
                if (std::holds_alternative<model::WorkflowJudgmentOccurredDeadlineBase>(
                        *deadline->deadline_event_base)) {
                    deadline->deadline_event_base = model::WorkflowOrderOccurredDeadlineBase{
                        model::WorkflowOrderId{"ca4m4.cinder.order.hostile-base"},
                        model::WorkflowOperationId{"ca4m4.cinder.operation.hostile-base"}};
                    return true;
                }
                bool changed = false;
                std::visit(
                    [&](auto& base) {
                        if constexpr (requires { base.operation_id; }) {
                            base.operation_id =
                                model::WorkflowOperationId{"ca4m4.cinder.operation.hostile-base"};
                            changed = true;
                        } else if constexpr (requires { base.operation_ids; }) {
                            if (!base.operation_ids.empty()) {
                                base.operation_ids.front() = model::WorkflowOperationId{
                                    "ca4m4.cinder.operation.hostile-base"};
                                changed = true;
                            }
                        }
                    },
                    *deadline->deadline_event_base);
                if (changed)
                    return true;
            }
        }
        return false;
    });
    rejectTamper(meta, runtime_case, initial, source, "dependent-deadline-base", [](auto& journal) {
        for (auto& entry : journal) {
            for (auto& event : entry.events) {
                auto* deadline = std::get_if<model::WorkflowDeadlineCalculated>(&event);
                if (deadline && deadline->deadline_base_id) {
                    deadline->deadline_base_id =
                        model::WorkflowDeadlineId{"ca4m4.cinder.deadline.hostile-dependent-base"};
                    return true;
                }
            }
        }
        return false;
    });
    rejectTamper(meta, runtime_case, initial, source, "authority", [](auto& journal) {
        eventHeader(journal.front().events.front()).authority.primary.id =
            model::AuthorityId{"us.federal.authority.hostile-tamper"};
        return true;
    });
    rejectTamper(meta, runtime_case, initial, source, "record-provenance", [](auto& journal) {
        for (auto& entry : journal) {
            for (auto& event : entry.events) {
                bool changed = false;
                std::visit(
                    [&](auto& concrete) {
                        for (auto& precondition : concrete.header.preconditions) {
                            std::visit(
                                [&](auto& condition) {
                                    if constexpr (requires { condition.record_entry_id; }) {
                                        if (!changed) {
                                            condition.record_entry_id =
                                                "ca4m4.cinder.record.entry.hostile-tamper";
                                            if constexpr (requires { condition.document_sha256; })
                                                condition.document_sha256 = std::string(64U, '0');
                                            changed = true;
                                        }
                                    }
                                },
                                precondition);
                            if (changed)
                                break;
                        }
                    },
                    event);
                if (changed)
                    return true;
            }
        }
        return false;
    });
    return 14U;
}

struct TraceTotals final {
    std::set<std::string> operations;
    std::map<std::string, std::string> deadlines;
    std::map<std::string, std::size_t> terminals;
    std::size_t commands{};
    std::size_t events{};
    std::size_t prefix_replays{};
    std::size_t full_replays{};
    std::size_t tampers{};
};

struct AuditedTrace final {
    model::WorkflowState initial;
    model::WorkflowState final;
    std::vector<model::WorkflowJournalEntry> journal;
};

[[nodiscard]] AuditedTrace auditTrace(const QDir& trace_dir, const TraceMeta& meta,
                                      const packs::RuntimeCase& runtime_case, TraceTotals& totals) {
    const auto trace_bytes = readAll(trace_dir.filePath(meta.file));
    const auto trace = parseObject(trace_bytes, meta.file);
    const QSet<QString> allowed_keys{
        QStringLiteral("command_count"),     QStringLiteral("digest"),
        QStringLiteral("engine_revision"),   QStringLiteral("event_count"),
        QStringLiteral("evidence_id"),       QStringLiteral("journal"),
        QStringLiteral("journal_sha256"),    QStringLiteral("operation_ids"),
        QStringLiteral("terminal_stage_id"), QStringLiteral("trace_id"),
        QStringLiteral("workflow_id"),
    };
    const auto journal_values = trace.value(QStringLiteral("journal")).toArray();
    const auto computed_journal = journalDigest(journal_values);
    const auto keys = trace.keys();
    if (sha256(trace_bytes).toStdString() != meta.file_sha256 ||
        QSet<QString>(keys.cbegin(), keys.cend()) != allowed_keys ||
        trace.value(QStringLiteral("trace_id")).toString().toStdString() != meta.trace_id ||
        trace.value(QStringLiteral("evidence_id")).toString().toStdString() != meta.evidence_id ||
        trace.value(QStringLiteral("workflow_id")).toString().toStdString() != workflow_id ||
        trace.value(QStringLiteral("engine_revision")).toString() !=
            QStringLiteral("appellate.realism-evidence.codec-replay-multi.v1") ||
        trace.value(QStringLiteral("command_count")).toInteger() !=
            static_cast<qint64>(meta.commands) ||
        trace.value(QStringLiteral("event_count")).toInteger() !=
            static_cast<qint64>(meta.events) ||
        !computed_journal || computed_journal->toStdString() != meta.journal_sha256 ||
        trace.value(QStringLiteral("journal_sha256")).toString().toStdString() !=
            meta.journal_sha256 ||
        traceDigest(trace).toStdString() != meta.digest ||
        trace.value(QStringLiteral("digest")).toString().toStdString() != meta.digest ||
        trace.value(QStringLiteral("terminal_stage_id")).toString().toStdString() !=
            meta.terminal) {
        fail(meta.label + " trace envelope/formula drift");
    }

    auto journal = decodeJournal(journal_values, meta.label);
    if (journal.empty() || journal.size() != meta.commands)
        fail(meta.label + " decoded journal command count drift");
    const auto initial = initialState(runtime_case.workflow, journal.front().command);
    auto rolling = initial;
    std::uint64_t expected_sequence = 1;
    std::vector<std::string> operation_ids;
    std::set<std::string> documents;
    std::size_t event_count = 0;
    for (std::size_t index = 0; index < journal.size(); ++index) {
        const auto& entry = journal.at(index);
        const auto& command_header = commandHeader(entry.command);
        const auto expected_command_id =
            command_header.session_id + ".command." + std::to_string(index + 1U);
        const auto court_date = dateText(command_header.occurred_at.court_date);
        const auto qdate = QDate::fromString(QString::fromStdString(court_date), Qt::ISODate);
        const auto midnight = QDateTime(qdate, QTime(0, 0), QTimeZone::UTC).toSecsSinceEpoch();
        const auto instant = std::chrono::duration_cast<std::chrono::seconds>(
                                 command_header.occurred_at.instant.time_since_epoch())
                                 .count();
        if (command_header.session_id != initial.session_id ||
            command_header.command_id.value != expected_command_id || !qdate.isValid() ||
            midnight != instant) {
            fail(meta.label + " command/session/LegalTime drift at " + std::to_string(index));
        }
        const auto decided = engine::decideWorkflow(runtime_case.workflow, runtime_case.definition,
                                                    rolling, entry.command);
        if (!decided || *decided != entry.events)
            fail(meta.label + " redecision drift at " + std::to_string(index));
        const auto prefix =
            std::span<const model::WorkflowJournalEntry>(journal.data(), index + 1U);
        const auto replayed =
            engine::replayWorkflow(runtime_case.workflow, runtime_case.definition, initial, prefix);
        if (!replayed)
            fail(meta.label + " prefix replay failed at " + std::to_string(index));
        rolling = *replayed;
        ++totals.prefix_replays;

        if (const auto digest = commandDocumentDigest(entry.command)) {
            const auto record_entry = std::ranges::find(runtime_case.record.docket_entries, *digest,
                                                        &packs::RuntimeDocketEntry::asset_sha256);
            if (record_entry == runtime_case.record.docket_entries.end() || record_entry->sealed ||
                record_entry->filed_on != command_header.occurred_at.court_date) {
                fail(meta.label + " document/date/public-record resolution drift at " +
                     std::to_string(index));
            }
            documents.insert(record_entry->id.value);
        }
        for (std::size_t event_index = 0; event_index < entry.events.size(); ++event_index) {
            const auto& event = entry.events.at(event_index);
            const auto& header = eventHeader(event);
            if (header.session_id != initial.session_id ||
                header.command_id != command_header.command_id ||
                header.occurred_at != command_header.occurred_at ||
                header.sequence != expected_sequence || header.command_event_index != event_index ||
                header.command_event_count != entry.events.size()) {
                fail(meta.label + " multi-event header drift at command " + std::to_string(index));
            }
            ++expected_sequence;
            ++event_count;
            operation_ids.push_back(header.operation_id.value);
            totals.operations.insert(header.operation_id.value);
            if (std::holds_alternative<model::WorkflowMandateIssued>(event))
                fail(meta.label + " emitted a forbidden original-proceeding mandate");
            if (const auto* deadline = std::get_if<model::WorkflowDeadlineCalculated>(&event)) {
                const auto [found, inserted] = totals.deadlines.emplace(
                    deadline->deadline_id.value, dateText(deadline->due_date));
                if (!inserted && found->second != dateText(deadline->due_date))
                    fail("deadline changed across canonical traces: " +
                         deadline->deadline_id.value);
            }
        }
    }

    const auto declared_operations = trace.value(QStringLiteral("operation_ids")).toArray();
    if (declared_operations.size() != static_cast<qsizetype>(operation_ids.size()))
        fail(meta.label + " operation list count drift");
    for (qsizetype index = 0; index < declared_operations.size(); ++index) {
        if (declared_operations.at(index).toString().toStdString() !=
            operation_ids.at(static_cast<std::size_t>(index))) {
            fail(meta.label + " operation list order drift");
        }
    }
    if (event_count != meta.events || documents != expectedTraceEntries(meta.label))
        fail(meta.label + " exact event/document coverage drift");

    if (meta.label == "deficiency") {
        if (journal.front().events.size() != 2U || journal.back().events.size() != 2U)
            fail("deficiency trace is not exact 2+1+1+2 event topology");
        const auto* deficiency =
            std::get_if<model::WorkflowDeficiencyIssued>(&journal.front().events.front());
        const auto* deadline =
            std::get_if<model::WorkflowDeadlineCalculated>(&journal.front().events.back());
        const auto* cured =
            std::get_if<model::WorkflowFilingAccepted>(&journal.back().events.front());
        if (!deficiency || !deadline || !cured || deficiency->missing_requirements.size() != 3U ||
            !deficiency->cure_deadline_id ||
            deficiency->cure_deadline_id->value != "ca4m4.cinder.deadline.b01-petition-cure" ||
            deadline->deadline_id.value != "ca4m4.cinder.deadline.b01-petition-cure" ||
            dateText(deadline->due_date) != "2026-04-03" || !cured->cured_deficiency_id ||
            cured->cured_deficiency_id != deficiency->deficiency_id ||
            !cured->satisfied_deadline_id ||
            cured->satisfied_deadline_id->value != "ca4m4.cinder.deadline.b01-petition-cure") {
            fail("B01 deficiency/deadline/B03 cure identity drift");
        }
    }

    const auto replay_a =
        engine::replayWorkflow(runtime_case.workflow, runtime_case.definition, initial, journal);
    const auto replay_b =
        engine::replayWorkflow(runtime_case.workflow, runtime_case.definition, initial, journal);
    if (!replay_a || !replay_b || *replay_a != *replay_b || *replay_a != rolling ||
        replay_a->current_stage_id.value != meta.terminal || replay_a->mandate_sha256) {
        fail(meta.label + " deterministic full replay/terminal/no-mandate drift");
    }
    totals.commands += journal.size();
    totals.events += event_count;
    totals.full_replays += 2U;
    totals.tampers += auditTampers(meta, runtime_case, initial, journal);
    ++totals.terminals[meta.terminal];
    return AuditedTrace{initial, *replay_a, std::move(journal)};
}

[[nodiscard]] model::WorkflowCommand rebaseCommand(model::WorkflowCommand command,
                                                   const std::string& session, std::string suffix) {
    auto& header = commandHeader(command);
    header.session_id = session;
    header.command_id = model::WorkflowCommandId{session + ".hostile." + std::move(suffix)};
    return command;
}

void expectUnmet(const packs::RuntimeCase& runtime_case, const model::WorkflowState& state,
                 const model::WorkflowCommand& command, std::string_view label) {
    const auto result =
        engine::decideWorkflow(runtime_case.workflow, runtime_case.definition, state, command);
    if (result || result.error().code != engine::WorkflowErrorCode::UnmetPrecondition)
        fail(std::string(label) + " was not rejected by its exact branch precondition");
}

void auditBranchIsolationAndNoMandate(const packs::RuntimeCase& runtime_case,
                                      const AuditedTrace& actual, const AuditedTrace& deficiency,
                                      const AuditedTrace& summary) {
    const auto actual_after_petition = engine::replayWorkflow(
        runtime_case.workflow, runtime_case.definition, actual.initial,
        std::span<const model::WorkflowJournalEntry>(actual.journal.data(), 1U));
    const auto summary_after_petition = engine::replayWorkflow(
        runtime_case.workflow, runtime_case.definition, summary.initial,
        std::span<const model::WorkflowJournalEntry>(summary.journal.data(), 1U));
    if (!actual_after_petition || !summary_after_petition)
        fail("cannot construct exact submitted-stage branch prefixes");
    expectUnmet(
        runtime_case, *actual_after_petition,
        rebaseCommand(summary.journal.at(1).command, actual.initial.session_id, "summary-selector"),
        "A01-to-summary mutex");
    expectUnmet(
        runtime_case, *summary_after_petition,
        rebaseCommand(actual.journal.at(1).command, summary.initial.session_id, "actual-selector"),
        "B04-to-actual mutex");
    expectUnmet(runtime_case, deficiency.final,
                rebaseCommand(actual.journal.at(1).command, deficiency.initial.session_id,
                              "actual-selector"),
                "B03-to-actual mutex");
    expectUnmet(runtime_case, deficiency.final,
                rebaseCommand(summary.journal.at(1).command, deficiency.initial.session_id,
                              "summary-selector"),
                "B03-to-summary mutex");

    auto mandate_header = commandHeader(actual.journal.back().command);
    mandate_header.command_id =
        model::WorkflowCommandId{actual.initial.session_id + ".hostile.mandate"};
    const model::WorkflowCommand mandate = model::IssueWorkflowMandate{
        mandate_header, model::WorkflowOperationId{"ca4m4.cinder.operation.issue-mandate"},
        std::string(64U, '0')};
    const auto decision = engine::decideWorkflow(runtime_case.workflow, runtime_case.definition,
                                                 actual.final, mandate);
    if (decision || decision.error().code != engine::WorkflowErrorCode::InvalidCommand)
        fail("original-proceeding terminal accepted a mandate command");
}

void auditHostileWorkflowDecisions(const packs::RuntimeCase& runtime_case,
                                   const AuditedTrace& actual, const AuditedTrace& summary) {
    auto malformed = actual.journal.front().command;
    auto* filing = std::get_if<model::SubmitWorkflowFiling>(&malformed);
    if (!filing)
        fail("canonical A01 cannot supply malformed-filing probe");
    filing->filing_id = model::WorkflowFilingId{"ca4m4.cinder.filing.hostile-opened"};
    filing->document_sha256 = std::string(64U, 'a');
    filing->header.actor_id = model::ActorId{"ca4m4.cinder.actor.rhea-calder"};
    filing->served_actors = {model::ActorId{"ca4m4.cinder.actor.cinder-lake"}};
    filing->header.command_id =
        model::WorkflowCommandId{actual.initial.session_id + ".hostile.malformed-a01"};
    const auto rejected = engine::decideWorkflow(runtime_case.workflow, runtime_case.definition,
                                                 actual.initial, malformed);
    if (!rejected || rejected->size() != 1U ||
        !std::holds_alternative<model::WorkflowFilingRejected>(rejected->front())) {
        fail("non-static malformed A01 did not emit one rejection");
    }
    const auto& rejection = std::get<model::WorkflowFilingRejected>(rejected->front());
    if (rejection.header.operation_id.value !=
            "ca4m4.cinder.operation.reject-opened-writ-petition" ||
        rejection.reason != model::WorkflowFilingRejectionReason::UnauthorizedActor) {
        fail("malformed A01 rejection operation/reason drift");
    }
    const std::array rejected_journal{
        model::WorkflowJournalEntry{malformed, *rejected},
    };
    const auto rejected_state = engine::replayWorkflow(
        runtime_case.workflow, runtime_case.definition, actual.initial, rejected_journal);
    if (!rejected_state || rejected_state->current_stage_id != actual.initial.current_stage_id ||
        !rejected_state->accepted_filings.empty() || !rejected_state->deficiencies.empty() ||
        !rejected_state->deadlines.empty()) {
        fail("malformed A01 rejection changed substantive state or became B01 deficiency");
    }

    const auto actual_judgment = std::ranges::find_if(actual.journal, [](const auto& entry) {
        return std::holds_alternative<model::IssueWorkflowJudgment>(entry.command);
    });
    const auto summary_judgment = std::ranges::find_if(summary.journal, [](const auto& entry) {
        return std::holds_alternative<model::IssueWorkflowJudgment>(entry.command);
    });
    if (actual_judgment == actual.journal.end() || summary_judgment == summary.journal.end())
        fail("canonical traces lack exact judgment commands");
    const auto actual_index =
        static_cast<std::size_t>(std::distance(actual.journal.begin(), actual_judgment));
    const auto summary_index =
        static_cast<std::size_t>(std::distance(summary.journal.begin(), summary_judgment));
    const auto actual_before = engine::replayWorkflow(
        runtime_case.workflow, runtime_case.definition, actual.initial,
        std::span<const model::WorkflowJournalEntry>(actual.journal.data(), actual_index));
    const auto summary_before = engine::replayWorkflow(
        runtime_case.workflow, runtime_case.definition, summary.initial,
        std::span<const model::WorkflowJournalEntry>(summary.journal.data(), summary_index));
    if (!actual_before || !summary_before || summary_before->argument_date)
        fail("cannot construct exact actual/summary pre-judgment states");

    auto actual_swap = actual_judgment->command;
    auto& actual_swap_command = std::get<model::IssueWorkflowJudgment>(actual_swap);
    actual_swap_command.header.command_id =
        model::WorkflowCommandId{actual.initial.session_id + ".hostile.plan-swap"};
    actual_swap_command.disposition =
        model::DispositionPlanId{"ca4m4.cinder.disposition.counterfactual-summary-denial"};
    const auto actual_swap_result = engine::decideWorkflow(
        runtime_case.workflow, runtime_case.definition, *actual_before, actual_swap);
    if (actual_swap_result ||
        actual_swap_result.error().code != engine::WorkflowErrorCode::InvalidCommand) {
        fail("actual judgment accepted counterfactual disposition plan");
    }

    auto summary_swap = summary_judgment->command;
    auto& summary_swap_command = std::get<model::IssueWorkflowJudgment>(summary_swap);
    summary_swap_command.header.command_id =
        model::WorkflowCommandId{summary.initial.session_id + ".hostile.plan-swap"};
    summary_swap_command.disposition =
        model::DispositionPlanId{"ca4m4.cinder.disposition.actual-partial-grant-vacatur-remand"};
    const auto summary_swap_result = engine::decideWorkflow(
        runtime_case.workflow, runtime_case.definition, *summary_before, summary_swap);
    if (summary_swap_result ||
        summary_swap_result.error().code != engine::WorkflowErrorCode::InvalidCommand) {
        fail("summary judgment accepted actual disposition plan");
    }

    auto argued_summary = *summary_before;
    argued_summary.argument_date = actual.final.argument_date;
    const auto argued_result = engine::decideWorkflow(
        runtime_case.workflow, runtime_case.definition, argued_summary, summary_judgment->command);
    if (argued_result || argued_result.error().code != engine::WorkflowErrorCode::UnmetPrecondition)
        fail("summary-denial judgment accepted a scheduled-argument state");
}

[[nodiscard]] bool expectsPdfRejection(ui::RecordWorkspace& workspace,
                                       const QString& stable_anchor) {
    const auto opened = workspace.navigateToAnchor(stable_anchor);
    return !opened && opened.error().code == ui::RecordWorkspaceErrorCode::PdfLoadFailed &&
           workspace.currentDocumentId().isEmpty();
}

struct DisclosureProbe final {
    std::string disclosure_id;
    QString sealed_entry_id;
    QString public_entry_id;
    QString stable_anchor_id;
    std::size_t page_count{};
    std::string asset_path;
    std::string asset_sha256;
};

void auditInstalledRecord(const QDir& temporary_root, packs::PackCatalog& catalog,
                          const packs::ResolvedPack& resolved, const packs::LoadedPack& loaded,
                          const packs::RuntimePack& runtime) {
    const auto& runtime_case = runtime.cases.front();
    const auto& record = runtime_case.record;
    const auto mapping_count = std::ranges::fold_left(
        record.sealed_disclosures, std::size_t{0}, [](std::size_t total, const auto& disclosure) {
            return total + disclosure.anchor_mappings.size();
        });
    if (record.id.value != "ca4m4.cinder.record" || record.dockets.size() != 4U ||
        record.docket_entries.size() != 48U || record.page_anchors.size() != 402U ||
        !record.disclosure_policy || record.sealed_disclosures.size() != 4U ||
        mapping_count != 52U) {
        fail("installed record 4/48/402/4/52 envelope drift");
    }

    ui::RecordWorkspace workspace;
    app::InstalledRecordController installed_controller(catalog, workspace);
    const auto installed = installed_controller.load(resolved, runtime, runtime_case.definition.id);
    if (!installed)
        fail("installed record load failed: " + installed.error().message.toStdString());
    const auto& full = ui::RecordWorkspaceTestAccess::fullDefinition(workspace);
    const auto public_pages = std::ranges::fold_left(
        installed->definition.documents, std::size_t{0}, [](std::size_t total, const auto& doc) {
            return total + static_cast<std::size_t>(doc.declared_page_count);
        });
    const auto full_pages = std::ranges::fold_left(
        full.documents, std::size_t{0}, [](std::size_t total, const auto& doc) {
            return total + static_cast<std::size_t>(doc.declared_page_count);
        });
    if (installed->definition.documents.size() != 44U ||
        installed->definition.docket.size() != 44U || installed->assets.size() != 44U ||
        workspace.visibleDocketCount() != 44 || public_pages != 350U ||
        full.documents.size() != 48U || full.docket.size() != 48U || full_pages != 402U ||
        full.sealed_disclosures.size() != 4U) {
        fail("installed public/full record projection is not exact 44/350 and 48/402/4");
    }

    std::vector<DisclosureProbe> probes;
    for (const auto& disclosure : record.sealed_disclosures) {
        if (!disclosure.public_entry_id || disclosure.anchor_mappings.empty())
            fail("sealed disclosure lacks a public stable-anchor twin");
        const auto sealed =
            std::ranges::find(record.docket_entries, disclosure.sealed_entry_id,
                              [](const packs::RuntimeDocketEntry& entry) { return entry.id; });
        if (sealed == record.docket_entries.end() || !sealed->sealed)
            fail("sealed disclosure entry is absent or public");
        DisclosureProbe probe{disclosure.disclosure_id.value,
                              asQString(disclosure.sealed_entry_id.value),
                              asQString(disclosure.public_entry_id->value),
                              asQString(disclosure.anchor_mappings.front().stable_anchor_id.value),
                              sealed->page_count,
                              sealed->asset_path,
                              sealed->asset_sha256};
        if (!workspace.navigateToAnchor(probe.stable_anchor_id) ||
            workspace.currentDocumentId() != probe.public_entry_id ||
            workspace.openDocketEntry(probe.sealed_entry_id)) {
            fail("unauthorized stable-anchor/public-twin boundary drift");
        }
        probes.push_back(std::move(probe));
    }

    const auto session_path =
        temporary_root.filePath(QStringLiteral("cinder-record-access.sqlite"));
    auto store = storage::SessionStore::open(session_path);
    if (!store)
        fail("record access session store cannot open");
    auto access = app::RecordAccessSessionController::create(
        QStringLiteral("integration.cinder.record-access"), runtime_case.definition.id,
        std::move(*store), QStringLiteral("engine.record-access.v1"),
        QStringLiteral("2026-08-19T04:00:00Z"), resolved);
    if (!access)
        fail("record access session cannot bind exact resolved closure");
    auto statuses = (*access)->disclosures();
    if (statuses.size() != 4U ||
        std::ranges::any_of(statuses,
                            [](const auto& status) {
                                return status.authorized || !status.blocking_deficiencies.empty();
                            }) ||
        !(*access)->applyCurrentProjection(workspace)) {
        fail("initial four-disclosure policy is not eligible and unauthorized");
    }

    for (std::size_t index = 0; index < probes.size(); ++index) {
        const auto& probe = probes.at(index);
        const auto event_id = "integration.cinder.grant." + std::to_string(index + 1U);
        const auto time =
            QStringLiteral("2026-08-19T04:00:0%1Z").arg(static_cast<qulonglong>(index + 1U));
        if (!(*access)->grant(probe.disclosure_id, event_id, time) ||
            !(*access)->applyCurrentProjection(workspace) ||
            workspace.visibleDocketCount() != static_cast<qsizetype>(45U + index) ||
            !workspace.navigateToAnchor(probe.stable_anchor_id) ||
            workspace.currentDocumentId() != probe.sealed_entry_id ||
            workspace.loadedPageCount() != static_cast<int>(probe.page_count)) {
            fail("exact disclosure grant did not expose one verified sealed twin");
        }
        statuses = (*access)->disclosures();
        if (std::ranges::count(statuses, true, &model::RecordAccessDisclosureStatus::authorized) !=
            static_cast<std::ptrdiff_t>(index + 1U)) {
            fail("grant authorization cardinality drift");
        }
    }
    const auto prefix_zero = (*access)->auditProjectionAt(0);
    const auto prefix_four = (*access)->auditProjectionAt(4);
    if (!prefix_zero || !prefix_zero->authorizedDisclosureIds().empty() || !prefix_four ||
        prefix_four->authorizedDisclosureIds().size() != 4U ||
        workspace.visibleDocketCount() != 48) {
        fail("all-four disclosure grant/prefix replay drift");
    }

    for (std::size_t index = 0; index < probes.size(); ++index) {
        const auto& probe = probes.at(index);
        const auto event_id = "integration.cinder.revoke." + std::to_string(index + 1U);
        const auto time =
            QStringLiteral("2026-08-19T04:00:1%1Z").arg(static_cast<qulonglong>(index + 1U));
        if (!workspace.navigateToAnchor(probe.stable_anchor_id) ||
            !(*access)->revoke(probe.disclosure_id, event_id, time) ||
            !(*access)->applyCurrentProjection(workspace) ||
            workspace.visibleDocketCount() != static_cast<qsizetype>(47U - index) ||
            !workspace.currentDocumentId().isEmpty()) {
            fail("exact disclosure revoke did not close and decrement projection");
        }
    }
    const auto prefix_eight = (*access)->auditProjectionAt(8);
    if (!prefix_eight || !prefix_eight->authorizedDisclosureIds().empty() ||
        (*access)->snapshot().sequence != 8 ||
        !workspace.navigateToAnchor(probes.front().stable_anchor_id) ||
        workspace.currentDocumentId() != probes.front().public_entry_id ||
        workspace.openDocketEntry(probes.front().sealed_entry_id)) {
        fail("all-four revocation/public projection replay drift");
    }

    (*access).reset();
    auto reopened_store = storage::SessionStore::open(session_path);
    if (!reopened_store)
        fail("record access journal cannot reopen");
    access = app::RecordAccessSessionController::reopen(
        QStringLiteral("integration.cinder.record-access"), runtime_case.definition.id,
        std::move(*reopened_store), QStringLiteral("engine.record-access.v1"), resolved);
    if (!access || (*access)->snapshot().sequence != 8 ||
        std::ranges::any_of((*access)->disclosures(),
                            &model::RecordAccessDisclosureStatus::authorized)) {
        fail("record access exact-closure replay drift");
    }

    const auto& probe = probes.front();
    if (!(*access)->grant(probe.disclosure_id, "integration.cinder.grant.cas",
                          QStringLiteral("2026-08-19T04:00:20Z")) ||
        !(*access)->applyCurrentProjection(workspace) ||
        !workspace.navigateToAnchor(probe.stable_anchor_id)) {
        fail("CAS probe disclosure cannot reopen");
    }
    const auto descriptor = std::ranges::find(
        loaded.blobs, probe.asset_path,
        [](const model::BlobDescriptor& blob) -> std::string_view { return blob.path; });
    if (descriptor == loaded.blobs.end())
        fail("sealed CAS descriptor is absent");
    const auto object_path =
        QDir(catalog.blobObjectsDirectory()).filePath(asQString(probe.asset_sha256));
    const auto original = readAll(object_path);
    const auto original_permissions = QFileInfo(object_path).permissions();
    if (original.isEmpty() ||
        static_cast<std::uint64_t>(original.size()) != descriptor->byte_size ||
        sha256(original).toStdString() != descriptor->sha256) {
        fail("sealed CAS object cannot be pinned before mutation");
    }
    const auto listed = catalog.list();
    const auto installed_root =
        listed ? std::ranges::find(*listed, loaded.revision,
                                   [](const packs::InstalledPack& pack) { return pack.revision; })
               : std::vector<packs::InstalledPack>::const_iterator{};
    if (!listed || listed->size() != 4U || installed_root == listed->end())
        fail("root archive is absent from four-revision catalog");
    const auto archive_path =
        QDir(catalog.archivesDirectory())
            .filePath(installed_root->archive_sha256 + QStringLiteral(".awpack"));
    const auto held_archive = archive_path + QStringLiteral(".held-for-cinder-test");
    if (!QFile::rename(archive_path, held_archive))
        fail("cannot isolate source archive for CAS mutation checks");
    const auto restore_archive = [&]() {
        return QFileInfo::exists(archive_path) || QFile::rename(held_archive, archive_path);
    };
    const auto restore_object = [&]() {
        if (QFileInfo(object_path).isSymLink())
            QFile::remove(object_path);
        const auto restored = writeAll(object_path, original);
        QFile::setPermissions(object_path, original_permissions);
        return restored;
    };
    const auto held_object = object_path + QStringLiteral(".held-for-cinder-test");
    if (!QFile::rename(object_path, held_object) ||
        !expectsPdfRejection(workspace, probe.stable_anchor_id) ||
        !QFile::rename(held_object, object_path)) {
        restore_object();
        restore_archive();
        fail("missing CAS object was not rejected");
    }
    auto mutation = original;
    mutation[mutation.size() / 2] = mutation.at(mutation.size() / 2) == 'x' ? 'y' : 'x';
    if (!writeAll(object_path, mutation) ||
        !expectsPdfRejection(workspace, probe.stable_anchor_id) || !restore_object() ||
        !restore_archive()) {
        restore_object();
        restore_archive();
        fail("same-size CAS digest mutation/recovery drift");
    }
    if (!workspace.navigateToAnchor(probe.stable_anchor_id) ||
        workspace.currentDocumentId() != probe.sealed_entry_id ||
        workspace.loadedPageCount() != static_cast<int>(probe.page_count)) {
        fail("restored CAS object did not reopen exact sealed twin");
    }
    if (!(*access)->revoke(probe.disclosure_id, "integration.cinder.revoke.cas",
                           QStringLiteral("2026-08-19T04:00:21Z")) ||
        !(*access)->applyCurrentProjection(workspace) || workspace.visibleDocketCount() != 44 ||
        !workspace.currentDocumentId().isEmpty() || (*access)->snapshot().sequence != 10) {
        fail("final CAS revoke did not restore public44 projection");
    }
}

[[nodiscard]] std::set<std::string> rejectOperationIds(const model::WorkflowDefinition& workflow) {
    std::set<std::string> result;
    for (const auto& operation : workflow.operations) {
        if (operation.opcode == model::WorkflowOpcode::RejectFiling)
            result.insert(operation.id.value);
    }
    return result;
}

} // namespace

int main(int argc, char** argv) try {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    const QDir content_root(QStringLiteral(APPELLATE_M4_CINDER_ROOT));
    const QDir foundations(QStringLiteral(APPELLATE_M4_FOUNDATIONS));
    auditStaticContent(content_root);

    const model::PackRevision root{model::PackId{"us.ca4.m4.cinderlake-writ"}, "1.2.0",
                                   std::string(ReleasePins::root_revision)};
    const std::array dependencies{
        model::PackRevision{model::PackId{"foundation.us-federal"}, "2025.12.01",
                            "866c90996c15e2076b9508a297ffce1a4e766b1432a9e11d08e8138c57e363c9"},
        model::PackRevision{model::PackId{"foundation.us-ca4"}, "2026.03.23",
                            "449d75c77e5c47883f750377450f2d1ec1fc0e42e20b1f247446b208661d3262"},
        model::PackRevision{model::PackId{"foundation.us-ca4-fictional-bench"}, "1.0.0",
                            "cee0bf93309cc9ad800f215a47d734b20a9fdf5dc889f2f440e4382b942d332d"},
    };
    const QDir pack_root(content_root.filePath(QStringLiteral("pack-candidate")));
    const auto loaded = packs::PackReader::readDirectory(
        pack_root.path(), packs::PackValidationScope::ResolvedClosure);
    if (!loaded || loaded->revision != root ||
        loaded->graph_state != packs::PackGraphState::DeferredReferences ||
        loaded->resources.size() != 9U || loaded->blobs.size() != 48U ||
        loaded->required_capabilities.size() != 18U || loaded->dependencies.size() != 3U) {
        fail("final deferred root cannot load with exact 9/48/18/3 envelope");
    }

    QTemporaryDir temporary;
    if (!temporary.isValid())
        fail("cannot create Cinder integration temporary directory");
    const QDir temporary_root(temporary.path());
    const auto archive_a = temporary_root.filePath(QStringLiteral("cinder-a.awpack"));
    const auto archive_b = temporary_root.filePath(QStringLiteral("cinder-b.awpack"));
    const auto exported_a = packs::PackArchive::exportDirectory(
        pack_root.path(), archive_a, {}, packs::PackValidationScope::ResolvedClosure);
    const auto exported_b = packs::PackArchive::exportDirectory(
        pack_root.path(), archive_b, {}, packs::PackValidationScope::ResolvedClosure);
    const auto archive_bytes = readAll(archive_a);
    if (!exported_a || !exported_b || *exported_a != root || *exported_b != root ||
        archive_bytes != readAll(archive_b) ||
        static_cast<std::uint64_t>(archive_bytes.size()) != ReleasePins::archive_byte_size ||
        sha256(archive_bytes).toStdString() != ReleasePins::archive_sha256) {
        fail("double export root/archive pin or determinism drift");
    }

    const auto catalog =
        packs::PackCatalog::open(temporary_root.filePath(QStringLiteral("catalog")));
    if (!catalog)
        fail("cannot open Cinder integration catalog");
    const auto federal = (*catalog)->installArchive(
        foundations.filePath(QStringLiteral("us-federal/foundation-us-federal-2025.12.01.awpack")),
        QStringLiteral("2026-08-19T00:00:00Z"));
    const auto ca4 = (*catalog)->installArchive(
        foundations.filePath(QStringLiteral("us-ca4/foundation-us-ca4-2026.03.23.awpack")),
        QStringLiteral("2026-08-19T00:00:01Z"));
    const auto bench = (*catalog)->installArchive(
        foundations.filePath(QStringLiteral(
            "us-ca4-fictional-bench/foundation-us-ca4-fictional-bench-1.0.0.awpack")),
        QStringLiteral("2026-08-19T00:00:02Z"));
    const auto installed_root =
        (*catalog)->installArchive(archive_a, QStringLiteral("2026-08-19T00:00:03Z"));
    if (!federal || !ca4 || !bench || !installed_root || federal->revision != dependencies.at(0) ||
        ca4->revision != dependencies.at(1) || bench->revision != dependencies.at(2) ||
        installed_root->revision != root ||
        installed_root->archive_sha256.toStdString() != ReleasePins::archive_sha256) {
        fail("exact three-dependency plus root installation drift");
    }
    const auto listed = (*catalog)->list();
    const auto resolved = (*catalog)->loadResolved(root);
    if (!listed || listed->size() != 4U || !resolved ||
        resolved->revisionsByPackId().size() != 4U) {
        fail("exact four-revision catalog/resolution drift");
    }
    for (const auto& blob : loaded->blobs) {
        const auto materialized = (*catalog)->materializeBlob(*resolved, root, blob.path);
        if (!materialized || materialized->descriptor != blob ||
            sha256(readAll(materialized->local_path)).toStdString() != blob.sha256) {
            fail("installed blob materialization drift: " + blob.path);
        }
    }

    const auto runtime = packs::loadRuntimePack(*resolved);
    if (!runtime || runtime->revision != root || runtime->cases.size() != 1U)
        fail("resolved Cinder runtime cannot load");
    const auto& runtime_case = runtime->cases.front();
    if (runtime_case.definition.id.value != case_id ||
        runtime_case.workflow.id.value != workflow_id ||
        runtime_case.workflow.stages.size() != 15U ||
        runtime_case.workflow.operations.size() != 45U ||
        runtime_case.workflow.filing_routes.size() != 8U ||
        runtime_case.record.docket_entries.size() != 48U ||
        runtime_case.record.page_anchors.size() != 402U ||
        runtime_case.record.sealed_disclosures.size() != 4U ||
        runtime_case.argument_configurations.size() != 2U ||
        runtime_case.definition.disposition_plans.size() != 2U) {
        fail("resolved Cinder runtime shape drift");
    }
    auditInstalledRecord(temporary_root, **catalog, *resolved, *loaded, *runtime);

    const QDir trace_dir(content_root.filePath(QStringLiteral("traces")));
    QStringList expected_trace_files;
    for (const auto& meta : trace_metas)
        expected_trace_files.push_back(meta.file);
    expected_trace_files.sort();
    if (trace_dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name) !=
        expected_trace_files) {
        fail("three-trace file inventory drift");
    }
    TraceTotals totals;
    std::vector<AuditedTrace> traces;
    traces.reserve(trace_metas.size());
    for (const auto& meta : trace_metas)
        traces.push_back(auditTrace(trace_dir, meta, runtime_case, totals));

    std::set<std::string> defined_operations;
    for (const auto& operation : runtime_case.workflow.operations)
        defined_operations.insert(operation.id.value);
    const auto reject_operations = rejectOperationIds(runtime_case.workflow);
    auto canonical_operations = defined_operations;
    for (const auto& operation : reject_operations)
        canonical_operations.erase(operation);
    const std::map<std::string, std::string> expected_deadlines{
        {"ca4m4.cinder.deadline.b01-petition-cure", "2026-04-03"},
        {"ca4m4.cinder.deadline.actual-answer", "2026-04-09"},
        {"ca4m4.cinder.deadline.actual-reply", "2026-04-13"},
        {"ca4m4.cinder.deadline.actual-rule40", "2026-05-19"},
        {"ca4m4.cinder.deadline.counterfactual-summary-rule40", "2026-04-15"},
    };
    const std::map<std::string, std::size_t> expected_terminals{
        {"ca4m4.cinder.stage.actual-terminal", 1U},
        {"ca4m4.cinder.stage.submitted", 1U},
        {"ca4m4.cinder.stage.counterfactual-summary-terminal", 1U},
    };
    if (defined_operations.size() != 45U || reject_operations.size() != 8U ||
        canonical_operations.size() != 37U || totals.operations != canonical_operations ||
        totals.commands != 36U || totals.events != 42U || totals.prefix_replays != 36U ||
        totals.full_replays != 6U || totals.tampers != 22U ||
        totals.deadlines != expected_deadlines || totals.terminals != expected_terminals) {
        fail("canonical 36/42, replay, tamper, union37, deadline, or terminal closure drift");
    }
    auditBranchIsolationAndNoMandate(runtime_case, traces.at(0), traces.at(1), traces.at(2));
    auditHostileWorkflowDecisions(runtime_case, traces.at(0), traces.at(2));

    std::cout << "CINDER CORE CLEAR pack=9/48/18/3 record=48/402/4/52 public=44 "
                 "workflow=15/45/8/10/10 traces=26/29+4/6+6/7 union=37 no-mandate\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Cinder integration: " << error.what() << '\n';
    return 1;
}
