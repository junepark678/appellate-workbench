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

IDs are globally namespaced lowercase tokens. Schema-version-1 pack and dependency versions
follow strict SemVer exactly as originally shipped. Schema version 2 accepts either that same
strict SemVer or an exact Gregorian `YYYY.MM.DD` date with a four-digit year from 2000 through
9999 and two-digit month and day; calendar versions have no prerelease or build suffix. Because
the alternatives are a union, a string in their lexical overlap is valid if it is valid under
either contract. Gregorian validation applies only to the exact zero-padded calendar shape:
`2026.12.11` and `1999.12.31` remain valid SemVer, and even the impossible date `2026.2.31`
remains valid SemVer. Version text is never normalized. Digests are lowercase SHA-256 hex.
Manifest content order does not confer behavior; canonical digest computation sorts entries by
content ID and blobs by descriptor fields, then hashes length-framed identity, kind, normalized
path, size, media type, and digest fields.

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

### Exact realism evidence

Every new schema-version-2 realism review uses the fail-closed shape owned by
`workbench.pack.realism-evidence` version 1, declares that capability, and includes the complete
evidence object. Removing the evidence, reviewer shape, or typed uncertainties cannot downgrade a
review into legacy semantics. Version-1 bytes and digest behavior are unchanged. The reader has a
narrow exact-revision allowlist for already pinned schema-version-2 compatibility fixtures; the
exception does not extend to a repacked or newly authored revision.

An evidence-bearing review binds one case and contains:

- every pack record in the case-owning pack's exact transitive dependency closure;
- every non-review resource descriptor and every blob descriptor in that closure;
- one or more executed workflow traces, record checks, and exact authority IDs; and
- a unique evidence ID for each resource, blob, trace, record check, and authority binding, plus
  evidence-ID references for every realism dimension.

A nonzero dimension has at least one resolving evidence reference; a zero dimension has none.
Evidence IDs are unique across all five binding categories. Trace IDs, record-check IDs, and
bound authority IDs are also unique. Typed uncertainties have stable IDs and explicit blocking
state. A blocking item must carry an HTTPS `remediation_issue`; prose alone does not clear the
gate.

The case-evidence closure deliberately excludes every `realism_review` resource at every depth,
so changing only review prose or reviewer metadata cannot create a hash self-reference. It still
binds each included pack's ID, version, manifest schema version, sorted required capability
ID/version pairs, and sorted direct dependency ID/version pairs. It does not bind a pack's
ordinary revision digest because that digest includes the excluded review bytes. After the pack
records, it binds exact resource tuples
`(evidence ID, owner pack ID/version, resource ID/kind/schema, path, SHA-256)` and blob tuples
`(evidence ID, owner pack ID/version, path, media type, byte size, SHA-256)`. Resource bindings
sort by the owner/descriptor fields shown and then evidence ID; blob bindings sort by their
owner/descriptor fields and then evidence ID. Each framed binding writes its evidence ID first,
followed by those descriptor fields.

All realism digests use SHA-256. Strings and byte strings are framed by an unsigned 64-bit
big-endian byte length followed by the exact UTF-8/byte payload; list counts use the same unsigned
64-bit representation. The closure domain is
`appellate-workbench-case-evidence-closure-v1`, followed by the case ID, sorted pack records,
sorted resource tuples, and sorted blob tuples. Resource/blob evidence IDs are part of this
digest, while the enclosing review resource descriptor is not.

An executed trace declares `engine_revision`, `command_count`, `event_count`, the SHA-256 of its
canonical journal, the immutable replay journal itself, ordered event `operation_ids`, and
`terminal_stage_id`. Each journal entry stores the canonical workflow-command bytes and its
canonical workflow-event bytes as base64. The loader decodes those bytes, rejects noncanonical
encodings, replays the command/event batches against the exact reviewed case and workflow, and
derives the counts, operation list, journal digest, and terminal stage from that replay. Its
evidence digest uses domain `appellate-workbench-executed-trace-evidence-v1` and binds, in order,
the case ID, evidence ID, trace ID, workflow ID, engine revision, both counts, journal SHA-256,
ordered operation IDs, and terminal stage.

The journal SHA-256 itself uses domain
`appellate-workbench-executed-workflow-journal-v1`, then the command count, then for each journal
entry the canonical `encodeWorkflowCommand` bytes, that entry's event count, and each canonical
`encodeWorkflowEvent` byte sequence in order. The gold replay fixture demonstrates the same
artifact boundary in `tests/fixtures/realism-evidence/gold-canonical-trace.json` and
`tests/integration/tst_gold_case_trace.cpp`.

Record-check digests use domain `appellate-workbench-record-check-evidence-v1` and bind the case
ID, evidence ID, check ID, record ID, check kind, and exact record descriptor. An
`asset_resolution` check additionally binds the ordered record-entry asset set as sorted exact
blob descriptors; a `page_anchor_resolution` check binds the record descriptor containing the
anchors. Authority evidence IDs must resolve to exact authority-set entries in the subject
closure.

Self and pending review evidence must live in the same exact pack as the case and cannot assign
score 3. An `independently_reviewed` score may live only in a detached, metadata-complete review
pack containing review resources and no blobs. That pack must have a direct dependency pin on
the case owner's exact ID, version, and revision digest. Its closure digest still covers the
reviewed case pack and that pack's transitive dependencies, not unrelated review-pack content.
Reviewer identity, qualification, affiliation, reference, and date are declared attributable
metadata; this version of the contract does not provide a cryptographic reviewer signature or
identity proof.

For developer authoring, `author-realism-evidence` completes an existing, manifest-declared
schema-version-2 review scaffold from an executed canonical trace and an exact local dependency
catalog. The command preserves the scaffold's review state, dimension scores, uncertainties, and
reviewer metadata as exact JSON values. It cannot create an independent-review
claim or substantiate a score above 1. A single workflow journal is not treated as machine proof of
adverse branches, oral or bench behavior, deadlines, or consequences at realism level 2. Manual
and independently authored reviews retain the ordinary score-2/3 contract when they do not claim
this helper's engine profile. Resource and blob bindings cover the complete review-excluded case
closure. Authority bindings are the exact union of canonical authorities actually referenced by
the selected case, selected workflow operations, record and case-specific oral resources, plus
only filing-catalog entries whose filing types occur in that workflow's routes. Each dimension
uses its relevant authority partition, and every authority remains within the selected procedure's
exact authority-set scope.

The trace input supplies `evidence_id`, `trace_id`, `workflow_id`, and the canonical command/event
`journal`. The command inserts the code-owned engine revision
`appellate.realism-evidence.codec-replay.v1`; if the input supplies `engine_revision`, it must equal
that value. The trace may also supply `command_count`, `event_count`,
`journal_sha256`, `operation_ids`, `terminal_stage_id`, and `digest`; omitted derived fields are
computed from the canonical bytes and replay, while a supplied stale value fails closed. The
command also derives closure metadata, deterministic resource/blob/check/authority evidence IDs,
both record checks, and all dimension references. Normal resolved validation recomputes those
helper-profile resource/blob evidence IDs and record-check IDs; changing an ID and repairing its
references or dependent digest does not turn it into valid helper evidence. Repeating the command
against unchanged inputs produces identical review and manifest bytes.

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

A schema-version-2 route may instead opt into `authorized_role_scope: "catalog_subset"` under
`workbench.pack.route-role-subsets` version 1. The route's authorized-role list must then be a
nonempty subset of the filing catalog's eligible roles. Omitting the field preserves exact role
equality. Required fields remain exact-equal in both modes; this capability does not relax field,
service-role, procedure-role, or operation authorization checks.

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

### Structured dispositions and bounded preconditions

Schema-version-2 cases may opt into a structured authored disposition. The case declares stable
issue/target pairs, one or more disposition plans, the authored plan ID, and the existing authored
judgment-operation ID. Each plan has final or nonfinal status and at most 32 non-overlapping
components. A component binds exactly one issue/target pair, `whole` or `part` scope, one action
from `affirm`, `reverse`, `vacate`, `dismiss`, `grant`, or `deny`, an explicit remand flag, and
bounded nonempty authority and record-anchor sets. Remand is valid only with reverse, vacate,
dismiss, or grant. Every target, authority, and anchor must resolve through the case's declared
issue graph. Target IDs are case-global and belong to exactly one issue; duplicate or overlapping
targets and contradictory components fail closed.

The plan digest is SHA-256 over a domain-separated, unsigned-64-bit big-endian length-framed
canonical form. Its domain is exactly `appellate-workbench-disposition-plan-v1`; it binds the
owning case ID, authored judgment-operation ID, plan ID, finality, and every component. Components
are sorted by issue and target ID, while authority and record-anchor IDs are sorted within each
component. Pack reading, runtime projection, and the workflow engine independently recompute the
same digest. A judgment command selects the authored plan ID; it cannot substitute free-form
authoritative outcome text. The resulting event snapshots the complete plan and the workflow
state retains that plan through mandate and reopen. Legacy cases without a plan retain their
existing authored-text behavior.

Version-2 workflow operations may declare one to 32 all-of preconditions. The closed forms are:

- filing type present or absent;
- exact order ID with granted, denied, or other disposition;
- exact deadline ID with open, satisfied, elapsed, not-elapsed, or reached condition;
- argument scheduled or not scheduled; and
- judgment issued or not issued.

Filing types must belong to a declared route. Order IDs are runtime-created and therefore receive
syntax checks rather than static-inventory resolution; a missing runtime order makes its guard
unmet. A deadline ID used by a guard must instead resolve statically to an exact named output or
accepted-filing deadline producer, and a missing runtime instance makes the guard unmet. Deadline
status and deadline elapsedness are separate axes, so an open deadline may also be elapsed, while
open+satisfied and elapsed+not-elapsed are contradictions.
Every guard is evaluated against command-start state and the command's explicit court date. An
otherwise valid command with an unmet guard is rejected without mutation. Each emitted event
snapshots the operation's exact precondition vector, so replay rejects a changed definition even
when the changed predicates would still evaluate true.

These optional fields are capability-owned. A pack using a structured plan must declare
`workbench.pack.structured-disposition` version 1; a pack using operation preconditions must
declare `workbench.pack.workflow-preconditions` version 1. Declaring either capability without
using its feature is allowed as a supported lower bound, but using a feature without its
capability fails at reader, resolved-catalog, and runtime boundaries. Existing version-2 packs
without the fields remain valid.

Dependent deadlines are a separate schema-version-2 feature owned by
`workbench.pack.dependent-deadlines` version 1. A `calculate_deadline` operation may name
`deadline_base_id`; the engine then uses the exact stored due date of that existing deadline as
the new rule's base date, not the command date. A `reached` deadline guard is true when the
command court date is greater than or equal to the stored due date, while `elapsed` remains
strictly greater than that date. Either `deadline_base_id` or a `reached` guard activates the
capability requirement independently of `workbench.pack.workflow-preconditions`. Declaring the
capability without using the feature is allowed; using either feature without declaring it fails
at reader, resolved-catalog, and runtime boundaries.

Named deadline outputs are owned by `workbench.pack.named-deadlines` version 1. A schema-version-2
`calculate_deadline` operation may declare one `produced_deadline_id`; that ID is globally unique
among named outputs and route-produced deadline IDs, and an explicit calculation command must use
it exactly. An unnamed calculation cannot claim a named output reserved by another operation.
Every `deadline_base_id`, deadline-status precondition, and `satisfies_deadline_id` must resolve to
an exact producer: a named output or an accepted-filing deadline plan. A base-bearing calculation
must itself have a named output, so it requires both the named- and dependent-deadline
capabilities. Filing-route accepted and deficiency deadline plans remain limited to independent,
unnamed calculations; dependent and named calculations are explicit court commands. An accepted
deadline ID is an exact route reservation, while a deficiency plan reserves every ID in its
`deadline_id.*` namespace. Named outputs, accepted IDs, and deficiency namespaces must not
overlap, including nested deficiency prefixes. A direct calculation cannot claim any route
reservation, regardless of operation, even when it uses the route plan's operation. Automatic
route emission may claim one only through the exact plan operation and, for a deficiency, the
exact filing command-derived ID; the operation may retain authorized roles for compatibility with
explicit calculations that use unreserved IDs.

Event-date deadline bases are owned by `workbench.pack.event-date-deadlines` version 1. A named
calculation may use `deadline_event_base` instead of `deadline_base_id`. The closed forms are the
recorded judgment occurrence and an entered order selected by both exact `order_id` and exact
originating `operation_id`. The engine uses the replay-derived occurrence date, even when the
calculation command is entered later. An order selector must name an `enter_order` operation and
must match the operation and time stored with that exact order record.

Argument-date guards are owned by `workbench.pack.argument-date-guards` version 1. The
schema-version-2 `argument_date_status: reached` precondition requires a scheduled argument and is
true only when the command court date is on or after that scheduled date. It conflicts with an
`argument_scheduled: false` guard and may accompany `argument_scheduled: true`.

### Exact workflow instances and record bindings

`workbench.pack.workflow-instance-preconditions` version 1 adds two schema-version-2 guard forms.
A `filing_instance` guard names `filing_type_id`, `present`, `actor_id`, `filing_id`,
`accept_operation_id`, `record_entry_id`, and `document_sha256`. A positive guard requires the
unique accepted filing with that ID to match every state-visible field. A negative guard succeeds
only when that filing ID is absent; if the ID exists with a substituted type, actor, accepting
operation, or digest, neither the positive nor negative exact guard succeeds. Distinct filing IDs
of the same type may be selected independently. A deficient submission also owns its globally
unique filing ID but is not an accepted filing instance, so it satisfies neither polarity. An
`order_instance` guard binds `order_id`, `disposition`, the entering `operation_id`,
`record_entry_id`, and `document_sha256`.

For each case using either selector, pack reading and runtime projection resolve
`record_entry_id` against that case's own docket, require an unsealed entry, and require its asset
SHA-256 to equal `document_sha256`. The entry ID is an authoring-time provenance anchor, not a
field carried by workflow commands or materialized workflow state. Operational matching therefore
uses filing ID/type/actor/accepting operation/SHA-256 or order ID/disposition/entering
operation/SHA-256. Two unsealed docket entries with the same asset SHA-256 are operationally
indistinguishable after load. The complete selector, including `record_entry_id`, is still
snapshotted in event persistence and compared exactly to the pinned definition during replay.
Accepted-filing state records learn `accept_operation_id` from the accepted event header; it is
not a new legacy event-payload field, so existing event bytes remain unchanged.

`workbench.pack.static-deficiency-deadlines` version 1 adds
`id_mode: "exact"` to a route's `deficiency_deadline`. Its `trigger_filing` binds the filing ID,
actor ID, unsealed record-entry provenance, document SHA-256, and expected court date; filing type
and stage are inherited from the route. An otherwise eligible deficient submission must match the
trigger's filing ID, actor, SHA-256, and authoritative command-header `court_date` before either
the deficiency or deadline event is emitted. A mismatch cannot reserve the exact ID. The exact
deadline is a normal producer visible to ordinary deadline-status guards. A route may use that
same exact ID as its own `satisfies_deadline_id`: the matching deficient trigger creates it, while
a later conforming submission must find it open and name the unique matching
`cures_deficiency_id` before it may both cure the deficiency and satisfy the deadline. An
initially conforming submission cannot create the deadline.

Omitting `id_mode` preserves the existing dynamic `deadline_id.command_id` namespace. Static
exact IDs have ordinary exact-ID uniqueness: `x.y` and `x.y.z` may coexist as two exact IDs. A
dynamic prefix reserves its boundary-aware namespace, so prefix `x.y` conflicts with exact
`x.y` and `x.y.z`, but not exact `x.yz`, independent of declaration order. Exact deficiencies and
their deadlines are validated bidirectionally on replay and on caller-supplied completed state;
an orphan deadline, duplicated trigger filing, or borrowed cure-deadline ID is invalid.

`workbench.pack.operation-document-bindings` version 1 adds an optional `document_binding` to
`enter_order`, `issue_judgment`, and `issue_mandate`. Every binding names an exact unsealed
`record_entry_id`, its SHA-256, and `expected_court_date`; pack reading requires that date to equal
the docket entry's `filed_on`. An `enter_order` binding additionally requires the operation's
`order_id` and `disposition` semantics, while judgment and mandate bindings forbid those fields.
The engine compares the command's document digest, authoritative header `court_date`, and, for an
order, ID and disposition before creating an event. The capability also allows
`expected_argument_date` only on `schedule_argument`, and the engine compares it to the command's
explicit argument date.

These bindings establish exact document/date identity; they do not prove a timezone, instant, or
inferred finality. `LegalTime.court_date` is the authoritative local court date. A bound historical
`enter_order` may feed the existing `order_occurred` deadline base without inventing a new kind of
finality. Deficiency or calendar notices may be modeled as companion bound orders. Opinion
provenance remains a separate existing check. Materialized state carries enough provenance to
revalidate accepted filings, entered orders, and static exact deficiencies, but it intentionally
does not add source-operation fields for scheduled arguments, judgments, or mandates; those
bindings are enforced at command decision and exact journal replay.

Persistence schema version 3 is reserved for a structured judgment command/event or an event
with a nonempty precondition snapshot. Schema-3 events require canonical authority provenance and
always carry the precondition array, including an empty array on a structured judgment with no
guards. Legacy schema-1 and canonical-authority schema-2 bytes remain unchanged. Relabeling,
mixing disposition forms, omitting the schema-3 snapshot, or using schema 3 for an old form fails
closed.

Persistence schema version 4 is the extended workflow-event form. It is selected when an event
snapshot contains `reached` or `argument_date_status`, or when a `deadline.calculated` event binds
a named, dependent, or event-date output. Every schema-4 event requires canonical authority and an
explicit precondition array. A schema-4 deadline event additionally carries a required non-null
`produced_deadline_id`, plus explicit nullable `deadline_base_id` and `deadline_event_base` fields;
the produced ID must equal the event deadline ID and the two base forms are mutually exclusive.
Replay compares all three fields to the pinned operation definition and verifies the exact stored
base occurrence. Schema-4 non-deadline events carry no deadline-binding keys. Schema 3 rejects the
new guard forms, and relabeling between schema 3 and 4 fails closed. Existing schema-1, schema-2,
and schema-3 bytes and behavior remain unchanged.

Persistence schema version 5 is selected if and only if an event header snapshots at least one
`filing_instance` or `order_instance` guard, including when schema-4 deadline or reached-date
features are present in the same event. It carries the complete instance selectors and otherwise
retains the applicable extended fields. Decode re-derives the required version from the semantic
payload, so upgrading an older event label to 5 or downgrading a schema-5 event to 1 through 4
fails closed. Canonical schemas 1 through 4 and their byte encodings are unchanged.

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
`workbench.pack.voice-style` version 2. Structured dispositions and workflow preconditions require
their version-1 feature capabilities described above. Named deadlines, dependent deadlines,
event-date deadline bases, and argument-date guards each require their independently negotiated
version-1 capability described above. Role subsets, exact workflow-instance guards, static exact
deficiency deadlines, and operation document/date bindings likewise require their independently
negotiated version-1 capabilities and schema-version-2 resources. Authored grounded questions and
exact realism evidence likewise require `workbench.pack.grounded-questions` version 1 and
`workbench.pack.realism-evidence` version 1, respectively. An empty list, an unrelated-only
declaration, or a declaration missing any required capability fails at pack read,
resolved-catalog load, and independent runtime projection. Unknown IDs, unsupported versions,
and capabilities declared for the wrong manifest generation fail before runtime projection.
Unknown resource kinds, unsupported kind/version pairs, operation codes, schema versions, and
mixed-version manifest/resource sets also fail closed.

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
appellate-pack author-realism-evidence <directory> <catalog> <review-resource-id> <trace-json>
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

`author-realism-evidence` requires every exact dependency to be installed in `catalog`; it never
installs the root or dependencies. It precomputes and validates the final review, manifest, replay,
and exact resolved graph before changing source files. A catalog row with the same root pack ID and
version is accepted only when its digest equals the prospective final digest. Trace, source, and
transaction inputs use bounded no-follow regular-file reads. The canonical root is held by a
directory descriptor; root members are traversed component-by-component without following links,
and publication targets descriptor-relative directories. Renaming or replacing the named root
or its named parent fails the retained device/inode identity checks, including a final check after
transaction cleanup and immediately before success. A private sibling lock with finite stale-lock
recovery serializes cooperating invocations for the same root; byte-stamp checks detect observed
edits by other tools, but this is not an absolute concurrency guarantee against noncooperating
processes.

The two-file update is deliberately not described as atomic. Authored and original bytes plus a
digest journal are fsynced in a private sibling transaction directory on the same filesystem; no
temporary member is placed inside the pack root. The transaction directory is created and opened
relative to the anchored parent, its device/inode identity is retained, and every member read,
write, inspection, enumeration, removal, and publication stays relative to that descriptor. The
named sibling must still identify that exact directory before publication or cleanup. The review
is atomically replaced first and `manifest.json` last; a target whose old and new bytes are equal is
not replaced. An interruption between distinct replacements leaves a fail-closed digest mismatch.
Under the sibling lock, rerun accepts only the journal's exact old/old, new/old, or new/new byte
states. Before a new/old recovery can publish the manifest, its journal root ID, version, and final
digest are checked against any installed immutable root. Recovery then completes and strictly
verifies the declared final state before removing the journal. An unrecognized target state is
never rolled back or overwritten. Ordinary rollback is likewise attempted only while both
observed target digests equal a declared transaction state. Final success is reported only after a
full reread and exact catalog resolution.
Success JSON includes the final pack revision, review ID/path/SHA-256, case and closure digest,
deterministic evidence counts, and `updated` (`false` for an idempotent rerun).
