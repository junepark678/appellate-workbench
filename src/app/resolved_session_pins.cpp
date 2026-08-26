#include "resolved_session_pins.hpp"

#include <QString>

namespace appellate::app {

std::vector<storage::RevisionPin> revisionPinsForSession(const packs::ResolvedPack& resolved_pack) {
    std::vector<storage::RevisionPin> pins;
    pins.reserve(resolved_pack.revisionsByPackId().size());
    for (const auto& revision : resolved_pack.revisionsByPackId()) {
        pins.push_back(storage::RevisionPin{
            QString::fromStdString(revision.id.value),
            QString::fromStdString(revision.version),
            QString::fromStdString(revision.digest),
        });
    }
    return pins;
}

} // namespace appellate::app
