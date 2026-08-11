#include "appellate/sync/logical_object_codec.hpp"

#include <QByteArray>
#include <QtTest>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace {

using appellate::sync::Checkpoint;
using appellate::sync::LogicalAuthorityContract;
using appellate::sync::LogicalObjectCodec;
using appellate::sync::LogicalObjectErrorCode;
using appellate::sync::LogicalObjectLimits;
using appellate::sync::LogicalRevisionPin;
using appellate::sync::SegmentCommandBatch;
using appellate::sync::SegmentEvent;
using appellate::sync::SessionEventSegment;
using appellate::sync::SyncObjectId;

[[nodiscard]] SyncObjectId objectId(unsigned char marker) {
    SyncObjectId id{};
    id.fill(marker);
    return id;
}

[[nodiscard]] SessionEventSegment segmentFixture() {
    return SessionEventSegment{
        QStringLiteral("session.one"),
        QStringLiteral("engine.v1"),
        LogicalAuthorityContract::CanonicalV2,
        0,
        std::nullopt,
        {SegmentCommandBatch{
            0,
            QStringLiteral("command.one"),
            QStringLiteral("2026-08-11T00:00:00Z"),
            QByteArrayLiteral("{}"),
            {
                SegmentEvent{QStringLiteral("event.one"), QStringLiteral("authority.one"),
                             QByteArrayLiteral(R"({"n":1})")},
                SegmentEvent{QStringLiteral("event.two"), QStringLiteral("authority.two"),
                             QByteArrayLiteral(R"({"n":2})")},
            },
        }}};
}

[[nodiscard]] Checkpoint checkpointFixture() {
    return Checkpoint{
        QStringLiteral("session.one"),
        QStringLiteral("engine.v1"),
        LogicalAuthorityContract::CanonicalV2,
        QStringLiteral("2026-08-11T00:00:00Z"),
        objectId(0x11U),
        2,
        objectId(0x91U),
        {
            LogicalRevisionPin{QStringLiteral("a.pack"), QStringLiteral("1.0.0"), objectId(0x21U)},
            LogicalRevisionPin{QStringLiteral("z.pack"), QStringLiteral("2.0.0"), objectId(0x22U)},
        },
        {objectId(0x31U), objectId(0x32U)},
        objectId(0x31U),
        {objectId(0x41U), objectId(0x42U)},
    };
}

void writeBigEndian16(QByteArray& bytes, qsizetype offset, std::uint16_t value) {
    bytes[offset] = static_cast<char>((value >> 8U) & 0xffU);
    bytes[offset + 1] = static_cast<char>(value & 0xffU);
}

} // namespace

class LogicalObjectCodecTest final : public QObject {
    Q_OBJECT

  private slots:
    void matchesFrozenSegmentVector();
    void matchesFrozenCheckpointVector();
    void roundTripsAndRejectsEveryTruncation();
    void rejectsVersionTrailingAndNoncanonicalReferences();
    void enforcesBoundsBeforeAllocation();
    void rejectsInvalidTextAndSequence();
};

void LogicalObjectCodecTest::matchesFrozenSegmentVector() {
    const auto encoded = LogicalObjectCodec::encodeSessionEventSegment(segmentFixture());
    QVERIFY(encoded.has_value());
    QCOMPARE(encoded->toHex(),
             QByteArrayLiteral(
                 "4157534700010000000b73657373696f6e2e6f6e650009656e67696e652e763102000000"
                 "00000000000000010000000000000000000b636f6d6d616e642e6f6e650014323032362d"
                 "30382d31315430303a30303a30305a000000027b7d000200096576656e742e6f6e65000d"
                 "617574686f726974792e6f6e65000000077b226e223a317d00096576656e742e74776f000d"
                 "617574686f726974792e74776f000000077b226e223a327d"));
    const auto decoded = LogicalObjectCodec::decodeSessionEventSegment(*encoded);
    QVERIFY(decoded.has_value());
    QCOMPARE(*decoded, segmentFixture());
    const auto final_sequence = LogicalObjectCodec::finalSequence(*decoded);
    QVERIFY(final_sequence.has_value());
    QCOMPARE(*final_sequence, std::uint64_t{2});
}

void LogicalObjectCodecTest::matchesFrozenCheckpointVector() {
    const auto encoded = LogicalObjectCodec::encodeCheckpoint(checkpointFixture());
    QVERIFY(encoded.has_value());
    QCOMPARE(encoded->toHex(),
             QByteArrayLiteral(
                 "4157435000010000000b73657373696f6e2e6f6e650009656e67696e652e7631020014323032362d"
                 "30382d31315430303a30303a30305a11111111111111111111111111111111111111111111111111"
                 "11111111111111000000000000000291919191919191919191919191919191919191919191919191"
                 "9191919191919100020006612e7061636b0005312e302e3021212121212121212121212121212121"
                 "2121212121212121212121212121212100067a2e7061636b0005322e302e30222222222222222222"
                 "22222222222222222222222222222222222222222222220002313131313131313131313131313131"
                 "31313131313131313131313131313131313232323232323232323232323232323232323232323232"
                 "32323232323232323201313131313131313131313131313131313131313131313131313131313131"
                 "31310002414141414141414141414141414141414141414141414141414141414141414142424242"
                 "42424242424242424242424242424242424242424242424242424242"));
    const auto decoded = LogicalObjectCodec::decodeCheckpoint(*encoded);
    QVERIFY(decoded.has_value());
    QCOMPARE(*decoded, checkpointFixture());
}

void LogicalObjectCodecTest::roundTripsAndRejectsEveryTruncation() {
    const auto segment = LogicalObjectCodec::encodeSessionEventSegment(segmentFixture());
    const auto checkpoint = LogicalObjectCodec::encodeCheckpoint(checkpointFixture());
    QVERIFY(segment.has_value());
    QVERIFY(checkpoint.has_value());

    for (qsizetype length = 0; length < segment->size(); ++length) {
        const auto result = LogicalObjectCodec::decodeSessionEventSegment(
            QByteArrayView{segment->constData(), length});
        QVERIFY2(!result.has_value(), "a truncated segment was accepted");
    }
    for (qsizetype length = 0; length < checkpoint->size(); ++length) {
        const auto result =
            LogicalObjectCodec::decodeCheckpoint(QByteArrayView{checkpoint->constData(), length});
        QVERIFY2(!result.has_value(), "a truncated checkpoint was accepted");
    }
}

void LogicalObjectCodecTest::rejectsVersionTrailingAndNoncanonicalReferences() {
    auto segment = *LogicalObjectCodec::encodeSessionEventSegment(segmentFixture());
    segment[5] = 2;
    auto rejected_segment = LogicalObjectCodec::decodeSessionEventSegment(segment);
    QVERIFY(!rejected_segment.has_value());
    QCOMPARE(rejected_segment.error().code, LogicalObjectErrorCode::UnsupportedFormat);

    auto checkpoint = *LogicalObjectCodec::encodeCheckpoint(checkpointFixture());
    checkpoint.append('\0');
    const auto trailing = LogicalObjectCodec::decodeCheckpoint(checkpoint);
    QVERIFY(!trailing.has_value());
    QCOMPARE(trailing.error().code, LogicalObjectErrorCode::TrailingData);

    auto value = checkpointFixture();
    std::ranges::reverse(value.parent_checkpoint_ids);
    auto unsorted = LogicalObjectCodec::encodeCheckpoint(value);
    QVERIFY(!unsorted.has_value());
    QCOMPARE(unsorted.error().code, LogicalObjectErrorCode::NonCanonicalOrder);

    value = checkpointFixture();
    value.authored_revision_ids.back() = value.authored_revision_ids.front();
    auto duplicate = LogicalObjectCodec::encodeCheckpoint(value);
    QVERIFY(!duplicate.has_value());
    QCOMPARE(duplicate.error().code, LogicalObjectErrorCode::NonCanonicalOrder);

    value = checkpointFixture();
    value.pins.front().revision_digest = {};
    auto zero_digest = LogicalObjectCodec::encodeCheckpoint(value);
    QVERIFY(!zero_digest.has_value());
    QCOMPARE(zero_digest.error().code, LogicalObjectErrorCode::InvalidArgument);

    value = checkpointFixture();
    value.parent_checkpoint_ids.resize(1);
    auto selected_on_linear = LogicalObjectCodec::encodeCheckpoint(value);
    QVERIFY(!selected_on_linear.has_value());
    QCOMPARE(selected_on_linear.error().code, LogicalObjectErrorCode::InvalidArgument);

    value = checkpointFixture();
    value.selected_base_checkpoint_id = objectId(0x77U);
    auto unknown_base = LogicalObjectCodec::encodeCheckpoint(value);
    QVERIFY(!unknown_base.has_value());
    QCOMPARE(unknown_base.error().code, LogicalObjectErrorCode::InvalidArgument);

    value = checkpointFixture();
    value.selected_base_checkpoint_id.reset();
    auto missing_merge_base = LogicalObjectCodec::encodeCheckpoint(value);
    QVERIFY(!missing_merge_base.has_value());
    QCOMPARE(missing_merge_base.error().code, LogicalObjectErrorCode::InvalidArgument);

    value = checkpointFixture();
    value.selected_base_checkpoint_id = SyncObjectId{};
    auto zero_merge_base = LogicalObjectCodec::encodeCheckpoint(value);
    QVERIFY(!zero_merge_base.has_value());
    QCOMPARE(zero_merge_base.error().code, LogicalObjectErrorCode::InvalidArgument);

    value = checkpointFixture();
    value.head_segment_id = {};
    auto zero_head = LogicalObjectCodec::encodeCheckpoint(value);
    QVERIFY(!zero_head.has_value());
    QCOMPARE(zero_head.error().code, LogicalObjectErrorCode::InvalidArgument);

    value = checkpointFixture();
    value.projection_digest = {};
    auto zero_projection = LogicalObjectCodec::encodeCheckpoint(value);
    QVERIFY(!zero_projection.has_value());
    QCOMPARE(zero_projection.error().code, LogicalObjectErrorCode::InvalidArgument);

    value = checkpointFixture();
    value.parent_checkpoint_ids.front() = {};
    auto zero_checkpoint_parent = LogicalObjectCodec::encodeCheckpoint(value);
    QVERIFY(!zero_checkpoint_parent.has_value());
    QCOMPARE(zero_checkpoint_parent.error().code, LogicalObjectErrorCode::InvalidArgument);

    value = checkpointFixture();
    value.authored_revision_ids.front() = {};
    auto zero_authored = LogicalObjectCodec::encodeCheckpoint(value);
    QVERIFY(!zero_authored.has_value());
    QCOMPARE(zero_authored.error().code, LogicalObjectErrorCode::InvalidArgument);

    auto segment_value = segmentFixture();
    segment_value.parent_segment_id = SyncObjectId{};
    segment_value.base_sequence = 1;
    segment_value.batches.front().expected_sequence = 1;
    auto zero_segment_parent = LogicalObjectCodec::encodeSessionEventSegment(segment_value);
    QVERIFY(!zero_segment_parent.has_value());
    QCOMPARE(zero_segment_parent.error().code, LogicalObjectErrorCode::InvalidSequence);
}

void LogicalObjectCodecTest::enforcesBoundsBeforeAllocation() {
    auto limits = LogicalObjectLimits{};
    limits.maximum_payload_bytes = 1;
    const auto encoded = LogicalObjectCodec::encodeSessionEventSegment(segmentFixture());
    QVERIFY(encoded.has_value());
    const auto bounded = LogicalObjectCodec::decodeSessionEventSegment(*encoded, limits);
    QVERIFY(!bounded.has_value());
    QCOMPARE(bounded.error().code, LogicalObjectErrorCode::InvalidEncoding);

    QByteArray oversized(static_cast<qsizetype>(limits.maximum_segment_bytes + 1U), '\0');
    const auto rejected_size = LogicalObjectCodec::decodeSessionEventSegment(oversized, limits);
    QVERIFY(!rejected_size.has_value());
    QCOMPARE(rejected_size.error().code, LogicalObjectErrorCode::ObjectTooLarge);

    auto malicious_count = *encoded;
    constexpr qsizetype batch_count_offset = 42;
    writeBigEndian16(malicious_count, batch_count_offset, 0xffffU);
    const auto rejected_count = LogicalObjectCodec::decodeSessionEventSegment(malicious_count);
    QVERIFY(!rejected_count.has_value());
    QCOMPARE(rejected_count.error().code, LogicalObjectErrorCode::LimitExceeded);

    limits = LogicalObjectLimits{};
    limits.maximum_events_per_segment = 1;
    const auto rejected_small_total =
        LogicalObjectCodec::decodeSessionEventSegment(*encoded, limits);
    QVERIFY(!rejected_small_total.has_value());
    QCOMPARE(rejected_small_total.error().code, LogicalObjectErrorCode::LimitExceeded);
    const auto rejected_small_total_encode =
        LogicalObjectCodec::encodeSessionEventSegment(segmentFixture(), limits);
    QVERIFY(!rejected_small_total_encode.has_value());
    QCOMPARE(rejected_small_total_encode.error().code, LogicalObjectErrorCode::LimitExceeded);

    auto value = checkpointFixture();
    value.authored_revision_ids.resize(LogicalObjectLimits::default_maximum_authored_revisions + 1U,
                                       objectId(0x44U));
    const auto excessive_references = LogicalObjectCodec::encodeCheckpoint(value);
    QVERIFY(!excessive_references.has_value());
    QCOMPARE(excessive_references.error().code, LogicalObjectErrorCode::LimitExceeded);

    auto invalid_limits = LogicalObjectLimits{};
    invalid_limits.maximum_batches = LogicalObjectLimits::default_maximum_batches + 1U;
    const auto rejected_limits =
        LogicalObjectCodec::decodeSessionEventSegment(*encoded, invalid_limits);
    QVERIFY(!rejected_limits.has_value());
    QCOMPARE(rejected_limits.error().code, LogicalObjectErrorCode::InvalidArgument);
}

void LogicalObjectCodecTest::rejectsInvalidTextAndSequence() {
    auto segment = segmentFixture();
    segment.session_id = QStringLiteral("contains space");
    auto invalid_text = LogicalObjectCodec::encodeSessionEventSegment(segment);
    QVERIFY(!invalid_text.has_value());
    QCOMPARE(invalid_text.error().code, LogicalObjectErrorCode::InvalidText);

    segment = segmentFixture();
    segment.batches.front().recorded_at_utc = QStringLiteral("2026-08-11T00:00:00+00:00");
    auto invalid_time = LogicalObjectCodec::encodeSessionEventSegment(segment);
    QVERIFY(!invalid_time.has_value());
    QCOMPARE(invalid_time.error().code, LogicalObjectErrorCode::InvalidSequence);

    segment = segmentFixture();
    segment.batches.front().expected_sequence = 1;
    auto discontinuous = LogicalObjectCodec::encodeSessionEventSegment(segment);
    QVERIFY(!discontinuous.has_value());
    QCOMPARE(discontinuous.error().code, LogicalObjectErrorCode::InvalidSequence);

    segment = segmentFixture();
    segment.parent_segment_id = objectId(0x12U);
    segment.base_sequence = static_cast<std::uint64_t>(std::numeric_limits<qint64>::max());
    segment.batches.front().expected_sequence = segment.base_sequence;
    auto overflow = LogicalObjectCodec::encodeSessionEventSegment(segment);
    QVERIFY(!overflow.has_value());
    QCOMPARE(overflow.error().code, LogicalObjectErrorCode::InvalidSequence);

    segment = segmentFixture();
    segment.batches.push_back(segment.batches.front());
    segment.batches.back().expected_sequence = 2;
    auto duplicate_command = LogicalObjectCodec::encodeSessionEventSegment(segment);
    QVERIFY(!duplicate_command.has_value());
    QCOMPARE(duplicate_command.error().code, LogicalObjectErrorCode::InvalidSequence);
}

QTEST_GUILESS_MAIN(LogicalObjectCodecTest)

#include "tst_logical_object_codec.moc"
