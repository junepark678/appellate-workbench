#pragma once

#include "appellate/packs/pack_reader.hpp"

#include <expected>
#include <span>

namespace appellate::packs {

// Validates capability-gated realism evidence against the exact, ownership-aware pack closure.
// The subject closure deliberately excludes every realism-review resource so a review can bind
// the case without introducing a digest self-reference.
[[nodiscard]] std::expected<void, Error>
validateRealismEvidence(const LoadedPack& root,
                        std::span<const LoadedPack* const> dependencies_dependency_first);

} // namespace appellate::packs
