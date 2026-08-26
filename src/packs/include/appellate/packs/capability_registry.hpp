#pragma once

#include "appellate/model/pack_id.hpp"
#include "appellate/model/resource.hpp"
#include "appellate/packs/error.hpp"

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace appellate::packs {

struct SupportedCapability final {
    std::string_view id;
    std::uint32_t version{};
    std::uint32_t minimum_manifest_schema_version{};
    std::uint32_t maximum_manifest_schema_version{};
};

// The declarative trust boundary negotiates capabilities exactly. A pack may
// only require an ID/version pair listed here for its manifest schema version.
// There is deliberately no best-effort downgrade or unknown-capability path.
class CapabilityRegistry final {
  public:
    [[nodiscard]] static std::span<const SupportedCapability> supported() noexcept;

    [[nodiscard]] static std::expected<void, Error>
    validateDeclarations(std::uint32_t manifest_schema_version,
                         std::span<const model::RequiredCapability> required_capabilities);

    // In addition to rejecting unknown declarations, verifies that a pack has
    // declared every capability required by the resource kinds it contains.
    // Schema v1 remains declaration-only for byte/behavior compatibility;
    // schema v2 and later fail closed on an incomplete declaration.
    [[nodiscard]] static std::expected<void, Error> validateCoverage(
        std::uint32_t manifest_schema_version,
        std::span<const model::RequiredCapability> required_capabilities,
        std::span<const model::ResourceKind> resource_kinds, bool uses_workflow_preconditions,
        bool uses_dependent_deadlines, bool uses_named_deadlines, bool uses_event_date_deadlines,
        bool uses_argument_date_guards, bool uses_structured_disposition,
        bool uses_grounded_questions, bool uses_realism_evidence, bool uses_sealed_record_twins,
        bool uses_route_role_subsets = false, bool uses_workflow_instance_preconditions = false,
        bool uses_static_deficiency_deadlines = false,
        bool uses_operation_document_bindings = false,
        bool uses_operation_disposition_bindings = false, bool uses_route_filing_bindings = false,
        bool uses_alternative_event_date_deadlines = false,
        bool uses_operation_legal_time_guards = false);
};

} // namespace appellate::packs
