#pragma once

#include "appellate/model/pack_id.hpp"
#include "appellate/packs/error.hpp"
#include "appellate/packs/pack_reader.hpp"

#include <QString>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace appellate::packs {

struct PackArchiveLimits final {
    std::size_t maximum_members{10'001};
    std::uint64_t maximum_file_bytes{512ULL * 1024ULL * 1024ULL};
    std::uint64_t maximum_total_bytes{3ULL * 1024ULL * 1024ULL * 1024ULL};

    friend bool operator==(const PackArchiveLimits&, const PackArchiveLimits&) = default;
};

class PackArchive final {
  public:
    [[nodiscard]] static std::expected<LoadedPack, Error>
    importArchive(const QString& archive_path, PackArchiveLimits limits = {});

    [[nodiscard]] static std::expected<model::PackRevision, Error>
    exportDirectory(const QString& directory, const QString& archive_path,
                    PackArchiveLimits limits = {});
};

} // namespace appellate::packs
