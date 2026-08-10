#pragma once

#include "appellate/model/judge_profile.hpp"
#include "appellate/model/pack_id.hpp"
#include "appellate/model/resource.hpp"
#include "appellate/packs/error.hpp"

#include <QJsonObject>
#include <QString>

#include <expected>
#include <vector>

namespace appellate::packs {

struct ValidatedResource final {
    model::DeclarativeResource descriptor;
    QJsonObject document;
};

struct LoadedPack final {
    model::PackRevision revision;
    std::vector<model::RequiredCapability> required_capabilities;
    std::vector<model::PackDependency> dependencies;
    std::vector<ValidatedResource> resources;
    std::vector<model::BlobDescriptor> blobs;
    std::vector<model::JudgeProfile> judge_profiles;
};

class PackReader final {
  public:
    [[nodiscard]] static std::expected<LoadedPack, Error> readDirectory(const QString& directory);
};

} // namespace appellate::packs
