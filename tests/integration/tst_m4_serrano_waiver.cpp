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

#ifndef APPELLATE_M4_SERRANO_ROOT
#error "APPELLATE_M4_SERRANO_ROOT must name content/m4/serrano-waiver"
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

namespace ReleasePins {
constexpr std::string_view manifest_sha256 =
    "a1910f6d853371b2061ffa10e7271389a914e2e39172c544e3dff2221665f577";
constexpr std::string_view realism_review_sha256 =
    "d0521658fb7eb2ed8eb35c30293aaf5b86ca029257ac6cadef7e14b56557a57d";
constexpr std::string_view record_sha256 =
    "ff91471c1fbde78ad6ef138a7cfc14358ea0ff985ccb58bb19f9c146f1785fb5";
constexpr std::string_view workflow_sha256 =
    "b31e490d3fa460c0b1bff551fe937d8acae41b53f1a6cdf41549d3ac411585c2";
constexpr std::string_view evidence_closure =
    "052e54ec473a8a427f1cda0c004f02878955e46f4e2282c64385ed4e404eb955";
constexpr std::string_view trace_closure =
    "3865f2eeecd51083a3c9a33b5dcc99194e9910783abbcfa541114a836a65fd5a";
constexpr std::string_view root_revision =
    "9b4941e97292faa0fceda1f1c719f6e38ce8478c82350c7fbbb74a010c27d344";
constexpr std::string_view archive_sha256 =
    "d76686cec2053f78334c73f1c3aac415b637e733f0494b527001368597a1c243";
constexpr std::uint64_t archive_byte_size = 3'453'568;
} // namespace ReleasePins

constexpr std::string_view case_id = "ca4m4.case.serrano-waiver";
constexpr std::string_view workflow_id = "ca4m4.serrano.workflow.waiver-allocution-appeal";
constexpr std::string_view entry_prefix = "ca4m4.serrano.record.entry.";

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

[[nodiscard]] QByteArray readAll(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        fail("cannot read " + path.toStdString());
    return file.readAll();
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

[[nodiscard]] const model::WorkflowEventHeader& eventHeader(const model::WorkflowEvent& event) {
    return std::visit([](const auto& concrete) -> const auto& { return concrete.header; }, event);
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
    addFrame(hash, QStringLiteral("ca4m4.case.serrano-waiver"));
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
    std::size_t unique_operations;
    std::string file_sha256;
    std::string journal_sha256;
    std::string digest;
};

const std::array<TraceMeta, 2> trace_metas{
    TraceMeta{QStringLiteral("actual-ordinary-through-mandate.json"), "actual",
              "ca4m4.serrano.trace.actual-ordinary", "ca4m4.serrano.evidence.trace.actual-ordinary",
              51, 49, "217675ff4ce270f0651b7929fc4c808de5884cae2b51639a516d752d0b046847",
              "a5d95607b046f21e16676c1aa884fb7e91c8d6a756f4d47f680bd8ecaee48407",
              "5d76674c2ab97bccbc5f35258352fa52860f31dacc6d0c24c6838ee6260dac31"},
    TraceMeta{QStringLiteral("counterfactual-ordinary-through-mandate.json"), "counterfactual",
              "ca4m4.serrano.trace.counterfactual-ordinary",
              "ca4m4.serrano.evidence.trace.counterfactual-ordinary", 35, 35,
              "e0ffe0653e015a40c023ef1c4ba7393dd4eb68b420c6318c6d9f4352693c7e86",
              "de5673eff22e5b532ce9d15d5b0a0c89d801212c98126a11567fb7e39c5f4113",
              "eeef08fe4b50fe0459591613354c346d8a86b506d986082f29253c6567f898a2"},
};

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
        const auto event_values = object.value(QStringLiteral("events_base64")).toArray();
        events.reserve(static_cast<std::size_t>(event_values.size()));
        for (qsizetype event_index = 0; event_index < event_values.size(); ++event_index) {
            const auto encoded_event = event_values.at(event_index).toString().toLatin1();
            const auto event_bytes = QByteArray::fromBase64(encoded_event);
            const auto event = storage::decodeWorkflowEvent(QByteArrayView(event_bytes));
            if (event_bytes.isEmpty() || event_bytes.toBase64() != encoded_event || !event ||
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

[[nodiscard]] std::set<std::string> expectedTraceEntries(std::string_view label) {
    const auto codes = label == "actual"
                           ? std::string_view{"l24 l27 l28 a01 a02 a03 a04 a05 a06 a08 a09 a10 "
                                              "a11 a13 a15 a17 a18 a19 a20"}
                           : std::string_view{"l24 b01 b02 b03 b04 b05 b06 b07 b08 b09 b10"};
    std::set<std::string> result;
    std::size_t start{};
    while (start < codes.size()) {
        const auto end = codes.find(' ', start);
        result.insert(std::string(entry_prefix) +
                      std::string(codes.substr(start, end == codes.npos ? codes.size() - start
                                                                        : end - start)));
        if (end == codes.npos)
            break;
        start = end + 1U;
    }
    return result;
}

struct TraceTotals final {
    std::set<std::string> operations;
    std::set<std::string> accepted_filings;
    std::set<std::string> rejected_filings;
    std::map<std::string, std::string> deadlines;
    std::size_t commands{};
    std::size_t events{};
    std::size_t prefix_replays{};
    std::size_t full_replays{};
};

void auditTrace(const QDir& trace_dir, const TraceMeta& meta,
                const packs::RuntimeCase& runtime_case, TraceTotals& totals) {
    const auto bytes = readAll(trace_dir.filePath(meta.file));
    const auto trace = parseObject(bytes, meta.file);
    const auto journal_values = trace.value(QStringLiteral("journal")).toArray();
    const auto computed_journal = journalDigest(journal_values);
    const auto declared_operations = trace.value(QStringLiteral("operation_ids")).toArray();
    if (sha256(bytes).toStdString() != meta.file_sha256 ||
        trace.value(QStringLiteral("trace_id")).toString().toStdString() != meta.trace_id ||
        trace.value(QStringLiteral("evidence_id")).toString().toStdString() != meta.evidence_id ||
        trace.value(QStringLiteral("workflow_id")).toString().toStdString() != workflow_id ||
        trace.value(QStringLiteral("engine_revision")).toString() !=
            QStringLiteral("appellate.realism-evidence.codec-replay-multi.v1") ||
        trace.value(QStringLiteral("command_count")).toInteger() !=
            static_cast<qint64>(meta.commands) ||
        trace.value(QStringLiteral("event_count")).toInteger() !=
            static_cast<qint64>(meta.commands) ||
        !computed_journal || computed_journal->toStdString() != meta.journal_sha256 ||
        trace.value(QStringLiteral("journal_sha256")).toString().toStdString() !=
            meta.journal_sha256 ||
        traceDigest(trace).toStdString() != meta.digest ||
        trace.value(QStringLiteral("digest")).toString().toStdString() != meta.digest ||
        trace.value(QStringLiteral("terminal_stage_id")).toString() !=
            QStringLiteral("ca4m4.serrano.stage.terminated") ||
        declared_operations.size() != static_cast<qsizetype>(meta.commands)) {
        fail(meta.label + " trace envelope/digest drift");
    }

    const auto journal = decodeJournal(journal_values, meta.label);
    if (journal.size() != meta.commands || journal.empty())
        fail(meta.label + " trace journal count drift");
    const auto initial = initialState(runtime_case.workflow, journal.front().command);
    auto rolling = initial;
    std::set<std::string> trace_operations;
    std::set<std::string> documents;
    std::vector<std::pair<std::size_t, std::string>> rejected;
    std::vector<std::pair<std::size_t, std::string>> accepted;
    std::uint64_t expected_sequence = 1;
    std::string session;

    for (std::size_t index = 0; index < journal.size(); ++index) {
        const auto& entry = journal.at(index);
        const auto& header = commandHeader(entry.command);
        if (session.empty())
            session = header.session_id;
        if (header.session_id != session ||
            header.command_id.value != session + ".command." + std::to_string(index + 1U)) {
            fail(meta.label + " command/session identity drift");
        }
        const auto command_date = dateText(header.occurred_at.court_date);
        const auto qdate = QDate::fromString(QString::fromStdString(command_date), Qt::ISODate);
        const auto instant = std::chrono::duration_cast<std::chrono::seconds>(
                                 header.occurred_at.instant.time_since_epoch())
                                 .count();
        if (!qdate.isValid() ||
            QDateTime(qdate, QTime(0, 0), QTimeZone::UTC).toSecsSinceEpoch() != instant) {
            fail(meta.label + " LegalTime date/instant drift");
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
            if (record_entry == runtime_case.record.docket_entries.end() ||
                record_entry->filed_on != header.occurred_at.court_date) {
                fail(meta.label + " command document/date does not resolve");
            }
            documents.insert(record_entry->id.value);
        }
        if (entry.events.size() != 1U)
            fail(meta.label + " trace command did not emit exactly one event");
        const auto& event = entry.events.front();
        const auto& event_header = eventHeader(event);
        if (event_header.session_id != session || event_header.command_id != header.command_id ||
            event_header.occurred_at != header.occurred_at ||
            event_header.sequence != expected_sequence || event_header.command_event_index != 0U ||
            event_header.command_event_count != 1U) {
            fail(meta.label + " event header drift");
        }
        ++expected_sequence;
        const auto operation = event_header.operation_id.value;
        trace_operations.insert(operation);
        totals.operations.insert(operation);
        if (declared_operations.at(static_cast<qsizetype>(index)).toString().toStdString() !=
            operation) {
            fail(meta.label + " operation order drift");
        }
        if (const auto* filing = std::get_if<model::WorkflowFilingRejected>(&event)) {
            if (filing->reason != model::WorkflowFilingRejectionReason::NonconformingFiling)
                fail(meta.label + " rejection reason drift");
            rejected.emplace_back(index, filing->filing_id.value);
            totals.rejected_filings.insert(filing->filing_id.value);
        }
        if (const auto* filing = std::get_if<model::WorkflowFilingAccepted>(&event)) {
            accepted.emplace_back(index, filing->filing_id.value);
            totals.accepted_filings.insert(filing->filing_id.value);
        }
        if (const auto* deadline = std::get_if<model::WorkflowDeadlineCalculated>(&event)) {
            const auto id = deadline->deadline_id.value;
            const auto due = dateText(deadline->due_date);
            const auto [found, inserted] = totals.deadlines.emplace(id, due);
            if (!inserted && found->second != due)
                fail("deadline changes across traces: " + id);
        }
    }

    for (const auto& [position, filing_id] : rejected) {
        const auto recovery = std::ranges::find_if(accepted, [&](const auto& value) {
            return value.first > position && value.second == filing_id;
        });
        if (recovery == accepted.end())
            fail(meta.label + " rejected filing lacks same-ID recovery: " + filing_id);
    }
    if (documents != expectedTraceEntries(meta.label) ||
        trace_operations.size() != meta.unique_operations) {
        fail(meta.label + " exact document/operation closure drift");
    }
    if (meta.label == "actual" &&
        (std::ranges::any_of(documents, [](const auto& id) { return id.contains(".entry.b"); }) ||
         !documents.contains("ca4m4.serrano.record.entry.l27"))) {
        fail("actual trace crossed into the counterfactual docket");
    }
    if (meta.label == "counterfactual" &&
        (std::ranges::any_of(documents, [](const auto& id) { return id.contains(".entry.a"); }) ||
         documents.contains("ca4m4.serrano.record.entry.l27"))) {
        fail("counterfactual trace crossed into actual appellate history");
    }
    const auto first =
        engine::replayWorkflow(runtime_case.workflow, runtime_case.definition, initial, journal);
    const auto second =
        engine::replayWorkflow(runtime_case.workflow, runtime_case.definition, initial, journal);
    if (!first || !second || *first != *second || *first != rolling ||
        first->current_stage_id.value != "ca4m4.serrano.stage.terminated") {
        fail(meta.label + " full replay/terminal drift");
    }
    totals.commands += journal.size();
    totals.events += journal.size();
    totals.full_replays += 2U;
}

[[nodiscard]] bool hasPrecondition(const QJsonObject& operation, QStringView kind,
                                   QStringView field, QStringView value,
                                   std::optional<bool> present = std::nullopt) {
    return std::ranges::any_of(
        operation.value(QStringLiteral("preconditions")).toArray(), [&](const auto& item) {
            const auto condition = item.toObject();
            return condition.value(QStringLiteral("kind")).toString() == kind &&
                   condition.value(field).toString() == value &&
                   (!present || condition.value(QStringLiteral("present")).toBool() == *present);
        });
}

[[nodiscard]] bool hasDeadlineStatus(const QJsonObject& operation, QStringView deadline_id,
                                     QStringView status) {
    return std::ranges::any_of(
        operation.value(QStringLiteral("preconditions")).toArray(), [&](const auto& item) {
            const auto condition = item.toObject();
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
    if (sha256(manifest_bytes).toStdString() != ReleasePins::manifest_sha256 ||
        sha256(review_bytes).toStdString() != ReleasePins::realism_review_sha256 ||
        sha256(record_bytes).toStdString() != ReleasePins::record_sha256 ||
        sha256(workflow_bytes).toStdString() != ReleasePins::workflow_sha256) {
        fail("frozen manifest/review/record/workflow digest drift");
    }

    const auto manifest = parseObject(manifest_bytes, QStringLiteral("manifest.json"));
    const auto review = parseObject(review_bytes, QStringLiteral("realism-review.json"));
    const auto record = parseObject(record_bytes, QStringLiteral("record.json"));
    const auto workflow = parseObject(workflow_bytes, QStringLiteral("workflow.json"));
    const auto case_definition =
        parseObject(readAll(pack_root.filePath(QStringLiteral("resources/case.json"))),
                    QStringLiteral("case.json"));
    const auto actual_argument =
        parseObject(readAll(pack_root.filePath(QStringLiteral("resources/argument-actual.json"))),
                    QStringLiteral("argument-actual.json"));
    const auto counterfactual_argument = parseObject(
        readAll(pack_root.filePath(QStringLiteral("resources/argument-counterfactual.json"))),
        QStringLiteral("argument-counterfactual.json"));

    const auto contents = manifest.value(QStringLiteral("contents")).toArray();
    const auto blobs = manifest.value(QStringLiteral("blobs")).toArray();
    const auto capabilities = manifest.value(QStringLiteral("required_capabilities")).toArray();
    const auto dependencies = manifest.value(QStringLiteral("dependencies")).toArray();
    if (manifest.value(QStringLiteral("schema_version")).toInt() != 2 ||
        manifest.value(QStringLiteral("pack_id")).toString() !=
            QStringLiteral("us.ca4.m4.serrano-waiver") ||
        manifest.value(QStringLiteral("version")).toString() != QStringLiteral("1.2.0") ||
        contents.size() != 9 || blobs.size() != 58 || capabilities.size() != 17 ||
        dependencies.size() != 3) {
        fail("final 9-content/58-blob/17-capability/3-dependency envelope drift");
    }
    const QSet<QString> expected_capabilities{
        QStringLiteral("workbench.pack.argument-date-guards"),
        QStringLiteral("workbench.pack.canonical-authority"),
        QStringLiteral("workbench.pack.declarative-resources"),
        QStringLiteral("workbench.pack.dependent-deadlines"),
        QStringLiteral("workbench.pack.event-date-deadlines"),
        QStringLiteral("workbench.pack.grounded-questions"),
        QStringLiteral("workbench.pack.named-deadlines"),
        QStringLiteral("workbench.pack.operation-disposition-bindings"),
        QStringLiteral("workbench.pack.operation-document-bindings"),
        QStringLiteral("workbench.pack.operation-legal-time-guards"),
        QStringLiteral("workbench.pack.realism-evidence"),
        QStringLiteral("workbench.pack.route-filing-bindings"),
        QStringLiteral("workbench.pack.route-role-subsets"),
        QStringLiteral("workbench.pack.sealed-record-twins"),
        QStringLiteral("workbench.pack.structured-disposition"),
        QStringLiteral("workbench.pack.workflow-instance-preconditions"),
        QStringLiteral("workbench.pack.workflow-preconditions"),
    };
    QSet<QString> actual_capabilities;
    for (const auto& value : capabilities) {
        const auto capability = value.toObject();
        const auto id = capability.value(QStringLiteral("id")).toString();
        if (id.isEmpty() || actual_capabilities.contains(id) ||
            capability.value(QStringLiteral("version")).toInt() !=
                (id == QStringLiteral("workbench.pack.declarative-resources") ? 2 : 1)) {
            fail("capability identity/version drift: " + id.toStdString());
        }
        actual_capabilities.insert(id);
    }
    if (actual_capabilities != expected_capabilities)
        fail("exact Serrano 17-capability set drift");

    const QHash<QString, QString> expected_dependencies{
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
        if (!expected_dependencies.contains(id) || dependency_ids.contains(id) ||
            dependency.value(QStringLiteral("sha256")).toString() != expected_dependencies[id]) {
            fail("dependency revision drift: " + id.toStdString());
        }
        dependency_ids.insert(id);
    }
    if (dependency_ids.size() != 3)
        fail("dependency closure drift");

    const QSet<QString> expected_content_ids{
        QStringLiteral("ca4m4.case.serrano-waiver"),
        QStringLiteral("ca4m4.serrano.argument.actual-record"),
        QStringLiteral("ca4m4.serrano.argument.day15-counterfactual"),
        QStringLiteral("ca4m4.serrano.authorities.case-specific"),
        QStringLiteral("ca4m4.serrano.bench.three-judge"),
        QStringLiteral("ca4m4.serrano.procedure.criminal-sentencing-appeal"),
        QStringLiteral("ca4m4.serrano.record"),
        QStringLiteral("ca4m4.serrano.workflow.waiver-allocution-appeal"),
        QStringLiteral("ca4m4.serrano.review.authoring-2026-08-19"),
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
            fail("content descriptor drift: " + path.toStdString());
        }
        content_ids.insert(id);
    }
    if (content_ids != expected_content_ids)
        fail("nine-content identity closure drift");

    const auto evidence = review.value(QStringLiteral("evidence")).toObject();
    const auto dimension_evidence = evidence.value(QStringLiteral("dimension_evidence")).toObject();
    const auto dimensions = review.value(QStringLiteral("dimensions")).toObject();
    const auto uncertainties = review.value(QStringLiteral("known_uncertainty")).toArray();
    if (review.value(QStringLiteral("resource_id")).toString() !=
            QStringLiteral("ca4m4.serrano.review.authoring-2026-08-19") ||
        review.value(QStringLiteral("review_state")).toString() !=
            QStringLiteral("independent_review_pending") ||
        review.value(QStringLiteral("reviewed_on")).toString() != QStringLiteral("2026-08-19") ||
        evidence.value(QStringLiteral("closure_digest")).toString().toStdString() !=
            ReleasePins::evidence_closure ||
        evidence.value(QStringLiteral("packs")).toArray().size() != 4 ||
        evidence.value(QStringLiteral("resources")).toArray().size() != 44 ||
        evidence.value(QStringLiteral("blobs")).toArray().size() != 58 ||
        evidence.value(QStringLiteral("traces")).toArray().size() != 2 ||
        evidence.value(QStringLiteral("record_checks")).toArray().size() != 2 ||
        evidence.value(QStringLiteral("authorities")).toArray().size() != 44 ||
        uncertainties.size() != 14 || dimensions.size() != 7 || dimension_evidence.size() != 7) {
        fail("realism review/evidence envelope drift");
    }
    const QStringList expected_uncertainties{
        QStringLiteral("qualified-review-pending"),
        QStringLiteral("automated-legal-realism-limit"),
        QStringLiteral("exact-document-classification-scope"),
        QStringLiteral("waiver-allocution-boundary"),
        QStringLiteral("guidelines-role-boundary"),
        QStringLiteral("rule4b-day15-proof-boundary"),
        QStringLiteral("source-date-authority-boundary"),
        QStringLiteral("sealed-twin-access-boundary"),
        QStringLiteral("counterfactual-docket-isolation"),
        QStringLiteral("two-path-workflow-preflight"),
        QStringLiteral("role-level-court-authorization"),
        QStringLiteral("synthetic-identities-and-fraud-detail"),
        QStringLiteral("synthetic-bench-oral-limit"),
        QStringLiteral("generated-pdf-provenance-limit"),
    };
    for (qsizetype index = 0; index < uncertainties.size(); ++index) {
        const auto uncertainty = uncertainties.at(index).toObject();
        if (uncertainty.value(QStringLiteral("uncertainty_id")).toString() !=
                QStringLiteral("ca4m4.serrano.uncertainty.%1").arg(expected_uncertainties[index]) ||
            uncertainty.value(QStringLiteral("blocking")).toBool(true) ||
            uncertainty.value(QStringLiteral("summary")).toString().isEmpty()) {
            fail("known uncertainty identity/order/blocking drift");
        }
    }
    const QHash<QString, int> expected_dimension_counts{
        {QStringLiteral("procedural_law"), 54},     {QStringLiteral("deadlines_authority"), 26},
        {QStringLiteral("record_consistency"), 61}, {QStringLiteral("consequences"), 35},
        {QStringLiteral("oral_argument"), 28},      {QStringLiteral("bench_differentiation"), 4},
        {QStringLiteral("provenance"), 109},
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
    if (evidence_ids.size() != 150)
        fail("150 unique realism evidence IDs drift");
    for (auto iterator = expected_dimension_counts.constBegin();
         iterator != expected_dimension_counts.constEnd(); ++iterator) {
        const auto references = dimension_evidence.value(iterator.key()).toArray();
        const auto unique = strings(references);
        if (dimensions.value(iterator.key()).toInt() != 2 ||
            references.size() != iterator.value() || unique.size() != references.size() ||
            std::ranges::any_of(unique,
                                [&](const auto& id) { return !evidence_ids.contains(id); })) {
            fail("dimension evidence drift: " + iterator.key().toStdString());
        }
    }
    int case_authorities = 0;
    int ca4_authorities = 0;
    int federal_authorities = 0;
    for (const auto& value : evidence.value(QStringLiteral("authorities")).toArray()) {
        const auto id = value.toObject().value(QStringLiteral("authority_id")).toString();
        case_authorities += id.startsWith(QStringLiteral("ca4m4.serrano.")) ? 1 : 0;
        ca4_authorities += id.startsWith(QStringLiteral("us.ca4.")) ? 1 : 0;
        federal_authorities += id.startsWith(QStringLiteral("us.federal.")) ? 1 : 0;
        if (id == QStringLiteral("ca4m4.serrano.authority.mdnc-standing-order-41-sealed-access"))
            fail("unreferenced Standing Order 41 leaked into evidence authorities");
    }
    if (case_authorities != 18 || ca4_authorities != 11 || federal_authorities != 15)
        fail("44-authority 18+11+15 evidence partition drift");

    const auto entries = record.value(QStringLiteral("docket_entries")).toArray();
    const auto anchors = record.value(QStringLiteral("page_anchors")).toArray();
    const auto disclosures = record.value(QStringLiteral("sealed_disclosures")).toArray();
    const auto policy = record.value(QStringLiteral("disclosure_policy")).toObject();
    if (record.value(QStringLiteral("resource_id")).toString() !=
            QStringLiteral("ca4m4.serrano.record") ||
        record.value(QStringLiteral("dockets")).toArray().size() != 3 || entries.size() != 58 ||
        anchors.size() != 456 || disclosures.size() != 8 ||
        policy.value(QStringLiteral("unauthorized_projection")).toString() !=
            QStringLiteral("public_counterparts_only") ||
        policy.value(QStringLiteral("authorized_projection")).toString() !=
            QStringLiteral("public_and_authorized_sealed") ||
        policy.value(QStringLiteral("sealed_asset_access")).toString() !=
            QStringLiteral("session_event_grant_required")) {
        fail("record 3/58/456/8 policy envelope drift");
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
        const bool counterfactual =
            docket == QStringLiteral("ca4m4.serrano.docket.counterfactual-day15");
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
    const auto district = QStringLiteral("ca4m4.serrano.docket.district");
    const auto actual = QStringLiteral("ca4m4.serrano.docket.appellate");
    const auto counterfactual = QStringLiteral("ca4m4.serrano.docket.counterfactual-day15");
    if (docket_documents[district] != 28 || docket_pages[district] != 260 ||
        public_documents[district] != 24 || docket_documents[actual] != 20 ||
        docket_pages[actual] != 155 || public_documents[actual] != 16 ||
        docket_documents[counterfactual] != 10 || docket_pages[counterfactual] != 41 ||
        public_documents[counterfactual] != 10 || sealed_documents != 8) {
        fail("record split is not exact 28/260 + 20/155 + 10/41 with public 24/16/10");
    }

    QHash<QString, QString> entry_by_anchor;
    QHash<QString, int> anchors_by_entry;
    QSet<QString> citation_labels;
    for (const auto& value : anchors) {
        const auto anchor = value.toObject();
        const auto id = anchor.value(QStringLiteral("anchor_id")).toString();
        const auto entry_id = anchor.value(QStringLiteral("entry_id")).toString();
        const auto citation = anchor.value(QStringLiteral("citation_label")).toString();
        const auto entry = objectAt(entries_by_id, entry_id, "page-anchor entry");
        if (id.isEmpty() || entry_by_anchor.contains(id) || citation.isEmpty() ||
            citation_labels.contains(citation) ||
            anchor.value(QStringLiteral("page_number")).toInt() < 1 ||
            anchor.value(QStringLiteral("page_number")).toInt() >
                entry.value(QStringLiteral("page_count")).toInt()) {
            fail("page-anchor identity/page drift: " + id.toStdString());
        }
        entry_by_anchor.insert(id, entry_id);
        citation_labels.insert(citation);
        ++anchors_by_entry[entry_id];
    }
    for (auto iterator = entries_by_id.constBegin(); iterator != entries_by_id.constEnd();
         ++iterator) {
        if (anchors_by_entry[iterator.key()] !=
            iterator.value().value(QStringLiteral("page_count")).toInt()) {
            fail("entry/page-anchor count drift: " + iterator.key().toStdString());
        }
    }

    QSet<QString> stable_anchors;
    QSet<QString> mapped_sealed;
    QSet<QString> mapped_public;
    QSet<QString> sealed_entries;
    int mapping_count = 0;
    for (const auto& value : disclosures) {
        const auto disclosure = value.toObject();
        const auto sealed_id = disclosure.value(QStringLiteral("sealed_entry_id")).toString();
        const auto public_id = disclosure.value(QStringLiteral("public_entry_id")).toString();
        const auto sealed_entry = objectAt(entries_by_id, sealed_id, "sealed disclosure entry");
        const auto public_entry = objectAt(entries_by_id, public_id, "public disclosure entry");
        if (!sealed_entry.value(QStringLiteral("sealed")).toBool() ||
            public_entry.value(QStringLiteral("sealed")).toBool() ||
            sealed_entry.value(QStringLiteral("page_count")).toInt() !=
                public_entry.value(QStringLiteral("page_count")).toInt() ||
            sealed_entries.contains(sealed_id)) {
            fail("sealed/public twin identity drift: " + sealed_id.toStdString());
        }
        sealed_entries.insert(sealed_id);
        const auto mappings = disclosure.value(QStringLiteral("anchor_mappings")).toArray();
        for (const auto& mapping_value : mappings) {
            const auto mapping = mapping_value.toObject();
            const auto stable = mapping.value(QStringLiteral("stable_anchor_id")).toString();
            const auto sealed_anchor = mapping.value(QStringLiteral("sealed_anchor_id")).toString();
            const auto public_anchor = mapping.value(QStringLiteral("public_anchor_id")).toString();
            if (stable.isEmpty() || stable_anchors.contains(stable) ||
                mapped_sealed.contains(sealed_anchor) || mapped_public.contains(public_anchor) ||
                entry_by_anchor.value(sealed_anchor) != sealed_id ||
                entry_by_anchor.value(public_anchor) != public_id) {
                fail("sealed stable-anchor mapping is not bijective");
            }
            stable_anchors.insert(stable);
            mapped_sealed.insert(sealed_anchor);
            mapped_public.insert(public_anchor);
            entry_by_anchor.insert(stable, public_id);
            ++mapping_count;
        }
    }
    if (mapping_count != 95 || stable_anchors.size() != 95 || sealed_entries.size() != 8)
        fail("sealed twin 8/95 bijection drift");

    const auto plan =
        parseObject(readAll(root.filePath(QStringLiteral("metadata/successor-document-plan.json"))),
                    QStringLiteral("successor-document-plan.json"));
    const auto named = strings(plan.value(QStringLiteral("sealed_record_twins"))
                                   .toObject()
                                   .value(QStringLiteral("referenced_stable_anchor_ids"))
                                   .toArray());
    if (named.size() != 13 ||
        std::ranges::any_of(named, [&](const auto& id) { return !stable_anchors.contains(id); })) {
        fail("13 named stable-anchor closure drift");
    }

    QSet<QString> manifest_blob_paths;
    for (const auto& value : blobs) {
        const auto descriptor = value.toObject();
        const auto path = descriptor.value(QStringLiteral("path")).toString();
        const auto bytes = readAll(pack_root.filePath(path));
        const auto entry = objectAt(entries_by_path, path, "blob record entry");
        if (manifest_blob_paths.contains(path) ||
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
            fail("manifest PDF/page count cannot load: " + path.toStdString());
        }
        manifest_blob_paths.insert(path);
    }
    if (manifest_blob_paths.size() != 58 || entries_by_path.size() != 58)
        fail("58-PDF manifest/record closure drift");

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
            QStringLiteral("face8ece5199db46d6d612d315bf7af3a3591188bdcd648a59a0697528f88fa1") ||
        actual_bank.value(QStringLiteral("questions")).toArray().size() != 12 ||
        std::ranges::any_of(
            actual_entries,
            [](const auto& id) { return id.contains(QStringLiteral(".entry.b")); }) ||
        counterfactual_bank.value(QStringLiteral("mode")).toString() !=
            QStringLiteral("counterfactual_training") ||
        counterfactual_bank.value(QStringLiteral("grounding_digest")).toString() !=
            QStringLiteral("c7107662fee21072ebd45c0a722e4f6c61345582f81a5b171dea1313a181c391") ||
        counterfactual_bank.value(QStringLiteral("questions")).toArray().size() != 12 ||
        std::ranges::any_of(
            counterfactual_entries,
            [](const auto& id) { return id.contains(QStringLiteral(".entry.a")); }) ||
        !counterfactual_entries.contains(QStringLiteral("ca4m4.serrano.record.entry.b01")) ||
        !actual_entries.contains(QStringLiteral("ca4m4.serrano.record.entry.a06"))) {
        fail("actual/counterfactual question-grounding isolation drift");
    }

    const auto plans = case_definition.value(QStringLiteral("disposition_plans")).toArray();
    QHash<QString, QJsonObject> plans_by_id;
    for (const auto& value : plans) {
        const auto disposition = value.toObject();
        plans_by_id.insert(disposition.value(QStringLiteral("plan_id")).toString(), disposition);
    }
    const auto actual_plan = plans_by_id.value(
        QStringLiteral("ca4m4.serrano.disposition.actual-partial-dismissal-vacatur-remand"));
    const auto counterfactual_plan = plans_by_id.value(
        QStringLiteral("ca4m4.serrano.disposition.counterfactual-rule4b-dismissal"));
    const auto actions = [](const QJsonObject& disposition) {
        QStringList result;
        for (const auto& value : disposition.value(QStringLiteral("components")).toArray())
            result.push_back(value.toObject().value(QStringLiteral("action")).toString());
        return result;
    };
    if (case_definition.value(QStringLiteral("actors")).toArray().size() != 5 ||
        case_definition.value(QStringLiteral("issues")).toArray().size() != 4 ||
        plans.size() != 2 ||
        case_definition.value(QStringLiteral("authored_disposition_plan_id")).toString() !=
            QStringLiteral("ca4m4.serrano.disposition.actual-partial-dismissal-vacatur-remand") ||
        case_definition.value(QStringLiteral("authored_disposition_id")).toString() !=
            QStringLiteral("ca4m4.serrano.operation.issue-actual-judgment") ||
        actual_plan.value(QStringLiteral("digest")).toString() !=
            QStringLiteral("3cfe81a52fc9e30fdc23f9b7ed6f715458a395c1be46e953a03d4717ff86f41d") ||
        counterfactual_plan.value(QStringLiteral("digest")).toString() !=
            QStringLiteral("c40cd3254a41c4800b621ee8a63d29dd48dcf7817625679aa8f4c15f14a435c7") ||
        actions(actual_plan) != QStringList{QStringLiteral("affirm"), QStringLiteral("dismiss"),
                                            QStringLiteral("vacate")} ||
        actions(counterfactual_plan) != QStringList{QStringLiteral("dismiss")}) {
        fail("five-actor/four-issue/two-disposition case contract drift");
    }

    const auto stages = workflow.value(QStringLiteral("stages")).toArray();
    const auto operations = workflow.value(QStringLiteral("operations")).toArray();
    const auto routes = workflow.value(QStringLiteral("filing_routes")).toArray();
    if (workflow.value(QStringLiteral("resource_id")).toString().toStdString() != workflow_id ||
        stages.size() != 24 || operations.size() != 79 || routes.size() != 14)
        fail("workflow 24-stage/79-operation/14-route envelope drift");
    QSet<QString> stage_ids = strings(stages);
    QHash<QString, QJsonObject> operations_by_id;
    QSet<QString> produced_deadlines;
    QSet<QString> bound_entries;
    QHash<QString, int> opcode_counts;
    int document_bindings = 0;
    int disposition_bindings = 0;
    for (const auto& value : operations) {
        const auto operation = value.toObject();
        const auto id = operation.value(QStringLiteral("operation_id")).toString();
        const auto opcode = operation.value(QStringLiteral("opcode")).toString();
        if (id.isEmpty() || operations_by_id.contains(id) ||
            !stage_ids.contains(operation.value(QStringLiteral("stage_id")).toString()) ||
            operation.value(QStringLiteral("preconditions")).toArray().isEmpty() ||
            operation.value(QStringLiteral("allowed_legal_times")).toArray().isEmpty()) {
            fail("unguarded or invalid workflow operation: " + id.toStdString());
        }
        ++opcode_counts[opcode];
        if (opcode == QStringLiteral("calculate_deadline")) {
            const auto deadline =
                operation.value(QStringLiteral("produced_deadline_id")).toString();
            if (deadline.isEmpty() || produced_deadlines.contains(deadline))
                fail("duplicate produced deadline: " + deadline.toStdString());
            produced_deadlines.insert(deadline);
        }
        if (operation.contains(QStringLiteral("document_binding"))) {
            const auto binding = operation.value(QStringLiteral("document_binding")).toObject();
            const auto entry_id = binding.value(QStringLiteral("record_entry_id")).toString();
            const auto entry = objectAt(entries_by_id, entry_id, "operation-bound entry");
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
        if (!operations_by_id.contains(
                route.value(QStringLiteral("accept_operation_id")).toString()) ||
            !operations_by_id.contains(
                route.value(QStringLiteral("reject_operation_id")).toString())) {
            fail("route operation reference drift");
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
        {QStringLiteral("accept_filing"), 14},      {QStringLiteral("reject_filing"), 14},
        {QStringLiteral("calculate_deadline"), 16}, {QStringLiteral("enter_order"), 9},
        {QStringLiteral("advance_stage"), 21},      {QStringLiteral("schedule_argument"), 1},
        {QStringLiteral("issue_judgment"), 2},      {QStringLiteral("issue_mandate"), 2},
    };
    if (stage_ids.size() != 24 || operations_by_id.size() != 79 ||
        opcode_counts != expected_opcodes || filing_bindings != 16 || filing_ids.size() != 16 ||
        document_bindings != 13 || disposition_bindings != 2 || produced_deadlines.size() != 16 ||
        bound_entries.size() != 29) {
        fail("workflow 24/79/14/16/13/16/29 topology drift");
    }

    const auto select_actual = operations_by_id.value(
        QStringLiteral("ca4m4.serrano.operation.select-actual-appellate-path"));
    const auto select_counterfactual = operations_by_id.value(
        QStringLiteral("ca4m4.serrano.operation.select-counterfactual-day15-path"));
    const auto b09 = operations_by_id.value(
        QStringLiteral("ca4m4.serrano.operation.issue-counterfactual-judgment"));
    const auto b10 = operations_by_id.value(
        QStringLiteral("ca4m4.serrano.operation.issue-counterfactual-mandate"));
    if (!hasPrecondition(select_actual, u"filing_instance", u"filing_id",
                         u"ca4m4.serrano.filing.l27-actual-day14-notice", true) ||
        !hasPrecondition(select_actual, u"filing_instance", u"filing_id",
                         u"ca4m4.serrano.filing.b01-counterfactual-day15-notice", false) ||
        !hasPrecondition(select_counterfactual, u"filing_instance", u"filing_id",
                         u"ca4m4.serrano.filing.b01-counterfactual-day15-notice", true) ||
        !hasPrecondition(select_counterfactual, u"filing_instance", u"filing_id",
                         u"ca4m4.serrano.filing.l27-actual-day14-notice", false) ||
        !hasDeadlineStatus(select_actual, u"ca4m4.serrano.deadline.notice-of-appeal",
                           u"not_elapsed") ||
        !hasDeadlineStatus(select_counterfactual, u"ca4m4.serrano.deadline.notice-of-appeal",
                           u"elapsed") ||
        !hasDeadlineStatus(b09, u"ca4m4.serrano.deadline.counterfactual-rule4b4-outer-window",
                           u"reached") ||
        !hasDeadlineStatus(b09, u"ca4m4.serrano.deadline.counterfactual-rule4b4-outer-window",
                           u"not_elapsed") ||
        !hasDeadlineStatus(b10, u"ca4m4.serrano.deadline.counterfactual-rule4b4-outer-window",
                           u"elapsed")) {
        fail("actual/counterfactual branch mutex or outer-window guard drift");
    }
    const auto workflow_text = QString::fromUtf8(workflow_bytes);
    for (const auto& value : operations) {
        const auto operation = value.toObject();
        const auto id = operation.value(QStringLiteral("operation_id")).toString();
        if (id.contains(QStringLiteral("counterfactual"))) {
            const auto serialized =
                QString::fromUtf8(QJsonDocument(operation).toJson(QJsonDocument::Compact));
            if (serialized.contains(QStringLiteral("hunter-miscarriage-limit")) ||
                serialized.contains(QStringLiteral("melvin-role-adjustment"))) {
                fail("future authority leaked into counterfactual operation: " + id.toStdString());
            }
        }
    }
    if (!workflow_text.contains(QStringLiteral("hunter-miscarriage-limit")))
        fail("actual published-opinion authority binding lost Hunter");

    QCryptographicHash trace_closure(QCryptographicHash::Sha256);
    QHash<QString, QJsonObject> authored_traces;
    for (const auto& value : evidence.value(QStringLiteral("traces")).toArray()) {
        const auto trace = value.toObject();
        authored_traces.insert(trace.value(QStringLiteral("trace_id")).toString(), trace);
    }
    for (const auto& meta : trace_metas) {
        const auto relative_path = QStringLiteral("traces/%1").arg(meta.file);
        const auto bytes = readAll(root.filePath(relative_path));
        const auto path_bytes =
            QStringLiteral("content/m4/serrano-waiver/%1").arg(relative_path).toUtf8();
        trace_closure.addData(QByteArrayView(path_bytes));
        trace_closure.addData(QByteArrayView("\0", 1));
        trace_closure.addData(QByteArrayView(bytes));
        trace_closure.addData(QByteArrayView("\0", 1));
        const auto trace = parseObject(bytes, relative_path);
        if (authored_traces.value(trace.value(QStringLiteral("trace_id")).toString()) != trace)
            fail("authored realism trace differs from committed trace: " +
                 relative_path.toStdString());
    }
    if (trace_closure.result().toHex().toStdString() != ReleasePins::trace_closure ||
        authored_traces.size() != 2) {
        fail("two-trace framed closure/evidence binding drift");
    }
}

[[nodiscard]] QString asQString(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

void auditInstalledRecord(const QDir& temporary_root, packs::PackCatalog& catalog,
                          const packs::ResolvedPack& resolved, const packs::RuntimePack& runtime) {
    const auto& runtime_case = runtime.cases.front();
    const auto& record = runtime_case.record;
    const auto mapping_count = std::ranges::fold_left(
        record.sealed_disclosures, std::size_t{0}, [](std::size_t total, const auto& disclosure) {
            return total + disclosure.anchor_mappings.size();
        });
    if (record.id.value != "ca4m4.serrano.record" || record.dockets.size() != 3U ||
        record.docket_entries.size() != 58U || record.page_anchors.size() != 456U ||
        !record.disclosure_policy || record.sealed_disclosures.size() != 8U ||
        mapping_count != 95U) {
        fail("installed record 3/58/456/8/95 envelope drift");
    }

    ui::RecordWorkspace workspace;
    app::InstalledRecordController installed_controller(catalog, workspace);
    const auto installed = installed_controller.load(resolved, runtime, runtime_case.definition.id);
    if (!installed)
        fail("installed record load failed: " + installed.error().message.toStdString());
    const auto& full = ui::RecordWorkspaceTestAccess::fullDefinition(workspace);
    if (installed->definition.documents.size() != 50U ||
        installed->definition.docket.size() != 50U || installed->assets.size() != 50U ||
        workspace.visibleDocketCount() != 50 || full.documents.size() != 58U ||
        full.sealed_disclosures.size() != 8U) {
        fail("installed public/full record projection is not exact 50/58/8");
    }

    const auto& first = record.sealed_disclosures.front();
    if (!first.public_entry_id || first.anchor_mappings.empty())
        fail("first disclosure lacks a public stable-anchor twin");
    const auto sealed_id = asQString(first.sealed_entry_id.value);
    const auto public_id = asQString(first.public_entry_id->value);
    const auto stable_anchor = asQString(first.anchor_mappings.front().stable_anchor_id.value);
    if (!workspace.navigateToAnchor(stable_anchor) || workspace.currentDocumentId() != public_id ||
        workspace.openDocketEntry(sealed_id)) {
        fail("unauthorized stable anchor exposed its sealed twin");
    }

    auto store = storage::SessionStore::open(
        temporary_root.filePath(QStringLiteral("serrano-record-access.sqlite")));
    if (!store)
        fail("record-access store cannot open");
    auto access = app::RecordAccessSessionController::create(
        QStringLiteral("integration.serrano.record-access"), runtime_case.definition.id,
        std::move(*store), QStringLiteral("engine.record-access.v1"),
        QStringLiteral("2026-08-19T03:00:00Z"), resolved);
    if (!access)
        fail("record-access session cannot bind exact closure");
    const auto statuses = (*access)->disclosures();
    if (statuses.size() != 8U || std::ranges::any_of(statuses, [](const auto& status) {
            return status.authorized || !status.blocking_deficiencies.empty();
        })) {
        fail("initial eight-disclosure policy is not eligible and unauthorized");
    }
    if (!(*access)->applyCurrentProjection(workspace))
        fail("initial public record projection cannot apply");
    const auto disclosure_id = first.disclosure_id.value;
    if (!(*access)->grant(disclosure_id, "integration.serrano.grant.1",
                          QStringLiteral("2026-08-19T03:00:01Z")) ||
        !(*access)->applyCurrentProjection(workspace) || workspace.visibleDocketCount() != 51 ||
        !workspace.navigateToAnchor(stable_anchor) || workspace.currentDocumentId() != sealed_id) {
        fail("one disclosure grant did not produce exact public50-to-grant51 behavior");
    }
    if (!(*access)->revoke(disclosure_id, "integration.serrano.revoke.1",
                           QStringLiteral("2026-08-19T03:00:02Z")) ||
        !(*access)->applyCurrentProjection(workspace) || workspace.visibleDocketCount() != 50 ||
        !workspace.currentDocumentId().isEmpty() || !workspace.navigateToAnchor(stable_anchor) ||
        workspace.currentDocumentId() != public_id || workspace.openDocketEntry(sealed_id)) {
        fail("revocation did not restore exact public50 projection");
    }
}

} // namespace

int main(int argc, char** argv) try {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    const QDir content_root(QStringLiteral(APPELLATE_M4_SERRANO_ROOT));
    const QDir foundations(QStringLiteral(APPELLATE_M4_FOUNDATIONS));
    auditStaticContent(content_root);

    const model::PackRevision root_revision{model::PackId{"us.ca4.m4.serrano-waiver"}, "1.2.0",
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
    if (!loaded || loaded->revision != root_revision ||
        loaded->graph_state != packs::PackGraphState::DeferredReferences ||
        loaded->resources.size() != 9U || loaded->blobs.size() != 58U ||
        loaded->required_capabilities.size() != 17U || loaded->dependencies.size() != 3U) {
        fail("final deferred root cannot load with exact 9/58/17/3 envelope");
    }

    QTemporaryDir temporary;
    if (!temporary.isValid())
        fail("cannot create Serrano integration temporary directory");
    const QDir temporary_root(temporary.path());
    const auto archive_a = temporary_root.filePath(QStringLiteral("serrano-a.awpack"));
    const auto archive_b = temporary_root.filePath(QStringLiteral("serrano-b.awpack"));
    const auto exported_a = packs::PackArchive::exportDirectory(
        pack_root.path(), archive_a, {}, packs::PackValidationScope::ResolvedClosure);
    const auto exported_b = packs::PackArchive::exportDirectory(
        pack_root.path(), archive_b, {}, packs::PackValidationScope::ResolvedClosure);
    const auto archive_bytes = readAll(archive_a);
    if (!exported_a || !exported_b || *exported_a != root_revision ||
        *exported_b != root_revision || archive_bytes != readAll(archive_b) ||
        static_cast<std::uint64_t>(archive_bytes.size()) != ReleasePins::archive_byte_size ||
        sha256(archive_bytes).toStdString() != ReleasePins::archive_sha256) {
        fail("double export archive/root pin or determinism drift");
    }

    const auto catalog =
        packs::PackCatalog::open(temporary_root.filePath(QStringLiteral("catalog")));
    if (!catalog)
        fail("cannot open Serrano integration catalog");
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
        installed_root->revision != root_revision ||
        installed_root->archive_sha256.toStdString() != ReleasePins::archive_sha256) {
        fail("exact three-dependency plus root installation drift");
    }
    const auto listed = (*catalog)->list();
    const auto resolved = (*catalog)->loadResolved(root_revision);
    if (!listed || listed->size() != 4U || !resolved ||
        resolved->revisionsByPackId().size() != 4U) {
        fail("exact four-revision catalog/resolution drift");
    }
    const auto runtime = packs::loadRuntimePack(*resolved);
    if (!runtime || runtime->revision != root_revision || runtime->cases.size() != 1U)
        fail("resolved Serrano runtime cannot load");
    const auto& runtime_case = runtime->cases.front();
    if (runtime_case.definition.id.value != case_id ||
        runtime_case.workflow.id.value != workflow_id ||
        runtime_case.workflow.stages.size() != 24U ||
        runtime_case.workflow.operations.size() != 79U ||
        runtime_case.workflow.filing_routes.size() != 14U ||
        runtime_case.record.docket_entries.size() != 58U ||
        runtime_case.record.page_anchors.size() != 456U ||
        runtime_case.record.sealed_disclosures.size() != 8U ||
        runtime_case.argument_configurations.size() != 2U ||
        runtime_case.definition.disposition_plans.size() != 2U) {
        fail("resolved runtime shape drift");
    }

    auditInstalledRecord(temporary_root, **catalog, *resolved, *runtime);

    const QDir trace_dir(content_root.filePath(QStringLiteral("traces")));
    QStringList expected_trace_files;
    for (const auto& meta : trace_metas)
        expected_trace_files.push_back(meta.file);
    expected_trace_files.sort();
    if (trace_dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name) !=
        expected_trace_files) {
        fail("trace file inventory drift");
    }
    TraceTotals totals;
    for (const auto& meta : trace_metas)
        auditTrace(trace_dir, meta, runtime_case, totals);
    std::set<std::string> defined_operations;
    for (const auto& operation : runtime_case.workflow.operations)
        defined_operations.insert(operation.id.value);
    std::set<std::string> defined_filings;
    for (const auto& route : runtime_case.workflow.filing_routes)
        for (const auto& binding : route.filing_bindings)
            defined_filings.insert(binding.filing_id.value);
    const std::map<std::string, std::string> expected_deadlines{
        {"ca4m4.serrano.deadline.notice-of-appeal", "2026-03-11"},
        {"ca4m4.serrano.deadline.actual-docketing-statement", "2026-03-23"},
        {"ca4m4.serrano.deadline.actual-appearances", "2026-03-23"},
        {"ca4m4.serrano.deadline.actual-waiver-opposition", "2026-05-04"},
        {"ca4m4.serrano.deadline.actual-allocution-response", "2026-05-19"},
        {"ca4m4.serrano.deadline.actual-allocution-reply", "2026-06-02"},
        {"ca4m4.serrano.deadline.actual-rehearing", "2026-08-11"},
        {"ca4m4.serrano.deadline.actual-mandate", "2026-08-18"},
        {"ca4m4.serrano.deadline.counterfactual-extension-statement", "2026-03-18"},
        {"ca4m4.serrano.deadline.counterfactual-extension-findings", "2026-03-20"},
        {"ca4m4.serrano.deadline.counterfactual-record-return", "2026-03-23"},
        {"ca4m4.serrano.deadline.counterfactual-rule4b4-outer-window", "2026-04-10"},
        {"ca4m4.serrano.deadline.counterfactual-threshold-opposition", "2026-04-06"},
        {"ca4m4.serrano.deadline.counterfactual-threshold-reply", "2026-04-06"},
        {"ca4m4.serrano.deadline.counterfactual-rehearing", "2026-04-24"},
        {"ca4m4.serrano.deadline.counterfactual-mandate", "2026-05-01"},
    };
    if (totals.commands != 86U || totals.events != 86U || totals.prefix_replays != 86U ||
        totals.full_replays != 4U || totals.operations != defined_operations ||
        totals.operations.size() != 79U || totals.accepted_filings != defined_filings ||
        totals.rejected_filings != defined_filings || defined_filings.size() != 16U ||
        totals.deadlines != expected_deadlines) {
        fail("two-trace 51+35/union79/deadline closure drift");
    }

    std::cout << "Serrano waiver integration passed: 9 resources / 58 PDFs / 456 anchors / "
                 "8 sealed disclosures / 95 mappings / public50-grant51; 24 stages / 79 "
                 "operations / 14 routes / 16 deadlines; traces 51+35 with union79.\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Serrano waiver integration failed: " << error.what() << '\n';
    return 1;
}
