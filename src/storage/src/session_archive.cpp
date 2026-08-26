#include "appellate/storage/session_archive.hpp"

#include "appellate/storage/asset_store.hpp"
#include "strict_json_scan.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace appellate::storage {
namespace {

constexpr std::array<unsigned char, 8> archive_magic{
    'A', 'P', 'S', 'A', 0, 0, 0, 1,
};
constexpr std::size_t archive_digest_bytes = 32U;
constexpr qsizetype maximum_text_characters = 512;
constexpr std::size_t maximum_text_bytes = 2048U;
constexpr std::size_t maximum_json_bytes = 1024U * 1024U;
constexpr std::size_t maximum_revision_pins = 128U;
constexpr qsizetype maximum_asset_purpose_length = 128;

struct ArchivedAsset final {
    QString digest;
    QByteArray bytes;
};

struct DecodedArchive final {
    std::vector<SessionSnapshot> sessions;
    std::vector<ArchivedAsset> assets;
};

[[nodiscard]] auto fail(SessionArchiveErrorCode code, QString message)
    -> std::unexpected<SessionArchiveError> {
    return std::unexpected(SessionArchiveError{code, std::move(message)});
}

[[nodiscard]] auto storeFailure(QString action, const QSqlQuery& query)
    -> std::unexpected<SessionArchiveError> {
    return fail(SessionArchiveErrorCode::SessionStoreFailure,
                QStringLiteral("%1: %2").arg(std::move(action), query.lastError().text()));
}

[[nodiscard]] bool validLimits(const SessionArchiveLimits& limits) {
    return limits.maximum_archive_bytes > 0 &&
           limits.maximum_archive_bytes <= SessionArchiveLimits::default_maximum_archive_bytes &&
           limits.maximum_sessions > 0 &&
           limits.maximum_sessions <= SessionArchiveLimits::default_maximum_sessions &&
           limits.maximum_rows_per_session > 0 &&
           limits.maximum_rows_per_session <=
               SessionArchiveLimits::default_maximum_rows_per_session &&
           limits.maximum_total_rows > 0 &&
           limits.maximum_total_rows <= SessionArchiveLimits::default_maximum_total_rows &&
           limits.maximum_assets > 0 &&
           limits.maximum_assets <= SessionArchiveLimits::default_maximum_assets &&
           limits.maximum_total_asset_bytes > 0 &&
           limits.maximum_total_asset_bytes <=
               SessionArchiveLimits::default_maximum_total_asset_bytes;
}

[[nodiscard]] bool validText(const QString& value) {
    return !value.isEmpty() && value.size() <= maximum_text_characters &&
           !value.contains(QChar::Null);
}

[[nodiscard]] bool validCanonicalUtc(const QString& value) {
    if (value.size() != 20 || !value.endsWith(u'Z')) {
        return false;
    }
    const auto parsed = QDateTime::fromString(value, Qt::ISODate);
    return parsed.isValid() && parsed.offsetFromUtc() == 0 &&
           parsed.toUTC().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'")) == value;
}

[[nodiscard]] bool validJsonObject(const QByteArray& value) {
    if (value.isEmpty() || static_cast<std::size_t>(value.size()) > maximum_json_bytes) {
        return false;
    }
    if (const auto strict = detail::scanStrictJson(QByteArrayView(value)); !strict) {
        return false;
    }
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(value, &parse_error);
    return parse_error.error == QJsonParseError::NoError && document.isObject();
}

[[nodiscard]] bool validDigest(QStringView value) {
    if (value.size() != 64) {
        return false;
    }
    return std::ranges::all_of(value, [](QChar character) {
        return (character >= u'0' && character <= u'9') || (character >= u'a' && character <= u'f');
    });
}

[[nodiscard]] bool validAssetPurpose(QStringView value) {
    if (value.isEmpty() || value.size() > maximum_asset_purpose_length) {
        return false;
    }
    return std::ranges::all_of(value, [](QChar character) {
        return (character >= u'a' && character <= u'z') ||
               (character >= u'0' && character <= u'9') || character == u'.' || character == u'_' ||
               character == u'-' || character == u':';
    });
}

[[nodiscard]] bool checkedAdd(qint64& total, qint64 amount, qint64 maximum) {
    if (amount < 0 || total > maximum - amount) {
        return false;
    }
    total += amount;
    return true;
}

[[nodiscard]] bool consumeRows(std::size_t& total, std::size_t count, std::size_t maximum) {
    if (count > maximum || total > maximum - count) {
        return false;
    }
    total += count;
    return true;
}

class Writer final {
  public:
    explicit Writer(qsizetype limit) : limit_(limit) {}

    [[nodiscard]] bool append8(std::uint8_t value) {
        return appendRaw(std::span<const unsigned char>{&value, 1U});
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

    [[nodiscard]] bool appendText(const QString& value) {
        const auto utf8 = value.toUtf8();
        if (utf8.size() < 0 ||
            static_cast<std::uint64_t>(utf8.size()) > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        return append32(static_cast<std::uint32_t>(utf8.size())) && appendRaw(QByteArrayView(utf8));
    }

    [[nodiscard]] bool appendBlob32(QByteArrayView value) {
        if (value.size() < 0 ||
            static_cast<std::uint64_t>(value.size()) > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        return append32(static_cast<std::uint32_t>(value.size())) && appendRaw(value);
    }

    [[nodiscard]] bool appendBlob64(QByteArrayView value) {
        if (value.size() < 0) {
            return false;
        }
        return append64(static_cast<std::uint64_t>(value.size())) && appendRaw(value);
    }

    template <std::size_t Size>
    [[nodiscard]] bool appendRaw(const std::array<unsigned char, Size>& value) {
        return appendRaw(std::span<const unsigned char>{value});
    }

    [[nodiscard]] bool appendRaw(QByteArrayView value) {
        if (value.size() < 0) {
            return false;
        }
        return appendRaw(
            std::span<const unsigned char>{reinterpret_cast<const unsigned char*>(value.data()),
                                           static_cast<std::size_t>(value.size())});
    }

    [[nodiscard]] bool appendRaw(std::span<const unsigned char> value) {
        if (value.size() > static_cast<std::size_t>(limit_) || bytes_.size() < 0 ||
            static_cast<std::size_t>(bytes_.size()) >
                static_cast<std::size_t>(limit_) - value.size()) {
            return false;
        }
        bytes_.append(reinterpret_cast<const char*>(value.data()),
                      static_cast<qsizetype>(value.size()));
        return true;
    }

    [[nodiscard]] const QByteArray& bytes() const noexcept { return bytes_; }
    [[nodiscard]] QByteArray take() && { return std::move(bytes_); }

  private:
    QByteArray bytes_;
    qsizetype limit_{};
};

class Reader final {
  public:
    explicit Reader(QByteArrayView bytes) : bytes_(bytes) {}

    [[nodiscard]] std::size_t remaining() const noexcept {
        return static_cast<std::size_t>(bytes_.size()) - offset_;
    }

    [[nodiscard]] auto read8() -> std::optional<std::uint8_t> {
        const auto bytes = take(1U);
        return bytes ? std::optional<std::uint8_t>{bytes->front()} : std::nullopt;
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

    [[nodiscard]] auto readText(std::size_t maximum_bytes) -> std::optional<QString> {
        const auto size = read32();
        if (!size || *size > maximum_bytes) {
            return std::nullopt;
        }
        const auto bytes = take(*size);
        if (!bytes) {
            return std::nullopt;
        }
        const QByteArray encoded(reinterpret_cast<const char*>(bytes->data()),
                                 static_cast<qsizetype>(bytes->size()));
        const auto decoded = QString::fromUtf8(encoded);
        return decoded.toUtf8() == encoded ? std::optional<QString>{decoded} : std::nullopt;
    }

    [[nodiscard]] auto readBlob32(std::size_t maximum_bytes) -> std::optional<QByteArray> {
        const auto size = read32();
        if (!size || *size > maximum_bytes) {
            return std::nullopt;
        }
        return readBlob(*size);
    }

    [[nodiscard]] auto readBlob64(std::uint64_t maximum_bytes) -> std::optional<QByteArray> {
        const auto size = read64();
        if (!size || *size > maximum_bytes ||
            *size > static_cast<std::uint64_t>(std::numeric_limits<qsizetype>::max())) {
            return std::nullopt;
        }
        return readBlob(static_cast<std::size_t>(*size));
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
    [[nodiscard]] auto readBlob(std::size_t size) -> std::optional<QByteArray> {
        const auto bytes = take(size);
        if (!bytes) {
            return std::nullopt;
        }
        return QByteArray(reinterpret_cast<const char*>(bytes->data()),
                          static_cast<qsizetype>(bytes->size()));
    }

    QByteArrayView bytes_;
    std::size_t offset_{};
};

[[nodiscard]] auto validateSnapshot(const SessionSnapshot& snapshot,
                                    const SessionArchiveLimits& limits)
    -> std::expected<void, SessionArchiveError> {
    if (!validText(snapshot.session_id) || !validText(snapshot.engine_revision) ||
        !validCanonicalUtc(snapshot.created_at_utc) || snapshot.sequence < 0 ||
        snapshot.pins.empty() || snapshot.pins.size() > maximum_revision_pins ||
        snapshot.pins.size() > limits.maximum_rows_per_session ||
        snapshot.commands.size() > limits.maximum_rows_per_session ||
        snapshot.events.size() > limits.maximum_rows_per_session ||
        snapshot.docket.size() > limits.maximum_rows_per_session ||
        snapshot.asset_references.size() > limits.maximum_rows_per_session) {
        return fail(SessionArchiveErrorCode::MalformedArchive,
                    QStringLiteral("Archived session metadata exceeds its bounds"));
    }
    if (snapshot.authority_contract != SessionAuthorityContract::LegacyV1 &&
        snapshot.authority_contract != SessionAuthorityContract::CanonicalV2) {
        return fail(SessionArchiveErrorCode::MalformedArchive,
                    QStringLiteral("Archived authority contract is invalid"));
    }

    QString previous_pack;
    for (const auto& pin : snapshot.pins) {
        if (!validText(pin.pack_id) || !validText(pin.version) || !validDigest(pin.digest) ||
            (!previous_pack.isEmpty() && previous_pack >= pin.pack_id)) {
            return fail(SessionArchiveErrorCode::MalformedArchive,
                        QStringLiteral("Archived revision pins are invalid or noncanonical"));
        }
        previous_pack = pin.pack_id;
    }

    if (snapshot.events.size() != static_cast<std::size_t>(snapshot.sequence) ||
        (snapshot.sequence == 0 && (!snapshot.commands.empty() || !snapshot.docket.empty() ||
                                    !snapshot.asset_references.empty())) ||
        (snapshot.sequence > 0 && snapshot.commands.empty())) {
        return fail(SessionArchiveErrorCode::MalformedArchive,
                    QStringLiteral("Archived session sequence does not match its rows"));
    }

    qint64 previous_expected = -1;
    QSet<QString> command_ids;
    for (std::size_t index = 0; index < snapshot.commands.size(); ++index) {
        const auto& command = snapshot.commands[index];
        if (!validText(command.command_id) || !validJsonObject(command.payload_json) ||
            !validCanonicalUtc(command.recorded_at_utc) || command.expected_sequence < 0 ||
            command.expected_sequence >= snapshot.sequence ||
            (index == 0U && command.expected_sequence != 0) ||
            (index > 0U && command.expected_sequence <= previous_expected) ||
            command_ids.contains(command.command_id)) {
            return fail(SessionArchiveErrorCode::MalformedArchive,
                        QStringLiteral("Archived commands are invalid or noncanonical"));
        }
        command_ids.insert(command.command_id);
        previous_expected = command.expected_sequence;
    }

    for (std::size_t index = 0; index < snapshot.events.size(); ++index) {
        const auto& event = snapshot.events[index];
        if (event.sequence != static_cast<qint64>(index) + 1 || !validText(event.event_type) ||
            !validJsonObject(event.payload_json) || !validText(event.authority_id)) {
            return fail(SessionArchiveErrorCode::MalformedArchive,
                        QStringLiteral("Archived events are invalid or noncanonical"));
        }
    }

    qint64 previous_docket_sequence = -1;
    QString previous_docket_id;
    QSet<QString> docket_ids;
    for (const auto& entry : snapshot.docket) {
        if (!validText(entry.entry_id) || !validText(entry.title) || !validText(entry.status) ||
            entry.event_sequence <= 0 || entry.event_sequence > snapshot.sequence ||
            (previous_docket_sequence >= 0 && (entry.event_sequence < previous_docket_sequence ||
                                               (entry.event_sequence == previous_docket_sequence &&
                                                entry.entry_id <= previous_docket_id))) ||
            docket_ids.contains(entry.entry_id)) {
            return fail(SessionArchiveErrorCode::MalformedArchive,
                        QStringLiteral("Archived docket rows are invalid or noncanonical"));
        }
        docket_ids.insert(entry.entry_id);
        previous_docket_sequence = entry.event_sequence;
        previous_docket_id = entry.entry_id;
    }

    QString previous_purpose;
    QString previous_digest;
    QSet<QString> reference_keys;
    for (const auto& reference : snapshot.asset_references) {
        const auto key = reference.digest + QChar::Null + reference.purpose;
        if (!validDigest(reference.digest) || !validAssetPurpose(reference.purpose) ||
            (!previous_purpose.isEmpty() &&
             (reference.purpose < previous_purpose ||
              (reference.purpose == previous_purpose && reference.digest <= previous_digest))) ||
            reference_keys.contains(key)) {
            return fail(SessionArchiveErrorCode::MalformedArchive,
                        QStringLiteral("Archived asset references are invalid or noncanonical"));
        }
        reference_keys.insert(key);
        previous_purpose = reference.purpose;
        previous_digest = reference.digest;
    }
    return {};
}

[[nodiscard]] auto validateDecoded(DecodedArchive& decoded, const SessionArchiveLimits& limits)
    -> std::expected<void, SessionArchiveError> {
    if (decoded.sessions.size() > limits.maximum_sessions ||
        decoded.assets.size() > limits.maximum_assets) {
        return fail(SessionArchiveErrorCode::LimitExceeded,
                    QStringLiteral("Archive entry count exceeds its bound"));
    }
    auto total_rows = decoded.sessions.size();
    if (total_rows > limits.maximum_total_rows) {
        return fail(SessionArchiveErrorCode::LimitExceeded,
                    QStringLiteral("Archive aggregate row count exceeds its bound"));
    }
    QString previous_session;
    QSet<QString> referenced_digests;
    for (const auto& snapshot : decoded.sessions) {
        if (!previous_session.isEmpty() && previous_session >= snapshot.session_id) {
            return fail(SessionArchiveErrorCode::MalformedArchive,
                        QStringLiteral("Archived sessions are duplicated or noncanonical"));
        }
        if (const auto valid = validateSnapshot(snapshot, limits); !valid) {
            return valid;
        }
        const std::array row_counts{snapshot.pins.size(), snapshot.commands.size(),
                                    snapshot.events.size(), snapshot.docket.size(),
                                    snapshot.asset_references.size()};
        for (const auto count : row_counts) {
            if (!consumeRows(total_rows, count, limits.maximum_total_rows)) {
                return fail(SessionArchiveErrorCode::LimitExceeded,
                            QStringLiteral("Archive aggregate row count exceeds its bound"));
            }
        }
        previous_session = snapshot.session_id;
        for (const auto& reference : snapshot.asset_references) {
            referenced_digests.insert(reference.digest);
        }
    }

    QString previous_asset;
    qint64 total_asset_bytes{};
    QSet<QString> archived_digests;
    for (const auto& asset : decoded.assets) {
        if (!validDigest(asset.digest) ||
            (!previous_asset.isEmpty() && previous_asset >= asset.digest) ||
            !checkedAdd(total_asset_bytes, asset.bytes.size(), limits.maximum_total_asset_bytes)) {
            return fail(SessionArchiveErrorCode::MalformedArchive,
                        QStringLiteral("Archived assets are invalid, duplicated, or too large"));
        }
        const auto actual = QString::fromLatin1(
            QCryptographicHash::hash(QByteArrayView(asset.bytes), QCryptographicHash::Sha256)
                .toHex());
        if (actual != asset.digest) {
            return fail(SessionArchiveErrorCode::DigestMismatch,
                        QStringLiteral("Archived asset digest does not match its bytes"));
        }
        archived_digests.insert(asset.digest);
        previous_asset = asset.digest;
    }
    if (referenced_digests != archived_digests) {
        return fail(SessionArchiveErrorCode::MalformedArchive,
                    QStringLiteral("Archive assets do not exactly close its references"));
    }
    return {};
}

[[nodiscard]] bool writeSnapshot(Writer& writer, const SessionSnapshot& snapshot) {
    const auto contract = snapshot.authority_contract == SessionAuthorityContract::LegacyV1
                              ? std::uint8_t{1}
                              : std::uint8_t{2};
    auto written = writer.appendText(snapshot.session_id) &&
                   writer.appendText(snapshot.engine_revision) && writer.append8(contract) &&
                   writer.append64(static_cast<std::uint64_t>(snapshot.sequence)) &&
                   writer.appendText(snapshot.created_at_utc) &&
                   writer.append32(static_cast<std::uint32_t>(snapshot.pins.size()));
    for (const auto& pin : snapshot.pins) {
        written = written && writer.appendText(pin.pack_id) && writer.appendText(pin.version) &&
                  writer.appendText(pin.digest);
    }
    written = written && writer.append32(static_cast<std::uint32_t>(snapshot.commands.size()));
    for (const auto& command : snapshot.commands) {
        written = written && writer.appendText(command.command_id) &&
                  writer.append64(static_cast<std::uint64_t>(command.expected_sequence)) &&
                  writer.appendBlob32(command.payload_json) &&
                  writer.appendText(command.recorded_at_utc);
    }
    written = written && writer.append32(static_cast<std::uint32_t>(snapshot.events.size()));
    for (const auto& event : snapshot.events) {
        written = written && writer.append64(static_cast<std::uint64_t>(event.sequence)) &&
                  writer.appendText(event.event_type) && writer.appendBlob32(event.payload_json) &&
                  writer.appendText(event.authority_id);
    }
    written = written && writer.append32(static_cast<std::uint32_t>(snapshot.docket.size()));
    for (const auto& entry : snapshot.docket) {
        written = written && writer.appendText(entry.entry_id) &&
                  writer.append64(static_cast<std::uint64_t>(entry.event_sequence)) &&
                  writer.appendText(entry.title) && writer.appendText(entry.status);
    }
    written =
        written && writer.append32(static_cast<std::uint32_t>(snapshot.asset_references.size()));
    for (const auto& reference : snapshot.asset_references) {
        written =
            written && writer.appendText(reference.digest) && writer.appendText(reference.purpose);
    }
    return written;
}

[[nodiscard]] auto boundedCount(Reader& reader, std::size_t maximum) -> std::optional<std::size_t> {
    const auto count = reader.read32();
    if (!count || *count > maximum) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(*count);
}

[[nodiscard]] auto readSequence(Reader& reader) -> std::optional<qint64> {
    const auto value = reader.read64();
    if (!value || *value > static_cast<std::uint64_t>(std::numeric_limits<qint64>::max())) {
        return std::nullopt;
    }
    return static_cast<qint64>(*value);
}

[[nodiscard]] auto readSnapshot(Reader& reader, const SessionArchiveLimits& limits,
                                std::size_t& total_rows)
    -> std::expected<SessionSnapshot, SessionArchiveError> {
    const auto session_id = reader.readText(maximum_text_bytes);
    const auto engine_revision = reader.readText(maximum_text_bytes);
    const auto contract = reader.read8();
    const auto sequence = readSequence(reader);
    const auto created_at = reader.readText(maximum_text_bytes);
    const auto pin_count =
        boundedCount(reader, std::min(maximum_revision_pins, limits.maximum_rows_per_session));
    if (!session_id || !engine_revision || !contract || (*contract != 1U && *contract != 2U) ||
        !sequence || !created_at || !pin_count) {
        return fail(SessionArchiveErrorCode::MalformedArchive,
                    QStringLiteral("Archived session header is truncated or invalid"));
    }
    if (!consumeRows(total_rows, *pin_count, limits.maximum_total_rows)) {
        return fail(SessionArchiveErrorCode::LimitExceeded,
                    QStringLiteral("Archive aggregate row count exceeds its bound"));
    }

    SessionSnapshot snapshot;
    snapshot.session_id = *session_id;
    snapshot.engine_revision = *engine_revision;
    snapshot.authority_contract = *contract == 1U ? SessionAuthorityContract::LegacyV1
                                                  : SessionAuthorityContract::CanonicalV2;
    snapshot.sequence = *sequence;
    snapshot.created_at_utc = *created_at;
    snapshot.pins.reserve(*pin_count);
    for (std::size_t index = 0; index < *pin_count; ++index) {
        const auto pack_id = reader.readText(maximum_text_bytes);
        const auto version = reader.readText(maximum_text_bytes);
        const auto digest = reader.readText(maximum_text_bytes);
        if (!pack_id || !version || !digest) {
            return fail(SessionArchiveErrorCode::MalformedArchive,
                        QStringLiteral("Archived revision pin is truncated"));
        }
        snapshot.pins.push_back(RevisionPin{*pack_id, *version, *digest});
    }

    const auto command_count = boundedCount(reader, limits.maximum_rows_per_session);
    if (!command_count) {
        return fail(SessionArchiveErrorCode::LimitExceeded,
                    QStringLiteral("Archived command count exceeds its bound"));
    }
    if (!consumeRows(total_rows, *command_count, limits.maximum_total_rows)) {
        return fail(SessionArchiveErrorCode::LimitExceeded,
                    QStringLiteral("Archive aggregate row count exceeds its bound"));
    }
    snapshot.commands.reserve(*command_count);
    for (std::size_t index = 0; index < *command_count; ++index) {
        const auto command_id = reader.readText(maximum_text_bytes);
        const auto expected_sequence = readSequence(reader);
        const auto payload = reader.readBlob32(maximum_json_bytes);
        const auto recorded_at = reader.readText(maximum_text_bytes);
        if (!command_id || !expected_sequence || !payload || !recorded_at) {
            return fail(SessionArchiveErrorCode::MalformedArchive,
                        QStringLiteral("Archived command is truncated"));
        }
        snapshot.commands.push_back(
            StoredCommand{*command_id, *expected_sequence, *payload, *recorded_at});
    }

    const auto event_count = boundedCount(reader, limits.maximum_rows_per_session);
    if (!event_count) {
        return fail(SessionArchiveErrorCode::LimitExceeded,
                    QStringLiteral("Archived event count exceeds its bound"));
    }
    if (!consumeRows(total_rows, *event_count, limits.maximum_total_rows)) {
        return fail(SessionArchiveErrorCode::LimitExceeded,
                    QStringLiteral("Archive aggregate row count exceeds its bound"));
    }
    snapshot.events.reserve(*event_count);
    for (std::size_t index = 0; index < *event_count; ++index) {
        const auto event_sequence = readSequence(reader);
        const auto event_type = reader.readText(maximum_text_bytes);
        const auto payload = reader.readBlob32(maximum_json_bytes);
        const auto authority_id = reader.readText(maximum_text_bytes);
        if (!event_sequence || !event_type || !payload || !authority_id) {
            return fail(SessionArchiveErrorCode::MalformedArchive,
                        QStringLiteral("Archived event is truncated"));
        }
        snapshot.events.push_back(
            StoredEvent{*event_sequence, *event_type, *payload, *authority_id});
    }

    const auto docket_count = boundedCount(reader, limits.maximum_rows_per_session);
    if (!docket_count) {
        return fail(SessionArchiveErrorCode::LimitExceeded,
                    QStringLiteral("Archived docket count exceeds its bound"));
    }
    if (!consumeRows(total_rows, *docket_count, limits.maximum_total_rows)) {
        return fail(SessionArchiveErrorCode::LimitExceeded,
                    QStringLiteral("Archive aggregate row count exceeds its bound"));
    }
    snapshot.docket.reserve(*docket_count);
    for (std::size_t index = 0; index < *docket_count; ++index) {
        const auto entry_id = reader.readText(maximum_text_bytes);
        const auto event_sequence = readSequence(reader);
        const auto title = reader.readText(maximum_text_bytes);
        const auto status = reader.readText(maximum_text_bytes);
        if (!entry_id || !event_sequence || !title || !status) {
            return fail(SessionArchiveErrorCode::MalformedArchive,
                        QStringLiteral("Archived docket row is truncated"));
        }
        snapshot.docket.push_back(DocketEntry{*entry_id, *event_sequence, *title, *status});
    }

    const auto reference_count = boundedCount(reader, limits.maximum_rows_per_session);
    if (!reference_count) {
        return fail(SessionArchiveErrorCode::LimitExceeded,
                    QStringLiteral("Archived asset-reference count exceeds its bound"));
    }
    if (!consumeRows(total_rows, *reference_count, limits.maximum_total_rows)) {
        return fail(SessionArchiveErrorCode::LimitExceeded,
                    QStringLiteral("Archive aggregate row count exceeds its bound"));
    }
    snapshot.asset_references.reserve(*reference_count);
    for (std::size_t index = 0; index < *reference_count; ++index) {
        const auto digest = reader.readText(maximum_text_bytes);
        const auto purpose = reader.readText(maximum_text_bytes);
        if (!digest || !purpose) {
            return fail(SessionArchiveErrorCode::MalformedArchive,
                        QStringLiteral("Archived asset reference is truncated"));
        }
        snapshot.asset_references.push_back(AssetReference{*digest, *purpose});
    }
    return snapshot;
}

[[nodiscard]] auto decodeArchive(QByteArrayView archive, const SessionArchiveLimits& limits)
    -> std::expected<DecodedArchive, SessionArchiveError> {
    if (archive.size() < 0 || archive.size() > limits.maximum_archive_bytes) {
        return fail(SessionArchiveErrorCode::LimitExceeded,
                    QStringLiteral("Archive size exceeds its bound"));
    }
    const auto minimum_size =
        archive_magic.size() + sizeof(std::uint32_t) * 2U + archive_digest_bytes;
    if (static_cast<std::size_t>(archive.size()) < minimum_size) {
        return fail(SessionArchiveErrorCode::MalformedArchive,
                    QStringLiteral("Archive is truncated"));
    }
    const auto payload_size = archive.size() - static_cast<qsizetype>(archive_digest_bytes);
    const auto payload = archive.first(payload_size);
    const auto expected_digest = archive.sliced(payload_size);
    const auto actual_digest = QCryptographicHash::hash(payload, QCryptographicHash::Sha256);
    if (QByteArrayView(actual_digest) != expected_digest) {
        return fail(SessionArchiveErrorCode::DigestMismatch,
                    QStringLiteral("Archive envelope digest does not match its contents"));
    }

    Reader reader(payload);
    const auto magic = reader.take(archive_magic.size());
    if (!magic || !std::equal(archive_magic.begin(), archive_magic.end(), magic->begin())) {
        return fail(SessionArchiveErrorCode::MalformedArchive,
                    QStringLiteral("Archive format or version is unsupported"));
    }
    const auto session_count = boundedCount(reader, limits.maximum_sessions);
    if (!session_count) {
        return fail(SessionArchiveErrorCode::LimitExceeded,
                    QStringLiteral("Archived session count exceeds its bound"));
    }
    DecodedArchive decoded;
    decoded.sessions.reserve(*session_count);
    auto total_rows = *session_count;
    if (total_rows > limits.maximum_total_rows) {
        return fail(SessionArchiveErrorCode::LimitExceeded,
                    QStringLiteral("Archive aggregate row count exceeds its bound"));
    }
    for (std::size_t index = 0; index < *session_count; ++index) {
        auto snapshot = readSnapshot(reader, limits, total_rows);
        if (!snapshot) {
            return std::unexpected(snapshot.error());
        }
        decoded.sessions.push_back(std::move(*snapshot));
    }

    const auto asset_count = boundedCount(reader, limits.maximum_assets);
    if (!asset_count) {
        return fail(SessionArchiveErrorCode::LimitExceeded,
                    QStringLiteral("Archived asset count exceeds its bound"));
    }
    decoded.assets.reserve(*asset_count);
    qint64 total_asset_bytes{};
    for (std::size_t index = 0; index < *asset_count; ++index) {
        const auto digest = reader.readText(maximum_text_bytes);
        const auto maximum_remaining = limits.maximum_total_asset_bytes - total_asset_bytes;
        const auto bytes = reader.readBlob64(static_cast<std::uint64_t>(maximum_remaining));
        if (!digest || !bytes ||
            !checkedAdd(total_asset_bytes, bytes->size(), limits.maximum_total_asset_bytes)) {
            return fail(SessionArchiveErrorCode::LimitExceeded,
                        QStringLiteral("Archived asset bytes exceed their bound"));
        }
        decoded.assets.push_back(ArchivedAsset{*digest, *bytes});
    }
    if (reader.remaining() != 0U) {
        return fail(SessionArchiveErrorCode::MalformedArchive,
                    QStringLiteral("Archive contains trailing framed data"));
    }
    if (const auto valid = validateDecoded(decoded, limits); !valid) {
        return std::unexpected(valid.error());
    }
    return decoded;
}

[[nodiscard]] auto encodeArchive(const DecodedArchive& decoded, const SessionArchiveLimits& limits)
    -> std::expected<QByteArray, SessionArchiveError> {
    Writer writer(limits.maximum_archive_bytes);
    auto written = writer.appendRaw(archive_magic) &&
                   writer.append32(static_cast<std::uint32_t>(decoded.sessions.size()));
    for (const auto& snapshot : decoded.sessions) {
        written = written && writeSnapshot(writer, snapshot);
    }
    written = written && writer.append32(static_cast<std::uint32_t>(decoded.assets.size()));
    for (const auto& asset : decoded.assets) {
        written = written && writer.appendText(asset.digest) && writer.appendBlob64(asset.bytes);
    }
    if (!written || writer.bytes().size() > limits.maximum_archive_bytes -
                                                static_cast<qsizetype>(archive_digest_bytes)) {
        return fail(SessionArchiveErrorCode::LimitExceeded,
                    QStringLiteral("Encoded archive exceeds its bound"));
    }
    const auto envelope_digest =
        QCryptographicHash::hash(QByteArrayView(writer.bytes()), QCryptographicHash::Sha256);
    if (!writer.appendRaw(QByteArrayView(envelope_digest))) {
        return fail(SessionArchiveErrorCode::LimitExceeded,
                    QStringLiteral("Encoded archive digest exceeds its bound"));
    }
    return std::move(writer).take();
}

[[nodiscard]] auto authorityContractName(SessionAuthorityContract contract)
    -> std::optional<QString> {
    switch (contract) {
    case SessionAuthorityContract::LegacyV1:
        return QStringLiteral("legacy-v1");
    case SessionAuthorityContract::CanonicalV2:
        return QStringLiteral("canonical-v2");
    }
    return std::nullopt;
}

[[nodiscard]] auto allReferencedDigests(QSqlDatabase database)
    -> std::expected<QStringList, SessionArchiveError> {
    QSqlQuery query(database);
    if (!query.exec(
            QStringLiteral("SELECT DISTINCT digest FROM asset_references ORDER BY digest"))) {
        return storeFailure(QStringLiteral("Read session asset-reference closure"), query);
    }
    QStringList digests;
    while (query.next()) {
        digests.push_back(query.value(0).toString());
    }
    return digests;
}

[[nodiscard]] auto validateAvailablePins(const DecodedArchive& decoded,
                                         const std::vector<RevisionPin>& available_revision_pins,
                                         const SessionArchiveLimits& limits)
    -> std::expected<void, SessionArchiveError> {
    if (available_revision_pins.size() > limits.maximum_total_rows) {
        return fail(SessionArchiveErrorCode::LimitExceeded,
                    QStringLiteral("Available revision-pin count exceeds its bound"));
    }
    QSet<QString> available;
    QSet<QString> available_pack_versions;
    for (const auto& pin : available_revision_pins) {
        if (!validText(pin.pack_id) || !validText(pin.version) || !validDigest(pin.digest)) {
            return fail(SessionArchiveErrorCode::InvalidArgument,
                        QStringLiteral("Available revision-pin closure is invalid"));
        }
        const auto pack_version_key = pin.pack_id + QChar::Null + pin.version;
        const auto key = pin.pack_id + QChar::Null + pin.version + QChar::Null + pin.digest;
        if (available.contains(key) || available_pack_versions.contains(pack_version_key)) {
            return fail(SessionArchiveErrorCode::InvalidArgument,
                        QStringLiteral("Available revision-pin closure contains a duplicate or "
                                       "conflicting pack version"));
        }
        available.insert(key);
        available_pack_versions.insert(pack_version_key);
    }
    for (const auto& snapshot : decoded.sessions) {
        for (const auto& pin : snapshot.pins) {
            const auto key = pin.pack_id + QChar::Null + pin.version + QChar::Null + pin.digest;
            if (!available.contains(key)) {
                return fail(SessionArchiveErrorCode::IncompatibleRevisionPins,
                            QStringLiteral("Required revision %1 %2 (%3) is unavailable")
                                .arg(pin.pack_id, pin.version, pin.digest));
            }
        }
    }
    return {};
}

[[nodiscard]] auto insertSnapshot(QSqlDatabase database, const SessionSnapshot& snapshot)
    -> std::expected<void, SessionArchiveError> {
    const auto contract = authorityContractName(snapshot.authority_contract);
    if (!contract) {
        return fail(SessionArchiveErrorCode::MalformedArchive,
                    QStringLiteral("Archived authority contract is invalid"));
    }
    QSqlQuery session(database);
    session.prepare(QStringLiteral(
        "INSERT INTO sessions(session_id, engine_revision, authority_contract, sequence, "
        "created_at_utc) VALUES(?, ?, ?, ?, ?)"));
    session.addBindValue(snapshot.session_id);
    session.addBindValue(snapshot.engine_revision);
    session.addBindValue(*contract);
    session.addBindValue(snapshot.sequence);
    session.addBindValue(snapshot.created_at_utc);
    if (!session.exec()) {
        return storeFailure(QStringLiteral("Import archived session"), session);
    }

    QSqlQuery pins(database);
    pins.prepare(QStringLiteral(
        "INSERT INTO session_pins(session_id, pack_id, version, digest) VALUES(?, ?, ?, ?)"));
    for (const auto& pin : snapshot.pins) {
        pins.bindValue(0, snapshot.session_id);
        pins.bindValue(1, pin.pack_id);
        pins.bindValue(2, pin.version);
        pins.bindValue(3, pin.digest);
        if (!pins.exec()) {
            return storeFailure(QStringLiteral("Import archived revision pin"), pins);
        }
    }

    QSqlQuery commands(database);
    commands.prepare(QStringLiteral(
        "INSERT INTO command_log(session_id, command_id, expected_sequence, payload_json, "
        "recorded_at_utc) VALUES(?, ?, ?, ?, ?)"));
    for (const auto& command : snapshot.commands) {
        commands.bindValue(0, snapshot.session_id);
        commands.bindValue(1, command.command_id);
        commands.bindValue(2, command.expected_sequence);
        commands.bindValue(3, command.payload_json);
        commands.bindValue(4, command.recorded_at_utc);
        if (!commands.exec()) {
            return storeFailure(QStringLiteral("Import archived command"), commands);
        }
    }

    QSqlQuery events(database);
    events.prepare(QStringLiteral(
        "INSERT INTO event_log(session_id, sequence, event_type, payload_json, authority_id) "
        "VALUES(?, ?, ?, ?, ?)"));
    for (const auto& event : snapshot.events) {
        events.bindValue(0, snapshot.session_id);
        events.bindValue(1, event.sequence);
        events.bindValue(2, event.event_type);
        events.bindValue(3, event.payload_json);
        events.bindValue(4, event.authority_id);
        if (!events.exec()) {
            return storeFailure(QStringLiteral("Import archived event"), events);
        }
    }

    QSqlQuery docket(database);
    docket.prepare(QStringLiteral(
        "INSERT INTO docket_projection(session_id, entry_id, event_sequence, title, status) "
        "VALUES(?, ?, ?, ?, ?)"));
    for (const auto& entry : snapshot.docket) {
        docket.bindValue(0, snapshot.session_id);
        docket.bindValue(1, entry.entry_id);
        docket.bindValue(2, entry.event_sequence);
        docket.bindValue(3, entry.title);
        docket.bindValue(4, entry.status);
        if (!docket.exec()) {
            return storeFailure(QStringLiteral("Import archived docket row"), docket);
        }
    }

    QSqlQuery references(database);
    references.prepare(QStringLiteral(
        "INSERT INTO asset_references(session_id, digest, purpose) VALUES(?, ?, ?)"));
    for (const auto& reference : snapshot.asset_references) {
        references.bindValue(0, snapshot.session_id);
        references.bindValue(1, reference.digest);
        references.bindValue(2, reference.purpose);
        if (!references.exec()) {
            return storeFailure(QStringLiteral("Import archived asset reference"), references);
        }
    }
    return {};
}

[[nodiscard]] auto archiveManifest(const DecodedArchive& decoded, QByteArrayView archive)
    -> SessionArchiveManifest {
    SessionArchiveManifest manifest;
    manifest.archive_sha256 =
        QString::fromLatin1(QCryptographicHash::hash(archive, QCryptographicHash::Sha256).toHex());
    manifest.sessions.reserve(decoded.sessions.size());
    for (const auto& snapshot : decoded.sessions) {
        manifest.sessions.push_back(SessionArchiveSessionManifest{
            snapshot.session_id,
            snapshot.engine_revision,
            snapshot.authority_contract,
            snapshot.sequence,
            snapshot.created_at_utc,
            snapshot.pins,
            snapshot.commands.size(),
            snapshot.events.size(),
            snapshot.docket.size(),
            snapshot.asset_references.size(),
        });
    }
    manifest.asset_digests.reserve(static_cast<qsizetype>(decoded.assets.size()));
    for (const auto& asset : decoded.assets) {
        manifest.asset_digests.push_back(asset.digest);
        manifest.total_asset_bytes += asset.bytes.size();
    }
    return manifest;
}

} // namespace

std::expected<void, SessionArchiveError>
SessionArchive::verifyAssetPair(const SessionStore& session_store, const AssetStore& asset_store,
                                const AssetStoreLock& lock, const QStringList& referenced_digests) {
    const auto database_identity = session_store.assetStoreIdentity();
    if (!database_identity) {
        return fail(SessionArchiveErrorCode::SessionStoreFailure,
                    database_identity.error().message);
    }
    const auto asset_identity = asset_store.identity(lock);
    if (!asset_identity) {
        return fail(SessionArchiveErrorCode::AssetStoreFailure, asset_identity.error().message);
    }
    if (asset_identity->has_value() && **asset_identity != *database_identity) {
        return fail(SessionArchiveErrorCode::AssetStoreFailure,
                    QStringLiteral("Session database and asset-store identities differ"));
    }
    const auto object_digests = asset_store.objectDigests(lock);
    if (!object_digests) {
        return fail(SessionArchiveErrorCode::AssetStoreFailure, object_digests.error().message);
    }
    auto expected = referenced_digests;
    expected.removeDuplicates();
    expected.sort();
    if (*object_digests != expected) {
        return fail(SessionArchiveErrorCode::AssetStoreFailure,
                    QStringLiteral("Session references do not exactly match CAS objects"));
    }
    for (const auto& digest : expected) {
        if (const auto bytes = asset_store.read(digest); !bytes) {
            return fail(SessionArchiveErrorCode::AssetStoreFailure,
                        QStringLiteral("Referenced CAS object %1 is unavailable: %2")
                            .arg(digest, bytes.error().message));
        }
    }
    return {};
}

std::expected<QByteArray, SessionArchiveError>
SessionArchive::exportSessions(const SessionStore& session_store, const AssetStore& asset_store,
                               const QStringList& selected_session_ids,
                               const SessionArchiveLimits& limits) {
    if (!validLimits(limits)) {
        return fail(SessionArchiveErrorCode::InvalidArgument,
                    QStringLiteral("Session archive limits are invalid"));
    }

    const auto export_all = selected_session_ids.isEmpty();
    QStringList session_ids;
    if (!export_all) {
        QSet<QString> unique;
        for (const auto& session_id : selected_session_ids) {
            if (!validText(session_id) || unique.contains(session_id)) {
                return fail(SessionArchiveErrorCode::InvalidArgument,
                            QStringLiteral("Selected session IDs are invalid or duplicated"));
            }
            unique.insert(session_id);
            session_ids.push_back(session_id);
        }
        session_ids.sort();
    }
    if (!export_all && static_cast<std::size_t>(session_ids.size()) > limits.maximum_sessions) {
        return fail(SessionArchiveErrorCode::LimitExceeded,
                    QStringLiteral("Selected session count exceeds its bound"));
    }

    // Begin the database transaction before taking the paired CAS lock, matching the mutation
    // lock order. DEFERRED takes no read lock until the queries below, so a writer already holding
    // the CAS lock can still commit before this export establishes its snapshot.
    QSqlQuery begin(session_store.database_);
    if (!begin.exec(QStringLiteral("BEGIN DEFERRED"))) {
        return storeFailure(QStringLiteral("Begin archive export snapshot"), begin);
    }
    const auto rollback = [&session_store] {
        QSqlQuery query(session_store.database_);
        static_cast<void>(query.exec(QStringLiteral("ROLLBACK")));
    };
    auto asset_lock = asset_store.acquireLock();
    if (!asset_lock) {
        rollback();
        return fail(
            SessionArchiveErrorCode::AssetStoreFailure,
            QStringLiteral("Cannot lock source asset store: %1").arg(asset_lock.error().message));
    }

    if (export_all) {
        QSqlQuery sessions(session_store.database_);
        if (!sessions.exec(QStringLiteral("SELECT session_id FROM sessions ORDER BY session_id"))) {
            rollback();
            return storeFailure(QStringLiteral("List sessions for archive export"), sessions);
        }
        while (sessions.next()) {
            session_ids.push_back(sessions.value(0).toString());
            if (static_cast<std::size_t>(session_ids.size()) > limits.maximum_sessions) {
                rollback();
                return fail(SessionArchiveErrorCode::LimitExceeded,
                            QStringLiteral("Selected session count exceeds its bound"));
            }
        }
    }

    const auto all_references = allReferencedDigests(session_store.database_);
    if (!all_references) {
        rollback();
        return std::unexpected(all_references.error());
    }
    if (const auto paired =
            verifyAssetPair(session_store, asset_store, *asset_lock, *all_references);
        !paired) {
        rollback();
        return std::unexpected(paired.error());
    }

    DecodedArchive decoded;
    decoded.sessions.reserve(static_cast<std::size_t>(session_ids.size()));
    QSet<QString> selected_asset_digests;
    for (const auto& session_id : session_ids) {
        auto snapshot = session_store.loadSession(session_id);
        if (!snapshot) {
            rollback();
            return fail(snapshot.error().code == StoreErrorCode::NotFound
                            ? SessionArchiveErrorCode::InvalidArgument
                            : SessionArchiveErrorCode::SessionStoreFailure,
                        snapshot.error().message);
        }
        if (const auto valid = validateSnapshot(*snapshot, limits); !valid) {
            rollback();
            return fail(
                SessionArchiveErrorCode::SessionStoreFailure,
                QStringLiteral("Stored session cannot be archived: %1").arg(valid.error().message));
        }
        for (const auto& reference : snapshot->asset_references) {
            selected_asset_digests.insert(reference.digest);
        }
        decoded.sessions.push_back(std::move(*snapshot));
    }

    auto sorted_digests = selected_asset_digests.values();
    sorted_digests.sort();
    if (static_cast<std::size_t>(sorted_digests.size()) > limits.maximum_assets) {
        rollback();
        return fail(SessionArchiveErrorCode::LimitExceeded,
                    QStringLiteral("Selected asset count exceeds its bound"));
    }
    qint64 total_asset_bytes{};
    decoded.assets.reserve(static_cast<std::size_t>(sorted_digests.size()));
    for (const auto& digest : sorted_digests) {
        auto bytes = asset_store.read(digest);
        if (!bytes) {
            rollback();
            return fail(SessionArchiveErrorCode::AssetStoreFailure,
                        QStringLiteral("Cannot read source asset %1: %2")
                            .arg(digest, bytes.error().message));
        }
        if (!checkedAdd(total_asset_bytes, bytes->size(), limits.maximum_total_asset_bytes)) {
            rollback();
            return fail(SessionArchiveErrorCode::LimitExceeded,
                        QStringLiteral("Selected asset bytes exceed their bound"));
        }
        decoded.assets.push_back(ArchivedAsset{digest, std::move(*bytes)});
    }
    if (const auto valid = validateDecoded(decoded, limits); !valid) {
        rollback();
        return fail(SessionArchiveErrorCode::SessionStoreFailure, valid.error().message);
    }
    QSqlQuery commit(session_store.database_);
    if (!commit.exec(QStringLiteral("COMMIT"))) {
        rollback();
        return storeFailure(QStringLiteral("Finish archive export snapshot"), commit);
    }
    return encodeArchive(decoded, limits);
}

std::expected<SessionArchiveManifest, SessionArchiveError>
SessionArchive::inspect(QByteArrayView archive, const SessionArchiveLimits& limits) {
    if (!validLimits(limits)) {
        return fail(SessionArchiveErrorCode::InvalidArgument,
                    QStringLiteral("Session archive limits are invalid"));
    }
    auto decoded = decodeArchive(archive, limits);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    return archiveManifest(*decoded, archive);
}

std::expected<SessionArchiveReplayContents, SessionArchiveError>
SessionArchive::readForReplay(QByteArrayView archive, const SessionArchiveLimits& limits) {
    if (!validLimits(limits)) {
        return fail(SessionArchiveErrorCode::InvalidArgument,
                    QStringLiteral("Session archive limits are invalid"));
    }
    auto decoded = decodeArchive(archive, limits);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    SessionArchiveReplayContents contents;
    contents.manifest = archiveManifest(*decoded, archive);
    contents.sessions = std::move(decoded->sessions);
    contents.assets.reserve(decoded->assets.size());
    for (auto& asset : decoded->assets) {
        contents.assets.push_back(
            SessionArchiveAssetContents{std::move(asset.digest), std::move(asset.bytes)});
    }
    return contents;
}

std::expected<void, SessionArchiveError>
SessionArchive::importSessions(QByteArrayView archive, SessionStore& session_store,
                               AssetStore& asset_store,
                               const SessionArchiveImportOptions& options) {
    if (!validLimits(options.limits)) {
        return fail(SessionArchiveErrorCode::InvalidArgument,
                    QStringLiteral("Session archive limits are invalid"));
    }
    auto decoded = decodeArchive(archive, options.limits);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    if (const auto compatible =
            validateAvailablePins(*decoded, options.available_revision_pins, options.limits);
        !compatible) {
        return std::unexpected(compatible.error());
    }
    if (decoded->sessions.empty()) {
        return {};
    }

    std::vector<StagedAsset> staged_assets;
    staged_assets.reserve(decoded->assets.size());
    for (const auto& asset : decoded->assets) {
        if (asset.bytes.size() > asset_store.maxAssetBytes()) {
            return fail(SessionArchiveErrorCode::LimitExceeded,
                        QStringLiteral("Archived asset exceeds the destination asset limit"));
        }
        auto staged = asset_store.stage(QByteArrayView(asset.bytes));
        if (!staged) {
            return fail(SessionArchiveErrorCode::AssetStoreFailure,
                        QStringLiteral("Cannot stage archived asset %1: %2")
                            .arg(asset.digest, staged.error().message));
        }
        if (staged->sha256() != asset.digest) {
            return fail(SessionArchiveErrorCode::DigestMismatch,
                        QStringLiteral("Staged archive asset digest changed unexpectedly"));
        }
        staged_assets.push_back(std::move(*staged));
    }

    if (const auto begun = session_store.beginImmediate(); !begun) {
        return fail(SessionArchiveErrorCode::SessionStoreFailure, begun.error().message);
    }
    const auto rollback = [&session_store] { session_store.rollback(); };

    QSqlQuery conflict(session_store.database_);
    conflict.prepare(QStringLiteral("SELECT 1 FROM sessions WHERE session_id=?"));
    for (const auto& snapshot : decoded->sessions) {
        conflict.bindValue(0, snapshot.session_id);
        if (!conflict.exec()) {
            rollback();
            return storeFailure(QStringLiteral("Check archived session conflict"), conflict);
        }
        if (conflict.next()) {
            rollback();
            return fail(SessionArchiveErrorCode::SessionConflict,
                        QStringLiteral("Session %1 already exists").arg(snapshot.session_id));
        }
        conflict.finish();
    }

    const auto existing_references = allReferencedDigests(session_store.database_);
    if (!existing_references) {
        rollback();
        return std::unexpected(existing_references.error());
    }

    auto asset_lock = asset_store.acquireLock();
    if (!asset_lock) {
        rollback();
        return fail(SessionArchiveErrorCode::AssetStoreFailure,
                    QStringLiteral("Cannot lock destination asset store: %1")
                        .arg(asset_lock.error().message));
    }
    if (const auto paired =
            verifyAssetPair(session_store, asset_store, *asset_lock, *existing_references);
        !paired) {
        rollback();
        return std::unexpected(paired.error());
    }

    for (const auto& snapshot : decoded->sessions) {
        if (const auto inserted = insertSnapshot(session_store.database_, snapshot); !inserted) {
            rollback();
            return std::unexpected(inserted.error());
        }
    }

    const auto cleanPublishedAssets = [&]() -> std::optional<QString> {
        QStringList failures;
        for (auto iterator = staged_assets.rbegin(); iterator != staged_assets.rend(); ++iterator) {
            if (const auto removed = asset_store.removeNewlyFinalized(*iterator, *asset_lock);
                !removed) {
                failures.push_back(removed.error().message);
            }
        }
        return failures.isEmpty() ? std::nullopt
                                  : std::optional<QString>{failures.join(QStringLiteral("; "))};
    };

    if (!decoded->assets.empty()) {
        for (auto& staged : staged_assets) {
            if (const auto finalized = asset_store.finalize(staged, *asset_lock); !finalized) {
                rollback();
                const auto cleanup_failure = cleanPublishedAssets();
                return fail(SessionArchiveErrorCode::AssetStoreFailure,
                            cleanup_failure
                                ? QStringLiteral("Cannot publish archived asset: %1; rollback "
                                                 "cleanup failed: %2")
                                      .arg(finalized.error().message, *cleanup_failure)
                                : QStringLiteral("Cannot publish archived asset: %1")
                                      .arg(finalized.error().message));
            }
        }
        const auto database_identity = session_store.assetStoreIdentity();
        if (!database_identity) {
            rollback();
            const auto cleanup_failure = cleanPublishedAssets();
            return fail(SessionArchiveErrorCode::SessionStoreFailure,
                        cleanup_failure
                            ? QStringLiteral("%1; CAS rollback cleanup failed: %2")
                                  .arg(database_identity.error().message, *cleanup_failure)
                            : database_identity.error().message);
        }
        if (const auto identity = asset_store.writeIdentity(*database_identity, *asset_lock);
            !identity) {
            rollback();
            const auto cleanup_failure = cleanPublishedAssets();
            return fail(
                SessionArchiveErrorCode::AssetStoreFailure,
                cleanup_failure
                    ? QStringLiteral("Cannot bind imported CAS: %1; rollback cleanup "
                                     "failed: %2")
                          .arg(identity.error().message, *cleanup_failure)
                    : QStringLiteral("Cannot bind imported CAS: %1").arg(identity.error().message));
        }
    }

    if (const auto committed = session_store.commit(); !committed) {
        rollback();
        QStringList cleanup_failures;
        for (auto& staged : staged_assets) {
            QSqlQuery references(session_store.database_);
            references.prepare(
                QStringLiteral("SELECT COUNT(*) FROM asset_references WHERE digest=?"));
            references.addBindValue(staged.sha256());
            const auto conclusively_unreferenced =
                references.exec() && references.next() && references.value(0).toLongLong() == 0;
            if (!conclusively_unreferenced) {
                continue;
            }
            if (const auto removed = asset_store.removeNewlyFinalized(staged, *asset_lock);
                !removed) {
                cleanup_failures.push_back(removed.error().message);
            }
        }
        return fail(
            SessionArchiveErrorCode::SessionStoreFailure,
            !cleanup_failures.isEmpty()
                ? QStringLiteral("%1; CAS rollback cleanup failed: %2")
                      .arg(committed.error().message, cleanup_failures.join(QStringLiteral("; ")))
                : committed.error().message);
    }
    return {};
}

} // namespace appellate::storage
