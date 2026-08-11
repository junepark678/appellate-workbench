#include "appellate/sync/logical_object_codec.hpp"

#include <QDateTime>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <span>
#include <utility>

namespace appellate::sync {
namespace {

constexpr std::array<unsigned char, 8> segment_magic{'A', 'W', 'S', 'G', 0, 1, 0, 0};
constexpr std::array<unsigned char, 8> checkpoint_magic{'A', 'W', 'C', 'P', 0, 1, 0, 0};
constexpr std::uint64_t maximum_sequence =
    static_cast<std::uint64_t>(std::numeric_limits<qint64>::max());

[[nodiscard]] auto fail(LogicalObjectErrorCode code, QString message)
    -> std::unexpected<LogicalObjectError> {
    return std::unexpected(LogicalObjectError{code, std::move(message)});
}

[[nodiscard]] bool isAllZero(std::span<const unsigned char> bytes) {
    unsigned char aggregate{};
    for (const auto byte : bytes) {
        aggregate = static_cast<unsigned char>(aggregate | byte);
    }
    return aggregate == 0U;
}

[[nodiscard]] bool isAllZero(const SyncObjectId& value) {
    return isAllZero(std::span<const unsigned char>{value});
}

[[nodiscard]] bool validAuthorityContract(LogicalAuthorityContract contract) {
    return contract == LogicalAuthorityContract::LegacyV1 ||
           contract == LogicalAuthorityContract::CanonicalV2;
}

[[nodiscard]] bool validIdentifier(const QString& value, const LogicalObjectLimits& limits) {
    if (value.isEmpty() || value.size() > static_cast<qsizetype>(limits.maximum_identifier_bytes)) {
        return false;
    }
    for (const auto character : value) {
        const auto code = character.unicode();
        if (code < 0x21U || code > 0x7eU) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool validCanonicalUtc(const QString& value) {
    if (value.size() != 20 || !value.endsWith(u'Z')) {
        return false;
    }
    const auto parsed = QDateTime::fromString(value, Qt::ISODate);
    return parsed.isValid() && parsed.offsetFromUtc() == 0 &&
           parsed.toUTC().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'")) == value;
}

[[nodiscard]] auto validateLimits(const LogicalObjectLimits& limits)
    -> std::expected<void, LogicalObjectError> {
    if (limits.maximum_segment_bytes == 0 ||
        limits.maximum_segment_bytes > LogicalObjectLimits::default_maximum_segment_bytes ||
        limits.maximum_checkpoint_bytes == 0 ||
        limits.maximum_checkpoint_bytes > LogicalObjectLimits::default_maximum_checkpoint_bytes ||
        limits.maximum_identifier_bytes == 0 ||
        limits.maximum_identifier_bytes > LogicalObjectLimits::default_maximum_identifier_bytes ||
        limits.maximum_payload_bytes == 0 ||
        limits.maximum_payload_bytes > LogicalObjectLimits::default_maximum_payload_bytes ||
        limits.maximum_batches == 0 ||
        limits.maximum_batches > LogicalObjectLimits::default_maximum_batches ||
        limits.maximum_events_per_batch == 0 ||
        limits.maximum_events_per_batch > LogicalObjectLimits::default_maximum_events_per_batch ||
        limits.maximum_events_per_segment == 0 ||
        limits.maximum_events_per_segment >
            LogicalObjectLimits::default_maximum_events_per_segment ||
        limits.maximum_pins == 0 ||
        limits.maximum_pins > LogicalObjectLimits::default_maximum_pins ||
        limits.maximum_checkpoint_parents == 0 ||
        limits.maximum_checkpoint_parents >
            LogicalObjectLimits::default_maximum_checkpoint_parents ||
        limits.maximum_authored_revisions == 0 ||
        limits.maximum_authored_revisions >
            LogicalObjectLimits::default_maximum_authored_revisions) {
        return fail(LogicalObjectErrorCode::InvalidArgument,
                    QStringLiteral("Logical-object limits are invalid"));
    }
    return {};
}

[[nodiscard]] bool strictlySortedIds(const std::vector<SyncObjectId>& values) {
    for (std::size_t index = 1; index < values.size(); ++index) {
        if (!(values[index - 1U] < values[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool strictlySortedPins(const std::vector<LogicalRevisionPin>& pins) {
    for (std::size_t index = 1; index < pins.size(); ++index) {
        if (!(pins[index - 1U].pack_id < pins[index].pack_id)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] auto validatePinSet(const std::vector<LogicalRevisionPin>& pins,
                                  const LogicalObjectLimits& limits)
    -> std::expected<void, LogicalObjectError> {
    if (pins.empty() || pins.size() > limits.maximum_pins) {
        return fail(LogicalObjectErrorCode::LimitExceeded,
                    QStringLiteral("Checkpoint revision-pin count is invalid"));
    }
    if (!strictlySortedPins(pins)) {
        return fail(LogicalObjectErrorCode::NonCanonicalOrder,
                    QStringLiteral("Checkpoint revision pins are not canonical"));
    }
    for (const auto& pin : pins) {
        if (!validIdentifier(pin.pack_id, limits) || !validIdentifier(pin.version, limits)) {
            return fail(LogicalObjectErrorCode::InvalidText,
                        QStringLiteral("Checkpoint revision pin text is invalid"));
        }
        if (isAllZero(pin.revision_digest)) {
            return fail(LogicalObjectErrorCode::InvalidArgument,
                        QStringLiteral("Checkpoint revision digest is invalid"));
        }
    }
    return {};
}

[[nodiscard]] auto validateCheckpoint(const Checkpoint& checkpoint,
                                      const LogicalObjectLimits& limits)
    -> std::expected<void, LogicalObjectError> {
    if (!validIdentifier(checkpoint.session_id, limits) ||
        !validIdentifier(checkpoint.engine_revision, limits) ||
        !validAuthorityContract(checkpoint.authority_contract) ||
        !validCanonicalUtc(checkpoint.session_created_at_utc)) {
        return fail(LogicalObjectErrorCode::InvalidText,
                    QStringLiteral("Checkpoint session metadata is invalid"));
    }
    if (isAllZero(checkpoint.head_segment_id) || isAllZero(checkpoint.projection_digest) ||
        checkpoint.head_sequence > maximum_sequence) {
        return fail(LogicalObjectErrorCode::InvalidArgument,
                    QStringLiteral("Checkpoint head metadata is invalid"));
    }
    if (const auto valid_pins = validatePinSet(checkpoint.pins, limits); !valid_pins) {
        return valid_pins;
    }
    if (checkpoint.parent_checkpoint_ids.size() > limits.maximum_checkpoint_parents ||
        checkpoint.authored_revision_ids.size() > limits.maximum_authored_revisions) {
        return fail(LogicalObjectErrorCode::LimitExceeded,
                    QStringLiteral("Checkpoint reference count exceeds its limit"));
    }
    if (!strictlySortedIds(checkpoint.parent_checkpoint_ids) ||
        !strictlySortedIds(checkpoint.authored_revision_ids)) {
        return fail(LogicalObjectErrorCode::NonCanonicalOrder,
                    QStringLiteral("Checkpoint references are not canonical"));
    }
    const auto zero_id = [](const SyncObjectId& id) { return isAllZero(id); };
    if (std::ranges::any_of(checkpoint.parent_checkpoint_ids, zero_id) ||
        std::ranges::any_of(checkpoint.authored_revision_ids, zero_id)) {
        return fail(LogicalObjectErrorCode::InvalidArgument,
                    QStringLiteral("Checkpoint contains a zero object reference"));
    }
    const auto parent_count = checkpoint.parent_checkpoint_ids.size();
    if (parent_count < 2U && checkpoint.selected_base_checkpoint_id.has_value()) {
        return fail(LogicalObjectErrorCode::InvalidArgument,
                    QStringLiteral("Linear checkpoint cannot select a merge base"));
    }
    if (parent_count >= 2U) {
        if (!checkpoint.selected_base_checkpoint_id ||
            isAllZero(*checkpoint.selected_base_checkpoint_id) ||
            !std::binary_search(checkpoint.parent_checkpoint_ids.begin(),
                                checkpoint.parent_checkpoint_ids.end(),
                                *checkpoint.selected_base_checkpoint_id)) {
            return fail(LogicalObjectErrorCode::InvalidArgument,
                        QStringLiteral("Resolution checkpoint has no canonical selected base"));
        }
    }
    return {};
}

class Writer final {
  public:
    explicit Writer(std::size_t limit) : limit_(limit) {}

    [[nodiscard]] bool appendByte(std::uint8_t value) {
        return appendRaw(std::span<const unsigned char>{&value, 1U});
    }

    [[nodiscard]] bool append16(std::uint16_t value) {
        const std::array<unsigned char, 2> bytes{
            static_cast<unsigned char>((value >> 8U) & 0xffU),
            static_cast<unsigned char>(value & 0xffU),
        };
        return appendRaw(bytes);
    }

    [[nodiscard]] bool append32(std::uint32_t value) {
        std::array<unsigned char, 4> bytes{};
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            const auto shift = static_cast<unsigned int>((bytes.size() - index - 1U) * 8U);
            bytes[index] = static_cast<unsigned char>((value >> shift) & 0xffU);
        }
        return appendRaw(bytes);
    }

    [[nodiscard]] bool append64(std::uint64_t value) {
        std::array<unsigned char, 8> bytes{};
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            const auto shift = static_cast<unsigned int>((bytes.size() - index - 1U) * 8U);
            bytes[index] = static_cast<unsigned char>((value >> shift) & 0xffU);
        }
        return appendRaw(bytes);
    }

    [[nodiscard]] bool appendId(const SyncObjectId& value) { return appendRaw(value); }

    [[nodiscard]] bool appendText(const QString& value) {
        const auto bytes = value.toLatin1();
        return append16(static_cast<std::uint16_t>(bytes.size())) &&
               appendRaw(std::span<const unsigned char>{
                   reinterpret_cast<const unsigned char*>(bytes.constData()),
                   static_cast<std::size_t>(bytes.size())});
    }

    [[nodiscard]] bool appendPayload(const QByteArray& value) {
        return append32(static_cast<std::uint32_t>(value.size())) &&
               appendRaw(std::span<const unsigned char>{
                   reinterpret_cast<const unsigned char*>(value.constData()),
                   static_cast<std::size_t>(value.size())});
    }

    [[nodiscard]] bool appendRaw(std::span<const unsigned char> value) {
        if (value.size() > limit_ ||
            static_cast<std::size_t>(bytes_.size()) > limit_ - value.size()) {
            return false;
        }
        bytes_.append(reinterpret_cast<const char*>(value.data()),
                      static_cast<qsizetype>(value.size()));
        return true;
    }

    [[nodiscard]] QByteArray take() && { return std::move(bytes_); }

  private:
    QByteArray bytes_;
    std::size_t limit_{};
};

class Reader final {
  public:
    explicit Reader(QByteArrayView bytes) : bytes_(bytes) {}

    [[nodiscard]] std::size_t remaining() const noexcept {
        return static_cast<std::size_t>(bytes_.size()) - offset_;
    }

    [[nodiscard]] auto readByte() -> std::optional<std::uint8_t> {
        const auto bytes = take(1U);
        if (!bytes) {
            return std::nullopt;
        }
        return bytes->front();
    }

    [[nodiscard]] auto read16() -> std::optional<std::uint16_t> {
        const auto bytes = take(2U);
        if (!bytes) {
            return std::nullopt;
        }
        return static_cast<std::uint16_t>((static_cast<std::uint16_t>((*bytes)[0]) << 8U) |
                                          static_cast<std::uint16_t>((*bytes)[1]));
    }

    [[nodiscard]] auto read32() -> std::optional<std::uint32_t> {
        const auto bytes = take(4U);
        if (!bytes) {
            return std::nullopt;
        }
        std::uint32_t value{};
        for (const auto byte : *bytes) {
            value = (value << 8U) | static_cast<std::uint32_t>(byte);
        }
        return value;
    }

    [[nodiscard]] auto read64() -> std::optional<std::uint64_t> {
        const auto bytes = take(8U);
        if (!bytes) {
            return std::nullopt;
        }
        std::uint64_t value{};
        for (const auto byte : *bytes) {
            value = (value << 8U) | static_cast<std::uint64_t>(byte);
        }
        return value;
    }

    [[nodiscard]] auto readId() -> std::optional<SyncObjectId> {
        const auto bytes = take(sync_object_id_bytes);
        if (!bytes) {
            return std::nullopt;
        }
        SyncObjectId value{};
        std::memcpy(value.data(), bytes->data(), value.size());
        return value;
    }

    [[nodiscard]] auto readText(std::size_t maximum_bytes) -> std::optional<QString> {
        const auto size = read16();
        if (!size || *size == 0U || *size > maximum_bytes) {
            return std::nullopt;
        }
        const auto bytes = take(*size);
        if (!bytes) {
            return std::nullopt;
        }
        return QString::fromLatin1(reinterpret_cast<const char*>(bytes->data()),
                                   static_cast<qsizetype>(bytes->size()));
    }

    [[nodiscard]] auto readPayload(std::size_t maximum_bytes) -> std::optional<QByteArray> {
        const auto size = read32();
        if (!size || *size == 0U || *size > maximum_bytes) {
            return std::nullopt;
        }
        const auto bytes = take(*size);
        if (!bytes) {
            return std::nullopt;
        }
        return QByteArray(reinterpret_cast<const char*>(bytes->data()),
                          static_cast<qsizetype>(bytes->size()));
    }

    [[nodiscard]] auto take(std::size_t count) -> std::optional<std::span<const unsigned char>> {
        if (count > remaining()) {
            return std::nullopt;
        }
        const auto* start = reinterpret_cast<const unsigned char*>(bytes_.data()) + offset_;
        offset_ += count;
        return std::span<const unsigned char>{start, count};
    }

  private:
    QByteArrayView bytes_;
    std::size_t offset_{};
};

[[nodiscard]] auto requireMagic(Reader& reader, const std::array<unsigned char, 8>& expected)
    -> std::expected<void, LogicalObjectError> {
    const auto actual = reader.take(expected.size());
    if (!actual) {
        return fail(LogicalObjectErrorCode::InvalidEncoding,
                    QStringLiteral("Logical-object header is truncated"));
    }
    if (!std::equal(expected.begin(), expected.begin() + 4, actual->begin())) {
        return fail(LogicalObjectErrorCode::InvalidEncoding,
                    QStringLiteral("Logical-object magic is invalid"));
    }
    if (!std::equal(expected.begin() + 4, expected.end(), actual->begin() + 4)) {
        return fail(LogicalObjectErrorCode::UnsupportedFormat,
                    QStringLiteral("Logical-object version or flags are unsupported"));
    }
    return {};
}

[[nodiscard]] auto requireReaderComplete(const Reader& reader)
    -> std::expected<void, LogicalObjectError> {
    if (reader.remaining() != 0U) {
        return fail(LogicalObjectErrorCode::TrailingData,
                    QStringLiteral("Logical object has trailing bytes"));
    }
    return {};
}

} // namespace

std::expected<std::uint64_t, LogicalObjectError>
LogicalObjectCodec::finalSequence(const SessionEventSegment& segment, LogicalObjectLimits limits) {
    if (const auto valid_limits = validateLimits(limits); !valid_limits) {
        return std::unexpected(valid_limits.error());
    }
    if (!validIdentifier(segment.session_id, limits) ||
        !validIdentifier(segment.engine_revision, limits) ||
        !validAuthorityContract(segment.authority_contract)) {
        return fail(LogicalObjectErrorCode::InvalidText,
                    QStringLiteral("Session-event segment metadata is invalid"));
    }
    if (segment.base_sequence > maximum_sequence ||
        (!segment.parent_segment_id && segment.base_sequence != 0U) ||
        (segment.parent_segment_id &&
         (isAllZero(*segment.parent_segment_id) || segment.base_sequence == 0U))) {
        return fail(LogicalObjectErrorCode::InvalidSequence,
                    QStringLiteral("Session-event segment base is invalid"));
    }
    if (segment.batches.empty() || segment.batches.size() > limits.maximum_batches) {
        return fail(LogicalObjectErrorCode::LimitExceeded,
                    QStringLiteral("Session-event segment batch count is invalid"));
    }

    std::uint64_t sequence = segment.base_sequence;
    std::size_t total_events{};
    std::vector<QString> command_ids;
    command_ids.reserve(segment.batches.size());
    for (const auto& batch : segment.batches) {
        if (batch.expected_sequence != sequence || !validIdentifier(batch.command_id, limits) ||
            !validCanonicalUtc(batch.recorded_at_utc)) {
            return fail(LogicalObjectErrorCode::InvalidSequence,
                        QStringLiteral("Session-event command metadata is invalid"));
        }
        if (batch.command_payload.isEmpty() ||
            static_cast<std::size_t>(batch.command_payload.size()) > limits.maximum_payload_bytes ||
            batch.events.empty() || batch.events.size() > limits.maximum_events_per_batch ||
            batch.events.size() > limits.maximum_events_per_segment ||
            total_events > limits.maximum_events_per_segment - batch.events.size()) {
            return fail(LogicalObjectErrorCode::LimitExceeded,
                        QStringLiteral("Session-event command payload exceeds its bounds"));
        }
        if (std::ranges::find(command_ids, batch.command_id) != command_ids.end()) {
            return fail(LogicalObjectErrorCode::InvalidSequence,
                        QStringLiteral("Session-event segment repeats a command identifier"));
        }
        command_ids.push_back(batch.command_id);
        for (const auto& event : batch.events) {
            if (!validIdentifier(event.event_type, limits) ||
                !validIdentifier(event.authority_id, limits)) {
                return fail(LogicalObjectErrorCode::InvalidText,
                            QStringLiteral("Session-event metadata is invalid"));
            }
            if (event.payload.isEmpty() ||
                static_cast<std::size_t>(event.payload.size()) > limits.maximum_payload_bytes) {
                return fail(LogicalObjectErrorCode::LimitExceeded,
                            QStringLiteral("Session-event payload exceeds its bounds"));
            }
        }
        total_events += batch.events.size();
        if (batch.events.size() > maximum_sequence - sequence) {
            return fail(LogicalObjectErrorCode::InvalidSequence,
                        QStringLiteral("Session-event sequence overflows"));
        }
        sequence += static_cast<std::uint64_t>(batch.events.size());
    }
    return sequence;
}

std::expected<QByteArray, LogicalObjectError>
LogicalObjectCodec::encodeSessionEventSegment(const SessionEventSegment& segment,
                                              LogicalObjectLimits limits) {
    const auto final_sequence = finalSequence(segment, limits);
    if (!final_sequence) {
        return std::unexpected(final_sequence.error());
    }

    Writer writer(limits.maximum_segment_bytes);
    bool written = writer.appendRaw(segment_magic) && writer.appendText(segment.session_id) &&
                   writer.appendText(segment.engine_revision) &&
                   writer.appendByte(static_cast<std::uint8_t>(segment.authority_contract)) &&
                   writer.append64(segment.base_sequence) &&
                   writer.appendByte(segment.parent_segment_id ? 1U : 0U);
    if (written && segment.parent_segment_id) {
        written = writer.appendId(*segment.parent_segment_id);
    }
    written = written && writer.append16(static_cast<std::uint16_t>(segment.batches.size()));
    for (const auto& batch : segment.batches) {
        written = written && writer.append64(batch.expected_sequence) &&
                  writer.appendText(batch.command_id) && writer.appendText(batch.recorded_at_utc) &&
                  writer.appendPayload(batch.command_payload) &&
                  writer.append16(static_cast<std::uint16_t>(batch.events.size()));
        for (const auto& event : batch.events) {
            written = written && writer.appendText(event.event_type) &&
                      writer.appendText(event.authority_id) && writer.appendPayload(event.payload);
        }
    }
    if (!written) {
        return fail(LogicalObjectErrorCode::ObjectTooLarge,
                    QStringLiteral("Session-event segment exceeds its encoded limit"));
    }
    return std::move(writer).take();
}

std::expected<SessionEventSegment, LogicalObjectError>
LogicalObjectCodec::decodeSessionEventSegment(QByteArrayView encoded, LogicalObjectLimits limits) {
    if (const auto valid_limits = validateLimits(limits); !valid_limits) {
        return std::unexpected(valid_limits.error());
    }
    if (encoded.size() < 0 ||
        static_cast<std::size_t>(encoded.size()) > limits.maximum_segment_bytes) {
        return fail(LogicalObjectErrorCode::ObjectTooLarge,
                    QStringLiteral("Session-event segment exceeds its encoded limit"));
    }
    Reader reader(encoded);
    if (const auto magic = requireMagic(reader, segment_magic); !magic) {
        return std::unexpected(magic.error());
    }
    const auto session_id = reader.readText(limits.maximum_identifier_bytes);
    const auto engine_revision = reader.readText(limits.maximum_identifier_bytes);
    const auto contract = reader.readByte();
    const auto base_sequence = reader.read64();
    const auto parent_present = reader.readByte();
    if (!session_id || !engine_revision || !contract || !base_sequence || !parent_present ||
        *parent_present > 1U) {
        return fail(LogicalObjectErrorCode::InvalidEncoding,
                    QStringLiteral("Session-event segment header is malformed"));
    }

    SessionEventSegment segment;
    segment.session_id = *session_id;
    segment.engine_revision = *engine_revision;
    segment.authority_contract = static_cast<LogicalAuthorityContract>(*contract);
    segment.base_sequence = *base_sequence;
    if (*parent_present == 1U) {
        const auto parent = reader.readId();
        if (!parent) {
            return fail(LogicalObjectErrorCode::InvalidEncoding,
                        QStringLiteral("Session-event parent reference is truncated"));
        }
        segment.parent_segment_id = *parent;
    }
    const auto batch_count = reader.read16();
    if (!batch_count || *batch_count == 0U || *batch_count > limits.maximum_batches) {
        return fail(LogicalObjectErrorCode::LimitExceeded,
                    QStringLiteral("Session-event batch count exceeds its bound"));
    }
    segment.batches.reserve(*batch_count);
    std::size_t total_events{};
    for (std::size_t batch_index = 0; batch_index < *batch_count; ++batch_index) {
        const auto expected_sequence = reader.read64();
        const auto command_id = reader.readText(limits.maximum_identifier_bytes);
        const auto recorded_at = reader.readText(limits.maximum_identifier_bytes);
        const auto command_payload = reader.readPayload(limits.maximum_payload_bytes);
        const auto event_count = reader.read16();
        if (!expected_sequence || !command_id || !recorded_at || !command_payload || !event_count) {
            return fail(LogicalObjectErrorCode::InvalidEncoding,
                        QStringLiteral("Session-event command batch is truncated"));
        }
        if (*event_count == 0U || *event_count > limits.maximum_events_per_batch ||
            *event_count > limits.maximum_events_per_segment ||
            total_events > limits.maximum_events_per_segment - *event_count) {
            return fail(LogicalObjectErrorCode::LimitExceeded,
                        QStringLiteral("Session-event count exceeds its bound"));
        }
        SegmentCommandBatch batch{
            *expected_sequence, *command_id, *recorded_at, *command_payload, {}};
        batch.events.reserve(*event_count);
        for (std::size_t event_index = 0; event_index < *event_count; ++event_index) {
            const auto event_type = reader.readText(limits.maximum_identifier_bytes);
            const auto authority_id = reader.readText(limits.maximum_identifier_bytes);
            const auto payload = reader.readPayload(limits.maximum_payload_bytes);
            if (!event_type || !authority_id || !payload) {
                return fail(LogicalObjectErrorCode::InvalidEncoding,
                            QStringLiteral("Session-event entry is truncated"));
            }
            batch.events.push_back(SegmentEvent{*event_type, *authority_id, *payload});
        }
        total_events += *event_count;
        segment.batches.push_back(std::move(batch));
    }
    if (const auto complete = requireReaderComplete(reader); !complete) {
        return std::unexpected(complete.error());
    }
    if (const auto valid = finalSequence(segment, limits); !valid) {
        return std::unexpected(valid.error());
    }
    return segment;
}

std::expected<QByteArray, LogicalObjectError>
LogicalObjectCodec::encodeCheckpoint(const Checkpoint& checkpoint, LogicalObjectLimits limits) {
    if (const auto valid_limits = validateLimits(limits); !valid_limits) {
        return std::unexpected(valid_limits.error());
    }
    if (const auto valid = validateCheckpoint(checkpoint, limits); !valid) {
        return std::unexpected(valid.error());
    }

    Writer writer(limits.maximum_checkpoint_bytes);
    bool written = writer.appendRaw(checkpoint_magic) && writer.appendText(checkpoint.session_id) &&
                   writer.appendText(checkpoint.engine_revision) &&
                   writer.appendByte(static_cast<std::uint8_t>(checkpoint.authority_contract)) &&
                   writer.appendText(checkpoint.session_created_at_utc) &&
                   writer.appendId(checkpoint.head_segment_id) &&
                   writer.append64(checkpoint.head_sequence) &&
                   writer.appendId(checkpoint.projection_digest) &&
                   writer.append16(static_cast<std::uint16_t>(checkpoint.pins.size()));
    for (const auto& pin : checkpoint.pins) {
        written = written && writer.appendText(pin.pack_id) && writer.appendText(pin.version) &&
                  writer.appendId(pin.revision_digest);
    }
    written = written &&
              writer.append16(static_cast<std::uint16_t>(checkpoint.parent_checkpoint_ids.size()));
    for (const auto& parent : checkpoint.parent_checkpoint_ids) {
        written = written && writer.appendId(parent);
    }
    written = written && writer.appendByte(checkpoint.selected_base_checkpoint_id ? 1U : 0U);
    if (written && checkpoint.selected_base_checkpoint_id) {
        written = writer.appendId(*checkpoint.selected_base_checkpoint_id);
    }
    written = written &&
              writer.append16(static_cast<std::uint16_t>(checkpoint.authored_revision_ids.size()));
    for (const auto& authored : checkpoint.authored_revision_ids) {
        written = written && writer.appendId(authored);
    }
    if (!written) {
        return fail(LogicalObjectErrorCode::ObjectTooLarge,
                    QStringLiteral("Checkpoint exceeds its encoded limit"));
    }
    return std::move(writer).take();
}

std::expected<Checkpoint, LogicalObjectError>
LogicalObjectCodec::decodeCheckpoint(QByteArrayView encoded, LogicalObjectLimits limits) {
    if (const auto valid_limits = validateLimits(limits); !valid_limits) {
        return std::unexpected(valid_limits.error());
    }
    if (encoded.size() < 0 ||
        static_cast<std::size_t>(encoded.size()) > limits.maximum_checkpoint_bytes) {
        return fail(LogicalObjectErrorCode::ObjectTooLarge,
                    QStringLiteral("Checkpoint exceeds its encoded limit"));
    }
    Reader reader(encoded);
    if (const auto magic = requireMagic(reader, checkpoint_magic); !magic) {
        return std::unexpected(magic.error());
    }
    const auto session_id = reader.readText(limits.maximum_identifier_bytes);
    const auto engine_revision = reader.readText(limits.maximum_identifier_bytes);
    const auto contract = reader.readByte();
    const auto created_at = reader.readText(limits.maximum_identifier_bytes);
    const auto head_segment = reader.readId();
    const auto head_sequence = reader.read64();
    const auto projection_digest = reader.readId();
    const auto pin_count = reader.read16();
    if (!session_id || !engine_revision || !contract || !created_at || !head_segment ||
        !head_sequence || !projection_digest || !pin_count || *pin_count == 0U ||
        *pin_count > limits.maximum_pins) {
        return fail(LogicalObjectErrorCode::InvalidEncoding,
                    QStringLiteral("Checkpoint header is malformed"));
    }

    Checkpoint checkpoint;
    checkpoint.session_id = *session_id;
    checkpoint.engine_revision = *engine_revision;
    checkpoint.authority_contract = static_cast<LogicalAuthorityContract>(*contract);
    checkpoint.session_created_at_utc = *created_at;
    checkpoint.head_segment_id = *head_segment;
    checkpoint.head_sequence = *head_sequence;
    checkpoint.projection_digest = *projection_digest;
    checkpoint.pins.reserve(*pin_count);
    for (std::size_t index = 0; index < *pin_count; ++index) {
        const auto pack_id = reader.readText(limits.maximum_identifier_bytes);
        const auto version = reader.readText(limits.maximum_identifier_bytes);
        const auto digest = reader.readId();
        if (!pack_id || !version || !digest) {
            return fail(LogicalObjectErrorCode::InvalidEncoding,
                        QStringLiteral("Checkpoint revision pin is truncated"));
        }
        checkpoint.pins.push_back(LogicalRevisionPin{*pack_id, *version, *digest});
    }

    const auto parent_count = reader.read16();
    if (!parent_count || *parent_count > limits.maximum_checkpoint_parents) {
        return fail(LogicalObjectErrorCode::LimitExceeded,
                    QStringLiteral("Checkpoint parent count exceeds its bound"));
    }
    checkpoint.parent_checkpoint_ids.reserve(*parent_count);
    for (std::size_t index = 0; index < *parent_count; ++index) {
        const auto parent = reader.readId();
        if (!parent) {
            return fail(LogicalObjectErrorCode::InvalidEncoding,
                        QStringLiteral("Checkpoint parent reference is truncated"));
        }
        checkpoint.parent_checkpoint_ids.push_back(*parent);
    }
    const auto selected_present = reader.readByte();
    if (!selected_present || *selected_present > 1U) {
        return fail(LogicalObjectErrorCode::InvalidEncoding,
                    QStringLiteral("Checkpoint selected-base flag is invalid"));
    }
    if (*selected_present == 1U) {
        const auto selected = reader.readId();
        if (!selected) {
            return fail(LogicalObjectErrorCode::InvalidEncoding,
                        QStringLiteral("Checkpoint selected-base reference is truncated"));
        }
        checkpoint.selected_base_checkpoint_id = *selected;
    }
    const auto authored_count = reader.read16();
    if (!authored_count || *authored_count > limits.maximum_authored_revisions) {
        return fail(LogicalObjectErrorCode::LimitExceeded,
                    QStringLiteral("Checkpoint authored-revision count exceeds its bound"));
    }
    checkpoint.authored_revision_ids.reserve(*authored_count);
    for (std::size_t index = 0; index < *authored_count; ++index) {
        const auto authored = reader.readId();
        if (!authored) {
            return fail(LogicalObjectErrorCode::InvalidEncoding,
                        QStringLiteral("Checkpoint authored-revision reference is truncated"));
        }
        checkpoint.authored_revision_ids.push_back(*authored);
    }
    if (const auto complete = requireReaderComplete(reader); !complete) {
        return std::unexpected(complete.error());
    }
    if (const auto valid = validateCheckpoint(checkpoint, limits); !valid) {
        return std::unexpected(valid.error());
    }
    return checkpoint;
}

} // namespace appellate::sync
