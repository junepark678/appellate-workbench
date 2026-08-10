# ADR 0002: Declarative content-pack trust boundary

- Status: accepted
- Date: 2026-08-11
- GitHub issue: #2

## Context

Courts, procedures, records, cases, and bench behavior need to expand without rebuilding the
desktop application. Treating downloaded native libraries as ordinary “plugins” would give
untrusted legal content the authority to execute code and mutate local data.

## Decision

User-installable MVP packs are immutable declarative data. They may contain only resource
kinds defined by a versioned schema and interpreted through built-in typed operations. They
cannot contain or invoke native code, scripts, SQL, shell commands, dynamic libraries, Qt
plugins, model code, or network requests.

Every pack has a namespaced ID, semantic version, schema version, declared dependencies,
content inventory, per-object SHA-256 digest, and canonical pack digest. A session pins the
exact `(pack ID, version, digest)` tuple for every dependency.

Import uses a staging directory and becomes visible atomically only after validation.
Validation rejects traversal and absolute paths, links, duplicate IDs or paths, undeclared
files, unsupported resource kinds, invalid dependencies, dependency cycles, size-limit
violations, digest mismatches, and executable payloads. Updating a pack creates a new immutable
revision and cannot alter an existing session.

## Capability boundary

A new jurisdiction needs no application change when its behavior is expressible using the
built-in operation catalog. Novel legal semantics require a reviewed application release that
adds a typed capability. Executable third-party plugins are not an MVP extension mechanism.

## Consequences

- Packs remain portable, inspectable, and testable without trusting their authors with code
  execution.
- The operation catalog is part of the engine compatibility contract.
- Import/export, validation, provenance, and dependency locking are first-class APIs.
