#pragma once

#include "appellate/model/pack_id.hpp"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <cstddef>
#include <expected>
#include <string_view>

namespace appellate::packs {

class PackCatalog;

enum class RealismEvidenceAuthoringErrorCode {
    InvalidInput,
    InvalidPack,
    CatalogFailure,
    ImmutableConflict,
};

inline constexpr std::string_view realism_evidence_authoring_engine_revision{
    "appellate.realism-evidence.codec-replay.v1"};

struct RealismEvidenceAuthoringError final {
    RealismEvidenceAuthoringErrorCode code{};
    QString message;

    friend bool operator==(const RealismEvidenceAuthoringError&,
                           const RealismEvidenceAuthoringError&) = default;
};

struct RealismEvidenceAuthoringInput final {
    QString root_directory;
    QString review_resource_id;
    QJsonObject trace;
};

struct RealismEvidenceCounts final {
    std::size_t packs{};
    std::size_t resources{};
    std::size_t blobs{};
    std::size_t traces{};
    std::size_t record_checks{};
    std::size_t authorities{};

    friend bool operator==(const RealismEvidenceCounts&, const RealismEvidenceCounts&) = default;
};

// Builds a complete schema-v2 realism review without writing to the source directory. The input
// directory may contain an incomplete target review scaffold, but no incomplete pack object or
// relaxed validation mode escapes this authoring boundary. The returned bytes have already passed
// normal exact catalog resolution and realism-evidence validation in memory.
struct AuthoredRealismEvidence final {
    model::PackRevision root_revision;
    QString case_id;
    QString review_resource_id;
    QString review_path;
    QByteArray source_review_bytes;
    QByteArray source_manifest_bytes;
    QByteArray review_bytes;
    QString review_sha256;
    QByteArray manifest_bytes;
    QString closure_digest;
    RealismEvidenceCounts counts;
};

[[nodiscard]] auto authorRealismEvidence(const PackCatalog& catalog,
                                         const RealismEvidenceAuthoringInput& input)
    -> std::expected<AuthoredRealismEvidence, RealismEvidenceAuthoringError>;

} // namespace appellate::packs
