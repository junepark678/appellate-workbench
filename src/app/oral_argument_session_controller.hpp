#pragma once

#include "appellate/model/oral_argument.hpp"
#include "appellate/storage/session_store.hpp"

#include <QString>

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace appellate::packs {
class ResolvedPack;
struct RuntimeArgumentConfigId;
struct RuntimePack;
}

namespace appellate::app {

enum class OralArgumentSessionErrorCode {
    InvalidConfiguration,
    EngineFailure,
    CommandCodecFailure,
    EventCodecFailure,
    SessionStoreFailure,
    CorruptSession,
};

struct OralArgumentSessionError final {
    OralArgumentSessionErrorCode code;
    QString message;

    friend bool operator==(const OralArgumentSessionError&,
                           const OralArgumentSessionError&) = default;
};

struct OralArgumentSubmissionResult final {
    model::OralArgumentEvent event;
    qint64 persisted_sequence{};
};

class OralArgumentSessionController final {
  public:
    OralArgumentSessionController(const OralArgumentSessionController&) = delete;
    OralArgumentSessionController& operator=(const OralArgumentSessionController&) = delete;
    OralArgumentSessionController(OralArgumentSessionController&&) = delete;
    OralArgumentSessionController& operator=(OralArgumentSessionController&&) = delete;
    ~OralArgumentSessionController() = default;

    [[nodiscard]] static auto
    create(QString session_id, model::OralArgumentConfiguration configuration,
           model::BenchConfiguration bench, model::ArgumentGrounding grounding,
           std::unique_ptr<storage::SessionStore> session_store, QString engine_revision,
           QString created_at_utc, std::vector<storage::RevisionPin> pins)
        -> std::expected<std::unique_ptr<OralArgumentSessionController>, OralArgumentSessionError>;

    [[nodiscard]] static auto
    create(QString session_id, const model::CaseId& case_id,
           const packs::RuntimeArgumentConfigId& argument_configuration_id,
           std::string legal_state_digest,
           std::unique_ptr<storage::SessionStore> session_store, QString engine_revision,
           QString created_at_utc, const packs::ResolvedPack& resolved_pack)
        -> std::expected<std::unique_ptr<OralArgumentSessionController>, OralArgumentSessionError>;

    [[nodiscard]] static auto
    reopen(QString session_id, model::OralArgumentConfiguration configuration,
           model::BenchConfiguration bench, model::ArgumentGrounding grounding,
           std::unique_ptr<storage::SessionStore> session_store, QString expected_engine_revision,
           std::vector<storage::RevisionPin> expected_pins)
        -> std::expected<std::unique_ptr<OralArgumentSessionController>, OralArgumentSessionError>;

    [[nodiscard]] static auto
    reopen(QString session_id, const model::CaseId& case_id,
           const packs::RuntimeArgumentConfigId& argument_configuration_id,
           std::string legal_state_digest,
           std::unique_ptr<storage::SessionStore> session_store, QString expected_engine_revision,
           const packs::ResolvedPack& resolved_pack)
        -> std::expected<std::unique_ptr<OralArgumentSessionController>, OralArgumentSessionError>;

    [[nodiscard]] auto submit(QString command_id, const model::CounselAnswer& answer,
                              const QString& recorded_at_utc)
        -> std::expected<OralArgumentSubmissionResult, OralArgumentSessionError>;

    [[nodiscard]] const QString& sessionId() const noexcept;
    [[nodiscard]] const model::OralArgumentState& initialState() const noexcept;
    [[nodiscard]] const model::OralArgumentState& state() const noexcept;
    [[nodiscard]] const storage::SessionSnapshot& snapshot() const noexcept;
    [[nodiscard]] const model::CanonicalOralArgumentDefinition*
    canonicalDefinition() const noexcept;

  private:
    friend class OralArgumentSessionControllerTestAccess;

    OralArgumentSessionController(
        QString session_id, model::OralArgumentConfiguration configuration,
        model::BenchConfiguration bench, model::ArgumentGrounding grounding,
        model::OralArgumentState initial_state, model::OralArgumentState state,
        std::unique_ptr<storage::SessionStore> session_store, QString engine_revision,
        std::vector<storage::RevisionPin> pins, storage::SessionSnapshot snapshot);

    OralArgumentSessionController(
        QString session_id, model::CanonicalOralArgumentDefinition definition,
        model::OralArgumentState initial_state, model::OralArgumentState state,
        std::unique_ptr<storage::SessionStore> session_store, QString engine_revision,
        std::vector<storage::RevisionPin> pins, storage::SessionSnapshot snapshot);

    [[nodiscard]] static auto createCanonicalForTesting(
        QString session_id, model::CanonicalOralArgumentDefinition definition,
        std::unique_ptr<storage::SessionStore> session_store, QString engine_revision,
        QString created_at_utc, std::vector<storage::RevisionPin> pins)
        -> std::expected<std::unique_ptr<OralArgumentSessionController>, OralArgumentSessionError>;

    [[nodiscard]] static auto reopenCanonicalForTesting(
        QString session_id, model::CanonicalOralArgumentDefinition definition,
        std::unique_ptr<storage::SessionStore> session_store, QString expected_engine_revision,
        std::vector<storage::RevisionPin> expected_pins)
        -> std::expected<std::unique_ptr<OralArgumentSessionController>, OralArgumentSessionError>;

    [[nodiscard]] static auto deriveCanonicalDefinitionForTesting(
        const model::CaseId& case_id,
        const packs::RuntimeArgumentConfigId& argument_configuration_id,
        std::string legal_state_digest, const packs::RuntimePack& runtime_pack)
        -> std::expected<model::CanonicalOralArgumentDefinition, OralArgumentSessionError>;

    QString session_id_;
    model::OralArgumentConfiguration configuration_;
    model::BenchConfiguration bench_;
    model::ArgumentGrounding grounding_;
    std::optional<model::CanonicalOralArgumentDefinition> canonical_definition_;
    model::OralArgumentState initial_state_;
    model::OralArgumentState state_;
    std::unique_ptr<storage::SessionStore> session_store_;
    QString engine_revision_;
    std::vector<storage::RevisionPin> pins_;
    storage::SessionSnapshot snapshot_;
};

} // namespace appellate::app
