#pragma once

#include <cstdint>
#include <string>

namespace appellate::model {

struct PackId final {
    std::string value;

    friend bool operator==(const PackId&, const PackId&) = default;
};

struct PackRevision final {
    PackId id;
    std::string version;
    std::string digest;

    friend bool operator==(const PackRevision&, const PackRevision&) = default;
};

struct PackDependency final {
    PackRevision revision;

    friend bool operator==(const PackDependency&, const PackDependency&) = default;
};

struct RequiredCapability final {
    std::string id;
    std::uint32_t version{};

    friend bool operator==(const RequiredCapability&, const RequiredCapability&) = default;
};

} // namespace appellate::model
