#include "appellate/storage/event_codec.hpp"
#include "strict_json_scan.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace appellate::storage {
namespace {

constexpr auto legacy_schema_version = 1;
constexpr auto provenance_schema_version = 2;
constexpr qsizetype maximum_payload_bytes = 1024 * 1024;
constexpr qsizetype maximum_id_characters = 256;
constexpr qsizetype maximum_citation_characters = 4096;
constexpr qsizetype maximum_source_version_characters = 256;
constexpr qsizetype maximum_proposition_characters = 16 * 1024;
constexpr qsizetype maximum_source_url_characters = 2048;
constexpr qsizetype maximum_canonical_text_code_units = 8192;
constexpr qsizetype maximum_supporting_authorities = 32;
constexpr qsizetype maximum_missing_fields = 256;

constexpr auto accepted_type = "filing.accepted";
constexpr auto deficiency_type = "filing.deficiency_issued";
constexpr auto rejected_type = "filing.rejected";

[[nodiscard]] auto fail(EventCodecErrorCode code, QString message)
    -> std::unexpected<EventCodecError> {
    return std::unexpected(EventCodecError{code, std::move(message)});
}

[[nodiscard]] auto exactKeys(const QJsonObject& object, std::initializer_list<QStringView> keys,
                             QStringView context) -> std::expected<void, EventCodecError> {
    QSet<QString> expected;
    for (const auto key : keys) {
        expected.insert(key.toString());
        if (!object.contains(key)) {
            return fail(EventCodecErrorCode::MissingField,
                        QStringLiteral("Missing %1.%2").arg(context, key));
        }
    }
    for (auto iterator = object.constBegin(); iterator != object.constEnd(); ++iterator) {
        if (!expected.contains(iterator.key())) {
            return fail(EventCodecErrorCode::UnexpectedField,
                        QStringLiteral("Unexpected %1.%2").arg(context, iterator.key()));
        }
    }
    return {};
}

[[nodiscard]] bool containsNull(QStringView value) {
    return std::ranges::find(value, QChar::Null) != value.end();
}

[[nodiscard]] auto readString(const QJsonObject& object, QStringView key, qsizetype maximum,
                              QStringView context) -> std::expected<QString, EventCodecError> {
    const auto value = object.value(key);
    if (!value.isString()) {
        return fail(EventCodecErrorCode::InvalidField,
                    QStringLiteral("%1.%2 must be a string").arg(context, key));
    }
    const auto result = value.toString();
    if (result.isEmpty() || result.size() > maximum || containsNull(result)) {
        return fail(EventCodecErrorCode::OutOfRange,
                    QStringLiteral("%1.%2 is empty or exceeds its bound").arg(context, key));
    }
    return result;
}

[[nodiscard]] bool isCanonicalId(QStringView value) {
    if (value.isEmpty() || value.size() > maximum_id_characters || containsNull(value) ||
        value.front().isSpace() || value.back().isSpace()) {
        return false;
    }
    return std::ranges::all_of(value, [](QChar character) {
        const auto code = character.unicode();
        return (code >= u'0' && code <= u'9') || (code >= u'A' && code <= u'Z') ||
               (code >= u'a' && code <= u'z') || code == u'.' || code == u'_' || code == u'-' ||
               code == u':';
    });
}

[[nodiscard]] bool roundTripsUtf8(std::string_view value) {
    const auto decoded = QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
    return decoded.toUtf8() == QByteArrayView(value.data(), static_cast<qsizetype>(value.size()));
}

[[nodiscard]] bool isNamespacedId(QStringView value) {
    if (value.size() < 3 || value.size() > 160) {
        return false;
    }
    bool has_separator = false;
    bool previous_was_separator = true;
    for (const auto character : value) {
        const auto code = character.unicode();
        const auto alphanumeric = (code >= u'0' && code <= u'9') || (code >= u'a' && code <= u'z');
        const auto separator = code == u'.' || code == u'-';
        if ((!alphanumeric && !separator) || (separator && previous_was_separator)) {
            return false;
        }
        has_separator = has_separator || separator;
        previous_was_separator = separator;
    }
    return has_separator && !previous_was_separator;
}

[[nodiscard]] bool isCanonicalDate(QStringView value) {
    if (value.size() != 10 || value.at(4) != u'-' || value.at(7) != u'-') {
        return false;
    }
    for (const auto index : {0, 1, 2, 3, 5, 6, 8, 9}) {
        if (value.at(index) < u'0' || value.at(index) > u'9') {
            return false;
        }
    }
    bool year_ok = false;
    bool month_ok = false;
    bool day_ok = false;
    const auto year = value.first(4).toInt(&year_ok);
    const auto month = value.sliced(5, 2).toUInt(&month_ok);
    const auto day = value.last(2).toUInt(&day_ok);
    return year_ok && month_ok && day_ok && year > 0 &&
           (std::chrono::year{year} / std::chrono::month{month} / std::chrono::day{day}).ok();
}

[[nodiscard]] auto readId(const QJsonObject& object, QStringView key, QStringView context)
    -> std::expected<std::string, EventCodecError> {
    const auto value = readString(object, key, maximum_id_characters, context);
    if (!value) {
        return std::unexpected(value.error());
    }
    if (!isCanonicalId(*value)) {
        return fail(EventCodecErrorCode::InvalidField,
                    QStringLiteral("%1.%2 is not a canonical ID").arg(context, key));
    }
    return value->toUtf8().toStdString();
}

[[nodiscard]] bool isLowercaseDigest(QStringView value) {
    return value.size() == 64 && std::ranges::all_of(value, [](QChar character) {
               return (character >= u'0' && character <= u'9') ||
                      (character >= u'a' && character <= u'f');
           });
}

[[nodiscard]] auto readDigest(const QJsonObject& object, QStringView key, QStringView context)
    -> std::expected<std::string, EventCodecError> {
    const auto value = readString(object, key, 64, context);
    if (!value) {
        return std::unexpected(value.error());
    }
    if (!isLowercaseDigest(*value)) {
        return fail(EventCodecErrorCode::InvalidField,
                    QStringLiteral("%1.%2 must be lowercase SHA-256").arg(context, key));
    }
    return value->toLatin1().toStdString();
}

[[nodiscard]] auto formatDate(model::LegalDate date) -> std::expected<QString, EventCodecError> {
    if (!date.value.ok()) {
        return fail(EventCodecErrorCode::InvalidField, QStringLiteral("Invalid legal date"));
    }
    const auto year = static_cast<int>(date.value.year());
    if (year < 1 || year > 9999) {
        return fail(EventCodecErrorCode::OutOfRange,
                    QStringLiteral("Legal date year must be between 0001 and 9999"));
    }
    return QStringLiteral("%1-%2-%3")
        .arg(year, 4, 10, QLatin1Char('0'))
        .arg(static_cast<unsigned>(date.value.month()), 2, 10, QLatin1Char('0'))
        .arg(static_cast<unsigned>(date.value.day()), 2, 10, QLatin1Char('0'));
}

[[nodiscard]] auto parseDate(const QJsonObject& object, QStringView key, QStringView context)
    -> std::expected<model::LegalDate, EventCodecError> {
    const auto encoded = readString(object, key, 10, context);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    if (encoded->size() != 10 || encoded->at(4) != u'-' || encoded->at(7) != u'-') {
        return fail(EventCodecErrorCode::InvalidField,
                    QStringLiteral("%1.%2 must use YYYY-MM-DD").arg(context, key));
    }
    for (const auto index : {0, 1, 2, 3, 5, 6, 8, 9}) {
        if (encoded->at(index) < u'0' || encoded->at(index) > u'9') {
            return fail(EventCodecErrorCode::InvalidField,
                        QStringLiteral("%1.%2 must use YYYY-MM-DD").arg(context, key));
        }
    }
    bool year_ok = false;
    bool month_ok = false;
    bool day_ok = false;
    const auto year = encoded->first(4).toInt(&year_ok);
    const auto month = encoded->sliced(5, 2).toUInt(&month_ok);
    const auto day = encoded->last(2).toUInt(&day_ok);
    const auto result = model::LegalDate{std::chrono::year{year} / std::chrono::month{month} /
                                         std::chrono::day{day}};
    if (!year_ok || !month_ok || !day_ok || !result.value.ok() || year == 0) {
        return fail(EventCodecErrorCode::InvalidField,
                    QStringLiteral("%1.%2 is not a valid date").arg(context, key));
    }
    return result;
}

[[nodiscard]] bool isCanonicalSignedInteger(std::string_view value) {
    if (value == "0") {
        return true;
    }
    if (value.empty()) {
        return false;
    }
    auto digits = value;
    if (digits.front() == '-') {
        digits.remove_prefix(1);
    }
    return !digits.empty() && digits.front() >= '1' && digits.front() <= '9' &&
           std::ranges::all_of(digits,
                               [](char character) { return character >= '0' && character <= '9'; });
}

[[nodiscard]] bool isCanonicalUnsignedInteger(std::string_view value) {
    return value == "0" || (!value.empty() && value.front() >= '1' && value.front() <= '9' &&
                            std::ranges::all_of(value, [](char character) {
                                return character >= '0' && character <= '9';
                            }));
}

template <typename Integer>
[[nodiscard]] auto parseInteger(const QJsonObject& object, QStringView key, QStringView context,
                                bool allow_negative) -> std::expected<Integer, EventCodecError> {
    const auto encoded = readString(object, key, 32, context);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    const auto bytes = encoded->toLatin1();
    const std::string_view view(bytes.constData(), static_cast<std::size_t>(bytes.size()));
    if ((allow_negative && !isCanonicalSignedInteger(view)) ||
        (!allow_negative && !isCanonicalUnsignedInteger(view))) {
        return fail(EventCodecErrorCode::InvalidField,
                    QStringLiteral("%1.%2 must be a canonical decimal string").arg(context, key));
    }
    Integer result{};
    const auto parsed = std::from_chars(view.data(), view.data() + view.size(), result);
    if (parsed.ec == std::errc::result_out_of_range) {
        return fail(EventCodecErrorCode::OutOfRange,
                    QStringLiteral("%1.%2 is outside its integer range").arg(context, key));
    }
    if (parsed.ec != std::errc{} || parsed.ptr != view.data() + view.size()) {
        return fail(EventCodecErrorCode::InvalidField,
                    QStringLiteral("%1.%2 is not an integer").arg(context, key));
    }
    return result;
}

[[nodiscard]] auto encodeTime(const model::LegalTime& time)
    -> std::expected<QJsonObject, EventCodecError> {
    const auto court_date = formatDate(time.court_date);
    if (!court_date) {
        return std::unexpected(court_date.error());
    }
    const auto seconds = time.instant.time_since_epoch().count();
    return QJsonObject{
        {QStringLiteral("court_date"), *court_date},
        {QStringLiteral("instant_unix_seconds"), QString::fromStdString(std::to_string(seconds))},
    };
}

[[nodiscard]] auto decodeTime(const QJsonObject& object)
    -> std::expected<model::LegalTime, EventCodecError> {
    if (const auto keys =
            exactKeys(object, {u"court_date", u"instant_unix_seconds"}, u"payload.submitted_at");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto date = parseDate(object, u"court_date", u"payload.submitted_at");
    if (!date) {
        return std::unexpected(date.error());
    }
    const auto seconds =
        parseInteger<std::int64_t>(object, u"instant_unix_seconds", u"payload.submitted_at", true);
    if (!seconds) {
        return std::unexpected(seconds.error());
    }
    return model::LegalTime{std::chrono::sys_seconds{std::chrono::seconds{*seconds}}, *date};
}

[[nodiscard]] auto encodeAuthorityType(model::AuthorityType type)
    -> std::expected<QString, EventCodecError> {
    switch (type) {
    case model::AuthorityType::Constitution:
        return QStringLiteral("constitution");
    case model::AuthorityType::Statute:
        return QStringLiteral("statute");
    case model::AuthorityType::Rule:
        return QStringLiteral("rule");
    case model::AuthorityType::Regulation:
        return QStringLiteral("regulation");
    case model::AuthorityType::Case:
        return QStringLiteral("case");
    case model::AuthorityType::Order:
        return QStringLiteral("order");
    case model::AuthorityType::AdministrativeDecision:
        return QStringLiteral("administrative_decision");
    case model::AuthorityType::Other:
        return QStringLiteral("other");
    }
    return fail(EventCodecErrorCode::IncompleteAuthority, QStringLiteral("Unknown authority type"));
}

[[nodiscard]] auto decodeAuthorityType(QStringView value) -> std::optional<model::AuthorityType> {
    if (value == u"constitution") {
        return model::AuthorityType::Constitution;
    }
    if (value == u"statute") {
        return model::AuthorityType::Statute;
    }
    if (value == u"rule") {
        return model::AuthorityType::Rule;
    }
    if (value == u"regulation") {
        return model::AuthorityType::Regulation;
    }
    if (value == u"case") {
        return model::AuthorityType::Case;
    }
    if (value == u"order") {
        return model::AuthorityType::Order;
    }
    if (value == u"administrative_decision") {
        return model::AuthorityType::AdministrativeDecision;
    }
    if (value == u"other") {
        return model::AuthorityType::Other;
    }
    return std::nullopt;
}

[[nodiscard]] auto encodePrecedentialStatus(model::PrecedentialStatus status)
    -> std::expected<QString, EventCodecError> {
    switch (status) {
    case model::PrecedentialStatus::NotApplicable:
        return QStringLiteral("not_applicable");
    case model::PrecedentialStatus::Precedential:
        return QStringLiteral("precedential");
    case model::PrecedentialStatus::Nonprecedential:
        return QStringLiteral("nonprecedential");
    }
    return fail(EventCodecErrorCode::IncompleteAuthority,
                QStringLiteral("Unknown precedential status"));
}

[[nodiscard]] auto decodePrecedentialStatus(QStringView value)
    -> std::optional<model::PrecedentialStatus> {
    if (value == u"not_applicable") {
        return model::PrecedentialStatus::NotApplicable;
    }
    if (value == u"precedential") {
        return model::PrecedentialStatus::Precedential;
    }
    if (value == u"nonprecedential") {
        return model::PrecedentialStatus::Nonprecedential;
    }
    return std::nullopt;
}

[[nodiscard]] auto encodeProvenance(const model::AuthorityProvenance& provenance)
    -> std::expected<QJsonObject, EventCodecError> {
    const auto type = encodeAuthorityType(provenance.type);
    const auto status = encodePrecedentialStatus(provenance.precedential_status);
    if (!type || !status || !roundTripsUtf8(provenance.jurisdiction_id) ||
        !roundTripsUtf8(provenance.issuing_body_id) || !roundTripsUtf8(provenance.checked_on) ||
        !roundTripsUtf8(provenance.locator) || !roundTripsUtf8(provenance.source_url)) {
        return fail(EventCodecErrorCode::IncompleteAuthority,
                    QStringLiteral("Authority provenance must be complete valid UTF-8"));
    }
    const auto jurisdiction_id = QString::fromUtf8(provenance.jurisdiction_id);
    const auto issuing_body_id = QString::fromUtf8(provenance.issuing_body_id);
    const auto checked_on = QString::fromUtf8(provenance.checked_on);
    const auto locator = QString::fromUtf8(provenance.locator);
    const auto source_url = QString::fromUtf8(provenance.source_url);
    if (!isNamespacedId(jurisdiction_id) || !isNamespacedId(issuing_body_id) ||
        !isCanonicalDate(checked_on) ||
        !model::isCanonicalAuthorityText(provenance.locator, 4096) || source_url.isEmpty() ||
        source_url.size() > maximum_source_url_characters || containsNull(source_url) ||
        !model::isCanonicalAuthoritySourceUrl(provenance.source_url)) {
        return fail(EventCodecErrorCode::IncompleteAuthority,
                    QStringLiteral("Authority provenance is incomplete or noncanonical"));
    }
    return QJsonObject{
        {QStringLiteral("authority_type"), *type},
        {QStringLiteral("checked_on"), checked_on},
        {QStringLiteral("issuing_body_id"), issuing_body_id},
        {QStringLiteral("jurisdiction_id"), jurisdiction_id},
        {QStringLiteral("locator"), locator},
        {QStringLiteral("official_source"), provenance.official_source},
        {QStringLiteral("precedential_status"), *status},
        {QStringLiteral("source_url"), source_url},
    };
}

[[nodiscard]] auto decodeProvenance(const QJsonObject& object, QStringView context)
    -> std::expected<model::AuthorityProvenance, EventCodecError> {
    if (const auto keys =
            exactKeys(object,
                      {u"authority_type", u"checked_on", u"issuing_body_id", u"jurisdiction_id",
                       u"locator", u"official_source", u"precedential_status", u"source_url"},
                      context);
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto type_text = readString(object, u"authority_type", 32, context);
    const auto jurisdiction_id = readString(object, u"jurisdiction_id", 160, context);
    const auto issuing_body_id = readString(object, u"issuing_body_id", 160, context);
    const auto status_text = readString(object, u"precedential_status", 32, context);
    const auto checked_on = readString(object, u"checked_on", 10, context);
    const auto locator = readString(object, u"locator", maximum_canonical_text_code_units, context);
    const auto source_url =
        readString(object, u"source_url", maximum_source_url_characters, context);
    const auto official_source = object.value(u"official_source");
    if (!type_text || !jurisdiction_id || !issuing_body_id || !status_text || !checked_on ||
        !locator || !source_url || !official_source.isBool()) {
        return fail(EventCodecErrorCode::IncompleteAuthority,
                    QStringLiteral("Authority provenance fields must be complete"));
    }
    const auto type = decodeAuthorityType(*type_text);
    const auto status = decodePrecedentialStatus(*status_text);
    const auto locator_bytes = locator->toUtf8().toStdString();
    const auto source_url_bytes = source_url->toUtf8().toStdString();
    if (!type || !status || !isNamespacedId(*jurisdiction_id) ||
        !isNamespacedId(*issuing_body_id) || !isCanonicalDate(*checked_on) ||
        !model::isCanonicalAuthorityText(locator_bytes, 4096) ||
        !model::isCanonicalAuthoritySourceUrl(source_url_bytes)) {
        return fail(EventCodecErrorCode::IncompleteAuthority,
                    QStringLiteral("Authority provenance is incomplete or noncanonical"));
    }
    return model::AuthorityProvenance{
        *type,
        jurisdiction_id->toUtf8().toStdString(),
        issuing_body_id->toUtf8().toStdString(),
        *status,
        official_source.toBool(),
        checked_on->toUtf8().toStdString(),
        std::move(locator_bytes),
        std::move(source_url_bytes),
    };
}

[[nodiscard]] auto encodeAuthorityRef(const model::AuthorityRef& authority)
    -> std::expected<QJsonObject, EventCodecError> {
    const auto id = QString::fromUtf8(authority.id.value);
    const auto citation = QString::fromUtf8(authority.citation);
    const auto source_version = QString::fromUtf8(authority.source_version);
    const auto proposition = QString::fromUtf8(authority.proposition);
    const auto has_provenance = authority.provenance.has_value();
    if (!roundTripsUtf8(authority.id.value) || !roundTripsUtf8(authority.citation) ||
        !roundTripsUtf8(authority.source_version) || !roundTripsUtf8(authority.proposition) ||
        !isCanonicalId(id) || citation.isEmpty() || source_version.isEmpty() ||
        source_version.size() > maximum_source_version_characters || proposition.isEmpty() ||
        containsNull(citation) || containsNull(source_version) || containsNull(proposition)) {
        return fail(EventCodecErrorCode::IncompleteAuthority,
                    QStringLiteral("Authority fields must be complete and bounded"));
    }
    if ((!has_provenance && (citation.size() > maximum_citation_characters ||
                             proposition.size() > maximum_proposition_characters)) ||
        (has_provenance && (!isNamespacedId(id) || !isCanonicalDate(source_version) ||
                            !model::isCanonicalAuthorityText(authority.citation, 4096) ||
                            !model::isCanonicalAuthorityText(authority.proposition, 4096) ||
                            !model::authorityVerificationNotBeforeSource(
                                authority.source_version, authority.provenance->checked_on)))) {
        return fail(EventCodecErrorCode::IncompleteAuthority,
                    QStringLiteral("Versioned authority metadata is noncanonical"));
    }
    QJsonObject result{
        {QStringLiteral("citation"), citation},
        {QStringLiteral("id"), id},
        {QStringLiteral("proposition"), proposition},
        {QStringLiteral("source_version"), source_version},
    };
    if (has_provenance) {
        const auto provenance = encodeProvenance(*authority.provenance);
        if (!provenance) {
            return std::unexpected(provenance.error());
        }
        result.insert(QStringLiteral("provenance"), *provenance);
    }
    return result;
}

[[nodiscard]] auto decodeAuthorityRef(const QJsonObject& object, QStringView context)
    -> std::expected<model::AuthorityRef, EventCodecError> {
    const auto has_provenance = object.contains(u"provenance");
    const auto keys =
        has_provenance
            ? exactKeys(object,
                        {u"citation", u"id", u"proposition", u"provenance", u"source_version"},
                        context)
            : exactKeys(object, {u"citation", u"id", u"proposition", u"source_version"}, context);
    if (!keys) {
        return std::unexpected(keys.error());
    }
    const auto id = readId(object, u"id", context);
    const auto citation = readString(
        object, u"citation",
        has_provenance ? maximum_canonical_text_code_units : maximum_citation_characters, context);
    const auto source_version =
        readString(object, u"source_version", maximum_source_version_characters, context);
    const auto proposition = readString(object, u"proposition",
                                        has_provenance ? maximum_canonical_text_code_units
                                                       : maximum_proposition_characters,
                                        context);
    if (!id || !citation || !source_version || !proposition) {
        const auto message = QStringLiteral("Authority fields must be complete and bounded");
        return fail(EventCodecErrorCode::IncompleteAuthority, message);
    }
    std::optional<model::AuthorityProvenance> provenance;
    if (has_provenance) {
        const auto provenance_value = object.value(u"provenance");
        const auto citation_bytes = citation->toUtf8().toStdString();
        const auto proposition_bytes = proposition->toUtf8().toStdString();
        if (!provenance_value.isObject() || !isNamespacedId(QString::fromUtf8(*id)) ||
            !isCanonicalDate(*source_version) ||
            !model::isCanonicalAuthorityText(citation_bytes, 4096) ||
            !model::isCanonicalAuthorityText(proposition_bytes, 4096)) {
            return fail(EventCodecErrorCode::IncompleteAuthority,
                        QStringLiteral("Versioned authority metadata is incomplete"));
        }
        auto decoded = decodeProvenance(provenance_value.toObject(),
                                        QStringLiteral("%1.provenance").arg(context));
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        if (!model::authorityVerificationNotBeforeSource(source_version->toUtf8().toStdString(),
                                                         decoded->checked_on)) {
            return fail(EventCodecErrorCode::IncompleteAuthority,
                        QStringLiteral("Authority verification predates its source version"));
        }
        provenance = std::move(*decoded);
    }
    return model::AuthorityRef{
        model::AuthorityId{*id},
        citation->toUtf8().toStdString(),
        source_version->toUtf8().toStdString(),
        proposition->toUtf8().toStdString(),
        std::move(provenance),
    };
}

[[nodiscard]] auto encodeAuthority(const model::AuthorityBasis& authority)
    -> std::expected<QJsonObject, EventCodecError> {
    if (authority.supporting.size() > static_cast<std::size_t>(maximum_supporting_authorities)) {
        return fail(EventCodecErrorCode::OutOfRange,
                    QStringLiteral("Too many supporting authorities"));
    }
    const auto primary = encodeAuthorityRef(authority.primary);
    if (!primary) {
        return std::unexpected(primary.error());
    }
    QJsonArray supporting;
    QSet<QString> ids{QString::fromUtf8(authority.primary.id.value)};
    for (const auto& item : authority.supporting) {
        const auto encoded = encodeAuthorityRef(item);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        const auto id = QString::fromUtf8(item.id.value);
        if (ids.contains(id)) {
            return fail(EventCodecErrorCode::InvalidField,
                        QStringLiteral("Authority IDs must be unique"));
        }
        ids.insert(id);
        supporting.append(*encoded);
    }
    return QJsonObject{
        {QStringLiteral("primary"), *primary},
        {QStringLiteral("supporting"), supporting},
    };
}

[[nodiscard]] auto decodeAuthority(const QJsonObject& object)
    -> std::expected<model::AuthorityBasis, EventCodecError> {
    if (const auto keys = exactKeys(object, {u"primary", u"supporting"}, u"payload.authority");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto primary_value = object.value(u"primary");
    const auto supporting_value = object.value(u"supporting");
    if (!primary_value.isObject() || !supporting_value.isArray()) {
        return fail(EventCodecErrorCode::IncompleteAuthority,
                    QStringLiteral("Authority primary/supporting have invalid types"));
    }
    const auto primary = decodeAuthorityRef(primary_value.toObject(), u"payload.authority.primary");
    if (!primary) {
        return std::unexpected(primary.error());
    }
    const auto array = supporting_value.toArray();
    if (array.size() > maximum_supporting_authorities) {
        return fail(EventCodecErrorCode::OutOfRange,
                    QStringLiteral("Too many supporting authorities"));
    }
    std::vector<model::AuthorityRef> supporting;
    supporting.reserve(static_cast<std::size_t>(array.size()));
    QSet<QString> ids{QString::fromUtf8(primary->id.value)};
    for (const auto& value : array) {
        if (!value.isObject()) {
            return fail(EventCodecErrorCode::IncompleteAuthority,
                        QStringLiteral("Supporting authority must be an object"));
        }
        auto decoded = decodeAuthorityRef(value.toObject(), u"payload.authority.supporting");
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        const auto id = QString::fromUtf8(decoded->id.value);
        if (ids.contains(id)) {
            return fail(EventCodecErrorCode::InvalidField,
                        QStringLiteral("Authority IDs must be unique"));
        }
        ids.insert(id);
        supporting.push_back(std::move(*decoded));
    }
    return model::AuthorityBasis{*primary, std::move(supporting)};
}

[[nodiscard]] auto authoritySchemaVersion(const model::AuthorityBasis& authority)
    -> std::expected<int, EventCodecError> {
    const auto has_provenance = authority.primary.provenance.has_value();
    if (std::ranges::any_of(authority.supporting, [&](const auto& supporting) {
            return supporting.provenance.has_value() != has_provenance;
        })) {
        return fail(EventCodecErrorCode::IncompleteAuthority,
                    QStringLiteral("Authority basis mixes legacy and provenance-bearing refs"));
    }
    return has_provenance ? provenance_schema_version : legacy_schema_version;
}

[[nodiscard]] auto authorityOf(const model::LegalEvent& event) -> const model::AuthorityBasis& {
    return std::visit(
        [](const auto& concrete) -> const model::AuthorityBasis& { return concrete.authority; },
        event);
}

[[nodiscard]] auto rejectionReason(model::FilingRejectionReason reason) -> QString {
    switch (reason) {
    case model::FilingRejectionReason::UnauthorizedActor:
        return QStringLiteral("unauthorized_actor");
    case model::FilingRejectionReason::WrongFilingType:
        return QStringLiteral("wrong_filing_type");
    case model::FilingRejectionReason::CureDeadlineExpired:
        return QStringLiteral("cure_deadline_expired");
    case model::FilingRejectionReason::DeficiencyNotCured:
        return QStringLiteral("deficiency_not_cured");
    case model::FilingRejectionReason::ProceedingAlreadyDocketed:
        return QStringLiteral("proceeding_already_docketed");
    }
    std::unreachable();
}

[[nodiscard]] auto parseRejectionReason(QStringView value)
    -> std::optional<model::FilingRejectionReason> {
    if (value == u"unauthorized_actor") {
        return model::FilingRejectionReason::UnauthorizedActor;
    }
    if (value == u"wrong_filing_type") {
        return model::FilingRejectionReason::WrongFilingType;
    }
    if (value == u"cure_deadline_expired") {
        return model::FilingRejectionReason::CureDeadlineExpired;
    }
    if (value == u"deficiency_not_cured") {
        return model::FilingRejectionReason::DeficiencyNotCured;
    }
    if (value == u"proceeding_already_docketed") {
        return model::FilingRejectionReason::ProceedingAlreadyDocketed;
    }
    return std::nullopt;
}

template <typename Event>
[[nodiscard]] auto encodeCommon(const Event& event) -> std::expected<QJsonObject, EventCodecError> {
    const auto submitted_at = encodeTime(event.submitted_at);
    const auto authority = encodeAuthority(event.authority);
    const auto session_id = QString::fromUtf8(event.session_id.value);
    const auto submission_id = QString::fromUtf8(event.submission_id.value);
    const auto actor_id = QString::fromUtf8(event.actor_id.value);
    const auto filing_type = QString::fromUtf8(event.filing_type.value);
    if (!submitted_at) {
        return std::unexpected(submitted_at.error());
    }
    if (!authority) {
        return std::unexpected(authority.error());
    }
    if (!roundTripsUtf8(event.session_id.value) || !roundTripsUtf8(event.submission_id.value) ||
        !roundTripsUtf8(event.actor_id.value) || !roundTripsUtf8(event.filing_type.value) ||
        !isCanonicalId(session_id) || !isCanonicalId(submission_id) || !isCanonicalId(actor_id) ||
        !isCanonicalId(filing_type)) {
        return fail(EventCodecErrorCode::InvalidField,
                    QStringLiteral("Event contains invalid IDs"));
    }
    return QJsonObject{
        {QStringLiteral("actor_id"), actor_id},
        {QStringLiteral("authority"), *authority},
        {QStringLiteral("filing_type"), filing_type},
        {QStringLiteral("session_id"), session_id},
        {QStringLiteral("submission_id"), submission_id},
        {QStringLiteral("submitted_at"), *submitted_at},
    };
}

template <typename Id>
[[nodiscard]] auto decodeId(const QJsonObject& object, QStringView key)
    -> std::expected<Id, EventCodecError> {
    const auto decoded = readId(object, key, u"payload");
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    return Id{*decoded};
}

struct DecodedCommon final {
    model::SessionId session_id;
    model::SubmissionId submission_id;
    model::ActorId actor_id;
    model::FilingTypeId filing_type;
    model::LegalTime submitted_at;
    model::AuthorityBasis authority;
};

[[nodiscard]] auto decodeCommon(const QJsonObject& payload)
    -> std::expected<DecodedCommon, EventCodecError> {
    const auto session_id = decodeId<model::SessionId>(payload, u"session_id");
    const auto submission_id = decodeId<model::SubmissionId>(payload, u"submission_id");
    const auto actor_id = decodeId<model::ActorId>(payload, u"actor_id");
    const auto filing_type = decodeId<model::FilingTypeId>(payload, u"filing_type");
    const auto submitted_value = payload.value(u"submitted_at");
    const auto authority_value = payload.value(u"authority");
    if (!session_id || !submission_id || !actor_id || !filing_type) {
        const auto* error = !session_id      ? &session_id.error()
                            : !submission_id ? &submission_id.error()
                            : !actor_id      ? &actor_id.error()
                                             : &filing_type.error();
        return std::unexpected(*error);
    }
    if (!submitted_value.isObject()) {
        return fail(EventCodecErrorCode::InvalidField,
                    QStringLiteral("payload.submitted_at must be an object"));
    }
    if (!authority_value.isObject()) {
        return fail(EventCodecErrorCode::IncompleteAuthority,
                    QStringLiteral("payload.authority must be an object"));
    }
    const auto submitted_at = decodeTime(submitted_value.toObject());
    const auto authority = decodeAuthority(authority_value.toObject());
    if (!submitted_at) {
        return std::unexpected(submitted_at.error());
    }
    if (!authority) {
        return std::unexpected(authority.error());
    }
    return DecodedCommon{*session_id,  *submission_id, *actor_id,
                         *filing_type, *submitted_at,  *authority};
}

[[nodiscard]] auto encodePayload(const model::FilingAccepted& event)
    -> std::expected<QJsonObject, EventCodecError> {
    auto payload = encodeCommon(event);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    const auto digest = QString::fromLatin1(event.document_sha256);
    if (!isLowercaseDigest(digest) || event.docket_sequence == 0) {
        return fail(EventCodecErrorCode::InvalidField,
                    QStringLiteral("Accepted filing has invalid digest or docket sequence"));
    }
    payload->insert(QStringLiteral("docket_sequence"),
                    QString::fromStdString(std::to_string(event.docket_sequence)));
    payload->insert(QStringLiteral("document_sha256"), digest);
    return payload;
}

[[nodiscard]] auto encodePayload(const model::FilingDeficiencyIssued& event)
    -> std::expected<QJsonObject, EventCodecError> {
    auto payload = encodeCommon(event);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    const auto digest = QString::fromLatin1(event.document_sha256);
    const auto deadline = formatDate(event.cure_deadline);
    if (!isLowercaseDigest(digest) || event.docket_sequence == 0 || !deadline ||
        event.missing_fields.empty() ||
        event.missing_fields.size() > static_cast<std::size_t>(maximum_missing_fields)) {
        return fail(EventCodecErrorCode::InvalidField,
                    QStringLiteral("Deficiency event has invalid fields"));
    }
    QJsonArray missing_fields;
    QSet<QString> seen;
    for (const auto& field : event.missing_fields) {
        const auto id = QString::fromUtf8(field.value);
        if (!roundTripsUtf8(field.value) || !isCanonicalId(id) || seen.contains(id)) {
            return fail(EventCodecErrorCode::InvalidField,
                        QStringLiteral("Missing-field IDs must be unique and canonical"));
        }
        seen.insert(id);
        missing_fields.append(id);
    }
    payload->insert(QStringLiteral("cure_deadline"), *deadline);
    payload->insert(QStringLiteral("docket_sequence"),
                    QString::fromStdString(std::to_string(event.docket_sequence)));
    payload->insert(QStringLiteral("document_sha256"), digest);
    payload->insert(QStringLiteral("missing_fields"), missing_fields);
    return payload;
}

[[nodiscard]] auto encodePayload(const model::FilingRejected& event)
    -> std::expected<QJsonObject, EventCodecError> {
    auto payload = encodeCommon(event);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    payload->insert(QStringLiteral("reason"), rejectionReason(event.reason));
    return payload;
}

[[nodiscard]] auto decodeAccepted(const QJsonObject& payload)
    -> std::expected<model::LegalEvent, EventCodecError> {
    if (const auto keys =
            exactKeys(payload,
                      {u"actor_id", u"authority", u"docket_sequence", u"document_sha256",
                       u"filing_type", u"session_id", u"submission_id", u"submitted_at"},
                      u"payload");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto common = decodeCommon(payload);
    const auto digest = readDigest(payload, u"document_sha256", u"payload");
    const auto docket = parseInteger<std::uint64_t>(payload, u"docket_sequence", u"payload", false);
    if (!common) {
        return std::unexpected(common.error());
    }
    if (!digest) {
        return std::unexpected(digest.error());
    }
    if (!docket) {
        return std::unexpected(docket.error());
    }
    if (*docket == 0) {
        return fail(EventCodecErrorCode::OutOfRange,
                    QStringLiteral("payload.docket_sequence must be positive"));
    }
    return model::FilingAccepted{common->session_id,
                                 common->submission_id,
                                 common->actor_id,
                                 common->filing_type,
                                 common->submitted_at,
                                 *digest,
                                 *docket,
                                 common->authority};
}

[[nodiscard]] auto decodeDeficiency(const QJsonObject& payload)
    -> std::expected<model::LegalEvent, EventCodecError> {
    if (const auto keys = exactKeys(
            payload,
            {u"actor_id", u"authority", u"cure_deadline", u"docket_sequence", u"document_sha256",
             u"filing_type", u"missing_fields", u"session_id", u"submission_id", u"submitted_at"},
            u"payload");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto common = decodeCommon(payload);
    const auto digest = readDigest(payload, u"document_sha256", u"payload");
    const auto docket = parseInteger<std::uint64_t>(payload, u"docket_sequence", u"payload", false);
    const auto deadline = parseDate(payload, u"cure_deadline", u"payload");
    const auto missing_value = payload.value(u"missing_fields");
    if (!common) {
        return std::unexpected(common.error());
    }
    if (!digest) {
        return std::unexpected(digest.error());
    }
    if (!docket) {
        return std::unexpected(docket.error());
    }
    if (*docket == 0) {
        return fail(EventCodecErrorCode::OutOfRange,
                    QStringLiteral("payload.docket_sequence must be positive"));
    }
    if (!deadline) {
        return std::unexpected(deadline.error());
    }
    if (!missing_value.isArray() || missing_value.toArray().isEmpty() ||
        missing_value.toArray().size() > maximum_missing_fields) {
        return fail(EventCodecErrorCode::InvalidField,
                    QStringLiteral("payload.missing_fields must be a bounded non-empty array"));
    }
    std::vector<model::FilingFieldId> missing_fields;
    QSet<QString> seen;
    for (const auto& value : missing_value.toArray()) {
        if (!value.isString() || !isCanonicalId(value.toString()) ||
            seen.contains(value.toString())) {
            return fail(EventCodecErrorCode::InvalidField,
                        QStringLiteral("Missing-field IDs must be unique strings"));
        }
        seen.insert(value.toString());
        missing_fields.push_back(model::FilingFieldId{value.toString().toUtf8().toStdString()});
    }
    return model::FilingDeficiencyIssued{common->session_id,
                                         common->submission_id,
                                         common->actor_id,
                                         common->filing_type,
                                         common->submitted_at,
                                         *digest,
                                         std::move(missing_fields),
                                         *deadline,
                                         *docket,
                                         common->authority};
}

[[nodiscard]] auto decodeRejected(const QJsonObject& payload)
    -> std::expected<model::LegalEvent, EventCodecError> {
    if (const auto keys = exactKeys(payload,
                                    {u"actor_id", u"authority", u"filing_type", u"reason",
                                     u"session_id", u"submission_id", u"submitted_at"},
                                    u"payload");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto common = decodeCommon(payload);
    const auto reason_value = readString(payload, u"reason", 64, u"payload");
    if (!common) {
        return std::unexpected(common.error());
    }
    if (!reason_value) {
        return std::unexpected(reason_value.error());
    }
    const auto reason = parseRejectionReason(*reason_value);
    if (!reason) {
        return fail(EventCodecErrorCode::InvalidField, QStringLiteral("payload.reason is unknown"));
    }
    return model::FilingRejected{common->session_id,  common->submission_id, common->actor_id,
                                 common->filing_type, common->submitted_at,  *reason,
                                 common->authority};
}

} // namespace

std::expected<QByteArray, EventCodecError> encodeEvent(const model::LegalEvent& event) {
    const auto version = authoritySchemaVersion(authorityOf(event));
    if (!version) {
        return std::unexpected(version.error());
    }
    const auto payload = std::visit(
        [](const auto& concrete) -> std::expected<QJsonObject, EventCodecError> {
            return encodePayload(concrete);
        },
        event);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    const QJsonObject envelope{
        {QStringLiteral("event_type"), eventType(event)},
        {QStringLiteral("payload"), *payload},
        {QStringLiteral("schema_version"), *version},
    };
    const auto result = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    if (result.size() > maximum_payload_bytes) {
        return fail(EventCodecErrorCode::PayloadTooLarge,
                    QStringLiteral("Encoded event exceeds the size limit"));
    }
    return result;
}

std::expected<model::LegalEvent, EventCodecError> decodeEvent(QByteArrayView encoded) {
    if (encoded.isEmpty() || encoded.size() > maximum_payload_bytes) {
        return fail(EventCodecErrorCode::PayloadTooLarge,
                    QStringLiteral("Event is empty or exceeds the size limit"));
    }
    if (const auto scan = detail::scanStrictJson(encoded); !scan) {
        return fail(scan.error().code == detail::StrictJsonErrorCode::DuplicateMember
                        ? EventCodecErrorCode::DuplicateMember
                        : EventCodecErrorCode::InvalidJson,
                    scan.error().message);
    }
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(encoded.toByteArray(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        return fail(EventCodecErrorCode::InvalidJson,
                    QStringLiteral("Invalid event JSON: %1").arg(parse_error.errorString()));
    }
    const auto envelope = document.object();
    if (const auto keys =
            exactKeys(envelope, {u"event_type", u"payload", u"schema_version"}, u"event");
        !keys) {
        return std::unexpected(keys.error());
    }
    const auto version = envelope.value(u"schema_version");
    if (!version.isDouble() || (version.toDouble() != legacy_schema_version &&
                                version.toDouble() != provenance_schema_version)) {
        return fail(EventCodecErrorCode::UnsupportedVersion,
                    QStringLiteral("Unsupported event schema version"));
    }
    const auto type = readString(envelope, u"event_type", 64, u"event");
    if (!type) {
        return std::unexpected(type.error());
    }
    if (!envelope.value(u"payload").isObject()) {
        return fail(EventCodecErrorCode::InvalidField,
                    QStringLiteral("event.payload must be an object"));
    }
    const auto payload = envelope.value(u"payload").toObject();
    auto decoded = [&]() -> std::expected<model::LegalEvent, EventCodecError> {
        if (*type == QLatin1StringView(accepted_type)) {
            return decodeAccepted(payload);
        }
        if (*type == QLatin1StringView(deficiency_type)) {
            return decodeDeficiency(payload);
        }
        if (*type == QLatin1StringView(rejected_type)) {
            return decodeRejected(payload);
        }
        return fail(EventCodecErrorCode::UnknownEventType,
                    QStringLiteral("Unknown event type %1").arg(*type));
    }();
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    const auto authority_version = authoritySchemaVersion(authorityOf(*decoded));
    if (!authority_version || *authority_version != version.toInt()) {
        return fail(EventCodecErrorCode::IncompleteAuthority,
                    QStringLiteral("Event schema version does not match its authority form"));
    }
    return decoded;
}

QString eventType(const model::LegalEvent& event) {
    return std::visit(
        [](const auto& concrete) {
            using Event = std::remove_cvref_t<decltype(concrete)>;
            if constexpr (std::same_as<Event, model::FilingAccepted>) {
                return QString::fromLatin1(accepted_type);
            } else if constexpr (std::same_as<Event, model::FilingDeficiencyIssued>) {
                return QString::fromLatin1(deficiency_type);
            } else {
                return QString::fromLatin1(rejected_type);
            }
        },
        event);
}

QString primaryAuthorityId(const model::LegalEvent& event) {
    return std::visit(
        [](const auto& concrete) { return QString::fromUtf8(concrete.authority.primary.id.value); },
        event);
}

} // namespace appellate::storage
