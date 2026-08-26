#pragma once

#include "appellate/sync/sync_object.hpp"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <vector>

namespace appellate::sync {

enum class LogicalAuthorityContract : std::uint8_t {
    LegacyV1 = 1,
    CanonicalV2 = 2,
};

struct LogicalRevisionPin final {
    QString pack_id;
    QString version;
    SyncObjectId revision_digest{};

    friend bool operator==(const LogicalRevisionPin&, const LogicalRevisionPin&) = default;
};

struct SegmentEvent final {
    QString event_type;
    QString authority_id;
    QByteArray payload;

    friend bool operator==(const SegmentEvent&, const SegmentEvent&) = default;
};

struct SegmentCommandBatch final {
    std::uint64_t expected_sequence{};
    QString command_id;
    QString recorded_at_utc;
    QByteArray command_payload;
    std::vector<SegmentEvent> events;

    friend bool operator==(const SegmentCommandBatch&, const SegmentCommandBatch&) = default;
};

struct SessionEventSegment final {
    QString session_id;
    QString engine_revision;
    LogicalAuthorityContract authority_contract{LogicalAuthorityContract::LegacyV1};
    std::uint64_t base_sequence{};
    std::optional<SyncObjectId> parent_segment_id;
    std::vector<SegmentCommandBatch> batches;

    friend bool operator==(const SessionEventSegment&, const SessionEventSegment&) = default;
};

struct Checkpoint final {
    QString session_id;
    QString engine_revision;
    LogicalAuthorityContract authority_contract{LogicalAuthorityContract::LegacyV1};
    QString session_created_at_utc;
    SyncObjectId head_segment_id{};
    std::uint64_t head_sequence{};
    SyncObjectId projection_digest{};
    std::vector<LogicalRevisionPin> pins;
    std::vector<SyncObjectId> parent_checkpoint_ids;
    std::optional<SyncObjectId> selected_base_checkpoint_id;
    std::vector<SyncObjectId> authored_revision_ids;

    friend bool operator==(const Checkpoint&, const Checkpoint&) = default;
};

struct LogicalObjectLimits final {
    static constexpr std::size_t default_maximum_segment_bytes = std::size_t{64} * 1024U * 1024U;
    static constexpr std::size_t default_maximum_checkpoint_bytes = std::size_t{4} * 1024U * 1024U;
    static constexpr std::size_t default_maximum_identifier_bytes = 512U;
    static constexpr std::size_t default_maximum_payload_bytes = std::size_t{1} * 1024U * 1024U;
    static constexpr std::size_t default_maximum_batches = 1'024U;
    static constexpr std::size_t default_maximum_events_per_batch = 4'096U;
    static constexpr std::size_t default_maximum_events_per_segment = 65'536U;
    static constexpr std::size_t default_maximum_pins = 128U;
    static constexpr std::size_t default_maximum_checkpoint_parents = 128U;
    static constexpr std::size_t default_maximum_authored_revisions = 4'096U;

    std::size_t maximum_segment_bytes{default_maximum_segment_bytes};
    std::size_t maximum_checkpoint_bytes{default_maximum_checkpoint_bytes};
    std::size_t maximum_identifier_bytes{default_maximum_identifier_bytes};
    std::size_t maximum_payload_bytes{default_maximum_payload_bytes};
    std::size_t maximum_batches{default_maximum_batches};
    std::size_t maximum_events_per_batch{default_maximum_events_per_batch};
    std::size_t maximum_events_per_segment{default_maximum_events_per_segment};
    std::size_t maximum_pins{default_maximum_pins};
    std::size_t maximum_checkpoint_parents{default_maximum_checkpoint_parents};
    std::size_t maximum_authored_revisions{default_maximum_authored_revisions};
};

enum class LogicalObjectErrorCode {
    InvalidArgument,
    ObjectTooLarge,
    InvalidEncoding,
    UnsupportedFormat,
    InvalidText,
    InvalidSequence,
    LimitExceeded,
    NonCanonicalOrder,
    TrailingData,
};

struct LogicalObjectError final {
    LogicalObjectErrorCode code{};
    QString message;

    friend bool operator==(const LogicalObjectError&, const LogicalObjectError&) = default;
};

class LogicalObjectCodec final {
  public:
    static constexpr std::uint16_t session_event_segment_schema_version = 1;
    static constexpr std::uint16_t checkpoint_schema_version = 1;

    [[nodiscard]] static auto encodeSessionEventSegment(const SessionEventSegment& segment,
                                                        LogicalObjectLimits limits = {})
        -> std::expected<QByteArray, LogicalObjectError>;
    [[nodiscard]] static auto decodeSessionEventSegment(QByteArrayView encoded,
                                                        LogicalObjectLimits limits = {})
        -> std::expected<SessionEventSegment, LogicalObjectError>;

    [[nodiscard]] static auto encodeCheckpoint(const Checkpoint& checkpoint,
                                               LogicalObjectLimits limits = {})
        -> std::expected<QByteArray, LogicalObjectError>;
    [[nodiscard]] static auto decodeCheckpoint(QByteArrayView encoded,
                                               LogicalObjectLimits limits = {})
        -> std::expected<Checkpoint, LogicalObjectError>;

    [[nodiscard]] static auto finalSequence(const SessionEventSegment& segment,
                                            LogicalObjectLimits limits = {})
        -> std::expected<std::uint64_t, LogicalObjectError>;
};

} // namespace appellate::sync
