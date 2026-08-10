#pragma once

#include "appellate/model/judge_profile.hpp"
#include "appellate/model/pack_id.hpp"
#include "appellate/packs/error.hpp"

#include <QString>

#include <expected>
#include <vector>

namespace appellate::packs {

struct LoadedPack final {
    model::PackRevision revision;
    std::vector<model::JudgeProfile> judge_profiles;
};

class PackReader final {
  public:
    [[nodiscard]] static std::expected<LoadedPack, Error> readDirectory(const QString& directory);
};

} // namespace appellate::packs
