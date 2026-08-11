#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace appellate::app {
class RecordAccessSessionController;
}

namespace appellate::ui {
class RecordWorkspace;
}

namespace appellate::model {

enum class RecordAccessAction {
    Grant,
    Revoke,
};

// A local, session-scoped simulation authorization. This is deliberately not an
// account or remote identity grant. `authority_id` must match the authority
// declared for the sealed document by the exact pinned record policy.
struct RecordAccessEvent final {
    std::string event_id;
    std::string session_id;
    std::string record_id;
    std::string policy_id;
    std::string sealed_document_id;
    std::string authority_id;
    std::string recorded_at_utc;
    RecordAccessAction action{RecordAccessAction::Grant};
    std::uint64_t sequence{};
    std::string previous_digest;
    std::string event_digest;

    friend bool operator==(const RecordAccessEvent&, const RecordAccessEvent&) = default;
};

enum class RecordDisclosureDeficiencyKind {
    MissingPublicMotion,
    MissingCertificate,
    MissingRedactedCounterpart,
};

struct RecordDisclosureDeficiency final {
    // Public, non-sensitive disclosure identity. This deliberately is not the
    // sealed docket-entry/document identity.
    std::string disclosure_id;
    RecordDisclosureDeficiencyKind kind{};

    friend bool operator==(const RecordDisclosureDeficiency&,
                           const RecordDisclosureDeficiency&) = default;
};

struct RecordAccessRule final {
    std::string sealed_document_id;
    std::string authority_id;
    std::string disclosure_id;
    std::vector<RecordDisclosureDeficiency> blocking_deficiencies;

    friend bool operator==(const RecordAccessRule&, const RecordAccessRule&) = default;
};

struct RecordAccessPolicy final {
    std::string record_id;
    std::string policy_id;
    std::vector<RecordAccessRule> rules;

    friend bool operator==(const RecordAccessPolicy&, const RecordAccessPolicy&) = default;
};

struct RecordAccessDisclosureStatus final {
    std::string disclosure_id;
    std::vector<RecordDisclosureDeficiency> blocking_deficiencies;
    bool authorized{};

    friend bool operator==(const RecordAccessDisclosureStatus&,
                           const RecordAccessDisclosureStatus&) = default;
};

struct RecordAccessProjection final {
    std::string session_id;
    std::string record_id;
    std::string policy_id;
    std::uint64_t through_sequence{};
    std::string head_digest;
    std::vector<std::string> authorized_document_ids;

    friend bool operator==(const RecordAccessProjection&, const RecordAccessProjection&) = default;
};

// A historical replay result intended only for audit/prefix inspection. This
// type is deliberately distinct from the workspace target boundary below, so
// a historical grant cannot be applied as authorization.
class RecordAccessAuditProjection final {
  public:
    [[nodiscard]] const std::vector<std::string>& authorizedDisclosureIds() const noexcept {
        return authorized_disclosure_ids_;
    }

    [[nodiscard]] std::uint64_t throughSequence() const noexcept { return through_sequence_; }

  private:
    friend class appellate::app::RecordAccessSessionController;

    RecordAccessAuditProjection(std::uint64_t through_sequence,
                                std::vector<std::string> authorized_disclosure_ids)
        : through_sequence_(through_sequence),
          authorized_disclosure_ids_(std::move(authorized_disclosure_ids)) {}

    std::uint64_t through_sequence_{};
    std::vector<std::string> authorized_disclosure_ids_;
};

// A sealed application target for live-head authorization replay. Only the
// concrete RecordWorkspace may construct this base, and only the controller
// may invoke it. No authorization projection value crosses the public API.
class RecordAccessProjectionTarget {
  public:
    RecordAccessProjectionTarget(const RecordAccessProjectionTarget&) = delete;
    RecordAccessProjectionTarget& operator=(const RecordAccessProjectionTarget&) = delete;
    RecordAccessProjectionTarget(RecordAccessProjectionTarget&&) = delete;
    RecordAccessProjectionTarget& operator=(RecordAccessProjectionTarget&&) = delete;
    virtual ~RecordAccessProjectionTarget() = default;

  private:
    friend class appellate::app::RecordAccessSessionController;
    friend class appellate::ui::RecordWorkspace;

    RecordAccessProjectionTarget() = default;
    [[nodiscard]] virtual bool applyRecordAccessProjection(RecordAccessProjection projection) = 0;
};

} // namespace appellate::model
