#pragma once

#include "appellate/model/case_definition.hpp"
#include "appellate/model/workflow.hpp"
#include "appellate/model/workflow_command.hpp"
#include "appellate/model/workflow_journal.hpp"
#include "appellate/model/workflow_state.hpp"
#include "appellate/storage/asset_store.hpp"
#include "appellate/storage/session_store.hpp"

#include <QByteArrayView>
#include <QString>

#include <expected>
#include <memory>
#include <optional>
#include <vector>

namespace appellate::app {

enum class WorkflowSessionErrorCode {
    InvalidConfiguration,
    UnexpectedDocument,
    AssetStoreFailure,
    DocumentDigestMismatch,
    EngineFailure,
    CommandCodecFailure,
    EventCodecFailure,
    SessionStoreFailure,
    CorruptSession,
};

struct WorkflowSessionError final {
    WorkflowSessionErrorCode code;
    QString message;

    friend bool operator==(const WorkflowSessionError&, const WorkflowSessionError&) = default;
};

struct WorkflowSubmissionResult final {
    std::optional<storage::StoredAsset> asset;
    std::vector<model::WorkflowEvent> events;
    qint64 persisted_sequence{};
};

class WorkflowSessionController final {
  public:
    WorkflowSessionController(const WorkflowSessionController&) = delete;
    WorkflowSessionController& operator=(const WorkflowSessionController&) = delete;
    WorkflowSessionController(WorkflowSessionController&&) = delete;
    WorkflowSessionController& operator=(WorkflowSessionController&&) = delete;
    ~WorkflowSessionController() = default;

    [[nodiscard]] static auto
    create(model::WorkflowDefinition workflow, model::CaseDefinition case_definition,
           model::WorkflowState initial_state, storage::AssetStore asset_store,
           std::unique_ptr<storage::SessionStore> session_store, QString engine_revision,
           QString created_at_utc, std::vector<storage::RevisionPin> pins)
        -> std::expected<std::unique_ptr<WorkflowSessionController>, WorkflowSessionError>;

    [[nodiscard]] static auto
    reopen(model::WorkflowDefinition workflow, model::CaseDefinition case_definition,
           model::WorkflowState initial_state, storage::AssetStore asset_store,
           std::unique_ptr<storage::SessionStore> session_store, QString expected_engine_revision,
           std::vector<storage::RevisionPin> expected_pins)
        -> std::expected<std::unique_ptr<WorkflowSessionController>, WorkflowSessionError>;

    [[nodiscard]] auto submit(const model::WorkflowCommand& command,
                              std::optional<QByteArrayView> document_bytes,
                              const QString& recorded_at_utc)
        -> std::expected<WorkflowSubmissionResult, WorkflowSessionError>;

    [[nodiscard]] const model::WorkflowState& initialState() const noexcept;
    [[nodiscard]] const model::WorkflowState& state() const noexcept;
    [[nodiscard]] const std::vector<model::WorkflowJournalEntry>& journal() const noexcept;
    [[nodiscard]] const storage::SessionSnapshot& snapshot() const noexcept;

  private:
    WorkflowSessionController(model::WorkflowDefinition workflow,
                              model::CaseDefinition case_definition,
                              model::WorkflowState initial_state, model::WorkflowState state,
                              std::vector<model::WorkflowJournalEntry> journal,
                              storage::AssetStore asset_store,
                              std::unique_ptr<storage::SessionStore> session_store,
                              QString engine_revision, std::vector<storage::RevisionPin> pins,
                              storage::SessionSnapshot snapshot);

    model::WorkflowDefinition workflow_;
    model::CaseDefinition case_definition_;
    model::WorkflowState initial_state_;
    model::WorkflowState state_;
    std::vector<model::WorkflowJournalEntry> journal_;
    storage::AssetStore asset_store_;
    std::unique_ptr<storage::SessionStore> session_store_;
    QString engine_revision_;
    std::vector<storage::RevisionPin> pins_;
    storage::SessionSnapshot snapshot_;
};

} // namespace appellate::app
