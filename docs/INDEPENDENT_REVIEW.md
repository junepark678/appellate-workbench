# Detached independent realism review

Appellate Workbench provides a two-command, offline coordination path for turning an eligible
`independent_review_pending` realism review into a separate schema-version-2 review-only pack. The
source case pack remains immutable. The commands do not install or archive a pack, mutate a
catalog, alter the subject root, contact a network service, or write a human review claim into the
handoff.

Reviewer identity, qualification, affiliation, reference, date, scores, and uncertainty are
attributable declarations. They are not a cryptographic signature or proof of reviewer identity.
Neither a prepared handoff nor a finalized review directory is, by itself, evidence that a case is
gold, release-ready, or MVP-complete. The full pack rules and catalog-recovery procedure are in the
[content-pack contract](spec/PACKS.md).

## Command boundary

Preparation has this exact command line:

```text
appellate-pack prepare-independent-review <catalog> <subject-pack-id> <subject-version> <subject-digest> <case-id> <new-handoff-directory>
```

`catalog` must already contain the exact subject revision and its exact dependency closure. The
subject must own one complete, schema-version-2, production-multi realism review for `case-id` in
state `independent_review_pending`. Its traces, evidence closure, scores, typed uncertainty, date,
and runtime prerequisites must pass strict resolved validation. Its `reviewed_on` date cannot be
later than the one current UTC calendar date captured by preparation. Preparation reads that
catalog through an immutable snapshot; it does not create, migrate, checkpoint, repair, or install
into the catalog.

The destination must be a previously absent directory. On success it contains exactly these two
regular files:

```text
handoff.json
review-declaration.template.json
```

The handoff contains the exact subject pin, source-review identity, reconstructed mechanical
evidence, and a digest associating those values. It contains no completed reviewer claim. The null
template is the only accepted declaration shape, but it is not itself associated with a handoff.

Finalization has this exact command line:

```text
appellate-pack finalize-independent-review <handoff-directory> <completed-declaration-json> <catalog> <new-pack-directory>
```

It reloads the exact subject from a fresh immutable catalog snapshot, reconstructs the payload, and
requires the handoff bytes, template hash, association digest, and completed declaration to agree.
Its destination must also be previously absent. On success the directory contains exactly:

```text
manifest.json
resources/realism-review.json
```

That directory is a deferred schema-version-2 pack with one `realism_review` resource, no blobs,
and one exact direct dependency on the subject root. The review state is
`independently_reviewed`; the source pack and its pending review are unchanged.

## Operator procedure

1. Record the exact subject pack ID, version, lowercase revision digest, and case ID. Install the
   subject's exact dependency closure into an isolated catalog if it is not already available, then
   run `validate-resolved` on the subject pin.
2. Choose a new handoff destination and run `prepare-independent-review`. Save its compact JSON
   success response with the review workpapers.
3. Transfer `handoff.json` and `review-declaration.template.json` byte-for-byte to the independent
   reviewer or coordinator. Moving or copying the complete unchanged handoff directory is allowed;
   its pathname is not part of the association. Do not reformat, rename, or edit either generated
   file.
4. Copy the template to a separate completed-declaration file and replace every null declaration
   value, except that `reviewer.affiliation` may remain null. Do not add fields. The completed file
   may use ordinary JSON whitespace, but duplicate keys, invalid Unicode, unknown fields, and
   trailing data are rejected.
5. Return the completed declaration alongside the unchanged handoff directory. The finalizing
   operator should verify the declared reviewer identity and supporting reference through the
   review process appropriate to their organization; the command does not perform that identity
   verification.
6. Against a catalog that still contains the exact subject revision, choose a new pack destination
   and run `finalize-independent-review`. Preserve its compact JSON success response, especially the
   detached pack ID, version, revision digest, review SHA-256, and subject dependency pin.
7. Archive, install, and resolve-check the finalized directory as separate operations. The
   prepare/finalize commands perform none of these steps:

   ```text
   appellate-pack export-deferred <new-pack-directory> <new-awpack>
   appellate-pack install <new-awpack> <verification-catalog>
   appellate-pack validate-resolved <verification-catalog> <review-pack-id> <review-pack-version> <review-pack-digest>
   ```

   Install the subject's exact dependency closure in `verification-catalog` before installing the
   detached archive. `validate-resolved` must then validate the detached root and its complete
   closure.

## Completing the declaration

The completed declaration has the generated template's exact closed shape. The operator supplies:

- `handoff_digest`: the exact digest reported by preparation;
- `review_pack_id`: a 3–128-byte schema-version-2 namespaced ID that differs from every pack ID in
  the subject closure;
- `review_pack_version`: a valid 5–128-byte schema-version-2 pack version;
- `review_resource_id`: a 3–128-byte ASCII ID accepted by both the common namespaced-ID and manifest
  content-ID rules and distinct from every resource ID in the subject closure;
- `review_state`: exactly `independently_reviewed`;
- `reviewed_on`: a canonical `YYYY-MM-DD` date no earlier than the source review and no later than
  the current UTC calendar date when finalization runs;
- `reviewer_reference`: trimmed, nonempty text of at most 512 UTF-8 bytes referring to the review
  workpapers or other operator-controlled evidence;
- `reviewer`: a namespaced `reviewer_id`, nonempty `display_name` and `qualification`, and either a
  nonempty `affiliation` or JSON null;
- all seven dimension scores as integers from 0 through 3; and
- `known_uncertainty`: zero through 256 unique typed uncertainty objects in declaration order.

All decoded keys and strings must be valid Unicode scalar sequences. `reviewer_id` is a common
namespaced ID of 3–160 UTF-8 bytes. `display_name` is trim-stable, nonempty text of at most 240
bytes; `qualification` has the same rule with a 1,024-byte ceiling; and a string `affiliation` has
the same rule with a 240-byte ceiling. A null affiliation is omitted from the finalized reviewer
object.

The seven dimension keys are `bench_differentiation`, `consequences`, `deadlines_authority`,
`oral_argument`, `procedural_law`, `provenance`, and `record_consistency`. A score of zero clears
that dimension's final evidence references. A nonzero score retains the complete reconstructed
mechanical partition and is invalid when no such partition exists. A nonzero oral-argument score
also requires a case-targeted argument configuration; a nonzero bench-differentiation score
requires the subject's referenced bench configuration and judge profile.

Every uncertainty contains exactly `uncertainty_id`, `summary`, and `blocking`.
`uncertainty_id` is a unique common namespaced ID of 3–160 UTF-8 bytes. `summary` is trim-stable,
nonempty text of at most 2,048 UTF-8 bytes. A blocking item also contains `remediation_issue`, which
must pass the shared canonical-authority-source URL predicate: lowercase HTTPS DNS, no user info,
port, or fragment, printable ASCII path/query bytes, uppercase percent escapes, and a 2,048-byte
ceiling. A nonblocking item must not contain `remediation_issue`. For example:

```json
[
    {
        "blocking": true,
        "remediation_issue": "https://reviews.example.org/issues/independent-001",
        "summary": "Resolve the identified record-grounding question.",
        "uncertainty_id": "review.uncertainty.record-grounding"
    },
    {
        "blocking": false,
        "summary": "Document the remaining nonblocking limitation.",
        "uncertainty_id": "review.uncertainty.documented-limitation"
    }
]
```

The generated template remains authoritative for field placement. Evidence, traces, dependencies,
capabilities, resource hashes, manifest fields, and alternate engine revisions do not belong in the
declaration. Pack and resource identities are deliberate coordinator inputs; they are not derived
from the reviewer name, date, or subject identity.

## Integrity and publication behavior

Every generated JSON file is deterministic UTF-8 JSON with one final LF. Generated handoff and
template inputs must remain byte-identical to that canonical form. Finalization does not trust the
handoff: it recomputes the handoff digest and null-template hash, reloads the exact subject, rebuilds
all mechanical evidence, replays every trace, and validates the complete detached resolved graph
before publication.

Both commands validate bounded, losslessly encoded native paths before opening operands or staging
output. Input members are read as stable, single-link, no-follow regular files. A destination cannot
equal or sit below the catalog, its archive/blob namespaces, or, for finalization, the handoff
directory. Existing destinations are never overwritten, and a failed destination is never retried
or repaired in place.

Publication uses a private sibling staging directory under a retained destination-parent
descriptor, a nonblocking cooperative parent lease, exact current-user ownership, exact directory
and file modes 0700/0600, checked cleanup, an atomic no-replace rename, and parent-directory fsync.
Success is reported only after the published directory and both members are rebound and verified.
Destination paths are intentionally omitted from deterministic success JSON.

## Errors and recovery

Each invocation emits exactly one compact schema-version-1 JSON object followed by LF. Success goes
to stdout with exit code 0 and empty stderr. Errors go to stderr with empty stdout and contain
exactly `code`, `command`, `message`, `schema_version`, and `status`.

- Exit 2, `invalid_arguments`, covers wrong arity, empty operands, and invalid native path spelling.
- Exit 3 covers invalid review sources, handoffs, declarations, and detached review packs.
- Exit 4 covers catalog, platform, destination, publication, and durability failures.

On an ordinary pre-publication failure, checked cleanup removes only objects created by that
attempt. An identity-ambiguous, outcome-uncertain, durability, or cleanup failure may deliberately
preserve a staging or destination tree. Preserve the response and every observed object; do not
delete or overwrite an artifact whose identity is not proven. Stop competing publishers, retain a
forensic copy, inspect the reported reachability/residue/fsync telemetry, and choose a new absent
destination only after the old outcome is established. Catalog artifacts use the separate ordered
manual-recovery procedure in the [content-pack contract](spec/PACKS.md); there is no automatic
catalog repair or persistent-lock eviction.

Handoffs and declarations are closed command artifacts, not pack resources, so this workflow adds
no installable schema, executable, private header, declaration, handoff, or detached test pack to a
release bundle. It provides neither a cryptographic declaration signature nor a persisted
provenance capability.
