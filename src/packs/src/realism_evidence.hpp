#pragma once

#include "appellate/packs/pack_reader.hpp"
#include "appellate/packs/realism_evidence_authoring.hpp"

#include <QByteArray>
#include <QDate>

#include <expected>
#include <span>

namespace appellate::packs {

// Validates capability-gated realism evidence against the exact, ownership-aware pack closure.
// The subject closure deliberately excludes every realism-review resource so a review can bind
// the case without introducing a digest self-reference.
[[nodiscard]] std::expected<void, Error>
validateRealismEvidence(const LoadedPack& root,
                        std::span<const LoadedPack* const> dependencies_dependency_first);

namespace detail {

// Reopens prepare output through the CLI's retained staging path, then applies the same closed
// JSON, association-digest, and catalog-reconstruction gates used by finalization. This is private
// to the in-tree CLI and deliberately does not create an alternate public handoff reader.
[[nodiscard]] auto validatePreparedIndependentReview(const PackCatalogSnapshot& snapshot,
                                                     const QByteArray& handoff_bytes,
                                                     const QByteArray& declaration_template_bytes,
                                                     const QDate& current_utc_date)
    -> std::expected<void, IndependentReviewError>;

} // namespace detail

} // namespace appellate::packs
