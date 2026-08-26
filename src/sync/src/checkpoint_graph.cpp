#include "appellate/sync/checkpoint_graph.hpp"

#include "appellate/sync/protocol_codec.hpp"

#include <QSet>

#include <algorithm>
#include <map>
#include <ranges>
#include <set>
#include <span>
#include <utility>

namespace appellate::sync {
namespace {

using SegmentMap = std::map<SyncObjectId, const IdentifiedSessionEventSegment*>;
using CheckpointMap = std::map<SyncObjectId, const IdentifiedCheckpoint*>;

[[nodiscard]] auto fail(CheckpointGraphErrorCode code, QString message)
    -> std::unexpected<CheckpointGraphError> {
    return std::unexpected(CheckpointGraphError{code, std::move(message)});
}

[[nodiscard]] bool isAllZero(const SyncObjectId& value) {
    unsigned char aggregate{};
    for (const auto byte : value) {
        aggregate = static_cast<unsigned char>(aggregate | byte);
    }
    return aggregate == 0U;
}

[[nodiscard]] bool strictlySortedIds(const std::vector<SyncObjectId>& values) {
    for (std::size_t index = 1; index < values.size(); ++index) {
        if (!(values[index - 1U] < values[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] auto validateLimits(const CheckpointGraphLimits& limits)
    -> std::expected<void, CheckpointGraphError> {
    if (limits.maximum_segments == 0 ||
        limits.maximum_segments > CheckpointGraphLimits::default_maximum_segments ||
        limits.maximum_checkpoints == 0 ||
        limits.maximum_checkpoints > CheckpointGraphLimits::default_maximum_checkpoints ||
        limits.maximum_ancestry_depth == 0 ||
        limits.maximum_ancestry_depth > CheckpointGraphLimits::default_maximum_ancestry_depth) {
        return fail(CheckpointGraphErrorCode::InvalidArgument,
                    QStringLiteral("Checkpoint-graph limits are invalid"));
    }
    return {};
}

[[nodiscard]] auto canonicalId(SyncObjectKind kind, std::uint16_t schema_version,
                               QByteArrayView payload)
    -> std::expected<SyncObjectId, CheckpointGraphError> {
    SyncSecretKey local_identity_key{};
    local_identity_key.fill(0x5aU);
    const auto identity =
        ProtocolCodec::canonicalIdentity(kind, schema_version, payload, local_identity_key);
    if (!identity) {
        return fail(CheckpointGraphErrorCode::InvalidLogicalObject,
                    QStringLiteral("Cannot recompute logical-object identity"));
    }
    return identity->canonical_id;
}

[[nodiscard]] auto validateSegmentIdentity(const IdentifiedSessionEventSegment& identified,
                                           const LogicalObjectLimits& limits)
    -> std::expected<void, CheckpointGraphError> {
    if (isAllZero(identified.canonical_id)) {
        return fail(CheckpointGraphErrorCode::InvalidArgument,
                    QStringLiteral("Segment identity is zero"));
    }
    const auto encoded = LogicalObjectCodec::encodeSessionEventSegment(identified.segment, limits);
    if (!encoded) {
        return fail(CheckpointGraphErrorCode::InvalidLogicalObject,
                    QStringLiteral("Session-event segment is invalid"));
    }
    const auto computed =
        canonicalId(SyncObjectKind::SessionEventSegment,
                    LogicalObjectCodec::session_event_segment_schema_version, *encoded);
    if (!computed) {
        return std::unexpected(computed.error());
    }
    if (*computed != identified.canonical_id) {
        return fail(CheckpointGraphErrorCode::IdentityMismatch,
                    QStringLiteral("Session-event segment identity does not match its payload"));
    }
    return {};
}

[[nodiscard]] auto validateCheckpointIdentity(const IdentifiedCheckpoint& identified,
                                              const LogicalObjectLimits& limits)
    -> std::expected<void, CheckpointGraphError> {
    if (isAllZero(identified.canonical_id)) {
        return fail(CheckpointGraphErrorCode::InvalidArgument,
                    QStringLiteral("Checkpoint identity is zero"));
    }
    const auto encoded = LogicalObjectCodec::encodeCheckpoint(identified.checkpoint, limits);
    if (!encoded) {
        return fail(CheckpointGraphErrorCode::InvalidLogicalObject,
                    QStringLiteral("Checkpoint is invalid"));
    }
    const auto computed = canonicalId(SyncObjectKind::Checkpoint,
                                      LogicalObjectCodec::checkpoint_schema_version, *encoded);
    if (!computed) {
        return std::unexpected(computed.error());
    }
    if (*computed != identified.canonical_id) {
        return fail(CheckpointGraphErrorCode::IdentityMismatch,
                    QStringLiteral("Checkpoint identity does not match its payload"));
    }
    return {};
}

template <typename Value> [[nodiscard]] bool duplicateIdentities(const std::vector<Value>& values) {
    for (std::size_t index = 1; index < values.size(); ++index) {
        if (values[index - 1U].canonical_id == values[index].canonical_id) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool sameSessionMetadata(const SessionEventSegment& segment,
                                       const Checkpoint& checkpoint) {
    return segment.session_id == checkpoint.session_id &&
           segment.engine_revision == checkpoint.engine_revision &&
           segment.authority_contract == checkpoint.authority_contract;
}

[[nodiscard]] bool sameCheckpointMetadata(const Checkpoint& left, const Checkpoint& right) {
    return left.session_id == right.session_id && left.engine_revision == right.engine_revision &&
           left.authority_contract == right.authority_contract &&
           left.session_created_at_utc == right.session_created_at_utc && left.pins == right.pins;
}

[[nodiscard]] bool segmentAncestorOrEqual(const SegmentMap& segments,
                                          const SyncObjectId& possible_ancestor,
                                          const SyncObjectId& descendant,
                                          std::size_t maximum_depth) {
    auto current = descendant;
    for (std::size_t depth = 0; depth < maximum_depth; ++depth) {
        if (current == possible_ancestor) {
            return true;
        }
        const auto found = segments.find(current);
        if (found == segments.end() || !found->second->segment.parent_segment_id) {
            return false;
        }
        current = *found->second->segment.parent_segment_id;
    }
    return false;
}

[[nodiscard]] bool checkpointAncestor(const CheckpointMap& checkpoints,
                                      const SyncObjectId& possible_ancestor,
                                      const SyncObjectId& descendant, std::size_t maximum_depth) {
    std::vector<std::pair<SyncObjectId, std::size_t>> pending{{descendant, 0U}};
    std::set<SyncObjectId> visited;
    while (!pending.empty()) {
        const auto [current, depth] = pending.back();
        pending.pop_back();
        if (depth >= maximum_depth || !visited.insert(current).second) {
            continue;
        }
        const auto found = checkpoints.find(current);
        if (found == checkpoints.end()) {
            continue;
        }
        for (const auto& parent : found->second->checkpoint.parent_checkpoint_ids) {
            if (parent == possible_ancestor) {
                return true;
            }
            pending.emplace_back(parent, depth + 1U);
        }
    }
    return false;
}

template <typename Map, typename Parents>
[[nodiscard]] auto topologicalOrder(const Map& objects, Parents parents, std::size_t maximum_depth,
                                    CheckpointGraphErrorCode invalid_root_code)
    -> std::expected<std::vector<SyncObjectId>, CheckpointGraphError> {
    std::map<SyncObjectId, std::size_t> indegrees;
    std::map<SyncObjectId, std::vector<SyncObjectId>> children;
    std::size_t roots{};
    for (const auto& [id, object] : objects) {
        const auto object_parents = parents(*object);
        indegrees.emplace(id, object_parents.size());
        if (object_parents.empty()) {
            ++roots;
        }
        for (const auto& parent : object_parents) {
            children[parent].push_back(id);
        }
    }
    if (roots == 0U) {
        return fail(CheckpointGraphErrorCode::Cycle,
                    QStringLiteral("Object graph has no acyclic root"));
    }
    if (roots != 1U) {
        return fail(invalid_root_code, QStringLiteral("Object graph has multiple roots"));
    }

    std::set<SyncObjectId> ready;
    std::map<SyncObjectId, std::size_t> depths;
    for (const auto& [id, degree] : indegrees) {
        if (degree == 0U) {
            ready.insert(id);
            depths.emplace(id, 1U);
        }
    }

    std::vector<SyncObjectId> order;
    order.reserve(objects.size());
    while (!ready.empty()) {
        const auto id = *ready.begin();
        ready.erase(ready.begin());
        order.push_back(id);
        const auto depth = depths.at(id);
        if (depth > maximum_depth) {
            return fail(CheckpointGraphErrorCode::LimitExceeded,
                        QStringLiteral("Object ancestry exceeds its depth limit"));
        }
        for (const auto& child : children[id]) {
            auto& child_depth = depths[child];
            child_depth = std::max(child_depth, depth + 1U);
            auto& degree = indegrees[child];
            --degree;
            if (degree == 0U) {
                ready.insert(child);
            }
        }
    }
    if (order.size() != objects.size()) {
        return fail(CheckpointGraphErrorCode::Cycle,
                    QStringLiteral("Object graph contains a cycle"));
    }
    return order;
}

[[nodiscard]] const IdentifiedSessionEventSegment*
findSegment(const std::vector<IdentifiedSessionEventSegment>& segments, const SyncObjectId& id) {
    const auto found =
        std::ranges::lower_bound(segments, id, {}, &IdentifiedSessionEventSegment::canonical_id);
    return found != segments.end() && found->canonical_id == id ? &*found : nullptr;
}

[[nodiscard]] const IdentifiedCheckpoint*
findCheckpoint(const std::vector<IdentifiedCheckpoint>& checkpoints, const SyncObjectId& id) {
    const auto found =
        std::ranges::lower_bound(checkpoints, id, {}, &IdentifiedCheckpoint::canonical_id);
    return found != checkpoints.end() && found->canonical_id == id ? &*found : nullptr;
}

} // namespace

CheckpointGraph::CheckpointGraph(std::vector<IdentifiedSessionEventSegment> segments,
                                 std::vector<IdentifiedCheckpoint> checkpoints,
                                 std::vector<SyncObjectId> segment_topological_order,
                                 std::vector<SyncObjectId> checkpoint_topological_order,
                                 std::vector<BranchHead> current_heads,
                                 CheckpointGraphLimits limits) noexcept
    : segments_(std::move(segments)), checkpoints_(std::move(checkpoints)),
      segment_topological_order_(std::move(segment_topological_order)),
      checkpoint_topological_order_(std::move(checkpoint_topological_order)),
      current_heads_(std::move(current_heads)), limits_(limits) {}

std::expected<CheckpointGraph, CheckpointGraphError>
CheckpointGraph::build(std::vector<IdentifiedSessionEventSegment> segments,
                       std::vector<IdentifiedCheckpoint> checkpoints,
                       CheckpointGraphLimits limits) {
    if (const auto valid_limits = validateLimits(limits); !valid_limits) {
        return std::unexpected(valid_limits.error());
    }
    if (segments.empty() || checkpoints.empty()) {
        return fail(CheckpointGraphErrorCode::InvalidArgument,
                    QStringLiteral("Checkpoint graph requires segments and checkpoints"));
    }
    if (segments.size() > limits.maximum_segments ||
        checkpoints.size() > limits.maximum_checkpoints) {
        return fail(CheckpointGraphErrorCode::LimitExceeded,
                    QStringLiteral("Checkpoint graph exceeds its object-count limit"));
    }

    for (const auto& segment : segments) {
        if (const auto valid = validateSegmentIdentity(segment, limits.object_limits); !valid) {
            return std::unexpected(valid.error());
        }
    }
    for (const auto& checkpoint : checkpoints) {
        if (const auto valid = validateCheckpointIdentity(checkpoint, limits.object_limits);
            !valid) {
            return std::unexpected(valid.error());
        }
    }
    std::ranges::sort(segments, {}, &IdentifiedSessionEventSegment::canonical_id);
    std::ranges::sort(checkpoints, {}, &IdentifiedCheckpoint::canonical_id);
    if (duplicateIdentities(segments) || duplicateIdentities(checkpoints)) {
        return fail(CheckpointGraphErrorCode::DuplicateObject,
                    QStringLiteral("Checkpoint graph contains a duplicate object identity"));
    }

    SegmentMap segment_map;
    for (const auto& segment : segments) {
        segment_map.emplace(segment.canonical_id, &segment);
    }
    CheckpointMap checkpoint_map;
    for (const auto& checkpoint : checkpoints) {
        checkpoint_map.emplace(checkpoint.canonical_id, &checkpoint);
    }

    for (const auto& segment : segments) {
        if (!segment.segment.parent_segment_id) {
            continue;
        }
        const auto parent = segment_map.find(*segment.segment.parent_segment_id);
        if (parent == segment_map.end()) {
            return fail(CheckpointGraphErrorCode::MissingObject,
                        QStringLiteral("Session-event segment parent is missing"));
        }
        const auto parent_final =
            LogicalObjectCodec::finalSequence(parent->second->segment, limits.object_limits);
        if (!parent_final || segment.segment.base_sequence != *parent_final) {
            return fail(CheckpointGraphErrorCode::SequenceMismatch,
                        QStringLiteral("Session-event segment sequence is discontinuous"));
        }
    }
    const auto segment_order = topologicalOrder(
        segment_map,
        [](const IdentifiedSessionEventSegment& value) {
            return value.segment.parent_segment_id
                       ? std::vector<SyncObjectId>{*value.segment.parent_segment_id}
                       : std::vector<SyncObjectId>{};
        },
        limits.maximum_ancestry_depth, CheckpointGraphErrorCode::InvalidAncestry);
    if (!segment_order) {
        return std::unexpected(segment_order.error());
    }

    const auto& metadata_checkpoint = checkpoints.front().checkpoint;
    for (const auto& segment : segments) {
        if (!sameSessionMetadata(segment.segment, metadata_checkpoint)) {
            return fail(CheckpointGraphErrorCode::MetadataMismatch,
                        QStringLiteral("Segment and checkpoint session metadata differ"));
        }
    }
    for (const auto& checkpoint : checkpoints) {
        if (!sameCheckpointMetadata(checkpoint.checkpoint, metadata_checkpoint)) {
            return fail(CheckpointGraphErrorCode::MetadataMismatch,
                        QStringLiteral("Checkpoint session metadata or pins differ"));
        }
        const auto head = segment_map.find(checkpoint.checkpoint.head_segment_id);
        if (head == segment_map.end()) {
            return fail(CheckpointGraphErrorCode::MissingObject,
                        QStringLiteral("Checkpoint head segment is missing"));
        }
        const auto head_sequence =
            LogicalObjectCodec::finalSequence(head->second->segment, limits.object_limits);
        if (!head_sequence || checkpoint.checkpoint.head_sequence != *head_sequence) {
            return fail(CheckpointGraphErrorCode::SequenceMismatch,
                        QStringLiteral("Checkpoint head sequence does not match its segment"));
        }
        for (const auto& parent : checkpoint.checkpoint.parent_checkpoint_ids) {
            if (!checkpoint_map.contains(parent)) {
                return fail(CheckpointGraphErrorCode::MissingObject,
                            QStringLiteral("Checkpoint parent is missing"));
            }
        }
    }

    const auto checkpoint_order = topologicalOrder(
        checkpoint_map,
        [](const IdentifiedCheckpoint& value) { return value.checkpoint.parent_checkpoint_ids; },
        limits.maximum_ancestry_depth, CheckpointGraphErrorCode::InvalidAncestry);
    if (!checkpoint_order) {
        return std::unexpected(checkpoint_order.error());
    }

    for (const auto& checkpoint_id : *checkpoint_order) {
        const auto& checkpoint = checkpoint_map.at(checkpoint_id)->checkpoint;
        if (checkpoint.parent_checkpoint_ids.size() == 1U) {
            const auto& parent =
                checkpoint_map.at(checkpoint.parent_checkpoint_ids.front())->checkpoint;
            if (!segmentAncestorOrEqual(segment_map, parent.head_segment_id,
                                        checkpoint.head_segment_id,
                                        limits.maximum_ancestry_depth)) {
                return fail(CheckpointGraphErrorCode::InvalidAncestry,
                            QStringLiteral("Linear checkpoint does not extend its parent history"));
            }
        } else if (checkpoint.parent_checkpoint_ids.size() >= 2U) {
            for (std::size_t left = 0; left < checkpoint.parent_checkpoint_ids.size(); ++left) {
                for (std::size_t right = left + 1U; right < checkpoint.parent_checkpoint_ids.size();
                     ++right) {
                    const auto& left_id = checkpoint.parent_checkpoint_ids[left];
                    const auto& right_id = checkpoint.parent_checkpoint_ids[right];
                    if (checkpointAncestor(checkpoint_map, left_id, right_id,
                                           limits.maximum_ancestry_depth) ||
                        checkpointAncestor(checkpoint_map, right_id, left_id,
                                           limits.maximum_ancestry_depth)) {
                        return fail(
                            CheckpointGraphErrorCode::InvalidAncestry,
                            QStringLiteral("Resolution checkpoint parents are not concurrent"));
                    }
                }
            }
            const auto& selected =
                checkpoint_map.at(*checkpoint.selected_base_checkpoint_id)->checkpoint;
            if (!segmentAncestorOrEqual(segment_map, selected.head_segment_id,
                                        checkpoint.head_segment_id,
                                        limits.maximum_ancestry_depth)) {
                return fail(
                    CheckpointGraphErrorCode::InvalidAncestry,
                    QStringLiteral("Resolution checkpoint does not extend its selected base"));
            }
        }
    }

    std::set<SyncObjectId> referenced_segments;
    for (const auto& checkpoint : checkpoints) {
        auto current = checkpoint.checkpoint.head_segment_id;
        for (std::size_t depth = 0; depth < limits.maximum_ancestry_depth; ++depth) {
            if (!referenced_segments.insert(current).second) {
                break;
            }
            const auto& segment = segment_map.at(current)->segment;
            if (!segment.parent_segment_id) {
                break;
            }
            current = *segment.parent_segment_id;
        }
    }
    if (referenced_segments.size() != segments.size()) {
        return fail(CheckpointGraphErrorCode::OrphanObject,
                    QStringLiteral("Checkpoint graph contains an unreferenced segment"));
    }

    std::set<SyncObjectId> checkpoint_parents;
    for (const auto& checkpoint : checkpoints) {
        checkpoint_parents.insert(checkpoint.checkpoint.parent_checkpoint_ids.begin(),
                                  checkpoint.checkpoint.parent_checkpoint_ids.end());
    }
    std::vector<BranchHead> heads;
    for (const auto& checkpoint : checkpoints) {
        if (!checkpoint_parents.contains(checkpoint.canonical_id)) {
            heads.push_back(BranchHead{checkpoint.canonical_id,
                                       checkpoint.checkpoint.head_segment_id,
                                       checkpoint.checkpoint.head_sequence,
                                       checkpoint.checkpoint.parent_checkpoint_ids.size() >= 2U});
        }
    }
    std::ranges::sort(heads, {}, &BranchHead::checkpoint_id);

    return CheckpointGraph{std::move(segments), std::move(checkpoints), *segment_order,
                           *checkpoint_order,   std::move(heads),       limits};
}

const std::vector<BranchHead>& CheckpointGraph::currentHeads() const noexcept {
    return current_heads_;
}

std::expected<RestoreImportPlan, CheckpointGraphError>
CheckpointGraph::makeImportPlan(const SyncObjectId& selected_checkpoint_id,
                                const RestoreExpectations& expectations) const {
    const auto selected_head =
        std::ranges::find(current_heads_, selected_checkpoint_id, &BranchHead::checkpoint_id);
    if (selected_head == current_heads_.end()) {
        return fail(CheckpointGraphErrorCode::NotCurrentHead,
                    QStringLiteral("Restore selection is not a current branch head"));
    }
    const auto* selected = findCheckpoint(checkpoints_, selected_checkpoint_id);
    if (selected == nullptr) {
        return fail(CheckpointGraphErrorCode::MissingObject,
                    QStringLiteral("Selected checkpoint is unavailable"));
    }
    const auto& checkpoint = selected->checkpoint;
    if (expectations.session_id != checkpoint.session_id ||
        expectations.engine_revision != checkpoint.engine_revision ||
        expectations.authority_contract != checkpoint.authority_contract ||
        expectations.session_created_at_utc != checkpoint.session_created_at_utc) {
        return fail(CheckpointGraphErrorCode::MetadataMismatch,
                    QStringLiteral("Restore expectations do not match session metadata"));
    }
    if (expectations.pins != checkpoint.pins) {
        return fail(CheckpointGraphErrorCode::PinsMismatch,
                    QStringLiteral("Restore expectations do not match revision pins"));
    }
    if (expectations.projection_digest != checkpoint.projection_digest) {
        return fail(CheckpointGraphErrorCode::ProjectionMismatch,
                    QStringLiteral("Restore projection digest does not match the selected head"));
    }
    if (!strictlySortedIds(expectations.available_authored_revision_ids) ||
        std::ranges::any_of(expectations.available_authored_revision_ids,
                            [](const SyncObjectId& id) { return isAllZero(id); })) {
        return fail(CheckpointGraphErrorCode::InvalidArgument,
                    QStringLiteral("Available authored-revision identities are not canonical"));
    }

    std::set<SyncObjectId> checkpoint_ancestry_ids;
    std::vector<SyncObjectId> pending_checkpoints{selected_checkpoint_id};
    while (!pending_checkpoints.empty()) {
        const auto current = pending_checkpoints.back();
        pending_checkpoints.pop_back();
        if (!checkpoint_ancestry_ids.insert(current).second) {
            continue;
        }
        const auto* item = findCheckpoint(checkpoints_, current);
        if (item == nullptr) {
            return fail(CheckpointGraphErrorCode::MissingObject,
                        QStringLiteral("Checkpoint ancestry became incomplete"));
        }
        pending_checkpoints.insert(pending_checkpoints.end(),
                                   item->checkpoint.parent_checkpoint_ids.begin(),
                                   item->checkpoint.parent_checkpoint_ids.end());
    }

    std::set<SyncObjectId> segment_ancestry_ids;
    std::set<SyncObjectId> authored_ids;
    for (const auto& checkpoint_id : checkpoint_ancestry_ids) {
        const auto* item = findCheckpoint(checkpoints_, checkpoint_id);
        authored_ids.insert(item->checkpoint.authored_revision_ids.begin(),
                            item->checkpoint.authored_revision_ids.end());
        auto current = item->checkpoint.head_segment_id;
        for (std::size_t depth = 0; depth < limits_.maximum_ancestry_depth; ++depth) {
            if (!segment_ancestry_ids.insert(current).second) {
                break;
            }
            const auto* segment = findSegment(segments_, current);
            if (segment == nullptr) {
                return fail(CheckpointGraphErrorCode::MissingObject,
                            QStringLiteral("Segment ancestry became incomplete"));
            }
            if (!segment->segment.parent_segment_id) {
                break;
            }
            current = *segment->segment.parent_segment_id;
        }
    }
    for (const auto& authored_id : authored_ids) {
        if (!std::binary_search(expectations.available_authored_revision_ids.begin(),
                                expectations.available_authored_revision_ids.end(), authored_id)) {
            return fail(
                CheckpointGraphErrorCode::MissingAuthoredRevision,
                QStringLiteral("Restore ancestry references an unavailable authored revision"));
        }
    }

    std::vector<SyncObjectId> selected_path;
    auto current_segment = checkpoint.head_segment_id;
    for (std::size_t depth = 0; depth < limits_.maximum_ancestry_depth; ++depth) {
        selected_path.push_back(current_segment);
        const auto* segment = findSegment(segments_, current_segment);
        if (segment == nullptr) {
            return fail(CheckpointGraphErrorCode::MissingObject,
                        QStringLiteral("Selected segment ancestry became incomplete"));
        }
        if (!segment->segment.parent_segment_id) {
            break;
        }
        current_segment = *segment->segment.parent_segment_id;
    }
    std::ranges::reverse(selected_path);
    QSet<QString> command_ids;
    for (const auto& segment_id : selected_path) {
        const auto* segment = findSegment(segments_, segment_id);
        for (const auto& batch : segment->segment.batches) {
            if (command_ids.contains(batch.command_id)) {
                return fail(CheckpointGraphErrorCode::DuplicateCommand,
                            QStringLiteral("Selected event ancestry repeats a command identifier"));
            }
            command_ids.insert(batch.command_id);
        }
    }

    RestoreImportPlan plan;
    plan.selected_checkpoint_id = selected_checkpoint_id;
    plan.current_heads = current_heads_;
    for (const auto& id : checkpoint_topological_order_) {
        if (checkpoint_ancestry_ids.contains(id)) {
            plan.checkpoint_ancestry.push_back(*findCheckpoint(checkpoints_, id));
        }
    }
    for (const auto& id : segment_topological_order_) {
        if (segment_ancestry_ids.contains(id)) {
            plan.segment_ancestry.push_back(*findSegment(segments_, id));
        }
    }
    plan.selected_segment_path = std::move(selected_path);
    plan.referenced_authored_revision_ids.assign(authored_ids.begin(), authored_ids.end());
    return plan;
}

std::expected<Checkpoint, CheckpointGraphError>
CheckpointGraph::makeResolutionCheckpoint(const SyncObjectId& selected_base_checkpoint_id) const {
    if (current_heads_.size() < 2U) {
        return fail(CheckpointGraphErrorCode::NoConflict,
                    QStringLiteral("Checkpoint graph has no branch conflict to resolve"));
    }
    if (current_heads_.size() > limits_.object_limits.maximum_checkpoint_parents) {
        return fail(CheckpointGraphErrorCode::LimitExceeded,
                    QStringLiteral("Branch conflict has too many heads for one resolution"));
    }
    const auto selected_head =
        std::ranges::find(current_heads_, selected_base_checkpoint_id, &BranchHead::checkpoint_id);
    if (selected_head == current_heads_.end()) {
        return fail(CheckpointGraphErrorCode::NotCurrentHead,
                    QStringLiteral("Resolution base is not a current branch head"));
    }
    const auto* base = findCheckpoint(checkpoints_, selected_base_checkpoint_id);
    if (base == nullptr) {
        return fail(CheckpointGraphErrorCode::MissingObject,
                    QStringLiteral("Resolution base checkpoint is unavailable"));
    }

    Checkpoint resolution = base->checkpoint;
    resolution.parent_checkpoint_ids.clear();
    resolution.parent_checkpoint_ids.reserve(current_heads_.size());
    for (const auto& head : current_heads_) {
        resolution.parent_checkpoint_ids.push_back(head.checkpoint_id);
    }
    resolution.selected_base_checkpoint_id = selected_base_checkpoint_id;
    if (const auto valid = LogicalObjectCodec::encodeCheckpoint(resolution, limits_.object_limits);
        !valid) {
        return fail(CheckpointGraphErrorCode::InvalidLogicalObject,
                    QStringLiteral("Generated resolution checkpoint is invalid"));
    }
    return resolution;
}

} // namespace appellate::sync
