#pragma once

#include "appellate/packs/pack_catalog.hpp"
#include "pack_archive_p.hpp"

#include <QByteArray>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace appellate::packs::detail {

// Private catalog lifecycle vocabulary. Keeping the event, subject, and injected result orthogonal
// lets tests cover binding, SQLite, lock, and reconciliation branches without a production global
// switch or a second filesystem implementation.
enum class CatalogEvent {
    ScratchContextAccepted,
    OperandValidated,
    CurrentDirectoryCaptured,
    ControllerOpened,
    AclProbed,
    NameRebound,
    RootRetained,
    RootLockAttempted,
    RootLockAcquired,
    RootLockReleased,
    NamespaceInventoried,
    IdentityRetained,
    DirectoryCreated,
    DirectoryNormalized,
    FileCreated,
    FileNormalized,
    FileRead,
    FileWritten,
    FileHashed,
    FileSynced,
    DirectorySynced,
    LegacyLockInspected,
    LockConstructed,
    LockStaleTimeSet,
    LockTried,
    LockDestroyed,
    CapturePreflight,
    CapturePassStarted,
    CaptureFileCopied,
    CapturePassFinished,
    CaptureBarrier,
    DatabaseAdded,
    DatabaseOpened,
    DatabaseConfigured,
    SqlAllocationPreflight,
    QueryPlanVerified,
    SchemaFingerprintCompared,
    IntegrityChecked,
    ForeignKeysChecked,
    MigrationStarted,
    MigrationCommitAttempted,
    MigrationCommitted,
    ArchiveCopied,
    ArchiveImported,
    ArchiveValidated,
    BlobValidated,
    SidecarsInventoried,
    TransactionBegun,
    // FailBefore is the install pre-COMMIT seam after final resolved-graph validation.
    TransactionCommitAttempted,
    TransactionRolledBack,
    TransactionCommitted,
    CheckpointExecuted,
    ArchiveStaged,
    ArchivePublished,
    BlobPublished,
    BlobMaterialized,
    QueriesFinished,
    DatabaseClosed,
    DatabaseReset,
    DatabaseRemoved,
    CleanupInspected,
    CleanupRemoved,
    CleanupSynced,
    Reconciled,
    ForcedTeardown,
};

enum class CatalogSubject {
    None,
    CatalogOperand,
    CurrentDirectory,
    ExternalController,
    CatalogRoot,
    ArchivesDirectory,
    BlobsDirectory,
    DatabaseMain,
    DatabaseWal,
    DatabaseShm,
    RollbackJournal,
    InstallLock,
    InstallRemoveLock,
    ArchiveObject,
    BlobObject,
    ArchiveStaging,
    BlobStaging,
    PassAWorkspace,
    PassBWorkspace,
    Version1Reference,
    CurrentReference,
    ArchiveWorkspace,
    LiveSnapshotWorkspace,
    PrivateDatabaseMain,
    PrivateDatabaseWal,
    PrivateDatabaseShm,
    SQLiteConnection,
};

enum class CatalogOperation {
    None,
    SnapshotOpen,
    WritableOpen,
    List,
    Load,
    LoadResolved,
    InstallArchive,
    MaterializeBlob,
    MaterializeResolvedBlob,
    SnapshotDestruction,
    WritableDestruction,
};

enum class CatalogAclKind {
    None,
    Access,
    Default,
};

enum class CatalogLockMode {
    None,
    Shared,
    Exclusive,
};

enum class CatalogInjectedAction {
    Continue,
    FailBefore,
    FailAfter,
};

enum class CatalogNodeType {
    Missing,
    Directory,
    RegularFile,
    Other,
};

enum class CatalogNamespaceShape {
    Unknown,
    Empty,
    ClosedVersion1,
    Current,
};

enum class CatalogCleanupOutcome {
    NotAttempted,
    Removed,
    Preserved,
};

struct CatalogIdentity final {
    std::uint64_t device{};
    std::uint64_t inode{};
    CatalogNodeType type{CatalogNodeType::Missing};
    std::uint64_t link_count{};
    std::uint64_t owner{};
    unsigned int mode{};
    std::uint64_t byte_size{};
    std::int64_t modification_seconds{};
    std::int64_t modification_nanoseconds{};
    std::int64_t change_seconds{};
    std::int64_t change_nanoseconds{};

    friend bool operator==(const CatalogIdentity&, const CatalogIdentity&) = default;
};

struct CatalogObservation final {
    CatalogEvent event{};
    CatalogSubject subject{};
    CatalogOperation operation{};
    CatalogAclKind acl_kind{};
    CatalogLockMode lock_mode{};
    QString absolute_path;
    QByteArray component;
    std::size_t ordinal{};
    std::size_t pass{};
    std::uint64_t byte_count{};
    unsigned int mode_before{};
    unsigned int mode_after{};
    std::optional<CatalogIdentity> identity_before;
    std::optional<CatalogIdentity> identity_after;
};

// When supplied, this replaces the real result at a seam. native_error represents errno-style
// outcomes, library_error represents APIs such as QLockFile, result_row represents SQLite scalar
// rows (including checkpoint results), and raw_names supports deterministic inventory boundaries.
// state_change_applied distinguishes an ordinary failure from an applied-then-error result.
struct CatalogInjectedOutcome final {
    bool operation_succeeded{};
    bool state_change_applied{};
    int native_error{};
    int library_error{};
    std::optional<CatalogIdentity> identity;
    std::optional<std::uint64_t> byte_count;
    std::vector<std::int64_t> result_row;
    std::vector<QByteArray> raw_names;
};

struct CatalogAllocationCounters final {
    std::size_t text_conversions{};
    std::size_t row_container_growth{};
    std::size_t archive_copy_allocations{};
    std::uint64_t copied_bytes{};
};

struct CatalogReport final {
    QString immutable_root_path;
    QString live_snapshot_workspace;
    CatalogNamespaceShape admitted_shape{CatalogNamespaceShape::Unknown};
    CatalogCleanupOutcome cleanup{CatalogCleanupOutcome::NotAttempted};
    std::optional<CatalogErrorCode> final_error;
    CatalogAllocationCounters allocations;
    std::vector<CatalogObservation> observations;
    std::vector<QString> remaining_ledger_paths;
    std::vector<QByteArray> unexpected_raw_names;
    std::size_t scratch_acquisitions{};
    std::size_t lock_attempts{};
    bool residue_identity_ambiguous{};
    bool forced_teardown{};
};

struct CatalogHooks final {
    std::function<QByteArray(std::size_t attempt)> name_source;
    std::function<CatalogInjectedAction(const CatalogObservation&)> inject;
    std::function<std::optional<CatalogInjectedOutcome>(const CatalogObservation&)> outcome;
    std::function<void(const CatalogObservation&)> barrier;
    std::function<void(const CatalogObservation&)> observe;
    CatalogReport* report{};
};

struct PackCatalogSnapshotFactory final {
    [[nodiscard]] static auto openExisting(const QString& root_directory,
                                           SecureScratchContext&& scratch_context)
        -> std::expected<std::unique_ptr<PackCatalogSnapshot>, CatalogError>;

    [[nodiscard]] static auto openExisting(const QString& root_directory,
                                           SecureScratchContext&& scratch_context,
                                           CatalogHooks hooks)
        -> std::expected<std::unique_ptr<PackCatalogSnapshot>, CatalogError>;
};

struct PackCatalogFactory final {
    [[nodiscard]] static auto open(const QString& root_directory,
                                   SecureScratchContext&& scratch_context, CatalogHooks hooks)
        -> std::expected<std::unique_ptr<PackCatalog>, CatalogError>;
};

} // namespace appellate::packs::detail
