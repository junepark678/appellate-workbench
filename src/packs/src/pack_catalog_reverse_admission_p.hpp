#pragma once

#include "pack_catalog_fingerprint_p.hpp"

#include "appellate/packs/error.hpp"
#include "appellate/packs/pack_reader.hpp"

#include <QSqlDatabase>
#include <QString>

#include <cstdint>
#include <expected>
#include <functional>
#include <set>

namespace appellate::packs::detail {

// The archive digest is the exact lowercase digest component with the `.awpack` suffix removed.
// The callback must load from an already retained/private copy rather than reopening the source
// catalog path.
using SecureCatalogArchiveLoader =
    std::function<std::expected<LoadedPack, Error>(const QString& archive_digest)>;

// The callback verifies one already retained physical CAS object against the supplied digest and
// byte size. It is invoked at most once for each present blob digest.
using SecureCatalogBlobVerifier = std::function<std::expected<void, CatalogError>(
    const QString& blob_digest, std::uint64_t expected_byte_size)>;

// `present_archive_digests` and `present_blob_digests` are exact, sorted namespace inventories
// whose entries contain only the digest component. Missing referenced blob objects are lazy-valid;
// every present blob object must be referenced, internally consistent, and verified by the
// callback.
[[nodiscard]] auto validateCatalogReverseAdmission(
    QSqlDatabase& private_database, CatalogSchemaGeneration generation,
    const std::set<QString>& present_archive_digests, const std::set<QString>& present_blob_digests,
    const SecureCatalogArchiveLoader& archive_loader,
    const SecureCatalogBlobVerifier& blob_verifier, const CatalogHooks& hooks,
    CatalogOperation operation) -> std::expected<void, CatalogError>;

} // namespace appellate::packs::detail
