#include "appellate/engine/workflow_engine.hpp"
#include "appellate/packs/pack_archive.hpp"
#include "appellate/packs/pack_catalog.hpp"
#include "appellate/packs/runtime_pack.hpp"
#include "appellate/storage/workflow_codec.hpp"

#include <QByteArrayView>
#include <QCoreApplication>
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

#ifndef APPELLATE_M4_BLUEEMBER_ROOT
#error "APPELLATE_M4_BLUEEMBER_ROOT must name content/m4/blueember-jmol"
#endif

#ifndef APPELLATE_M4_FOUNDATIONS
#error "APPELLATE_M4_FOUNDATIONS must name content/foundations"
#endif

namespace model = appellate::model;
namespace engine = appellate::engine;
namespace packs = appellate::packs;
namespace storage = appellate::storage;
using namespace std::chrono_literals;

namespace {

namespace ReleasePins {
constexpr std::string_view manifest_sha256 =
    "2b545cee1aaba7a1475b2f5085ae93d50ec9e3255a68f9762d3bec63492a8dac";
constexpr std::string_view realism_review_sha256 =
    "8fe8d9b06f38ca16fe535c917c3da4b2c6d92a5ee2f17924d49a945d2e5e0688";
constexpr std::string_view evidence_closure =
    "3f38cd1a12f7f61c037f338fef4f1600ab83434208aa13a8ff5cf56a52fe5d5a";
constexpr std::string_view root_revision =
    "08d88e4811e8ed8ad6e642cc041365508808f7158862aa93199de867f31431ec";
constexpr std::string_view archive_sha256 =
    "c6332ae33e351ccb27ed17b5576b147a47f9f5f0b44583365212b1781a288ed2";
constexpr std::uint64_t archive_byte_size = 5'326'158;
constexpr std::string_view record_sha256 =
    "080ff7772d73131a5471f2fc530b4d63c6215831a82ffcd671ef50beff8d1c7a";
constexpr std::string_view workflow_sha256 =
    "7c2356718286505eee16d62b48ca281f92eee367c9e21319ddcae02d87c1a120";
constexpr std::string_view trace_plan_sha256 =
    "664b8632be87d885cebc4625282f0b452c5d376be607bc889ee86012c3ddcee5";
constexpr std::string_view trace_closure =
    "b6ba5a4be1ac19c672b121f1e0e48a5a16c5b17e94792721e82261500e2adefb";
} // namespace ReleasePins

constexpr std::string_view entry_prefix = "ca4m4.blueember.record.entry.";

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
    addFrame(hash, QStringLiteral("ca4m4.case.blueember-jmol"));
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

const std::array<Meta, 6> metas{
    Meta{QStringLiteral("actual-ordinary-through-mandate.json"), "actual",
         "ca4m4.blueember.trace.actual-ordinary", "ca4m4.blueember.evidence.trace.actual-ordinary",
         41, "ca4m4.blueember.stage.terminated",
         "6ecfa835b9d2a353242756a37bebb1c21e146fcedce6c4d11c0c384c2a1a873b",
         "a2d24a6420e5beef36c307eff9dadb57f7aa16c94e871d314b1de335b0a58217",
         "29ed2c7b846025c844ec38c142ffb48051db764451c81917bdcbd293026dc0dd"},
    Meta{QStringLiteral("counterfactual-ordinary-through-mandate.json"), "cf-ordinary",
         "ca4m4.blueember.trace.counterfactual-ordinary",
         "ca4m4.blueember.evidence.trace.counterfactual-ordinary", 39,
         "ca4m4.blueember.stage.terminated",
         "634a808ac338ac861db77436cda5d1b27bcaad5e023bc425d0a83d79f9d8109b",
         "28d627d08d55d5ab9cd07f16faeaea27684a76e89f7050e63ada785b0133e009",
         "9dc9c4950a96a79111c5bc7c8d72c88c864503cc9d2521870a6e1ef3effd0d44"},
    Meta{QStringLiteral("counterfactual-rehearing-denial-through-mandate.json"), "cf-rehearing",
         "ca4m4.blueember.trace.counterfactual-rehearing",
         "ca4m4.blueember.evidence.trace.counterfactual-rehearing", 45,
         "ca4m4.blueember.stage.terminated",
         "ac367c6e75993770a63707a76bf6f8080a808ee5caeaaf4d18e43e47defa9ef3",
         "5a6f74c632483281b1f780c142ef1d590d570ab719a6c9efee991a4b12966698",
         "d8b9b1c3ac01ab5b76c404b07c564c89ca6dca5fe3d56bb44a4a4446c6d882da"},
    Meta{QStringLiteral("mandate-stay-denial-through-mandate.json"), "cf-denial",
         "ca4m4.blueember.trace.stay-denial", "ca4m4.blueember.evidence.trace.stay-denial", 48,
         "ca4m4.blueember.stage.terminated",
         "052a46d036c9f238ed51dedf99d610c8ecf78955997b206725ae7e10aec4cb92",
         "863ea4c651372361dea1f6beaff8c45986e772269c5268deee3d58b009b29364",
         "770d258e2e470f0cfac8a4e5e6773e7aefbf9823e2f564345ee8c6972ee77cef"},
    Meta{QStringLiteral("mandate-stay-grant-blocked.json"), "cf-grant-blocked",
         "ca4m4.blueember.trace.stay-grant-blocked",
         "ca4m4.blueember.evidence.trace.stay-grant-blocked", 46,
         "ca4m4.blueember.stage.counterfactual-mandate-stayed",
         "ae6eac9c6ba3fad96eb103bc37c0baa119e7d34f9f64cd13fd713bf2ded61557",
         "1d977e402e60a3b11655bbba995be52ddf55a8cc99e82ec142056b0ba39d4b29",
         "ae812cde4928365c0a769cef686f7542b6d402cdb874d652ebe203ddcd8bc516"},
    Meta{QStringLiteral("mandate-stay-grant-dissolution-through-mandate.json"), "cf-dissolution",
         "ca4m4.blueember.trace.stay-grant-dissolution",
         "ca4m4.blueember.evidence.trace.stay-grant-dissolution", 51,
         "ca4m4.blueember.stage.terminated",
         "ab0430468c0c521e43d261014d33f85234cad8f37b04c56ce172cba633c87ea3",
         "a9e0d95b2e5b1fbe3baab91b3b4bd77127d8ba17f6707c5277645f5451e1d280",
         "d758c5e13d280da81cefa467ec2b34e497731f902ac99a22aa8bd71bfbce4c39"},
};

[[nodiscard]] std::set<std::string> expectedEntries(std::string_view path) {
    std::set<std::string> codes{"l41", "l42", "a01", "a02", "a03", "a04", "a05"};
    const auto add = [&](char prefix, unsigned first, unsigned last) {
        for (unsigned value = first; value <= last; ++value) {
            codes.insert(std::string(1, prefix) + (value < 10 ? "0" : "") + std::to_string(value));
        }
    };
    if (path == "actual") {
        add('a', 6, 16);
    } else {
        add('b', 1, 8);
        if (path == "cf-ordinary")
            add('b', 9, 10);
        else if (path == "cf-rehearing")
            add('b', 11, 14);
        else if (path == "cf-denial")
            add('b', 15, 19);
        else if (path == "cf-grant-blocked")
            add('b', 20, 22);
        else if (path == "cf-dissolution")
            add('b', 20, 25);
        else
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

void auditTampers(const Meta& meta, const packs::RuntimeCase& runtime_case,
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
                order->order_id = model::WorkflowOrderId{"ca4m4.blueember.order.hostile-tamper"};
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
                        model::WorkflowDeadlineId{"ca4m4.blueember.deadline.hostile-tamper"};
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
                bool changed = false;
                std::visit(
                    [&](auto& base) {
                        if constexpr (requires { base.operation_id; }) {
                            base.operation_id = model::WorkflowOperationId{
                                "ca4m4.blueember.operation.hostile-tamper"};
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
                            model::WorkflowDeadlineId{"ca4m4.blueember.deadline.hostile-base"};
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
                                                    "ca4m4.blueember.record.entry.hostile-tamper";
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
            QStringLiteral("ca4m4.blueember.workflow.post-trial-jmol") ||
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
        if (!has("a06") || !has("a11") || has("b01") || has("b06"))
            fail("actual A/CF mutex drift");
    } else {
        if (!has("b01") || !has("b06") || has("a06") || has("a11"))
            fail(meta.path + " A/CF mutex drift");
    }
    const bool denial = meta.path == "cf-denial";
    const bool grant = meta.path == "cf-grant-blocked" || meta.path == "cf-dissolution";
    if ((denial &&
         (!has("b15") || !has("b16") || !has("b17") || has("b20") || has("b21") || has("b22"))) ||
        (grant &&
         (!has("b20") || !has("b21") || !has("b22") || has("b15") || has("b16") || has("b17"))))
        fail(meta.path + " denial/grant mutex drift");
    for (unsigned value = 15; value <= 25; ++value) {
        const auto code = std::string("b") + (value < 10 ? "0" : "") + std::to_string(value);
        const auto id = std::string(entry_prefix) + code;
        if (!documents.contains(id))
            continue;
        if (meta.path == "actual" ||
            !document_positions.contains(std::string(entry_prefix) + "b07") ||
            !document_positions.contains(std::string(entry_prefix) + "b08") ||
            document_positions.at(id) <= document_positions.at(std::string(entry_prefix) + "b07") ||
            document_positions.at(id) <= document_positions.at(std::string(entry_prefix) + "b08")) {
            fail(meta.path + " B15-B25 not exclusively downstream of B07/B08");
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
    auditTampers(meta, runtime_case, initial, journal);
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
            QStringLiteral("us.ca4.m4.blueember-jmol") ||
        manifest.value(QStringLiteral("version")).toString() != QStringLiteral("1.2.0") ||
        contents.size() != 9 || blobs.size() != 83 || capabilities.size() != 16 ||
        dependencies.size() != 3) {
        fail("final 9-content/83-blob/16-capability/3-dependency envelope drift");
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
        fail("exact Ellison-compatible 16-capability set drift");
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
        QStringLiteral("ca4m4.case.blueember-jmol"),
        QStringLiteral("ca4m4.blueember.argument.actual-record"),
        QStringLiteral("ca4m4.blueember.argument.preserved-counterfactual"),
        QStringLiteral("ca4m4.blueember.authorities.case-specific"),
        QStringLiteral("ca4m4.blueember.bench.three-judge"),
        QStringLiteral("ca4m4.blueember.procedure.civil-post-trial-appeal"),
        QStringLiteral("ca4m4.blueember.record"),
        QStringLiteral("ca4m4.blueember.workflow.post-trial-jmol"),
        QStringLiteral("ca4m4.blueember.review.authoring-2026-08-19"),
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
        evidence.value(QStringLiteral("blobs")).toArray().size() != 83 ||
        evidence.value(QStringLiteral("traces")).toArray().size() != 6 ||
        evidence.value(QStringLiteral("record_checks")).toArray().size() != 2 ||
        evidence.value(QStringLiteral("authorities")).toArray().size() != 39) {
        fail("realism evidence 4/44/83/6/2/39 envelope drift");
    }
    const QHash<QString, int> expected_dimension_counts{
        {QStringLiteral("procedural_law"), 53},     {QStringLiteral("deadlines_authority"), 19},
        {QStringLiteral("record_consistency"), 86}, {QStringLiteral("consequences"), 37},
        {QStringLiteral("oral_argument"), 20},      {QStringLiteral("bench_differentiation"), 4},
        {QStringLiteral("provenance"), 129},
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
    if (evidence_ids.size() != 174 || dimension_evidence.size() != 7) {
        fail("174-ID/seven-dimension evidence closure drift");
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
        case_authorities += id.startsWith(QStringLiteral("ca4m4.blueember.")) ? 1 : 0;
        ca4_authorities += id.startsWith(QStringLiteral("us.ca4.")) ? 1 : 0;
        federal_authorities += id.startsWith(QStringLiteral("us.federal.")) ? 1 : 0;
    }
    if (case_authorities != 15 || ca4_authorities != 10 || federal_authorities != 14) {
        fail("39-authority 15+10+14 partition drift");
    }

    const auto entries = record.value(QStringLiteral("docket_entries")).toArray();
    const auto anchors = record.value(QStringLiteral("page_anchors")).toArray();
    if (record.value(QStringLiteral("dockets")).toArray().size() != 3 || entries.size() != 83 ||
        anchors.size() != 656) {
        fail("record three-docket/83-entry/656-anchor envelope drift");
    }
    QHash<QString, QJsonObject> entries_by_id;
    QHash<QString, QJsonObject> entries_by_path;
    QHash<QString, int> docket_documents;
    QHash<QString, int> docket_pages;
    for (const auto& value : entries) {
        const auto entry = value.toObject();
        const auto id = entry.value(QStringLiteral("entry_id")).toString();
        const auto path = entry.value(QStringLiteral("asset_path")).toString();
        const auto docket = entry.value(QStringLiteral("docket_id")).toString();
        const auto tags = strings(entry.value(QStringLiteral("tags")).toArray());
        if (id.isEmpty() || entries_by_id.contains(id) || entries_by_path.contains(path) ||
            entry.value(QStringLiteral("sealed")).toBool(true)) {
            fail("record entry identity/sealing drift: " + id.toStdString());
        }
        const bool counterfactual =
            docket == QStringLiteral("ca4m4.blueember.docket.counterfactual-branches");
        if (counterfactual != tags.contains(QStringLiteral("never_filed")) ||
            counterfactual != tags.contains(QStringLiteral("never_occurred_on_actual_docket"))) {
            fail("actual/counterfactual record isolation tag drift: " + id.toStdString());
        }
        entries_by_id.insert(id, entry);
        entries_by_path.insert(path, entry);
        ++docket_documents[docket];
        docket_pages[docket] += entry.value(QStringLiteral("page_count")).toInt();
    }
    const auto lower_docket = QStringLiteral("ca4m4.blueember.docket.district");
    const auto actual_docket = QStringLiteral("ca4m4.blueember.docket.appellate");
    const auto counterfactual_docket =
        QStringLiteral("ca4m4.blueember.docket.counterfactual-branches");
    if (docket_documents.value(lower_docket) != 42 || docket_pages.value(lower_docket) != 430 ||
        docket_documents.value(actual_docket) != 16 || docket_pages.value(actual_docket) != 108 ||
        docket_documents.value(counterfactual_docket) != 25 ||
        docket_pages.value(counterfactual_docket) != 118) {
        fail("record split is not exact 42/430 + 16/108 + 25/118");
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
    if (anchor_dockets.value(lower_docket) != 430 || anchor_dockets.value(actual_docket) != 108 ||
        anchor_dockets.value(counterfactual_docket) != 118) {
        fail("record anchor docket split drift");
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
    if (manifest_blob_paths.size() != 83 || entries_by_path.size() != 83) {
        fail("83-PDF manifest/record path closure drift");
    }

    const auto l32 =
        objectAt(entries_by_id, QStringLiteral("ca4m4.blueember.record.entry.l32"), "L32");
    const auto l33 =
        objectAt(entries_by_id, QStringLiteral("ca4m4.blueember.record.entry.l33"), "L33");
    const auto b01 =
        objectAt(entries_by_id, QStringLiteral("ca4m4.blueember.record.entry.b01"), "B01");
    const auto l32_text = firstPageText(pack_root, l32);
    const auto l33_text = firstPageText(pack_root, l33);
    const auto b01_text = firstPageText(pack_root, b01);
    if (!l32_text.contains(
            QStringLiteral("moves for judgment as a matter of law on causation only")) ||
        !l32_text.contains(QStringLiteral(
            "does not seek judgment on the affirmative defense of failure to mitigate")) ||
        !l33_text.contains(QStringLiteral(
            "renews its request for judgment as a matter of law on causation only")) ||
        !b01_text.contains(
            QStringLiteral("not only on causation but also expressly on failure to mitigate")) ||
        !strings(b01.value(QStringLiteral("tags")).toArray())
             .contains(QStringLiteral("never_filed"))) {
        fail("actual Rule 50 causation-only versus isolated B01 premise drift");
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
            QStringLiteral("ca4m4.blueember.argument.actual-record") ||
        actual_argument.value(QStringLiteral("grounded_question_bank"))
                .toObject()
                .value(QStringLiteral("mode"))
                .toString() != QStringLiteral("actual_record") ||
        actual_argument.value(QStringLiteral("grounded_question_bank"))
                .toObject()
                .value(QStringLiteral("questions"))
                .toArray()
                .size() != 12 ||
        std::ranges::any_of(
            actual_entries,
            [](const auto& id) { return id.contains(QStringLiteral(".entry.b")); }) ||
        !actual_entries.contains(QStringLiteral("ca4m4.blueember.record.entry.l32")) ||
        !actual_entries.contains(QStringLiteral("ca4m4.blueember.record.entry.l33")) ||
        counterfactual_argument.value(QStringLiteral("resource_id")).toString() !=
            QStringLiteral("ca4m4.blueember.argument.preserved-counterfactual") ||
        counterfactual_argument.value(QStringLiteral("grounded_question_bank"))
                .toObject()
                .value(QStringLiteral("mode"))
                .toString() != QStringLiteral("counterfactual_training") ||
        counterfactual_argument.value(QStringLiteral("grounded_question_bank"))
                .toObject()
                .value(QStringLiteral("questions"))
                .toArray()
                .size() != 12 ||
        std::ranges::any_of(
            counterfactual_entries,
            [](const auto& id) { return id.contains(QStringLiteral(".entry.a")); }) ||
        !counterfactual_entries.contains(QStringLiteral("ca4m4.blueember.record.entry.b01")) ||
        !counterfactual_entries.contains(QStringLiteral("ca4m4.blueember.record.entry.b07")) ||
        !counterfactual_entries.contains(QStringLiteral("ca4m4.blueember.record.entry.b08"))) {
        fail("actual/counterfactual argument-grounding isolation drift");
    }

    if (case_definition.value(QStringLiteral("actors")).toArray().size() != 5 ||
        case_definition.value(QStringLiteral("issues")).toArray().size() != 4 ||
        case_definition.value(QStringLiteral("disposition_plans")).toArray().size() != 2 ||
        case_definition.value(QStringLiteral("authored_disposition_plan_id")).toString() !=
            QStringLiteral("ca4m4.blueember.disposition.actual-reversal-new-trial") ||
        case_definition.value(QStringLiteral("authored_disposition_id")).toString() !=
            QStringLiteral("ca4m4.blueember.operation.issue-actual-judgment")) {
        fail("Blue Ember five-actor/four-issue/two-disposition case shape drift");
    }

    const auto stages = workflow.value(QStringLiteral("stages")).toArray();
    const auto operations = workflow.value(QStringLiteral("operations")).toArray();
    const auto routes = workflow.value(QStringLiteral("filing_routes")).toArray();
    if (workflow.value(QStringLiteral("resource_id")).toString() !=
            QStringLiteral("ca4m4.blueember.workflow.post-trial-jmol") ||
        stages.size() != 24 || operations.size() != 93 || routes.size() != 15) {
        fail("workflow 24-stage/93-operation/15-route envelope drift");
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
        QStringLiteral("ca4m4.blueember.filing.l41-notice-of-appeal"),
        QStringLiteral("ca4m4.blueember.filing.a02-blue-ember-appearance"),
        QStringLiteral("ca4m4.blueember.filing.a03-granite-heron-appearance"),
        QStringLiteral("ca4m4.blueember.filing.a04-blue-ember-docketing-statement"),
        QStringLiteral("ca4m4.blueember.filing.a05-transcript-order-record-designation"),
        QStringLiteral("ca4m4.blueember.filing.a07-blue-ember-opening-brief"),
        QStringLiteral("ca4m4.blueember.filing.a08-joint-appendix"),
        QStringLiteral("ca4m4.blueember.filing.a09-granite-heron-response-brief"),
        QStringLiteral("ca4m4.blueember.filing.a10-blue-ember-reply-brief"),
        QStringLiteral("ca4m4.blueember.filing.b02-blue-ember-opening-brief"),
        QStringLiteral("ca4m4.blueember.filing.b03-joint-appendix"),
        QStringLiteral("ca4m4.blueember.filing.b04-granite-heron-response-brief"),
        QStringLiteral("ca4m4.blueember.filing.b05-blue-ember-reply-brief"),
        QStringLiteral("ca4m4.blueember.filing.b11-rehearing-petition"),
        QStringLiteral("ca4m4.blueember.filing.b15-stay-motion-denial"),
        QStringLiteral("ca4m4.blueember.filing.b16-stay-response-denial"),
        QStringLiteral("ca4m4.blueember.filing.b20-stay-motion-grant"),
        QStringLiteral("ca4m4.blueember.filing.b21-stay-response-grant"),
    };
    if (stage_ids.size() != 24 || operations_by_id.size() != 93 || deadline_operations != 14 ||
        produced_deadlines.size() != 14 || document_bindings != 25 || court_entries.size() != 25 ||
        disposition_bindings != 2 || filing_bindings != 18 || filing_ids != expected_filing_ids ||
        filing_entries.size() != 18 || bound_entries.size() != 43) {
        fail("workflow 24/93/15/18/25/2/14/43 exact topology drift");
    }

    const auto has_filing_guard = [&](QStringView filing_id, QStringView opposite_id,
                                      bool present) {
        for (const auto& route_value : routes) {
            for (const auto& binding_value :
                 route_value.toObject().value(QStringLiteral("filing_bindings")).toArray()) {
                const auto binding = binding_value.toObject();
                if (binding.value(QStringLiteral("filing_id")).toString() != filing_id) {
                    continue;
                }
                return std::ranges::any_of(
                    binding.value(QStringLiteral("preconditions")).toArray(),
                    [&](const auto& guard_value) {
                        const auto guard = guard_value.toObject();
                        return guard.value(QStringLiteral("kind")).toString() ==
                                   QStringLiteral("filing_instance") &&
                               guard.value(QStringLiteral("filing_id")).toString() == opposite_id &&
                               guard.value(QStringLiteral("present")).toBool() == present;
                    });
            }
        }
        return false;
    };
    if (!has_filing_guard(u"ca4m4.blueember.filing.b15-stay-motion-denial",
                          u"ca4m4.blueember.filing.b20-stay-motion-grant", false) ||
        !has_filing_guard(u"ca4m4.blueember.filing.b20-stay-motion-grant",
                          u"ca4m4.blueember.filing.b15-stay-motion-denial", false) ||
        !has_filing_guard(u"ca4m4.blueember.filing.b16-stay-response-denial",
                          u"ca4m4.blueember.filing.b21-stay-response-grant", false) ||
        !has_filing_guard(u"ca4m4.blueember.filing.b21-stay-response-grant",
                          u"ca4m4.blueember.filing.b16-stay-response-denial", false)) {
        fail("B15/B20 or B16/B21 bidirectional filing mutex drift");
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
    relative_trace_paths.sort();
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
        authored_traces.size() != 6) {
        fail("six-trace framed closure or authored evidence binding drift");
    }
}

[[nodiscard]] std::set<std::string> exactFilingIds() {
    return {
        "ca4m4.blueember.filing.l41-notice-of-appeal",
        "ca4m4.blueember.filing.a02-blue-ember-appearance",
        "ca4m4.blueember.filing.a03-granite-heron-appearance",
        "ca4m4.blueember.filing.a04-blue-ember-docketing-statement",
        "ca4m4.blueember.filing.a05-transcript-order-record-designation",
        "ca4m4.blueember.filing.a07-blue-ember-opening-brief",
        "ca4m4.blueember.filing.a08-joint-appendix",
        "ca4m4.blueember.filing.a09-granite-heron-response-brief",
        "ca4m4.blueember.filing.a10-blue-ember-reply-brief",
        "ca4m4.blueember.filing.b02-blue-ember-opening-brief",
        "ca4m4.blueember.filing.b03-joint-appendix",
        "ca4m4.blueember.filing.b04-granite-heron-response-brief",
        "ca4m4.blueember.filing.b05-blue-ember-reply-brief",
        "ca4m4.blueember.filing.b11-rehearing-petition",
        "ca4m4.blueember.filing.b15-stay-motion-denial",
        "ca4m4.blueember.filing.b16-stay-response-denial",
        "ca4m4.blueember.filing.b20-stay-motion-grant",
        "ca4m4.blueember.filing.b21-stay-response-grant",
    };
}

} // namespace

int main(int argc, char** argv) try {
    QCoreApplication application(argc, argv);
    const QDir content_root(QStringLiteral(APPELLATE_M4_BLUEEMBER_ROOT));
    const QDir foundations(QStringLiteral(APPELLATE_M4_FOUNDATIONS));
    auditStaticContent(content_root);

    const model::PackRevision root{model::PackId{"us.ca4.m4.blueember-jmol"}, "1.2.0",
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
        loaded->resources.size() != 9U || loaded->blobs.size() != 83U ||
        loaded->required_capabilities.size() != 16U || loaded->dependencies.size() != 3U) {
        fail("final deferred root cannot load with exact 9/83/16/3 envelope");
    }

    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        fail("cannot create integration temporary directory");
    }
    const QDir temporary_root(temporary.path());
    const auto archive_a = temporary_root.filePath(QStringLiteral("blueember-a.awpack"));
    const auto archive_b = temporary_root.filePath(QStringLiteral("blueember-b.awpack"));
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
    if (runtime_case.definition.id.value != "ca4m4.case.blueember-jmol" ||
        runtime_case.workflow.id.value != "ca4m4.blueember.workflow.post-trial-jmol" ||
        runtime_case.workflow.stages.size() != 24U ||
        runtime_case.workflow.operations.size() != 93U ||
        runtime_case.workflow.filing_routes.size() != 15U ||
        runtime_case.record.docket_entries.size() != 83U ||
        runtime_case.record.page_anchors.size() != 656U ||
        runtime_case.argument_configurations.size() != 2U ||
        runtime_case.definition.disposition_plans.size() != 2U) {
        fail("exact root/runtime shape drift");
    }
    std::set<std::string> disposition_ids;
    for (const auto& disposition : runtime_case.definition.disposition_plans) {
        disposition_ids.insert(disposition.id.value);
    }
    if (disposition_ids !=
        std::set<std::string>{"ca4m4.blueember.disposition.actual-reversal-new-trial",
                              "ca4m4.blueember.disposition.counterfactual-preserved-jmol"}) {
        fail("exact two-disposition identity drift");
    }

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
    for (const auto& route : runtime_case.workflow.filing_routes)
        for (const auto& binding : route.filing_bindings)
            defined_filings.insert(binding.filing_id.value);
    if (defined_filings != exactFilingIds()) {
        fail("runtime exact 18-filing ID set drift");
    }
    const std::map<std::string, std::string> expected_deadlines{
        {"ca4m4.blueember.deadline.actual-opening-brief", "2026-03-09"},
        {"ca4m4.blueember.deadline.actual-response-brief", "2026-04-08"},
        {"ca4m4.blueember.deadline.actual-reply-brief", "2026-04-20"},
        {"ca4m4.blueember.deadline.counterfactual-opening-brief", "2026-03-09"},
        {"ca4m4.blueember.deadline.counterfactual-response-brief", "2026-04-08"},
        {"ca4m4.blueember.deadline.counterfactual-reply-brief", "2026-04-20"},
        {"ca4m4.blueember.deadline.actual-rehearing-petition", "2026-07-23"},
        {"ca4m4.blueember.deadline.actual-ordinary-mandate", "2026-07-30"},
        {"ca4m4.blueember.deadline.counterfactual-rehearing-petition", "2026-07-23"},
        {"ca4m4.blueember.deadline.counterfactual-ordinary-mandate", "2026-07-30"},
        {"ca4m4.blueember.deadline.counterfactual-post-rehearing-mandate", "2026-08-13"},
        {"ca4m4.blueember.deadline.counterfactual-post-stay-denial-mandate", "2026-07-30"},
        {"ca4m4.blueember.deadline.counterfactual-stay-through", "2026-10-27"},
        {"ca4m4.blueember.deadline.counterfactual-post-dissolution-mandate", "2026-08-13"},
    };
    if (totals.commands != 270U || totals.events != 270U || totals.prefix_replays != 270U ||
        totals.full_replays != 12U || totals.operations != defined_operations ||
        totals.operations.size() != 93U || totals.accepted != defined_filings ||
        totals.rejected != defined_filings || defined_filings.size() != 18U ||
        totals.rejections != 61U || totals.court_documents.size() != 25U ||
        totals.deadlines != expected_deadlines ||
        totals.terminals["ca4m4.blueember.stage.terminated"] != 5U ||
        totals.terminals["ca4m4.blueember.stage.counterfactual-mandate-stayed"] != 1U) {
        fail("corpus closure mismatch");
    }
    std::cout
        << "INDEPENDENT TRACE AUDIT CLEAR paths=6 commands=270 events=270 prefix-replays=270 "
           "full-replays=12 operations=93 filings=18 rejections=61 court-docs=25 deadlines=14 "
           "endpoints=5+1 tampers=78\n";
    std::cout << "Blue Ember JMOL integration passed: 9 contents / 83 PDFs / 656 anchors; "
                 "24 stages / 93 operations / 15 routes / 43 bound documents; "
                 "6 traces / 270 commands / 270 events.\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Blue Ember JMOL integration failed: " << error.what() << '\n';
    return 1;
}
