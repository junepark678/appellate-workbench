#pragma once

#include <string>
#include <vector>

namespace appellate::model {

enum class CourtRole {
    District,
    Appellate,
};

enum class ProfileClass {
    FictionalComposite,
};

enum class VoiceRegister {
    Plain,
    Formal,
    Technical,
};

enum class VoiceCadence {
    Clipped,
    Measured,
    Expansive,
};

struct ProfileCompatibility final {
    std::vector<CourtRole> court_roles;
    std::vector<std::string> jurisdiction_ids;

    friend bool operator==(const ProfileCompatibility&, const ProfileCompatibility&) = default;
};

struct IssueFocus final {
    std::string topic_id;
    double weight{};

    friend bool operator==(const IssueFocus&, const IssueFocus&) = default;
};

struct InteractionStyle final {
    double directness{};
    double formality{};
    double question_length{};
    double interruption_frequency{};
    double follow_up_depth{};
    double hypothetical_frequency{};
    double concession_recall{};
    double time_strictness{};
    std::vector<IssueFocus> issue_focus;

    friend bool operator==(const InteractionStyle&, const InteractionStyle&) = default;
};

struct VoiceStyle final {
    VoiceRegister register_style{VoiceRegister::Formal};
    VoiceCadence cadence{VoiceCadence::Measured};
    double verbosity{};
    double sentence_complexity{};

    friend bool operator==(const VoiceStyle&, const VoiceStyle&) = default;
};

struct JudgeProfile final {
    std::string id;
    std::string display_name;
    ProfileClass profile_class{ProfileClass::FictionalComposite};
    ProfileCompatibility compatibility;
    InteractionStyle interaction;
    VoiceStyle voice;

    friend bool operator==(const JudgeProfile&, const JudgeProfile&) = default;
};

} // namespace appellate::model
