# Ellison v1.2.0 validation scaffold

Evidence status as of **2026-08-12**: all 72 synthetic sources are frozen; the fresh
production render and schema-v2 record are accepted; and seven non-workflow core resources
have been copied byte-for-byte into the pack candidate and pass schema and resolved-reference
checks. Workflow authoring, canonical replay traces, manifest closure, promoted realism-review
evidence, root revision, deterministic archive, resolved installation, and focused release tests
remain pending.

## Evidence pins

| Evidence item | Current value | Status |
| --- | --- | --- |
| Path-framed source closure | `0a33de749c1c6118d87bf3c0f550afcbd82f26fe3e5a2b38d5b8d183ecada3c3` | Frozen: 72 sources / 449 logical pages |
| Successor document-plan SHA-256 | `e7293dc1350ce66b63b7c70afc2ee088d55906b307cc85e90dcd663b5635d7a0` | Frozen planning/allocation contract |
| Render-plan SHA-256 | `f68262e843d8af527a77e8ecf7d6b2e14cf83933e3e1808caa50bc82c0bd995d` | Frozen: schema v1 / 72 rows |
| Accepted checked-in render-inventory SHA-256 | `dffa33a8607a08c3ef3f8b5af5c15d505255d850a58fad7dc04bc7162aff3f14` | Frozen: 72 PDFs / 449 physical pages |
| Renderer-emitted raw inventory SHA-256 | `bbda5e5c20944e2e4d7adbe7ffbcde382906c5555bbeae31b307dacfa48e6710` | Same JSON value as accepted inventory; byte formatting differs |
| Canonical compact inventory SHA-256 | `1e99b6d59f88ba8edd85b6d1df72e88653b555559a63496f09d9509c8f132b22` | Both inventories after `jq -S -c` canonicalization |
| Record-resource SHA-256 | `3269cf7be84b0e1545f17d19876a64e86930d44259bb13d543d53ddb12bd199f` | Frozen: 3 dockets / 72 entries / 449 anchors |
| Ordered raw PDF corpus SHA-256 | `9e1ce3a92c61b1eb4240e97b49de83530c2e37dd38f2526ddd261d7edebfdc23` | Frozen: accepted inventory output-path order |
| Framed PDF corpus SHA-256 | `277c650a0d1263acb972d1cb86c7be242f3a80d5c134c11eb1e85a61abe36afa` | Frozen |
| PDF corpus size | `2,763,463` bytes | Frozen: 72 accepted PDFs |
| Actual argument resource SHA-256 | `2abba8688906f2d6c248028ba56b49a83e88514757e5470502d10f5e485203d7` | Frozen: 12 grounded questions |
| Counterfactual argument resource SHA-256 | `5baff068b6b38dd1dace80f47e518aa3cda5f64ae534ca838afd4fffa9fdb550` | Frozen: 12 grounded questions |
| Authority-set resource SHA-256 | `9acda100f645562809f050bc338dbba04d81d8bfaa88da6820d3ef4a5d818591` | Frozen: 11 authority entries |
| Bench-configuration resource SHA-256 | `829c7e0ae4721bbd246dbb12987f014ddbc836b7be0e32a825d204a396b155a6` | Frozen: three seats |
| Case resource SHA-256 | `e29d11e570d49b058a6dc57c020e433dc4210638fbd3434d3aacb40467f91822` | Frozen: three issues / two asymmetric disposition plans; former district defendant has a distinct non-service role |
| Procedure-profile resource SHA-256 | `050d5fe439316267c79eeecff9528ede7f6abf7272c8004e51682baa608190ba` | Frozen core resource; declares the former-district-defendant role separately from the active appellee |
| Actual question-bank grounding digest | `5b5d06119598daa1ff883de75642f3476aff4d5b47b56c5d562a2e3f2314152e` | Independently recomputed from frozen authority/record bytes |
| Counterfactual question-bank grounding digest | `cf7097c563a40f40feac132943f3f231efa6c1fb1ade0e3b7e433e12f1a38849` | Independently recomputed from frozen authority/record bytes |
| Actual disposition digest | `d98cbd88077bb3415931f324c5fdca0e714a62bb3894279e8e5b269ccd66823c` | Independently recomputed from the three-component actual plan |
| Counterfactual disposition digest | `6f1a4ce2280a21f41f085c72faa36fb4e72dcee09ea104d4867822bf1754b793` | Independently recomputed from the two-component adverse plan |
| Realism-review authoring scaffold SHA-256 | `ad4b08d350f5cf843b57970e7b5d0342cd95edda08ba2d91f575196c8786c28f` | Level 2 in all seven dimensions; `independent_review_pending`; no evidence field |
| Workflow resource SHA-256 | `PENDING_WORKFLOW_SHA256` | Pending workflow authorship and review |
| Canonical trace bundle | `PENDING_TRACE_BUNDLE_SHA256` | Pending production-engine replay |
| Manifest SHA-256 | `PENDING_MANIFEST_SHA256` | Pending closure over finalized resources and blobs |
| Promoted realism-review SHA-256 | `PENDING_REALISM_REVIEW_SHA256` | Pending exact evidence and independent-review status recording |
| Exact evidence-closure digest | `PENDING_EVIDENCE_CLOSURE_SHA256` | Pending workflow, trace, record-check, and authority closure |
| Root revision | `PENDING_ROOT_REVISION` | Pending manifest finalization |
| Deterministic archive | `PENDING_ARCHIVE_SHA256` | Pending two-export byte comparison |
| Resolved installation | `PENDING_RESOLVED_INSTALL` | Pending exact dependency installation and resolved validation |
| Focused tests | `PENDING_RELEASE_TEST_PINS` | Pending local and `TZ=UTC` case/UI runs |

## Completed source, render, record, and core gates

The following claims are confined to the frozen source/render/record/core bytes:

1. The source allocation resolves 37 lower sources to JA1–JA275, 15 actual appellate
   sources to PA1–PA91, and 20 never-filed counterfactual sources to PA92–PA174. The
   arithmetic is 37 + 15 + 20 = 72 documents and 275 + 91 + 83 = 449 pages.
2. A fresh production render emitted exactly 72 PDFs and 449 physical pages. Every accepted
   PDF passed `qpdf --check`; inventory/output hashes, byte sizes, page counts, and JA/PA
   labels match the accepted record. The accepted corpus contains no MP4.
3. The schema-v2 record has exactly three dockets, 72 entries, and 449 unique anchors.
   JA1–JA275 and PA1–PA174 are each contiguous. Every B entry uses only the counterfactual
   docket and carries `counterfactual_appellate_branch`, `never_filed`, and
   `never_occurred_on_actual_docket` tags.
4. The 72 accepted PDFs match the second fresh render byte-for-byte. No inventory member or
   record link retains a stale first-render PDF hash.
5. The seven promoted non-workflow resources are byte-identical to their frozen candidates,
   validate against their schema-v2 resource schemas, and pass the resolved-reference audit.
6. Actual and counterfactual grounded-question digests independently recompute from the exact
   authority propositions/provenance and record asset/anchor bytes. Their document sets remain
   isolated by docket and branch.
7. Actual and counterfactual disposition digests independently recompute from the case plans.
   The three-component actual plan and two-component adverse plan preserve their intentionally
   asymmetric target coverage.

These gates establish provenance and internal consistency for the completed slice. They do not
establish workflow reachability, replay completeness, release closure, deterministic export,
resolved installability, accessibility conformance, legal correctness, or independent review.

## Pending workflow and release gates

Before promotion, exact evidence must establish all of the following:

1. a schema-valid workflow whose operation, filing-route, exact-document-binding, disposition,
   deadline, stage, actor, and branch references resolve against the frozen resources;
2. runtime reachability for every workflow operation and successful production-engine replay of
   each named actual and mutually exclusive counterfactual route;
3. actual-history exclusion of PA92–PA174 from actual operations, disposition grounding, and
   actual-record questions;
4. manifest closure over every finalized resource, blob, capability, and exact dependency, with
   no undeclared or missing member;
5. a promoted realism review with exact evidence references, while retaining
   `independent_review_pending` unless a qualified reviewer actually supplies attributable review;
6. two byte-identical deferred exports, fresh exact-dependency installation, resolved validation,
   and focused case/UI tests both locally and under `TZ=UTC`.

No placeholder above reserves a future hash or success result.

## Procedural and merits consistency gates

Final validation must preserve these authored boundaries:

- The February 3, 2025 incident is reviewed on plaintiff-favorable assumed facts for the legal
  questions. The appellate record does not authorize a factfinder's resolution of the disputed
  forearm strike, grab, flight, reach toward equipment, immediate threat, or competing accounts.
- The actual disposition dismisses the factual-sufficiency target, affirms the Fourth Amendment
  legal ruling on assumed facts, and affirms the clearly-established-law denial of qualified
  immunity. Each side bears its own appellate costs.
- The counterfactual adverse disposition dismisses the factual target, assumes without deciding
  constitutional excessiveness, and reverses only the clearly-established/qualified-immunity
  target for entry of summary judgment for Rusk. It does not falsely resolve the constitutional
  target.
- *Barricks* issued March 3, 2026 and first enters the March 16 opening brief. *Zorn* issued
  March 23, 2026 and first enters the later response/reply analysis. Neither is district-court
  authority, and neither can itself supply notice for the February 3, 2025 incident.
- Body-camera material is PDF-only and includes transcripts, frames, timing, occlusion, and
  authentication material. No operation may claim machine inspection of video or infer disputed
  movement, resistance, threat, force, or credibility.
- Ordinary mandate, rehearing, stay-denial, and stay-grant/dissolution documents are mutually
  exclusive Rule 41 paths. Their later-of calculations and shortened intervals are exact authored
  branch consequences, not a general scheduling oracle or one combined chronology.
- No authored event postdates the August 12, 2026 evidence cutoff. The stay-grant route ends with
  the August 10 abandonment/dissolution and August 11 release/mandate documents.
- Alder, Reed, and March are synthetic/composite bench profiles. Deterministic questioning does
  not model a real judge or predict an actual outcome.

## Reproduction scaffold

The completed-slice gates can be rechecked from the repository root with the repository's
schema validator, reference-audit harness, `sha256sum`, `jq`, `qpdf`, and PDF page/text tools.
The exact workflow and release commands remain intentionally absent until those artifacts exist.
The future root must use deferred export because its schema-v2 references depend on exact-pinned
foundation resources.

Expected dependency revisions are:

- `foundation.us-federal@2025.12.01` —
  `866c90996c15e2076b9508a297ffce1a4e766b1432a9e11d08e8138c57e363c9`;
- `foundation.us-ca4@2026.03.23` —
  `449d75c77e5c47883f750377450f2d1ec1fc0e42e20b1f247446b208661d3262`;
- `foundation.us-ca4-fictional-bench@1.0.0` —
  `cee0bf93309cc9ad800f215a47d734b20a9fdf5dc889f2f440e4382b942d332d`.

## Independent-review gate

Automated checks and authoring review cannot establish qualified-immunity accuracy, current
Fourth Circuit practice, practical realism, or level 3. A qualified civil-rights,
qualified-immunity, and appellate practitioner must review an exact finalized root. Any change
to the record, authority set, questions, workflow, traces, manifest, or dependency topology
invalidates earlier exact evidence.
