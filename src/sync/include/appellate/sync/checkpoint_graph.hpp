#pragma once

#include "appellate/sync/logical_object_codec.hpp"

#include <QString>

#include <cstddef>
#include <expected>
#include <vector>

namespace appellate::sync {

struct IdentifiedSessionEventSegment final {
    SyncObjectId canonical_id{};
    SessionEventSegment segment;

    friend bool operator==(const IdentifiedSessionEventSegment&,
                           const IdentifiedSessionEventSegment&) = default;
};

struct IdentifiedCheckpoint final {
    SyncObjectId canonical_id{};
    Checkpoint checkpoint;

    friend bool operator==(const IdentifiedCheckpoint&, const IdentifiedCheckpoint&) = default;
};

struct BranchHead final {
    SyncObjectId checkpoint_id{};
    SyncObjectId head_segment_id{};
    std::uint64_t head_sequence{};
    bool is_resolution{};

    friend bool operator==(const BranchHead&, const BranchHead&) = default;
};

struct RestoreExpectations final {
    QString session_id;
    QString engine_revision;
    LogicalAuthorityContract authority_contract{LogicalAuthorityContract::LegacyV1};
    QString session_created_at_utc;
    std::vector<LogicalRevisionPin> pins;
    SyncObjectId projection_digest{};
    std::vector<SyncObjectId> available_authored_revision_ids;
};

struct RestoreImportPlan final {
    SyncObjectId selected_checkpoint_id{};
    std::vector<BranchHead> current_heads;
    std::vector<IdentifiedCheckpoint> checkpoint_ancestry;
    std::vector<IdentifiedSessionEventSegment> segment_ancestry;
    std::vector<SyncObjectId> selected_segment_path;
    std::vector<SyncObjectId> referenced_authored_revision_ids;
};

struct CheckpointGraphLimits final {
    static constexpr std::size_t default_maximum_segments = 4'096U;
    static constexpr std::size_t default_maximum_checkpoints = 4'096U;
    static constexpr std::size_t default_maximum_ancestry_depth = 4'096U;

    std::size_t maximum_segments{default_maximum_segments};
    std::size_t maximum_checkpoints{default_maximum_checkpoints};
    std::size_t maximum_ancestry_depth{default_maximum_ancestry_depth};
    LogicalObjectLimits object_limits{};
};

enum class CheckpointGraphErrorCode {
    InvalidArgument,
    LimitExceeded,
    InvalidLogicalObject,
    IdentityMismatch,
    DuplicateObject,
    MissingObject,
    Cycle,
    MetadataMismatch,
    SequenceMismatch,
    InvalidAncestry,
    OrphanObject,
    NotCurrentHead,
    ProjectionMismatch,
    PinsMismatch,
    MissingAuthoredRevision,
    DuplicateCommand,
    NoConflict,
};

struct CheckpointGraphError final {
    CheckpointGraphErrorCode code{};
    QString message;

    friend bool operator==(const CheckpointGraphError&, const CheckpointGraphError&) = default;
};

class CheckpointGraph final {
  public:
    [[nodiscard]] static auto build(std::vector<IdentifiedSessionEventSegment> segments,
                                    std::vector<IdentifiedCheckpoint> checkpoints,
                                    CheckpointGraphLimits limits = {})
        -> std::expected<CheckpointGraph, CheckpointGraphError>;

    [[nodiscard]] const std::vector<BranchHead>& currentHeads() const noexcept;

    [[nodiscard]] auto makeImportPlan(const SyncObjectId& selected_checkpoint_id,
                                      const RestoreExpectations& expectations) const
        -> std::expected<RestoreImportPlan, CheckpointGraphError>;

    // Produces a value-only resolution checkpoint. The caller gives it an authenticated envelope
    // identity before rebuilding the graph; no history is rewritten or deleted here.
    [[nodiscard]] auto
    makeResolutionCheckpoint(const SyncObjectId& selected_base_checkpoint_id) const
        -> std::expected<Checkpoint, CheckpointGraphError>;

  private:
    CheckpointGraph(std::vector<IdentifiedSessionEventSegment> segments,
                    std::vector<IdentifiedCheckpoint> checkpoints,
                    std::vector<SyncObjectId> segment_topological_order,
                    std::vector<SyncObjectId> checkpoint_topological_order,
                    std::vector<BranchHead> current_heads, CheckpointGraphLimits limits) noexcept;

    std::vector<IdentifiedSessionEventSegment> segments_;
    std::vector<IdentifiedCheckpoint> checkpoints_;
    std::vector<SyncObjectId> segment_topological_order_;
    std::vector<SyncObjectId> checkpoint_topological_order_;
    std::vector<BranchHead> current_heads_;
    CheckpointGraphLimits limits_;
};

} // namespace appellate::sync
