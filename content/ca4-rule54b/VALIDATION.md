# Authoring validation and exact blockers

Validation run on **2026-08-11** from the repository root.

## Completed checks

- `find content/ca4-rule54b -name '*.json' -print0 | xargs -0 -n1 jq empty` succeeded for every
  JSON file.
- Local validation with `fastjsonschema` 2.22.1 against the matching files in `schemas/v1/`
  passed for all 18 digest-independent resource candidates. As expected,
  `record.candidate.json` failed only because each of its nine docket entries omits the required
  `asset_sha256` property.
- A read-only authoring audit confirmed all declared Markdown sources exist, every intended page
  count matches its page-break markers (except the deliberately excluded annotation page in
  `d59`), JA ranges are continuous from JA1 through JA47, case authority and record-anchor IDs
  resolve, trace authority IDs resolve, and workflow authority copies match the canonical
  citation/source-version/proposition triples in `authority-set.json`.
- The audit covered 19 resource candidates, 22 authority objects, 10 joint-appendix segments,
  and 18 intended PDFs (17 Markdown documents plus the generated joint-appendix plan).
- No PDF, hash, manifest, archive, database entry, commit, or external filing was created.

The repository CLI validates only a complete manifest-governed pack, which this authoring tree
intentionally is not. The local per-resource schema results do not constitute pack-level
cross-reference, semantic, archive, trust-boundary, or engine validation, and no such claim is
made.

## Exact current-schema and loader blockers

1. **Record digest is required but intentionally absent.**
   `schemas/v1/record.schema.json` requires `asset_sha256` in every `docket_entries` item.
   `resources/record.candidate.json` omits that property for all nine entries because the task
   forbids SHA fields that depend on the in-progress blob-format change. Consequently, that one
   resource is expected to fail the current record schema at
   `/docket_entries/*/asset_sha256`.

2. **The current pack trust boundary requires a root manifest and blob declarations.**
   The task expressly excludes a root `manifest.json` and all manifest SHA fields. The current
   reader therefore has no `contents` declarations, `blobs` declarations, content digests, byte
   sizes, or asset digests and cannot load this directory as a pack. This is intentional.

3. **Intended assets are not blobs yet.**
   `asset_path` values name intended PDFs from `metadata/intended-pdfs.json`, but no PDF exists.
   Current cross-reference validation requires each record asset path to resolve to a declared
   blob and its digest to match. Deterministic rendering, PDF safety checks, final byte/page
   verification, and blob declarations must occur after the blob-format decision. Creating fake
   PDFs merely to satisfy the reader would be misleading and was not done.

4. **Legally accurate filing routing conflicts with current workflow semantics.**
   The workflow schema permits an empty `filing_routes` array, but the current pack reader later
   requires every catalog filing to have a route. Every route, in turn, must contain a
   `deficiency_deadline` backed by a fixed `calculate_deadline` operation, and the reader requires
   exactly one `reject_filing` operation for every stage. The sourced rules do not establish one
   uniform fixed cure period or one uniform rejection consequence for every notice, initial
   document, brief, and rehearing petition. `resources/workflow.json` therefore keeps
   `filing_routes` empty and does not invent reject operations. It should pass the JSON schema but
   is expected to fail current pack semantic validation with the reader's "every catalog filing
   requires a workflow route" and per-stage `reject_filing` requirements.

5. **Conditional deadline and branch logic is not expressible in workflow v1.**
   `calculate_deadline` stores only a fixed number of days and calendar/business-day counting. It
   has no trigger field or predicates for private versus federal parties, Rule 58 entry,
   FRAP 4(a)(4) tolling, FRAP 4(a)(5) extension, FRAP 4(a)(6) reopening, electronic versus other
   service, amended decisions, rehearing petitions, stay motions, or Rule 41's "whichever is
   later" calculation. The executable candidate contains only the authored fixed-day operations;
   `traces/expected-traces.json` preserves the missing predicates and adverse expectations.

6. **Rule 54(b) gate conditions cannot select workflow branches.**
   Workflow v1 has no condition language for ultimate disposition of an individual claim,
   express no-just-reason text, case-specific findings, or abuse-of-discretion review. It also
   cannot distinguish dismissal for no final decision from a merits judgment. The authored
   `issue_judgment` operation points to the dismissal outcome, while the supported-certification
   and sparse-findings alternatives remain in the trace evidence.

7. **Record anchors are entry-level only.**
   `case.schema.json` accepts record entry IDs but not page ranges or JA ranges. The operative
   omission is specifically at JA40; that page-level relationship lives in
   `metadata/joint-appendix.json` and the briefs, not the case resource.

8. **Argument configuration cannot say "counterfactual."**
   The authored appellate docket states that the panel decided without oral argument under FRAP
   34(a)(2). `argument-config.schema.json` has no mode/status field to distinguish a moot-court
   configuration from an argument that actually occurred. The README, opinion, docket metadata,
   and realism uncertainty consistently identify `argument-config.json` as counterfactual.

9. **Authority and realism evidence have limited structured fields.**
   The authority schema has no per-source checked-on date distinct from `source_version`, no
   precedential-status field, and no uncertainty field. The realism schema has no dimension-level
   evidence links, reviewer digest, or trace IDs. `sources/SOURCE_NOTES.md`, the nonprecedential
   qualifier in the *McPherson* citation/proposition, and `known_uncertainty` preserve that
   information without extending current schemas.

## Release gates still required

- resolve blob/manifest representation and render real deterministic PDFs from the UTF-8 sources;
- verify rendered page counts, operative-page exclusions, PDF safety, byte sizes, and hashes;
- update the record candidate with final asset digests and add a root manifest only after the
  blob format is settled;
- add schema/runtime support for sourced conditional branches or narrow the executable filing
  catalog without inventing legal deadlines;
- run full schema, cross-reference, semantic, and trace tests; and
- obtain attributable independent qualified review if any later release seeks realism level 3.

This authoring revision remains `independent_review_pending` and makes no level-3 claim.
