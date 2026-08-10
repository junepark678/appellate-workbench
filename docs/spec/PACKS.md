# Content-pack contract

This document defines the intended version-1 resource taxonomy. The checked-in JSON schemas
and parser are the executable subset; adding a resource kind requires schema, parser,
validation, and round-trip tests together.

## Archive and identity

A distributable pack is a deterministic ZIP-compatible archive with this logical layout:

```text
manifest.json
authorities/*.json
courts/*.json
procedures/*.json
forms/*
cases/*
records/*
benches/*.json
```

Paths use normalized UTF-8 forward-slash names. Archives may not contain links, device files,
encrypted members, duplicate paths, absolute paths, `.` or `..` segments, or undeclared files.
Readers enforce bounded member count, per-member size, total expanded size, and compression
ratio before extraction.

IDs are globally namespaced lowercase tokens. Versions follow SemVer. Digests are lowercase
SHA-256 hex. Manifest content order does not confer behavior; canonical digest computation
sorts entries by content ID and hashes identity, kind, normalized path, and object digest.

## Version-1 resource kinds

| Kind | Owns | May reference |
| --- | --- | --- |
| `authority_set` | Rules, statutes, orders, holiday calendars, source cutoffs | Stable public source URLs and quoted propositions |
| `court` | Court identity, roles, calendars, filing vocabulary | Authority sets |
| `procedure` | Typed stages, actors, filing catalog, deadlines, allowed operation codes | Court and authority IDs |
| `form` | Declarative fields and validation constraints | Procedure filing IDs |
| `case` | Synthetic parties, issues, facts, authored branches and dispositions | Procedure, authority, record, and form IDs |
| `record_asset` | Immutable document bytes and metadata | Docket entry and stable page anchors |
| `bench_profile` | Fictional/composite interaction controls and compatibility | Court IDs and issue-focus vocabulary |
| `realism_review` | Per-dimension evidence, reviewer status, uncertainty | Case and source IDs |

## Dependencies and compatibility

Dependencies lock an exact pack ID, version, and digest. Optional or version-range dependencies
are not supported in an executable session. A directed dependency graph must be acyclic.

The manifest declares the minimum and maximum engine capability versions it understands.
Unknown resource kinds, operation codes, schema versions, or required capabilities fail closed.

## Immutability and session pins

Installed revisions live under a digest-derived directory and are read-only. Re-importing an
identical revision is idempotent. Reusing `(pack ID, version)` with a different digest is a
hard conflict. A session records all transitive revision pins before its first command.

## Import transaction

1. Stream the archive to a bounded staging location while hashing it.
2. Validate archive structure and declared limits without trusting file extensions.
3. Extract only declared regular files into a new staging directory.
4. Validate schemas, identities, dependencies, cross-references, digests, and semantics.
5. Fsync staged files and atomically rename the revision into the local pack store.
6. Commit the installed-revision row in SQLite. On any failure, remove staging state and leave
   the prior store and catalog unchanged.
