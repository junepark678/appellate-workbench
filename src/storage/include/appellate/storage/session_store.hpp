#pragma once

#include <QByteArray>
#include <QSqlDatabase>
#include <QString>

#include <expected>
#include <memory>
#include <vector>

namespace appellate::storage {

enum class StoreErrorCode {
    InvalidArgument,
    OpenFailed,
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

struct RevisionPin final {
    QString pack_id;
    QString version;
    QString digest;

    friend bool operator==(const RevisionPin&, const RevisionPin&) = default;
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
    qint64 sequence{};
    std::vector<RevisionPin> pins;
    std::vector<StoredCommand> commands;
    std::vector<StoredEvent> events;
    std::vector<DocketEntry> docket;
    std::vector<AssetReference> asset_references{};
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

    [[nodiscard]] std::expected<void, StoreError>
    createSession(const QString& session_id, const QString& engine_revision,
                  const QString& created_at_utc, const std::vector<RevisionPin>& pins);

    [[nodiscard]] std::expected<qint64, StoreError>
    append(const QString& session_id, qint64 expected_sequence, const CommitBatch& batch);

    [[nodiscard]] std::expected<SessionSnapshot, StoreError>
    loadSession(const QString& session_id) const;

    [[nodiscard]] std::expected<void, StoreError> backupTo(const QString& backup_path) const;

    [[nodiscard]] static std::expected<void, StoreError>
    restoreBackup(const QString& backup_path, const QString& destination_path);

    [[nodiscard]] int schemaVersion() const;

  private:
    explicit SessionStore(QString connection_name);

    [[nodiscard]] std::expected<void, StoreError> configure();
    [[nodiscard]] std::expected<void, StoreError> migrate();
    [[nodiscard]] std::expected<void, StoreError> beginImmediate();
    [[nodiscard]] std::expected<void, StoreError> commit();
    void rollback();
    void closeConnection();

    QString connection_name_;
    QSqlDatabase database_;
};

} // namespace appellate::storage
