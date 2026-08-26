#pragma once

#include "pack_catalog_p.hpp"

#include <QString>

#include <expected>
#include <memory>

namespace appellate::packs::detail {

// Attempt-owned adapter for the public QLockFile used at <catalog>/.install.lock. The caller keeps
// the borrowed root descriptor alive, holds the catalog root's exclusive flock, and has already
// proved that the immutable absolute spelling names that exact current-euid mode-0700 root and that
// the lock name is absent. This type owns only the QLockFile attempt and retained lock-file fds.
class CatalogInstallLock final {
  public:
    CatalogInstallLock(const CatalogInstallLock&) = delete;
    CatalogInstallLock& operator=(const CatalogInstallLock&) = delete;
    CatalogInstallLock(CatalogInstallLock&&) noexcept;
    CatalogInstallLock& operator=(CatalogInstallLock&&) noexcept;
    ~CatalogInstallLock();

    [[nodiscard]] bool isHeld() const noexcept;
    [[nodiscard]] const CatalogIdentity& identity() const noexcept;
    [[nodiscard]] const QString& absolutePath() const noexcept;

    // Revalidates the exact attempt-owned lock/root binding without destroying or removing it.
    // Call this before any failure cleanup whose authority depends on the lock still being ours.
    [[nodiscard]] auto revalidate() -> std::expected<void, CatalogError>;

    // Performs checked QLockFile destruction, descriptor-relative reinventory, root fsync, and
    // immutable-path/root rebind. A later call after any completed teardown is a no-op.
    [[nodiscard]] auto release() -> std::expected<void, CatalogError>;

  private:
    friend auto acquireCatalogInstallLock(const QString&, int, CatalogOperation, CatalogHooks,
                                          CatalogErrorCode)
        -> std::expected<CatalogInstallLock, CatalogError>;

    struct Impl;
    explicit CatalogInstallLock(std::unique_ptr<Impl> state);
    std::unique_ptr<Impl> impl_;
};

// Acquires from an initially absent name. operational_failure_code must be CannotOpen,
// CannotStoreArchive, or CannotStoreBlob and is used for non-contention, exactly reconciled
// failures. A structurally safe lock won by a racing legacy/peer QLockFile is CatalogBusy; unsafe,
// rebound, or .install.lock.rmlock state is CorruptCatalog. At most one fresh-helper retry is
// performed.
[[nodiscard]] auto acquireCatalogInstallLock(const QString& immutable_absolute_root,
                                             int borrowed_root_descriptor,
                                             CatalogOperation operation, CatalogHooks hooks,
                                             CatalogErrorCode operational_failure_code)
    -> std::expected<CatalogInstallLock, CatalogError>;

} // namespace appellate::packs::detail
