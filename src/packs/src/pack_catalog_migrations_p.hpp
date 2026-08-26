#pragma once

#include <array>

namespace appellate::packs::detail {

inline constexpr auto catalog_migration_timestamp = "2026-08-11T00:00:00Z";

// These are the canonical production migration prefixes. Normative fingerprint references and the
// writable catalog must consume these same statements so a historical reference cannot drift from
// the migration it is intended to reproduce.
inline constexpr std::array catalog_version_one_schema{
    "CREATE TABLE IF NOT EXISTS catalog_migrations (version INTEGER PRIMARY KEY, "
    "applied_at_utc TEXT NOT NULL) STRICT",
    "CREATE TABLE IF NOT EXISTS pack_revisions ("
    "pack_id TEXT NOT NULL, version TEXT NOT NULL, digest TEXT NOT NULL, "
    "archive_sha256 TEXT NOT NULL, installed_at_utc TEXT NOT NULL, "
    "PRIMARY KEY(pack_id, version), UNIQUE(pack_id, version, digest)) STRICT",
    "CREATE TABLE IF NOT EXISTS pack_dependencies ("
    "pack_id TEXT NOT NULL, version TEXT NOT NULL, dependency_pack_id TEXT NOT NULL, "
    "dependency_version TEXT NOT NULL, dependency_digest TEXT NOT NULL, "
    "PRIMARY KEY(pack_id, version, dependency_pack_id, dependency_version), "
    "FOREIGN KEY(pack_id, version) REFERENCES pack_revisions(pack_id, version) "
    "ON DELETE CASCADE, FOREIGN KEY(dependency_pack_id, dependency_version, "
    "dependency_digest) REFERENCES pack_revisions(pack_id, version, digest) "
    "ON DELETE RESTRICT) STRICT",
};

inline constexpr std::array catalog_version_two_schema{
    "CREATE TABLE IF NOT EXISTS pack_blobs ("
    "pack_id TEXT NOT NULL, version TEXT NOT NULL, path TEXT NOT NULL, "
    "media_type TEXT NOT NULL CHECK(media_type = 'application/pdf'), "
    "byte_size INTEGER NOT NULL CHECK(byte_size BETWEEN 1 AND 536870912), "
    "sha256 TEXT NOT NULL CHECK(length(sha256) = 64 AND sha256 = lower(sha256) "
    "AND sha256 NOT GLOB '*[^0-9a-f]*'), "
    "PRIMARY KEY(pack_id, version, path), "
    "FOREIGN KEY(pack_id, version) REFERENCES pack_revisions(pack_id, version) "
    "ON DELETE CASCADE) STRICT",
    "CREATE INDEX IF NOT EXISTS pack_blobs_sha256 ON pack_blobs(sha256)",
    "CREATE TABLE IF NOT EXISTS pack_blob_sets ("
    "pack_id TEXT NOT NULL, version TEXT NOT NULL, "
    "blob_count INTEGER NOT NULL CHECK(blob_count BETWEEN 0 AND 10000), "
    "descriptor_sha256 TEXT NOT NULL CHECK(length(descriptor_sha256) = 64 "
    "AND descriptor_sha256 = lower(descriptor_sha256) "
    "AND descriptor_sha256 NOT GLOB '*[^0-9a-f]*'), "
    "PRIMARY KEY(pack_id, version), "
    "FOREIGN KEY(pack_id, version) REFERENCES pack_revisions(pack_id, version) "
    "ON DELETE CASCADE) STRICT",
};

} // namespace appellate::packs::detail
