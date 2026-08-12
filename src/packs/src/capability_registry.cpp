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
    SupportedCapability{"workbench.pack.dependent-deadlines", 1, 2, 2},
    SupportedCapability{"workbench.pack.named-deadlines", 1, 2, 2},
    SupportedCapability{"workbench.pack.event-date-deadlines", 1, 2, 2},
    SupportedCapability{"workbench.pack.argument-date-guards", 1, 2, 2},
    SupportedCapability{"workbench.pack.grounded-questions", 1, 2, 2},
    SupportedCapability{"workbench.pack.realism-evidence", 1, 2, 2},
    SupportedCapability{"workbench.pack.sealed-record-twins", 1, 2, 2},
    SupportedCapability{"workbench.pack.route-role-subsets", 1, 2, 2},
    SupportedCapability{"workbench.pack.workflow-instance-preconditions", 1, 2, 2},
    SupportedCapability{"workbench.pack.static-deficiency-deadlines", 1, 2, 2},
    SupportedCapability{"workbench.pack.operation-document-bindings", 1, 2, 2},
    SupportedCapability{"workbench.pack.operation-disposition-bindings", 1, 2, 2},
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
    bool uses_dependent_deadlines, bool uses_named_deadlines, bool uses_event_date_deadlines,
    bool uses_argument_date_guards, bool uses_structured_disposition, bool uses_grounded_questions,
    bool uses_realism_evidence, bool uses_sealed_record_twins, bool uses_route_role_subsets,
    bool uses_workflow_instance_preconditions, bool uses_static_deficiency_deadlines,
    bool uses_operation_document_bindings, bool uses_operation_disposition_bindings) {
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
    if (uses_dependent_deadlines &&
        !hasCapability(required_capabilities, "workbench.pack.dependent-deadlines", 1)) {
        return fail(QStringLiteral("Schema-2 dependent deadline bases and reached conditions "
                                   "require workbench.pack.dependent-deadlines version 1"));
    }
    if (uses_named_deadlines &&
        !hasCapability(required_capabilities, "workbench.pack.named-deadlines", 1)) {
        return fail(QStringLiteral("Schema-2 named deadline outputs require "
                                   "workbench.pack.named-deadlines version 1"));
    }
    if (uses_event_date_deadlines &&
        !hasCapability(required_capabilities, "workbench.pack.event-date-deadlines", 1)) {
        return fail(QStringLiteral("Schema-2 event-date deadline bases require "
                                   "workbench.pack.event-date-deadlines version 1"));
    }
    if (uses_argument_date_guards &&
        !hasCapability(required_capabilities, "workbench.pack.argument-date-guards", 1)) {
        return fail(QStringLiteral("Schema-2 argument-date guards require "
                                   "workbench.pack.argument-date-guards version 1"));
    }
    if (uses_grounded_questions &&
        !hasCapability(required_capabilities, "workbench.pack.grounded-questions", 1)) {
        return fail(QStringLiteral("Schema-2 authored grounded questions require "
                                   "workbench.pack.grounded-questions version 1"));
    }
    if (uses_realism_evidence &&
        !hasCapability(required_capabilities, "workbench.pack.realism-evidence", 1)) {
        return fail(QStringLiteral("Schema-2 exact realism evidence requires "
                                   "workbench.pack.realism-evidence version 1"));
    }
    if (uses_sealed_record_twins &&
        !hasCapability(required_capabilities, "workbench.pack.sealed-record-twins", 1)) {
        return fail(QStringLiteral("Schema-2 sealed/public record twins require "
                                   "workbench.pack.sealed-record-twins version 1"));
    }
    if (uses_route_role_subsets &&
        !hasCapability(required_capabilities, "workbench.pack.route-role-subsets", 1)) {
        return fail(QStringLiteral("Schema-2 catalog-subset filing routes require "
                                   "workbench.pack.route-role-subsets version 1"));
    }
    if (uses_workflow_instance_preconditions &&
        !hasCapability(required_capabilities, "workbench.pack.workflow-instance-preconditions",
                       1)) {
        return fail(QStringLiteral("Schema-2 workflow instance preconditions require "
                                   "workbench.pack.workflow-instance-preconditions version 1"));
    }
    if (uses_static_deficiency_deadlines &&
        !hasCapability(required_capabilities, "workbench.pack.static-deficiency-deadlines", 1)) {
        return fail(QStringLiteral("Schema-2 exact deficiency deadlines require "
                                   "workbench.pack.static-deficiency-deadlines version 1"));
    }
    if (uses_operation_document_bindings &&
        !hasCapability(required_capabilities, "workbench.pack.operation-document-bindings", 1)) {
        return fail(QStringLiteral("Schema-2 operation document bindings require "
                                   "workbench.pack.operation-document-bindings version 1"));
    }
    if (uses_operation_disposition_bindings &&
        !hasCapability(required_capabilities, "workbench.pack.operation-disposition-bindings", 1)) {
        return fail(QStringLiteral("Schema-2 operation disposition bindings require "
                                   "workbench.pack.operation-disposition-bindings version 1"));
    }
    if (uses_operation_disposition_bindings &&
        !hasCapability(required_capabilities, "workbench.pack.structured-disposition", 1)) {
        return fail(QStringLiteral("Schema-2 operation disposition bindings require "
                                   "workbench.pack.structured-disposition version 1"));
    }
    return {};
}

} // namespace appellate::packs
