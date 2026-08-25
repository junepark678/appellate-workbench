#include "pack_catalog_fingerprint_p.hpp"
#include "pack_catalog_migrations_p.hpp"

#include <QByteArray>
#include <QMetaType>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace appellate::packs::detail {
namespace {

constexpr std::uint64_t maximum_fingerprint_projection_rows = 20'000;
constexpr std::uint64_t maximum_fingerprint_cell_bytes = 1024ULL * 1024ULL;
constexpr int maximum_fingerprint_projection_columns = 16;
constexpr std::size_t maximum_query_plan_rows = 256;
constexpr qsizetype maximum_query_plan_detail_bytes = 4'096;

constexpr std::array version_one_tables{
    "catalog_migrations",
    "pack_dependencies",
    "pack_revisions",
};

constexpr std::array current_tables{
    "catalog_migrations", "pack_blob_sets", "pack_blobs", "pack_dependencies", "pack_revisions",
};

[[nodiscard]] auto failure(CatalogErrorCode code, QString message)
    -> std::unexpected<CatalogError> {
    return std::unexpected(CatalogError{code, std::move(message)});
}

[[nodiscard]] CatalogObservation observation(CatalogEvent event, CatalogSubject subject,
                                             CatalogOperation operation, const QString& label,
                                             std::size_t ordinal = 0,
                                             std::uint64_t byte_count = 0) {
    CatalogObservation result;
    result.event = event;
    result.subject = subject;
    result.operation = operation;
    result.component = label.toUtf8();
    result.ordinal = ordinal;
    result.byte_count = byte_count;
    return result;
}

template <typename Function>
[[nodiscard]] auto withLifecycle(const CatalogHooks& hooks, CatalogObservation seen,
                                 Function&& function) -> decltype(function()) {
    using Result = decltype(function());
    if (hooks.report != nullptr) {
        hooks.report->observations.push_back(seen);
    }
    const auto action = hooks.inject ? hooks.inject(seen) : CatalogInjectedAction::Continue;
    if (action == CatalogInjectedAction::FailBefore) {
        return Result(failure(CatalogErrorCode::CannotOpen,
                              QStringLiteral("Injected catalog-fingerprint failure")));
    }
    if (hooks.outcome) {
        const auto injected = hooks.outcome(seen);
        if (injected.has_value() && !injected->operation_succeeded) {
            return Result(failure(CatalogErrorCode::CannotOpen,
                                  QStringLiteral("Injected catalog-fingerprint outcome")));
        }
    }
    auto result = function();
    if (hooks.barrier) {
        hooks.barrier(seen);
    }
    if (hooks.observe) {
        hooks.observe(seen);
    }
    if (action == CatalogInjectedAction::FailAfter) {
        return Result(failure(CatalogErrorCode::CannotOpen,
                              QStringLiteral("Injected catalog-fingerprint failure")));
    }
    return result;
}

[[nodiscard]] auto queryFailure(const QSqlQuery& query, const QString& action)
    -> std::unexpected<CatalogError> {
    return failure(CatalogErrorCode::CannotOpen,
                   QStringLiteral("%1: %2").arg(action, query.lastError().text()));
}

[[nodiscard]] auto execute(QSqlDatabase& database, const QString& sql, const QString& action)
    -> std::expected<void, CatalogError> {
    QSqlQuery query(database);
    if (!query.exec(sql)) {
        return queryFailure(query, action);
    }
    return {};
}

[[nodiscard]] auto readIntegerPragma(QSqlDatabase& database, const QString& pragma,
                                     qlonglong expected, const QString& action)
    -> std::expected<void, CatalogError> {
    QSqlQuery query(database);
    query.setForwardOnly(true);
    if (!query.exec(QStringLiteral("PRAGMA %1").arg(pragma))) {
        return queryFailure(query, action);
    }
    bool converted = false;
    if (!query.next() || query.value(0).isNull() ||
        query.value(0).metaType().id() != QMetaType::LongLong ||
        query.value(0).toLongLong(&converted) != expected || !converted || query.next()) {
        return failure(CatalogErrorCode::CannotOpen,
                       QStringLiteral("%1 returned an unexpected value").arg(action));
    }
    return {};
}

[[nodiscard]] auto configureIntegerPragma(QSqlDatabase& database, const QString& assignment,
                                          const QString& readback, qlonglong expected,
                                          CatalogSubject subject, const CatalogHooks& hooks,
                                          CatalogOperation operation, std::size_t ordinal)
    -> std::expected<void, CatalogError> {
    return withLifecycle(
        hooks, observation(CatalogEvent::DatabaseConfigured, subject, operation, readback, ordinal),
        [&]() -> std::expected<void, CatalogError> {
            if (const auto configured =
                    execute(database, QStringLiteral("PRAGMA %1").arg(assignment),
                            QStringLiteral("configure %1").arg(readback));
                !configured) {
                return configured;
            }
            return readIntegerPragma(database, readback, expected,
                                     QStringLiteral("verify %1").arg(readback));
        });
}

[[nodiscard]] auto configureTextPragma(QSqlDatabase& database, const QString& assignment,
                                       const QString& readback, const QString& expected,
                                       CatalogSubject subject, const CatalogHooks& hooks,
                                       CatalogOperation operation, std::size_t ordinal)
    -> std::expected<void, CatalogError> {
    return withLifecycle(
        hooks, observation(CatalogEvent::DatabaseConfigured, subject, operation, readback, ordinal),
        [&]() -> std::expected<void, CatalogError> {
            QSqlQuery query(database);
            query.setForwardOnly(true);
            if (!query.exec(QStringLiteral("PRAGMA %1").arg(assignment))) {
                return queryFailure(query, QStringLiteral("configure %1").arg(readback));
            }
            if (!query.next() || query.value(0).isNull() ||
                query.value(0).metaType().id() != QMetaType::QString ||
                query.value(0).toString().compare(expected, Qt::CaseInsensitive) != 0 ||
                query.next()) {
                return failure(
                    CatalogErrorCode::CannotOpen,
                    QStringLiteral("verify %1 returned an unexpected value").arg(readback));
            }
            return {};
        });
}

enum class CellKind : std::uint8_t {
    Null,
    Integer,
    Real,
    Text,
    Blob,
};

constexpr std::size_t cell_kind_count = 5;

struct FingerprintCell final {
    CellKind kind{};
    QByteArray bytes;

    friend bool operator==(const FingerprintCell&, const FingerprintCell&) = default;
};

using FingerprintRow = std::vector<FingerprintCell>;
using FingerprintRows = std::vector<FingerprintRow>;

struct ColumnBounds final {
    std::array<std::uint64_t, cell_kind_count> kind_counts{};
    std::uint64_t maximum_bytes{};
};

struct ProjectionBounds final {
    std::uint64_t row_count{};
    std::vector<ColumnBounds> columns;
};

struct Projection final {
    QString label;
    QString sql;
    int column_count{};
};

struct CapturedProjection final {
    ProjectionBounds bounds;
    FingerprintRows rows;
};

[[nodiscard]] QString quotedSqlText(QString value) {
    value.replace(u'\'', QStringLiteral("''"));
    return QStringLiteral("'%1'").arg(value);
}

[[nodiscard]] auto nonnegativeInteger(const QVariant& value, const QString& action)
    -> std::expected<std::uint64_t, CatalogError> {
    bool converted = false;
    const auto result = value.toLongLong(&converted);
    if (!converted || result < 0) {
        return failure(CatalogErrorCode::CannotOpen,
                       QStringLiteral("%1 returned a non-integer bound").arg(action));
    }
    return static_cast<std::uint64_t>(result);
}

[[nodiscard]] QString preflightSql(const Projection& projection) {
    QString sql = QStringLiteral("SELECT COUNT(*)");
    constexpr std::array kinds{"null", "integer", "real", "text", "blob"};
    for (int column = 0; column < projection.column_count; ++column) {
        for (const auto* kind : kinds) {
            sql += QStringLiteral(", COALESCE(SUM(CASE WHEN typeof(c%1) = '%2' THEN 1 ELSE 0 "
                                  "END), 0)")
                       .arg(column)
                       .arg(QLatin1StringView(kind));
        }
        sql += QStringLiteral(", COALESCE(MAX(length(CAST(c%1 AS BLOB))), 0)").arg(column);
    }
    sql += QStringLiteral(" FROM (%1) AS bounded_projection").arg(projection.sql);
    return sql;
}

[[nodiscard]] auto preflightProjection(QSqlDatabase& database, const Projection& projection,
                                       const ProjectionBounds* reference_bounds)
    -> std::expected<ProjectionBounds, CatalogError> {
    if (projection.column_count <= 0 ||
        projection.column_count > maximum_fingerprint_projection_columns) {
        return failure(CatalogErrorCode::CannotOpen,
                       QStringLiteral("Catalog fingerprint projection has invalid width"));
    }
    QSqlQuery query(database);
    query.setForwardOnly(true);
    if (!query.exec(preflightSql(projection))) {
        return queryFailure(query, QStringLiteral("preflight %1").arg(projection.label));
    }
    if (!query.next()) {
        return failure(CatalogErrorCode::CannotOpen,
                       QStringLiteral("Catalog fingerprint preflight returned no row"));
    }
    const auto row_count = nonnegativeInteger(query.value(0), projection.label);
    if (!row_count) {
        return std::unexpected(row_count.error());
    }
    if (*row_count > maximum_fingerprint_projection_rows ||
        (reference_bounds != nullptr && *row_count != reference_bounds->row_count)) {
        return failure(CatalogErrorCode::CorruptCatalog,
                       QStringLiteral("Catalog fingerprint projection count differs"));
    }

    ProjectionBounds result;
    result.row_count = *row_count;
    result.columns.resize(static_cast<std::size_t>(projection.column_count));
    int value_index = 1;
    for (auto& column : result.columns) {
        for (auto& count : column.kind_counts) {
            const auto parsed = nonnegativeInteger(query.value(value_index), projection.label);
            ++value_index;
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            count = *parsed;
        }
        const auto maximum = nonnegativeInteger(query.value(value_index), projection.label);
        ++value_index;
        if (!maximum) {
            return std::unexpected(maximum.error());
        }
        column.maximum_bytes = *maximum;
        if (column.maximum_bytes > maximum_fingerprint_cell_bytes) {
            return failure(CatalogErrorCode::CorruptCatalog,
                           QStringLiteral("Catalog fingerprint cell exceeds its bound"));
        }
    }
    if (query.next()) {
        return failure(CatalogErrorCode::CannotOpen,
                       QStringLiteral("Catalog fingerprint preflight returned extra rows"));
    }
    if (reference_bounds != nullptr) {
        if (result.columns.size() != reference_bounds->columns.size()) {
            return failure(CatalogErrorCode::CorruptCatalog,
                           QStringLiteral("Catalog fingerprint projection width differs"));
        }
        for (std::size_t index = 0; index < result.columns.size(); ++index) {
            const auto& candidate = result.columns.at(index);
            const auto& reference = reference_bounds->columns.at(index);
            if (candidate.kind_counts != reference.kind_counts ||
                candidate.maximum_bytes > reference.maximum_bytes) {
                return failure(CatalogErrorCode::CorruptCatalog,
                               QStringLiteral("Catalog fingerprint projection type differs"));
            }
        }
    }
    return result;
}

[[nodiscard]] auto cellFromVariant(const QVariant& value, CatalogAllocationCounters* allocations)
    -> std::expected<FingerprintCell, CatalogError> {
    if (value.isNull()) {
        return FingerprintCell{CellKind::Null, {}};
    }
    const auto type = value.metaType().id();
    if (type == QMetaType::Int || type == QMetaType::LongLong || type == QMetaType::UInt ||
        type == QMetaType::ULongLong) {
        bool converted = false;
        const auto integer = value.toLongLong(&converted);
        if (!converted) {
            return failure(CatalogErrorCode::CannotOpen,
                           QStringLiteral("Cannot consume integer fingerprint cell"));
        }
        return FingerprintCell{CellKind::Integer, QByteArray::number(integer)};
    }
    if (type == QMetaType::Double) {
        bool converted = false;
        const auto real = value.toDouble(&converted);
        if (!converted) {
            return failure(CatalogErrorCode::CannotOpen,
                           QStringLiteral("Cannot consume real fingerprint cell"));
        }
        return FingerprintCell{CellKind::Real, QByteArray::number(real, 'g', 17)};
    }
    if (type == QMetaType::QString) {
        if (allocations != nullptr) {
            ++allocations->text_conversions;
        }
        return FingerprintCell{CellKind::Text, value.toString().toUtf8()};
    }
    if (type == QMetaType::QByteArray) {
        return FingerprintCell{CellKind::Blob, value.toByteArray()};
    }
    return failure(CatalogErrorCode::CorruptCatalog,
                   QStringLiteral("Catalog fingerprint contains an unexpected SQLite type"));
}

[[nodiscard]] bool cellLess(const FingerprintCell& left, const FingerprintCell& right) {
    if (left.kind != right.kind) {
        return left.kind < right.kind;
    }
    return left.bytes < right.bytes;
}

[[nodiscard]] bool rowLess(const FingerprintRow& left, const FingerprintRow& right) {
    return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
                                        cellLess);
}

[[nodiscard]] auto captureProjection(QSqlDatabase& database, const Projection& projection,
                                     const ProjectionBounds* reference_bounds,
                                     CatalogSubject subject, const CatalogHooks& hooks,
                                     CatalogOperation operation, bool count_allocations)
    -> std::expected<CapturedProjection, CatalogError> {
    if (const auto plan = verifyCatalogQueryPlan(database, preflightSql(projection),
                                                 projection.label + QStringLiteral(":preflight"),
                                                 subject, hooks, operation);
        !plan) {
        return std::unexpected(plan.error());
    }
    const auto bounds = preflightProjection(database, projection, reference_bounds);
    if (!bounds) {
        return std::unexpected(bounds.error());
    }
    auto seen = observation(CatalogEvent::SqlAllocationPreflight, subject, operation,
                            projection.label, static_cast<std::size_t>(bounds->row_count));
    for (const auto& column : bounds->columns) {
        seen.byte_count = std::max(seen.byte_count, column.maximum_bytes);
    }
    return withLifecycle(
        hooks, std::move(seen), [&]() -> std::expected<CapturedProjection, CatalogError> {
            if (const auto plan = verifyCatalogQueryPlan(database, projection.sql,
                                                         projection.label + QStringLiteral(":scan"),
                                                         subject, hooks, operation);
                !plan) {
                return std::unexpected(plan.error());
            }
            QSqlQuery query(database);
            query.setForwardOnly(true);
            if (!query.exec(projection.sql)) {
                return queryFailure(query, QStringLiteral("read %1").arg(projection.label));
            }
            CapturedProjection result;
            result.bounds = *bounds;
            result.rows.reserve(static_cast<std::size_t>(bounds->row_count));
            auto* allocations =
                count_allocations && hooks.report != nullptr ? &hooks.report->allocations : nullptr;
            if (allocations != nullptr) {
                allocations->row_container_growth += static_cast<std::size_t>(bounds->row_count);
            }
            std::array<std::array<std::uint64_t, cell_kind_count>,
                       maximum_fingerprint_projection_columns>
                observed_counts{};
            while (query.next()) {
                if (result.rows.size() >= static_cast<std::size_t>(bounds->row_count)) {
                    return failure(CatalogErrorCode::CorruptCatalog,
                                   QStringLiteral("Catalog fingerprint projection grew"));
                }
                FingerprintRow row;
                row.reserve(static_cast<std::size_t>(projection.column_count));
                for (int column = 0; column < projection.column_count; ++column) {
                    auto cell = cellFromVariant(query.value(column), allocations);
                    if (!cell) {
                        return std::unexpected(cell.error());
                    }
                    const auto kind_index = static_cast<std::size_t>(cell->kind);
                    ++observed_counts.at(static_cast<std::size_t>(column)).at(kind_index);
                    if (static_cast<std::uint64_t>(cell->bytes.size()) >
                        bounds->columns.at(static_cast<std::size_t>(column)).maximum_bytes) {
                        return failure(CatalogErrorCode::CorruptCatalog,
                                       QStringLiteral("Catalog fingerprint cell changed after "
                                                      "preflight"));
                    }
                    row.push_back(std::move(*cell));
                }
                result.rows.push_back(std::move(row));
            }
            if (result.rows.size() != static_cast<std::size_t>(bounds->row_count)) {
                return failure(CatalogErrorCode::CorruptCatalog,
                               QStringLiteral("Catalog fingerprint projection count changed"));
            }
            for (int column = 0; column < projection.column_count; ++column) {
                if (observed_counts.at(static_cast<std::size_t>(column)) !=
                    bounds->columns.at(static_cast<std::size_t>(column)).kind_counts) {
                    return failure(CatalogErrorCode::CorruptCatalog,
                                   QStringLiteral("Catalog fingerprint types changed after "
                                                  "preflight"));
                }
            }
            std::ranges::sort(result.rows, rowLess);
            return result;
        });
}

[[nodiscard]] auto compareProjection(QSqlDatabase& candidate, QSqlDatabase& reference,
                                     const Projection& projection, CatalogSubject candidate_subject,
                                     CatalogSubject reference_subject, const CatalogHooks& hooks,
                                     CatalogOperation operation)
    -> std::expected<CapturedProjection, CatalogError> {
    const auto expected = captureProjection(reference, projection, nullptr, reference_subject,
                                            hooks, operation, false);
    if (!expected) {
        return std::unexpected(expected.error());
    }
    const auto actual = captureProjection(candidate, projection, &expected->bounds,
                                          candidate_subject, hooks, operation, true);
    if (!actual) {
        return std::unexpected(actual.error());
    }
    if (actual->rows != expected->rows) {
        return failure(
            CatalogErrorCode::CorruptCatalog,
            QStringLiteral("Catalog logical fingerprint differs at %1").arg(projection.label));
    }
    return expected;
}

[[nodiscard]] Projection schemaProjection() {
    return Projection{
        QStringLiteral("sqlite_schema"),
        QStringLiteral("SELECT type AS c0, name AS c1, tbl_name AS c2, "
                       "COALESCE(sql, '') AS c3 FROM sqlite_schema "
                       "WHERE name NOT LIKE 'sqlite_%'"),
        4,
    };
}

[[nodiscard]] Projection databasePragmaProjection() {
    return Projection{
        QStringLiteral("database pragmas"),
        QStringLiteral("SELECT (SELECT application_id FROM pragma_application_id) AS c0, "
                       "(SELECT auto_vacuum FROM pragma_auto_vacuum) AS c1, "
                       "(SELECT timeout FROM pragma_busy_timeout) AS c2, "
                       "(SELECT encoding FROM pragma_encoding) AS c3, "
                       "(SELECT foreign_keys FROM pragma_foreign_keys) AS c4, "
                       "(SELECT journal_mode FROM pragma_journal_mode) AS c5, "
                       "(SELECT page_size FROM pragma_page_size) AS c6, "
                       "(SELECT query_only FROM pragma_query_only) AS c7, "
                       "(SELECT synchronous FROM pragma_synchronous) AS c8, "
                       "(SELECT temp_store FROM pragma_temp_store) AS c9, "
                       "(SELECT user_version FROM pragma_user_version) AS c10"),
        11,
    };
}

[[nodiscard]] Projection tableXinfoProjection(const QString& table) {
    return Projection{
        QStringLiteral("table_xinfo:%1").arg(table),
        QStringLiteral("SELECT cid AS c0, name AS c1, type AS c2, \"notnull\" AS c3, "
                       "dflt_value AS c4, pk AS c5, hidden AS c6 FROM pragma_table_xinfo(%1)")
            .arg(quotedSqlText(table)),
        7,
    };
}

[[nodiscard]] Projection indexListProjection(const QString& table) {
    return Projection{
        QStringLiteral("index_list:%1").arg(table),
        QStringLiteral("SELECT seq AS c0, name AS c1, \"unique\" AS c2, origin AS c3, "
                       "partial AS c4 FROM pragma_index_list(%1)")
            .arg(quotedSqlText(table)),
        5,
    };
}

[[nodiscard]] Projection indexXinfoProjection(const QString& index) {
    return Projection{
        QStringLiteral("index_xinfo:%1").arg(index),
        QStringLiteral("SELECT seqno AS c0, cid AS c1, name AS c2, \"desc\" AS c3, "
                       "coll AS c4, key AS c5 FROM pragma_index_xinfo(%1)")
            .arg(quotedSqlText(index)),
        6,
    };
}

[[nodiscard]] Projection foreignKeyListProjection(const QString& table) {
    return Projection{
        QStringLiteral("foreign_key_list:%1").arg(table),
        QStringLiteral("SELECT id AS c0, seq AS c1, \"table\" AS c2, \"from\" AS c3, "
                       "\"to\" AS c4, on_update AS c5, on_delete AS c6, \"match\" AS c7 "
                       "FROM pragma_foreign_key_list(%1)")
            .arg(quotedSqlText(table)),
        8,
    };
}

[[nodiscard]] Projection migrationProjection() {
    return Projection{
        QStringLiteral("catalog_migrations ledger"),
        QStringLiteral("SELECT version AS c0, applied_at_utc AS c1 FROM catalog_migrations "
                       "ORDER BY version"),
        2,
    };
}

[[nodiscard]] Projection integrityProjection() {
    return Projection{
        QStringLiteral("integrity_check"),
        QStringLiteral("SELECT integrity_check AS c0 FROM pragma_integrity_check"),
        1,
    };
}

[[nodiscard]] Projection foreignKeyCheckProjection() {
    return Projection{
        QStringLiteral("foreign_key_check"),
        QStringLiteral("SELECT \"table\" AS c0, rowid AS c1, parent AS c2, fkid AS c3 "
                       "FROM pragma_foreign_key_check"),
        4,
    };
}

[[nodiscard]] FingerprintRows expectedMigrationRows(CatalogSchemaGeneration generation) {
    FingerprintRows result;
    result.push_back(FingerprintRow{
        FingerprintCell{CellKind::Integer, QByteArrayLiteral("1")},
        FingerprintCell{CellKind::Text, QByteArray(catalog_migration_timestamp)},
    });
    if (generation == CatalogSchemaGeneration::Current) {
        result.push_back(FingerprintRow{
            FingerprintCell{CellKind::Integer, QByteArrayLiteral("2")},
            FingerprintCell{CellKind::Text, QByteArray(catalog_migration_timestamp)},
        });
    }
    return result;
}

[[nodiscard]] auto tablesFor(CatalogSchemaGeneration generation) -> std::span<const char* const> {
    if (generation == CatalogSchemaGeneration::Version1) {
        return version_one_tables;
    }
    return current_tables;
}

[[nodiscard]] auto executeSchemaStatements(QSqlDatabase& database,
                                           std::span<const char* const> statements)
    -> std::expected<void, CatalogError> {
    for (const auto* statement : statements) {
        if (const auto applied = execute(database, QLatin1StringView(statement),
                                         QStringLiteral("create normative catalog schema"));
            !applied) {
            return applied;
        }
    }
    return {};
}

} // namespace

auto verifyCatalogQueryPlan(QSqlDatabase& database, const QString& sql, const QString& label,
                            CatalogSubject subject, const CatalogHooks& hooks,
                            CatalogOperation operation, int bind_count,
                            CatalogQueryPlanRequirement requirement)
    -> std::expected<void, CatalogError> {
    if (!database.isValid() || !database.isOpen() || sql.isEmpty() || bind_count < 0 ||
        bind_count > maximum_fingerprint_projection_columns) {
        return failure(CatalogErrorCode::CannotOpen,
                       QStringLiteral("Cannot verify an invalid catalog query plan"));
    }
    return withLifecycle(
        hooks, observation(CatalogEvent::QueryPlanVerified, subject, operation, label),
        [&]() -> std::expected<void, CatalogError> {
            QSqlQuery query(database);
            query.setForwardOnly(true);
            if (!query.prepare(QStringLiteral("EXPLAIN QUERY PLAN ") + sql)) {
                return queryFailure(query, QStringLiteral("prepare query plan for %1").arg(label));
            }
            for (int index = 0; index < bind_count; ++index) {
                query.addBindValue(QVariant{});
            }
            if (!query.exec()) {
                return queryFailure(query, QStringLiteral("verify query plan for %1").arg(label));
            }
            std::size_t rows = 0;
            bool indexed_access = false;
            while (query.next()) {
                if (++rows > maximum_query_plan_rows || query.value(0).isNull() ||
                    query.value(1).isNull() || query.value(2).isNull() ||
                    query.value(0).metaType().id() != QMetaType::LongLong ||
                    query.value(1).metaType().id() != QMetaType::LongLong ||
                    query.value(2).metaType().id() != QMetaType::LongLong ||
                    query.value(3).isNull() ||
                    query.value(3).metaType().id() != QMetaType::QString) {
                    return failure(CatalogErrorCode::CannotOpen,
                                   QStringLiteral("Catalog query plan is malformed"));
                }
                const auto detail = query.value(3).toString();
                if (detail.size() > maximum_query_plan_detail_bytes ||
                    detail.toUtf8().size() > maximum_query_plan_detail_bytes) {
                    return failure(CatalogErrorCode::CannotOpen,
                                   QStringLiteral("Catalog query plan exceeds its bound"));
                }
                const auto unsafe_code = subject == CatalogSubject::Version1Reference ||
                                                 subject == CatalogSubject::CurrentReference
                                             ? CatalogErrorCode::CannotOpen
                                             : CatalogErrorCode::CorruptCatalog;
                if (detail.contains(QStringLiteral("TEMP B-TREE"), Qt::CaseInsensitive) ||
                    detail.contains(QStringLiteral("AUTOMATIC INDEX"), Qt::CaseInsensitive)) {
                    return failure(
                        unsafe_code,
                        QStringLiteral("Catalog query plan requires a temporary B-tree"));
                }
                indexed_access =
                    indexed_access ||
                    detail.contains(QStringLiteral("USING INDEX"), Qt::CaseInsensitive) ||
                    detail.contains(QStringLiteral("USING COVERING INDEX"), Qt::CaseInsensitive) ||
                    detail.contains(QStringLiteral("USING INTEGER PRIMARY KEY"),
                                    Qt::CaseInsensitive) ||
                    detail.contains(QStringLiteral("VIRTUAL TABLE INDEX"), Qt::CaseInsensitive);
            }
            if (rows == 0) {
                return failure(CatalogErrorCode::CannotOpen,
                               QStringLiteral("Catalog query plan returned no rows"));
            }
            if (requirement == CatalogQueryPlanRequirement::IndexedAccess && !indexed_access) {
                return failure(CatalogErrorCode::CorruptCatalog,
                               QStringLiteral("Catalog query plan does not use an admitted index"));
            }
            return {};
        });
}

auto configureCatalogFingerprintConnection(QSqlDatabase& database,
                                           CatalogSqliteConfiguration configuration,
                                           CatalogSubject subject, const CatalogHooks& hooks,
                                           CatalogOperation operation)
    -> std::expected<void, CatalogError> {
    if (!database.isValid() || !database.isOpen() || configuration.busy_timeout_milliseconds < 0 ||
        configuration.busy_timeout_milliseconds > maximum_catalog_busy_timeout_milliseconds) {
        return failure(CatalogErrorCode::CannotOpen,
                       QStringLiteral("Invalid catalog fingerprint database configuration"));
    }
    const auto foreign_keys = configuration.foreign_keys ? 1LL : 0LL;
    if (const auto configured = configureIntegerPragma(
            database,
            QStringLiteral("foreign_keys = %1")
                .arg(configuration.foreign_keys ? QStringLiteral("ON") : QStringLiteral("OFF")),
            QStringLiteral("foreign_keys"), foreign_keys, subject, hooks, operation, 0);
        !configured) {
        return configured;
    }
    const auto query_only = configuration.query_only ? 1LL : 0LL;
    if (const auto configured = configureIntegerPragma(
            database,
            QStringLiteral("query_only = %1")
                .arg(configuration.query_only ? QStringLiteral("ON") : QStringLiteral("OFF")),
            QStringLiteral("query_only"), query_only, subject, hooks, operation, 1);
        !configured) {
        return configured;
    }
    if (const auto configured =
            configureIntegerPragma(database, QStringLiteral("temp_store = MEMORY"),
                                   QStringLiteral("temp_store"), 2, subject, hooks, operation, 2);
        !configured) {
        return configured;
    }
    return configureIntegerPragma(
        database, QStringLiteral("busy_timeout = %1").arg(configuration.busy_timeout_milliseconds),
        QStringLiteral("busy_timeout"), configuration.busy_timeout_milliseconds, subject, hooks,
        operation, 3);
}

auto createNormativeCatalogSchema(QSqlDatabase& database, CatalogSchemaGeneration generation,
                                  CatalogSubject subject, const CatalogHooks& hooks,
                                  CatalogOperation operation) -> std::expected<void, CatalogError> {
    if (generation != CatalogSchemaGeneration::Version1 &&
        generation != CatalogSchemaGeneration::Current) {
        return failure(CatalogErrorCode::CannotOpen,
                       QStringLiteral("Unknown normative catalog schema generation"));
    }
    if (const auto configured = configureCatalogFingerprintConnection(
            database, CatalogSqliteConfiguration{true, false, 5'000}, subject, hooks, operation);
        !configured) {
        return configured;
    }
    if (const auto journal = configureTextPragma(
            database, QStringLiteral("journal_mode = WAL"), QStringLiteral("journal_mode"),
            QStringLiteral("wal"), subject, hooks, operation, 4);
        !journal) {
        return journal;
    }
    if (const auto synchronous =
            configureIntegerPragma(database, QStringLiteral("synchronous = FULL"),
                                   QStringLiteral("synchronous"), 2, subject, hooks, operation, 5);
        !synchronous) {
        return synchronous;
    }

    return withLifecycle(
        hooks,
        observation(CatalogEvent::MigrationStarted, subject, operation,
                    generation == CatalogSchemaGeneration::Version1
                        ? QStringLiteral("normative-v1")
                        : QStringLiteral("normative-current")),
        [&]() -> std::expected<void, CatalogError> {
            if (const auto begun = execute(database, QStringLiteral("BEGIN IMMEDIATE"),
                                           QStringLiteral("begin normative schema transaction"));
                !begun) {
                return begun;
            }
            const auto rollback = [&database]() {
                QSqlQuery query(database);
                static_cast<void>(query.exec(QStringLiteral("ROLLBACK")));
            };
            if (const auto applied = executeSchemaStatements(database, catalog_version_one_schema);
                !applied) {
                rollback();
                return applied;
            }
            if (generation == CatalogSchemaGeneration::Current) {
                if (const auto applied =
                        executeSchemaStatements(database, catalog_version_two_schema);
                    !applied) {
                    rollback();
                    return applied;
                }
            }
            QSqlQuery migration(database);
            migration.prepare(QStringLiteral(
                "INSERT INTO catalog_migrations(version, applied_at_utc) VALUES(?, ?)"));
            const auto last_version = generation == CatalogSchemaGeneration::Version1 ? 1 : 2;
            for (int version = 1; version <= last_version; ++version) {
                migration.bindValue(0, version);
                migration.bindValue(1, QLatin1StringView(catalog_migration_timestamp));
                if (!migration.exec()) {
                    const auto error = queryFailure(
                        migration, QStringLiteral("record normative catalog migration"));
                    rollback();
                    return error;
                }
            }
            if (const auto committed = execute(database, QStringLiteral("COMMIT"),
                                               QStringLiteral("commit normative schema"));
                !committed) {
                rollback();
                return committed;
            }
            return withLifecycle(hooks,
                                 observation(CatalogEvent::MigrationCommitted, subject, operation,
                                             QStringLiteral("normative-schema")),
                                 []() -> std::expected<void, CatalogError> { return {}; });
        });
}

auto compareCatalogLogicalFingerprint(QSqlDatabase& candidate, QSqlDatabase& normative_reference,
                                      CatalogSchemaGeneration generation,
                                      CatalogSubject candidate_subject, const CatalogHooks& hooks,
                                      CatalogOperation operation)
    -> std::expected<void, CatalogError> {
    if (!candidate.isValid() || !candidate.isOpen() || !normative_reference.isValid() ||
        !normative_reference.isOpen() ||
        (generation != CatalogSchemaGeneration::Version1 &&
         generation != CatalogSchemaGeneration::Current)) {
        return failure(CatalogErrorCode::CannotOpen,
                       QStringLiteral("Catalog fingerprint databases are not open"));
    }
    const auto reference_subject = generation == CatalogSchemaGeneration::Version1
                                       ? CatalogSubject::Version1Reference
                                       : CatalogSubject::CurrentReference;

    return withLifecycle(
        hooks,
        observation(CatalogEvent::SchemaFingerprintCompared, candidate_subject, operation,
                    QStringLiteral("logical-fingerprint")),
        [&]() -> std::expected<void, CatalogError> {
            if (const auto schema =
                    compareProjection(candidate, normative_reference, schemaProjection(),
                                      candidate_subject, reference_subject, hooks, operation);
                !schema) {
                return std::unexpected(schema.error());
            }
            if (const auto pragmas =
                    compareProjection(candidate, normative_reference, databasePragmaProjection(),
                                      candidate_subject, reference_subject, hooks, operation);
                !pragmas) {
                return std::unexpected(pragmas.error());
            }
            const auto migrations =
                compareProjection(candidate, normative_reference, migrationProjection(),
                                  candidate_subject, reference_subject, hooks, operation);
            if (!migrations) {
                return std::unexpected(migrations.error());
            }
            if (migrations->rows != expectedMigrationRows(generation)) {
                return failure(CatalogErrorCode::CorruptCatalog,
                               QStringLiteral("Catalog migration ledger differs"));
            }

            for (const auto* table_name : tablesFor(generation)) {
                const auto table = QLatin1StringView(table_name);
                if (const auto columns = compareProjection(
                        candidate, normative_reference, tableXinfoProjection(table),
                        candidate_subject, reference_subject, hooks, operation);
                    !columns) {
                    return std::unexpected(columns.error());
                }
                const auto indexes =
                    compareProjection(candidate, normative_reference, indexListProjection(table),
                                      candidate_subject, reference_subject, hooks, operation);
                if (!indexes) {
                    return std::unexpected(indexes.error());
                }
                if (const auto foreign_keys = compareProjection(
                        candidate, normative_reference, foreignKeyListProjection(table),
                        candidate_subject, reference_subject, hooks, operation);
                    !foreign_keys) {
                    return std::unexpected(foreign_keys.error());
                }
                for (const auto& index_row : indexes->rows) {
                    if (index_row.size() != 5 || index_row.at(1).kind != CellKind::Text) {
                        return failure(CatalogErrorCode::CannotOpen,
                                       QStringLiteral("Normative index metadata is invalid"));
                    }
                    const auto index_name = QString::fromUtf8(index_row.at(1).bytes);
                    if (const auto index = compareProjection(
                            candidate, normative_reference, indexXinfoProjection(index_name),
                            candidate_subject, reference_subject, hooks, operation);
                        !index) {
                        return std::unexpected(index.error());
                    }
                }
            }

            if (const auto integrity = withLifecycle(
                    hooks,
                    observation(CatalogEvent::IntegrityChecked, candidate_subject, operation,
                                QStringLiteral("integrity_check")),
                    [&]() -> std::expected<void, CatalogError> {
                        const auto captured = compareProjection(
                            candidate, normative_reference, integrityProjection(),
                            candidate_subject, reference_subject, hooks, operation);
                        if (!captured) {
                            return std::unexpected(captured.error());
                        }
                        const FingerprintRows ok{FingerprintRow{
                            FingerprintCell{CellKind::Text, QByteArrayLiteral("ok")}}};
                        if (captured->rows != ok) {
                            return failure(CatalogErrorCode::CorruptCatalog,
                                           QStringLiteral("Catalog integrity check failed"));
                        }
                        return {};
                    });
                !integrity) {
                return integrity;
            }
            return withLifecycle(
                hooks,
                observation(CatalogEvent::ForeignKeysChecked, candidate_subject, operation,
                            QStringLiteral("foreign_key_check")),
                [&]() -> std::expected<void, CatalogError> {
                    const auto captured = compareProjection(
                        candidate, normative_reference, foreignKeyCheckProjection(),
                        candidate_subject, reference_subject, hooks, operation);
                    if (!captured) {
                        return std::unexpected(captured.error());
                    }
                    if (!captured->rows.empty()) {
                        return failure(CatalogErrorCode::CorruptCatalog,
                                       QStringLiteral("Catalog foreign-key check failed"));
                    }
                    return {};
                });
        });
}

} // namespace appellate::packs::detail
