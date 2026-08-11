#include "appellate/engine/workflow_engine.hpp"
#include "appellate/packs/pack_reader.hpp"
#include "appellate/packs/runtime_pack.hpp"
#include "appellate/storage/workflow_codec.hpp"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace engine = appellate::engine;
namespace model = appellate::model;
namespace packs = appellate::packs;

constexpr auto pack_id = "us.ca4.rule54b.asterglen";
constexpr auto pack_version = "0.1.0";
constexpr auto session_id = "ca4r54b.session.gold-canonical";
constexpr auto appellant_actor = "ca4r54b.actor.asterglen";
constexpr auto first_appellee_actor = "ca4r54b.actor.copper-kestrel";
constexpr auto second_appellee_actor = "ca4r54b.actor.meridian-silt";
constexpr auto court_actor = "ca4r54b.actor.composite-panel";
constexpr auto clerk_actor = "ca4r54b.actor.ca4-clerk";
constexpr auto evidence_engine_revision = "appellate.workflow-engine.gold.v1";

[[nodiscard]] QString goldPackPath() { return QStringLiteral(APPELLATE_GOLD_PACK); }

[[nodiscard]] model::LegalDate date(int year, unsigned month, unsigned day) {
    return model::LegalDate{std::chrono::year{year} / std::chrono::month{month} /
                            std::chrono::day{day}};
}

[[nodiscard]] model::LegalTime at(model::LegalDate court_date) {
    return model::LegalTime{std::chrono::sys_seconds{std::chrono::sys_days{court_date.value}},
                            court_date};
}

[[nodiscard]] auto loadGoldPack() -> std::expected<packs::RuntimePack, std::string> {
    const auto loaded = packs::PackReader::readDirectory(goldPackPath());
    if (!loaded) {
        return std::unexpected(loaded.error().message.toStdString());
    }
    const auto runtime = packs::loadRuntimePack(*loaded);
    if (!runtime) {
        return std::unexpected(runtime.error().message);
    }
    return *runtime;
}

[[nodiscard]] model::WorkflowState initialState(const packs::RuntimeCase& runtime_case) {
    return model::WorkflowState{
        session_id,
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
}

[[nodiscard]] model::WorkflowCommandHeader header(std::string command_id, std::string actor_id,
                                                  model::LegalDate occurred_on) {
    return model::WorkflowCommandHeader{
        session_id,
        model::WorkflowCommandId{std::move(command_id)},
        model::ActorId{std::move(actor_id)},
        at(occurred_on),
    };
}

[[nodiscard]] model::WorkflowFieldValue field(std::string id, std::string value) {
    return model::WorkflowFieldValue{model::FilingFieldId{std::move(id)}, std::move(value)};
}

[[nodiscard]] auto recordDigest(const packs::RuntimeCase& runtime_case, std::string_view entry_id)
    -> std::optional<std::string> {
    const auto entry = std::ranges::find_if(
        runtime_case.record.docket_entries,
        [&](const packs::RuntimeDocketEntry& candidate) { return candidate.id.value == entry_id; });
    if (entry == runtime_case.record.docket_entries.end()) {
        return std::nullopt;
    }
    return entry->asset_sha256;
}

struct Run final {
    model::WorkflowState initial_state;
    model::WorkflowState state;
    std::vector<model::WorkflowJournalEntry> journal;
    std::vector<model::WorkflowEvent> trace;
};

[[nodiscard]] Run emptyRun(const packs::RuntimeCase& runtime_case) {
    const auto initial = initialState(runtime_case);
    return Run{initial, initial, {}, {}};
}

[[nodiscard]] auto execute(const packs::RuntimeCase& runtime_case, Run& run,
                           model::WorkflowCommand command) -> std::expected<void, std::string> {
    const auto decision =
        engine::decideWorkflow(runtime_case.workflow, runtime_case.definition, run.state, command);
    if (!decision) {
        return std::unexpected(decision.error().message);
    }
    auto candidate_journal = run.journal;
    candidate_journal.push_back(model::WorkflowJournalEntry{std::move(command), *decision});
    const auto replayed = engine::replayWorkflow(runtime_case.workflow, runtime_case.definition,
                                                 run.initial_state, candidate_journal);
    if (!replayed) {
        return std::unexpected(replayed.error().message);
    }
    run.trace.insert(run.trace.end(), decision->begin(), decision->end());
    run.journal = std::move(candidate_journal);
    run.state = *replayed;
    return {};
}

[[nodiscard]] auto decideOnly(const packs::RuntimeCase& runtime_case, const Run& run,
                              const model::WorkflowCommand& command)
    -> std::expected<std::vector<model::WorkflowEvent>, std::string> {
    const auto decision =
        engine::decideWorkflow(runtime_case.workflow, runtime_case.definition, run.state, command);
    if (!decision) {
        return std::unexpected(decision.error().message);
    }
    return *decision;
}

[[nodiscard]] const model::WorkflowEventHeader& eventHeader(const model::WorkflowEvent& event) {
    return std::visit(
        [](const auto& concrete) -> const model::WorkflowEventHeader& { return concrete.header; },
        event);
}

void addUint64(QCryptographicHash& hash, std::uint64_t value) {
    std::array<char, 8> bytes{};
    for (int index = 7; index >= 0; --index) {
        bytes.at(static_cast<std::size_t>(index)) = static_cast<char>(value & 0xffU);
        value >>= 8U;
    }
    hash.addData(QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())));
}

void addFrame(QCryptographicHash& hash, QByteArrayView bytes) {
    addUint64(hash, static_cast<std::uint64_t>(bytes.size()));
    hash.addData(bytes);
}

void addFrame(QCryptographicHash& hash, const QString& value) {
    const auto bytes = value.toUtf8();
    addFrame(hash, QByteArrayView(bytes));
}

[[nodiscard]] auto journalDigest(std::span<const model::WorkflowJournalEntry> journal)
    -> std::expected<QString, QString> {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, QStringLiteral("appellate-workbench-executed-workflow-journal-v1"));
    addUint64(hash, journal.size());
    for (const auto& entry : journal) {
        const auto command = appellate::storage::encodeWorkflowCommand(entry.command);
        if (!command) {
            return std::unexpected(command.error().message);
        }
        addFrame(hash, QByteArrayView(*command));
        addUint64(hash, entry.events.size());
        for (const auto& event : entry.events) {
            const auto encoded = appellate::storage::encodeWorkflowEvent(event);
            if (!encoded) {
                return std::unexpected(encoded.error().message);
            }
            addFrame(hash, QByteArrayView(*encoded));
        }
    }
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] QString traceDigest(const QString& case_id, const QJsonObject& trace) {
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
    const auto operations = trace.value(QStringLiteral("operation_ids")).toArray();
    addUint64(hash, static_cast<std::uint64_t>(operations.size()));
    for (const auto& operation : operations) {
        addFrame(hash, operation.toString());
    }
    addFrame(hash, trace.value(QStringLiteral("terminal_stage_id")).toString());
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] const model::WorkflowDeadlineRecord* deadlineFor(const model::WorkflowState& state,
                                                               std::string_view deadline_id) {
    const auto deadline =
        std::ranges::find_if(state.deadlines, [&](const model::WorkflowDeadlineRecord& candidate) {
            return candidate.deadline_id.value == deadline_id;
        });
    return deadline == state.deadlines.end() ? nullptr : &*deadline;
}

[[nodiscard]] model::SubmitWorkflowFiling noticeFiling(const packs::RuntimeCase& runtime_case) {
    return model::SubmitWorkflowFiling{
        header("ca4r54b.command.file-notice", appellant_actor, date(2026, 3, 3)),
        model::WorkflowFilingId{"ca4r54b.filing-instance.notice"},
        model::FilingTypeId{"ca4r54b.filing.notice-appeal"},
        *recordDigest(runtime_case, "ca4r54b.record.d61"),
        {
            field("ca4r54b.field.notice-party", "Asterglen Freight Software, Inc."),
            field("ca4r54b.field.notice-judgment", "District ECF Nos. 59 and 60"),
            field("ca4r54b.field.notice-court",
                  "United States Court of Appeals for the Fourth Circuit"),
            field("ca4r54b.field.notice-date", "2026-02-04"),
        },
        {},
        std::nullopt,
    };
}

[[nodiscard]] model::SubmitWorkflowFiling
docketingStatement(const packs::RuntimeCase& runtime_case) {
    return model::SubmitWorkflowFiling{
        header("ca4r54b.command.file-docketing-statement", appellant_actor, date(2026, 3, 16)),
        model::WorkflowFilingId{"ca4r54b.filing-instance.docketing-statement"},
        model::FilingTypeId{"ca4r54b.filing.docketing-statement"},
        *recordDigest(runtime_case, "ca4r54b.record.a02"),
        {
            field("ca4r54b.field.docket-jurisdiction", "28 U.S.C. section 1291 and Rule 54(b)"),
            field("ca4r54b.field.docket-issues", "Finality and notice timeliness"),
            field("ca4r54b.field.docket-transcript", "No transcript will be ordered"),
        },
        {model::ActorId{first_appellee_actor}, model::ActorId{second_appellee_actor}},
        std::nullopt,
    };
}

[[nodiscard]] model::SubmitWorkflowFiling openingBrief(const packs::RuntimeCase& runtime_case) {
    return model::SubmitWorkflowFiling{
        header("ca4r54b.command.file-opening-brief", appellant_actor, date(2026, 4, 29)),
        model::WorkflowFilingId{"ca4r54b.filing-instance.opening-brief"},
        model::FilingTypeId{"ca4r54b.filing.opening-brief"},
        *recordDigest(runtime_case, "ca4r54b.record.a10"),
        {
            field("ca4r54b.field.brief-jurisdiction", "Appellant asserts Rule 54(b) finality"),
            field("ca4r54b.field.brief-issues", "Whether the partial judgment is appealable"),
            field("ca4r54b.field.brief-record-citations", "JA1-JA47"),
            field("ca4r54b.field.brief-relief", "Vacatur and remand"),
        },
        {model::ActorId{first_appellee_actor}, model::ActorId{second_appellee_actor}},
        std::nullopt,
    };
}

[[nodiscard]] model::SubmitWorkflowFiling responseBrief(const packs::RuntimeCase& runtime_case,
                                                        std::string command_id,
                                                        std::string filing_id,
                                                        model::LegalDate filed_on) {
    return model::SubmitWorkflowFiling{
        header(std::move(command_id), first_appellee_actor, filed_on),
        model::WorkflowFilingId{std::move(filing_id)},
        model::FilingTypeId{"ca4r54b.filing.response-brief"},
        *recordDigest(runtime_case, "ca4r54b.record.a11"),
        {
            field("ca4r54b.field.response-jurisdiction", "The Rule 54(b) predicates are absent"),
            field("ca4r54b.field.response-argument", "Dismiss for want of appellate jurisdiction"),
        },
        {model::ActorId{appellant_actor}},
        std::nullopt,
    };
}

class GoldCaseTraceTest final : public QObject {
    Q_OBJECT

  private slots:
    void loadsExactPackAndResolvesEveryRuntimeLink();
    void replaysCanonicalTraceThroughMandateAndKeepsAdverseBranchesIsolated();
};

void GoldCaseTraceTest::loadsExactPackAndResolvesEveryRuntimeLink() {
    const auto loaded = packs::PackReader::readDirectory(goldPackPath());
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error().message));
    QCOMPARE(loaded->revision.id.value, std::string(pack_id));
    QCOMPARE(loaded->revision.version, std::string(pack_version));
    QCOMPARE(loaded->revision.digest.size(), std::size_t{64});
    QVERIFY(std::ranges::all_of(loaded->revision.digest, [](char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    }));
    QCOMPARE(loaded->resources.size(), std::size_t{19});
    QCOMPARE(loaded->blobs.size(), std::size_t{18});
    QCOMPARE(loaded->judge_profiles.size(), std::size_t{3});

    const auto runtime = packs::loadRuntimePack(*loaded);
    QVERIFY2(runtime.has_value(), runtime ? "" : runtime.error().message.c_str());
    QCOMPARE(runtime->revision, loaded->revision);
    QCOMPARE(runtime->cases.size(), std::size_t{1});
    const auto& runtime_case = runtime->cases.front();

    QCOMPARE(runtime_case.definition.id.value, std::string("ca4r54b.case.asterglen"));
    QCOMPARE(runtime_case.definition.procedure_id.value,
             std::string("ca4r54b.procedure.civil-appeal"));
    QCOMPARE(runtime_case.definition.actors.size(), std::size_t{5});
    const auto clerk =
        std::ranges::find(runtime_case.definition.actors, model::ActorId{std::string(clerk_actor)},
                          &model::CaseActor::id);
    QVERIFY(clerk != runtime_case.definition.actors.end());
    QCOMPARE(clerk->role.value, std::string("ca4r54b.role.court"));
    const auto panel =
        std::ranges::find(runtime_case.definition.actors, model::ActorId{std::string(court_actor)},
                          &model::CaseActor::id);
    QVERIFY(panel != runtime_case.definition.actors.end());
    QCOMPARE(panel->role.value, std::string("ca4r54b.role.court"));
    QCOMPARE(runtime_case.procedure.court_id.value, std::string("ca4r54b.court.ca4"));
    QCOMPARE(runtime_case.court.id, runtime_case.procedure.court_id);
    QCOMPARE(runtime_case.court.jurisdiction_id.value, std::string("us.ca4"));
    QCOMPARE(runtime_case.procedure.workflow_id, runtime_case.workflow.id);
    QCOMPARE(runtime_case.workflow.id.value, std::string("ca4r54b.workflow.civil-rule54b"));
    QCOMPARE(runtime_case.record.id.value, std::string("ca4r54b.record.asterglen"));
    QCOMPARE(runtime_case.record.dockets.size(), std::size_t{2});
    QCOMPARE(runtime_case.record.docket_entries.size(), std::size_t{18});
    QCOMPARE(runtime_case.record.page_anchors.size(), std::size_t{47});
    QCOMPARE(runtime_case.issues.size(), std::size_t{3});
    QCOMPARE(runtime_case.argument_configurations.size(), std::size_t{1});

    const auto district_docket = std::ranges::find_if(
        runtime_case.record.dockets, [](const packs::RuntimeDocketDescriptor& docket) {
            return docket.id.value == "ca4r54b.docket.edva";
        });
    QVERIFY(district_docket != runtime_case.record.dockets.end());
    QCOMPARE(district_docket->type, packs::RuntimeDocketType::District);
    QVERIFY(!district_docket->court_id.has_value());
    QCOMPARE(district_docket->court_ref,
             std::string("United States District Court for the Eastern District of Virginia"));
    QCOMPARE(district_docket->public_docket_number, std::string("SYN-25-0117"));

    const auto appellate_docket = std::ranges::find_if(
        runtime_case.record.dockets, [](const packs::RuntimeDocketDescriptor& docket) {
            return docket.id.value == "ca4r54b.docket.ca4";
        });
    QVERIFY(appellate_docket != runtime_case.record.dockets.end());
    QCOMPARE(appellate_docket->type, packs::RuntimeDocketType::Appellate);
    QVERIFY(appellate_docket->court_id.has_value());
    QCOMPARE(appellate_docket->court_id->value, std::string("ca4r54b.court.ca4"));
    QCOMPARE(appellate_docket->court_ref,
             std::string("United States Court of Appeals for the Fourth Circuit"));
    QCOMPARE(appellate_docket->public_docket_number, std::string("SYN-26-1427"));

    std::unordered_map<std::string, const model::WorkflowOperation*> operations;
    for (const auto& operation : runtime_case.workflow.operations) {
        QVERIFY(operations.emplace(operation.id.value, &operation).second);
    }
    for (const auto& route : runtime_case.workflow.filing_routes) {
        QVERIFY(operations.contains(route.accept_operation_id.value));
        QVERIFY(operations.contains(route.reject_operation_id.value));
        QCOMPARE(operations.at(route.accept_operation_id.value)->stage_id, route.stage_id);
        QCOMPARE(operations.at(route.reject_operation_id.value)->stage_id, route.stage_id);
        QVERIFY(!route.advance_operation_id.has_value());
        if (route.deficiency_operation_id.has_value()) {
            QVERIFY(operations.contains(route.deficiency_operation_id->value));
        }
        if (route.accepted_deadline.has_value()) {
            QVERIFY(operations.contains(route.accepted_deadline->operation_id.value));
        }
    }
    QVERIFY(operations.contains(runtime_case.authored_disposition_id.value));
    QCOMPARE(operations.at(runtime_case.authored_disposition_id.value)->opcode,
             model::WorkflowOpcode::IssueJudgment);

    std::unordered_set<std::string> record_ids;
    std::unordered_set<std::string> asset_paths;
    for (const auto& entry : runtime_case.record.docket_entries) {
        QVERIFY(record_ids.emplace(entry.id.value).second);
        QVERIFY(asset_paths.emplace(entry.asset_path).second);
        const auto blob =
            std::ranges::find_if(loaded->blobs, [&](const model::BlobDescriptor& item) {
                return item.path == entry.asset_path;
            });
        QVERIFY(blob != loaded->blobs.end());
        QCOMPARE(blob->sha256, entry.asset_sha256);
        QCOMPARE(blob->media_type, std::string("application/pdf"));
        QVERIFY(entry.page_count > 0U);
    }
    QCOMPARE(asset_paths.size(), loaded->blobs.size());
    const auto joint_appendix = std::ranges::find_if(
        runtime_case.record.docket_entries, [](const packs::RuntimeDocketEntry& entry) {
            return entry.id.value == "ca4r54b.record.a12";
        });
    QVERIFY(joint_appendix != runtime_case.record.docket_entries.end());
    QCOMPARE(joint_appendix->page_count, std::uint32_t{47});
    QCOMPARE(joint_appendix->asset_path, std::string("assets/a12-joint-appendix.pdf"));

    struct ExpectedRecordEntry final {
        std::string_view id;
        std::string_view docket_id;
        std::string_view entry_label;
        std::string_view actor;
    };
    constexpr std::array<ExpectedRecordEntry, 18> expected_record_entries{{
        {"ca4r54b.record.d01", "ca4r54b.docket.edva", "ECF No. 1",
         "Asterglen Freight Software, Inc."},
        {"ca4r54b.record.d08", "ca4r54b.docket.edva", "ECF No. 8",
         "Copper Kestrel Logistics, LLC and Meridian Silt Holdings, LLC"},
        {"ca4r54b.record.d55", "ca4r54b.docket.edva", "ECF No. 55",
         "District Jurist Solace (fictional composite)"},
        {"ca4r54b.record.d57", "ca4r54b.docket.edva", "ECF No. 57",
         "Asterglen Freight Software, Inc."},
        {"ca4r54b.record.d58", "ca4r54b.docket.edva", "ECF No. 58",
         "Copper Kestrel Logistics, LLC and Meridian Silt Holdings, LLC"},
        {"ca4r54b.record.d59", "ca4r54b.docket.edva", "ECF No. 59",
         "District Jurist Solace (fictional composite)"},
        {"ca4r54b.record.d60", "ca4r54b.docket.edva", "ECF No. 60", "Exercise district clerk"},
        {"ca4r54b.record.d61", "ca4r54b.docket.edva", "ECF No. 61",
         "Asterglen Freight Software, Inc."},
        {"ca4r54b.record.a01", "ca4r54b.docket.ca4", "ECF No. 1", "Exercise Fourth Circuit clerk"},
        {"ca4r54b.record.a02", "ca4r54b.docket.ca4", "ECF No. 5",
         "Asterglen Freight Software, Inc."},
        {"ca4r54b.record.a03", "ca4r54b.docket.ca4", "ECF No. 6",
         "Asterglen Freight Software, Inc."},
        {"ca4r54b.record.d62", "ca4r54b.docket.edva", "ECF No. 65", "Exercise district clerk"},
        {"ca4r54b.record.a10", "ca4r54b.docket.ca4", "ECF No. 14",
         "Asterglen Freight Software, Inc."},
        {"ca4r54b.record.a12", "ca4r54b.docket.ca4", "ECF No. 15", "Parties jointly (fictional)"},
        {"ca4r54b.record.a11", "ca4r54b.docket.ca4", "ECF No. 18",
         "Copper Kestrel Logistics, LLC and Meridian Silt Holdings, LLC"},
        {"ca4r54b.record.a20", "ca4r54b.docket.ca4", "ECF No. 21",
         "Fictional composite appellate panel"},
        {"ca4r54b.record.a21", "ca4r54b.docket.ca4", "ECF No. 22", "Exercise Fourth Circuit clerk"},
        {"ca4r54b.record.a22", "ca4r54b.docket.ca4", "ECF No. 24", "Exercise Fourth Circuit clerk"},
    }};
    for (std::size_t index = 0; index < expected_record_entries.size(); ++index) {
        const auto& expected = expected_record_entries[index];
        const auto entry = std::ranges::find_if(runtime_case.record.docket_entries,
                                                [&](const packs::RuntimeDocketEntry& candidate) {
                                                    return candidate.id.value == expected.id;
                                                });
        QVERIFY(entry != runtime_case.record.docket_entries.end());
        QCOMPARE(entry->entry_number, static_cast<std::uint32_t>(index + 1U));
        QVERIFY(entry->docket_id.has_value());
        QCOMPARE(entry->docket_id->value, std::string(expected.docket_id));
        QVERIFY(entry->entry_label.has_value());
        QCOMPARE(*entry->entry_label, std::string(expected.entry_label));
        QVERIFY(entry->actor.has_value());
        QCOMPARE(*entry->actor, std::string(expected.actor));
        QVERIFY(entry->description.has_value());
        QVERIFY(!entry->description->empty());
        QCOMPARE(entry->tags.size(), std::size_t{3});
        QVERIFY(!entry->parent_entry_id.has_value());
        QVERIFY(!entry->relationship.has_value());
    }

    std::unordered_set<std::string> page_anchor_ids;
    for (std::size_t index = 0; index < runtime_case.record.page_anchors.size(); ++index) {
        const auto& anchor = runtime_case.record.page_anchors[index];
        const auto page_number = static_cast<std::uint32_t>(index + 1U);
        const auto suffix = std::to_string(page_number);
        QCOMPARE(anchor.id.value, std::string("ca4r54b.record.anchor.ja") + suffix);
        QCOMPARE(anchor.entry_id.value, std::string("ca4r54b.record.a12"));
        QCOMPARE(anchor.page_number, page_number);
        QVERIFY(anchor.citation_label.has_value());
        QCOMPARE(*anchor.citation_label, std::string("JA") + suffix);
        QVERIFY(page_anchor_ids.emplace(anchor.id.value).second);
    }

    std::unordered_set<std::string> issue_ids;
    for (const auto& issue : runtime_case.issues) {
        QVERIFY(issue_ids.emplace(issue.id.value).second);
        QVERIFY(!issue.authority_ids.empty());
        QVERIFY(!issue.record_anchor_ids.empty());
        for (const auto& anchor : issue.record_anchor_ids) {
            QVERIFY(page_anchor_ids.contains(anchor.value));
        }
    }

    const auto issue = [&](std::string_view id) {
        return std::ranges::find_if(runtime_case.issues, [&](const packs::RuntimeIssue& candidate) {
            return candidate.id.value == id;
        });
    };
    const auto finality_issue = issue("ca4r54b.issue.rule54b-finality");
    QVERIFY(finality_issue != runtime_case.issues.end());
    QVERIFY(finality_issue->record_anchor_ids == std::vector<packs::RuntimeRecordAnchorId>({
                                                     {"ca4r54b.record.anchor.ja27"},
                                                     {"ca4r54b.record.anchor.ja29"},
                                                     {"ca4r54b.record.anchor.ja37"},
                                                     {"ca4r54b.record.anchor.ja40"},
                                                     {"ca4r54b.record.anchor.ja41"},
                                                 }));
    const auto timeliness_issue = issue("ca4r54b.issue.notice-timeliness");
    QVERIFY(timeliness_issue != runtime_case.issues.end());
    QVERIFY(timeliness_issue->record_anchor_ids == std::vector<packs::RuntimeRecordAnchorId>({
                                                       {"ca4r54b.record.anchor.ja41"},
                                                       {"ca4r54b.record.anchor.ja42"},
                                                       {"ca4r54b.record.anchor.ja43"},
                                                       {"ca4r54b.record.anchor.ja46"},
                                                   }));
    const auto merits_issue = issue("ca4r54b.issue.merits-not-reached");
    QVERIFY(merits_issue != runtime_case.issues.end());
    QVERIFY(merits_issue->record_anchor_ids == std::vector<packs::RuntimeRecordAnchorId>({
                                                   {"ca4r54b.record.anchor.ja7"},
                                                   {"ca4r54b.record.anchor.ja8"},
                                                   {"ca4r54b.record.anchor.ja15"},
                                                   {"ca4r54b.record.anchor.ja25"},
                                                   {"ca4r54b.record.anchor.ja26"},
                                               }));

    const auto& argument = runtime_case.argument_configurations.front();
    QCOMPARE(argument.case_id, runtime_case.definition.id);
    QCOMPARE(argument.bench.court_id, runtime_case.court.id);
    QCOMPARE(argument.bench.seats.size(), std::size_t{3});
    QCOMPARE(argument.permitted_issue_ids.size(), issue_ids.size());
    for (const auto& permitted : argument.permitted_issue_ids) {
        QVERIFY(issue_ids.contains(permitted.value));
    }
    const auto presiding = std::ranges::find(argument.bench.seats, argument.bench.presiding_seat_id,
                                             &packs::RuntimeBenchSeat::id);
    QVERIFY(presiding != argument.bench.seats.end());
    std::unordered_set<std::string> profile_ids;
    for (const auto& seat : argument.bench.seats) {
        QVERIFY(profile_ids.emplace(seat.profile_id.value).second);
        QCOMPARE(seat.profile.id, seat.profile_id.value);
        QCOMPARE(seat.profile.profile_class, model::ProfileClass::FictionalComposite);
        QVERIFY(std::ranges::find(seat.profile.compatibility.court_roles,
                                  model::CourtRole::Appellate) !=
                seat.profile.compatibility.court_roles.end());
        QVERIFY(std::ranges::find(seat.profile.compatibility.jurisdiction_ids,
                                  runtime_case.court.jurisdiction_id.value) !=
                seat.profile.compatibility.jurisdiction_ids.end());
    }
}

void GoldCaseTraceTest::replaysCanonicalTraceThroughMandateAndKeepsAdverseBranchesIsolated() {
    const auto runtime = loadGoldPack();
    QVERIFY2(runtime.has_value(), runtime ? "" : runtime.error().c_str());
    const auto& runtime_case = runtime->cases.front();
    auto run = emptyRun(runtime_case);

    const auto mustExecute = [&](model::WorkflowCommand command) {
        const auto result = execute(runtime_case, run, std::move(command));
        QVERIFY2(result.has_value(), result ? "" : result.error().c_str());
    };

    mustExecute(model::CalculateWorkflowDeadline{
        header("ca4r54b.command.calculate-notice", clerk_actor, date(2026, 2, 4)),
        model::WorkflowOperationId{"ca4r54b.operation.calculate-notice-deadline"},
        model::WorkflowDeadlineId{"ca4r54b.deadline.notice-appeal"}});
    mustExecute(noticeFiling(runtime_case));
    mustExecute(model::AdvanceWorkflowStage{
        header("ca4r54b.command.advance-docketed", clerk_actor, date(2026, 3, 5)),
        model::WorkflowOperationId{"ca4r54b.operation.advance-docketed"}});
    mustExecute(model::EnterWorkflowOrder{
        header("ca4r54b.command.enter-docketing-notice", clerk_actor, date(2026, 3, 5)),
        model::WorkflowOperationId{"ca4r54b.operation.enter-docketing-notice"},
        model::WorkflowOrderId{"ca4r54b.order.docketing-notice"},
        model::WorkflowOrderDisposition::Other, *recordDigest(runtime_case, "ca4r54b.record.a01"),
        std::nullopt});
    mustExecute(model::CalculateWorkflowDeadline{
        header("ca4r54b.command.calculate-initial", clerk_actor, date(2026, 3, 5)),
        model::WorkflowOperationId{"ca4r54b.operation.calculate-initial-deadline"},
        model::WorkflowDeadlineId{"ca4r54b.deadline.initial-documents"}});
    mustExecute(model::AdvanceWorkflowStage{
        header("ca4r54b.command.advance-initial", clerk_actor, date(2026, 3, 5)),
        model::WorkflowOperationId{"ca4r54b.operation.advance-initial-requirements"}});

    const auto before_deficiency = run;
    const auto deficient_initial = model::SubmitWorkflowFiling{
        header("ca4r54b.command.adverse-initial", appellant_actor, date(2026, 3, 15)),
        model::WorkflowFilingId{"ca4r54b.filing-instance.adverse-initial"},
        model::FilingTypeId{"ca4r54b.filing.docketing-statement"},
        *recordDigest(runtime_case, "ca4r54b.record.a02"),
        {},
        {},
        std::nullopt};
    const auto deficiency = decideOnly(runtime_case, run, deficient_initial);
    QVERIFY2(deficiency.has_value(), deficiency ? "" : deficiency.error().c_str());
    QCOMPARE(deficiency->size(), std::size_t{1});
    const auto* issued = std::get_if<model::WorkflowDeficiencyIssued>(&deficiency->front());
    QVERIFY(issued != nullptr);
    QCOMPARE(issued->header.operation_id.value,
             std::string("ca4r54b.operation.deficiency-initial-documents"));
    QCOMPARE(issued->missing_requirements,
             std::vector<model::WorkflowRequirementId>({
                 model::WorkflowRequirementId{"ca4r54b.field.docket-issues"},
                 model::WorkflowRequirementId{"ca4r54b.field.docket-jurisdiction"},
                 model::WorkflowRequirementId{"ca4r54b.field.docket-transcript"},
                 model::WorkflowRequirementId{"service.ca4r54b.actor.copper-kestrel"},
                 model::WorkflowRequirementId{"service.ca4r54b.actor.meridian-silt"},
             }));
    QVERIFY(!issued->cure_deadline_id.has_value());
    QVERIFY(run.state == before_deficiency.state);
    QVERIFY(run.journal == before_deficiency.journal);

    mustExecute(docketingStatement(runtime_case));
    mustExecute(model::CalculateWorkflowDeadline{
        header("ca4r54b.command.calculate-opening", clerk_actor, date(2026, 3, 20)),
        model::WorkflowOperationId{"ca4r54b.operation.calculate-opening-brief-deadline"},
        model::WorkflowDeadlineId{"ca4r54b.deadline.opening-brief"}});
    mustExecute(model::AdvanceWorkflowStage{
        header("ca4r54b.command.advance-briefing", clerk_actor, date(2026, 3, 23)),
        model::WorkflowOperationId{"ca4r54b.operation.advance-briefing"}});

    auto nonconforming_opening = openingBrief(runtime_case);
    nonconforming_opening.header.command_id =
        model::WorkflowCommandId{"ca4r54b.command.adverse-opening"};
    nonconforming_opening.header.occurred_at = at(date(2026, 4, 28));
    nonconforming_opening.filing_id =
        model::WorkflowFilingId{"ca4r54b.filing-instance.adverse-opening"};
    nonconforming_opening.fields.clear();
    nonconforming_opening.served_actors.clear();
    const auto rejected_opening = decideOnly(runtime_case, run, nonconforming_opening);
    QVERIFY2(rejected_opening.has_value(),
             rejected_opening ? "" : rejected_opening.error().c_str());
    QCOMPARE(rejected_opening->size(), std::size_t{1});
    const auto* opening_rejection =
        std::get_if<model::WorkflowFilingRejected>(&rejected_opening->front());
    QVERIFY(opening_rejection != nullptr);
    QCOMPARE(opening_rejection->reason, model::WorkflowFilingRejectionReason::NonconformingFiling);
    QCOMPARE(opening_rejection->header.operation_id.value,
             std::string("ca4r54b.operation.reject-opening-brief"));

    mustExecute(openingBrief(runtime_case));
    mustExecute(model::AdvanceWorkflowStage{
        header("ca4r54b.command.advance-response", clerk_actor, date(2026, 4, 29)),
        model::WorkflowOperationId{"ca4r54b.operation.advance-response-briefing"}});

    const auto before_late_response = run;
    const auto late_response =
        responseBrief(runtime_case, "ca4r54b.command.adverse-late-response",
                      "ca4r54b.filing-instance.adverse-late-response", date(2026, 6, 1));
    const auto rejected_late = decideOnly(runtime_case, run, late_response);
    QVERIFY2(rejected_late.has_value(), rejected_late ? "" : rejected_late.error().c_str());
    QCOMPARE(rejected_late->size(), std::size_t{1});
    const auto* late_rejection =
        std::get_if<model::WorkflowFilingRejected>(&rejected_late->front());
    QVERIFY(late_rejection != nullptr);
    QCOMPARE(late_rejection->reason, model::WorkflowFilingRejectionReason::DeadlineExpired);
    QCOMPARE(late_rejection->header.operation_id.value,
             std::string("ca4r54b.operation.reject-response-brief"));
    QVERIFY(run.state == before_late_response.state);
    QVERIFY(run.journal == before_late_response.journal);

    mustExecute(responseBrief(runtime_case, "ca4r54b.command.file-response-brief",
                              "ca4r54b.filing-instance.response-brief", date(2026, 5, 8)));
    mustExecute(model::AdvanceWorkflowStage{
        header("ca4r54b.command.advance-submitted", clerk_actor, date(2026, 6, 3)),
        model::WorkflowOperationId{"ca4r54b.operation.advance-submitted"}});
    mustExecute(model::AdvanceWorkflowStage{
        header("ca4r54b.command.advance-judgment", court_actor, date(2026, 6, 8)),
        model::WorkflowOperationId{"ca4r54b.operation.advance-judgment"}});
    mustExecute(model::IssueWorkflowJudgment{
        header("ca4r54b.command.issue-dismissal", court_actor, date(2026, 6, 8)),
        model::WorkflowOperationId{"ca4r54b.operation.issue-dismissal-judgment"},
        *recordDigest(runtime_case, "ca4r54b.record.a21"),
        "Dismissed for want of appellate jurisdiction and remanded"});
    mustExecute(model::AdvanceWorkflowStage{
        header("ca4r54b.command.advance-rehearing", clerk_actor, date(2026, 6, 8)),
        model::WorkflowOperationId{"ca4r54b.operation.advance-rehearing"}});
    mustExecute(model::CalculateWorkflowDeadline{
        header("ca4r54b.command.calculate-rehearing", clerk_actor, date(2026, 6, 8)),
        model::WorkflowOperationId{"ca4r54b.operation.calculate-rehearing-deadline"},
        model::WorkflowDeadlineId{"ca4r54b.deadline.rehearing"}});
    mustExecute(model::CalculateWorkflowDeadline{
        header("ca4r54b.command.calculate-mandate", clerk_actor, date(2026, 6, 22)),
        model::WorkflowOperationId{"ca4r54b.operation.calculate-mandate-deadline"},
        model::WorkflowDeadlineId{"ca4r54b.deadline.mandate"}});
    mustExecute(model::AdvanceWorkflowStage{
        header("ca4r54b.command.advance-mandate", clerk_actor, date(2026, 6, 29)),
        model::WorkflowOperationId{"ca4r54b.operation.advance-mandate"}});
    mustExecute(model::IssueWorkflowMandate{
        header("ca4r54b.command.issue-mandate", clerk_actor, date(2026, 6, 29)),
        model::WorkflowOperationId{"ca4r54b.operation.issue-mandate"},
        *recordDigest(runtime_case, "ca4r54b.record.a22")});

    QCOMPARE(run.journal.size(), std::size_t{20});
    QCOMPARE(run.trace.size(), std::size_t{21});
    const auto computed_journal_digest = journalDigest(run.journal);
    QVERIFY2(computed_journal_digest.has_value(),
             computed_journal_digest ? "" : qPrintable(computed_journal_digest.error()));
    QJsonArray encoded_journal;
    for (const auto& entry : run.journal) {
        const auto command = appellate::storage::encodeWorkflowCommand(entry.command);
        QVERIFY2(command.has_value(), command ? "" : qPrintable(command.error().message));
        QJsonArray events;
        for (const auto& event : entry.events) {
            const auto encoded = appellate::storage::encodeWorkflowEvent(event);
            QVERIFY2(encoded.has_value(), encoded ? "" : qPrintable(encoded.error().message));
            events.push_back(QString::fromLatin1(encoded->toBase64()));
        }
        encoded_journal.push_back(QJsonObject{
            {QStringLiteral("command_base64"), QString::fromLatin1(command->toBase64())},
            {QStringLiteral("events_base64"), events},
        });
    }
    QJsonArray executed_operation_ids;
    for (const auto& event : run.trace) {
        executed_operation_ids.push_back(
            QString::fromStdString(eventHeader(event).operation_id.value));
    }
    QJsonObject executed_trace{
        {QStringLiteral("evidence_id"), QStringLiteral("ca4r54b.evidence.trace.gold-canonical")},
        {QStringLiteral("trace_id"), QStringLiteral("ca4r54b.trace.gold-canonical")},
        {QStringLiteral("workflow_id"), QString::fromStdString(runtime_case.workflow.id.value)},
        {QStringLiteral("engine_revision"), QString::fromLatin1(evidence_engine_revision)},
        {QStringLiteral("command_count"), static_cast<qint64>(run.journal.size())},
        {QStringLiteral("event_count"), static_cast<qint64>(run.trace.size())},
        {QStringLiteral("journal_sha256"), *computed_journal_digest},
        {QStringLiteral("journal"), encoded_journal},
        {QStringLiteral("operation_ids"), executed_operation_ids},
        {QStringLiteral("terminal_stage_id"),
         QString::fromStdString(run.state.current_stage_id.value)},
    };
    executed_trace.insert(
        QStringLiteral("digest"),
        traceDigest(QString::fromStdString(runtime_case.definition.id.value), executed_trace));
    QFile trace_fixture(QStringLiteral(APPELLATE_TEST_FIXTURES) +
                        QStringLiteral("/realism-evidence/gold-canonical-trace.json"));
    QVERIFY(trace_fixture.open(QIODevice::ReadOnly));
    QJsonParseError trace_fixture_error;
    const auto trace_fixture_document =
        QJsonDocument::fromJson(trace_fixture.readAll(), &trace_fixture_error);
    QCOMPARE(trace_fixture_error.error, QJsonParseError::NoError);
    QVERIFY(trace_fixture_document.isObject());
    const auto declared_trace = trace_fixture_document.object();
    QVERIFY2(
        declared_trace == executed_trace,
        qPrintable(QStringLiteral("declared=%1\nexecuted=%2")
                       .arg(QString::fromUtf8(
                                QJsonDocument(declared_trace).toJson(QJsonDocument::Compact)),
                            QString::fromUtf8(
                                QJsonDocument(executed_trace).toJson(QJsonDocument::Compact)))));
    QCOMPARE(declared_trace.value(QStringLiteral("digest")).toString(),
             traceDigest(QString::fromStdString(runtime_case.definition.id.value), declared_trace));
    QCOMPARE(run.state.next_event_sequence, std::uint64_t{22});
    QCOMPARE(run.state.current_stage_id.value, std::string("ca4r54b.stage.mandate-issued"));
    QCOMPARE(run.state.accepted_filings.size(), std::size_t{4});
    QCOMPARE(run.state.deficiencies.size(), std::size_t{0});
    QCOMPARE(run.state.orders.size(), std::size_t{1});
    QCOMPARE(run.state.deadlines.size(), std::size_t{6});
    QVERIFY(run.state.judgment_sha256 == recordDigest(runtime_case, "ca4r54b.record.a21"));
    QVERIFY(run.state.mandate_sha256 == recordDigest(runtime_case, "ca4r54b.record.a22"));

    const std::vector<std::string> expected_command_ids{
        "ca4r54b.command.calculate-notice",         "ca4r54b.command.file-notice",
        "ca4r54b.command.advance-docketed",         "ca4r54b.command.enter-docketing-notice",
        "ca4r54b.command.calculate-initial",        "ca4r54b.command.advance-initial",
        "ca4r54b.command.file-docketing-statement", "ca4r54b.command.calculate-opening",
        "ca4r54b.command.advance-briefing",         "ca4r54b.command.file-opening-brief",
        "ca4r54b.command.advance-response",         "ca4r54b.command.file-response-brief",
        "ca4r54b.command.advance-submitted",        "ca4r54b.command.advance-judgment",
        "ca4r54b.command.issue-dismissal",          "ca4r54b.command.advance-rehearing",
        "ca4r54b.command.calculate-rehearing",      "ca4r54b.command.calculate-mandate",
        "ca4r54b.command.advance-mandate",          "ca4r54b.command.issue-mandate",
    };
    std::vector<std::string> actual_command_ids;
    actual_command_ids.reserve(run.journal.size());
    for (const auto& entry : run.journal) {
        actual_command_ids.push_back(std::visit(
            [](const auto& concrete) { return concrete.header.command_id.value; }, entry.command));
        const auto actor_role_is_exact = std::visit(
            [&](const auto& concrete) {
                const auto actor =
                    std::ranges::find(runtime_case.definition.actors, concrete.header.actor_id,
                                      &model::CaseActor::id);
                if (actor == runtime_case.definition.actors.end()) {
                    return false;
                }
                using Command = std::remove_cvref_t<decltype(concrete)>;
                if constexpr (std::same_as<Command, model::SubmitWorkflowFiling>) {
                    const auto expected_role =
                        concrete.filing_type.value == "ca4r54b.filing.response-brief"
                            ? std::string_view{"ca4r54b.role.appellee"}
                            : std::string_view{"ca4r54b.role.appellant"};
                    return actor->role.value == expected_role;
                } else {
                    return actor->role.value == "ca4r54b.role.court";
                }
            },
            entry.command);
        QVERIFY(actor_role_is_exact);
    }
    QCOMPARE(actual_command_ids, expected_command_ids);

    const std::vector expected_deadlines{
        std::pair{std::string_view{"ca4r54b.deadline.notice-appeal"}, date(2026, 3, 6)},
        std::pair{std::string_view{"ca4r54b.deadline.initial-documents"}, date(2026, 3, 19)},
        std::pair{std::string_view{"ca4r54b.deadline.opening-brief"}, date(2026, 4, 29)},
        std::pair{std::string_view{"ca4r54b.deadline.response-brief"}, date(2026, 5, 29)},
        std::pair{std::string_view{"ca4r54b.deadline.rehearing"}, date(2026, 6, 22)},
        std::pair{std::string_view{"ca4r54b.deadline.mandate"}, date(2026, 6, 29)},
    };
    for (const auto& [id, due] : expected_deadlines) {
        const auto* deadline = deadlineFor(run.state, id);
        QVERIFY(deadline != nullptr);
        QVERIFY(deadline->due_date == due);
    }
    QCOMPARE(deadlineFor(run.state, "ca4r54b.deadline.response-brief")->status,
             model::WorkflowDeadlineStatus::Satisfied);
    QCOMPARE(deadlineFor(run.state, "ca4r54b.deadline.rehearing")->status,
             model::WorkflowDeadlineStatus::Open);
    QCOMPARE(deadlineFor(run.state, "ca4r54b.deadline.mandate")->status,
             model::WorkflowDeadlineStatus::Open);

    for (const auto& event : run.trace) {
        QVERIFY(!eventHeader(event).authority.primary.id.value.empty());
        if (!std::holds_alternative<model::WorkflowStageAdvanced>(event)) {
            continue;
        }
        const auto command =
            std::ranges::find_if(run.journal, [&](const model::WorkflowJournalEntry& entry) {
                return std::visit(
                    [&](const auto& concrete) {
                        return concrete.header.command_id == eventHeader(event).command_id;
                    },
                    entry.command);
            });
        QVERIFY(command != run.journal.end());
        QVERIFY(std::holds_alternative<model::AdvanceWorkflowStage>(command->command));
        const auto& stage_command = std::get<model::AdvanceWorkflowStage>(command->command);
        const auto expected_actor =
            stage_command.header.command_id.value == "ca4r54b.command.advance-judgment"
                ? std::string_view{court_actor}
                : std::string_view{clerk_actor};
        QCOMPARE(stage_command.header.actor_id.value, std::string(expected_actor));
    }

    const auto replayed = engine::replayWorkflow(runtime_case.workflow, runtime_case.definition,
                                                 run.initial_state, run.journal);
    QVERIFY2(replayed.has_value(), replayed ? "" : replayed.error().message.c_str());
    QVERIFY(*replayed == run.state);
    const auto replayed_again = engine::replayWorkflow(
        runtime_case.workflow, runtime_case.definition, run.initial_state, run.journal);
    QVERIFY2(replayed_again.has_value(),
             replayed_again ? "" : replayed_again.error().message.c_str());
    QVERIFY(*replayed_again == *replayed);
}

} // namespace

QTEST_APPLESS_MAIN(GoldCaseTraceTest)

#include "tst_gold_case_trace.moc"
