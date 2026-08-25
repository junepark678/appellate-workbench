#pragma once

#include "appellate/model/pack_id.hpp"
#include "appellate/packs/pack_reader.hpp"

#include <algorithm>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace appellate::packs {

class PackCatalog;
class PackCatalogSnapshot;

// An exact, catalog-verified pack closure. Only catalog implementations construct resolved packs.
// Callers cannot accidentally present a partially resolved set, reorder it into override order, or
// omit a transitive session pin.
class ResolvedPack final {
  public:
    [[nodiscard]] const LoadedPack& root() const noexcept { return root_; }

    // Dependencies are in deterministic post-order: every dependency precedes each pack that
    // consumes it. The root is deliberately not included in this span.
    [[nodiscard]] std::span<const LoadedPack> dependenciesDependencyFirst() const noexcept {
        return dependencies_dependency_first_;
    }

    // Includes the root and every transitive dependency, sorted by pack ID. A resolved closure
    // cannot contain two revisions of the same pack ID, so these are directly usable as the
    // canonical session-pin source.
    [[nodiscard]] std::span<const model::PackRevision> revisionsByPackId() const noexcept {
        return revisions_by_pack_id_;
    }

    [[nodiscard]] bool containsRevision(const model::PackRevision& revision) const noexcept {
        return std::ranges::find(revisions_by_pack_id_, revision) != revisions_by_pack_id_.end();
    }

    [[nodiscard]] std::optional<model::PackRevision>
    resourceOwner(std::string_view resource_id) const {
        const auto owns = [resource_id](const LoadedPack& pack) {
            return std::ranges::any_of(pack.resources, [resource_id](const auto& resource) {
                return resource.descriptor.id == resource_id;
            });
        };
        if (owns(root_)) {
            return root_.revision;
        }
        for (const auto& dependency : dependencies_dependency_first_) {
            if (owns(dependency)) {
                return dependency.revision;
            }
        }
        return std::nullopt;
    }

  private:
    friend class PackCatalog;
    friend class PackCatalogSnapshot;

    ResolvedPack(LoadedPack root, std::vector<LoadedPack> dependencies_dependency_first,
                 std::vector<model::PackRevision> revisions_by_pack_id)
        : root_(std::move(root)),
          dependencies_dependency_first_(std::move(dependencies_dependency_first)),
          revisions_by_pack_id_(std::move(revisions_by_pack_id)) {}

    LoadedPack root_;
    std::vector<LoadedPack> dependencies_dependency_first_;
    std::vector<model::PackRevision> revisions_by_pack_id_;
};

} // namespace appellate::packs
