#include "appellate/sync/checkpoint_graph.hpp"

#include "appellate/sync/protocol_codec.hpp"

#include <QBuffer>
#include <QByteArray>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

namespace {

using appellate::sync::Checkpoint;
using appellate::sync::CheckpointGraph;
using appellate::sync::CheckpointGraphErrorCode;
using appellate::sync::CheckpointGraphLimits;
using appellate::sync::IdentifiedCheckpoint;
using appellate::sync::IdentifiedSessionEventSegment;
using appellate::sync::LogicalAuthorityContract;
using appellate::sync::LogicalObjectCodec;
using appellate::sync::LogicalRevisionPin;
using appellate::sync::ProtocolCodec;
using appellate::sync::ProtocolKeySet;
using appellate::sync::ProtocolKeySlot;
using appellate::sync::RestoreExpectations;
using appellate::sync::SegmentCommandBatch;
using appellate::sync::SegmentEvent;
using appellate::sync::SessionEventSegment;
using appellate::sync::SyncObjectId;
using appellate::sync::SyncObjectKind;

[[nodiscard]] SyncObjectId objectId(unsigned char marker) {
    SyncObjectId id{};
    id.fill(marker);
    return id;
}

[[nodiscard]] std::vector<LogicalRevisionPin> pins() {
    return {
        LogicalRevisionPin{QStringLiteral("appeal.core"), QStringLiteral("1.0.0"), objectId(0x21U)},
        LogicalRevisionPin{QStringLiteral("appeal.rules"), QStringLiteral("2.0.0"),
                           objectId(0x22U)},
    };
}

[[nodiscard]] SessionEventSegment
makeSegment(QString command_id, std::uint64_t base_sequence,
            std::optional<SyncObjectId> parent_segment_id = std::nullopt) {
    const auto payload =
        QByteArrayLiteral("exact-event:") + command_id.toLatin1() + QByteArray::fromHex("00ff");
    return SessionEventSegment{
        QStringLiteral("session.one"),
        QStringLiteral("engine.v1"),
        LogicalAuthorityContract::CanonicalV2,
        base_sequence,
        parent_segment_id,
        {SegmentCommandBatch{
            base_sequence,
            std::move(command_id),
            QStringLiteral("2026-08-11T00:00:00Z"),
            QByteArrayLiteral("exact-command\0payload"),
            {SegmentEvent{QStringLiteral("session.committed"), QStringLiteral("authority.engine"),
                          payload}},
        }},
    };
}

[[nodiscard]] Checkpoint makeCheckpoint(const SyncObjectId& head_segment_id,
                                        std::uint64_t head_sequence,
                                        std::vector<SyncObjectId> parents,
                                        unsigned char projection_marker,
                                        std::vector<SyncObjectId> authored_revision_ids = {},
                                        std::optional<SyncObjectId> selected_base = std::nullopt) {
    std::ranges::sort(parents);
    std::ranges::sort(authored_revision_ids);
    return Checkpoint{
        QStringLiteral("session.one"),
        QStringLiteral("engine.v1"),
        LogicalAuthorityContract::CanonicalV2,
        QStringLiteral("2026-08-11T00:00:00Z"),
        head_segment_id,
        head_sequence,
        objectId(projection_marker),
        pins(),
        std::move(parents),
        selected_base,
        std::move(authored_revision_ids),
    };
}

[[nodiscard]] IdentifiedSessionEventSegment identifySegment(SessionEventSegment segment) {
    const auto payload = LogicalObjectCodec::encodeSessionEventSegment(segment).value();
    const auto identity =
        ProtocolCodec::canonicalIdentity(SyncObjectKind::SessionEventSegment,
                                         LogicalObjectCodec::session_event_segment_schema_version,
                                         payload, objectId(0x5aU))
            .value();
    return IdentifiedSessionEventSegment{identity.canonical_id, std::move(segment)};
}

[[nodiscard]] IdentifiedCheckpoint identifyCheckpoint(Checkpoint checkpoint) {
    const auto payload = LogicalObjectCodec::encodeCheckpoint(checkpoint).value();
    const auto identity =
        ProtocolCodec::canonicalIdentity(SyncObjectKind::Checkpoint,
                                         LogicalObjectCodec::checkpoint_schema_version, payload,
                                         objectId(0x5aU))
            .value();
    return IdentifiedCheckpoint{identity.canonical_id, std::move(checkpoint)};
}

struct ForkFixture final {
    std::vector<IdentifiedSessionEventSegment> segments;
    std::vector<IdentifiedCheckpoint> checkpoints;
    SyncObjectId root_segment_id{};
    SyncObjectId branch_a_segment_id{};
    SyncObjectId branch_b_segment_id{};
    SyncObjectId root_checkpoint_id{};
    SyncObjectId branch_a_checkpoint_id{};
    SyncObjectId branch_b_checkpoint_id{};
    SyncObjectId authored_a_id{};
    SyncObjectId authored_b_id{};
};

[[nodiscard]] ForkFixture makeForkFixture() {
    const auto root_segment = identifySegment(makeSegment(QStringLiteral("command.root"), 0));
    const auto branch_a_segment =
        identifySegment(makeSegment(QStringLiteral("command.a"), 1, root_segment.canonical_id));
    const auto branch_b_segment =
        identifySegment(makeSegment(QStringLiteral("command.b"), 1, root_segment.canonical_id));

    const auto authored_a = objectId(0xa1U);
    const auto authored_b = objectId(0xb1U);
    const auto root_checkpoint =
        identifyCheckpoint(makeCheckpoint(root_segment.canonical_id, 1, {}, 0x90U));
    const auto branch_a_checkpoint = identifyCheckpoint(makeCheckpoint(
        branch_a_segment.canonical_id, 2, {root_checkpoint.canonical_id}, 0x91U, {authored_a}));
    const auto branch_b_checkpoint = identifyCheckpoint(makeCheckpoint(
        branch_b_segment.canonical_id, 2, {root_checkpoint.canonical_id}, 0x92U, {authored_b}));

    return ForkFixture{
        {branch_b_segment, root_segment, branch_a_segment},
        {branch_b_checkpoint, root_checkpoint, branch_a_checkpoint},
        root_segment.canonical_id,
        branch_a_segment.canonical_id,
        branch_b_segment.canonical_id,
        root_checkpoint.canonical_id,
        branch_a_checkpoint.canonical_id,
        branch_b_checkpoint.canonical_id,
        authored_a,
        authored_b,
    };
}

[[nodiscard]] const IdentifiedCheckpoint& checkpointWithId(const ForkFixture& fixture,
                                                           const SyncObjectId& id) {
    return *std::ranges::find(fixture.checkpoints, id, &IdentifiedCheckpoint::canonical_id);
}

[[nodiscard]] RestoreExpectations
expectationsFor(const Checkpoint& checkpoint,
                std::vector<SyncObjectId> available_authored_revision_ids) {
    std::ranges::sort(available_authored_revision_ids);
    return RestoreExpectations{
        checkpoint.session_id,
        checkpoint.engine_revision,
        checkpoint.authority_contract,
        checkpoint.session_created_at_utc,
        checkpoint.pins,
        checkpoint.projection_digest,
        std::move(available_authored_revision_ids),
    };
}

template <typename Identified>
[[nodiscard]] std::vector<SyncObjectId> identities(const std::vector<Identified>& values) {
    std::vector<SyncObjectId> result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(value.canonical_id);
    }
    return result;
}

[[nodiscard]] bool containsId(const std::vector<SyncObjectId>& values, const SyncObjectId& id) {
    return std::ranges::find(values, id) != values.end();
}

[[nodiscard]] ProtocolKeySet deterministicKeys() {
    ProtocolKeySet keys;
    for (std::size_t index = 0; index < keys.object_id_key.size(); ++index) {
        keys.object_id_key[index] = static_cast<unsigned char>(0x5aU + index);
    }
    ProtocolKeySlot slot;
    for (std::size_t index = 0; index < slot.id.size(); ++index) {
        slot.id[index] = static_cast<unsigned char>(0xa0U + index);
    }
    for (std::size_t index = 0; index < slot.encryption_key.size(); ++index) {
        slot.encryption_key[index] = static_cast<unsigned char>(0x40U + index);
    }
    keys.key_slots.push_back(slot);
    return keys;
}

} // namespace

class CheckpointGraphTest final : public QObject {
    Q_OBJECT

  private slots:
    void plansOneSelectedConcurrentBranch();
    void resolutionRoundTripsThroughEnvelopeAndCollapsesHeads();
    void rejectsIdentityDuplicatesCountsAndDepth();
    void rejectsCycleAttemptsAtContentIdentityGate();
    void rejectsMultipleRoots();
    void rejectsMissingDiscontinuousAndOrphanObjects();
    void rejectsMetadataPinsAndHeadSequenceMismatch();
    void rejectsSegmentCheckpointMetadataMismatch();
    void rejectsInvalidLinearAndMergeHeads();
    void rejectsZeroIdentifiedObjectIds();
    void rejectsEveryRestoreMetadataMismatchAndMissingHead();
    void rejectsAncestralMergeParentsAndDuplicateCommands();
};

void CheckpointGraphTest::plansOneSelectedConcurrentBranch() {
    const auto fixture = makeForkFixture();
    const auto graph = CheckpointGraph::build(fixture.segments, fixture.checkpoints);
    QVERIFY(graph.has_value());
    QCOMPARE(graph->currentHeads().size(), std::size_t{2});
    QVERIFY(graph->currentHeads().front().checkpoint_id <
            graph->currentHeads().back().checkpoint_id);

    const auto& selected = checkpointWithId(fixture, fixture.branch_a_checkpoint_id).checkpoint;
    auto expectations = expectationsFor(selected, {fixture.authored_a_id, objectId(0xf0U)});
    const auto plan = graph->makeImportPlan(fixture.branch_a_checkpoint_id, expectations);
    QVERIFY(plan.has_value());
    QCOMPARE(plan->selected_checkpoint_id, fixture.branch_a_checkpoint_id);
    QCOMPARE(plan->current_heads, graph->currentHeads());
    QCOMPARE(plan->checkpoint_ancestry.size(), std::size_t{2});
    QCOMPARE(identities(plan->checkpoint_ancestry).front(), fixture.root_checkpoint_id);
    QCOMPARE(identities(plan->checkpoint_ancestry).back(), fixture.branch_a_checkpoint_id);
    QCOMPARE(plan->segment_ancestry.size(), std::size_t{2});
    QCOMPARE(identities(plan->segment_ancestry).front(), fixture.root_segment_id);
    QCOMPARE(identities(plan->segment_ancestry).back(), fixture.branch_a_segment_id);
    QCOMPARE(plan->selected_segment_path,
             std::vector<SyncObjectId>({fixture.root_segment_id, fixture.branch_a_segment_id}));
    QCOMPARE(plan->referenced_authored_revision_ids,
             std::vector<SyncObjectId>({fixture.authored_a_id}));

    expectations.available_authored_revision_ids.clear();
    const auto missing_authored =
        graph->makeImportPlan(fixture.branch_a_checkpoint_id, expectations);
    QVERIFY(!missing_authored.has_value());
    QCOMPARE(missing_authored.error().code, CheckpointGraphErrorCode::MissingAuthoredRevision);

    expectations = expectationsFor(selected, {fixture.authored_a_id});
    expectations.projection_digest = objectId(0xeeU);
    const auto wrong_projection =
        graph->makeImportPlan(fixture.branch_a_checkpoint_id, expectations);
    QVERIFY(!wrong_projection.has_value());
    QCOMPARE(wrong_projection.error().code, CheckpointGraphErrorCode::ProjectionMismatch);

    expectations = expectationsFor(selected, {fixture.authored_a_id});
    expectations.pins.front().revision_digest = objectId(0xeeU);
    const auto wrong_pins = graph->makeImportPlan(fixture.branch_a_checkpoint_id, expectations);
    QVERIFY(!wrong_pins.has_value());
    QCOMPARE(wrong_pins.error().code, CheckpointGraphErrorCode::PinsMismatch);

    expectations = expectationsFor(selected, {objectId(0xf0U), fixture.authored_a_id});
    std::ranges::reverse(expectations.available_authored_revision_ids);
    const auto unsorted_available =
        graph->makeImportPlan(fixture.branch_a_checkpoint_id, expectations);
    QVERIFY(!unsorted_available.has_value());
    QCOMPARE(unsorted_available.error().code, CheckpointGraphErrorCode::InvalidArgument);

    const auto stale = graph->makeImportPlan(
        fixture.root_checkpoint_id,
        expectationsFor(checkpointWithId(fixture, fixture.root_checkpoint_id).checkpoint, {}));
    QVERIFY(!stale.has_value());
    QCOMPARE(stale.error().code, CheckpointGraphErrorCode::NotCurrentHead);
}

void CheckpointGraphTest::resolutionRoundTripsThroughEnvelopeAndCollapsesHeads() {
    auto fixture = makeForkFixture();
    const auto original_segments = fixture.segments;
    const auto original_checkpoints = fixture.checkpoints;
    const auto graph = CheckpointGraph::build(fixture.segments, fixture.checkpoints);
    QVERIFY(graph.has_value());

    const auto resolution = graph->makeResolutionCheckpoint(fixture.branch_a_checkpoint_id);
    QVERIFY(resolution.has_value());
    QVERIFY(fixture.segments == original_segments);
    QVERIFY(fixture.checkpoints == original_checkpoints);
    QCOMPARE(resolution->head_segment_id, fixture.branch_a_segment_id);
    QCOMPARE(resolution->head_sequence, std::uint64_t{2});
    QCOMPARE(
        resolution->projection_digest,
        checkpointWithId(fixture, fixture.branch_a_checkpoint_id).checkpoint.projection_digest);
    QCOMPARE(resolution->pins, pins());
    QCOMPARE(resolution->authored_revision_ids, std::vector<SyncObjectId>({fixture.authored_a_id}));
    QVERIFY(resolution->selected_base_checkpoint_id.has_value());
    QCOMPARE(*resolution->selected_base_checkpoint_id, fixture.branch_a_checkpoint_id);
    std::vector<SyncObjectId> expected_parents;
    for (const auto& head : graph->currentHeads()) {
        expected_parents.push_back(head.checkpoint_id);
    }
    QCOMPARE(resolution->parent_checkpoint_ids, expected_parents);

    const auto logical_payload = LogicalObjectCodec::encodeCheckpoint(*resolution);
    QVERIFY(logical_payload.has_value());
    auto payload = *logical_payload;
    QBuffer plaintext(&payload);
    QVERIFY(plaintext.open(QIODevice::ReadOnly));
    QByteArray ciphertext_bytes;
    QBuffer ciphertext_destination(&ciphertext_bytes);
    QVERIFY(ciphertext_destination.open(QIODevice::WriteOnly));
    const auto keys = deterministicKeys();
    const auto encrypted = ProtocolCodec::encrypt(
        SyncObjectKind::Checkpoint, LogicalObjectCodec::checkpoint_schema_version, plaintext,
        static_cast<std::uint64_t>(payload.size()), keys, ciphertext_destination);
    QVERIFY(encrypted.has_value());

    QTemporaryDir quarantine;
    QVERIFY(quarantine.isValid());
    QBuffer ciphertext_source(&ciphertext_bytes);
    QVERIFY(ciphertext_source.open(QIODevice::ReadOnly));
    auto decrypted = ProtocolCodec::decrypt(ciphertext_source, encrypted->identity.remote_object_id,
                                            keys, quarantine.path());
    QVERIFY(decrypted.has_value());
    QCOMPARE(decrypted->kind, SyncObjectKind::Checkpoint);
    QCOMPARE(decrypted->schema_version, LogicalObjectCodec::checkpoint_schema_version);
    const auto decoded =
        LogicalObjectCodec::decodeCheckpoint(decrypted->quarantined_payload->readAll());
    QVERIFY(decoded.has_value());
    QCOMPARE(*decoded, *resolution);

    const auto resolution_id = encrypted->identity.canonical_id;
    fixture.checkpoints.push_back(IdentifiedCheckpoint{resolution_id, *decoded});
    const auto resolved_graph = CheckpointGraph::build(fixture.segments, fixture.checkpoints);
    QVERIFY(resolved_graph.has_value());
    QCOMPARE(resolved_graph->currentHeads().size(), std::size_t{1});
    QCOMPARE(resolved_graph->currentHeads().front().checkpoint_id, resolution_id);
    QVERIFY(resolved_graph->currentHeads().front().is_resolution);

    const auto import_plan = resolved_graph->makeImportPlan(
        resolution_id,
        expectationsFor(*resolution, {fixture.authored_a_id, fixture.authored_b_id}));
    QVERIFY(import_plan.has_value());
    QCOMPARE(import_plan->checkpoint_ancestry.size(), std::size_t{4});
    QCOMPARE(identities(import_plan->checkpoint_ancestry).front(), fixture.root_checkpoint_id);
    QCOMPARE(identities(import_plan->checkpoint_ancestry).back(), resolution_id);
    QCOMPARE(import_plan->segment_ancestry.size(), std::size_t{3});
    const auto segment_ids = identities(import_plan->segment_ancestry);
    QCOMPARE(segment_ids.front(), fixture.root_segment_id);
    QVERIFY(containsId(segment_ids, fixture.branch_a_segment_id));
    QVERIFY(containsId(segment_ids, fixture.branch_b_segment_id));
    QCOMPARE(import_plan->selected_segment_path,
             std::vector<SyncObjectId>({fixture.root_segment_id, fixture.branch_a_segment_id}));
    QCOMPARE(import_plan->referenced_authored_revision_ids,
             std::vector<SyncObjectId>({fixture.authored_a_id, fixture.authored_b_id}));
}

void CheckpointGraphTest::rejectsIdentityDuplicatesCountsAndDepth() {
    auto fixture = makeForkFixture();
    fixture.segments.front().canonical_id.front() ^= 0x01U;
    const auto mismatched = CheckpointGraph::build(fixture.segments, fixture.checkpoints);
    QVERIFY(!mismatched.has_value());
    QCOMPARE(mismatched.error().code, CheckpointGraphErrorCode::IdentityMismatch);

    fixture = makeForkFixture();
    fixture.segments.push_back(fixture.segments.front());
    const auto duplicate = CheckpointGraph::build(fixture.segments, fixture.checkpoints);
    QVERIFY(!duplicate.has_value());
    QCOMPARE(duplicate.error().code, CheckpointGraphErrorCode::DuplicateObject);

    fixture = makeForkFixture();
    auto limits = CheckpointGraphLimits{};
    limits.maximum_segments = 2;
    const auto excessive = CheckpointGraph::build(fixture.segments, fixture.checkpoints, limits);
    QVERIFY(!excessive.has_value());
    QCOMPARE(excessive.error().code, CheckpointGraphErrorCode::LimitExceeded);

    limits = CheckpointGraphLimits{};
    limits.maximum_ancestry_depth = 1;
    const auto too_deep = CheckpointGraph::build(fixture.segments, fixture.checkpoints, limits);
    QVERIFY(!too_deep.has_value());
    QCOMPARE(too_deep.error().code, CheckpointGraphErrorCode::LimitExceeded);
}

void CheckpointGraphTest::rejectsCycleAttemptsAtContentIdentityGate() {
    // Parent references are part of the hashed payload. A cycle cannot retain valid content
    // identities without a hash fixed point, so real cycle attempts reject before DAG traversal.
    // CheckpointGraph's topological cycle check remains defense in depth for that hypothetical.
    auto fixture = makeForkFixture();
    auto root_segment = std::ranges::find(fixture.segments, fixture.root_segment_id,
                                          &IdentifiedSessionEventSegment::canonical_id);
    QVERIFY(root_segment != fixture.segments.end());
    root_segment->segment.parent_segment_id = fixture.branch_a_segment_id;
    root_segment->segment.base_sequence = 2;
    root_segment->segment.batches.front().expected_sequence = 2;
    const auto segment_cycle = CheckpointGraph::build(fixture.segments, fixture.checkpoints);
    QVERIFY(!segment_cycle.has_value());
    QCOMPARE(segment_cycle.error().code, CheckpointGraphErrorCode::IdentityMismatch);

    fixture = makeForkFixture();
    auto root_checkpoint = std::ranges::find(fixture.checkpoints, fixture.root_checkpoint_id,
                                             &IdentifiedCheckpoint::canonical_id);
    QVERIFY(root_checkpoint != fixture.checkpoints.end());
    root_checkpoint->checkpoint.parent_checkpoint_ids = {fixture.branch_a_checkpoint_id};
    const auto checkpoint_cycle = CheckpointGraph::build(fixture.segments, fixture.checkpoints);
    QVERIFY(!checkpoint_cycle.has_value());
    QCOMPARE(checkpoint_cycle.error().code, CheckpointGraphErrorCode::IdentityMismatch);
}

void CheckpointGraphTest::rejectsMultipleRoots() {
    auto fixture = makeForkFixture();
    fixture.segments.push_back(
        identifySegment(makeSegment(QStringLiteral("command.second-root"), 0)));
    const auto segment_roots = CheckpointGraph::build(fixture.segments, fixture.checkpoints);
    QVERIFY(!segment_roots.has_value());
    QCOMPARE(segment_roots.error().code, CheckpointGraphErrorCode::InvalidAncestry);

    fixture = makeForkFixture();
    fixture.checkpoints.push_back(
        identifyCheckpoint(makeCheckpoint(fixture.branch_b_segment_id, 2, {}, 0x93U)));
    const auto checkpoint_roots = CheckpointGraph::build(fixture.segments, fixture.checkpoints);
    QVERIFY(!checkpoint_roots.has_value());
    QCOMPARE(checkpoint_roots.error().code, CheckpointGraphErrorCode::InvalidAncestry);
}

void CheckpointGraphTest::rejectsMissingDiscontinuousAndOrphanObjects() {
    auto fixture = makeForkFixture();
    auto branch_a = *std::ranges::find(fixture.segments, fixture.branch_a_segment_id,
                                       &IdentifiedSessionEventSegment::canonical_id);
    branch_a.segment.parent_segment_id = objectId(0xeeU);
    branch_a = identifySegment(std::move(branch_a.segment));
    *std::ranges::find(fixture.segments, fixture.branch_a_segment_id,
                       &IdentifiedSessionEventSegment::canonical_id) = branch_a;
    auto branch_a_checkpoint = checkpointWithId(fixture, fixture.branch_a_checkpoint_id);
    branch_a_checkpoint.checkpoint.head_segment_id = branch_a.canonical_id;
    branch_a_checkpoint = identifyCheckpoint(std::move(branch_a_checkpoint.checkpoint));
    *std::ranges::find(fixture.checkpoints, fixture.branch_a_checkpoint_id,
                       &IdentifiedCheckpoint::canonical_id) = branch_a_checkpoint;
    const auto missing_segment = CheckpointGraph::build(fixture.segments, fixture.checkpoints);
    QVERIFY(!missing_segment.has_value());
    QCOMPARE(missing_segment.error().code, CheckpointGraphErrorCode::MissingObject);

    fixture = makeForkFixture();
    branch_a = *std::ranges::find(fixture.segments, fixture.branch_a_segment_id,
                                  &IdentifiedSessionEventSegment::canonical_id);
    branch_a.segment.base_sequence = 2;
    branch_a.segment.batches.front().expected_sequence = 2;
    branch_a = identifySegment(std::move(branch_a.segment));
    *std::ranges::find(fixture.segments, fixture.branch_a_segment_id,
                       &IdentifiedSessionEventSegment::canonical_id) = branch_a;
    branch_a_checkpoint = checkpointWithId(fixture, fixture.branch_a_checkpoint_id);
    branch_a_checkpoint.checkpoint.head_segment_id = branch_a.canonical_id;
    branch_a_checkpoint.checkpoint.head_sequence = 3;
    branch_a_checkpoint = identifyCheckpoint(std::move(branch_a_checkpoint.checkpoint));
    *std::ranges::find(fixture.checkpoints, fixture.branch_a_checkpoint_id,
                       &IdentifiedCheckpoint::canonical_id) = branch_a_checkpoint;
    const auto discontinuous = CheckpointGraph::build(fixture.segments, fixture.checkpoints);
    QVERIFY(!discontinuous.has_value());
    QCOMPARE(discontinuous.error().code, CheckpointGraphErrorCode::SequenceMismatch);

    fixture = makeForkFixture();
    fixture.segments.push_back(
        identifySegment(makeSegment(QStringLiteral("command.orphan"), 1, fixture.root_segment_id)));
    const auto orphan = CheckpointGraph::build(fixture.segments, fixture.checkpoints);
    QVERIFY(!orphan.has_value());
    QCOMPARE(orphan.error().code, CheckpointGraphErrorCode::OrphanObject);

    fixture = makeForkFixture();
    branch_a_checkpoint = checkpointWithId(fixture, fixture.branch_a_checkpoint_id);
    branch_a_checkpoint.checkpoint.parent_checkpoint_ids = {objectId(0xeeU)};
    branch_a_checkpoint = identifyCheckpoint(std::move(branch_a_checkpoint.checkpoint));
    *std::ranges::find(fixture.checkpoints, fixture.branch_a_checkpoint_id,
                       &IdentifiedCheckpoint::canonical_id) = branch_a_checkpoint;
    const auto missing_checkpoint = CheckpointGraph::build(fixture.segments, fixture.checkpoints);
    QVERIFY(!missing_checkpoint.has_value());
    QCOMPARE(missing_checkpoint.error().code, CheckpointGraphErrorCode::MissingObject);
}

void CheckpointGraphTest::rejectsMetadataPinsAndHeadSequenceMismatch() {
    auto fixture = makeForkFixture();
    auto branch_a_checkpoint = checkpointWithId(fixture, fixture.branch_a_checkpoint_id);
    branch_a_checkpoint.checkpoint.session_created_at_utc = QStringLiteral("2026-08-12T00:00:00Z");
    branch_a_checkpoint = identifyCheckpoint(std::move(branch_a_checkpoint.checkpoint));
    *std::ranges::find(fixture.checkpoints, fixture.branch_a_checkpoint_id,
                       &IdentifiedCheckpoint::canonical_id) = branch_a_checkpoint;
    const auto metadata = CheckpointGraph::build(fixture.segments, fixture.checkpoints);
    QVERIFY(!metadata.has_value());
    QCOMPARE(metadata.error().code, CheckpointGraphErrorCode::MetadataMismatch);

    fixture = makeForkFixture();
    branch_a_checkpoint = checkpointWithId(fixture, fixture.branch_a_checkpoint_id);
    branch_a_checkpoint.checkpoint.pins.front().revision_digest = objectId(0xeeU);
    branch_a_checkpoint = identifyCheckpoint(std::move(branch_a_checkpoint.checkpoint));
    *std::ranges::find(fixture.checkpoints, fixture.branch_a_checkpoint_id,
                       &IdentifiedCheckpoint::canonical_id) = branch_a_checkpoint;
    const auto mismatched_pins = CheckpointGraph::build(fixture.segments, fixture.checkpoints);
    QVERIFY(!mismatched_pins.has_value());
    QCOMPARE(mismatched_pins.error().code, CheckpointGraphErrorCode::MetadataMismatch);

    fixture = makeForkFixture();
    branch_a_checkpoint = checkpointWithId(fixture, fixture.branch_a_checkpoint_id);
    branch_a_checkpoint.checkpoint.head_sequence = 1;
    branch_a_checkpoint = identifyCheckpoint(std::move(branch_a_checkpoint.checkpoint));
    *std::ranges::find(fixture.checkpoints, fixture.branch_a_checkpoint_id,
                       &IdentifiedCheckpoint::canonical_id) = branch_a_checkpoint;
    const auto wrong_sequence = CheckpointGraph::build(fixture.segments, fixture.checkpoints);
    QVERIFY(!wrong_sequence.has_value());
    QCOMPARE(wrong_sequence.error().code, CheckpointGraphErrorCode::SequenceMismatch);
}

void CheckpointGraphTest::rejectsSegmentCheckpointMetadataMismatch() {
    for (int variation = 0; variation < 3; ++variation) {
        auto fixture = makeForkFixture();
        auto branch_a = std::ranges::find(fixture.segments, fixture.branch_a_segment_id,
                                          &IdentifiedSessionEventSegment::canonical_id);
        QVERIFY(branch_a != fixture.segments.end());
        auto changed_segment = branch_a->segment;
        switch (variation) {
        case 0:
            changed_segment.session_id = QStringLiteral("session.other");
            break;
        case 1:
            changed_segment.engine_revision = QStringLiteral("engine.v2");
            break;
        case 2:
            changed_segment.authority_contract = LogicalAuthorityContract::LegacyV1;
            break;
        default:
            Q_UNREACHABLE();
        }
        const auto changed = identifySegment(std::move(changed_segment));
        *branch_a = changed;

        auto branch_a_checkpoint = checkpointWithId(fixture, fixture.branch_a_checkpoint_id);
        branch_a_checkpoint.checkpoint.head_segment_id = changed.canonical_id;
        branch_a_checkpoint = identifyCheckpoint(std::move(branch_a_checkpoint.checkpoint));
        *std::ranges::find(fixture.checkpoints, fixture.branch_a_checkpoint_id,
                           &IdentifiedCheckpoint::canonical_id) = branch_a_checkpoint;

        const auto result = CheckpointGraph::build(fixture.segments, fixture.checkpoints);
        QVERIFY2(!result.has_value(), "cross-object metadata mismatch was accepted");
        QCOMPARE(result.error().code, CheckpointGraphErrorCode::MetadataMismatch);
    }
}

void CheckpointGraphTest::rejectsInvalidLinearAndMergeHeads() {
    auto fixture = makeForkFixture();
    fixture.checkpoints.push_back(identifyCheckpoint(
        makeCheckpoint(fixture.branch_b_segment_id, 2, {fixture.branch_a_checkpoint_id}, 0x94U,
                       {fixture.authored_b_id})));
    const auto nonextending_linear = CheckpointGraph::build(fixture.segments, fixture.checkpoints);
    QVERIFY(!nonextending_linear.has_value());
    QCOMPARE(nonextending_linear.error().code, CheckpointGraphErrorCode::InvalidAncestry);

    fixture = makeForkFixture();
    fixture.checkpoints.push_back(identifyCheckpoint(
        makeCheckpoint(fixture.branch_b_segment_id, 2,
                       {fixture.branch_a_checkpoint_id, fixture.branch_b_checkpoint_id}, 0x92U,
                       {fixture.authored_b_id}, fixture.branch_a_checkpoint_id)));
    const auto nonextending_merge = CheckpointGraph::build(fixture.segments, fixture.checkpoints);
    QVERIFY(!nonextending_merge.has_value());
    QCOMPARE(nonextending_merge.error().code, CheckpointGraphErrorCode::InvalidAncestry);
}

void CheckpointGraphTest::rejectsZeroIdentifiedObjectIds() {
    auto fixture = makeForkFixture();
    fixture.segments.front().canonical_id = {};
    const auto zero_segment = CheckpointGraph::build(fixture.segments, fixture.checkpoints);
    QVERIFY(!zero_segment.has_value());
    QCOMPARE(zero_segment.error().code, CheckpointGraphErrorCode::InvalidArgument);

    fixture = makeForkFixture();
    fixture.checkpoints.front().canonical_id = {};
    const auto zero_checkpoint = CheckpointGraph::build(fixture.segments, fixture.checkpoints);
    QVERIFY(!zero_checkpoint.has_value());
    QCOMPARE(zero_checkpoint.error().code, CheckpointGraphErrorCode::InvalidArgument);
}

void CheckpointGraphTest::rejectsEveryRestoreMetadataMismatchAndMissingHead() {
    const auto fixture = makeForkFixture();
    const auto graph = CheckpointGraph::build(fixture.segments, fixture.checkpoints);
    QVERIFY(graph.has_value());
    const auto& selected = checkpointWithId(fixture, fixture.branch_a_checkpoint_id).checkpoint;

    auto expectations = expectationsFor(selected, {fixture.authored_a_id});
    expectations.session_id = QStringLiteral("session.other");
    auto result = graph->makeImportPlan(fixture.branch_a_checkpoint_id, expectations);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, CheckpointGraphErrorCode::MetadataMismatch);

    expectations = expectationsFor(selected, {fixture.authored_a_id});
    expectations.engine_revision = QStringLiteral("engine.v2");
    result = graph->makeImportPlan(fixture.branch_a_checkpoint_id, expectations);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, CheckpointGraphErrorCode::MetadataMismatch);

    expectations = expectationsFor(selected, {fixture.authored_a_id});
    expectations.authority_contract = LogicalAuthorityContract::LegacyV1;
    result = graph->makeImportPlan(fixture.branch_a_checkpoint_id, expectations);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, CheckpointGraphErrorCode::MetadataMismatch);

    expectations = expectationsFor(selected, {fixture.authored_a_id});
    expectations.session_created_at_utc = QStringLiteral("2026-08-12T00:00:00Z");
    result = graph->makeImportPlan(fixture.branch_a_checkpoint_id, expectations);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, CheckpointGraphErrorCode::MetadataMismatch);

    expectations = expectationsFor(selected, {fixture.authored_a_id});
    const auto missing = graph->makeImportPlan(objectId(0xeeU), expectations);
    QVERIFY(!missing.has_value());
    QCOMPARE(missing.error().code, CheckpointGraphErrorCode::NotCurrentHead);

    const auto stale_resolution = graph->makeResolutionCheckpoint(fixture.root_checkpoint_id);
    QVERIFY(!stale_resolution.has_value());
    QCOMPARE(stale_resolution.error().code, CheckpointGraphErrorCode::NotCurrentHead);

    const auto missing_resolution = graph->makeResolutionCheckpoint(objectId(0xeeU));
    QVERIFY(!missing_resolution.has_value());
    QCOMPARE(missing_resolution.error().code, CheckpointGraphErrorCode::NotCurrentHead);
}

void CheckpointGraphTest::rejectsAncestralMergeParentsAndDuplicateCommands() {
    auto fixture = makeForkFixture();
    const auto invalid_resolution = identifyCheckpoint(
        makeCheckpoint(fixture.branch_a_segment_id, 2,
                       {fixture.root_checkpoint_id, fixture.branch_a_checkpoint_id}, 0x91U,
                       {fixture.authored_a_id}, fixture.branch_a_checkpoint_id));
    fixture.checkpoints.push_back(invalid_resolution);
    const auto ancestral_parents = CheckpointGraph::build(fixture.segments, fixture.checkpoints);
    QVERIFY(!ancestral_parents.has_value());
    QCOMPARE(ancestral_parents.error().code, CheckpointGraphErrorCode::InvalidAncestry);

    fixture = makeForkFixture();
    auto branch_a = *std::ranges::find(fixture.segments, fixture.branch_a_segment_id,
                                       &IdentifiedSessionEventSegment::canonical_id);
    branch_a.segment.batches.front().command_id = QStringLiteral("command.root");
    branch_a = identifySegment(std::move(branch_a.segment));
    *std::ranges::find(fixture.segments, fixture.branch_a_segment_id,
                       &IdentifiedSessionEventSegment::canonical_id) = branch_a;
    auto branch_a_checkpoint = checkpointWithId(fixture, fixture.branch_a_checkpoint_id);
    branch_a_checkpoint.checkpoint.head_segment_id = branch_a.canonical_id;
    branch_a_checkpoint = identifyCheckpoint(std::move(branch_a_checkpoint.checkpoint));
    *std::ranges::find(fixture.checkpoints, fixture.branch_a_checkpoint_id,
                       &IdentifiedCheckpoint::canonical_id) = branch_a_checkpoint;
    const auto duplicate_graph = CheckpointGraph::build(fixture.segments, fixture.checkpoints);
    QVERIFY(duplicate_graph.has_value());
    const auto duplicate_plan = duplicate_graph->makeImportPlan(
        branch_a_checkpoint.canonical_id,
        expectationsFor(branch_a_checkpoint.checkpoint, {fixture.authored_a_id}));
    QVERIFY(!duplicate_plan.has_value());
    QCOMPARE(duplicate_plan.error().code, CheckpointGraphErrorCode::DuplicateCommand);

    const auto no_conflict_fixture = makeForkFixture();
    const std::vector single_path_segments{
        *std::ranges::find(no_conflict_fixture.segments, no_conflict_fixture.root_segment_id,
                           &IdentifiedSessionEventSegment::canonical_id),
    };
    const std::vector single_path_checkpoints{
        checkpointWithId(no_conflict_fixture, no_conflict_fixture.root_checkpoint_id),
    };
    const auto no_conflict = CheckpointGraph::build(single_path_segments, single_path_checkpoints);
    QVERIFY(no_conflict.has_value());
    const auto rejected_resolution =
        no_conflict->makeResolutionCheckpoint(no_conflict_fixture.root_checkpoint_id);
    QVERIFY(!rejected_resolution.has_value());
    QCOMPARE(rejected_resolution.error().code, CheckpointGraphErrorCode::NoConflict);
}

QTEST_GUILESS_MAIN(CheckpointGraphTest)

#include "tst_checkpoint_graph.moc"
