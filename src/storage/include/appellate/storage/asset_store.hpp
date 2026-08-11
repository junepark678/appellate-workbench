#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <expected>
#include <memory>
#include <optional>

class QIODevice;

namespace appellate::storage {

enum class AssetStoreErrorCode {
    InvalidConfiguration,
    InvalidDigest,
    AssetTooLarge,
    CannotCreateDirectory,
    CannotRead,
    CannotWrite,
    CannotSync,
    CannotCommit,
    NotFound,
    CorruptObject,
};

struct AssetStoreError final {
    AssetStoreErrorCode code;
    QString message;

    friend bool operator==(const AssetStoreError&, const AssetStoreError&) = default;
};

struct StoredAsset final {
    QString sha256;
    qint64 size{};
    bool deduplicated{};

    friend bool operator==(const StoredAsset&, const StoredAsset&) = default;
};

class AssetStoreLock final {
  public:
    AssetStoreLock(const AssetStoreLock&) = delete;
    AssetStoreLock& operator=(const AssetStoreLock&) = delete;
    AssetStoreLock(AssetStoreLock&& other) noexcept;
    AssetStoreLock& operator=(AssetStoreLock&& other) noexcept;
    ~AssetStoreLock();

  private:
    friend class AssetStore;
    friend class SessionStore;
    explicit AssetStoreLock(int descriptor);
    void reset() noexcept;
    int descriptor_{-1};
};

class StagedAsset final {
  public:
    StagedAsset(const StagedAsset&) = delete;
    StagedAsset& operator=(const StagedAsset&) = delete;
    StagedAsset(StagedAsset&& other) noexcept;
    StagedAsset& operator=(StagedAsset&& other) noexcept;
    ~StagedAsset();

    [[nodiscard]] const QString& sha256() const noexcept;
    [[nodiscard]] qint64 size() const noexcept;
    [[nodiscard]] bool wasDeduplicated() const noexcept;

  private:
    friend class AssetStore;
    friend class SessionStore;
    StagedAsset(int objects_descriptor, int file_descriptor, QByteArray temporary_name,
                QString sha256, qint64 size);
    void reset() noexcept;

    int objects_descriptor_{-1};
    int file_descriptor_{-1};
    QByteArray temporary_name_;
    QString sha256_;
    qint64 size_{};
    bool finalized_{};
    bool newly_finalized_{};
};

class AssetStore final {
  public:
    static constexpr qint64 default_max_asset_bytes = 256LL * 1024LL * 1024LL;

    explicit AssetStore(QString root_directory, qint64 max_asset_bytes = default_max_asset_bytes);
    AssetStore(const AssetStore&) = delete;
    AssetStore& operator=(const AssetStore&) = delete;
    AssetStore(AssetStore&& other) noexcept;
    AssetStore& operator=(AssetStore&& other) noexcept;
    ~AssetStore();

    [[nodiscard]] auto put(QIODevice& source) -> std::expected<StoredAsset, AssetStoreError>;
    [[nodiscard]] auto put(QByteArrayView bytes) -> std::expected<StoredAsset, AssetStoreError>;
    [[nodiscard]] auto read(QStringView sha256) const -> std::expected<QByteArray, AssetStoreError>;

    // Transactional callers stage verified bytes first, take the cross-process CAS lock, then
    // publish the immutable object immediately before their database commit.
    [[nodiscard]] auto stage(QIODevice& source) -> std::expected<StagedAsset, AssetStoreError>;
    [[nodiscard]] auto stage(QByteArrayView bytes) -> std::expected<StagedAsset, AssetStoreError>;
    [[nodiscard]] auto acquireLock() const -> std::expected<AssetStoreLock, AssetStoreError>;
    [[nodiscard]] auto finalize(StagedAsset& staged, const AssetStoreLock& lock) const
        -> std::expected<StoredAsset, AssetStoreError>;

    [[nodiscard]] QString objectsDirectory() const;
    [[nodiscard]] qint64 maxAssetBytes() const noexcept;

  private:
    friend class SessionStore;
    [[nodiscard]] auto validateConfiguration() const -> std::expected<void, AssetStoreError>;
    [[nodiscard]] auto ensureReady() const -> std::expected<void, AssetStoreError>;
    [[nodiscard]] auto removeNewlyFinalized(StagedAsset& staged,
                                            const AssetStoreLock& lock) const
        -> std::expected<void, AssetStoreError>;
    [[nodiscard]] auto hasPublishedLock() const
        -> std::expected<bool, AssetStoreError>;
    [[nodiscard]] auto recoverPairedObjects(QStringView database_identity,
                                            const AssetStoreLock& lock,
                                            const QStringList& referenced_digests) const
        -> std::expected<void, AssetStoreError>;
    [[nodiscard]] auto identity(const AssetStoreLock& lock) const
        -> std::expected<std::optional<QString>, AssetStoreError>;
    [[nodiscard]] auto writeIdentity(QStringView identity, const AssetStoreLock& lock) const
        -> std::expected<void, AssetStoreError>;
    [[nodiscard]] auto objectDigests(const AssetStoreLock& lock) const
        -> std::expected<QStringList, AssetStoreError>;
    [[nodiscard]] auto preflightPair(QStringView database_identity,
                                     const QStringList& referenced_digests,
                                     bool require_exact_object_set) const
        -> std::expected<void, AssetStoreError>;
    [[nodiscard]] auto identityUnlocked() const
        -> std::expected<std::optional<QString>, AssetStoreError>;
    [[nodiscard]] auto objectDigestsUnlocked() const
        -> std::expected<QStringList, AssetStoreError>;
    void closeDescriptors() noexcept;

    QString root_directory_;
    qint64 max_asset_bytes_{};
    mutable int root_descriptor_{-1};
    mutable int objects_descriptor_{-1};
};

} // namespace appellate::storage
