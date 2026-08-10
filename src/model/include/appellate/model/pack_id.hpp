#pragma once

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

} // namespace appellate::model
