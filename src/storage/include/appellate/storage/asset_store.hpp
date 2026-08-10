#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QStringView>

#include <expected>

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

class AssetStore final {
  public:
    static constexpr qint64 default_max_asset_bytes = 256LL * 1024LL * 1024LL;

    explicit AssetStore(QString root_directory, qint64 max_asset_bytes = default_max_asset_bytes);

    [[nodiscard]] auto put(QIODevice& source) -> std::expected<StoredAsset, AssetStoreError>;
    [[nodiscard]] auto put(QByteArrayView bytes) -> std::expected<StoredAsset, AssetStoreError>;
    [[nodiscard]] auto read(QStringView sha256) const -> std::expected<QByteArray, AssetStoreError>;

    [[nodiscard]] QString objectsDirectory() const;
    [[nodiscard]] qint64 maxAssetBytes() const noexcept;

  private:
    [[nodiscard]] auto validateConfiguration() const -> std::expected<void, AssetStoreError>;

    QString root_directory_;
    qint64 max_asset_bytes_{};
};

} // namespace appellate::storage
