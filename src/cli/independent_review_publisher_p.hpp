#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace appellate::cli::detail {

enum class IndependentReviewArtifactKind {
    PreparedHandoff,
    FinalizedPack,
};

enum class IndependentReviewPublicationErrorCode {
    InvalidArguments,
    DestinationExists,
    DestinationOverlapsProtectedInput,
    UnsafeDestinationParent,
    CannotPublishDestination,
    PublicationCleanupFailed,
    PublicationIdentityFailed,
    PublicationOutcomeUncertain,
    PublicationDurabilityFailed,
    UnsupportedAuthoringPlatform,
    InvalidStagedArtifact,
};

struct IndependentReviewPublicationError final {
    IndependentReviewPublicationErrorCode code{};
    QString message;
};

struct IndependentReviewPublicationMember final {
    QString relative_path;
    QByteArray bytes;
};

struct IndependentReviewProtectedDirectory final {
    std::uint64_t device{};
    std::uint64_t inode{};

    friend bool operator==(const IndependentReviewProtectedDirectory&,
                           const IndependentReviewProtectedDirectory&) = default;
};

// A lexical command-line operand captured before any environmental observation.  Its native bytes
// are the single authoritative encoding used by the later retained-descriptor walk.
struct IndependentReviewPathToken final {
    QString supplied_path;
    QStringList supplied_components;
    std::vector<QByteArray> supplied_native_components;
    QByteArray supplied_native_path;
    bool destination_leaf{};
};

enum class IndependentReviewStagedValidationErrorCode {
    InvalidArtifact,
    PublicationMismatch,
};

struct IndependentReviewStagedValidationError final {
    QString message;
    IndependentReviewStagedValidationErrorCode code{
        IndependentReviewStagedValidationErrorCode::InvalidArtifact};
};

using IndependentReviewStagedValidator =
    std::function<std::expected<void, IndependentReviewStagedValidationError>(const QString&)>;

enum class IndependentReviewPublisherEvent {
    OperandValidated,
    CurrentDirectoryCaptured,
    ControllerOpened,
    ControllerRebound,
    AccessAclProbe,
    DefaultAclProbe,
    ProtectedInventory,
    ProtectedOverlapChecked,
    ParentLeaseAttempted,
    ParentLeaseAcquired,
    ProcfsPreflight,
    ModeNormalizePreflight,
    PlanDeclared,
    NameCandidate,
    StagingCreateAttempted,
    StagingRetainAttempted,
    StagingCreated,
    DirectoryCreateAttempted,
    DirectoryRetainAttempted,
    DirectoryCreated,
    FileCreateAttempted,
    FileCreated,
    ModeNormalizeAttempted,
    ModeNormalized,
    DirectoryUsableRetainAttempted,
    EntryRebound,
    FileWrite,
    FileSync,
    DirectorySync,
    InventoryChecked,
    BytesChecked,
    BeforeStagedValidation,
    AfterStagedValidation,
    BeforeFinalTreeBinding,
    BeforeRename,
    RenameAttempted,
    RenameReturned,
    Reconciled,
    ParentSync,
    CleanupInspected,
    CleanupRemoved,
    CleanupSynced,
};

enum class IndependentReviewPublisherInjectedAction {
    Continue,
    FailBefore,
    FailAfter,
};

struct IndependentReviewPublisherObservation final {
    IndependentReviewPublisherEvent event{};
    QString absolute_path;
    QByteArray component;
    std::size_t ordinal{};
};

struct IndependentReviewPublisherRetainedCreation final {
    std::size_t ordinal{};
    int descriptor{-1};
    std::uint64_t device{};
    std::uint64_t inode{};
};

enum class IndependentReviewPublisherSyntheticNodeType {
    Directory,
    RegularFile,
    Other,
};

struct IndependentReviewPublisherSyntheticStat final {
    std::optional<std::uint64_t> device;
    std::optional<std::uint64_t> inode;
    std::optional<IndependentReviewPublisherSyntheticNodeType> type;
    std::optional<std::uint64_t> owner;
    std::optional<std::uint32_t> mode;
    std::optional<std::uint64_t> link_count;
};

struct IndependentReviewPublisherInjectedOutcome final {
    IndependentReviewPublisherInjectedOutcome() = default;
    IndependentReviewPublisherInjectedOutcome(
        bool succeeded, bool applied, int error,
        std::optional<IndependentReviewPublisherSyntheticStat> retained = std::nullopt,
        std::optional<IndependentReviewPublisherSyntheticStat> named = std::nullopt)
        : operation_succeeded(succeeded), state_change_applied(applied), native_error(error),
          retained_stat(std::move(retained)), named_stat(std::move(named)) {}

    bool operation_succeeded{};
    bool state_change_applied{};
    int native_error{};
    std::optional<IndependentReviewPublisherSyntheticStat> retained_stat;
    std::optional<IndependentReviewPublisherSyntheticStat> named_stat;
    std::optional<std::size_t> maximum_write_bytes;
};

struct IndependentReviewPublisherReport final {
    QString destination_path;
    QString staging_path;
    QStringList remaining_ledger_paths;
    std::vector<IndependentReviewPublisherObservation> observations;
    std::vector<IndependentReviewPublisherRetainedCreation> retained_creations;
    bool retained_creations_verified_at_completion{};
};

struct IndependentReviewPublisherHooks final {
    std::function<QByteArray(std::size_t attempt)> staging_suffix_source;
    std::function<IndependentReviewPublisherInjectedAction(
        const IndependentReviewPublisherObservation&)>
        inject;
    std::function<std::optional<IndependentReviewPublisherInjectedOutcome>(
        const IndependentReviewPublisherObservation&)>
        outcome;
    std::function<void(const IndependentReviewPublisherObservation&)> barrier;
    std::function<void(const IndependentReviewPublisherObservation&)> observe;
    IndependentReviewPublisherReport* report{};
    std::optional<qsizetype> injected_final_review_byte_limit;
};

struct IndependentReviewPublicationRequest final {
    IndependentReviewPublicationRequest() = default;
    IndependentReviewPublicationRequest(
        IndependentReviewArtifactKind artifact_kind, QString destination,
        std::vector<IndependentReviewProtectedDirectory> protected_inputs,
        std::size_t protected_inputs_entry_count, QStringList protected_paths,
        std::vector<IndependentReviewPublicationMember> publication_members,
        IndependentReviewStagedValidator staged_validator,
        std::optional<IndependentReviewPathToken> encoded_destination = std::nullopt)
        : kind(artifact_kind), destination_path(std::move(destination)),
          protected_directories(std::move(protected_inputs)),
          protected_entry_count(protected_inputs_entry_count),
          protected_directory_paths(std::move(protected_paths)),
          members(std::move(publication_members)), validate_staged(std::move(staged_validator)),
          destination_token(std::move(encoded_destination)) {}

    IndependentReviewArtifactKind kind{};
    QString destination_path;
    std::vector<IndependentReviewProtectedDirectory> protected_directories;
    std::size_t protected_entry_count{};
    QStringList protected_directory_paths;
    std::vector<IndependentReviewPublicationMember> members;
    IndependentReviewStagedValidator validate_staged;
    std::optional<IndependentReviewPathToken> destination_token;
};

struct IndependentReviewHandoffInput final {
    QByteArray handoff_bytes;
    QByteArray declaration_template_bytes;
    IndependentReviewProtectedDirectory protected_directory;
    std::size_t protected_entry_count{};
};

struct IndependentReviewInputReaderHooks final {
    std::function<void()> before_final_rebind;
};

[[nodiscard]] auto validateIndependentReviewPathSpelling(const QString& path, bool destination_leaf)
    -> std::expected<void, IndependentReviewPublicationError>;

[[nodiscard]] auto encodeIndependentReviewPathSpelling(const QString& path, bool destination_leaf)
    -> std::expected<IndependentReviewPathToken, IndependentReviewPublicationError>;

[[nodiscard]] auto validateIndependentReviewDestinationPath(const QString& path,
                                                            IndependentReviewArtifactKind kind)
    -> std::expected<void, IndependentReviewPublicationError>;

[[nodiscard]] auto encodeIndependentReviewDestinationPath(const QString& path,
                                                          IndependentReviewArtifactKind kind)
    -> std::expected<IndependentReviewPathToken, IndependentReviewPublicationError>;

[[nodiscard]] auto validateIndependentReviewHandoffPath(const QString& path)
    -> std::expected<void, IndependentReviewPublicationError>;

[[nodiscard]] auto encodeIndependentReviewHandoffPath(const QString& path)
    -> std::expected<IndependentReviewPathToken, IndependentReviewPublicationError>;

[[nodiscard]] auto
readIndependentReviewHandoffDirectory(const QString& directory_path,
                                      const IndependentReviewInputReaderHooks& hooks = {})
    -> std::expected<IndependentReviewHandoffInput, IndependentReviewPublicationError>;

[[nodiscard]] auto
readIndependentReviewHandoffDirectory(IndependentReviewPathToken directory_path,
                                      const IndependentReviewInputReaderHooks& hooks = {})
    -> std::expected<IndependentReviewHandoffInput, IndependentReviewPublicationError>;

[[nodiscard]] auto
readIndependentReviewStagedHandoffDirectory(const QString& retained_parent_proc_root,
                                            const IndependentReviewInputReaderHooks& hooks = {})
    -> std::expected<IndependentReviewHandoffInput, IndependentReviewPublicationError>;

[[nodiscard]] auto
readIndependentReviewDeclaration(const QString& declaration_path,
                                 const IndependentReviewInputReaderHooks& hooks = {})
    -> std::expected<QByteArray, IndependentReviewPublicationError>;

[[nodiscard]] auto
readIndependentReviewDeclaration(IndependentReviewPathToken declaration_path,
                                 const IndependentReviewInputReaderHooks& hooks = {})
    -> std::expected<QByteArray, IndependentReviewPublicationError>;

[[nodiscard]] auto
publishIndependentReviewArtifacts(const IndependentReviewPublicationRequest& request,
                                  const IndependentReviewPublisherHooks& hooks = {})
    -> std::expected<void, IndependentReviewPublicationError>;

} // namespace appellate::cli::detail
