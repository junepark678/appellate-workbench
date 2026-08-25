#pragma once

#include "pack_catalog_p.hpp"

#include <QSqlDatabase>

#include <expected>

namespace appellate::packs::detail {

enum class CatalogSchemaGeneration {
    Version1 = 1,
    Current = 2,
};

struct CatalogSqliteConfiguration final {
    bool foreign_keys{true};
    bool query_only{true};
    int busy_timeout_milliseconds{5'000};
};

inline constexpr int maximum_catalog_busy_timeout_milliseconds = 5'000;

enum class CatalogQueryPlanRequirement {
    NoTemporaryBTree,
    IndexedAccess,
};

// Verifies that SQLite can execute an application scan without a temporary B-tree. Placeholder
// parameters are bound to NULL because only the selected plan, not query results, is inspected.
[[nodiscard]] auto verifyCatalogQueryPlan(
    QSqlDatabase& database, const QString& sql, const QString& label, CatalogSubject subject,
    const CatalogHooks& hooks, CatalogOperation operation, int bind_count = 0,
    CatalogQueryPlanRequirement requirement = CatalogQueryPlanRequirement::NoTemporaryBTree)
    -> std::expected<void, CatalogError>;

// Configures connection-local policy and verifies every readback. This helper deliberately does not
// select a journal mode: admission connections are read-only, while normative-reference creation
// establishes the production WAL/synchronous policy before creating its schema.
[[nodiscard]] auto configureCatalogFingerprintConnection(
    QSqlDatabase& database, CatalogSqliteConfiguration configuration, CatalogSubject subject,
    const CatalogHooks& hooks, CatalogOperation operation) -> std::expected<void, CatalogError>;

// Creates a fresh deterministic reference from the production migration prefix. Version1 contains
// exactly the historical three-table schema and migration row 1; Current additionally contains the
// v2 blob tables/index and migration row 2.
[[nodiscard]] auto createNormativeCatalogSchema(QSqlDatabase& database,
                                                CatalogSchemaGeneration generation,
                                                CatalogSubject subject, const CatalogHooks& hooks,
                                                CatalogOperation operation)
    -> std::expected<void, CatalogError>;

// Compares candidate logical metadata with a separately built normative reference. Query failures
// and injected lifecycle failures are operational CannotOpen results; any successfully read logical
// mismatch is CorruptCatalog.
[[nodiscard]] auto
compareCatalogLogicalFingerprint(QSqlDatabase& candidate, QSqlDatabase& normative_reference,
                                 CatalogSchemaGeneration generation,
                                 CatalogSubject candidate_subject, const CatalogHooks& hooks,
                                 CatalogOperation operation) -> std::expected<void, CatalogError>;

} // namespace appellate::packs::detail
