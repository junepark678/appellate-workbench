#pragma once

#include <string>
#include <vector>

namespace appellate::model {

struct AuthorityId final {
    std::string value;

    friend bool operator==(const AuthorityId&, const AuthorityId&) = default;
};

struct AuthorityRef final {
    AuthorityId id;
    std::string citation;
    std::string source_version;
    std::string proposition;

    friend bool operator==(const AuthorityRef&, const AuthorityRef&) = default;
};

// A primary authority is required by construction. The engine additionally rejects an
// authority whose fields are empty before it emits or applies a rule-driven event.
struct AuthorityBasis final {
    AuthorityRef primary;
    std::vector<AuthorityRef> supporting;

    friend bool operator==(const AuthorityBasis&, const AuthorityBasis&) = default;
};

} // namespace appellate::model
