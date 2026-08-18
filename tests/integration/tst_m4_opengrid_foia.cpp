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
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QTimeZone>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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

#ifndef APPELLATE_M4_OPENGRID_ROOT
#error "APPELLATE_M4_OPENGRID_ROOT must name content/m4/opengrid-foia"
#endif

#ifndef APPELLATE_M4_FOUNDATIONS
#error "APPELLATE_M4_FOUNDATIONS must name content/foundations"
#endif

namespace model = appellate::model;
namespace engine = appellate::engine;
namespace packs = appellate::packs;
namespace storage = appellate::storage;
using namespace std::chrono_literals;

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
    "94ff5a6f3e89686c6119578033cbec7218afe5b0543ddbebba6e54e787bf3f70";
constexpr std::string_view realism_review_sha256 =
    "2f0b0b06fd7de51dd2fa53466458b3e3e4ba7bc1c181ea0342c543e65b36ba34";
constexpr std::string_view evidence_closure =
    "1acce5699c11f75736f2f611e0eae6158f37f26970bf1f7d8b99ddbd85956534";
constexpr std::string_view root_revision =
    "9cb2879b1cc27e98d8def7c926a38e9f4eb2cbec90785be74c009156b4a1e4c5";
constexpr std::string_view archive_sha256 =
    "1efa067767f3c729bbd67c40b3faa239673025f421133bddf32ec6b090231b09";
constexpr std::uint64_t archive_byte_size = 5'244'039;
constexpr std::string_view record_sha256 =
    "4fb13f25af4e06234cfa0ffbb0c0f77b7476ad7c65ed9365cc9642cb38f27f5a";
constexpr std::string_view workflow_sha256 =
    "66361faed640a8e274834b49595061b51d5fc41b1ef34b4d494f0ca49de67503";
constexpr std::string_view trace_plan_sha256 =
    "89b30afb72227c71f32926fdc418b25727b4914ad15e13240dc8d8cc1d371ddf";
constexpr std::string_view trace_closure =
    "9247eb77b22dc166db60ef7d984ac70114b00af4387cd61ebdc1d377654b02ae";
} // namespace ReleasePins

constexpr std::string_view entry_prefix = "ca4m4.opengrid.record.entry.";

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

[[nodiscard]] bool copyDirectory(const QString& source_path, const QString& target_path) {
    const QDir source(source_path);
    if (!source.exists() || !QDir().mkpath(target_path))
        return false;
    for (const auto& info :
         source.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDir::Name)) {
        const auto target = QDir(target_path).filePath(info.fileName());
        if (info.isDir()) {
            if (!copyDirectory(info.absoluteFilePath(), target))
                return false;
        } else if (!QFile::copy(info.absoluteFilePath(), target)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] QByteArray sha256(QByteArrayView bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

[[nodiscard]] QByteArray sha256(const QByteArray& bytes) { return sha256(QByteArrayView(bytes)); }

[[nodiscard]] QJsonObject parseObject(const QByteArray& bytes, const QString& path) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        fail("invalid JSON object " + path.toStdString());
    }
    return document.object();
}

[[nodiscard]] QSet<QString> strings(const QJsonArray& values) {
    QSet<QString> result;
    for (const auto& value : values) {
        result.insert(value.toString());
    }
    return result;
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
    const auto year = static_cast<int>(date.value.year());
    const auto month = static_cast<unsigned>(date.value.month());
    const auto day = static_cast<unsigned>(date.value.day());
    char buffer[11]{};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02u", year, month, day);
    return buffer;
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
        const auto command_encoded =
            entry.value(QStringLiteral("command_base64")).toString().toLatin1();
        const auto command = QByteArray::fromBase64(command_encoded);
        const auto events = entry.value(QStringLiteral("events_base64")).toArray();
        if (entry.isEmpty() || command.isEmpty() || command.toBase64() != command_encoded) {
            return std::nullopt;
        }
        addFrame(hash, QByteArrayView(command));
        addUint64(hash, static_cast<std::uint64_t>(events.size()));
        for (const auto& event : events) {
            const auto encoded = event.toString().toLatin1();
            const auto decoded = QByteArray::fromBase64(encoded);
            if (decoded.isEmpty() || decoded.toBase64() != encoded) {
                return std::nullopt;
            }
            addFrame(hash, QByteArrayView(decoded));
        }
    }
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] QString traceDigest(const QJsonObject& trace) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, QByteArrayView("appellate-workbench-executed-trace-evidence-v1"));
    addFrame(hash, QStringLiteral("ca4m4.case.opengrid-foia"));
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
    for (const auto& value : operation_ids) {
        addFrame(hash, value.toString());
    }
    addFrame(hash, trace.value(QStringLiteral("terminal_stage_id")).toString());
    return QString::fromLatin1(hash.result().toHex());
}

struct Meta final {
    QString file;
    std::string path;
    std::string trace_id;
    std::string evidence_id;
    std::size_t commands;
    std::string terminal;
    std::string file_sha256;
    std::string journal_sha256;
    std::string digest;
};

const std::array<Meta, 5> metas{
    Meta{QStringLiteral("actual-ordinary-through-mandate.json"), "actual",
         "ca4m4.opengrid.trace.actual-ordinary", "ca4m4.opengrid.evidence.trace.actual-ordinary",
         42, "ca4m4.opengrid.stage.terminated",
         "bbdda19bf1435be26279ff230adbaeb82b165e2fc90c28b81976d83f368e0a46",
         "e158e057f05bafee0dc3ddc27a13f32f79b455cfc98062e3fb367caac0a1db36",
         "496d734ed581c990debfdb99076270d8bbb59e61090202fea9b53a3ae1b62eee"},
    Meta{QStringLiteral("counterfactual-rehearing-denial-through-mandate.json"), "cf-rehearing",
         "ca4m4.opengrid.trace.counterfactual-rehearing-denial",
         "ca4m4.opengrid.evidence.trace.counterfactual-rehearing-denial", 40,
         "ca4m4.opengrid.stage.terminated",
         "415e1ecaff88005e75916ba7b315ee98586180203322fd1ea6ee948425c9d5ac",
         "2becec6d9dc58a851a687832b5a8ca6c40390090d02834a07a386d0404a9f962",
         "63dcbece5924a73a30460af689cf9005adf5c165c901ea1aa1953e69abe9c4a7"},
    Meta{QStringLiteral("counterfactual-stay-denial-through-mandate.json"), "cf-denial",
         "ca4m4.opengrid.trace.counterfactual-stay-denial",
         "ca4m4.opengrid.evidence.trace.counterfactual-stay-denial", 49,
         "ca4m4.opengrid.stage.terminated",
         "876c229f8443cfbc987a230e554895989f0c4b1c1bda50f441a119c6e32f2b22",
         "66cada38e814196fbf182e4bfaaed5ae9f3b8d1e969f850a87ad3e08a5b80a98",
         "f8b78013cc1369bddaa1ea08f4a3120199ed1373527908c593433a0fd19f5842"},
    Meta{QStringLiteral("counterfactual-rehearing-grant-mandate-stayed.json"), "cf-grant-blocked",
         "ca4m4.opengrid.trace.counterfactual-rehearing-grant-blocked",
         "ca4m4.opengrid.evidence.trace.counterfactual-rehearing-grant-blocked", 39,
         "ca4m4.opengrid.stage.counterfactual-mandate-stayed",
         "c904aa859eb73463860164299c99ffacbd6e90458a8dba71b5f367541bc9a91e",
         "0a73e1a551e783f1b16aa66edc53fac0f24f80b0ff53bf88dc0d23ce17bd60ac",
         "f838f3275d96a866adaccb53f5f96f96145805246cf04296b88f909e819689c6"},
    Meta{QStringLiteral("counterfactual-rehearing-grant-through-revised-mandate.json"),
         "cf-grant-complete", "ca4m4.opengrid.trace.counterfactual-rehearing-grant-complete",
         "ca4m4.opengrid.evidence.trace.counterfactual-rehearing-grant-complete", 51,
         "ca4m4.opengrid.stage.terminated",
         "708b0f0fe83f1adc14f36f1b58fdc8f8ddc25a2ad7e4c2d913d89c3d3cd13f46",
         "d84750ce6c774fb9ae3bdb29b483f7ba9a7c422446cb215f6e6fca8ffbf10fc9",
         "fd75f8dfca868d5da62568009f5ef880a8ad10e1fc9ec75f940255eb6abaa2bc"},
};

[[nodiscard]] std::set<std::string> expectedEntries(std::string_view path) {
    std::set<std::string> codes;
    const auto add = [&](std::string_view text) {
        std::size_t start{};
        while (start < text.size()) {
            const auto end = text.find(' ', start);
            codes.emplace(text.substr(start, end == text.npos ? text.size() - start : end - start));
            if (end == text.npos)
                break;
            start = end + 1U;
        }
    };
    if (path == "actual") {
        add("l36 l37 a01 a02 a03 a04 a05 a06 a08 a10 a12 a14 a15 a16 a17 a18 a19");
    } else if (path == "cf-rehearing") {
        add("l36 l37 b01 b02 b03 b04 b06 b08 b10 b11 b12 b13 b14 b15 b16");
    } else if (path == "cf-denial") {
        add("l36 l37 b01 b02 b03 b04 b06 b08 b10 b11 b12 b13 b14 b15 b17 b18 b19 b20");
    } else if (path == "cf-grant-blocked") {
        add("l36 l37 b01 b02 b03 b04 b06 b08 b10 b11 b12 b13 b14 b21 b22");
    } else if (path == "cf-grant-complete") {
        add("l36 l37 b01 b02 b03 b04 b06 b08 b10 b11 b12 b13 b14 b21 b22 b23 b24 b26 b27 b28");
    } else {
        fail("unknown path");
    }
    std::set<std::string> result;
    for (const auto& code : codes)
        result.insert(std::string(entry_prefix) + code);
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

template <typename Mutator>
void rejectTamper(const Meta& meta, const packs::RuntimeCase& runtime_case,
                  const model::WorkflowState& initial,
                  const std::vector<model::WorkflowJournalEntry>& source, std::string_view name,
                  Mutator mutate) {
    auto copy = source;
    if (!mutate(copy))
        fail(meta.path + " had no target for " + std::string(name) + " tamper");
    if (engine::replayWorkflow(runtime_case.workflow, runtime_case.definition, initial, copy)) {
        fail(meta.path + " accepted " + std::string(name) + " tamper");
    }
}

[[nodiscard]] std::size_t auditTampers(const Meta& meta, const packs::RuntimeCase& runtime_case,
                                       const model::WorkflowState& initial,
                                       const std::vector<model::WorkflowJournalEntry>& source) {
    rejectTamper(meta, runtime_case, initial, source, "event-sequence", [](auto& journal) {
        ++std::visit([](auto& event) -> auto& { return event.header.sequence; },
                     journal.front().events.front());
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
    rejectTamper(meta, runtime_case, initial, source, "actor-substitution", [&](auto& journal) {
        for (auto& entry : journal) {
            auto* filing = std::get_if<model::SubmitWorkflowFiling>(&entry.command);
            if (!filing || entry.events.empty() ||
                !std::holds_alternative<model::WorkflowFilingAccepted>(entry.events.front()))
                continue;
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
        for (auto& entry : journal)
            if (auto* order = std::get_if<model::EnterWorkflowOrder>(&entry.command)) {
                order->order_id = model::WorkflowOrderId{"ca4m4.opengrid.order.hostile-tamper"};
                return true;
            }
        return false;
    });
    rejectTamper(meta, runtime_case, initial, source, "order-disposition", [](auto& journal) {
        for (auto& entry : journal)
            if (auto* order = std::get_if<model::EnterWorkflowOrder>(&entry.command)) {
                order->disposition = order->disposition == model::WorkflowOrderDisposition::Granted
                                         ? model::WorkflowOrderDisposition::Denied
                                         : model::WorkflowOrderDisposition::Granted;
                return true;
            }
        return false;
    });
    rejectTamper(meta, runtime_case, initial, source, "deadline-date", [](auto& journal) {
        for (auto& entry : journal)
            for (auto& event : entry.events)
                if (auto* deadline = std::get_if<model::WorkflowDeadlineCalculated>(&event)) {
                    deadline->due_date.value = std::chrono::year_month_day{
                        std::chrono::sys_days{deadline->due_date.value} + std::chrono::days{1}};
                    return true;
                }
        return false;
    });
    rejectTamper(meta, runtime_case, initial, source, "deadline-ID", [](auto& journal) {
        for (auto& entry : journal)
            for (auto& event : entry.events)
                if (auto* deadline = std::get_if<model::WorkflowDeadlineCalculated>(&event)) {
                    deadline->deadline_id =
                        model::WorkflowDeadlineId{"ca4m4.opengrid.deadline.hostile-tamper"};
                    return true;
                }
        return false;
    });
    rejectTamper(meta, runtime_case, initial, source, "deadline-event-base", [](auto& journal) {
        for (auto& entry : journal)
            for (auto& event : entry.events) {
                auto* deadline = std::get_if<model::WorkflowDeadlineCalculated>(&event);
                if (!deadline || !deadline->deadline_event_base)
                    continue;
                if (std::holds_alternative<model::WorkflowJudgmentOccurredDeadlineBase>(
                        *deadline->deadline_event_base)) {
                    deadline->deadline_event_base = model::WorkflowOrderOccurredDeadlineBase{
                        model::WorkflowOrderId{"ca4m4.opengrid.order.hostile-deadline-base"},
                        model::WorkflowOperationId{
                            "ca4m4.opengrid.operation.hostile-deadline-base"}};
                    return true;
                }
                bool changed = false;
                std::visit(
                    [&](auto& base) {
                        if constexpr (requires { base.operation_id; }) {
                            base.operation_id = model::WorkflowOperationId{
                                "ca4m4.opengrid.operation.hostile-tamper"};
                            changed = true;
                        }
                    },
                    *deadline->deadline_event_base);
                if (changed)
                    return true;
            }
        return false;
    });
    rejectTamper(
        meta, runtime_case, initial, source, "dependent-deadline-base-ID", [](auto& journal) {
            for (auto& entry : journal)
                for (auto& event : entry.events) {
                    auto* deadline = std::get_if<model::WorkflowDeadlineCalculated>(&event);
                    if (deadline && deadline->deadline_base_id) {
                        deadline->deadline_base_id =
                            model::WorkflowDeadlineId{"ca4m4.opengrid.deadline.hostile-base"};
                        return true;
                    }
                }
            return false;
        });
    rejectTamper(meta, runtime_case, initial, source, "authority", [](auto& journal) {
        std::visit(
            [](auto& event) {
                event.header.authority.primary.id =
                    model::AuthorityId{"us.federal.authority.hostile-tamper"};
            },
            journal.front().events.front());
        return true;
    });
    rejectTamper(
        meta, runtime_case, initial, source, "applied-record-provenance", [](auto& journal) {
            for (auto& entry : journal)
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
                                                    "ca4m4.opengrid.record.entry.hostile-tamper";
                                                if constexpr (requires {
                                                                  condition.document_sha256;
                                                              }) {
                                                    condition.document_sha256 =
                                                        std::string(64, '0');
                                                }
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
            return false;
        });
    return 13U;
}

struct Totals final {
    std::set<std::string> operations;
    std::set<std::string> accepted;
    std::set<std::string> rejected;
    std::set<std::string> court_documents;
    std::map<std::string, std::string> deadlines;
    std::size_t commands{};
    std::size_t events{};
    std::size_t rejections{};
    std::size_t prefix_replays{};
    std::size_t full_replays{};
    std::size_t tampers{};
    std::map<std::string, std::size_t> terminals;
};

void auditTrace(const QDir& trace_dir, const Meta& meta, const packs::RuntimeCase& runtime_case,
                Totals& totals) {
    const auto trace_bytes = readAll(trace_dir.filePath(meta.file));
    const auto trace = parseObject(trace_bytes, meta.file);
    const auto allowed_keys =
        QSet<QString>{QStringLiteral("command_count"),     QStringLiteral("digest"),
                      QStringLiteral("engine_revision"),   QStringLiteral("event_count"),
                      QStringLiteral("evidence_id"),       QStringLiteral("journal"),
                      QStringLiteral("journal_sha256"),    QStringLiteral("operation_ids"),
                      QStringLiteral("terminal_stage_id"), QStringLiteral("trace_id"),
                      QStringLiteral("workflow_id")};
    const auto trace_keys = trace.keys();
    const auto journal_values = trace.value(QStringLiteral("journal")).toArray();
    const auto computed_journal_digest = journalDigest(journal_values);
    if (sha256(trace_bytes).toStdString() != meta.file_sha256 ||
        QSet<QString>(trace_keys.cbegin(), trace_keys.cend()) != allowed_keys ||
        trace.value(QStringLiteral("trace_id")).toString().toStdString() != meta.trace_id ||
        trace.value(QStringLiteral("evidence_id")).toString().toStdString() != meta.evidence_id ||
        trace.value(QStringLiteral("workflow_id")).toString() !=
            QStringLiteral("ca4m4.opengrid.workflow.foia-sealed-appeal") ||
        trace.value(QStringLiteral("engine_revision")).toString() !=
            QStringLiteral("appellate.realism-evidence.codec-replay-multi.v1") ||
        trace.value(QStringLiteral("command_count")).toInteger() !=
            static_cast<qint64>(meta.commands) ||
        trace.value(QStringLiteral("event_count")).toInteger() !=
            static_cast<qint64>(meta.commands) ||
        !computed_journal_digest || computed_journal_digest->toStdString() != meta.journal_sha256 ||
        trace.value(QStringLiteral("journal_sha256")).toString().toStdString() !=
            meta.journal_sha256 ||
        traceDigest(trace).toStdString() != meta.digest ||
        trace.value(QStringLiteral("digest")).toString().toStdString() != meta.digest ||
        trace.value(QStringLiteral("terminal_stage_id")).toString().toStdString() !=
            meta.terminal) {
        fail(meta.path + " trace envelope drift");
    }
    const auto journal = decodeJournal(journal_values, meta.path);
    if (journal.size() != meta.commands || journal.empty())
        fail(meta.path + " journal count drift");
    const auto initial = initialState(runtime_case.workflow, journal.front().command);
    auto rolling = initial;
    std::uint64_t expected_sequence = 1;
    std::vector<std::string> operations;
    std::set<std::string> documents;
    std::map<std::string, std::size_t> document_positions;
    std::vector<std::pair<std::size_t, std::string>> rejected;
    std::vector<std::pair<std::size_t, std::string>> accepted;
    std::string session;
    std::size_t event_count = 0;
    for (std::size_t index = 0; index < journal.size(); ++index) {
        const auto& entry = journal.at(index);
        const auto& command_header = commandHeader(entry.command);
        if (session.empty())
            session = command_header.session_id;
        if (command_header.session_id != session ||
            command_header.command_id.value != session + ".command." + std::to_string(index + 1U)) {
            fail(meta.path + " command/session identity drift at " + std::to_string(index));
        }
        const auto command_date = dateText(command_header.occurred_at.court_date);
        const auto qdate = QDate::fromString(QString::fromStdString(command_date), Qt::ISODate);
        if (!qdate.isValid() || QDateTime(qdate, QTime(0, 0), QTimeZone::UTC).toSecsSinceEpoch() !=
                                    std::chrono::duration_cast<std::chrono::seconds>(
                                        command_header.occurred_at.instant.time_since_epoch())
                                        .count()) {
            fail(meta.path + " LegalTime date/instant mismatch at " + std::to_string(index));
        }
        const auto decided = engine::decideWorkflow(runtime_case.workflow, runtime_case.definition,
                                                    rolling, entry.command);
        if (!decided || *decided != entry.events)
            fail(meta.path + " redecision drift at " + std::to_string(index));
        const auto prefix =
            std::span<const model::WorkflowJournalEntry>(journal.data(), index + 1U);
        const auto replayed =
            engine::replayWorkflow(runtime_case.workflow, runtime_case.definition, initial, prefix);
        if (!replayed)
            fail(meta.path + " prefix replay failed at " + std::to_string(index));
        rolling = *replayed;
        ++totals.prefix_replays;
        if (const auto digest = commandDocumentDigest(entry.command)) {
            const auto record_entry = std::ranges::find(runtime_case.record.docket_entries, *digest,
                                                        &packs::RuntimeDocketEntry::asset_sha256);
            if (record_entry == runtime_case.record.docket_entries.end() ||
                record_entry->filed_on != command_header.occurred_at.court_date) {
                fail(meta.path + " document/date does not resolve at " + std::to_string(index));
            }
            documents.insert(record_entry->id.value);
            document_positions.emplace(record_entry->id.value, index);
            if (!std::holds_alternative<model::SubmitWorkflowFiling>(entry.command)) {
                totals.court_documents.insert(record_entry->id.value);
            }
        }
        for (std::size_t event_index = 0; event_index < entry.events.size(); ++event_index) {
            const auto& event = entry.events.at(event_index);
            const auto& header = eventHeader(event);
            if (header.session_id != session || header.command_id != command_header.command_id ||
                header.occurred_at != command_header.occurred_at ||
                header.sequence != expected_sequence || header.command_event_index != event_index ||
                header.command_event_count != entry.events.size()) {
                fail(meta.path + " event header drift at " + std::to_string(index));
            }
            ++expected_sequence;
            operations.push_back(header.operation_id.value);
            totals.operations.insert(header.operation_id.value);
            ++event_count;
            if (const auto* rejected_event = std::get_if<model::WorkflowFilingRejected>(&event)) {
                if (rejected_event->reason !=
                    model::WorkflowFilingRejectionReason::NonconformingFiling)
                    fail(meta.path + " rejection is not Nonconforming");
                rejected.emplace_back(event_count - 1U, rejected_event->filing_id.value);
                totals.rejected.insert(rejected_event->filing_id.value);
                ++totals.rejections;
            }
            if (const auto* accepted_event = std::get_if<model::WorkflowFilingAccepted>(&event)) {
                accepted.emplace_back(event_count - 1U, accepted_event->filing_id.value);
                totals.accepted.insert(accepted_event->filing_id.value);
            }
            if (const auto* deadline = std::get_if<model::WorkflowDeadlineCalculated>(&event)) {
                const auto id = deadline->deadline_id.value;
                const auto date = dateText(deadline->due_date);
                const auto [found, inserted] = totals.deadlines.emplace(id, date);
                if (!inserted && found->second != date)
                    fail("deadline date changes across traces: " + id);
            }
        }
    }
    const auto declared = trace.value(QStringLiteral("operation_ids")).toArray();
    if (declared.size() != static_cast<qsizetype>(operations.size()))
        fail(meta.path + " operation list count drift");
    for (qsizetype index = 0; index < declared.size(); ++index)
        if (declared.at(index).toString().toStdString() !=
            operations.at(static_cast<std::size_t>(index)))
            fail(meta.path + " operation list order drift");
    for (const auto& [position, id] : rejected) {
        const auto recovery = std::ranges::find_if(
            accepted, [&](const auto& item) { return item.first > position && item.second == id; });
        if (recovery == accepted.end())
            fail(meta.path + " rejected filing lacks same-trace same-ID recovery: " + id);
    }
    if (documents != expectedEntries(meta.path))
        fail(meta.path + " exact document set drift");
    const auto has = [&](std::string_view code) {
        return documents.contains(std::string(entry_prefix) + std::string(code));
    };
    if (meta.path == "actual") {
        if (!has("a06") || has("b01") || has("b06"))
            fail("actual A/CF mutex drift");
    } else if (!has("b01") || !has("b06") || has("a06")) {
        fail(meta.path + " A/CF mutex drift");
    }
    if (meta.path == "cf-rehearing" && (!has("b15") || !has("b16") || has("b17") || has("b19") ||
                                        has("b20") || has("b21") || has("b22"))) {
        fail(meta.path + " ordinary-denial mutex drift");
    }
    if (meta.path == "cf-denial" && (!has("b15") || has("b16") || !has("b17") || !has("b18") ||
                                     !has("b19") || !has("b20") || has("b21") || has("b22"))) {
        fail(meta.path + " stay-denial mutex drift");
    }
    const bool grant = meta.path == "cf-grant-blocked" || meta.path == "cf-grant-complete";
    if (grant && (!has("b21") || !has("b22") || has("b15") || has("b16") || has("b17") ||
                  has("b18") || has("b19") || has("b20"))) {
        fail(meta.path + " rehearing-grant mutex drift");
    }
    for (unsigned value = 15; value <= 28; ++value) {
        if (value == 25U)
            continue;
        const auto code = std::string("b") + (value < 10 ? "0" : "") + std::to_string(value);
        const auto id = std::string(entry_prefix) + code;
        if (!documents.contains(id))
            continue;
        if (meta.path == "actual" ||
            !document_positions.contains(std::string(entry_prefix) + "b11") ||
            !document_positions.contains(std::string(entry_prefix) + "b12") ||
            document_positions.at(id) <= document_positions.at(std::string(entry_prefix) + "b11") ||
            document_positions.at(id) <= document_positions.at(std::string(entry_prefix) + "b12")) {
            fail(meta.path + " B15-B28 not exclusively downstream of B11/B12");
        }
    }
    if (meta.path == "cf-grant-complete") {
        const std::array<std::string, 7> chain{"b21", "b22", "b23", "b24", "b26", "b27", "b28"};
        for (std::size_t index = 1U; index < chain.size(); ++index) {
            if (document_positions.at(std::string(entry_prefix) + chain.at(index - 1U)) >=
                document_positions.at(std::string(entry_prefix) + chain.at(index))) {
                fail("complete grant path exact B21-B28 order drift");
            }
        }
    }
    const auto first =
        engine::replayWorkflow(runtime_case.workflow, runtime_case.definition, initial, journal);
    const auto second =
        engine::replayWorkflow(runtime_case.workflow, runtime_case.definition, initial, journal);
    if (!first || !second || *first != *second || *first != rolling ||
        first->current_stage_id.value != meta.terminal)
        fail(meta.path + " full replay nondeterministic/terminal drift");
    totals.full_replays += 2U;
    ++totals.terminals[meta.terminal];
    totals.commands += journal.size();
    totals.events += event_count;
    totals.tampers += auditTampers(meta, runtime_case, initial, journal);
    std::cout << "TRACE CLEAR " << meta.path << " commands=" << journal.size()
              << " events=" << event_count << " prefixes=" << journal.size() << " tampers=13\n";
}

[[nodiscard]] QJsonObject objectAt(const QHash<QString, QJsonObject>& objects, const QString& id,
                                   std::string_view label) {
    const auto found = objects.constFind(id);
    if (found == objects.constEnd()) {
        fail(std::string(label) + " is missing: " + id.toStdString());
    }
    return *found;
}

[[nodiscard]] QString firstPageText(const QDir& pack_root, const QJsonObject& entry) {
    const auto path = pack_root.filePath(entry.value(QStringLiteral("asset_path")).toString());
    QPdfDocument document;
    if (document.load(path) != QPdfDocument::Error::None ||
        document.status() != QPdfDocument::Status::Ready || document.pageCount() < 1) {
        fail("cannot inspect PDF text: " + path.toStdString());
    }
    return document.getAllText(0).text().simplified();
}

void auditStaticContent(const QDir& root) {
    const QDir pack_root(root.filePath(QStringLiteral("pack-candidate")));
    const auto manifest_bytes = readAll(pack_root.filePath(QStringLiteral("manifest.json")));
    const auto review_bytes =
        readAll(pack_root.filePath(QStringLiteral("resources/realism-review.json")));
    const auto record_bytes = readAll(pack_root.filePath(QStringLiteral("resources/record.json")));
    const auto workflow_bytes =
        readAll(pack_root.filePath(QStringLiteral("resources/workflow.json")));
    const auto plan_bytes =
        readAll(root.filePath(QStringLiteral("metadata/successor-appellate-plan.json")));
    if (sha256(manifest_bytes).toStdString() != ReleasePins::manifest_sha256 ||
        sha256(review_bytes).toStdString() != ReleasePins::realism_review_sha256 ||
        sha256(record_bytes).toStdString() != ReleasePins::record_sha256 ||
        sha256(workflow_bytes).toStdString() != ReleasePins::workflow_sha256 ||
        sha256(plan_bytes).toStdString() != ReleasePins::trace_plan_sha256) {
        fail("frozen manifest/review/record/workflow/trace-plan digest drift");
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
            QStringLiteral("us.ca4.m4.opengrid-foia") ||
        manifest.value(QStringLiteral("version")).toString() != QStringLiteral("1.2.0") ||
        contents.size() != 9 || blobs.size() != 84 || capabilities.size() != 17 ||
        dependencies.size() != 3) {
        fail("final 9-content/84-blob/17-capability/3-dependency envelope drift");
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
    if (actual_capabilities != expected_capabilities) {
        fail("exact Open Grid 17-capability set drift");
    }

    const QHash<QString, QString> expected_dependency_digests{
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
        if (!expected_dependency_digests.contains(id) || dependency_ids.contains(id) ||
            dependency.value(QStringLiteral("sha256")).toString() !=
                expected_dependency_digests.value(id)) {
            fail("dependency revision drift: " + id.toStdString());
        }
        dependency_ids.insert(id);
    }
    if (dependency_ids.size() != 3) {
        fail("dependency ID closure drift");
    }

    const QSet<QString> expected_content_ids{
        QStringLiteral("ca4m4.case.opengrid-foia"),
        QStringLiteral("ca4m4.opengrid.argument.actual-record"),
        QStringLiteral("ca4m4.opengrid.argument.corrected-counterfactual"),
        QStringLiteral("ca4m4.opengrid.authorities.case-specific"),
        QStringLiteral("ca4m4.opengrid.bench.three-judge"),
        QStringLiteral("ca4m4.opengrid.procedure.civil-foia-appeal"),
        QStringLiteral("ca4m4.opengrid.record"),
        QStringLiteral("ca4m4.opengrid.workflow.foia-sealed-appeal"),
        QStringLiteral("ca4m4.opengrid.review.authoring-2026-08-19"),
    };
    QSet<QString> content_ids;
    for (const auto& value : contents) {
        const auto descriptor = value.toObject();
        const auto relative_path = descriptor.value(QStringLiteral("path")).toString();
        const auto resource_bytes = readAll(pack_root.filePath(relative_path));
        const auto resource = parseObject(resource_bytes, relative_path);
        const auto id = descriptor.value(QStringLiteral("id")).toString();
        if (content_ids.contains(id) || !expected_content_ids.contains(id) ||
            descriptor.value(QStringLiteral("schema_version")).toInt() != 2 ||
            descriptor.value(QStringLiteral("sha256")).toString().toLatin1() !=
                sha256(resource_bytes) ||
            resource.value(QStringLiteral("resource_id")).toString() != id ||
            resource.value(QStringLiteral("resource_kind")).toString() !=
                descriptor.value(QStringLiteral("kind")).toString()) {
            fail("content descriptor drift: " + relative_path.toStdString());
        }
        content_ids.insert(id);
    }
    if (content_ids != expected_content_ids) {
        fail("exact nine-content ID set drift");
    }

    const auto evidence = review.value(QStringLiteral("evidence")).toObject();
    const auto dimension_evidence = evidence.value(QStringLiteral("dimension_evidence")).toObject();
    if (review.value(QStringLiteral("review_state")).toString() !=
            QStringLiteral("independent_review_pending") ||
        evidence.value(QStringLiteral("closure_digest")).toString().toStdString() !=
            ReleasePins::evidence_closure ||
        evidence.value(QStringLiteral("packs")).toArray().size() != 4 ||
        evidence.value(QStringLiteral("resources")).toArray().size() != 44 ||
        evidence.value(QStringLiteral("blobs")).toArray().size() != 84 ||
        evidence.value(QStringLiteral("traces")).toArray().size() != 5 ||
        evidence.value(QStringLiteral("record_checks")).toArray().size() != 2 ||
        evidence.value(QStringLiteral("authorities")).toArray().size() != 33) {
        fail("realism evidence 4/44/84/5/2/33 envelope drift");
    }
    QSet<QString> record_check_contracts;
    for (const auto& value : evidence.value(QStringLiteral("record_checks")).toArray()) {
        const auto check = value.toObject();
        record_check_contracts.insert(check.value(QStringLiteral("evidence_id")).toString() + u'|' +
                                      check.value(QStringLiteral("check_id")).toString() + u'|' +
                                      check.value(QStringLiteral("digest")).toString());
    }
    const QSet<QString> expected_record_check_contracts{
        QStringLiteral(
            "workbench.evidence.record-check."
            "83e5efb7fe3fd96befc0d427f196fba6fd8936f9ab3aa20160a7dcfdda3a499c|workbench.check."
            "asset-resolution.46cd95556d5f68f515b986caf12964e93d7f4803530ce6a4c5e8e5a0779eeddc|"
            "97036cd0e89b3d5e44862d4a199ecc5bf0186c205d325c6b8dbf8d3e3a35d965"),
        QStringLiteral(
            "workbench.evidence.record-check."
            "72e097a9c2755fd893941dd4d75c3b78098b019c72b5e5e028580ba9015de83b|workbench.check.page-"
            "anchor-resolution.341b639d59c1bce40c780c6d538501c7891dc7c7624297f703ad647db3d99826|"
            "46b6e37501731cdad512f67f3a598bb18ad29d8b8b4adf6e554ea4ec06b04e98"),
    };
    if (record_check_contracts != expected_record_check_contracts) {
        fail("exact asset/page-anchor authoring check identity or digest drift");
    }
    const QHash<QString, int> expected_dimension_counts{
        {QStringLiteral("procedural_law"), 46},     {QStringLiteral("deadlines_authority"), 20},
        {QStringLiteral("record_consistency"), 87}, {QStringLiteral("consequences"), 32},
        {QStringLiteral("oral_argument"), 18},      {QStringLiteral("bench_differentiation"), 4},
        {QStringLiteral("provenance"), 124},
    };
    QSet<QString> evidence_ids;
    for (const auto& group_name :
         {QStringLiteral("resources"), QStringLiteral("blobs"), QStringLiteral("traces"),
          QStringLiteral("record_checks"), QStringLiteral("authorities")}) {
        for (const auto& value : evidence.value(group_name).toArray()) {
            const auto id = value.toObject().value(QStringLiteral("evidence_id")).toString();
            if (id.isEmpty() || evidence_ids.contains(id)) {
                fail("empty or duplicate realism evidence ID");
            }
            evidence_ids.insert(id);
        }
    }
    if (evidence_ids.size() != 168 || dimension_evidence.size() != 7) {
        fail("168-ID/seven-dimension evidence closure drift");
    }
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
    int case_authorities = 0;
    int ca4_authorities = 0;
    int federal_authorities = 0;
    for (const auto& value : evidence.value(QStringLiteral("authorities")).toArray()) {
        const auto id = value.toObject().value(QStringLiteral("authority_id")).toString();
        case_authorities += id.startsWith(QStringLiteral("ca4m4.opengrid.")) ? 1 : 0;
        ca4_authorities += id.startsWith(QStringLiteral("us.ca4.")) ? 1 : 0;
        federal_authorities += id.startsWith(QStringLiteral("us.federal.")) ? 1 : 0;
    }
    if (case_authorities != 9 || ca4_authorities != 11 || federal_authorities != 13) {
        fail("33-authority 9+11+13 partition drift");
    }

    const auto entries = record.value(QStringLiteral("docket_entries")).toArray();
    const auto anchors = record.value(QStringLiteral("page_anchors")).toArray();
    const auto disclosures = record.value(QStringLiteral("sealed_disclosures")).toArray();
    if (record.value(QStringLiteral("dockets")).toArray().size() != 3 || entries.size() != 84 ||
        anchors.size() != 654 || disclosures.size() != 17 ||
        record.value(QStringLiteral("disclosure_policy")).toObject().isEmpty()) {
        fail("record three-docket/84-entry/654-anchor/17-disclosure envelope drift");
    }
    QHash<QString, QJsonObject> entries_by_id;
    QHash<QString, QJsonObject> entries_by_path;
    QHash<QString, int> docket_documents;
    QHash<QString, int> docket_pages;
    QHash<QString, int> public_docket_documents;
    int sealed_documents = 0;
    for (const auto& value : entries) {
        const auto entry = value.toObject();
        const auto id = entry.value(QStringLiteral("entry_id")).toString();
        const auto path = entry.value(QStringLiteral("asset_path")).toString();
        const auto docket = entry.value(QStringLiteral("docket_id")).toString();
        const auto tags = strings(entry.value(QStringLiteral("tags")).toArray());
        if (id.isEmpty() || entries_by_id.contains(id) || entries_by_path.contains(path)) {
            fail("record entry identity drift: " + id.toStdString());
        }
        const bool counterfactual =
            docket == QStringLiteral("ca4m4.opengrid.docket.counterfactual-branches");
        if (counterfactual != tags.contains(QStringLiteral("never_filed")) ||
            counterfactual != tags.contains(QStringLiteral("never_occurred_on_actual_docket"))) {
            fail("actual/counterfactual record isolation tag drift: " + id.toStdString());
        }
        entries_by_id.insert(id, entry);
        entries_by_path.insert(path, entry);
        ++docket_documents[docket];
        docket_pages[docket] += entry.value(QStringLiteral("page_count")).toInt();
        if (entry.value(QStringLiteral("sealed")).toBool())
            ++sealed_documents;
        else
            ++public_docket_documents[docket];
    }
    const auto lower_docket = QStringLiteral("ca4m4.opengrid.docket.district");
    const auto actual_docket = QStringLiteral("ca4m4.opengrid.docket.appellate");
    const auto counterfactual_docket =
        QStringLiteral("ca4m4.opengrid.docket.counterfactual-branches");
    if (docket_documents.value(lower_docket) != 37 || docket_pages.value(lower_docket) != 290 ||
        docket_documents.value(actual_docket) != 19 || docket_pages.value(actual_docket) != 155 ||
        docket_documents.value(counterfactual_docket) != 28 ||
        docket_pages.value(counterfactual_docket) != 209 || sealed_documents != 17 ||
        public_docket_documents.value(lower_docket) != 28 ||
        public_docket_documents.value(actual_docket) != 15 ||
        public_docket_documents.value(counterfactual_docket) != 24) {
        fail("record split is not exact 37/290 + 19/155 + 28/209 with public 28/15/24");
    }

    QHash<QString, int> anchors_by_entry;
    QHash<QString, QString> entry_by_anchor;
    QSet<QString> citation_labels;
    QHash<QString, int> anchor_dockets;
    for (const auto& value : anchors) {
        const auto anchor = value.toObject();
        const auto anchor_id = anchor.value(QStringLiteral("anchor_id")).toString();
        const auto entry_id = anchor.value(QStringLiteral("entry_id")).toString();
        const auto citation = anchor.value(QStringLiteral("citation_label")).toString();
        const auto entry = objectAt(entries_by_id, entry_id, "anchor record entry");
        if (anchor_id.isEmpty() || entry_by_anchor.contains(anchor_id) || citation.isEmpty() ||
            citation_labels.contains(citation) ||
            anchor.value(QStringLiteral("page_number")).toInt() < 1 ||
            anchor.value(QStringLiteral("page_number")).toInt() >
                entry.value(QStringLiteral("page_count")).toInt()) {
            fail("record anchor identity/page drift: " + anchor_id.toStdString());
        }
        entry_by_anchor.insert(anchor_id, entry_id);
        citation_labels.insert(citation);
        ++anchors_by_entry[entry_id];
        ++anchor_dockets[entry.value(QStringLiteral("docket_id")).toString()];
    }
    for (auto iterator = entries_by_id.constBegin(); iterator != entries_by_id.constEnd();
         ++iterator) {
        if (anchors_by_entry.value(iterator.key()) !=
            iterator.value().value(QStringLiteral("page_count")).toInt()) {
            fail("record entry/page-anchor count drift: " + iterator.key().toStdString());
        }
    }
    if (anchor_dockets.value(lower_docket) != 290 || anchor_dockets.value(actual_docket) != 155 ||
        anchor_dockets.value(counterfactual_docket) != 209) {
        fail("record anchor docket split drift");
    }

    QSet<QString> disclosed_sealed_entries;
    QSet<QString> stable_anchor_ids;
    QSet<QString> mapped_sealed_anchors;
    QSet<QString> mapped_public_anchors;
    int mapping_count = 0;
    int named_stable_anchors = 0;
    for (const auto& value : disclosures) {
        const auto disclosure = value.toObject();
        const auto sealed_id = disclosure.value(QStringLiteral("sealed_entry_id")).toString();
        const auto public_id = disclosure.value(QStringLiteral("public_entry_id")).toString();
        const auto sealed_entry = objectAt(entries_by_id, sealed_id, "sealed disclosure entry");
        const auto public_entry = objectAt(entries_by_id, public_id, "public disclosure entry");
        if (disclosed_sealed_entries.contains(sealed_id) ||
            !sealed_entry.value(QStringLiteral("sealed")).toBool() ||
            public_entry.value(QStringLiteral("sealed")).toBool() ||
            sealed_entry.value(QStringLiteral("page_count")).toInt() !=
                public_entry.value(QStringLiteral("page_count")).toInt()) {
            fail("sealed disclosure twin identity/page drift: " + sealed_id.toStdString());
        }
        disclosed_sealed_entries.insert(sealed_id);
        for (const auto& mapping_value :
             disclosure.value(QStringLiteral("anchor_mappings")).toArray()) {
            const auto mapping = mapping_value.toObject();
            const auto stable = mapping.value(QStringLiteral("stable_anchor_id")).toString();
            const auto sealed_anchor = mapping.value(QStringLiteral("sealed_anchor_id")).toString();
            const auto public_anchor = mapping.value(QStringLiteral("public_anchor_id")).toString();
            if (stable.isEmpty() || stable_anchor_ids.contains(stable) ||
                mapped_sealed_anchors.contains(sealed_anchor) ||
                mapped_public_anchors.contains(public_anchor) ||
                entry_by_anchor.value(sealed_anchor) != sealed_id ||
                entry_by_anchor.value(public_anchor) != public_id) {
                fail("sealed stable-anchor mapping drift: " + stable.toStdString());
            }
            stable_anchor_ids.insert(stable);
            mapped_sealed_anchors.insert(sealed_anchor);
            mapped_public_anchors.insert(public_anchor);
            entry_by_anchor.insert(stable, public_id);
            ++mapping_count;
            named_stable_anchors +=
                stable.contains(QRegularExpression(QStringLiteral("\\.page-[0-9]{2}$"))) ? 0 : 1;
        }
    }
    if (disclosed_sealed_entries.size() != 17 || stable_anchor_ids.size() != 172 ||
        mapped_sealed_anchors.size() != 172 || mapped_public_anchors.size() != 172 ||
        mapping_count != 172 || named_stable_anchors != 22) {
        fail("sealed record 17-disclosure/172-mapping/22-named-anchor closure drift");
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
            fail("manifest PDF/page count cannot be loaded: " + path.toStdString());
        }
        manifest_blob_paths.insert(path);
    }
    if (manifest_blob_paths.size() != 84 || entries_by_path.size() != 84) {
        fail("84-PDF manifest/record path closure drift");
    }

    const auto l33 =
        objectAt(entries_by_id, QStringLiteral("ca4m4.opengrid.record.entry.l33"), "L33");
    const auto a17 =
        objectAt(entries_by_id, QStringLiteral("ca4m4.opengrid.record.entry.a17"), "A17");
    const auto b01 =
        objectAt(entries_by_id, QStringLiteral("ca4m4.opengrid.record.entry.b01"), "B01");
    const auto l33_text = firstPageText(pack_root, l33);
    const auto a17_text = firstPageText(pack_root, a17);
    const auto b01_text = firstPageText(pack_root, b01);
    if (!l33_text.contains(
            QStringLiteral("accepts DOE’s categorical Exemption 7(F) presentation")) ||
        !l33_text.contains(QStringLiteral("maintains the controlled counterparts")) ||
        !a17_text.contains(QStringLiteral("affirm that limited ruling")) ||
        !a17_text.contains(QStringLiteral("vacate those three rulings and remand")) ||
        !b01_text.contains(QStringLiteral(
            "operational-decision-tree showing under Exemption 7(E) remains unchanged")) ||
        !b01_text.contains(QStringLiteral("four specified corrections")) ||
        !strings(b01.value(QStringLiteral("tags")).toArray())
             .contains(QStringLiteral("never_filed"))) {
        fail("actual partial-vacatur versus isolated corrected-record premise drift");
    }

    const auto argument_entries = [&](const QJsonObject& configuration) {
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
                const auto anchor_id = grounding.value(QStringLiteral("anchor_id")).toString();
                if (!entry_by_anchor.contains(anchor_id)) {
                    fail("argument grounding anchor is absent: " + anchor_id.toStdString());
                }
                result.insert(entry_by_anchor.value(anchor_id));
            }
        }
        return result;
    };
    const auto actual_entries = argument_entries(actual_argument);
    const auto counterfactual_entries = argument_entries(counterfactual_argument);
    if (actual_argument.value(QStringLiteral("resource_id")).toString() !=
            QStringLiteral("ca4m4.opengrid.argument.actual-record") ||
        actual_argument.value(QStringLiteral("grounded_question_bank"))
                .toObject()
                .value(QStringLiteral("mode"))
                .toString() != QStringLiteral("actual_record") ||
        actual_argument.value(QStringLiteral("grounded_question_bank"))
                .toObject()
                .value(QStringLiteral("grounding_digest"))
                .toString() !=
            QStringLiteral("bdbfefea95255ef6d323ce0baebf27a5bb0f93f73484cc68df1e6e5e59b1c23b") ||
        actual_argument.value(QStringLiteral("grounded_question_bank"))
                .toObject()
                .value(QStringLiteral("questions"))
                .toArray()
                .size() != 12 ||
        std::ranges::any_of(
            actual_entries,
            [](const auto& id) { return id.contains(QStringLiteral(".entry.b")); }) ||
        !actual_entries.contains(QStringLiteral("ca4m4.opengrid.record.entry.l32")) ||
        !actual_entries.contains(QStringLiteral("ca4m4.opengrid.record.entry.a17")) ||
        counterfactual_argument.value(QStringLiteral("resource_id")).toString() !=
            QStringLiteral("ca4m4.opengrid.argument.corrected-counterfactual") ||
        counterfactual_argument.value(QStringLiteral("grounded_question_bank"))
                .toObject()
                .value(QStringLiteral("mode"))
                .toString() != QStringLiteral("counterfactual_training") ||
        counterfactual_argument.value(QStringLiteral("grounded_question_bank"))
                .toObject()
                .value(QStringLiteral("grounding_digest"))
                .toString() !=
            QStringLiteral("68f4d32cb47237aea128c33d638cafd3529a8cc1a807d43dd970b4b70d49b3f8") ||
        counterfactual_argument.value(QStringLiteral("grounded_question_bank"))
                .toObject()
                .value(QStringLiteral("questions"))
                .toArray()
                .size() != 12 ||
        std::ranges::any_of(
            counterfactual_entries,
            [](const auto& id) { return id.contains(QStringLiteral(".entry.a")); }) ||
        !counterfactual_entries.contains(QStringLiteral("ca4m4.opengrid.record.entry.b01")) ||
        !counterfactual_entries.contains(QStringLiteral("ca4m4.opengrid.record.entry.b11")) ||
        !counterfactual_entries.contains(QStringLiteral("ca4m4.opengrid.record.entry.b03"))) {
        fail("actual/counterfactual argument-grounding isolation drift");
    }

    const auto disposition_plans =
        case_definition.value(QStringLiteral("disposition_plans")).toArray();
    QHash<QString, QJsonObject> plans_by_id;
    for (const auto& value : disposition_plans) {
        const auto plan = value.toObject();
        plans_by_id.insert(plan.value(QStringLiteral("plan_id")).toString(), plan);
    }
    const auto actual_plan = plans_by_id.value(
        QStringLiteral("ca4m4.opengrid.disposition.actual-partial-vacatur-remand"));
    const auto counterfactual_plan = plans_by_id.value(
        QStringLiteral("ca4m4.opengrid.disposition.counterfactual-full-affirmance"));
    const auto action_sequence = [](const QJsonObject& plan) {
        QStringList result;
        for (const auto& value : plan.value(QStringLiteral("components")).toArray())
            result.push_back(value.toObject().value(QStringLiteral("action")).toString());
        return result;
    };
    if (case_definition.value(QStringLiteral("resource_id")).toString() !=
            QStringLiteral("ca4m4.case.opengrid-foia") ||
        case_definition.value(QStringLiteral("actors")).toArray().size() != 5 ||
        case_definition.value(QStringLiteral("issues")).toArray().size() != 4 ||
        disposition_plans.size() != 2 || plans_by_id.size() != 2 ||
        case_definition.value(QStringLiteral("authored_disposition_plan_id")).toString() !=
            QStringLiteral("ca4m4.opengrid.disposition.actual-partial-vacatur-remand") ||
        case_definition.value(QStringLiteral("authored_disposition_id")).toString() !=
            QStringLiteral("ca4m4.opengrid.operation.issue-actual-judgment") ||
        actual_plan.value(QStringLiteral("digest")).toString() !=
            QStringLiteral("523c7bc9574fd76f87c4b67955d7366a30a9cc6c30847c9fbffbf6df87d6b8d0") ||
        counterfactual_plan.value(QStringLiteral("digest")).toString() !=
            QStringLiteral("6309ed33d1d12a0dd64c88b1be3951368fc9e15a6586b065d7886e60bb7428e6") ||
        action_sequence(actual_plan) !=
            QStringList{QStringLiteral("affirm"), QStringLiteral("vacate"),
                        QStringLiteral("vacate"), QStringLiteral("vacate")} ||
        action_sequence(counterfactual_plan) !=
            QStringList{QStringLiteral("affirm"), QStringLiteral("affirm"),
                        QStringLiteral("affirm"), QStringLiteral("affirm")}) {
        fail("Open Grid five-actor/four-issue/two-disposition case shape drift");
    }

    const auto stages = workflow.value(QStringLiteral("stages")).toArray();
    const auto operations = workflow.value(QStringLiteral("operations")).toArray();
    const auto routes = workflow.value(QStringLiteral("filing_routes")).toArray();
    if (workflow.value(QStringLiteral("resource_id")).toString() !=
            QStringLiteral("ca4m4.opengrid.workflow.foia-sealed-appeal") ||
        stages.size() != 35 || operations.size() != 101 || routes.size() != 17) {
        fail("workflow 35-stage/101-operation/17-route envelope drift");
    }
    QSet<QString> stage_ids;
    for (const auto& value : stages) {
        stage_ids.insert(value.toString());
    }
    QHash<QString, QJsonObject> operations_by_id;
    QSet<QString> court_entries;
    QSet<QString> bound_entries;
    QSet<QString> produced_deadlines;
    int document_bindings = 0;
    int disposition_bindings = 0;
    int deadline_operations = 0;
    for (const auto& value : operations) {
        const auto operation = value.toObject();
        const auto id = operation.value(QStringLiteral("operation_id")).toString();
        if (id.isEmpty() || operations_by_id.contains(id) ||
            !stage_ids.contains(operation.value(QStringLiteral("stage_id")).toString()) ||
            operation.value(QStringLiteral("preconditions")).toArray().isEmpty() ||
            operation.value(QStringLiteral("allowed_legal_times")).toArray().isEmpty()) {
            fail("unguarded or invalid workflow operation: " + id.toStdString());
        }
        if (operation.value(QStringLiteral("opcode")).toString() ==
            QStringLiteral("calculate_deadline")) {
            ++deadline_operations;
            const auto deadline_id =
                operation.value(QStringLiteral("produced_deadline_id")).toString();
            if (deadline_id.isEmpty() || produced_deadlines.contains(deadline_id)) {
                fail("empty or duplicate produced deadline: " + id.toStdString());
            }
            produced_deadlines.insert(deadline_id);
        }
        if (operation.contains(QStringLiteral("document_binding"))) {
            ++document_bindings;
            const auto binding = operation.value(QStringLiteral("document_binding")).toObject();
            const auto entry_id = binding.value(QStringLiteral("record_entry_id")).toString();
            const auto entry = objectAt(entries_by_id, entry_id, "operation document binding");
            if (binding.value(QStringLiteral("document_sha256")).toString() !=
                entry.value(QStringLiteral("asset_sha256")).toString()) {
                fail("operation binding digest drift: " + id.toStdString());
            }
            court_entries.insert(entry_id);
            bound_entries.insert(entry_id);
        }
        disposition_bindings += operation.contains(QStringLiteral("disposition_plan_id")) ? 1 : 0;
        operations_by_id.insert(id, operation);
    }

    QSet<QString> filing_ids;
    QSet<QString> filing_entries;
    int filing_bindings = 0;
    for (const auto& value : routes) {
        const auto route = value.toObject();
        if (!operations_by_id.contains(
                route.value(QStringLiteral("accept_operation_id")).toString()) ||
            !operations_by_id.contains(
                route.value(QStringLiteral("reject_operation_id")).toString())) {
            fail("filing route operation reference drift");
        }
        for (const auto& binding_value : route.value(QStringLiteral("filing_bindings")).toArray()) {
            const auto binding = binding_value.toObject();
            const auto filing_id = binding.value(QStringLiteral("filing_id")).toString();
            const auto entry_id = binding.value(QStringLiteral("record_entry_id")).toString();
            const auto entry = objectAt(entries_by_id, entry_id, "route filing binding");
            if (filing_id.isEmpty() || filing_ids.contains(filing_id) ||
                binding.value(QStringLiteral("document_sha256")).toString() !=
                    entry.value(QStringLiteral("asset_sha256")).toString()) {
                fail("filing binding identity/digest drift: " + filing_id.toStdString());
            }
            filing_ids.insert(filing_id);
            filing_entries.insert(entry_id);
            bound_entries.insert(entry_id);
            ++filing_bindings;
        }
    }
    const QSet<QString> expected_filing_ids{
        QStringLiteral("ca4m4.opengrid.filing.l36-notice-of-appeal"),
        QStringLiteral("ca4m4.opengrid.filing.a02-docketing-statement"),
        QStringLiteral("ca4m4.opengrid.filing.a03-appearances-disclosures"),
        QStringLiteral("ca4m4.opengrid.filing.a04-motion-limited-sealing"),
        QStringLiteral("ca4m4.opengrid.filing.a05-certificate-confidentiality"),
        QStringLiteral("ca4m4.opengrid.filing.a06-public-opening-brief"),
        QStringLiteral("ca4m4.opengrid.filing.a08-public-response-brief"),
        QStringLiteral("ca4m4.opengrid.filing.a10-public-reply-brief"),
        QStringLiteral("ca4m4.opengrid.filing.a12-public-joint-appendix"),
        QStringLiteral("ca4m4.opengrid.filing.b02-counterfactual-motion-limited-sealing"),
        QStringLiteral("ca4m4.opengrid.filing.b03-counterfactual-certificate-confidentiality"),
        QStringLiteral("ca4m4.opengrid.filing.b04-counterfactual-public-opening-brief"),
        QStringLiteral("ca4m4.opengrid.filing.b06-counterfactual-public-response-brief"),
        QStringLiteral("ca4m4.opengrid.filing.b08-counterfactual-public-reply-brief"),
        QStringLiteral("ca4m4.opengrid.filing.b13-counterfactual-rehearing-petition"),
        QStringLiteral("ca4m4.opengrid.filing.b14-counterfactual-rehearing-response"),
        QStringLiteral("ca4m4.opengrid.filing.b17-counterfactual-mandate-stay-motion"),
        QStringLiteral("ca4m4.opengrid.filing.b18-counterfactual-mandate-stay-opposition"),
        QStringLiteral("ca4m4.opengrid.filing.b24-counterfactual-public-supplemental-brief"),
    };
    if (stage_ids.size() != 35 || operations_by_id.size() != 101 || deadline_operations != 12 ||
        produced_deadlines.size() != 12 || document_bindings != 22 || court_entries.size() != 22 ||
        disposition_bindings != 2 || filing_bindings != 19 || filing_ids != expected_filing_ids ||
        filing_entries.size() != 19 || bound_entries.size() != 41 ||
        std::ranges::any_of(bound_entries, [&](const auto& entry_id) {
            return objectAt(entries_by_id, entry_id, "workflow-bound entry")
                .value(QStringLiteral("sealed"))
                .toBool();
        })) {
        fail("workflow 35/101/17/19/22/2/12/41 exact public topology drift");
    }

    const auto b16_operation = operations_by_id.value(
        QStringLiteral("ca4m4.opengrid.operation.issue-counterfactual-post-denial-mandate"));
    const auto b28_operation = operations_by_id.value(
        QStringLiteral("ca4m4.opengrid.operation.issue-counterfactual-post-rehearing-mandate"));
    const auto has_exact_precondition = [](const QJsonObject& operation, QStringView kind,
                                           QStringView field, QStringView expected) {
        return std::ranges::any_of(
            operation.value(QStringLiteral("preconditions")).toArray(), [&](const auto& value) {
                const auto condition = value.toObject();
                return condition.value(QStringLiteral("kind")).toString() == kind &&
                       condition.value(field).toString() == expected;
            });
    };
    const bool b16_excludes_stay = std::ranges::any_of(
        b16_operation.value(QStringLiteral("preconditions")).toArray(), [](const auto& value) {
            const auto condition = value.toObject();
            return condition.value(QStringLiteral("kind")).toString() ==
                       QStringLiteral("filing_instance") &&
                   condition.value(QStringLiteral("filing_id")).toString() ==
                       QStringLiteral(
                           "ca4m4.opengrid.filing.b17-counterfactual-mandate-stay-motion") &&
                   !condition.value(QStringLiteral("present")).toBool(true);
        });
    if (!b16_excludes_stay ||
        !has_exact_precondition(b28_operation, u"order_instance", u"order_id",
                                u"ca4m4.opengrid.order.counterfactual-revised-judgment") ||
        !has_exact_precondition(
            b28_operation, u"deadline_status", u"deadline_id",
            u"ca4m4.opengrid.deadline.counterfactual-revised-rehearing-petition") ||
        !has_exact_precondition(b28_operation, u"deadline_status", u"deadline_id",
                                u"ca4m4.opengrid.deadline.counterfactual-revised-mandate")) {
        fail("B16 exclusion or exact B27/B28 revised-mandate guard drift");
    }

    QCryptographicHash trace_closure(QCryptographicHash::Sha256);
    QHash<QString, QJsonObject> authored_traces;
    for (const auto& value : evidence.value(QStringLiteral("traces")).toArray()) {
        const auto trace = value.toObject();
        authored_traces.insert(trace.value(QStringLiteral("trace_id")).toString(), trace);
    }
    QStringList relative_trace_paths;
    for (const auto& meta : metas) {
        relative_trace_paths.push_back(QStringLiteral("traces/%1").arg(meta.file));
    }
    for (const auto& relative_path : relative_trace_paths) {
        const auto bytes = readAll(root.filePath(relative_path));
        const auto path_bytes = relative_path.toUtf8();
        trace_closure.addData(QByteArrayView(path_bytes));
        trace_closure.addData(QByteArrayView("\0", 1));
        trace_closure.addData(QByteArrayView(bytes));
        trace_closure.addData(QByteArrayView("\0", 1));
        const auto trace = parseObject(bytes, relative_path);
        const auto authored =
            authored_traces.value(trace.value(QStringLiteral("trace_id")).toString());
        if (authored.isEmpty() || authored != trace) {
            fail("authored realism trace differs from frozen trace: " +
                 relative_path.toStdString());
        }
    }
    if (trace_closure.result().toHex().toStdString() != ReleasePins::trace_closure ||
        authored_traces.size() != 5) {
        fail("five-trace framed closure or authored evidence binding drift");
    }
}

[[nodiscard]] QString asQString(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

[[nodiscard]] bool expectsPdfRejection(ui::RecordWorkspace& workspace,
                                       const QString& stable_anchor) {
    const auto opened = workspace.navigateToAnchor(stable_anchor);
    return !opened && opened.error().code == ui::RecordWorkspaceErrorCode::PdfLoadFailed &&
           workspace.currentDocumentId().isEmpty();
}

void auditInstalledSealedRecord(const QDir& pack_root, const QDir& foundations,
                                const QDir& temporary_root, packs::PackCatalog& catalog,
                                const packs::ResolvedPack& resolved,
                                const packs::LoadedPack& loaded, const packs::RuntimePack& runtime,
                                std::span<const model::PackRevision> dependencies) {
    if (runtime.cases.size() != 1U)
        fail("sealed runtime requires exactly one case");
    const auto& runtime_case = runtime.cases.front();
    const auto& record = runtime_case.record;
    const auto mapping_count = std::ranges::fold_left(
        record.sealed_disclosures, std::size_t{0}, [](std::size_t total, const auto& disclosure) {
            return total + disclosure.anchor_mappings.size();
        });
    if (record.id.value != "ca4m4.opengrid.record" || record.dockets.size() != 3U ||
        record.docket_entries.size() != 84U || record.page_anchors.size() != 654U ||
        !record.disclosure_policy || record.sealed_disclosures.size() != 17U ||
        mapping_count != 172U) {
        fail("installed sealed runtime record envelope drift");
    }

    ui::RecordWorkspace workspace;
    app::InstalledRecordController installed_controller(catalog, workspace);
    const auto installed = installed_controller.load(resolved, runtime, runtime_case.definition.id);
    if (!installed) {
        fail("installed exact record load failed: " + installed.error().message.toStdString());
    }
    const auto& full_definition = ui::RecordWorkspaceTestAccess::fullDefinition(workspace);
    if (installed->definition.documents.size() != 67U ||
        installed->definition.docket.size() != 67U || installed->assets.size() != 67U ||
        workspace.visibleDocketCount() != 67 || full_definition.documents.size() != 84U ||
        full_definition.sealed_disclosures.size() != 17U) {
        fail("installed public/full projection is not exact 67/84/17");
    }

    const auto& first = record.sealed_disclosures.front();
    if (!first.public_entry_id || first.anchor_mappings.empty())
        fail("first sealed disclosure lacks a public stable-anchor twin");
    const auto sealed_id = asQString(first.sealed_entry_id.value);
    const auto public_id = asQString(first.public_entry_id->value);
    const auto stable_anchor = asQString(first.anchor_mappings.front().stable_anchor_id.value);
    const auto sealed_runtime =
        std::ranges::find(record.docket_entries, first.sealed_entry_id,
                          [](const packs::RuntimeDocketEntry& entry) { return entry.id; });
    if (sealed_runtime == record.docket_entries.end())
        fail("first sealed disclosure entry is absent from runtime");
    const auto public_open = workspace.navigateToAnchor(stable_anchor);
    if (!public_open || workspace.currentDocumentId() != public_id ||
        workspace.openDocketEntry(sealed_id)) {
        fail("unauthorized stable anchor/public projection exposed a sealed twin");
    }

    const auto session_path =
        temporary_root.filePath(QStringLiteral("opengrid-record-access.sqlite"));
    auto store = storage::SessionStore::open(session_path);
    if (!store)
        fail("record access session store cannot open");
    auto access = app::RecordAccessSessionController::create(
        QStringLiteral("integration.opengrid.record-access"), runtime_case.definition.id,
        std::move(*store), QStringLiteral("engine.record-access.v1"),
        QStringLiteral("2026-08-19T03:00:00Z"), resolved);
    if (!access) {
        fail("record access session cannot bind exact closure: " +
             access.error().message.toStdString());
    }
    auto statuses = (*access)->disclosures();
    if (statuses.size() != 17U || std::ranges::any_of(statuses, [](const auto& status) {
            return status.authorized || !status.blocking_deficiencies.empty();
        })) {
        fail("initial 17-disclosure policy is not eligible and unauthorized");
    }
    if (!(*access)->applyCurrentProjection(workspace))
        fail("initial public access projection cannot apply");

    const auto disclosure_id = first.disclosure_id.value;
    if (!(*access)->grant(disclosure_id, "integration.opengrid.grant.1",
                          QStringLiteral("2026-08-19T03:00:01Z")) ||
        !(*access)->applyCurrentProjection(workspace) || workspace.visibleDocketCount() != 68) {
        fail("exact-authority grant did not expose exactly one sealed twin");
    }
    statuses = (*access)->disclosures();
    if (std::ranges::count(statuses, true, &model::RecordAccessDisclosureStatus::authorized) != 1) {
        fail("grant authorized more or fewer than one disclosure");
    }
    const auto sealed_open = workspace.navigateToAnchor(stable_anchor);
    if (!sealed_open || workspace.currentDocumentId() != sealed_id ||
        workspace.loadedPageCount() != static_cast<int>(sealed_runtime->page_count)) {
        fail("authorized stable anchor did not open the verified sealed twin");
    }
    const auto prefix_zero = (*access)->auditProjectionAt(0);
    const auto prefix_one = (*access)->auditProjectionAt(1);
    if (!prefix_zero || !prefix_zero->authorizedDisclosureIds().empty() || !prefix_one ||
        prefix_one->authorizedDisclosureIds() != std::vector<std::string>{disclosure_id}) {
        fail("grant prefix replay differs from the exact access journal");
    }

    if (!(*access)->revoke(disclosure_id, "integration.opengrid.revoke.1",
                           QStringLiteral("2026-08-19T03:00:02Z")) ||
        !(*access)->applyCurrentProjection(workspace) || workspace.visibleDocketCount() != 67 ||
        !workspace.currentDocumentId().isEmpty()) {
        fail("revocation did not close the sealed UI and restore public 67");
    }
    const auto revoked_open = workspace.navigateToAnchor(stable_anchor);
    const auto prefix_two = (*access)->auditProjectionAt(2);
    if (!revoked_open || workspace.currentDocumentId() != public_id ||
        workspace.openDocketEntry(sealed_id) || !prefix_two ||
        !prefix_two->authorizedDisclosureIds().empty()) {
        fail("revocation did not route the stable anchor back to the public twin");
    }

    // The same persisted journal must reject a closure with the same pack ID and version but a
    // different exact root digest. Only the review summary is varied in this temporary hostile
    // root, leaving the production case and record identities unchanged.
    (*access).reset();
    const QDir cross_root(temporary_root.filePath(QStringLiteral("same-identity-root")));
    if (!copyDirectory(pack_root.path(), cross_root.path()))
        fail("cannot copy exact pack for same-identity hostile root");
    const auto cross_review_path =
        cross_root.filePath(QStringLiteral("resources/realism-review.json"));
    auto cross_review = parseObject(readAll(cross_review_path), cross_review_path);
    auto uncertainties = cross_review.value(QStringLiteral("known_uncertainty")).toArray();
    if (uncertainties.isEmpty())
        fail("hostile cross-root review has no uncertainty to vary");
    auto uncertainty = uncertainties.at(0).toObject();
    uncertainty.insert(QStringLiteral("summary"),
                       uncertainty.value(QStringLiteral("summary")).toString() +
                           QStringLiteral(" Exact-closure hostile variant."));
    uncertainties[0] = uncertainty;
    cross_review.insert(QStringLiteral("known_uncertainty"), uncertainties);
    const auto cross_review_bytes = QJsonDocument(cross_review).toJson(QJsonDocument::Indented);
    if (!writeAll(cross_review_path, cross_review_bytes))
        fail("cannot write same-identity hostile review");
    const auto cross_manifest_path = cross_root.filePath(QStringLiteral("manifest.json"));
    auto cross_manifest = parseObject(readAll(cross_manifest_path), cross_manifest_path);
    auto cross_contents = cross_manifest.value(QStringLiteral("contents")).toArray();
    bool review_descriptor_changed = false;
    for (qsizetype index = 0; index < cross_contents.size(); ++index) {
        auto descriptor = cross_contents.at(index).toObject();
        if (descriptor.value(QStringLiteral("path")).toString() !=
            QStringLiteral("resources/realism-review.json")) {
            continue;
        }
        descriptor.insert(QStringLiteral("sha256"),
                          QString::fromLatin1(sha256(cross_review_bytes)));
        cross_contents[index] = descriptor;
        review_descriptor_changed = true;
    }
    cross_manifest.insert(QStringLiteral("contents"), cross_contents);
    if (!review_descriptor_changed ||
        !writeAll(cross_manifest_path,
                  QJsonDocument(cross_manifest).toJson(QJsonDocument::Indented))) {
        fail("cannot update same-identity hostile manifest");
    }
    const auto cross_archive = temporary_root.filePath(QStringLiteral("same-identity-root.awpack"));
    const auto cross_revision = packs::PackArchive::exportDirectory(
        cross_root.path(), cross_archive, {}, packs::PackValidationScope::ResolvedClosure);
    if (!cross_revision || cross_revision->id != loaded.revision.id ||
        cross_revision->version != loaded.revision.version ||
        cross_revision->digest == loaded.revision.digest) {
        fail("same-ID/version hostile export did not produce a distinct exact root");
    }
    const auto cross_catalog =
        packs::PackCatalog::open(temporary_root.filePath(QStringLiteral("same-identity-catalog")));
    if (!cross_catalog)
        fail("same-identity hostile catalog cannot open");
    const auto cross_federal =
        (*cross_catalog)
            ->installArchive(foundations.filePath(QStringLiteral(
                                 "us-federal/foundation-us-federal-2025.12.01.awpack")),
                             QStringLiteral("2026-08-19T03:00:10Z"));
    const auto cross_ca4 =
        (*cross_catalog)
            ->installArchive(
                foundations.filePath(QStringLiteral("us-ca4/foundation-us-ca4-2026.03.23.awpack")),
                QStringLiteral("2026-08-19T03:00:11Z"));
    const auto cross_bench =
        (*cross_catalog)
            ->installArchive(
                foundations.filePath(QStringLiteral(
                    "us-ca4-fictional-bench/foundation-us-ca4-fictional-bench-1.0.0.awpack")),
                QStringLiteral("2026-08-19T03:00:12Z"));
    const auto cross_installed =
        (*cross_catalog)->installArchive(cross_archive, QStringLiteral("2026-08-19T03:00:13Z"));
    if (dependencies.size() != 3U || !cross_federal || !cross_ca4 || !cross_bench ||
        !cross_installed || cross_federal->revision != dependencies[0] ||
        cross_ca4->revision != dependencies[1] || cross_bench->revision != dependencies[2] ||
        cross_installed->revision != *cross_revision) {
        fail("same-identity hostile exact four-pack install drift");
    }
    const auto cross_resolved = (*cross_catalog)->loadResolved(*cross_revision);
    if (!cross_resolved || cross_resolved->revisionsByPackId().size() != 4U)
        fail("same-identity hostile exact closure cannot resolve");
    auto cross_store = storage::SessionStore::open(session_path);
    if (!cross_store)
        fail("same-identity hostile session store cannot reopen");
    const auto cross_reopen = app::RecordAccessSessionController::reopen(
        QStringLiteral("integration.opengrid.record-access"), runtime_case.definition.id,
        std::move(*cross_store), QStringLiteral("engine.record-access.v1"), *cross_resolved);
    if (cross_reopen ||
        cross_reopen.error().code != app::SessionControllerErrorCode::CorruptSession) {
        fail("same-ID/version different-root closure reopened the access journal");
    }

    auto baseline_store = storage::SessionStore::open(session_path);
    if (!baseline_store)
        fail("baseline access journal cannot reopen");
    access = app::RecordAccessSessionController::reopen(
        QStringLiteral("integration.opengrid.record-access"), runtime_case.definition.id,
        std::move(*baseline_store), QStringLiteral("engine.record-access.v1"), resolved);
    if (!access || (*access)->snapshot().sequence != 2 ||
        std::ranges::any_of((*access)->disclosures(),
                            &model::RecordAccessDisclosureStatus::authorized)) {
        fail("baseline exact closure did not replay the revoked journal");
    }
    if (!(*access)->grant(disclosure_id, "integration.opengrid.grant.2",
                          QStringLiteral("2026-08-19T03:00:20Z")) ||
        !(*access)->applyCurrentProjection(workspace)) {
        fail("second exact-authority grant failed");
    }

    const auto descriptor =
        std::ranges::find(loaded.blobs, sealed_runtime->asset_path,
                          [](const model::BlobDescriptor& blob) { return blob.path; });
    if (descriptor == loaded.blobs.end())
        fail("sealed asset descriptor is absent from exact root");
    const auto object_path =
        QDir(catalog.blobObjectsDirectory()).filePath(asQString(sealed_runtime->asset_sha256));
    const auto original = readAll(object_path);
    if (original.isEmpty() || original.size() != static_cast<qint64>(descriptor->byte_size))
        fail("sealed CAS object cannot be pinned before per-open mutations");
    const auto original_permissions = QFileInfo(object_path).permissions();
    const auto installed_list = catalog.list();
    if (!installed_list || installed_list->size() != 4U)
        fail("baseline catalog inventory is not exact four revisions");
    const auto installed_root =
        std::ranges::find(*installed_list, loaded.revision,
                          [](const packs::InstalledPack& candidate) { return candidate.revision; });
    if (installed_root == installed_list->end())
        fail("baseline root is absent from catalog inventory");
    const auto archive_path =
        QDir(catalog.archivesDirectory())
            .filePath(installed_root->archive_sha256 + QStringLiteral(".awpack"));
    const auto held_archive = archive_path + QStringLiteral(".held-for-integration-test");
    if (!QFile::rename(archive_path, held_archive))
        fail("cannot hold source archive for per-open CAS mutation checks");
    const auto restore_archive = [&]() {
        return QFileInfo::exists(archive_path) ||
               (QFileInfo::exists(held_archive) && QFile::rename(held_archive, archive_path));
    };
    const auto restore_object = [&]() {
        if (QFileInfo(object_path).isSymLink())
            QFile::remove(object_path);
        const auto restored = writeAll(object_path, original);
        QFile::setPermissions(object_path, original_permissions);
        return restored;
    };
    const auto held_object = object_path + QStringLiteral(".held-for-integration-test");
    if (!QFile::rename(object_path, held_object) ||
        !expectsPdfRejection(workspace, stable_anchor) ||
        !QFile::rename(held_object, object_path)) {
        restore_object();
        restore_archive();
        fail("per-open missing CAS path was not rejected");
    }
    if (!QFile::rename(object_path, held_object) || !QFile::link(held_object, object_path) ||
        !expectsPdfRejection(workspace, stable_anchor) || !QFile::remove(object_path) ||
        !QFile::rename(held_object, object_path)) {
        restore_object();
        restore_archive();
        fail("per-open symlink CAS path was not rejected");
    }
    if (!writeAll(object_path, original.first(original.size() - 1)) ||
        !expectsPdfRejection(workspace, stable_anchor) || !restore_object()) {
        restore_object();
        restore_archive();
        fail("per-open CAS size mutation was not rejected");
    }
    auto digest_mutation = original;
    digest_mutation[digest_mutation.size() / 2] =
        digest_mutation.at(digest_mutation.size() / 2) == 'x' ? 'y' : 'x';
    if (!writeAll(object_path, digest_mutation) || !expectsPdfRejection(workspace, stable_anchor) ||
        !restore_object()) {
        restore_object();
        restore_archive();
        fail("per-open same-size CAS digest mutation was not rejected");
    }
    if (!restore_archive()) {
        restore_object();
        fail("source archive could not be restored after CAS mutation checks");
    }
    const auto recovered = workspace.navigateToAnchor(stable_anchor);
    if (!recovered || workspace.currentDocumentId() != sealed_id ||
        workspace.loadedPageCount() != static_cast<int>(sealed_runtime->page_count)) {
        fail("exact CAS recovery did not reopen the sealed twin");
    }

    digest_mutation = original;
    digest_mutation[digest_mutation.size() / 3] =
        digest_mutation.at(digest_mutation.size() / 3) == 'm' ? 'n' : 'm';
    if (!writeAll(object_path, digest_mutation) || workspace.currentDocumentId() != sealed_id ||
        workspace.loadedPageCount() != static_cast<int>(sealed_runtime->page_count) ||
        !workspace.goToPage(static_cast<int>(sealed_runtime->page_count) - 1) ||
        !restore_object()) {
        restore_object();
        fail("leased sealed snapshot was not isolated from later CAS mutation");
    }

    // Wrap the production installed resolver to corrupt only its private verified snapshot. This
    // reaches the post-CAS PDF readability check without weakening the descriptor/digest checks.
    auto unreadable_definition = ui::RecordWorkspaceTestAccess::fullDefinition(workspace);
    const auto unreadable_document =
        std::ranges::find(unreadable_definition.documents, sealed_id, &ui::RecordDocument::id);
    if (unreadable_document == unreadable_definition.documents.end() ||
        !unreadable_document->deferred_asset) {
        fail("sealed installed resolver is absent for readability check");
    }
    const auto production_resolver = unreadable_document->deferred_asset;
    unreadable_document->deferred_asset =
        [production_resolver]() -> std::expected<ui::RecordAssetLease, QString> {
        auto lease = production_resolver();
        if (!lease)
            return std::unexpected(lease.error());
        if (!writeAll(lease->file_path, QByteArray("not-a-readable-pdf")))
            return std::unexpected(QStringLiteral("cannot corrupt verified PDF snapshot"));
        return *lease;
    };
    ui::RecordWorkspace unreadable_workspace;
    if (!unreadable_workspace.setRecord(std::move(unreadable_definition)) ||
        !(*access)->applyCurrentProjection(unreadable_workspace) ||
        !expectsPdfRejection(unreadable_workspace, stable_anchor)) {
        fail("descriptor-verified unreadable sealed PDF was not rejected");
    }

    auto wrong_pages_definition = ui::RecordWorkspaceTestAccess::fullDefinition(workspace);
    const auto wrong_pages_document =
        std::ranges::find(wrong_pages_definition.documents, sealed_id, &ui::RecordDocument::id);
    if (wrong_pages_document == wrong_pages_definition.documents.end())
        fail("sealed installed resolver is absent for page-count check");
    ++wrong_pages_document->declared_page_count;
    ui::RecordWorkspace wrong_pages_workspace;
    if (!wrong_pages_workspace.setRecord(std::move(wrong_pages_definition)) ||
        !(*access)->applyCurrentProjection(wrong_pages_workspace) ||
        !expectsPdfRejection(wrong_pages_workspace, stable_anchor)) {
        fail("descriptor-verified sealed PDF page-count mismatch was not rejected");
    }

    if (!(*access)->revoke(disclosure_id, "integration.opengrid.revoke.2",
                           QStringLiteral("2026-08-19T03:00:21Z")) ||
        !(*access)->applyCurrentProjection(workspace) || workspace.visibleDocketCount() != 67 ||
        !workspace.currentDocumentId().isEmpty() || (*access)->snapshot().sequence != 4) {
        fail("final revoke did not restore the exact public projection at sequence four");
    }

    std::cout << "SEALED RECORD CLEAR public=67 grant=68 revoke=67 disclosures=17 mappings=172 "
                 "same-id-different-root=REJECTED cas-path/symlink/size/digest=REJECTED "
                 "pdf-readability/page-count=REJECTED snapshot/recovery=VERIFIED\n";
}

[[nodiscard]] std::set<std::string> exactFilingIds() {
    return {
        "ca4m4.opengrid.filing.l36-notice-of-appeal",
        "ca4m4.opengrid.filing.a02-docketing-statement",
        "ca4m4.opengrid.filing.a03-appearances-disclosures",
        "ca4m4.opengrid.filing.a04-motion-limited-sealing",
        "ca4m4.opengrid.filing.a05-certificate-confidentiality",
        "ca4m4.opengrid.filing.a06-public-opening-brief",
        "ca4m4.opengrid.filing.a08-public-response-brief",
        "ca4m4.opengrid.filing.a10-public-reply-brief",
        "ca4m4.opengrid.filing.a12-public-joint-appendix",
        "ca4m4.opengrid.filing.b02-counterfactual-motion-limited-sealing",
        "ca4m4.opengrid.filing.b03-counterfactual-certificate-confidentiality",
        "ca4m4.opengrid.filing.b04-counterfactual-public-opening-brief",
        "ca4m4.opengrid.filing.b06-counterfactual-public-response-brief",
        "ca4m4.opengrid.filing.b08-counterfactual-public-reply-brief",
        "ca4m4.opengrid.filing.b13-counterfactual-rehearing-petition",
        "ca4m4.opengrid.filing.b14-counterfactual-rehearing-response",
        "ca4m4.opengrid.filing.b17-counterfactual-mandate-stay-motion",
        "ca4m4.opengrid.filing.b18-counterfactual-mandate-stay-opposition",
        "ca4m4.opengrid.filing.b24-counterfactual-public-supplemental-brief",
    };
}

} // namespace

int main(int argc, char** argv) try {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    const QDir content_root(QStringLiteral(APPELLATE_M4_OPENGRID_ROOT));
    const QDir foundations(QStringLiteral(APPELLATE_M4_FOUNDATIONS));
    auditStaticContent(content_root);

    const model::PackRevision root{model::PackId{"us.ca4.m4.opengrid-foia"}, "1.2.0",
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
        loaded->resources.size() != 9U || loaded->blobs.size() != 84U ||
        loaded->required_capabilities.size() != 17U || loaded->dependencies.size() != 3U) {
        fail("final deferred root cannot load with exact 9/84/17/3 envelope");
    }

    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        fail("cannot create integration temporary directory");
    }
    const QDir temporary_root(temporary.path());
    const auto archive_a = temporary_root.filePath(QStringLiteral("opengrid-a.awpack"));
    const auto archive_b = temporary_root.filePath(QStringLiteral("opengrid-b.awpack"));
    const auto exported_a = packs::PackArchive::exportDirectory(
        pack_root.path(), archive_a, {}, packs::PackValidationScope::ResolvedClosure);
    const auto exported_b = packs::PackArchive::exportDirectory(
        pack_root.path(), archive_b, {}, packs::PackValidationScope::ResolvedClosure);
    const auto archive_bytes = readAll(archive_a);
    if (!exported_a || !exported_b || *exported_a != root || *exported_b != root ||
        archive_bytes != readAll(archive_b) ||
        static_cast<std::uint64_t>(archive_bytes.size()) != ReleasePins::archive_byte_size ||
        sha256(archive_bytes).toStdString() != ReleasePins::archive_sha256) {
        fail("double export root/archive digest, size, or determinism drift");
    }

    const auto catalog =
        packs::PackCatalog::open(temporary_root.filePath(QStringLiteral("catalog")));
    if (!catalog) {
        fail("cannot open integration catalog: " + catalog.error().message.toStdString());
    }
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
    if (!listed || listed->size() != 4U) {
        fail("fresh catalog does not contain exactly four revisions");
    }
    const auto resolved = (*catalog)->loadResolved(root);
    if (!resolved || resolved->revisionsByPackId().size() != 4U) {
        fail("exact four-revision root cannot resolve");
    }
    for (const auto& blob : loaded->blobs) {
        const auto materialized = (*catalog)->materializeBlob(*resolved, root, blob.path);
        if (!materialized || materialized->descriptor != blob ||
            sha256(readAll(materialized->local_path)).toStdString() != blob.sha256) {
            fail("installed blob materialization drift: " + blob.path);
        }
    }
    const auto runtime = packs::loadRuntimePack(*resolved);
    if (!runtime || runtime->revision != root || runtime->cases.size() != 1U) {
        fail("exact root runtime cannot load");
    }
    const auto& runtime_case = runtime->cases.front();
    if (runtime_case.definition.id.value != "ca4m4.case.opengrid-foia" ||
        runtime_case.workflow.id.value != "ca4m4.opengrid.workflow.foia-sealed-appeal" ||
        runtime_case.workflow.stages.size() != 35U ||
        runtime_case.workflow.operations.size() != 101U ||
        runtime_case.workflow.filing_routes.size() != 17U ||
        runtime_case.record.docket_entries.size() != 84U ||
        runtime_case.record.page_anchors.size() != 654U ||
        runtime_case.record.sealed_disclosures.size() != 17U ||
        runtime_case.argument_configurations.size() != 2U ||
        runtime_case.definition.disposition_plans.size() != 2U) {
        fail("exact root/runtime shape drift");
    }
    std::set<std::string> disposition_ids;
    for (const auto& disposition : runtime_case.definition.disposition_plans) {
        disposition_ids.insert(disposition.id.value);
    }
    if (disposition_ids !=
        std::set<std::string>{"ca4m4.opengrid.disposition.actual-partial-vacatur-remand",
                              "ca4m4.opengrid.disposition.counterfactual-full-affirmance"}) {
        fail("exact two-disposition identity drift");
    }
    auditInstalledSealedRecord(pack_root, foundations, temporary_root, **catalog, *resolved,
                               *loaded, *runtime, dependencies);

    const QDir trace_dir(content_root.filePath(QStringLiteral("traces")));
    QStringList expected_files;
    for (const auto& meta : metas)
        expected_files.push_back(meta.file);
    expected_files.sort();
    if (trace_dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name) != expected_files)
        fail("trace file inventory drift");
    Totals totals;
    for (const auto& meta : metas)
        auditTrace(trace_dir, meta, runtime_case, totals);
    std::set<std::string> defined_operations;
    for (const auto& operation : runtime_case.workflow.operations)
        defined_operations.insert(operation.id.value);
    std::set<std::string> defined_filings;
    std::set<std::string> recovery_filings;
    for (const auto& route : runtime_case.workflow.filing_routes)
        for (std::size_t index = 0U; index < route.filing_bindings.size(); ++index) {
            defined_filings.insert(route.filing_bindings.at(index).filing_id.value);
            if (index == 0U)
                recovery_filings.insert(route.filing_bindings.at(index).filing_id.value);
        }
    if (defined_filings != exactFilingIds()) {
        fail("runtime exact 19-filing ID set drift");
    }
    const std::map<std::string, std::string> expected_deadlines{
        {"ca4m4.opengrid.deadline.actual-opening-brief", "2026-03-13"},
        {"ca4m4.opengrid.deadline.actual-response-brief", "2026-04-13"},
        {"ca4m4.opengrid.deadline.actual-reply-brief", "2026-05-04"},
        {"ca4m4.opengrid.deadline.actual-rehearing-petition", "2026-07-31"},
        {"ca4m4.opengrid.deadline.actual-mandate", "2026-08-07"},
        {"ca4m4.opengrid.deadline.counterfactual-initial-rehearing-petition", "2026-07-13"},
        {"ca4m4.opengrid.deadline.counterfactual-initial-ordinary-mandate", "2026-07-20"},
        {"ca4m4.opengrid.deadline.counterfactual-post-rehearing-denial-mandate", "2026-06-29"},
        {"ca4m4.opengrid.deadline.counterfactual-post-stay-denial-mandate", "2026-06-30"},
        {"ca4m4.opengrid.deadline.counterfactual-supplemental-brief", "2026-07-02"},
        {"ca4m4.opengrid.deadline.counterfactual-revised-rehearing-petition", "2026-07-27"},
        {"ca4m4.opengrid.deadline.counterfactual-revised-mandate", "2026-08-03"},
    };
    if (totals.commands != 221U || totals.events != 221U || totals.prefix_replays != 221U ||
        totals.full_replays != 10U || totals.operations != defined_operations ||
        totals.operations.size() != 101U || totals.accepted != defined_filings ||
        totals.rejected != recovery_filings || defined_filings.size() != 19U ||
        recovery_filings.size() != 17U || totals.rejections != 39U ||
        totals.court_documents.size() != 22U || totals.deadlines != expected_deadlines ||
        totals.tampers != 65U || totals.terminals["ca4m4.opengrid.stage.terminated"] != 4U ||
        totals.terminals["ca4m4.opengrid.stage.counterfactual-mandate-stayed"] != 1U) {
        fail("corpus closure mismatch");
    }
    std::cout << "INDEPENDENT TRACE AUDIT CLEAR paths=5 commands=221 events=221 prefix-replays=221 "
                 "full-replays=10 operations=101 filings=19 recovered-routes=17 rejections=39 "
                 "court-docs=22 deadlines=12 endpoints=4+1 tampers=65\n";
    std::cout << "Open Grid FOIA integration passed: 9 contents / 84 PDFs / 654 anchors / "
                 "17 sealed disclosures / 172 mappings / 22 named anchors; 35 stages / 101 "
                 "operations / 17 routes / 41 public bound documents; 5 traces / 221 commands / "
                 "221 events.\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Open Grid FOIA integration failed: " << error.what() << '\n';
    return 1;
}
