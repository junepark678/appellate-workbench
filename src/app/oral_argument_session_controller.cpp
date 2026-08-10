#include "oral_argument_session_controller.hpp"

#include "appellate/engine/oral_argument_engine.hpp"
#include "appellate/storage/oral_argument_codec.hpp"

#include <QDateTime>
#include <QRegularExpression>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <utility>

namespace appellate::app {
namespace {

constexpr auto persisted_event_type = "oral_argument.event.v1";
constexpr auto persisted_authority_id = "oral.argument.engine.v1";
constexpr std::size_t maximum_events = 4'096;

[[nodiscard]] auto fail(OralArgumentSessionErrorCode code, QString message)
    -> std::unexpected<OralArgumentSessionError> {
    return std::unexpected(OralArgumentSessionError{code, std::move(message)});
}

[[nodiscard]] QString asQString(const std::string& value) { return QString::fromStdString(value); }

[[nodiscard]] bool validCanonicalId(const QString& value) {
    if (value.size() < 3 || value.size() > 160) {
        return false;
    }
    bool has_separator = false;
    bool previous_separator = true;
    for (const auto character : value) {
        const auto code = character.unicode();
        const auto alphanumeric = (code >= u'a' && code <= u'z') || (code >= u'0' && code <= u'9');
        const auto separator = code == u'.' || code == u'-';
        if ((!alphanumeric && !separator) || (separator && previous_separator)) {
            return false;
        }
        has_separator = has_separator || separator;
        previous_separator = separator;
    }
    return has_separator && !previous_separator;
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
                                 OralArgumentSessionErrorCode error_code)
    -> std::expected<std::vector<storage::RevisionPin>, OralArgumentSessionError> {
    if (pins.empty() || pins.size() > 128) {
        return fail(error_code,
                    QStringLiteral("A bounded nonempty pack-revision pin set is required"));
    }
    for (const auto& pin : pins) {
        if (!validPackId(pin.pack_id) || !validSemanticVersion(pin.version) ||
            !validDigest(pin.digest)) {
            return fail(error_code, QStringLiteral("An oral-argument revision pin is invalid"));
        }
    }
    std::ranges::sort(
        pins, [](const auto& left, const auto& right) { return left.pack_id < right.pack_id; });
    if (std::ranges::adjacent_find(pins, {}, &storage::RevisionPin::pack_id) != pins.end()) {
        return fail(error_code,
                    QStringLiteral("Oral-argument revision pins contain a duplicate pack"));
    }
    return pins;
}

[[nodiscard]] auto engineFailure(const engine::Error& error, OralArgumentSessionErrorCode code)
    -> std::unexpected<OralArgumentSessionError> {
    return fail(code, asQString(error.message));
}

[[nodiscard]] auto validateDefinitions(const QString& session_id, const QString& engine_revision,
                                       const model::OralArgumentConfiguration& configuration,
                                       const model::BenchConfiguration& bench,
                                       const model::ArgumentGrounding& grounding,
                                       OralArgumentSessionErrorCode error_code)
    -> std::expected<model::OralArgumentState, OralArgumentSessionError> {
    if (!validCanonicalId(session_id) || !validCanonicalId(engine_revision)) {
        return fail(error_code,
                    QStringLiteral("Canonical session and engine revision IDs are required"));
    }
    const auto encoded = storage::encodeOralArgumentConfiguration(configuration);
    if (!encoded) {
        return fail(error_code, encoded.error().message);
    }
    const auto decoded = storage::decodeOralArgumentConfiguration(*encoded);
    if (!decoded || *decoded != configuration) {
        return fail(error_code,
                    decoded ? QStringLiteral("Configuration does not round-trip canonically")
                            : decoded.error().message);
    }
    const auto initial = engine::initializeOralArgument(configuration, bench, grounding);
    if (!initial) {
        return engineFailure(initial.error(), error_code);
    }
    return *initial;
}

[[nodiscard]] QString openingCommandId(const QString& session_id) {
    return session_id + QStringLiteral(".opening");
}

[[nodiscard]] auto
validateSnapshot(const QString& session_id, const QString& expected_engine_revision,
                 const std::vector<storage::RevisionPin>& expected_pins,
                 const model::OralArgumentConfiguration& configuration,
                 const model::BenchConfiguration& bench, const model::ArgumentGrounding& grounding,
                 const model::OralArgumentState& initial_state,
                 const storage::SessionSnapshot& snapshot)
    -> std::expected<model::OralArgumentState, OralArgumentSessionError> {
    if (snapshot.session_id != session_id || snapshot.engine_revision != expected_engine_revision ||
        snapshot.pins != expected_pins || snapshot.sequence <= 0 ||
        snapshot.sequence > static_cast<qint64>(maximum_events) ||
        snapshot.commands.size() != snapshot.events.size() ||
        snapshot.events.size() != static_cast<std::size_t>(snapshot.sequence) ||
        !snapshot.docket.empty() || !snapshot.asset_references.empty()) {
        return fail(OralArgumentSessionErrorCode::CorruptSession,
                    QStringLiteral("Oral-argument session identity, pins, or row counts differ"));
    }

    auto state = initial_state;
    std::vector<model::OralArgumentEvent> journal;
    journal.reserve(snapshot.events.size());
    for (std::size_t index = 0; index < snapshot.events.size(); ++index) {
        const auto expected_sequence = static_cast<qint64>(index);
        const auto& command = snapshot.commands[index];
        const auto& stored_event = snapshot.events[index];
        if (command.expected_sequence != expected_sequence ||
            stored_event.sequence != expected_sequence + 1 ||
            !validCanonicalUtc(command.recorded_at_utc) ||
            stored_event.event_type != QLatin1StringView(persisted_event_type) ||
            stored_event.authority_id != QLatin1StringView(persisted_authority_id)) {
            return fail(OralArgumentSessionErrorCode::CorruptSession,
                        QStringLiteral("Oral-argument command/event envelope was tampered"));
        }

        std::optional<model::CounselAnswer> answer;
        if (index == 0) {
            if (command.command_id != openingCommandId(session_id)) {
                return fail(OralArgumentSessionErrorCode::CorruptSession,
                            QStringLiteral("Oral-argument opening command identity differs"));
            }
            const auto persisted = storage::decodeOralArgumentOpeningCommand(command.payload_json);
            if (!persisted || persisted->session_id != session_id ||
                persisted->command_id != command.command_id ||
                persisted->engine_revision != expected_engine_revision ||
                persisted->recorded_at_utc != command.recorded_at_utc ||
                persisted->configuration != configuration) {
                return fail(
                    OralArgumentSessionErrorCode::CorruptSession,
                    persisted ? QStringLiteral("Persisted oral-argument configuration pin differs")
                              : persisted.error().message);
            }
        } else {
            if (!validCanonicalId(command.command_id) ||
                command.command_id == openingCommandId(session_id)) {
                return fail(OralArgumentSessionErrorCode::CorruptSession,
                            QStringLiteral("Persisted counsel command ID is invalid"));
            }
            const auto persisted = storage::decodeOralArgumentCounselCommand(command.payload_json);
            if (!persisted || persisted->session_id != session_id ||
                persisted->command_id != command.command_id ||
                persisted->recorded_at_utc != command.recorded_at_utc) {
                return fail(OralArgumentSessionErrorCode::CorruptSession,
                            persisted ? QStringLiteral("Persisted counsel command header differs")
                                      : persisted.error().message);
            }
            answer = persisted->answer;
        }

        const auto event = storage::decodeOralArgumentEvent(stored_event.payload_json);
        if (!event || event->sequence != static_cast<std::uint64_t>(stored_event.sequence) ||
            event->counsel != answer) {
            return fail(OralArgumentSessionErrorCode::CorruptSession,
                        event ? QStringLiteral("Persisted answer/event pair differs")
                              : event.error().message);
        }

        std::expected<model::OralArgumentEvent, engine::Error> decision =
            index == 0
                ? engine::planOpeningQuestion(configuration, bench, grounding, state)
                : engine::decideCounselAnswer(configuration, bench, grounding, state, *answer);
        if (!decision || *decision != *event) {
            return fail(OralArgumentSessionErrorCode::CorruptSession,
                        decision ? QStringLiteral("Persisted event differs from exact re-decision")
                                 : asQString(decision.error().message));
        }
        const auto applied =
            engine::applyOralArgumentEvent(configuration, bench, grounding, state, *event);
        if (!applied) {
            return fail(OralArgumentSessionErrorCode::CorruptSession,
                        asQString(applied.error().message));
        }
        state = *applied;
        journal.push_back(*event);
    }

    const auto replayed =
        engine::replayOralArgument(configuration, bench, grounding, initial_state, journal);
    if (!replayed || *replayed != state || state.journal != journal ||
        state.next_event_sequence != static_cast<std::uint64_t>(snapshot.sequence) + 1) {
        return fail(OralArgumentSessionErrorCode::CorruptSession,
                    replayed ? QStringLiteral("Persisted transcript is not its exact replay")
                             : asQString(replayed.error().message));
    }
    return state;
}

} // namespace

OralArgumentSessionController::OralArgumentSessionController(
    QString session_id, model::OralArgumentConfiguration configuration,
    model::BenchConfiguration bench, model::ArgumentGrounding grounding,
    model::OralArgumentState initial_state, model::OralArgumentState state,
    std::unique_ptr<storage::SessionStore> session_store, QString engine_revision,
    std::vector<storage::RevisionPin> pins, storage::SessionSnapshot snapshot)
    : session_id_(std::move(session_id)), configuration_(std::move(configuration)),
      bench_(std::move(bench)), grounding_(std::move(grounding)),
      initial_state_(std::move(initial_state)), state_(std::move(state)),
      session_store_(std::move(session_store)), engine_revision_(std::move(engine_revision)),
      pins_(std::move(pins)), snapshot_(std::move(snapshot)) {}

std::expected<std::unique_ptr<OralArgumentSessionController>, OralArgumentSessionError>
OralArgumentSessionController::create(QString session_id,
                                      model::OralArgumentConfiguration configuration,
                                      model::BenchConfiguration bench,
                                      model::ArgumentGrounding grounding,
                                      std::unique_ptr<storage::SessionStore> session_store,
                                      QString engine_revision, QString created_at_utc,
                                      std::vector<storage::RevisionPin> pins) {
    if (!session_store || !validCanonicalUtc(created_at_utc)) {
        return fail(OralArgumentSessionErrorCode::InvalidConfiguration,
                    QStringLiteral("Session store and canonical creation time are required"));
    }
    const auto normalized_pins =
        normalizePins(std::move(pins), OralArgumentSessionErrorCode::InvalidConfiguration);
    if (!normalized_pins) {
        return std::unexpected(normalized_pins.error());
    }
    const auto initial =
        validateDefinitions(session_id, engine_revision, configuration, bench, grounding,
                            OralArgumentSessionErrorCode::InvalidConfiguration);
    if (!initial) {
        return std::unexpected(initial.error());
    }
    const auto encoded_opening = storage::encodeOralArgumentOpeningCommand(
        storage::OralArgumentOpeningCommand{session_id, openingCommandId(session_id),
                                            engine_revision, created_at_utc, configuration});
    if (!encoded_opening) {
        return fail(OralArgumentSessionErrorCode::CommandCodecFailure,
                    encoded_opening.error().message);
    }
    const auto opening = engine::planOpeningQuestion(configuration, bench, grounding, *initial);
    if (!opening) {
        return engineFailure(opening.error(), OralArgumentSessionErrorCode::EngineFailure);
    }
    const auto encoded_event = storage::encodeOralArgumentEvent(*opening);
    if (!encoded_event) {
        return fail(OralArgumentSessionErrorCode::EventCodecFailure, encoded_event.error().message);
    }
    const auto projected =
        engine::applyOralArgumentEvent(configuration, bench, grounding, *initial, *opening);
    if (!projected) {
        return engineFailure(projected.error(), OralArgumentSessionErrorCode::EngineFailure);
    }

    if (const auto created = session_store->createSession(session_id, engine_revision,
                                                          created_at_utc, *normalized_pins);
        !created) {
        return fail(OralArgumentSessionErrorCode::SessionStoreFailure, created.error().message);
    }
    const storage::CommitBatch opening_batch{
        openingCommandId(session_id),
        *encoded_opening,
        created_at_utc,
        {storage::EventWrite{QLatin1StringView(persisted_event_type), *encoded_event,
                             QLatin1StringView(persisted_authority_id)}},
        {},
        {},
    };
    const auto appended = session_store->append(session_id, 0, opening_batch);
    if (!appended) {
        return fail(OralArgumentSessionErrorCode::SessionStoreFailure, appended.error().message);
    }
    const auto snapshot = session_store->loadSession(session_id);
    if (!snapshot) {
        return fail(OralArgumentSessionErrorCode::SessionStoreFailure, snapshot.error().message);
    }
    const auto verified = validateSnapshot(session_id, engine_revision, *normalized_pins,
                                           configuration, bench, grounding, *initial, *snapshot);
    if (!verified || *verified != *projected || *appended != 1) {
        return fail(OralArgumentSessionErrorCode::CorruptSession,
                    verified ? QStringLiteral("New oral-argument session failed verification")
                             : verified.error().message);
    }
    return std::unique_ptr<OralArgumentSessionController>(new OralArgumentSessionController(
        std::move(session_id), std::move(configuration), std::move(bench), std::move(grounding),
        *initial, *verified, std::move(session_store), std::move(engine_revision), *normalized_pins,
        *snapshot));
}

std::expected<std::unique_ptr<OralArgumentSessionController>, OralArgumentSessionError>
OralArgumentSessionController::reopen(QString session_id,
                                      model::OralArgumentConfiguration configuration,
                                      model::BenchConfiguration bench,
                                      model::ArgumentGrounding grounding,
                                      std::unique_ptr<storage::SessionStore> session_store,
                                      QString expected_engine_revision,
                                      std::vector<storage::RevisionPin> expected_pins) {
    if (!session_store) {
        return fail(OralArgumentSessionErrorCode::InvalidConfiguration,
                    QStringLiteral("Session store is required"));
    }
    const auto normalized_pins =
        normalizePins(std::move(expected_pins), OralArgumentSessionErrorCode::InvalidConfiguration);
    if (!normalized_pins) {
        return std::unexpected(normalized_pins.error());
    }
    const auto initial =
        validateDefinitions(session_id, expected_engine_revision, configuration, bench, grounding,
                            OralArgumentSessionErrorCode::InvalidConfiguration);
    if (!initial) {
        return std::unexpected(initial.error());
    }
    const auto snapshot = session_store->loadSession(session_id);
    if (!snapshot) {
        return fail(OralArgumentSessionErrorCode::SessionStoreFailure, snapshot.error().message);
    }
    const auto state = validateSnapshot(session_id, expected_engine_revision, *normalized_pins,
                                        configuration, bench, grounding, *initial, *snapshot);
    if (!state) {
        return std::unexpected(state.error());
    }
    return std::unique_ptr<OralArgumentSessionController>(new OralArgumentSessionController(
        std::move(session_id), std::move(configuration), std::move(bench), std::move(grounding),
        *initial, *state, std::move(session_store), std::move(expected_engine_revision),
        *normalized_pins, *snapshot));
}

std::expected<OralArgumentSubmissionResult, OralArgumentSessionError>
OralArgumentSessionController::submit(QString command_id, const model::CounselAnswer& answer,
                                      const QString& recorded_at_utc) {
    if (!validCanonicalId(command_id) || command_id == openingCommandId(session_id_) ||
        !validCanonicalUtc(recorded_at_utc) || snapshot_.sequence < 1 ||
        snapshot_.sequence != static_cast<qint64>(state_.journal.size()) ||
        snapshot_.sequence >= static_cast<qint64>(maximum_events)) {
        return fail(
            OralArgumentSessionErrorCode::InvalidConfiguration,
            QStringLiteral("Counsel command identity, time, or session sequence is invalid"));
    }
    const auto encoded_command = storage::encodeOralArgumentCounselCommand(
        storage::OralArgumentCounselCommand{session_id_, command_id, recorded_at_utc, answer});
    if (!encoded_command) {
        return fail(OralArgumentSessionErrorCode::CommandCodecFailure,
                    encoded_command.error().message);
    }
    const auto event =
        engine::decideCounselAnswer(configuration_, bench_, grounding_, state_, answer);
    if (!event) {
        return engineFailure(event.error(), OralArgumentSessionErrorCode::EngineFailure);
    }
    const auto encoded_event = storage::encodeOralArgumentEvent(*event);
    if (!encoded_event) {
        return fail(OralArgumentSessionErrorCode::EventCodecFailure, encoded_event.error().message);
    }
    const auto projected =
        engine::applyOralArgumentEvent(configuration_, bench_, grounding_, state_, *event);
    if (!projected) {
        return engineFailure(projected.error(), OralArgumentSessionErrorCode::EngineFailure);
    }
    const storage::CommitBatch batch{
        command_id,
        *encoded_command,
        recorded_at_utc,
        {storage::EventWrite{QLatin1StringView(persisted_event_type), *encoded_event,
                             QLatin1StringView(persisted_authority_id)}},
        {},
        {},
    };
    const auto appended = session_store_->append(session_id_, snapshot_.sequence, batch);
    if (!appended) {
        return fail(OralArgumentSessionErrorCode::SessionStoreFailure, appended.error().message);
    }
    const auto snapshot = session_store_->loadSession(session_id_);
    if (!snapshot) {
        return fail(OralArgumentSessionErrorCode::SessionStoreFailure, snapshot.error().message);
    }
    const auto verified = validateSnapshot(session_id_, engine_revision_, pins_, configuration_,
                                           bench_, grounding_, initial_state_, *snapshot);
    if (!verified || *verified != *projected || *appended != snapshot->sequence) {
        return fail(OralArgumentSessionErrorCode::CorruptSession,
                    verified ? QStringLiteral("Appended oral-argument event failed verification")
                             : verified.error().message);
    }
    state_ = *verified;
    snapshot_ = *snapshot;
    return OralArgumentSubmissionResult{*event, *appended};
}

const QString& OralArgumentSessionController::sessionId() const noexcept { return session_id_; }

const model::OralArgumentState& OralArgumentSessionController::initialState() const noexcept {
    return initial_state_;
}

const model::OralArgumentState& OralArgumentSessionController::state() const noexcept {
    return state_;
}

const storage::SessionSnapshot& OralArgumentSessionController::snapshot() const noexcept {
    return snapshot_;
}

} // namespace appellate::app
