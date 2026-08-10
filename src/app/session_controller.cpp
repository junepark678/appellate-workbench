#include "session_controller.hpp"

#include "appellate/engine/procedure_engine.hpp"
#include "appellate/storage/event_codec.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

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
                        QStringLiteral("Cannot verify referenced asset %1")
                            .arg(reference.digest));
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
                            QStringLiteral("Event asset %1 has no session reference")
                                .arg(*digest));
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
        !isPristineInitialState(procedure, case_definition, initial_state)) {
        return fail(SessionControllerErrorCode::InvalidConfiguration,
                    QStringLiteral("Cannot create a session from the supplied configuration"));
    }

    const auto session_id = asQString(initial_state.id.value);
    const auto created =
        session_store->createSession(session_id, engine_revision, created_at_utc, pins);
    if (!created) {
        return fail(SessionControllerErrorCode::SessionStoreFailure, created.error().message);
    }
    const auto loaded = session_store->loadSession(session_id);
    if (!loaded) {
        return fail(SessionControllerErrorCode::SessionStoreFailure, loaded.error().message);
    }

    auto controller = std::unique_ptr<SessionController>(new SessionController(
        std::move(procedure), std::move(case_definition), initial_state, std::move(asset_store),
        std::move(session_store), *loaded));
    return controller;
}

std::expected<std::unique_ptr<SessionController>, SessionControllerError>
SessionController::reopen(model::ProcedureDefinition procedure,
                          model::CaseDefinition case_definition, model::SessionState initial_state,
                          storage::AssetStore asset_store,
                          std::unique_ptr<storage::SessionStore> session_store) {
    if (!session_store || !isPristineInitialState(procedure, case_definition, initial_state)) {
        return fail(SessionControllerErrorCode::InvalidConfiguration,
                    QStringLiteral("Cannot reopen a session from the supplied configuration"));
    }

    const auto loaded = session_store->loadSession(asQString(initial_state.id.value));
    if (!loaded) {
        return fail(SessionControllerErrorCode::SessionStoreFailure, loaded.error().message);
    }
    const auto replayed =
        replaySnapshot(procedure, case_definition, initial_state, *loaded, asset_store);
    if (!replayed) {
        return std::unexpected(replayed.error());
    }

    auto controller = std::unique_ptr<SessionController>(new SessionController(
        std::move(procedure), std::move(case_definition), *replayed, std::move(asset_store),
        std::move(session_store), *loaded));
    return controller;
}

std::expected<SubmissionResult, SessionControllerError>
SessionController::submit(const model::SubmitFiling& command, QByteArrayView document_bytes,
                          const QString& recorded_at_utc) {
    if (recorded_at_utc.isEmpty() || document_bytes.empty()) {
        return fail(SessionControllerErrorCode::InvalidConfiguration,
                    QStringLiteral("The recorded time and submitted document are required"));
    }

    const auto stored_asset = asset_store_.put(document_bytes);
    if (!stored_asset) {
        return fail(SessionControllerErrorCode::AssetStoreFailure, stored_asset.error().message);
    }
    if (stored_asset->sha256 != asQString(command.document_sha256)) {
        return fail(SessionControllerErrorCode::DocumentDigestMismatch,
                    QStringLiteral("Command document digest does not match the stored asset"));
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
    const auto asset_reference = filingDocumentReference(stored_asset->sha256);
    if (std::ranges::find(snapshot_.asset_references, asset_reference) ==
        snapshot_.asset_references.end()) {
        batch.asset_references.push_back(asset_reference);
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
        session_store_->append(asQString(state_.id.value), snapshot_.sequence, batch);
    if (!appended) {
        return fail(SessionControllerErrorCode::SessionStoreFailure, appended.error().message);
    }

    const auto loaded = session_store_->loadSession(asQString(state_.id.value));
    if (!loaded) {
        return fail(SessionControllerErrorCode::SessionStoreFailure, loaded.error().message);
    }
    if (loaded->sequence != *appended) {
        return fail(SessionControllerErrorCode::CorruptSession,
                    QStringLiteral("Persisted session sequence does not match append result"));
    }

    state_ = std::move(next_state);
    snapshot_ = *loaded;
    return SubmissionResult{*stored_asset, *decided, *appended};
}

const model::SessionState& SessionController::state() const noexcept { return state_; }

const storage::SessionSnapshot& SessionController::snapshot() const noexcept { return snapshot_; }

} // namespace appellate::app
