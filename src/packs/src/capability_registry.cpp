#include "appellate/packs/capability_registry.hpp"

#include <QString>

#include <algorithm>
#include <array>
#include <utility>

namespace appellate::packs {
namespace {

constexpr std::array supported_capabilities{
    SupportedCapability{"workbench.pack.declarative-resources", 1, 1, 1},
    SupportedCapability{"workbench.pack.declarative-resources", 2, 2, 2},
    SupportedCapability{"workbench.pack.judge-profile", 1, 1, 1},
    SupportedCapability{"workbench.pack.judge-profile", 2, 2, 2},
    SupportedCapability{"workbench.pack.voice-style", 1, 1, 1},
    SupportedCapability{"workbench.pack.voice-style", 2, 2, 2},
};

[[nodiscard]] auto fail(QString message) -> std::unexpected<Error> {
    return std::unexpected(Error{ErrorCode::UnsupportedCapability, std::move(message)});
}

} // namespace

std::span<const SupportedCapability> CapabilityRegistry::supported() noexcept {
    return supported_capabilities;
}

std::expected<void, Error>
CapabilityRegistry::validate(std::uint32_t manifest_schema_version,
                             std::span<const model::RequiredCapability> required_capabilities) {
    for (const auto& required : required_capabilities) {
        const auto supported = std::ranges::find_if(
            supported_capabilities, [&required](const SupportedCapability& candidate) {
                return candidate.id == required.id && candidate.version == required.version;
            });
        if (supported == supported_capabilities.end()) {
            return fail(QStringLiteral("Unsupported required capability %1 version %2")
                            .arg(QString::fromStdString(required.id))
                            .arg(required.version));
        }
        if (manifest_schema_version < supported->minimum_manifest_schema_version ||
            manifest_schema_version > supported->maximum_manifest_schema_version) {
            return fail(
                QStringLiteral("Capability %1 version %2 is unavailable for manifest schema %3")
                    .arg(QString::fromStdString(required.id))
                    .arg(required.version)
                    .arg(manifest_schema_version));
        }
    }
    return {};
}

} // namespace appellate::packs
