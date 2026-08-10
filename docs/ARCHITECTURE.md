# Architecture

This architecture implements the accepted
[native, offline-first MVP boundary](adr/0001-native-offline-mvp.md).

## Shape

Appellate Workbench is a native Qt 6 application compiled in C++26 mode. Qt Widgets is used
for the dense docket, record, filing, and argument workspace. There is no embedded web
runtime and no application server.

The dependency direction is intentionally narrow:

```text
desktop-ui / cli -> packs / engine / storage
packs            -> model + Qt Core / Sql + libarchive
engine           -> model
storage          -> model + Qt Core / Sql
model            -> standard C++ only
```

Targets are added only with a real vertical slice. Optional encrypted object replication will
eventually live in a separate `sync` target; it is not part of persistence or simulation
correctness. There is no generic `core` junk drawer.

## Determinism and persistence

A session pins exact jurisdiction, procedure, case, bench-profile, and engine revisions.
Commands produce typed events. SQLite stores the event log and rebuildable projections;
large records live in a local content-addressed store. Exact replay persists typed acts and
rendered utterances, not merely a model seed.

Wall-clock and randomness enter through explicit interfaces and are recorded. Given the same
versions, initial state, seed, and user choices, the legal event trace is identical.

## Pack trust boundary

MVP packs are immutable, declarative data. A manifest names a schema version, namespaced pack
ID, semantic version, dependencies, content paths, and digests. Sessions pin `(pack ID,
version, digest)`.

Import is transactional. Validation rejects malformed schemas, absolute or traversal paths,
symlinks, duplicate IDs, digest mismatches, dependency cycles, and any executable payload.
Pack data cannot run SQL, native code, scripts, or network requests. New jurisdictions work
without recompilation only when their behavior can be expressed by built-in typed operations.

## Bench boundary

Bench configuration is structured data, not a branch on a profile name. Profiles declare a
compatible court role and observable interaction controls. A district-court profile cannot
be placed silently on an appellate panel. Changing interaction-only data may alter question
selection and phrasing, but never facts, deadlines, filing validity, or authored disposition.

Questions are planned as typed acts containing an intent and permitted authority, brief, or
record references. A deterministic template renderer is always available. A later local-model
adapter may realize prose, but it receives bounded acts and has no authority to mutate legal
state.

## Sync boundary

Sync is a replica layer, never the database. It transfers encrypted immutable objects through
a minimal provider interface. A local-folder adapter supports user-chosen tools such as
Syncthing or a cloud-drive folder; a later S3-compatible adapter is the reference remote.
SQLite, WAL files, plaintext records, and live row mutations are never uploaded. Conflicts
produce explicit branches.
