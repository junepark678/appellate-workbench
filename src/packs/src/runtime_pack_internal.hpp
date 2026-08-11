#pragma once

#include "appellate/packs/runtime_pack.hpp"

#include <expected>
#include <span>

namespace appellate::packs {

// Projects an already graph-validated exact subject closure for evidence replay. Unlike the
// public entry points, this deliberately accepts a dependency-owned root because a detached
// review pack does not own the case it reviews.
[[nodiscard]] std::expected<RuntimePack, RuntimePackError>
loadRuntimePackForEvidence(const LoadedPack& case_owner,
                           std::span<const LoadedPack* const> subject_dependency_first);

} // namespace appellate::packs
