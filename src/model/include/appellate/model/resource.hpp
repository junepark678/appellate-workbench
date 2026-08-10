#pragma once

#include <cstdint>
#include <string>

namespace appellate::model {

enum class ResourceKind {
    ArgumentConfig,
    AuthoritySet,
    BenchConfiguration,
    Case,
    Court,
    FilingCatalog,
    Form,
    JudgeProfile,
    ProcedureProfile,
    RealismReview,
    Record,
    Workflow,
};

struct DeclarativeResource final {
    ResourceKind kind{};
    std::string id;
    std::uint32_t schema_version{};
    std::string path;
    std::string sha256;

    friend bool operator==(const DeclarativeResource&, const DeclarativeResource&) = default;
};

} // namespace appellate::model
