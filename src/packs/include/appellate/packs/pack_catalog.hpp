#pragma once

#include "appellate/model/pack_id.hpp"
#include "appellate/packs/pack_archive.hpp"
#include "appellate/packs/realism_evidence_authoring.hpp"
#include "appellate/packs/resolved_pack.hpp"

#include <QSqlDatabase>
#include <QString>

#include <expected>
#include <memory>
#include <vector>

namespace appellate::packs {

namespace detail {
enum class CatalogOperation;
struct CatalogHooks;
struct PackCatalogFactory;
struct PackCatalogSnapshotFactory;
} // namespace detail

enum class CatalogErrorCode {
    InvalidConfiguration,
    CannotOpen,
    CatalogBusy,
    UninitializedCatalog,
    MigrationFailed,
    ArchiveInvalid,
    ImmutableConflict,
    MissingDependency,
    DependencyCycle,
    DependencyVersionSplit,
    DependencyClosureTooLarge,
    UnsupportedCapability,
    ResourceCollision,
    InvalidResolvedGraph,
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

class PackCatalogSnapshot final {
  public:
    PackCatalogSnapshot(const PackCatalogSnapshot&) = delete;
    PackCatalogSnapshot& operator=(const PackCatalogSnapshot&) = delete;
    PackCatalogSnapshot(PackCatalogSnapshot&&) = delete;
    PackCatalogSnapshot& operator=(PackCatalogSnapshot&&) = delete;
    ~PackCatalogSnapshot();

    [[nodiscard]] static auto openExisting(const QString& root_directory)
        -> std::expected<std::unique_ptr<PackCatalogSnapshot>, CatalogError>;

    [[nodiscard]] auto list() const -> std::expected<std::vector<InstalledPack>, CatalogError>;

    [[nodiscard]] auto load(const model::PackId& id, const std::string& version) const
        -> std::expected<LoadedPack, CatalogError>;

    [[nodiscard]] auto loadResolved(const model::PackRevision& exact_root) const
        -> std::expected<ResolvedPack, CatalogError>;

  private:
    friend struct detail::PackCatalogSnapshotFactory;
    struct Impl;
    explicit PackCatalogSnapshot(std::unique_ptr<Impl> state);
    std::unique_ptr<Impl> impl_;
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

    [[nodiscard]] auto installArchive(const QString& archive_path, const QString& installed_at_utc)
        -> std::expected<InstalledPack, CatalogError>;

    [[nodiscard]] auto load(const model::PackId& id, const std::string& version) const
        -> std::expected<LoadedPack, CatalogError>;

    [[nodiscard]] auto loadResolved(const model::PackRevision& exact_root) const
        -> std::expected<ResolvedPack, CatalogError>;

    [[nodiscard]] auto materializeBlob(const model::PackRevision& exact_revision,
                                       const std::string& blob_path) const
        -> std::expected<MaterializedBlob, CatalogError>;

    [[nodiscard]] auto materializeBlob(const ResolvedPack& closure,
                                       const model::PackRevision& owning_revision,
                                       const std::string& blob_path) const
        -> std::expected<MaterializedBlob, CatalogError>;

    [[nodiscard]] auto list() const -> std::expected<std::vector<InstalledPack>, CatalogError>;

    [[nodiscard]] QString archivesDirectory() const;
    [[nodiscard]] QString blobObjectsDirectory() const;
    [[nodiscard]] QString rootDirectory() const;
    [[nodiscard]] int schemaVersion() const;

  private:
    friend struct detail::PackCatalogFactory;
    friend struct detail::PackCatalogSnapshotFactory;
    friend auto authorRealismEvidence(const PackCatalog& catalog,
                                      const RealismEvidenceAuthoringInput& input)
        -> std::expected<AuthoredRealismEvidence, RealismEvidenceAuthoringError>;
    friend auto authorRealismEvidence(const PackCatalog& catalog,
                                      const RealismEvidenceTraceSetAuthoringInput& input)
        -> std::expected<AuthoredRealismEvidence, RealismEvidenceAuthoringError>;

    struct Impl;
    PackCatalog(QString root_directory, QString connection_name);
    PackCatalog(QString root_directory, QString connection_name, std::unique_ptr<Impl> state);

    [[nodiscard]] auto configure() -> std::expected<void, CatalogError>;
    [[nodiscard]] auto migrate(const std::vector<LoadedPack>* admitted_archives,
                               const detail::CatalogHooks& hooks, bool* commit_applied,
                               bool* commit_ambiguous) -> std::expected<void, CatalogError>;
    [[nodiscard]] auto beginImmediate() -> std::expected<void, CatalogError>;
    [[nodiscard]] auto commit(bool* commit_applied, bool* commit_ambiguous)
        -> std::expected<void, CatalogError>;
    [[nodiscard]] auto loadExactRevision(const model::PackRevision& exact_revision,
                                         CatalogErrorCode missing_code,
                                         detail::CatalogOperation catalog_operation) const
        -> std::expected<LoadedPack, CatalogError>;
    [[nodiscard]] auto resolveClosure(const model::PackRevision& exact_root,
                                      const LoadedPack* staged_root) const
        -> std::expected<ResolvedPack, CatalogError>;
    [[nodiscard]] auto resolveClosure(const model::PackRevision& exact_root,
                                      const LoadedPack* staged_root,
                                      detail::CatalogOperation catalog_operation) const
        -> std::expected<ResolvedPack, CatalogError>;
    void rollback();
    void closeConnection();

    QString root_directory_;
    QString connection_name_;
    QSqlDatabase database_;
    std::unique_ptr<Impl> impl_;
};

} // namespace appellate::packs
