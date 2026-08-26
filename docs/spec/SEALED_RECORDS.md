# Sealed/public record twins

Status: MVP contract for `workbench.pack.sealed-record-twins@1`.

This boundary models access inside an offline simulation. It is not DRM and does
not claim that plaintext shipped in an `.awpack` is hidden from the workstation
owner. There are no accounts, tenants, instructors, institutions, or server
identity. The installed pack revision remains immutable and the local SQLite
session journal is authoritative.

## Declarative record policy

A schema-v2 record opts in by declaring both `disclosure_policy` and
`sealed_disclosures`. A pre-feature schema-v2 record, and every schema-v1
record, retains its existing bytes and behavior.

The policy values are intentionally closed:

- `unauthorized_projection`: `public_counterparts_only`
- `authorized_projection`: `public_and_authorized_sealed`
- `sealed_asset_access`: `session_event_grant_required`

Each disclosure has a public, non-sensitive `disclosure_id` and privately
binds one sealed entry to its policy authority and, when available, a
public/redacted counterpart, public motion, and public certificate.
`required_items` produces typed deficiencies keyed only by `disclosure_id` for a missing
`motion`, `certificate`, or `redacted_counterpart`; missing required material
is a simulated workflow state, not malformed JSON. It is operative: a grant
for that disclosure is rejected until every declared requirement resolves to
its public support entry.

Twin anchor mappings bind a new stable logical anchor to one physical anchor in
each counterpart. Stable IDs and physical mappings are record-unique. A
physical anchor must belong to the declared side of the pair, and counterparts
must be one-to-one entries in the same docket. Without authorization, a stable
anchor resolves only to the public physical anchor. With authorization, it
resolves to the sealed physical anchor. Raw sealed anchor and citation IDs are
not placed in the unauthorized projection.

## Session access journal

`record.access.granted` and `record.access.revoked` are stored in the existing
SQLite `event_log` under the canonical-v2 session authority contract. A
dedicated record-access session is pinned to the exact resolved pack closure;
it does not share or synchronize the live SQLite file.

Each strict JSON event binds:

- event, session, record, policy, sealed-document, and authority IDs;
- the exact canonical `recorded_at_utc` value;
- the global stored event sequence;
- the preceding access-event digest; and
- a domain-separated SHA-256 digest of all preceding fields and the action.

Replay compares the payload with the `event_log` type, sequence, and authority,
checks the hash chain, checks the target authority against the exact policy,
and rejects redundant grants or revocations. The matching command row is also
reconstructed byte-for-byte. A branch or prefix is projected by replaying its
own ordered journal; an unauthorized branch cannot inherit a grant merely
because another branch contains one.

The journal is capped at 4,096 events. Projection, reopen, and append all
enforce the same bound. Desktop callers address transitions only by public
`disclosure_id`; the controller resolves the sealed target internally and
returns no authorization value. It applies only its current replayed head to a
sealed workspace target. Historical prefixes use a distinct audit-only type,
so neither a saved grant nor a stale prefix can authorize a fresh workspace
after revocation. Before each transition or workspace application, the
controller reloads and exactly replays the SQLite head; a second local
controller's later revoke therefore supersedes an older in-memory grant.

## Disclosure projection

Before a valid grant, sealed rows are absent rather than disabled. The docket
model and its accessible text/search corpus therefore contain no sealed title,
actor, description, tag, document metadata, page count, citation label, raw
anchor ID, snippet, thumbnail, or PDF text. Only public/redacted bytes may be
opened or searched.

After a valid grant, the sealed row and PDF search are available for that one
document. Revocation synchronously closes the loaded sealed PDF, clears its
search model and page controls, removes sealed rows/metadata/anchors/citations,
and restores stable anchors to their public counterparts. Direct docket opens,
selection/Return, stable and raw anchors, citation navigation, and page
navigation all use the same projected document table and fail closed.

The installed-record bridge verifies and opens public assets while constructing
the public workspace. For an opted-in sealed disclosure, it retains only a
deferred resolver capability inside the workspace: immutable root revision,
portable asset path, expected descriptor data, page count, and the installed
catalog root. It holds no controller, resolved-pack, or catalog pointer. It
does not materialize, hash, page-count, open, index, or return the sealed local
path during public load.

The first authorized open resolves that exact sealed asset from the offline
content-addressed store, verifies its path, size, and SHA-256, opens it as a
PDF, and compares its page count with the authored declaration. Missing,
linked, truncated, digest-mismatched, unreadable, or wrong-page-count sealed
objects therefore fail the authorized open without breaking public counterpart
browsing. Verification runs again on each subsequent authorized open;
authorization never bypasses CAS verification.

After verifying the CAS object, the resolver copies and re-hashes it into an
owner-readable leased temporary snapshot. `QPdfDocument` opens that snapshot,
and the workspace retains its owner for the lifetime of the loaded document.
Later CAS mutation therefore cannot change the active PDF between verification
and viewing. Each resolution reopens the catalog from its captured local root,
so close/reopen and offline access do not depend on authoring inputs or a
longer-lived controller object.

The desktop stores the record-access journal under its local application-data
sessions directory. Its session ID is deterministic from the exact root
revision and selected case. Create/reopen, persisted projection application,
and the first public PDF open are staged before replacing the last-good
workspace. Grant/revoke actions are labelled only with `disclosure_id`; their
event ID and canonical time come from an injectable transition provider.
