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
    SupportedCapability{"workbench.pack.canonical-authority", 1, 2, 2},
    SupportedCapability{"workbench.pack.structured-disposition", 1, 2, 2},
    SupportedCapability{"workbench.pack.workflow-preconditions", 1, 2, 2},
};

[[nodiscard]] auto fail(QString message) -> std::unexpected<Error> {
    return std::unexpected(Error{ErrorCode::UnsupportedCapability, std::move(message)});
}

[[nodiscard]] bool hasCapability(std::span<const model::RequiredCapability> required_capabilities,
                                 std::string_view id, std::uint32_t version) {
    return std::ranges::any_of(required_capabilities, [id, version](const auto& capability) {
        return capability.id == id && capability.version == version;
    });
}

} // namespace

std::span<const SupportedCapability> CapabilityRegistry::supported() noexcept {
    return supported_capabilities;
}

std::expected<void, Error> CapabilityRegistry::validateDeclarations(
    std::uint32_t manifest_schema_version,
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

std::expected<void, Error> CapabilityRegistry::validateCoverage(
    std::uint32_t manifest_schema_version,
    std::span<const model::RequiredCapability> required_capabilities,
    std::span<const model::ResourceKind> resource_kinds, bool uses_workflow_preconditions,
    bool uses_structured_disposition) {
    const auto declarations = validateDeclarations(manifest_schema_version, required_capabilities);
    if (!declarations) {
        return declarations;
    }
    if (manifest_schema_version == 1) {
        return {};
    }
    if (manifest_schema_version != 2) {
        return fail(
            QStringLiteral("Unsupported manifest schema version %1").arg(manifest_schema_version));
    }

    if (!hasCapability(required_capabilities, "workbench.pack.declarative-resources", 2)) {
        return fail(QStringLiteral(
            "Manifest schema 2 requires workbench.pack.declarative-resources version 2"));
    }

    const auto contains_judge_profile =
        std::ranges::find(resource_kinds, model::ResourceKind::JudgeProfile) !=
        resource_kinds.end();
    if (contains_judge_profile &&
        !hasCapability(required_capabilities, "workbench.pack.judge-profile", 2)) {
        return fail(QStringLiteral(
            "Schema-2 judge profiles require workbench.pack.judge-profile version 2"));
    }
    if (contains_judge_profile &&
        !hasCapability(required_capabilities, "workbench.pack.voice-style", 2)) {
        return fail(
            QStringLiteral("Schema-2 judge profiles require workbench.pack.voice-style version 2"));
    }
    constexpr std::array authority_bearing_kinds{
        model::ResourceKind::AuthoritySet,
        model::ResourceKind::Case,
        model::ResourceKind::Court,
        model::ResourceKind::FilingCatalog,
        model::ResourceKind::ProcedureProfile,
        model::ResourceKind::Workflow,
    };
    const auto contains_authority_bearing_content =
        std::ranges::any_of(resource_kinds, [&authority_bearing_kinds](model::ResourceKind kind) {
            return std::ranges::find(authority_bearing_kinds, kind) !=
                   authority_bearing_kinds.end();
        });
    if (contains_authority_bearing_content &&
        !hasCapability(required_capabilities, "workbench.pack.canonical-authority", 1)) {
        return fail(QStringLiteral("Schema-2 authority-bearing content requires "
                                   "workbench.pack.canonical-authority version 1"));
    }
    if (uses_structured_disposition &&
        !hasCapability(required_capabilities, "workbench.pack.structured-disposition", 1)) {
        return fail(QStringLiteral("Schema-2 structured disposition plans require "
                                   "workbench.pack.structured-disposition version 1"));
    }
    if (uses_workflow_preconditions &&
        !hasCapability(required_capabilities, "workbench.pack.workflow-preconditions", 1)) {
        return fail(QStringLiteral("Schema-2 nonempty workflow preconditions require "
                                   "workbench.pack.workflow-preconditions version 1"));
    }
    return {};
}

} // namespace appellate::packs
