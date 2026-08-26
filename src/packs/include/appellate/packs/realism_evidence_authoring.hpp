#pragma once

#include "appellate/model/pack_id.hpp"

#include <QByteArray>
#include <QDate>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <cstddef>
#include <expected>
#include <optional>
#include <string_view>

namespace appellate::packs {

class PackCatalog;
class PackCatalogSnapshot;
enum class CatalogErrorCode;

enum class RealismEvidenceAuthoringErrorCode {
    InvalidInput,
    InvalidPack,
    CatalogFailure,
    ImmutableConflict,
};

inline constexpr std::string_view realism_evidence_authoring_engine_revision{
    "appellate.realism-evidence.codec-replay.v1"};
inline constexpr std::string_view realism_evidence_multi_trace_authoring_engine_revision{
    "appellate.realism-evidence.codec-replay-multi.v1"};
inline constexpr std::string_view realism_evidence_detached_review_engine_revision{
    "appellate.realism-evidence.detached-review-replay.v1"};

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

enum class RealismEvidenceTraceSetProfile {
    SingleTraceHelperV1,
    MultiTraceProductionV1,
};

// The trace-set API is explicit so existing single-trace callers retain the original helper
// profile and its level-1 boundary. MultiTraceProductionV1 accepts one to 256 traces and assigns
// the separate multi-trace engine revision before exact replay and normalization.
struct RealismEvidenceTraceSetAuthoringInput final {
    QString root_directory;
    QString review_resource_id;
    QJsonArray traces;
    RealismEvidenceTraceSetProfile profile{RealismEvidenceTraceSetProfile::MultiTraceProductionV1};
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

enum class IndependentReviewErrorCode {
    InvalidInput,
    InvalidReviewSource,
    InvalidHandoff,
    InvalidDeclaration,
    InvalidIndependentReviewPack,
    CatalogFailure,
    ImmutableConflict,
};

struct IndependentReviewError final {
    IndependentReviewErrorCode code{};
    QString message;
    std::optional<CatalogErrorCode> catalog_code;

    friend bool operator==(const IndependentReviewError&, const IndependentReviewError&) = default;
};

struct IndependentReviewPrepareInput final {
    model::PackRevision subject_revision;
    QString case_id;
    QDate current_utc_date;
};

// Pure deterministic output. Publishing these bytes and applying filesystem policy belongs to the
// CLI layer; this builder does not write a directory or mutate the snapshot's source catalog.
struct PreparedIndependentReview final {
    model::PackRevision subject_revision;
    QString case_id;
    QString source_review_resource_id;
    QString closure_digest;
    QString handoff_digest;
    QJsonObject handoff;
    QJsonObject declaration_template;
    QByteArray handoff_bytes;
    QByteArray declaration_template_bytes;
    RealismEvidenceCounts counts;
};

struct IndependentReviewFinalizeInput final {
    QByteArray handoff_bytes;
    QByteArray declaration_template_bytes;
    QByteArray completed_declaration_bytes;
    QDate current_utc_date;
};

struct FinalizedIndependentReview final {
    model::PackRevision revision;
    model::PackRevision dependency_revision;
    QString case_id;
    QString review_resource_id;
    QString review_sha256;
    QString closure_digest;
    QString handoff_digest;
    QJsonObject review_document;
    QJsonObject manifest;
    QByteArray review_bytes;
    QByteArray manifest_bytes;
};

[[nodiscard]] auto authorRealismEvidence(const PackCatalog& catalog,
                                         const RealismEvidenceAuthoringInput& input)
    -> std::expected<AuthoredRealismEvidence, RealismEvidenceAuthoringError>;

[[nodiscard]] auto authorRealismEvidence(const PackCatalog& catalog,
                                         const RealismEvidenceTraceSetAuthoringInput& input)
    -> std::expected<AuthoredRealismEvidence, RealismEvidenceAuthoringError>;

[[nodiscard]] auto prepareIndependentReview(const PackCatalogSnapshot& snapshot,
                                            const IndependentReviewPrepareInput& input)
    -> std::expected<PreparedIndependentReview, IndependentReviewError>;

[[nodiscard]] auto finalizeIndependentReview(const PackCatalogSnapshot& snapshot,
                                             const IndependentReviewFinalizeInput& input)
    -> std::expected<FinalizedIndependentReview, IndependentReviewError>;

} // namespace appellate::packs
