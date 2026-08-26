# Operator guide

This guide describes the current pre-MVP Linux desktop's local data, portable-session,
pack-authoring, bench-profile, sealed-record, and optional-sync boundaries. It does not make any
case gold or level 3, satisfy the independent-review gate, or declare the MVP complete. Release
builders should also follow the [native release procedure](RELEASE.md); pack authors should use
the complete [pack contract and CLI reference](spec/PACKS.md).

## Offline operation and local state

The ordinary desktop performs no telemetry, analytics, crash upload, update check, or network
request. It needs no account, server, institution, instructor, browser runtime, or network
connection. URLs stored as pack authorities are citations, not instructions for the application to
fetch them. The optional sync library described below is not connected to the desktop and does not
change this ordinary-operation boundary.

`QStandardPaths::AppLocalDataLocation` is the authoritative default local-state root. Its concrete
path is selected by Qt from the platform environment; operators must not infer the authoritative
path from an example home-directory string. In the production desktop, the default tree is:

```text
<AppLocalDataLocation>/
|-- packs/                         local immutable-pack catalog
`-- sessions/
    |-- sessions.sqlite            Workflow/Oral session authority
    |-- assets/                    paired content-addressed session documents
    `-- record-access.sqlite       separate sealed-record journal, when applicable
```

SQLite may create its validated lock, WAL, shared-memory, and journal sidecars beside each
database. The asset store creates private identity, lock, object, and recovery entries below
`sessions/assets/`. `--catalog <directory>` explicitly overrides only the desktop's default
`packs/` catalog; it does not relocate the production Workflow/Oral provider or record-access
database. Test-only injected paths are not a production data-location contract.

The default pack catalog, databases, assets, and settings live outside the extracted application
prefix. Removing an extracted program directory therefore does not remove default user state. Back
up state only while Appellate Workbench is stopped. A raw copy of `sessions.sqlite` is not a
portable product backup when it references `sessions/assets/`; use the paired session archive
below. The lower-level database-only backup primitive accepts only asset-free state and is not a
substitute for that archive.

## Portable Workflow/Oral sessions

Use **File > Export Workflow/Oral Sessions** to create a schema-1 `.awsessions` file. Export
validates the complete database/CAS pair, every exact installed revision closure, supported engine
identities, and deterministic Workflow/Oral replay before publishing one owner-only file. It never
overwrites an existing destination. Export covers all current Workflow and oral-argument snapshots
in the production local provider and every CAS document those snapshots reference.

The portable boundary contains:

- canonical Workflow/Oral session snapshots, commands, events, docket rows, and revision pins;
- the exact bytes and digests of every referenced local session document; and
- an unkeyed digest over the bounded canonical archive envelope.

It excludes:

- installed `.awpack` revisions and the local pack catalog;
- pack-authoring directories and other source material;
- standalone fictional/composite profile JSON files;
- `record-access.sqlite`, grants, revocations, and sealed-record workspace state;
- optional sync objects, provider configuration, credentials, vault keys, and recovery capsules;
- settings, caches, logs, unreferenced CAS objects, and raw SQLite/WAL/journal files.

Use **File > Import Workflow/Oral Sessions (Create Only)** on the receiving desktop. Import is
create-only: it never overwrites or merges an existing session. The exact pinned pack closure must
already be installed in the importing desktop's local catalog. The importer accepts only a bounded
regular non-symlink file, revalidates the complete envelope and replay, requires every archived
session ID to be absent, and commits those snapshots and CAS objects as one paired operation. A
conflict or validation failure leaves pre-existing live session and CAS names unchanged; guarded
rollback or recovery may retain the private quarantine tombstones described below.

An oral session is pinned to the Workflow head that launched it. If that exact Workflow snapshot
is absent, or the current Workflow later advanced so the two current snapshots no longer prove the
binding, export refuses to create an archive. Transfer the needed `.awpack` closure separately and
install it before importing; the session archive never installs content for the operator.

The checksum is unkeyed SHA-256: it detects corruption but provides neither authentication nor
encryption. Treat every imported archive as untrusted input and every exported archive as
potentially sensitive data. Protect authenticity and confidentiality with an operator-managed
authenticated, encrypted transport or container outside Appellate Workbench.

## Pack-author CLI workflow

Use the installed `bin/appellate-pack` directly, or place that directory on `PATH`. Every command
is local and emits one compact schema-versioned JSON result. A basic create-and-admit cycle is:

```sh
appellate-pack template /path/to/new-pack
appellate-pack validate /path/to/new-pack
appellate-pack export /path/to/new-pack /path/to/new-pack.awpack
appellate-pack install /path/to/new-pack.awpack /path/to/local-catalog
appellate-pack list /path/to/local-catalog
appellate-pack validate-resolved \
  /path/to/local-catalog <pack-id> <version> <revision-digest>
```

`template` creates a complete fictional/composite schema-version-2 authoring example and refuses
an existing destination. Edit only the generated data files, then validate before export. `export`
is deterministic for identical source bytes and refuses an existing archive destination. Install
is immutable and catalog-local; use the exact identity reported by export/install for
`validate-resolved`.

When an authoring root uses exact dependency-resolved references, install its full dependency
closure first and use `export-deferred` instead of `export`. The deferred archive does not become
executable merely because it was created: install it into the catalog and run `validate-resolved`
against its exact root revision. Realism evidence and detached independent review have additional
ordered requirements; follow the [independent-review operator guide](INDEPENDENT_REVIEW.md) rather
than treating a validation result as gold, release-ready, or MVP-complete evidence.

## Fictional/composite bench profiles

The desktop's **File > Import Profile**, **Clone Profile**, editor, and **Export Profile** actions
form a standalone schema-1 JSON editing workflow. Import accepts a bounded regular non-symlink JSON
file whose closed schema says `profile_class: fictional_composite`. Clone requires a new namespaced
ID and display name. Export validates the edited profile and publishes a new file without
overwriting an existing path.

An imported or exported profile JSON file is not an installed pack revision, is not included in a
`.awsessions` file, and does not replace a profile in the currently loaded case bench. To make an
edited profile available to a simulation, a pack author must add the profile resource and bind it
to a compatible bench in a newly validated, exported, and installed immutable pack revision.

All shipped and editable MVP profiles are fictional or composite and must remain visibly labeled
that way. Compatibility is checked by court role and jurisdiction. Controls can change
deterministic question style, focus, interruption, follow-up, hypothetical, and time-management
behavior; they cannot change facts, procedural validity, deadlines, legal state, or an authored
disposition. See the [bench-profile contract](spec/BENCH_PROFILES.md) for the exact schema and
behavior boundary.

## Sealed/public record twins

Sealed-record authorization is a deterministic simulation state, not login, identity proof, DRM,
or workstation-owner access control. Before a valid in-session grant, the public projection omits
sealed rows, metadata, anchors, citations, search text, and PDF bytes. A grant is journaled in the
separate `sessions/record-access.sqlite`; revocation closes and clears the loaded sealed projection.
That journal is not part of `.awsessions` export and is not synchronized.

The `.awpack` and local content-addressed object may contain the sealed document as plaintext. A
person or malicious process with the same operating-system user authority can read or alter files
to the extent allowed by that account and filesystem; the feature does not claim otherwise. Pack
bytes are still digest-checked before an authorized open, and the viewed PDF uses a verified leased
snapshot, but those checks are integrity boundaries rather than user authentication. See the
[sealed/public record contract](spec/SEALED_RECORDS.md).

## Optional sync primitives

The repository implements versioned encrypted-envelope, identity, keyring, recovery-capsule,
session-segment/checkpoint graph, restore-planning, create-only local-folder provider, and
provider-neutral transport primitives. These library components do not make sync a desktop
feature. There is no production OS secret-store adapter, remote provider, coordinator-to-SQLite
bridge, background service, or application UI. There is also no production pack-revision or
authored-revision payload codec or application import transaction.

Do not configure the local-folder adapter as a product backup or assume that enabling the build
option uploads anything. The application never syncs a live SQLite database, WAL, journal, table,
or row mutation. Until the missing production boundaries exist and have their own operator flow,
local save/resume and `.awsessions` remain the supported session paths. The exact implemented and
pending slices are listed in the [encrypted immutable-object sync protocol](spec/SYNC.md).

## Linux private-state and recovery limits

Exact private-state enforcement is Linux-only. The production Workflow/Oral provider requires
every private directory at and below its selected boundary to be owned by the current effective
user with mode `0700`; private files must be owned by that user, be single-linked where required,
and have mode `0600`. The controller chain must be owned by the user or root and must not be
group/world-writable unless a directory is sticky. Traversal is descriptor-anchored and does not
follow symlinks. Unsafe existing paths are rejected without permission repair.

Access and default POSIX ACLs must be absent. If the filesystem cannot report ACL absence, the
provider fails closed rather than assuming the path is private. The backing filesystem must
support `O_TMPFILE` for CAS staging and descriptor-bound publication. The implementation also uses
Linux no-replace publication and descriptor-relative operations; filesystems or kernels that do
not provide the required semantics are unsupported for this local state and fail closed. Use a
local Linux filesystem with working POSIX modes, extended-attribute ACL inspection, unnamed
temporary files, durable directory `fsync`, and atomic no-replace publication.

This is cooperative same-UID protection, not a filesystem sandbox. In particular, SQLite must
reopen named database/sidecar paths, so an untrusted process running as the same user may still
race outside the cooperative lock. Do not place `<AppLocalDataLocation>` or an explicit catalog
under a same-UID-untrusted controller, shared writable tree, network filesystem, or synchronizing
folder. Stop all Appellate Workbench processes before copying or inspecting state.

Crash and race recovery moves identity-proven residue away from live names into hidden
`.appellate-quarantine-*.tmp` or `.asset-quarantine-*.tmp` entries. Quarantine tombstones are
permanent by design: the application never automatically unlinks or reuses them. They may contain
sensitive document or database bytes and consume disk space, but they are deliberately outside the
live database and CAS names. An interrupted directory publication may separately leave an inert
`.appellate-directory-stage-*.tmp` directory; it is not automatically reclaimed either. Do not
rename any residue back to a live name and do not remove it while the application is running.
Out-of-band inspection or reclamation is an administrator decision outside the application
contract; preserve a forensic copy and a verified portable backup before changing any residue.
