#pragma once

#include "appellate/model/case_definition.hpp"
#include "appellate/model/command.hpp"
#include "appellate/model/event.hpp"
#include "appellate/model/procedure.hpp"
#include "appellate/model/session.hpp"
#include "appellate/storage/asset_store.hpp"
#include "appellate/storage/session_store.hpp"

#include <QByteArrayView>
#include <QString>

#include <expected>
#include <memory>
#include <vector>

namespace appellate::app {

enum class SessionControllerErrorCode {
    InvalidConfiguration,
    AssetStoreFailure,
    DocumentDigestMismatch,
    EngineFailure,
    EventCodecFailure,
    SessionStoreFailure,
    CorruptSession,
};

struct SessionControllerError final {
    SessionControllerErrorCode code;
    QString message;

    friend bool operator==(const SessionControllerError&, const SessionControllerError&) = default;
};

struct SubmissionResult final {
    storage::StoredAsset asset;
    std::vector<model::LegalEvent> events;
    qint64 persisted_sequence{};
};

class SessionController final {
  public:
    SessionController(const SessionController&) = delete;
    SessionController& operator=(const SessionController&) = delete;
    SessionController(SessionController&&) = delete;
    SessionController& operator=(SessionController&&) = delete;
    ~SessionController() = default;

    [[nodiscard]] static auto
    create(model::ProcedureDefinition procedure, model::CaseDefinition case_definition,
           model::SessionState initial_state, storage::AssetStore asset_store,
           std::unique_ptr<storage::SessionStore> session_store, QString engine_revision,
           QString created_at_utc, std::vector<storage::RevisionPin> pins)
        -> std::expected<std::unique_ptr<SessionController>, SessionControllerError>;

    [[nodiscard]] static auto
    reopen(model::ProcedureDefinition procedure, model::CaseDefinition case_definition,
           model::SessionState initial_state, storage::AssetStore asset_store,
           std::unique_ptr<storage::SessionStore> session_store)
        -> std::expected<std::unique_ptr<SessionController>, SessionControllerError>;

    [[nodiscard]] auto submit(const model::SubmitFiling& command, QByteArrayView document_bytes,
                              const QString& recorded_at_utc)
        -> std::expected<SubmissionResult, SessionControllerError>;

    [[nodiscard]] const model::SessionState& state() const noexcept;
    [[nodiscard]] const storage::SessionSnapshot& snapshot() const noexcept;

  private:
    SessionController(model::ProcedureDefinition procedure, model::CaseDefinition case_definition,
                      model::SessionState state, storage::AssetStore asset_store,
                      std::unique_ptr<storage::SessionStore> session_store,
                      storage::SessionSnapshot snapshot);

    model::ProcedureDefinition procedure_;
    model::CaseDefinition case_definition_;
    model::SessionState state_;
    storage::AssetStore asset_store_;
    std::unique_ptr<storage::SessionStore> session_store_;
    storage::SessionSnapshot snapshot_;
};

} // namespace appellate::app
