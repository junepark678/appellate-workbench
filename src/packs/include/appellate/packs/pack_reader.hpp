#pragma once

#include "appellate/model/judge_profile.hpp"
#include "appellate/model/pack_id.hpp"
#include "appellate/model/resource.hpp"
#include "appellate/packs/error.hpp"

#include <QJsonObject>
#include <QString>

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace appellate::packs {

struct ValidatedResource final {
    model::DeclarativeResource descriptor;
    QJsonObject document;
};

enum class PackGraphState {
    StandaloneValidated,
    DeferredReferences,
};

struct LoadedPack final {
    std::uint32_t manifest_schema_version{};
    model::PackRevision revision;
    std::vector<model::RequiredCapability> required_capabilities;
    std::vector<model::PackDependency> dependencies;
    std::vector<ValidatedResource> resources;
    std::vector<model::BlobDescriptor> blobs;
    std::vector<model::JudgeProfile> judge_profiles;
    PackGraphState graph_state{PackGraphState::StandaloneValidated};
};

enum class PackValidationScope {
    // All resource references must resolve inside this one pack.
    Standalone,
    // Schema, identity, digest, file, blob, and resource payload checks run immediately. The
    // cross-resource graph is deferred for dependency-bearing packs and must subsequently pass
    // validateResolvedGraph before becoming executable or durably installed.
    ResolvedClosure,
};

class PackReader final {
  public:
    [[nodiscard]] static std::expected<LoadedPack, Error>
    readDirectory(const QString& directory,
                  PackValidationScope scope = PackValidationScope::Standalone);

    [[nodiscard]] static std::expected<void, Error>
    validateResolvedGraph(const LoadedPack& root,
                          std::span<const LoadedPack* const> dependencies_dependency_first);
};

} // namespace appellate::packs
