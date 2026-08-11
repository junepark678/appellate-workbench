#include "workflow_session_controller.hpp"

#include "resolved_session_pins.hpp"

#include "appellate/engine/workflow_engine.hpp"
#include "appellate/packs/runtime_pack.hpp"
#include "appellate/storage/workflow_codec.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

namespace appellate::app {
namespace {

constexpr std::size_t maximum_journal_entries = 4096;
constexpr std::size_t maximum_events_per_command = 3;
constexpr std::size_t maximum_workflow_events =
    maximum_journal_entries * maximum_events_per_command;

[[nodiscard]] auto fail(WorkflowSessionErrorCode code, QString message)
    -> std::unexpected<WorkflowSessionError> {
    return std::unexpected(WorkflowSessionError{code, std::move(message)});
}

[[nodiscard]] auto authorityContractFor(const model::WorkflowDefinition& workflow)
    -> std::optional<storage::SessionAuthorityContract> {
    bool saw_legacy{};
    bool saw_canonical{};
    const auto observe = [&](const model::AuthorityRef& authority) {
        if (authority.provenance.has_value()) {
            saw_canonical = true;
        } else {
            saw_legacy = true;
        }
    };
    for (const auto& operation : workflow.operations) {
        observe(operation.authority.primary);
        for (const auto& supporting : operation.authority.supporting) {
            observe(supporting);
        }
    }
    if (saw_legacy == saw_canonical) {
        return std::nullopt;
    }
    return saw_canonical ? storage::SessionAuthorityContract::CanonicalV2
                         : storage::SessionAuthorityContract::LegacyV1;
}

[[nodiscard]] QString asQString(const std::string& value) { return QString::fromStdString(value); }

[[nodiscard]] bool validText(const QString& value) {
    return !value.isEmpty() && value.size() <= 512 && !value.contains(QChar::Null);
}

[[nodiscard]] bool validDigest(const QString& value) {
    return value.size() == 64 && std::ranges::all_of(value, [](QChar character) {
               return (character >= u'0' && character <= u'9') ||
                      (character >= u'a' && character <= u'f');
           });
}

[[nodiscard]] bool validPackId(const QString& value) {
    static const QRegularExpression pattern(
        QStringLiteral(R"(^[a-z0-9]+(?:[.-][a-z0-9]+)+(?:[-.][a-z0-9]+)*$)"));
    return value.size() >= 3 && value.size() <= 128 && pattern.match(value).hasMatch();
}

[[nodiscard]] bool validSemanticVersion(const QString& value) {
    static const QRegularExpression pattern(QStringLiteral(
        R"(^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)(?:\.(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*))*)?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$)"));
    return value.size() >= 5 && value.size() <= 128 && pattern.match(value).hasMatch();
}

[[nodiscard]] bool validCanonicalUtc(const QString& value) {
    if (value.size() != 20 || !value.endsWith(u'Z')) {
        return false;
    }
    const auto parsed = QDateTime::fromString(value, Qt::ISODate);
    return parsed.isValid() && parsed.offsetFromUtc() == 0 &&
           parsed.toUTC().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'")) == value;
}

[[nodiscard]] auto normalizePins(std::vector<storage::RevisionPin> pins,
                                 WorkflowSessionErrorCode error_code)
    -> std::expected<std::vector<storage::RevisionPin>, WorkflowSessionError> {
    if (pins.empty() || pins.size() > 128) {
        return fail(error_code, QStringLiteral("A bounded nonempty revision-pin set is required"));
    }
    for (const auto& pin : pins) {
        if (!validPackId(pin.pack_id) || !validSemanticVersion(pin.version) ||
            !validDigest(pin.digest)) {
            return fail(error_code, QStringLiteral("A workflow revision pin is invalid"));
        }
    }
    std::ranges::sort(
        pins, [](const auto& left, const auto& right) { return left.pack_id < right.pack_id; });
    if (std::ranges::adjacent_find(pins, {}, &storage::RevisionPin::pack_id) != pins.end()) {
        return fail(error_code, QStringLiteral("Workflow revision pins contain a duplicate pack"));
    }
    return pins;
}

[[nodiscard]] auto commandHeader(const model::WorkflowCommand& command)
    -> const model::WorkflowCommandHeader& {
    return std::visit(
        [](const auto& concrete) -> const model::WorkflowCommandHeader& { return concrete.header; },
        command);
}

[[nodiscard]] auto eventHeader(const model::WorkflowEvent& event)
    -> const model::WorkflowEventHeader& {
    return std::visit(
        [](const auto& concrete) -> const model::WorkflowEventHeader& { return concrete.header; },
        event);
}

struct DocumentRequirement final {
    QString digest;
    QString purpose;
};

[[nodiscard]] auto documentRequirement(const model::WorkflowCommand& command)
    -> std::optional<DocumentRequirement> {
    return std::visit(
        [](const auto& concrete) -> std::optional<DocumentRequirement> {
            using Command = std::remove_cvref_t<decltype(concrete)>;
            if constexpr (std::same_as<Command, model::SubmitWorkflowFiling>) {
                return DocumentRequirement{asQString(concrete.document_sha256),
                                           QStringLiteral("workflow.filing-document")};
            } else if constexpr (std::same_as<Command, model::EnterWorkflowOrder>) {
                return DocumentRequirement{asQString(concrete.document_sha256),
                                           QStringLiteral("workflow.order-document")};
            } else if constexpr (std::same_as<Command, model::IssueWorkflowJudgment>) {
                return DocumentRequirement{asQString(concrete.document_sha256),
                                           QStringLiteral("workflow.judgment-document")};
            } else if constexpr (std::same_as<Command, model::IssueWorkflowMandate>) {
                return DocumentRequirement{asQString(concrete.document_sha256),
                                           QStringLiteral("workflow.mandate-document")};
            } else {
                return std::nullopt;
            }
        },
        command);
}

[[nodiscard]] QString legalDateString(model::LegalDate date) {
    return QStringLiteral("%1-%2-%3")
        .arg(static_cast<int>(date.value.year()), 4, 10, QLatin1Char('0'))
        .arg(static_cast<unsigned>(date.value.month()), 2, 10, QLatin1Char('0'))
        .arg(static_cast<unsigned>(date.value.day()), 2, 10, QLatin1Char('0'));
}

[[nodiscard]] QString rejectionStatus(model::WorkflowFilingRejectionReason reason) {
    switch (reason) {
    case model::WorkflowFilingRejectionReason::UnauthorizedActor:
        return QStringLiteral("rejected-unauthorized");
    case model::WorkflowFilingRejectionReason::IneligibleFiling:
        return QStringLiteral("rejected-ineligible");
    case model::WorkflowFilingRejectionReason::NonconformingFiling:
        return QStringLiteral("rejected-nonconforming");
    case model::WorkflowFilingRejectionReason::DeadlineExpired:
        return QStringLiteral("rejected-late");
    case model::WorkflowFilingRejectionReason::UnknownDeficiency:
        return QStringLiteral("rejected-unknown-deficiency");
    }
    return QStringLiteral("rejected-invalid");
}

[[nodiscard]] QString orderStatus(model::WorkflowOrderDisposition disposition) {
    switch (disposition) {
    case model::WorkflowOrderDisposition::Granted:
        return QStringLiteral("granted");
    case model::WorkflowOrderDisposition::Denied:
        return QStringLiteral("denied");
    case model::WorkflowOrderDisposition::Other:
        return QStringLiteral("entered");
    }
    return QStringLiteral("invalid");
}

[[nodiscard]] storage::DocketWrite docketWrite(const model::WorkflowEvent& event,
                                               qsizetype event_offset) {
    const auto sequence = eventHeader(event).sequence;
    const auto entry_id = QStringLiteral("workflow-event.%1").arg(sequence);
    return std::visit(
        [&](const auto& concrete) -> storage::DocketWrite {
            using Event = std::remove_cvref_t<decltype(concrete)>;
            if constexpr (std::same_as<Event, model::WorkflowFilingAccepted>) {
                return {
                    entry_id, event_offset,
                    QStringLiteral("Filing accepted: %1").arg(asQString(concrete.filing_id.value)),
                    QStringLiteral("accepted")};
            } else if constexpr (std::same_as<Event, model::WorkflowFilingRejected>) {
                return {
                    entry_id, event_offset,
                    QStringLiteral("Filing rejected: %1").arg(asQString(concrete.filing_id.value)),
                    rejectionStatus(concrete.reason)};
            } else if constexpr (std::same_as<Event, model::WorkflowDeficiencyIssued>) {
                return {entry_id, event_offset,
                        QStringLiteral("Deficiency issued: %1")
                            .arg(asQString(concrete.deficiency_id.value)),
                        QStringLiteral("open")};
            } else if constexpr (std::same_as<Event, model::WorkflowDeadlineCalculated>) {
                return {entry_id, event_offset,
                        QStringLiteral("Deadline calculated: %1 due %2")
                            .arg(asQString(concrete.deadline_id.value),
                                 legalDateString(concrete.due_date)),
                        QStringLiteral("open")};
            } else if constexpr (std::same_as<Event, model::WorkflowOrderEntered>) {
                return {entry_id, event_offset,
                        QStringLiteral("Order entered: %1").arg(asQString(concrete.order_id.value)),
                        orderStatus(concrete.disposition)};
            } else if constexpr (std::same_as<Event, model::WorkflowStageAdvanced>) {
                return {entry_id, event_offset,
                        QStringLiteral("Stage advanced: %1 to %2")
                            .arg(asQString(concrete.previous_stage_id.value),
                                 asQString(concrete.next_stage_id.value)),
                        QStringLiteral("advanced")};
            } else if constexpr (std::same_as<Event, model::WorkflowSealedSet>) {
                return {entry_id, event_offset,
                        concrete.sealed ? QStringLiteral("Case sealed")
                                        : QStringLiteral("Case unsealed"),
                        concrete.sealed ? QStringLiteral("sealed") : QStringLiteral("unsealed")};
            } else if constexpr (std::same_as<Event, model::WorkflowArgumentScheduled>) {
                return {entry_id, event_offset,
                        QStringLiteral("Argument scheduled for %1")
                            .arg(legalDateString(concrete.argument_date)),
                        QStringLiteral("scheduled")};
            } else if constexpr (std::same_as<Event, model::WorkflowJudgmentIssued>) {
                return {entry_id, event_offset, QStringLiteral("Judgment issued"),
                        QStringLiteral("issued")};
            } else {
                return {entry_id, event_offset, QStringLiteral("Mandate issued"),
                        QStringLiteral("issued")};
            }
        },
        event);
}

[[nodiscard]] storage::DocketEntry projectedDocketEntry(const model::WorkflowEvent& event) {
    const auto write = docketWrite(event, 0);
    return storage::DocketEntry{write.entry_id, static_cast<qint64>(eventHeader(event).sequence),
                                write.title, write.status};
}

[[nodiscard]] bool assetReferenceLess(const storage::AssetReference& left,
                                      const storage::AssetReference& right) {
    if (left.purpose != right.purpose) {
        return left.purpose < right.purpose;
    }
    return left.digest < right.digest;
}

[[nodiscard]] bool isCanonicalCleanState(const model::WorkflowDefinition& workflow,
                                         const model::WorkflowState& state) {
    return !state.session_id.empty() && state.workflow_id == workflow.id &&
           state.current_stage_id == workflow.initial_stage_id && state.next_event_sequence == 1 &&
           !state.pending_command.has_value() && state.decided_commands.empty() &&
           state.accepted_filings.empty() && state.deadlines.empty() &&
           state.deficiencies.empty() && state.orders.empty() && !state.sealed &&
           !state.argument_date.has_value() && !state.judgment_sha256.has_value() &&
           !state.mandate_sha256.has_value() && !state.legal_time_cursor.has_value() &&
           !state.judgment_disposition.has_value();
}

[[nodiscard]] auto validateInitialConfiguration(const model::WorkflowDefinition& workflow,
                                                const model::CaseDefinition& case_definition,
                                                const model::WorkflowState& initial_state)
    -> std::expected<void, WorkflowSessionError> {
    if (!isCanonicalCleanState(workflow, initial_state)) {
        return fail(WorkflowSessionErrorCode::InvalidConfiguration,
                    QStringLiteral("Workflow sessions require a canonical clean initial state"));
    }
    const std::span<const model::WorkflowJournalEntry> empty_journal;
    const auto replayed =
        engine::replayWorkflow(workflow, case_definition, initial_state, empty_journal);
    if (!replayed || *replayed != initial_state) {
        return fail(WorkflowSessionErrorCode::InvalidConfiguration,
                    QStringLiteral("The workflow, case, and initial state are inconsistent%1")
                        .arg(replayed ? QString{}
                                      : QStringLiteral(": %1").arg(
                                            QString::fromStdString(replayed.error().message))));
    }
    return {};
}

struct RestoredWorkflow final {
    model::WorkflowState state;
    std::vector<model::WorkflowJournalEntry> journal;
};

[[nodiscard]] auto
restoreSnapshot(const model::WorkflowDefinition& workflow,
                const model::CaseDefinition& case_definition,
                const model::WorkflowState& initial_state, const storage::SessionSnapshot& snapshot,
                const storage::AssetStore& asset_store, const QString& expected_engine_revision,
                const std::vector<storage::RevisionPin>& expected_pins,
                storage::SessionAuthorityContract expected_authority_contract)
    -> std::expected<RestoredWorkflow, WorkflowSessionError> {
    if (snapshot.session_id != asQString(initial_state.session_id) ||
        snapshot.engine_revision != expected_engine_revision || snapshot.pins != expected_pins ||
        snapshot.authority_contract != expected_authority_contract) {
        return fail(WorkflowSessionErrorCode::CorruptSession,
                    QStringLiteral("Stored session identity, engine, revision pins, or authority "
                                   "contract differ"));
    }
    if (snapshot.sequence < 0 || snapshot.sequence > static_cast<qint64>(maximum_workflow_events) ||
        snapshot.commands.size() > maximum_journal_entries ||
        snapshot.events.size() > maximum_workflow_events ||
        snapshot.sequence != static_cast<qint64>(snapshot.events.size())) {
        return fail(WorkflowSessionErrorCode::CorruptSession,
                    QStringLiteral("Stored workflow history is unbounded or inconsistent"));
    }
    if (snapshot.commands.empty()) {
        if (snapshot.sequence != 0 || !snapshot.events.empty() || !snapshot.docket.empty() ||
            !snapshot.asset_references.empty()) {
            return fail(WorkflowSessionErrorCode::CorruptSession,
                        QStringLiteral("A pristine workflow session contains derived history"));
        }
        return RestoredWorkflow{initial_state, {}};
    }
    if (snapshot.commands.front().expected_sequence != 0) {
        return fail(WorkflowSessionErrorCode::CorruptSession,
                    QStringLiteral("The first workflow command does not start at sequence zero"));
    }

    QSet<QString> command_ids;
    std::vector<model::WorkflowJournalEntry> journal;
    journal.reserve(snapshot.commands.size());
    std::vector<storage::DocketEntry> expected_docket;
    expected_docket.reserve(snapshot.events.size());
    std::vector<storage::AssetReference> expected_asset_references;

    for (std::size_t command_index = 0; command_index < snapshot.commands.size(); ++command_index) {
        const auto& stored_command = snapshot.commands[command_index];
        const auto group_start = stored_command.expected_sequence;
        const auto group_end = command_index + 1 < snapshot.commands.size()
                                   ? snapshot.commands[command_index + 1].expected_sequence
                                   : snapshot.sequence;
        if (group_start < 0 || group_end <= group_start ||
            group_end - group_start > static_cast<qint64>(maximum_events_per_command) ||
            group_end > snapshot.sequence || !validCanonicalUtc(stored_command.recorded_at_utc) ||
            !validText(stored_command.command_id) ||
            command_ids.contains(stored_command.command_id)) {
            return fail(WorkflowSessionErrorCode::CorruptSession,
                        QStringLiteral("Stored workflow command grouping is invalid"));
        }
        command_ids.insert(stored_command.command_id);

        const auto decoded_command = storage::decodeWorkflowCommand(stored_command.payload_json);
        if (!decoded_command) {
            return fail(WorkflowSessionErrorCode::CommandCodecFailure,
                        QStringLiteral("Cannot decode stored workflow command: %1")
                            .arg(decoded_command.error().message));
        }
        const auto canonical_command = storage::encodeWorkflowCommand(*decoded_command);
        if (!canonical_command || *canonical_command != stored_command.payload_json) {
            return fail(WorkflowSessionErrorCode::CorruptSession,
                        QStringLiteral("Stored workflow command is not canonically encoded"));
        }
        const auto& decoded_header = commandHeader(*decoded_command);
        if (asQString(decoded_header.session_id) != snapshot.session_id ||
            asQString(decoded_header.command_id.value) != stored_command.command_id) {
            return fail(
                WorkflowSessionErrorCode::CorruptSession,
                QStringLiteral("Stored workflow command metadata differs from its payload"));
        }

        if (const auto document = documentRequirement(*decoded_command); document.has_value()) {
            expected_asset_references.push_back(
                storage::AssetReference{document->digest, document->purpose});
        }

        const auto event_count = static_cast<std::uint32_t>(group_end - group_start);
        std::vector<model::WorkflowEvent> group;
        group.reserve(event_count);
        for (qint64 sequence = group_start + 1; sequence <= group_end; ++sequence) {
            const auto event_position = static_cast<std::size_t>(sequence - 1);
            if (event_position >= snapshot.events.size()) {
                return fail(WorkflowSessionErrorCode::CorruptSession,
                            QStringLiteral("A workflow command references a missing event"));
            }
            const auto& stored_event = snapshot.events[event_position];
            if (stored_event.sequence != sequence) {
                return fail(WorkflowSessionErrorCode::CorruptSession,
                            QStringLiteral("Stored workflow event sequence is not contiguous"));
            }
            const auto decoded_event = storage::decodeWorkflowEvent(stored_event.payload_json);
            if (!decoded_event) {
                return fail(WorkflowSessionErrorCode::EventCodecFailure,
                            QStringLiteral("Cannot decode stored workflow event %1: %2")
                                .arg(sequence)
                                .arg(decoded_event.error().message));
            }
            const auto canonical_event = storage::encodeWorkflowEvent(*decoded_event);
            if (!canonical_event || *canonical_event != stored_event.payload_json) {
                return fail(WorkflowSessionErrorCode::CorruptSession,
                            QStringLiteral("Stored workflow event is not canonically encoded"));
            }
            const auto& decoded_event_header = eventHeader(*decoded_event);
            const auto local_index = static_cast<std::uint32_t>(sequence - group_start - 1);
            if (storage::workflowEventType(*decoded_event) != stored_event.event_type ||
                storage::workflowPrimaryAuthorityId(*decoded_event) != stored_event.authority_id ||
                decoded_event_header.session_id != initial_state.session_id ||
                decoded_event_header.workflow_id != workflow.id ||
                decoded_event_header.command_id != decoded_header.command_id ||
                decoded_event_header.sequence != static_cast<std::uint64_t>(sequence) ||
                decoded_event_header.command_event_index != local_index ||
                decoded_event_header.command_event_count != event_count) {
                return fail(
                    WorkflowSessionErrorCode::CorruptSession,
                    QStringLiteral("Stored workflow event metadata differs from its payload"));
            }
            expected_docket.push_back(projectedDocketEntry(*decoded_event));
            group.push_back(*decoded_event);
        }
        journal.push_back(model::WorkflowJournalEntry{*decoded_command, std::move(group)});
    }

    std::ranges::sort(expected_asset_references, assetReferenceLess);
    expected_asset_references.erase(std::ranges::unique(expected_asset_references).begin(),
                                    expected_asset_references.end());
    if (snapshot.docket != expected_docket ||
        snapshot.asset_references != expected_asset_references) {
        return fail(WorkflowSessionErrorCode::CorruptSession,
                    QStringLiteral("Stored docket or asset projection differs from the journal"));
    }
    for (const auto& reference : expected_asset_references) {
        const auto restored_asset = asset_store.read(reference.digest);
        if (!restored_asset) {
            return fail(WorkflowSessionErrorCode::CorruptSession,
                        QStringLiteral("Referenced workflow asset %1 is missing or corrupt: %2")
                            .arg(reference.digest, restored_asset.error().message));
        }
    }

    const auto replayed = engine::replayWorkflow(workflow, case_definition, initial_state, journal);
    if (!replayed) {
        return fail(WorkflowSessionErrorCode::EngineFailure,
                    QStringLiteral("Stored workflow journal cannot be replayed: %1")
                        .arg(QString::fromStdString(replayed.error().message)));
    }
    if (replayed->next_event_sequence !=
        static_cast<std::uint64_t>(snapshot.sequence) + std::uint64_t{1}) {
        return fail(WorkflowSessionErrorCode::CorruptSession,
                    QStringLiteral("Replayed workflow sequence differs from storage"));
    }
    return RestoredWorkflow{*replayed, std::move(journal)};
}

} // namespace

WorkflowSessionController::WorkflowSessionController(
    model::WorkflowDefinition workflow, model::CaseDefinition case_definition,
    model::WorkflowState initial_state, model::WorkflowState state,
    std::vector<model::WorkflowJournalEntry> journal, storage::AssetStore asset_store,
    std::unique_ptr<storage::SessionStore> session_store, QString engine_revision,
    std::vector<storage::RevisionPin> pins, storage::SessionSnapshot snapshot)
    : workflow_(std::move(workflow)), case_definition_(std::move(case_definition)),
      initial_state_(std::move(initial_state)), state_(std::move(state)),
      journal_(std::move(journal)), asset_store_(std::move(asset_store)),
      session_store_(std::move(session_store)), engine_revision_(std::move(engine_revision)),
      pins_(std::move(pins)), snapshot_(std::move(snapshot)) {}

std::expected<std::unique_ptr<WorkflowSessionController>, WorkflowSessionError>
WorkflowSessionController::create(model::WorkflowDefinition workflow,
                                  model::CaseDefinition case_definition,
                                  model::WorkflowState initial_state,
                                  storage::AssetStore asset_store,
                                  std::unique_ptr<storage::SessionStore> session_store,
                                  QString engine_revision, QString created_at_utc,
                                  std::vector<storage::RevisionPin> pins) {
    return createBound(std::move(workflow), std::move(case_definition), std::move(initial_state),
                       std::move(asset_store), std::move(session_store), std::move(engine_revision),
                       std::move(created_at_utc), std::move(pins),
                       storage::SessionAuthorityContract::LegacyV1);
}

std::expected<std::unique_ptr<WorkflowSessionController>, WorkflowSessionError>
WorkflowSessionController::createBound(model::WorkflowDefinition workflow,
                                       model::CaseDefinition case_definition,
                                       model::WorkflowState initial_state,
                                       storage::AssetStore asset_store,
                                       std::unique_ptr<storage::SessionStore> session_store,
                                       QString engine_revision, QString created_at_utc,
                                       std::vector<storage::RevisionPin> pins,
                                       storage::SessionAuthorityContract authority_contract) {
    if (!session_store || !validText(engine_revision) || !validCanonicalUtc(created_at_utc)) {
        return fail(
            WorkflowSessionErrorCode::InvalidConfiguration,
            QStringLiteral(
                "Session store, engine revision, and canonical creation time are required"));
    }
    const auto detected_authority_contract = authorityContractFor(workflow);
    if (!detected_authority_contract.has_value() ||
        *detected_authority_contract != authority_contract) {
        return fail(WorkflowSessionErrorCode::InvalidConfiguration,
                    QStringLiteral("The workflow authority contract does not match its session "
                                   "entry point"));
    }
    const auto normalized_pins =
        normalizePins(std::move(pins), WorkflowSessionErrorCode::InvalidConfiguration);
    if (!normalized_pins) {
        return std::unexpected(normalized_pins.error());
    }
    if (const auto valid = validateInitialConfiguration(workflow, case_definition, initial_state);
        !valid) {
        return std::unexpected(valid.error());
    }

    const auto session_id = asQString(initial_state.session_id);
    const auto created = session_store->createSession(session_id, engine_revision, created_at_utc,
                                                      *normalized_pins, authority_contract);
    if (!created) {
        return fail(WorkflowSessionErrorCode::SessionStoreFailure, created.error().message);
    }
    const auto loaded = session_store->loadSession(session_id);
    if (!loaded) {
        return fail(WorkflowSessionErrorCode::SessionStoreFailure, loaded.error().message);
    }
    const auto restored =
        restoreSnapshot(workflow, case_definition, initial_state, *loaded, asset_store,
                        engine_revision, *normalized_pins, authority_contract);
    if (!restored) {
        return std::unexpected(restored.error());
    }

    return std::unique_ptr<WorkflowSessionController>(new WorkflowSessionController(
        std::move(workflow), std::move(case_definition), std::move(initial_state), restored->state,
        restored->journal, std::move(asset_store), std::move(session_store),
        std::move(engine_revision), *normalized_pins, *loaded));
}

std::expected<std::unique_ptr<WorkflowSessionController>, WorkflowSessionError>
WorkflowSessionController::create(model::WorkflowDefinition workflow,
                                  model::CaseDefinition case_definition,
                                  model::WorkflowState initial_state,
                                  storage::AssetStore asset_store,
                                  std::unique_ptr<storage::SessionStore> session_store,
                                  QString engine_revision, QString created_at_utc,
                                  const packs::ResolvedPack& resolved_pack) {
    if (resolved_pack.root().manifest_schema_version != 1) {
        return fail(WorkflowSessionErrorCode::InvalidConfiguration,
                    QStringLiteral("Schema-v2 workflows must be selected by case ID from their "
                                   "resolved pack"));
    }
    return create(std::move(workflow), std::move(case_definition), std::move(initial_state),
                  std::move(asset_store), std::move(session_store), std::move(engine_revision),
                  std::move(created_at_utc), revisionPinsForSession(resolved_pack));
}

std::expected<std::unique_ptr<WorkflowSessionController>, WorkflowSessionError>
WorkflowSessionController::create(model::CaseId case_id, model::WorkflowState initial_state,
                                  storage::AssetStore asset_store,
                                  std::unique_ptr<storage::SessionStore> session_store,
                                  QString engine_revision, QString created_at_utc,
                                  const packs::ResolvedPack& resolved_pack) {
    const auto runtime = packs::loadRuntimePack(resolved_pack);
    if (!runtime) {
        return fail(WorkflowSessionErrorCode::InvalidConfiguration,
                    QStringLiteral("Cannot project the resolved pack: %1")
                        .arg(QString::fromStdString(runtime.error().message)));
    }
    const auto selected = std::ranges::find(
        runtime->cases, case_id, [](const auto& candidate) { return candidate.definition.id; });
    if (selected == runtime->cases.end()) {
        return fail(WorkflowSessionErrorCode::InvalidConfiguration,
                    QStringLiteral("The resolved pack does not contain the selected case"));
    }
    const auto authority_contract = authorityContractFor(selected->workflow);
    if (!authority_contract.has_value()) {
        return fail(WorkflowSessionErrorCode::InvalidConfiguration,
                    QStringLiteral("The resolved workflow mixes authority contracts"));
    }
    return createBound(selected->workflow, selected->definition, std::move(initial_state),
                       std::move(asset_store), std::move(session_store), std::move(engine_revision),
                       std::move(created_at_utc), revisionPinsForSession(resolved_pack),
                       *authority_contract);
}

std::expected<std::unique_ptr<WorkflowSessionController>, WorkflowSessionError>
WorkflowSessionController::reopen(model::WorkflowDefinition workflow,
                                  model::CaseDefinition case_definition,
                                  model::WorkflowState initial_state,
                                  storage::AssetStore asset_store,
                                  std::unique_ptr<storage::SessionStore> session_store,
                                  QString expected_engine_revision,
                                  std::vector<storage::RevisionPin> expected_pins) {
    return reopenBound(std::move(workflow), std::move(case_definition), std::move(initial_state),
                       std::move(asset_store), std::move(session_store),
                       std::move(expected_engine_revision), std::move(expected_pins),
                       storage::SessionAuthorityContract::LegacyV1);
}

std::expected<std::unique_ptr<WorkflowSessionController>, WorkflowSessionError>
WorkflowSessionController::reopenBound(model::WorkflowDefinition workflow,
                                       model::CaseDefinition case_definition,
                                       model::WorkflowState initial_state,
                                       storage::AssetStore asset_store,
                                       std::unique_ptr<storage::SessionStore> session_store,
                                       QString expected_engine_revision,
                                       std::vector<storage::RevisionPin> expected_pins,
                                       storage::SessionAuthorityContract authority_contract) {
    if (!session_store || !validText(expected_engine_revision)) {
        return fail(WorkflowSessionErrorCode::InvalidConfiguration,
                    QStringLiteral("Session store and expected engine revision are required"));
    }
    const auto detected_authority_contract = authorityContractFor(workflow);
    if (!detected_authority_contract.has_value() ||
        *detected_authority_contract != authority_contract) {
        return fail(WorkflowSessionErrorCode::InvalidConfiguration,
                    QStringLiteral("The workflow authority contract does not match its session "
                                   "entry point"));
    }
    const auto normalized_pins =
        normalizePins(std::move(expected_pins), WorkflowSessionErrorCode::InvalidConfiguration);
    if (!normalized_pins) {
        return std::unexpected(normalized_pins.error());
    }
    if (const auto valid = validateInitialConfiguration(workflow, case_definition, initial_state);
        !valid) {
        return std::unexpected(valid.error());
    }

    const auto loaded = session_store->loadSession(asQString(initial_state.session_id));
    if (!loaded) {
        return fail(WorkflowSessionErrorCode::SessionStoreFailure, loaded.error().message);
    }
    const auto restored =
        restoreSnapshot(workflow, case_definition, initial_state, *loaded, asset_store,
                        expected_engine_revision, *normalized_pins, authority_contract);
    if (!restored) {
        return std::unexpected(restored.error());
    }

    return std::unique_ptr<WorkflowSessionController>(new WorkflowSessionController(
        std::move(workflow), std::move(case_definition), std::move(initial_state), restored->state,
        restored->journal, std::move(asset_store), std::move(session_store),
        std::move(expected_engine_revision), *normalized_pins, *loaded));
}

std::expected<std::unique_ptr<WorkflowSessionController>, WorkflowSessionError>
WorkflowSessionController::reopen(model::WorkflowDefinition workflow,
                                  model::CaseDefinition case_definition,
                                  model::WorkflowState initial_state,
                                  storage::AssetStore asset_store,
                                  std::unique_ptr<storage::SessionStore> session_store,
                                  QString expected_engine_revision,
                                  const packs::ResolvedPack& resolved_pack) {
    if (resolved_pack.root().manifest_schema_version != 1) {
        return fail(WorkflowSessionErrorCode::InvalidConfiguration,
                    QStringLiteral("Schema-v2 workflows must be selected by case ID from their "
                                   "resolved pack"));
    }
    return reopen(std::move(workflow), std::move(case_definition), std::move(initial_state),
                  std::move(asset_store), std::move(session_store),
                  std::move(expected_engine_revision), revisionPinsForSession(resolved_pack));
}

std::expected<std::unique_ptr<WorkflowSessionController>, WorkflowSessionError>
WorkflowSessionController::reopen(model::CaseId case_id, model::WorkflowState initial_state,
                                  storage::AssetStore asset_store,
                                  std::unique_ptr<storage::SessionStore> session_store,
                                  QString expected_engine_revision,
                                  const packs::ResolvedPack& resolved_pack) {
    const auto runtime = packs::loadRuntimePack(resolved_pack);
    if (!runtime) {
        return fail(WorkflowSessionErrorCode::InvalidConfiguration,
                    QStringLiteral("Cannot project the resolved pack: %1")
                        .arg(QString::fromStdString(runtime.error().message)));
    }
    const auto selected = std::ranges::find(
        runtime->cases, case_id, [](const auto& candidate) { return candidate.definition.id; });
    if (selected == runtime->cases.end()) {
        return fail(WorkflowSessionErrorCode::InvalidConfiguration,
                    QStringLiteral("The resolved pack does not contain the selected case"));
    }
    const auto authority_contract = authorityContractFor(selected->workflow);
    if (!authority_contract.has_value()) {
        return fail(WorkflowSessionErrorCode::InvalidConfiguration,
                    QStringLiteral("The resolved workflow mixes authority contracts"));
    }
    return reopenBound(selected->workflow, selected->definition, std::move(initial_state),
                       std::move(asset_store), std::move(session_store),
                       std::move(expected_engine_revision), revisionPinsForSession(resolved_pack),
                       *authority_contract);
}

std::expected<WorkflowSubmissionResult, WorkflowSessionError>
WorkflowSessionController::submit(const model::WorkflowCommand& command,
                                  std::optional<QByteArrayView> document_bytes,
                                  const QString& recorded_at_utc) {
    if (!validCanonicalUtc(recorded_at_utc)) {
        return fail(WorkflowSessionErrorCode::InvalidConfiguration,
                    QStringLiteral("A canonical UTC recorded time is required"));
    }
    const auto document = documentRequirement(command);
    if (document.has_value() && (!document_bytes.has_value() || document_bytes->empty())) {
        return fail(WorkflowSessionErrorCode::UnexpectedDocument,
                    QStringLiteral("This workflow command requires nonempty document bytes"));
    }
    if (!document.has_value() && document_bytes.has_value()) {
        return fail(WorkflowSessionErrorCode::UnexpectedDocument,
                    QStringLiteral("This workflow command does not accept document bytes"));
    }

    const auto encoded_command = storage::encodeWorkflowCommand(command);
    if (!encoded_command) {
        return fail(WorkflowSessionErrorCode::CommandCodecFailure,
                    QStringLiteral("Cannot encode workflow command: %1")
                        .arg(encoded_command.error().message));
    }
    const auto decided = engine::decideWorkflow(workflow_, case_definition_, state_, command);
    if (!decided) {
        return fail(WorkflowSessionErrorCode::EngineFailure,
                    QString::fromStdString(decided.error().message));
    }

    auto candidate_journal = journal_;
    candidate_journal.push_back(model::WorkflowJournalEntry{command, *decided});
    const auto next_state =
        engine::replayWorkflow(workflow_, case_definition_, initial_state_, candidate_journal);
    if (!next_state) {
        return fail(WorkflowSessionErrorCode::EngineFailure,
                    QStringLiteral("Candidate workflow journal cannot be replayed: %1")
                        .arg(QString::fromStdString(next_state.error().message)));
    }

    storage::CommitBatch batch;
    batch.command_id = asQString(commandHeader(command).command_id.value);
    batch.command_json = *encoded_command;
    batch.recorded_at_utc = recorded_at_utc;
    batch.events.reserve(decided->size());
    batch.docket_changes.reserve(decided->size());
    for (std::size_t index = 0; index < decided->size(); ++index) {
        const auto& event = (*decided)[index];
        const auto encoded_event = storage::encodeWorkflowEvent(event);
        if (!encoded_event) {
            return fail(WorkflowSessionErrorCode::EventCodecFailure,
                        QStringLiteral("Cannot encode workflow event: %1")
                            .arg(encoded_event.error().message));
        }
        batch.events.push_back(storage::EventWrite{storage::workflowEventType(event),
                                                   *encoded_event,
                                                   storage::workflowPrimaryAuthorityId(event)});
        batch.docket_changes.push_back(docketWrite(event, static_cast<qsizetype>(index)));
    }

    std::optional<storage::StoredAsset> stored_asset;
    if (document.has_value()) {
        const auto supplied_digest = QString::fromLatin1(
            QCryptographicHash::hash(*document_bytes, QCryptographicHash::Sha256).toHex());
        if (supplied_digest != document->digest) {
            return fail(WorkflowSessionErrorCode::DocumentDigestMismatch,
                        QStringLiteral("Command document digest does not match its bytes"));
        }
        const auto put = asset_store_.put(*document_bytes);
        if (!put) {
            return fail(WorkflowSessionErrorCode::AssetStoreFailure, put.error().message);
        }
        if (put->sha256 != supplied_digest) {
            return fail(WorkflowSessionErrorCode::DocumentDigestMismatch,
                        QStringLiteral("Stored document digest differs from its verified bytes"));
        }
        stored_asset = *put;
        const storage::AssetReference reference{put->sha256, document->purpose};
        if (std::ranges::find(snapshot_.asset_references, reference) ==
            snapshot_.asset_references.end()) {
            batch.asset_references.push_back(reference);
        }
    }

    const auto appended =
        session_store_->append(asQString(initial_state_.session_id), snapshot_.sequence, batch);
    if (!appended) {
        return fail(WorkflowSessionErrorCode::SessionStoreFailure, appended.error().message);
    }
    const auto loaded = session_store_->loadSession(asQString(initial_state_.session_id));
    if (!loaded) {
        return fail(WorkflowSessionErrorCode::SessionStoreFailure, loaded.error().message);
    }
    if (loaded->sequence != *appended) {
        return fail(WorkflowSessionErrorCode::CorruptSession,
                    QStringLiteral("Persisted workflow sequence differs from append result"));
    }
    const auto restored =
        restoreSnapshot(workflow_, case_definition_, initial_state_, *loaded, asset_store_,
                        engine_revision_, pins_, snapshot_.authority_contract);
    if (!restored) {
        return std::unexpected(restored.error());
    }
    if (restored->state != *next_state || restored->journal != candidate_journal) {
        return fail(WorkflowSessionErrorCode::CorruptSession,
                    QStringLiteral("Persisted workflow differs from the submitted journal"));
    }

    state_ = restored->state;
    journal_ = restored->journal;
    snapshot_ = *loaded;
    return WorkflowSubmissionResult{std::move(stored_asset), *decided, *appended};
}

const model::WorkflowState& WorkflowSessionController::initialState() const noexcept {
    return initial_state_;
}

const model::WorkflowState& WorkflowSessionController::state() const noexcept { return state_; }

const std::vector<model::WorkflowJournalEntry>&
WorkflowSessionController::journal() const noexcept {
    return journal_;
}

const storage::SessionSnapshot& WorkflowSessionController::snapshot() const noexcept {
    return snapshot_;
}

} // namespace appellate::app
