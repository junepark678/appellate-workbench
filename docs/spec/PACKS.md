# Content-pack contract

This document defines the declarative resource taxonomy. Schema version 1 is frozen, including
its canonical digest algorithm. Schema version 2 initially has the same resource semantics but
uses its own manifest/resource schemas, exact capability versions, and canonical digest domain;
later version-2 additions must not change how a version-1 pack is interpreted. The checked-in
JSON schemas and parser are the executable subset; adding a resource kind requires schema,
parser, validation, and round-trip tests together.

## Archive and identity

A distributable `.awpack` is a deterministic, uncompressed, non-ZIP64 ZIP archive. A typical
logical layout is:

```text
manifest.json
resources/*.json
objects/*.pdf
```

Paths use normalized UTF-8 forward-slash names. Archives may not contain links, device files,
encrypted members, duplicate paths, absolute paths, `.` or `..` segments, or undeclared files.
Version 1 rejects compression, ZIP64, comments, encryption, extra fields, links, directory
entries, and hidden bytes between members. Readers enforce bounded member count, per-member
size, and total size before staging any declared regular file.

Every version-1 manifest has a `blobs` array, including packs that declare no blobs. A blob
descriptor has exactly `path`, `media_type`, `byte_size`, and `sha256`; version 1 accepts only
`application/pdf`. Content and blob paths share one portable, non-overlapping namespace, with at
most 10,000 combined descriptors. Each blob is limited to 512 MiB and declared blob bytes are
limited to 3 GiB per pack. Readers stream blobs to verify their exact size and SHA-256 plus a PDF
header and end marker. Every record `asset_path` and `asset_sha256` must match one declared blob,
and unreferenced blobs are rejected.

The pack reader deliberately does not implement a partial PDF parser. Checking a record's
declared `page_count` against the parsed document belongs at the QtPdf runtime record-workspace
boundary; fixture integration tests compare those values so authored packs remain consistent.

IDs are globally namespaced lowercase tokens. Versions follow SemVer. Digests are lowercase
SHA-256 hex. Manifest content order does not confer behavior; canonical digest computation
sorts entries by content ID and blobs by descriptor fields, then hashes length-framed identity,
kind, normalized path, size, media type, and digest fields.

## Version-1 resource kinds

| Kind | Owns | May reference |
| --- | --- | --- |
| `argument_config` | Argument clocks, issue graph, permitted source anchors | Case, bench, record, and authority IDs |
| `authority_set` | Versioned rules, statutes, orders, source dates, and propositions | Stable public source URLs |
| `bench_configuration` | Typed seats and presiding seat | Judge profiles and court |
| `case` | Synthetic parties, issues, facts, and authored disposition | Procedure, workflow, record, form, authority, and argument IDs |
| `court` | Court identity, jurisdiction, roles, and calendar | Authority sets |
| `filing_catalog` | Filing types, field inventories, and eligible actor roles | Authority IDs |
| `form` | Declarative fields and validation constraints | Filing catalog entries |
| `judge_profile` | Fictional/composite interaction controls and voice templates | Compatible courts and issue vocabulary |
| `procedure_profile` | Proceeding identity and supported built-in operations | Court, workflow, and filing catalog |
| `realism_review` | Per-dimension evidence, reviewer status, and uncertainty | Case and authority IDs |
| `record` | Docket entries, immutable document digests, and stable page anchors | Case and asset paths |
| `workflow` | Typed stages, roles, routes, deadlines, calendar, and authority bases | Filing catalog and authority IDs |

### Canonical authority provenance

Schema version 1 remains frozen: authority references keep their original citation, source date,
proposition, and optional public URL shape, and their bytes and revision digests do not change.
Schema version 2 makes authority identity a closed, canonical contract. Every authority-set entry
must declare its type, jurisdiction ID, issuing-body ID, precedential status, official-source
flag, citation, source version, verification date, locator, HTTPS source URL, and proposition.
The source version may not be later than the set's source cutoff, and the verification date may
not predate the source version.

Authority source URLs use one deliberately narrow representation at every boundary: lowercase
DNS host, no credentials or explicit port, no fragment, printable ASCII path/query bytes, and
uppercase percent escapes. Pack reading, dependency resolution, runtime projection, engine
validation, and journal decoding all apply the same predicate so a pack cannot install
successfully and then fail only when a sourced event is replayed.

Version-2 workflow authority bases contain only `primary_authority_id` and a bounded unique list
of `supporting_authority_ids`. Runtime projection resolves those IDs through the exact authority
sets visible in the pack's dependency closure and copies the complete authority snapshot into
each emitted event. Whether an authority is primary or supporting, and what legal effect it has
for a particular operation, remain contextual rather than intrinsic source metadata. Duplicate
authority IDs anywhere in a resolved closure are fatal; there is no metadata merge or override.

Any version-2 pack containing authority-bearing resources must declare
`workbench.pack.canonical-authority` version 1 in addition to its generation baseline. The
canonical-authority event envelopes use persistence schema version 2. Legacy authority events
remain schema version 1 byte-for-byte; mixed legacy/canonical bases and schema relabeling fail
closed.

### Record metadata and grounding

The original version-1 record fields remain valid. A record may additionally declare bounded
`dockets` and `page_anchors` arrays. Each docket has a namespaced `docket_id`, a `docket_type`
of `district`, `appellate`, `agency`, or `original`, a human-readable `court_ref`, a
`public_docket_number`, and a `caption`. An optional `court_id` is a strict reference to a court
resource; `court_ref` remains required so a lower tribunal that is not otherwise modeled can be
identified honestly.

A docket entry keeps its globally unique positive integer `entry_number` and may add a
`docket_id`, display `entry_label`, `actor`, `description`, unique bounded `tags`, and a
`parent_entry_id`. A parent link and its `relationship` must appear together; relationships are
limited to `attachment`, `amendment`, `supplement`, and `component`. Parent links must resolve
within the same docket and form an acyclic graph. Display labels are unique within a docket.

A page anchor requires a namespaced `anchor_id`, an `entry_id`, and a one-based `page_number`,
and may carry a `citation_label` such as `JA40`. Anchor IDs cannot collide with record-entry IDs;
citations are unique, entries must resolve, and pages may not exceed the entry's declared
`page_count`. A case issue's `record_anchor_ids` may name either a record-entry ID or a declared
page-anchor ID. The native controller converts the authored page number to the zero-based page
index used by QtPdf, while preserving the citation for filtering and direct navigation. Sealed
documents remain listed but the native workspace refuses both docket and page-anchor opening.

When optional metadata is absent, the native projection uses `Not specified by pack`; it does
not infer an actor, public docket number, display entry number, description, parent, or
relationship.

The installed-record controller accepts either a standalone `LoadedPack` or a catalog-created
`ResolvedPack`; both are trusted only as output of the corresponding pack boundary. It rebuilds
the canonical runtime projection, requires exact equality with any separately supplied runtime
value, and then uses only the rebuilt projection. This closes split-representation changes to
sealed state, metadata, relationships, and anchors without retaining the authoring tree or the
author-provided source archive. An already resolved in-memory closure can continue opening its
verified root blobs from the content store if the installed archive becomes unavailable; a future
catalog reload still requires that installed archive because declarative resources are not stored
separately.

### Filing-route outcomes

A runnable workflow declares at least one executable filing route. A filing catalog may be a
strict superset of those routes so it can also describe reference-only or template filing types.
Every executable route must still name one declared catalog filing, exactly match that filing's
eligible roles and required fields, and use only actor and service roles declared by the owning
procedure profile.

Each route names its own `accept_operation_id` and `reject_operation_id`. Both operations must
belong to the route's stage and carry the matching typed opcode and a complete, versioned
authority basis. Different filing types in the same stage may therefore use different rejection
authorities. A submission for which the current stage has no route fails closed as an invalid
command; the engine does not borrow an unrelated rejection operation or synthesize authority.

Nonconforming submissions have three explicit supported outcomes:

- omit `deficiency_operation_id` to reject under the route's rejection operation;
- provide `deficiency_operation_id` alone to issue a curable deficiency with no invented fixed
  deadline; or
- also provide `deficiency_deadline` to issue a deficiency and calculate its sourced deadline.

`deficiency_deadline` is invalid without `deficiency_operation_id`. Deadline plans must reference
a local `calculate_deadline` operation; accepted-filing deadline IDs remain unique and exact.
This version intentionally has no generic condition or expression language.

Fixed deadlines whose trigger is a court event use an explicit recorded
`CalculateWorkflowDeadline` command. The selected `calculate_deadline` operation supplies the
versioned authority, day count, and calendar/business-day rule; the command records the court
actor, trigger instant and court date, and deadline ID. Replay recalculates the due date and
rejects a changed trigger, authority, identifier, calendar, or result. Wall-clock time is never
read inside the engine.

Court-controlled stage transitions use an explicit `AdvanceWorkflowStage` command. The command
contains only the common header and an `advance_stage` operation ID: the pinned workflow
definition supplies both the next stage and its versioned authority. The operation must belong to
the current stage and authorize the command actor's court role. Filing-route `advance_operation_id`
references are limited to operations with no court-role restriction, so a party filing cannot
silently exercise an operation reserved to the court.

## Dependencies and compatibility

Dependencies lock an exact pack ID, version, and digest. Optional or version-range dependencies
are not supported in an executable session. A directed dependency graph must be acyclic.

Standalone validation and export remain strict: every reference must resolve inside that one
pack. A thin pack must be exported explicitly with `export-deferred`; this validates its archive,
schemas, identities, files, blobs, and resource payloads but marks its resource graph as
non-executable until catalog resolution. The catalog never fetches dependencies. It resolves only
exact revisions already installed on the device, verifies each archive against its recorded
direct-dependency rows, and constructs a non-forgeable `ResolvedPack` in deterministic
dependency-first order. The closure is limited to 128 revisions and 10,000 combined resources.

Only one exact revision of a pack ID may appear in a closure. Resource IDs are globally unique:
there is no root-wins, last-wins, override, or identical-byte exception. Each pack may see only
itself and its own declared transitive dependencies; a sibling or consuming root cannot satisfy
an undeclared reference. Root-owned cases and argument configurations are the only runtime entry
points, and a root case must use a root-owned record. Blob paths remain local to their owning pack
even when two packs use the same relative path. Closure-scoped materialization therefore binds
both the verified closure and the exact owning revision; standalone packs continue to use their
one exact revision directly.

The manifest declares exact required capability IDs and positive integer versions. Capability
negotiation uses the executable registry: version-1 packs use version-1 capabilities and
version-2 packs use version-2 capabilities. Schema version 1 remains declaration-only for
compatibility with frozen revisions. Every schema-version-2 pack must declare
`workbench.pack.declarative-resources` version 2. A version-2 pack containing a `judge_profile`
must additionally declare `workbench.pack.judge-profile` version 2 and
`workbench.pack.voice-style` version 2. An empty list, an unrelated-only declaration, or a
declaration missing any required capability fails at pack read, resolved-catalog load, and
independent runtime projection. Unknown IDs, unsupported versions, and capabilities declared for
the wrong manifest generation fail before runtime projection. Unknown resource kinds, unsupported
kind/version pairs, operation codes, schema versions, and mixed-version manifest/resource sets
also fail closed.

Schema and resource-kind registries are separate per manifest generation even where their current
file names match. Adding a version-2-only schema or kind therefore cannot make the version-1
loader require or expose it.

Version 1 uses the `appellate-workbench-pack-revision-v1` digest domain exactly as originally
shipped. Version 2 uses `appellate-workbench-pack-revision-v2`; a version-2 descriptor can never
be interpreted through a version-1 schema or collide with an otherwise corresponding
version-1 revision.

## Immutability and session pins

Installed archive objects use their archive SHA-256 as the filename and an SQLite catalog
records their immutable pack revision. Re-importing an identical revision is idempotent.
Reusing `(pack ID, version)` with a different digest is a hard conflict. A session records all
transitive revision pins before its first command, sorted by pack ID. Reopen requires the exact
same complete pin set; a missing or changed dependency pin is treated as a corrupt session.
Catalog-backed session-controller entry points accept the `ResolvedPack` itself and derive this
pin set centrally rather than asking UI code to reconstruct it.

Every persisted session also records an immutable authority contract: `legacy-v1` or
`canonical-v2`. Existing schema-1 SQLite databases migrate to `legacy-v1` without rewriting
their command or event bytes. A schema-version-2 workflow session can be created or reopened only
by selecting a root-owned case ID from an exact `ResolvedPack`; the controller reconstructs its
workflow, case, complete authority snapshots, and revision pins from that closure. Caller-supplied
definition creation remains available only for an exact schema-version-1 `ResolvedPack`; raw
vector-pin creation seams are private to tests. Raw definition reopen remains public solely to
restore existing `legacy-v1` rows. Reopen compares the stored contract before decoding the journal,
so stripping provenance and relabeling events cannot route a canonical session through a legacy
API.

## Import transaction

1. Stream the archive to a bounded staging location while hashing it.
2. Validate archive structure and declared limits without trusting file extensions.
3. Stage only declared regular files under generated private names.
4. Validate schemas, identities, dependencies, cross-references, digests, and semantics.
5. Fsync the staged archive and atomically publish archive and content-addressed blob objects.
6. Commit the installed revision, exact dependencies, and blob descriptors in one SQLite
   transaction. On any failure, roll back SQLite and remove only newly published objects that no
   committed catalog row references; pre-existing deduplicated objects are preserved. A
   catalog-local interprocess lock covers publication, commit, and cleanup so two installers
   cannot race that reference check.

## Authoring CLI

`appellate-pack` is deliberately non-interactive and emits exactly one compact JSON object per
invocation. Successful output goes to stdout with exit code `0`; errors go to stderr with a
nonzero exit code. Every response includes `schema_version: 1` and `status`.

```text
appellate-pack template <new-directory>
appellate-pack validate <directory-or-awpack>
appellate-pack export <directory> <new-awpack>
appellate-pack export-deferred <directory> <new-awpack>
appellate-pack install <awpack> <catalog> [--installed-at YYYY-MM-DDTHH:MM:SSZ]
appellate-pack list <catalog>
appellate-pack validate-resolved <catalog> <pack-id> <version> <digest>
```

`template` creates a complete fictional/composite schema-version-2 example containing every
currently supported resource kind and refuses an existing destination. The CLI continues to
validate, export, install, and list immutable version-1 packs without rewriting them. `export`
also refuses an existing destination.
Export orders members and fixes archive metadata, so identical source bytes produce identical
archives and the same revision digest. Supplying `--installed-at` is intended for reproducible
tests; normal interactive use records the current UTC second.

`export-deferred` is the only authoring command that permits dependency-resolved references; its
JSON response states that resolution has not yet occurred. `validate-resolved` accepts an exact
root revision in a local catalog, validates the whole installed closure, and returns the complete
pack-ID-sorted revision-pin set. Neither command performs network access or makes a deferred pack
directly executable.
