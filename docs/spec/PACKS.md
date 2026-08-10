# Content-pack contract

This document defines the intended version-1 resource taxonomy. The checked-in JSON schemas
and parser are the executable subset; adding a resource kind requires schema, parser,
validation, and round-trip tests together.

## Archive and identity

A distributable `.awpack` is a deterministic, uncompressed, non-ZIP64 ZIP archive. A typical
logical layout is:

```text
manifest.json
resources/*.json
assets/*.pdf
```

Paths use normalized UTF-8 forward-slash names. Archives may not contain links, device files,
encrypted members, duplicate paths, absolute paths, `.` or `..` segments, or undeclared files.
Version 1 rejects compression, ZIP64, comments, encryption, extra fields, links, directory
entries, and hidden bytes between members. Readers enforce bounded member count, per-member
size, and total size before staging any declared regular file.

IDs are globally namespaced lowercase tokens. Versions follow SemVer. Digests are lowercase
SHA-256 hex. Manifest content order does not confer behavior; canonical digest computation
sorts entries by content ID and hashes identity, kind, normalized path, and object digest.

## Version-1 resource kinds

| Kind | Owns | May reference |
| --- | --- | --- |
| `argument_config` | Argument clocks, issue graph, permitted source anchors | Case, bench, record, and authority IDs |
| `authority_set` | Versioned rules, statutes, orders, source dates, and propositions | Stable public source URLs |
| `bench_configuration` | Typed seats and presiding seat | Judge profiles and court |
| `case` | Synthetic parties, issues, facts, and authored disposition | Procedure, workflow, record, form, authority, and argument IDs |
| `court` | Court identity, jurisdiction, roles, and calendar | Authority sets |
| `filing_catalog` | Filing types, fields, service rules, and routes | Court, procedure, form, and authority IDs |
| `form` | Declarative fields and validation constraints | Filing catalog entries |
| `judge_profile` | Fictional/composite interaction controls and voice templates | Compatible courts and issue vocabulary |
| `procedure_profile` | Proceeding identity and supported built-in operations | Court, workflow, and filing catalog |
| `realism_review` | Per-dimension evidence, reviewer status, and uncertainty | Case and authority IDs |
| `record` | Docket entries, immutable document digests, and stable page anchors | Case and asset paths |
| `workflow` | Typed stages, roles, routes, deadlines, calendar, and authority bases | Filing catalog and authority IDs |

## Dependencies and compatibility

Dependencies lock an exact pack ID, version, and digest. Optional or version-range dependencies
are not supported in an executable session. A directed dependency graph must be acyclic.

The manifest declares exact required capability IDs and positive integer versions. Unknown
resource kinds, operation codes, schema versions, or required capabilities fail closed.

## Immutability and session pins

Installed archive objects use their archive SHA-256 as the filename and an SQLite catalog
records their immutable pack revision. Re-importing an identical revision is idempotent.
Reusing `(pack ID, version)` with a different digest is a hard conflict. A session records all
transitive revision pins before its first command.

## Import transaction

1. Stream the archive to a bounded staging location while hashing it.
2. Validate archive structure and declared limits without trusting file extensions.
3. Stage only declared regular files under generated private names.
4. Validate schemas, identities, dependencies, cross-references, digests, and semantics.
5. Fsync the staged archive and atomically rename it into the local pack store.
6. Commit the installed-revision row in SQLite. On any failure, remove staging state and leave
   the prior store and catalog unchanged.

## Authoring CLI

`appellate-pack` is deliberately non-interactive and emits exactly one compact JSON object per
invocation. Successful output goes to stdout with exit code `0`; errors go to stderr with a
nonzero exit code. Every response includes `schema_version: 1` and `status`.

```text
appellate-pack template <new-directory>
appellate-pack validate <directory-or-awpack>
appellate-pack export <directory> <new-awpack>
appellate-pack install <awpack> <catalog> [--installed-at YYYY-MM-DDTHH:MM:SSZ]
appellate-pack list <catalog>
```

`template` creates a complete fictional/composite example containing all version-1 resource
kinds and refuses an existing destination. `export` also refuses an existing destination.
Export orders members and fixes archive metadata, so identical source bytes produce identical
archives and the same revision digest. Supplying `--installed-at` is intended for reproducible
tests; normal interactive use records the current UTC second.
