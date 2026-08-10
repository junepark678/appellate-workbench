#pragma once

#include <string>

namespace appellate::model {

enum class CourtRole {
    District,
    Appellate,
};

struct InteractionStyle final {
    double directness{};
    double formality{};
    double interruption_frequency{};
    double follow_up_depth{};
    double hypothetical_frequency{};
    double time_strictness{};

    friend bool operator==(const InteractionStyle&, const InteractionStyle&) = default;
};

struct JudgeProfile final {
    std::string id;
    std::string display_name;
    CourtRole court_role{CourtRole::Appellate};
    InteractionStyle interaction;

    friend bool operator==(const JudgeProfile&, const JudgeProfile&) = default;
};

} // namespace appellate::model
