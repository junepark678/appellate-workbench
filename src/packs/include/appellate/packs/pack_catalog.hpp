#pragma once

#include "appellate/model/pack_id.hpp"
#include "appellate/packs/pack_archive.hpp"

#include <QSqlDatabase>
#include <QString>

#include <expected>
#include <memory>
#include <vector>

namespace appellate::packs {

enum class CatalogErrorCode {
    InvalidConfiguration,
    CannotOpen,
    MigrationFailed,
    ArchiveInvalid,
    ImmutableConflict,
    MissingDependency,
    DependencyCycle,
    CorruptCatalog,
    NotFound,
    QueryFailed,
    CannotStoreArchive,
    CannotStoreBlob,
};

struct CatalogError final {
    CatalogErrorCode code;
    QString message;

    friend bool operator==(const CatalogError&, const CatalogError&) = default;
};

struct InstalledPack final {
    model::PackRevision revision;
    QString archive_sha256;
    QString installed_at_utc;
    std::vector<model::PackDependency> dependencies;

    friend bool operator==(const InstalledPack&, const InstalledPack&) = default;
};

struct MaterializedBlob final {
    model::BlobDescriptor descriptor;
    QString local_path;

    friend bool operator==(const MaterializedBlob&, const MaterializedBlob&) = default;
};

class PackCatalog final {
  public:
    PackCatalog(const PackCatalog&) = delete;
    PackCatalog& operator=(const PackCatalog&) = delete;
    PackCatalog(PackCatalog&&) = delete;
    PackCatalog& operator=(PackCatalog&&) = delete;
    ~PackCatalog();

    [[nodiscard]] static auto open(const QString& root_directory)
        -> std::expected<std::unique_ptr<PackCatalog>, CatalogError>;

    [[nodiscard]] auto installArchive(const QString& archive_path,
                                      const QString& installed_at_utc)
        -> std::expected<InstalledPack, CatalogError>;

    [[nodiscard]] auto load(const model::PackId& id, const std::string& version) const
        -> std::expected<LoadedPack, CatalogError>;

    [[nodiscard]] auto materializeBlob(const model::PackRevision& exact_revision,
                                       const std::string& blob_path) const
        -> std::expected<MaterializedBlob, CatalogError>;

    [[nodiscard]] auto list() const
        -> std::expected<std::vector<InstalledPack>, CatalogError>;

    [[nodiscard]] QString archivesDirectory() const;
    [[nodiscard]] QString blobObjectsDirectory() const;
    [[nodiscard]] int schemaVersion() const;

  private:
    PackCatalog(QString root_directory, QString connection_name);

    [[nodiscard]] auto configure() -> std::expected<void, CatalogError>;
    [[nodiscard]] auto migrate() -> std::expected<void, CatalogError>;
    [[nodiscard]] auto beginImmediate() -> std::expected<void, CatalogError>;
    [[nodiscard]] auto commit() -> std::expected<void, CatalogError>;
    void rollback();
    void closeConnection();

    QString root_directory_;
    QString connection_name_;
    QSqlDatabase database_;
};

} // namespace appellate::packs
