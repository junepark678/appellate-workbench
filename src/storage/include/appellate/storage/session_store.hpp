#pragma once

#include <QByteArray>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

#include <expected>
#include <functional>
#include <memory>
#include <vector>

namespace appellate::storage {

namespace detail {
struct AssetRecoveryHooks;
}

class AssetStore;
class AssetStoreLock;
class StagedAsset;
class SessionArchive;

enum class StoreErrorCode {
    InvalidArgument,
    OpenFailed,
    StateInUse,
    MigrationFailed,
    NotFound,
    AlreadyExists,
    StaleSequence,
    ConstraintViolation,
    QueryFailed,
    BackupFailed,
    RestoreFailed,
};

struct StoreError final {
    StoreErrorCode code;
    QString message;
};

namespace detail {

// Deterministic barrier for proving failure cleanup against namespace replacement. Production
// callers use the hook-free open() overload.
struct SessionStoreOpenHooks final {
    std::function<void(const QString&)> after_private_preflight;
    bool reject_after_private_preflight{};
};

} // namespace detail

struct RevisionPin final {
    QString pack_id;
    QString version;
    QString digest;

    friend bool operator==(const RevisionPin&, const RevisionPin&) = default;
};

enum class SessionAuthorityContract {
    LegacyV1,
    CanonicalV2,
};

struct EventWrite final {
    QString event_type;
    QByteArray payload_json;
    QString authority_id;
};

struct DocketWrite final {
    QString entry_id;
    qsizetype source_event_offset{};
    QString title;
    QString status;
};

struct AssetReference final {
    QString digest;
    QString purpose;

    friend bool operator==(const AssetReference&, const AssetReference&) = default;
};

struct CommitBatch final {
    QString command_id;
    QByteArray command_json;
    QString recorded_at_utc;
    std::vector<EventWrite> events;
    std::vector<DocketWrite> docket_changes;
    std::vector<AssetReference> asset_references{};
};

struct StoredEvent final {
    qint64 sequence{};
    QString event_type;
    QByteArray payload_json;
    QString authority_id;

    friend bool operator==(const StoredEvent&, const StoredEvent&) = default;
};

struct StoredCommand final {
    QString command_id;
    qint64 expected_sequence{};
    QByteArray payload_json;
    QString recorded_at_utc;

    friend bool operator==(const StoredCommand&, const StoredCommand&) = default;
};

struct DocketEntry final {
    QString entry_id;
    qint64 event_sequence{};
    QString title;
    QString status;

    friend bool operator==(const DocketEntry&, const DocketEntry&) = default;
};

struct SessionSnapshot final {
    QString session_id;
    QString engine_revision;
    SessionAuthorityContract authority_contract{SessionAuthorityContract::LegacyV1};
    qint64 sequence{};
    std::vector<RevisionPin> pins;
    std::vector<StoredCommand> commands;
    std::vector<StoredEvent> events;
    std::vector<DocketEntry> docket;
    std::vector<AssetReference> asset_references{};
    QString created_at_utc{};
};

class SessionStore final {
  public:
    SessionStore(const SessionStore&) = delete;
    SessionStore& operator=(const SessionStore&) = delete;
    SessionStore(SessionStore&&) = delete;
    SessionStore& operator=(SessionStore&&) = delete;
    ~SessionStore();

    [[nodiscard]] static std::expected<std::unique_ptr<SessionStore>, StoreError>
    open(const QString& database_path);

    [[nodiscard]] static std::expected<std::unique_ptr<SessionStore>, StoreError>
    open(const QString& database_path, const detail::SessionStoreOpenHooks& hooks);

    // Creates an intentional in-process child connection under this owner's retained validated
    // lifetime lease. Public open() remains exclusive; child connections cannot fork again.
    [[nodiscard]] std::expected<std::unique_ptr<SessionStore>, StoreError>
    forkConnection() const;

    [[nodiscard]] std::expected<void, StoreError>
    createSession(const QString& session_id, const QString& engine_revision,
                  const QString& created_at_utc, const std::vector<RevisionPin>& pins,
                  SessionAuthorityContract authority_contract = SessionAuthorityContract::LegacyV1);

    // Creates the session and its required first command/event batch in one SQLite transaction.
    // No zero-event session is observable if validation, insertion, or commit fails.
    [[nodiscard]] std::expected<qint64, StoreError> createSessionWithInitialBatch(
        const QString& session_id, const QString& engine_revision,
        const QString& created_at_utc, const std::vector<RevisionPin>& pins,
        SessionAuthorityContract authority_contract, const CommitBatch& initial_batch);

    [[nodiscard]] std::expected<qint64, StoreError>
    append(const QString& session_id, qint64 expected_sequence, const CommitBatch& batch);

    [[nodiscard]] std::expected<qint64, StoreError>
    appendWithStagedAsset(const QString& session_id, qint64 expected_sequence,
                          const CommitBatch& batch, AssetStore& asset_store,
                          StagedAsset& staged_asset);

    // With both the SQLite write reservation and the CAS lock held, quarantines abandoned staging
    // names and immutable objects that have no reference from any persisted session. Missing or
    // corrupt referenced objects fail closed. Physical quarantine reclamation is deliberately
    // outside this transaction.
    [[nodiscard]] std::expected<void, StoreError> recoverAssetStore(AssetStore& asset_store);

    [[nodiscard]] std::expected<void, StoreError>
    recoverAssetStore(AssetStore& asset_store, const detail::AssetRecoveryHooks& hooks);

    [[nodiscard]] std::expected<SessionSnapshot, StoreError>
    loadSession(const QString& session_id) const;

    [[nodiscard]] std::expected<void, StoreError> backupTo(const QString& backup_path) const;

    [[nodiscard]] static std::expected<void, StoreError>
    restoreBackup(const QString& backup_path, const QString& destination_path);

    [[nodiscard]] int schemaVersion() const;

  private:
    friend class SessionArchive;
    explicit SessionStore(QString connection_name);

    [[nodiscard]] std::expected<void, StoreError> configure();
    [[nodiscard]] std::expected<void, StoreError> migrate();
    [[nodiscard]] std::expected<void, StoreError> validateActiveLease() const;
    [[nodiscard]] std::expected<void, StoreError> beginImmediate();
    [[nodiscard]] std::expected<QString, StoreError> assetStoreIdentity() const;
    [[nodiscard]] std::expected<void, StoreError>
    ensureAssetStoreIdentity(AssetStore& asset_store, const AssetStoreLock& lock,
                             const QStringList& referenced_digests,
                             bool require_exact_object_set);
    [[nodiscard]] std::expected<void, StoreError> commit();
    void rollback();
    void closeConnection();

    QString connection_name_;
    QString database_path_;
    QSqlDatabase database_;
    std::shared_ptr<void> lifetime_lease_;
    bool may_fork_{};
};

} // namespace appellate::storage
