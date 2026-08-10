#pragma once

#include "appellate/packs/resolved_pack.hpp"
#include "appellate/storage/session_store.hpp"

#include <vector>

namespace appellate::app {

// The only production conversion from an exact resolved closure to persisted session identity.
// ResolvedPack guarantees one revision per pack ID and exposes them in canonical pack-ID order.
[[nodiscard]] std::vector<storage::RevisionPin>
revisionPinsForSession(const packs::ResolvedPack& resolved_pack);

} // namespace appellate::app
