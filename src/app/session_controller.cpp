#include "session_controller.hpp"

#include "resolved_session_pins.hpp"

#include "appellate/engine/procedure_engine.hpp"
#include "appellate/packs/runtime_pack.hpp"
#include "appellate/storage/event_codec.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <chrono>
#include <optional>
#include <ranges>
#include <string>
#include <type_traits>
#include <utility>

namespace appellate::app {
namespace {

[[nodiscard]] auto fail(SessionControllerErrorCode code, QString message)
    -> std::unexpected<SessionControllerError> {
    return std::unexpected(SessionControllerError{code, std::move(message)});
}

[[nodiscard]] QString asQString(const std::string& value) { return QString::fromStdString(value); }

[[nodiscard]] bool usesCanonicalAuthority(const model::AuthorityBasis& basis) {
    return basis.primary.provenance.has_value() ||
           std::ranges::any_of(basis.supporting, [](const auto& authority) {
               return authority.provenance.has_value();
           });
}

[[nodiscard]] bool usesCanonicalAuthority(const model::ProcedureDefinition& procedure) {
    const auto& filing = procedure.initiating_filing;
    return usesCanonicalAuthority(filing.filing_authority) ||
           usesCanonicalAuthority(filing.actor_authority) ||
           usesCanonicalAuthority(filing.deficiency_authority);
}

[[nodiscard]] bool isPristineInitialState(const model::ProcedureDefinition& procedure,
                                          const model::CaseDefinition& case_definition,
                                          const model::SessionState& state) {
    return !state.id.value.empty() && case_definition.procedure_id == procedure.id &&
           state.procedure_id == procedure.id && state.case_id == case_definition.id &&
           state.phase == model::SessionPhase::AwaitingInitiatingFiling &&
           state.next_docket_sequence == 1 && !state.pending_deficiency.has_value() &&
           !state.accepted_filing.has_value() && state.decided_submissions.empty();
}

[[nodiscard]] QString dateString(model::LegalDate date) {
    const auto value = date.value;
    return QStringLiteral("%1-%2-%3")
        .arg(static_cast<int>(value.year()), 4, 10, QLatin1Char('0'))
        .arg(static_cast<unsigned>(value.month()), 2, 10, QLatin1Char('0'))
        .arg(static_cast<unsigned>(value.day()), 2, 10, QLatin1Char('0'));
}

[[nodiscard]] QByteArray encodeCommand(const model::SubmitFiling& command) {
    QJsonArray fields;
    for (const auto& field : command.fields) {
        fields.push_back(QJsonObject{
            {QStringLiteral("id"), asQString(field.id.value)},
            {QStringLiteral("value"), asQString(field.value)},
        });
    }

    return QJsonDocument(
               QJsonObject{
                   {QStringLiteral("schema_version"), 1},
                   {QStringLiteral("command_type"), QStringLiteral("submit_filing")},
                   {QStringLiteral("session_id"), asQString(command.session_id.value)},
                   {QStringLiteral("submission_id"), asQString(command.submission_id.value)},
                   {QStringLiteral("actor_id"), asQString(command.actor_id.value)},
                   {QStringLiteral("filing_type"), asQString(command.filing_type.value)},
                   {QStringLiteral("submitted_at_epoch_seconds"),
                    QString::number(command.submitted_at.instant.time_since_epoch().count())},
                   {QStringLiteral("court_date"), dateString(command.submitted_at.court_date)},
                   {QStringLiteral("document_sha256"), asQString(command.document_sha256)},
                   {QStringLiteral("fields"), fields},
               })
        .toJson(QJsonDocument::Compact);
}

[[nodiscard]] std::optional<QString> documentDigest(const model::LegalEvent& event) {
    return std::visit(
        [](const auto& concrete) -> std::optional<QString> {
            using Event = std::remove_cvref_t<decltype(concrete)>;
            if constexpr (std::same_as<Event, model::FilingAccepted> ||
                          std::same_as<Event, model::FilingDeficiencyIssued>) {
                return asQString(concrete.document_sha256);
            }
            return std::nullopt;
        },
        event);
}

[[nodiscard]] storage::AssetReference filingDocumentReference(const QString& digest) {
    return storage::AssetReference{digest, QStringLiteral("filing-document")};
}

[[nodiscard]] std::optional<storage::DocketWrite> docketWrite(const model::LegalEvent& event,
                                                              qsizetype event_offset) {
    return std::visit(
        [event_offset](const auto& concrete) -> std::optional<storage::DocketWrite> {
            using Event = std::remove_cvref_t<decltype(concrete)>;
            if constexpr (std::same_as<Event, model::FilingAccepted>) {
                return storage::DocketWrite{
                    QStringLiteral("initiation.%1").arg(asQString(concrete.submission_id.value)),
                    event_offset,
                    QStringLiteral("Initiating filing accepted"),
                    QStringLiteral("accepted"),
                };
            }
            if constexpr (std::same_as<Event, model::FilingDeficiencyIssued>) {
                return storage::DocketWrite{
                    QStringLiteral("initiation.%1").arg(asQString(concrete.submission_id.value)),
                    event_offset,
                    QStringLiteral("Initiating filing deficiency issued"),
                    QStringLiteral("deficient"),
                };
            }
            return std::nullopt;
        },
        event);
}

[[nodiscard]] auto replaySnapshot(const model::ProcedureDefinition& procedure,
                                  const model::CaseDefinition& case_definition,
                                  const model::SessionState& initial_state,
                                  const storage::SessionSnapshot& snapshot,
                                  const storage::AssetStore& asset_store)
    -> std::expected<model::SessionState, SessionControllerError> {
    if (snapshot.session_id != asQString(initial_state.id.value) || snapshot.sequence < 0 ||
        snapshot.sequence != static_cast<qint64>(snapshot.events.size())) {
        return fail(SessionControllerErrorCode::CorruptSession,
                    QStringLiteral("Stored session identity or sequence is inconsistent"));
    }

    std::vector<model::LegalEvent> decoded_events;
    decoded_events.reserve(snapshot.events.size());
    std::vector<storage::DocketEntry> expected_docket;
    for (const auto& reference : snapshot.asset_references) {
        if (!asset_store.read(reference.digest)) {
            return fail(SessionControllerErrorCode::AssetStoreFailure,
                        QStringLiteral("Cannot verify referenced asset %1").arg(reference.digest));
        }
    }
    for (std::size_t index = 0; index < snapshot.events.size(); ++index) {
        const auto& stored = snapshot.events[index];
        const auto expected_sequence = static_cast<qint64>(index) + 1;
        if (stored.sequence != expected_sequence) {
            return fail(SessionControllerErrorCode::CorruptSession,
                        QStringLiteral("Stored event sequence is not contiguous"));
        }

        const auto decoded = storage::decodeEvent(stored.payload_json);
        if (!decoded) {
            return fail(SessionControllerErrorCode::EventCodecFailure,
                        QStringLiteral("Cannot decode stored event %1").arg(stored.sequence));
        }
        if (storage::eventType(*decoded) != stored.event_type ||
            storage::primaryAuthorityId(*decoded) != stored.authority_id) {
            return fail(SessionControllerErrorCode::CorruptSession,
                        QStringLiteral("Stored event metadata does not match its payload"));
        }
        if (const auto digest = documentDigest(*decoded); digest.has_value()) {
            const auto reference = filingDocumentReference(*digest);
            if (std::ranges::find(snapshot.asset_references, reference) ==
                snapshot.asset_references.end()) {
                return fail(SessionControllerErrorCode::CorruptSession,
                            QStringLiteral("Event asset %1 has no session reference").arg(*digest));
            }
        }
        if (const auto docket = docketWrite(*decoded, static_cast<qsizetype>(index));
            docket.has_value()) {
            expected_docket.push_back(storage::DocketEntry{
                docket->entry_id,
                stored.sequence,
                docket->title,
                docket->status,
            });
        }
        decoded_events.push_back(*decoded);
    }

    if (expected_docket != snapshot.docket) {
        return fail(SessionControllerErrorCode::CorruptSession,
                    QStringLiteral("Stored docket projection does not match the event log"));
    }

    const auto replayed = engine::replay(procedure, case_definition, initial_state, decoded_events);
    if (!replayed) {
        return fail(SessionControllerErrorCode::EngineFailure,
                    QStringLiteral("Stored events cannot be replayed: %1")
                        .arg(QString::fromStdString(replayed.error().message)));
    }
    return *replayed;
}

[[nodiscard]] QByteArray encodeRecordAccessCommand(const model::RecordAccessEvent& event) {
    return QJsonDocument(
               QJsonObject{
                   {QStringLiteral("action"), event.action == model::RecordAccessAction::Grant
                                                  ? QStringLiteral("grant")
                                                  : QStringLiteral("revoke")},
                   {QStringLiteral("command_type"), QStringLiteral("record_access_transition")},
                   {QStringLiteral("event_digest"), QString::fromLatin1(event.event_digest)},
                   {QStringLiteral("event_id"), QString::fromUtf8(event.event_id)},
                   {QStringLiteral("policy_id"), QString::fromUtf8(event.policy_id)},
                   {QStringLiteral("record_id"), QString::fromUtf8(event.record_id)},
                   {QStringLiteral("recorded_at_utc"), QString::fromUtf8(event.recorded_at_utc)},
                   {QStringLiteral("sealed_document_id"),
                    QString::fromUtf8(event.sealed_document_id)},
                   {QStringLiteral("schema_version"), 1},
                   {QStringLiteral("session_id"), QString::fromUtf8(event.session_id)},
               })
        .toJson(QJsonDocument::Compact);
}

[[nodiscard]] auto replayRecordAccessSnapshot(const storage::SessionSnapshot& snapshot,
                                              const model::RecordAccessPolicy& policy)
    -> std::expected<model::RecordAccessProjection, SessionControllerError> {
    if (snapshot.sequence < 0 ||
        snapshot.sequence > static_cast<qint64>(storage::maximum_record_access_events) ||
        snapshot.events.size() > storage::maximum_record_access_events ||
        snapshot.commands.size() > storage::maximum_record_access_events ||
        static_cast<std::uint64_t>(snapshot.sequence) != snapshot.events.size() ||
        snapshot.commands.size() != snapshot.events.size() || !snapshot.docket.empty() ||
        !snapshot.asset_references.empty()) {
        return fail(SessionControllerErrorCode::CorruptSession,
                    QStringLiteral("Stored record-access journal shape is inconsistent"));
    }
    for (std::size_t index = 0; index < snapshot.events.size(); ++index) {
        const auto& stored = snapshot.events[index];
        const auto& command = snapshot.commands[index];
        const auto expected_sequence = static_cast<qint64>(index) + 1;
        const auto decoded = storage::decodeRecordAccessEvent(stored.payload_json);
        if (!decoded || stored.sequence != expected_sequence ||
            stored.event_type != storage::recordAccessEventType(decoded->action) ||
            stored.authority_id != QString::fromUtf8(decoded->authority_id) ||
            command.expected_sequence != static_cast<qint64>(index) ||
            command.command_id != QString::fromUtf8(decoded->event_id) ||
            command.recorded_at_utc != QString::fromUtf8(decoded->recorded_at_utc) ||
            command.payload_json != encodeRecordAccessCommand(*decoded)) {
            return fail(SessionControllerErrorCode::CorruptSession,
                        QStringLiteral("Stored record-access command/event pair was altered"));
        }
    }
    const auto projection = storage::projectRecordAccess(snapshot, policy);
    if (!projection) {
        return fail(SessionControllerErrorCode::CorruptSession,
                    QStringLiteral("Stored record-access journal cannot be replayed: %1")
                        .arg(projection.error().message));
    }
    return *projection;
}

[[nodiscard]] auto deriveRecordAccessPolicy(const model::CaseId& selected_case_id,
                                            const packs::ResolvedPack& resolved_pack)
    -> std::expected<model::RecordAccessPolicy, SessionControllerError> {
    const auto runtime = packs::loadRuntimePack(resolved_pack);
    if (!runtime) {
        return fail(SessionControllerErrorCode::InvalidConfiguration,
                    QStringLiteral("Cannot project the resolved record-access pack: %1")
                        .arg(QString::fromStdString(runtime.error().message)));
    }
    const auto selected = std::ranges::find(
        runtime->cases, selected_case_id,
        [](const packs::RuntimeCase& candidate) { return candidate.definition.id; });
    if (selected == runtime->cases.end() || !selected->record.disclosure_policy.has_value() ||
        selected->record.sealed_disclosures.empty()) {
        return fail(SessionControllerErrorCode::InvalidConfiguration,
                    QStringLiteral("The selected root case has no sealed-record access policy"));
    }
    const auto owner = resolved_pack.resourceOwner(selected->record.id.value);
    if (!owner.has_value() || *owner != resolved_pack.root().revision) {
        return fail(SessionControllerErrorCode::InvalidConfiguration,
                    QStringLiteral("The sealed-record policy is not owned by the exact root"));
    }
    model::RecordAccessPolicy policy{
        selected->record.id.value, selected->record.disclosure_policy->policy_id, {}};
    policy.rules.reserve(selected->record.sealed_disclosures.size());
    for (const auto& disclosure : selected->record.sealed_disclosures) {
        std::vector<model::RecordDisclosureDeficiency> deficiencies;
        for (const auto requirement : disclosure.required_items) {
            std::optional<model::RecordDisclosureDeficiencyKind> missing;
            switch (requirement) {
            case packs::RuntimeDisclosureRequirement::Motion:
                if (!disclosure.motion_entry_id.has_value()) {
                    missing = model::RecordDisclosureDeficiencyKind::MissingPublicMotion;
                }
                break;
            case packs::RuntimeDisclosureRequirement::Certificate:
                if (!disclosure.certificate_entry_id.has_value()) {
                    missing = model::RecordDisclosureDeficiencyKind::MissingCertificate;
                }
                break;
            case packs::RuntimeDisclosureRequirement::RedactedCounterpart:
                if (!disclosure.public_entry_id.has_value()) {
                    missing = model::RecordDisclosureDeficiencyKind::MissingRedactedCounterpart;
                }
                break;
            }
            if (missing.has_value()) {
                deficiencies.push_back(
                    model::RecordDisclosureDeficiency{disclosure.disclosure_id.value, *missing});
            }
        }
        policy.rules.push_back(model::RecordAccessRule{
            disclosure.sealed_entry_id.value, disclosure.authorization_authority_id.value,
            disclosure.disclosure_id.value, std::move(deficiencies)});
    }
    std::ranges::sort(policy.rules, {}, &model::RecordAccessRule::sealed_document_id);
    storage::SessionSnapshot preflight;
    preflight.session_id = QStringLiteral("record.access.policy.preflight");
    if (const auto valid = storage::projectRecordAccess(preflight, policy); !valid) {
        return fail(SessionControllerErrorCode::InvalidConfiguration,
                    QStringLiteral("The exact root record-access policy is invalid"));
    }
    return policy;
}

} // namespace

SessionController::SessionController(model::ProcedureDefinition procedure,
                                     model::CaseDefinition case_definition,
                                     model::SessionState state, storage::AssetStore asset_store,
                                     std::unique_ptr<storage::SessionStore> session_store,
                                     storage::SessionSnapshot snapshot)
    : procedure_(std::move(procedure)), case_definition_(std::move(case_definition)),
      state_(std::move(state)), asset_store_(std::move(asset_store)),
      session_store_(std::move(session_store)), snapshot_(std::move(snapshot)) {}

std::expected<std::unique_ptr<SessionController>, SessionControllerError> SessionController::create(
    model::ProcedureDefinition procedure, model::CaseDefinition case_definition,
    model::SessionState initial_state, storage::AssetStore asset_store,
    std::unique_ptr<storage::SessionStore> session_store, QString engine_revision,
    QString created_at_utc, std::vector<storage::RevisionPin> pins) {
    if (!session_store || engine_revision.isEmpty() || created_at_utc.isEmpty() || pins.empty() ||
        usesCanonicalAuthority(procedure) ||
        !isPristineInitialState(procedure, case_definition, initial_state)) {
        return fail(SessionControllerErrorCode::InvalidConfiguration,
                    QStringLiteral("Cannot create a session from the supplied configuration"));
    }
    if (const auto recovered = session_store->recoverAssetStore(asset_store); !recovered) {
        return fail(SessionControllerErrorCode::SessionStoreFailure,
                    QStringLiteral("Cannot bind or recover the session asset store: %1")
                        .arg(recovered.error().message));
    }

    const auto session_id = asQString(initial_state.id.value);
    const auto created =
        session_store->createSession(session_id, engine_revision, created_at_utc, pins,
                                     storage::SessionAuthorityContract::LegacyV1);
    if (!created) {
        return fail(SessionControllerErrorCode::SessionStoreFailure, created.error().message);
    }
    const auto loaded = session_store->loadSession(session_id);
    if (!loaded) {
        return fail(SessionControllerErrorCode::SessionStoreFailure, loaded.error().message);
    }
    if (loaded->authority_contract != storage::SessionAuthorityContract::LegacyV1) {
        return fail(SessionControllerErrorCode::CorruptSession,
                    QStringLiteral("Stored authority contract is not legacy-v1"));
    }

    auto controller = std::unique_ptr<SessionController>(
        new SessionController(std::move(procedure), std::move(case_definition), initial_state,
                              std::move(asset_store), std::move(session_store), *loaded));
    return controller;
}

std::expected<std::unique_ptr<SessionController>, SessionControllerError> SessionController::create(
    model::ProcedureDefinition procedure, model::CaseDefinition case_definition,
    model::SessionState initial_state, storage::AssetStore asset_store,
    std::unique_ptr<storage::SessionStore> session_store, QString engine_revision,
    QString created_at_utc, const packs::ResolvedPack& resolved_pack) {
    if (resolved_pack.root().manifest_schema_version != 1) {
        return fail(SessionControllerErrorCode::InvalidConfiguration,
                    QStringLiteral("Schema-v2 procedures require a catalog-derived session entry "
                                   "point"));
    }
    return create(std::move(procedure), std::move(case_definition), std::move(initial_state),
                  std::move(asset_store), std::move(session_store), std::move(engine_revision),
                  std::move(created_at_utc), revisionPinsForSession(resolved_pack));
}

std::expected<std::unique_ptr<SessionController>, SessionControllerError> SessionController::reopen(
    model::ProcedureDefinition procedure, model::CaseDefinition case_definition,
    model::SessionState initial_state, storage::AssetStore asset_store,
    std::unique_ptr<storage::SessionStore> session_store, QString expected_engine_revision,
    std::vector<storage::RevisionPin> expected_pins) {
    if (!session_store || expected_engine_revision.isEmpty() || expected_pins.empty() ||
        usesCanonicalAuthority(procedure) ||
        !isPristineInitialState(procedure, case_definition, initial_state)) {
        return fail(SessionControllerErrorCode::InvalidConfiguration,
                    QStringLiteral("Cannot reopen a session from the supplied configuration"));
    }
    if (const auto recovered = session_store->recoverAssetStore(asset_store); !recovered) {
        return fail(SessionControllerErrorCode::CorruptSession,
                    QStringLiteral("Cannot bind or recover the session asset store: %1")
                        .arg(recovered.error().message));
    }

    std::ranges::sort(expected_pins, {}, &storage::RevisionPin::pack_id);

    const auto loaded = session_store->loadSession(asQString(initial_state.id.value));
    if (!loaded) {
        return fail(SessionControllerErrorCode::SessionStoreFailure, loaded.error().message);
    }
    if (loaded->engine_revision != expected_engine_revision || loaded->pins != expected_pins ||
        loaded->authority_contract != storage::SessionAuthorityContract::LegacyV1) {
        return fail(SessionControllerErrorCode::CorruptSession,
                    QStringLiteral("Stored engine, exact pack-revision pins, or authority contract "
                                   "differ"));
    }
    const auto replayed =
        replaySnapshot(procedure, case_definition, initial_state, *loaded, asset_store);
    if (!replayed) {
        return std::unexpected(replayed.error());
    }

    auto controller = std::unique_ptr<SessionController>(
        new SessionController(std::move(procedure), std::move(case_definition), *replayed,
                              std::move(asset_store), std::move(session_store), *loaded));
    return controller;
}

std::expected<std::unique_ptr<SessionController>, SessionControllerError> SessionController::reopen(
    model::ProcedureDefinition procedure, model::CaseDefinition case_definition,
    model::SessionState initial_state, storage::AssetStore asset_store,
    std::unique_ptr<storage::SessionStore> session_store, QString expected_engine_revision,
    const packs::ResolvedPack& resolved_pack) {
    if (resolved_pack.root().manifest_schema_version != 1) {
        return fail(SessionControllerErrorCode::InvalidConfiguration,
                    QStringLiteral("Schema-v2 procedures require a catalog-derived session entry "
                                   "point"));
    }
    return reopen(std::move(procedure), std::move(case_definition), std::move(initial_state),
                  std::move(asset_store), std::move(session_store),
                  std::move(expected_engine_revision), revisionPinsForSession(resolved_pack));
}

std::expected<SubmissionResult, SessionControllerError>
SessionController::submit(const model::SubmitFiling& command, QByteArrayView document_bytes,
                          const QString& recorded_at_utc) {
    if (recorded_at_utc.isEmpty() || document_bytes.empty()) {
        return fail(SessionControllerErrorCode::InvalidConfiguration,
                    QStringLiteral("The recorded time and submitted document are required"));
    }

    auto staged_asset = asset_store_.stage(document_bytes);
    if (!staged_asset) {
        return fail(SessionControllerErrorCode::AssetStoreFailure, staged_asset.error().message);
    }
    if (staged_asset->sha256() != asQString(command.document_sha256)) {
        return fail(SessionControllerErrorCode::DocumentDigestMismatch,
                    QStringLiteral("Command document digest does not match the staged asset"));
    }

    const auto decided = engine::decide(procedure_, case_definition_, state_, command);
    if (!decided) {
        return fail(SessionControllerErrorCode::EngineFailure,
                    QString::fromStdString(decided.error().message));
    }

    storage::CommitBatch batch;
    batch.command_id = asQString(command.submission_id.value);
    batch.command_json = encodeCommand(command);
    batch.recorded_at_utc = recorded_at_utc;
    const auto asset_reference = filingDocumentReference(staged_asset->sha256());
    const auto adds_asset_reference =
        std::ranges::find(snapshot_.asset_references, asset_reference) ==
        snapshot_.asset_references.end();
    if (adds_asset_reference) {
        batch.asset_references.push_back(asset_reference);
    } else if (const auto existing = asset_store_.read(staged_asset->sha256()); !existing) {
        return fail(SessionControllerErrorCode::AssetStoreFailure, existing.error().message);
    }
    batch.events.reserve(decided->size());
    for (std::size_t index = 0; index < decided->size(); ++index) {
        const auto& event = (*decided)[index];
        const auto encoded = storage::encodeEvent(event);
        if (!encoded) {
            return fail(SessionControllerErrorCode::EventCodecFailure,
                        QStringLiteral("Cannot encode legal event"));
        }
        batch.events.push_back(storage::EventWrite{
            storage::eventType(event),
            *encoded,
            storage::primaryAuthorityId(event),
        });
        if (const auto docket = docketWrite(event, static_cast<qsizetype>(index));
            docket.has_value()) {
            batch.docket_changes.push_back(*docket);
        }
    }

    // Prove that every event can advance the in-memory state before durably committing it. If a
    // future engine regression violates decide/apply symmetry, the authoritative log remains
    // untouched instead of persisting an unreplayable event.
    auto next_state = state_;
    for (const auto& event : *decided) {
        const auto applied = engine::apply(procedure_, case_definition_, next_state, event);
        if (!applied) {
            return fail(SessionControllerErrorCode::EngineFailure,
                        QStringLiteral("Persisted event cannot be applied: %1")
                            .arg(QString::fromStdString(applied.error().message)));
        }
        next_state = *applied;
    }

    const auto appended =
        adds_asset_reference
            ? session_store_->appendWithStagedAsset(asQString(state_.id.value),
                                                    snapshot_.sequence, batch, asset_store_,
                                                    *staged_asset)
            : session_store_->append(asQString(state_.id.value), snapshot_.sequence, batch);
    if (!appended) {
        return fail(SessionControllerErrorCode::SessionStoreFailure, appended.error().message);
    }

    const auto loaded = session_store_->loadSession(asQString(state_.id.value));
    if (!loaded) {
        return fail(SessionControllerErrorCode::SessionStoreFailure, loaded.error().message);
    }
    if (loaded->sequence != *appended ||
        loaded->authority_contract != storage::SessionAuthorityContract::LegacyV1) {
        return fail(SessionControllerErrorCode::CorruptSession,
                    QStringLiteral("Persisted session sequence or authority contract differs"));
    }

    state_ = std::move(next_state);
    snapshot_ = *loaded;
    return SubmissionResult{storage::StoredAsset{staged_asset->sha256(), staged_asset->size(),
                                                  !adds_asset_reference ||
                                                      staged_asset->wasDeduplicated()},
                            *decided, *appended};
}

const model::SessionState& SessionController::state() const noexcept { return state_; }

const storage::SessionSnapshot& SessionController::snapshot() const noexcept { return snapshot_; }

RecordAccessSessionController::RecordAccessSessionController(
    model::RecordAccessPolicy policy, std::unique_ptr<storage::SessionStore> session_store,
    storage::SessionSnapshot snapshot, model::RecordAccessProjection projection)
    : policy_(std::move(policy)), session_store_(std::move(session_store)),
      snapshot_(std::move(snapshot)), projection_(std::move(projection)) {}

std::expected<std::unique_ptr<RecordAccessSessionController>, SessionControllerError>
RecordAccessSessionController::create(QString session_id, const model::CaseId& selected_case_id,
                                      std::unique_ptr<storage::SessionStore> session_store,
                                      QString engine_revision, QString created_at_utc,
                                      const packs::ResolvedPack& resolved_pack) {
    if (resolved_pack.root().manifest_schema_version != 2) {
        return fail(SessionControllerErrorCode::InvalidConfiguration,
                    QStringLiteral("Cannot create the record-access session"));
    }
    const auto policy = deriveRecordAccessPolicy(selected_case_id, resolved_pack);
    if (!policy) {
        return std::unexpected(policy.error());
    }
    return create(std::move(session_id), *policy, std::move(session_store),
                  std::move(engine_revision), std::move(created_at_utc),
                  revisionPinsForSession(resolved_pack));
}

std::expected<std::unique_ptr<RecordAccessSessionController>, SessionControllerError>
RecordAccessSessionController::create(QString session_id, model::RecordAccessPolicy policy,
                                      std::unique_ptr<storage::SessionStore> session_store,
                                      QString engine_revision, QString created_at_utc,
                                      std::vector<storage::RevisionPin> pins) {
    if (!session_store || session_id.isEmpty() || engine_revision.isEmpty() ||
        created_at_utc.isEmpty() || pins.empty()) {
        return fail(SessionControllerErrorCode::InvalidConfiguration,
                    QStringLiteral("Cannot create the record-access session"));
    }
    storage::SessionSnapshot preflight;
    preflight.session_id = session_id;
    const auto valid_policy = storage::projectRecordAccess(preflight, policy);
    if (!valid_policy) {
        return fail(SessionControllerErrorCode::InvalidConfiguration,
                    QStringLiteral("Record-access policy is invalid: %1")
                        .arg(valid_policy.error().message));
    }
    const auto created =
        session_store->createSession(session_id, engine_revision, created_at_utc, pins,
                                     storage::SessionAuthorityContract::CanonicalV2);
    if (!created) {
        return fail(SessionControllerErrorCode::SessionStoreFailure, created.error().message);
    }
    const auto loaded = session_store->loadSession(session_id);
    if (!loaded) {
        return fail(SessionControllerErrorCode::SessionStoreFailure, loaded.error().message);
    }
    if (loaded->authority_contract != storage::SessionAuthorityContract::CanonicalV2) {
        return fail(SessionControllerErrorCode::CorruptSession,
                    QStringLiteral("Record-access authority contract is not canonical-v2"));
    }
    const auto projection = replayRecordAccessSnapshot(*loaded, policy);
    if (!projection) {
        return std::unexpected(projection.error());
    }
    return std::unique_ptr<RecordAccessSessionController>(new RecordAccessSessionController(
        std::move(policy), std::move(session_store), *loaded, *projection));
}

std::expected<std::unique_ptr<RecordAccessSessionController>, SessionControllerError>
RecordAccessSessionController::reopen(QString session_id, const model::CaseId& selected_case_id,
                                      std::unique_ptr<storage::SessionStore> session_store,
                                      QString expected_engine_revision,
                                      const packs::ResolvedPack& resolved_pack) {
    if (resolved_pack.root().manifest_schema_version != 2) {
        return fail(SessionControllerErrorCode::InvalidConfiguration,
                    QStringLiteral("Cannot reopen the record-access session"));
    }
    const auto policy = deriveRecordAccessPolicy(selected_case_id, resolved_pack);
    if (!policy) {
        return std::unexpected(policy.error());
    }
    return reopen(std::move(session_id), *policy, std::move(session_store),
                  std::move(expected_engine_revision), revisionPinsForSession(resolved_pack));
}

std::expected<std::unique_ptr<RecordAccessSessionController>, SessionControllerError>
RecordAccessSessionController::reopen(QString session_id, model::RecordAccessPolicy policy,
                                      std::unique_ptr<storage::SessionStore> session_store,
                                      QString expected_engine_revision,
                                      std::vector<storage::RevisionPin> expected_pins) {
    if (!session_store || session_id.isEmpty() || expected_engine_revision.isEmpty() ||
        expected_pins.empty()) {
        return fail(SessionControllerErrorCode::InvalidConfiguration,
                    QStringLiteral("Cannot reopen the record-access session"));
    }
    std::ranges::sort(expected_pins, {}, &storage::RevisionPin::pack_id);
    const auto loaded = session_store->loadSession(session_id);
    if (!loaded) {
        return fail(SessionControllerErrorCode::SessionStoreFailure, loaded.error().message);
    }
    if (loaded->engine_revision != expected_engine_revision || loaded->pins != expected_pins ||
        loaded->authority_contract != storage::SessionAuthorityContract::CanonicalV2) {
        return fail(
            SessionControllerErrorCode::CorruptSession,
            QStringLiteral("Stored record-access engine, pins, or authority contract differ"));
    }
    const auto projection = replayRecordAccessSnapshot(*loaded, policy);
    if (!projection) {
        return std::unexpected(projection.error());
    }
    return std::unique_ptr<RecordAccessSessionController>(new RecordAccessSessionController(
        std::move(policy), std::move(session_store), *loaded, *projection));
}

std::expected<void, SessionControllerError>
RecordAccessSessionController::grant(std::string_view disclosure_id, std::string_view event_id,
                                     const QString& recorded_at_utc) {
    const auto rule =
        std::ranges::find(policy_.rules, disclosure_id, &model::RecordAccessRule::disclosure_id);
    if (rule == policy_.rules.end()) {
        return fail(SessionControllerErrorCode::InvalidConfiguration,
                    QStringLiteral("Record disclosure is unavailable"));
    }
    const auto projected = transition(rule->sealed_document_id, event_id,
                                      model::RecordAccessAction::Grant, recorded_at_utc);
    if (!projected) {
        return std::unexpected(projected.error());
    }
    return {};
}

std::expected<void, SessionControllerError>
RecordAccessSessionController::revoke(std::string_view disclosure_id, std::string_view event_id,
                                      const QString& recorded_at_utc) {
    const auto rule =
        std::ranges::find(policy_.rules, disclosure_id, &model::RecordAccessRule::disclosure_id);
    if (rule == policy_.rules.end()) {
        return fail(SessionControllerErrorCode::InvalidConfiguration,
                    QStringLiteral("Record disclosure is unavailable"));
    }
    const auto projected = transition(rule->sealed_document_id, event_id,
                                      model::RecordAccessAction::Revoke, recorded_at_utc);
    if (!projected) {
        return std::unexpected(projected.error());
    }
    return {};
}

std::expected<model::RecordAccessProjection, SessionControllerError>
RecordAccessSessionController::transition(std::string_view sealed_document_id,
                                          std::string_view event_id,
                                          model::RecordAccessAction action,
                                          const QString& recorded_at_utc) {
    if (recorded_at_utc.isEmpty()) {
        return fail(SessionControllerErrorCode::InvalidConfiguration,
                    QStringLiteral("Record-access transition time is required"));
    }
    if (const auto refreshed = refresh(); !refreshed) {
        return std::unexpected(refreshed.error());
    }
    const auto event =
        storage::makeRecordAccessEvent(snapshot_, policy_, event_id, sealed_document_id, action,
                                       recorded_at_utc.toUtf8().toStdString());
    if (!event) {
        return fail(SessionControllerErrorCode::EventCodecFailure, event.error().message);
    }
    const auto encoded = storage::encodeRecordAccessEvent(*event);
    if (!encoded) {
        return fail(SessionControllerErrorCode::EventCodecFailure, encoded.error().message);
    }
    storage::CommitBatch batch;
    batch.command_id = QString::fromUtf8(event->event_id);
    batch.command_json = encodeRecordAccessCommand(*event);
    batch.recorded_at_utc = recorded_at_utc;
    batch.events.push_back(storage::EventWrite{storage::recordAccessEventType(event->action),
                                               *encoded, QString::fromUtf8(event->authority_id)});
    const auto appended = session_store_->append(snapshot_.session_id, snapshot_.sequence, batch);
    if (!appended) {
        return fail(SessionControllerErrorCode::SessionStoreFailure, appended.error().message);
    }
    const auto loaded = session_store_->loadSession(snapshot_.session_id);
    if (!loaded) {
        return fail(SessionControllerErrorCode::SessionStoreFailure, loaded.error().message);
    }
    if (loaded->sequence != *appended || loaded->engine_revision != snapshot_.engine_revision ||
        loaded->pins != snapshot_.pins ||
        loaded->authority_contract != storage::SessionAuthorityContract::CanonicalV2) {
        return fail(SessionControllerErrorCode::CorruptSession,
                    QStringLiteral("Persisted record-access session identity changed"));
    }
    const auto replayed = replayRecordAccessSnapshot(*loaded, policy_);
    if (!replayed) {
        return std::unexpected(replayed.error());
    }
    snapshot_ = *loaded;
    projection_ = *replayed;
    return projection_;
}

std::expected<void, SessionControllerError>
RecordAccessSessionController::applyCurrentProjection(model::RecordAccessProjectionTarget& target) {
    if (const auto refreshed = refresh(); !refreshed) {
        return std::unexpected(refreshed.error());
    }
    if (!target.applyRecordAccessProjection(projection_)) {
        return fail(SessionControllerErrorCode::CorruptSession,
                    QStringLiteral("Live record-access projection was rejected by its workspace"));
    }
    return {};
}

std::expected<void, SessionControllerError> RecordAccessSessionController::refresh() {
    const auto loaded = session_store_->loadSession(snapshot_.session_id);
    if (!loaded) {
        return fail(SessionControllerErrorCode::SessionStoreFailure, loaded.error().message);
    }
    if (loaded->engine_revision != snapshot_.engine_revision || loaded->pins != snapshot_.pins ||
        loaded->authority_contract != storage::SessionAuthorityContract::CanonicalV2 ||
        loaded->sequence < snapshot_.sequence) {
        return fail(SessionControllerErrorCode::CorruptSession,
                    QStringLiteral("Stored record-access identity or sequence regressed"));
    }
    const auto replayed = replayRecordAccessSnapshot(*loaded, policy_);
    if (!replayed) {
        return std::unexpected(replayed.error());
    }
    if (loaded->sequence == snapshot_.sequence && *replayed != projection_) {
        return fail(SessionControllerErrorCode::CorruptSession,
                    QStringLiteral("Stored record-access head changed without advancing"));
    }
    snapshot_ = *loaded;
    projection_ = *replayed;
    return {};
}

std::vector<model::RecordAccessDisclosureStatus>
RecordAccessSessionController::disclosures() const {
    std::vector<model::RecordAccessDisclosureStatus> disclosures;
    disclosures.reserve(policy_.rules.size());
    for (const auto& rule : policy_.rules) {
        disclosures.push_back(model::RecordAccessDisclosureStatus{
            rule.disclosure_id, rule.blocking_deficiencies,
            std::ranges::find(projection_.authorized_document_ids, rule.sealed_document_id) !=
                projection_.authorized_document_ids.end()});
    }
    std::ranges::sort(disclosures, {}, &model::RecordAccessDisclosureStatus::disclosure_id);
    return disclosures;
}

std::expected<model::RecordAccessAuditProjection, SessionControllerError>
RecordAccessSessionController::auditProjectionAt(qint64 through_sequence) const {
    const auto projected = storage::projectRecordAccess(snapshot_, policy_, through_sequence);
    if (!projected) {
        return fail(SessionControllerErrorCode::CorruptSession,
                    QStringLiteral("Record-access prefix cannot be projected: %1")
                        .arg(projected.error().message));
    }
    std::vector<std::string> authorized_disclosures;
    for (const auto& rule : policy_.rules) {
        if (std::ranges::find(projected->authorized_document_ids, rule.sealed_document_id) !=
            projected->authorized_document_ids.end()) {
            authorized_disclosures.push_back(rule.disclosure_id);
        }
    }
    std::ranges::sort(authorized_disclosures);
    return model::RecordAccessAuditProjection{projected->through_sequence,
                                              std::move(authorized_disclosures)};
}

const storage::SessionSnapshot& RecordAccessSessionController::snapshot() const noexcept {
    return snapshot_;
}

} // namespace appellate::app
