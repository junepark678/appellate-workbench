# Norvale v1.2.0 validation scaffold

Evidence status as of **2026-08-12**: all 73 synthetic sources are frozen; the fresh
production render and schema-v2 record are accepted; seven non-workflow core resources
have been copied byte-for-byte into the pack candidate; and the workflow is frozen after
schema, resolved, reachability, replay, and hostile review. Canonical trace, manifest,
realism-review, root, archive, and focused-test evidence are not final here. This document
deliberately records placeholders instead of projecting a release result.

## Evidence pins

| Evidence item | Current value | Status |
| --- | --- | --- |
| Path-framed source closure | `e85ec3ff6c0fefabec9b1884c1256af09ed431c0f1c19f0a0d7bafe8d1ba0780` | Frozen: 73 sources / 383 logical pages |
| Successor document-plan SHA-256 | `b950388515198f489b00857965c3db053421b811b7bd7675cf26b1ddf3100ea4` | Frozen planning/allocation contract |
| Render plan SHA-256 | `24b1a198889301f3b294d2d865479f1821a9bb0ede4e5ae8965ea09b1efed773` | Frozen: schema v1 / 73 rows |
| Accepted render-inventory SHA-256 | `12de3cd55506d0d3d11f2140381e129013995db5f615c7ead987b1e2e8786470` | Frozen: 73 PDFs / 383 physical pages |
| Renderer-emitted raw inventory SHA-256 | `5b9ed49acaab2d4f3ae9d8d3615e6c37cb3c5217309ec8858a2555e608f5e78b` | Same JSON value as accepted inventory; byte formatting differs |
| Record-resource SHA-256 | `a25bb89f96b78bbf7b084b50c4327953ed0af602359e49460dd1e10ef48306c3` | Frozen: 3 dockets / 73 entries / 383 anchors |
| Ordered raw PDF corpus SHA-256 | `09478456855e46cd3bf4a8ba6abb44a14d38761f4be7b7dc8aa0d87cb28e47f1` | Frozen: accepted inventory output-path order |
| Framed PDF corpus SHA-256 | `9f3ec6f5843e067562363883636b14867c1b9dd55447fd6744b291178d1ac448` | Frozen |
| Raw-inventory-plus-PDF closure SHA-256 | `c33d42fc122a969ffe11b9d95f7dd32ce8683330b3722a122366aa8117917034` | Frozen render-audit closure |
| PDF corpus size | `2,639,080` bytes | Frozen: 73 unique output hashes |
| Actual argument resource SHA-256 | `b99581558b2dfc53a294cf6bf357104b5eeb610fbf479aaef78e398b774200fc` | Frozen core resource |
| Counterfactual argument resource SHA-256 | `8fb8c1348b1f03afc45e8335e2f3ff84f055f875e60a247e907014ae871040f9` | Frozen core resource |
| Authority-set resource SHA-256 | `2f046241ffb7b802b54bd98b5079256da708f4f25ce2bfd770c333c5e84d713d` | Frozen: eight authorities |
| Bench-configuration resource SHA-256 | `89f52481e722dac8dddc310010fe43308fe03ce72357b0d0d67ac14f6ba9853f` | Frozen: three seats |
| Case resource SHA-256 | `2f24762e0548d6e2c62544f6bf0918ba08c472d97677271a2251ee67e0c261e1` | Frozen: four issues / two disposition plans |
| Procedure-profile resource SHA-256 | `4bb5fee6613a9a1e300d12d9a4d30248ee8478d1076d9f712e2f768bb852bdee` | Frozen core resource |
| Actual question-bank grounding digest | `a7b9d3f45093cd389d57fea1522b1c2ae80fac29bf5705dd5b24033694c6f4ea` | Recomputed from frozen authority/record bytes |
| Counterfactual question-bank grounding digest | `5b5559db07537e94046dffc733b8f3f104f09533ff9ffede190f4e1758260ecb` | Recomputed from frozen authority/record bytes |
| Workflow resource SHA-256 | `1b285f65a38c4be2a7bc8dbe29d3822aee2963d05019fbe5f79d5917272cc74a` | Frozen: 16 stages / 90 operations / 13 routes / 24 filing bindings / 31 document bindings / 2 dispositions / 14 concrete deadline IDs (12 logical concepts) |
| Workflow hostile audit | `056370216e48f8cd04e0f078e31706b18d17c228e2df9ec1b7abc176fd7d4ea2` | Clear: 90/90 operations reachable; 2,212/2,212 replay mutations rejected |
| Canonical trace pins | `PENDING_TRACE_PINS` | Nine traces planned; paths, hashes, command/event counts, coverage, and replay pending |
| Manifest SHA-256 | `PENDING_MANIFEST_SHA256` | Not yet supplied |
| Realism-review SHA-256 | `PENDING_REVIEW_SHA256` | Not yet supplied; independent review remains pending |
| Exact evidence-closure digest | `PENDING_EVIDENCE_CLOSURE_SHA256` | Not yet supplied |
| Root revision | `PENDING_ROOT_REVISION` | Not yet supplied |
| Deterministic archive | `PENDING_ARCHIVE_SIZE`; `PENDING_ARCHIVE_SHA256` | Not exported or accepted here |
| Focused tests | `PENDING_TEST_PINS` | Exact local and `TZ=UTC` targets/results not yet supplied |

The planned manifest must declare exactly 16 capabilities. That count is frozen in the
successor plan, but manifest presence, resource closure, dependency resolution, and
capability negotiation remain pending until the manifest itself is supplied.

## Completed source, render, and record gates

The following claims are confined to the frozen source/render/record bytes:

1. The source plan resolves 24 lower sources to JA1–JA149, 23 actual appellate
   sources to PA1–PA135, and 26 never-filed branch sources to PA136–PA234. The
   arithmetic is 24 + 23 + 26 = 73 documents and 149 + 135 + 99 = 383 pages. All
   73 row statuses and the top-level source status are `source_review_clear`; the
   top-level render status is `rendered_accepted`.
2. All 383 source pages meet the authoring density floor after headings and synthetic
   front matter are excluded. Duplicate substantive-paragraph and cross-page 30-word
   sequence scans are clear.
3. A fresh invocation of the production `appellate-render` utility emitted exactly 73
   PDFs and 383 physical pages. Every PDF passed `qpdf --check`, `pdfinfo` page count,
   US-Letter/unencrypted checks, searchable extracted-text checks, and exact JA/PA footer
   checks. L16 is exactly 22 physical pages.
4. Inventory source hashes, assembly provenance, plan hashes, output hashes, byte sizes,
   page counts, and labels match accepted bytes. All 73 PDF hashes are unique.
5. The schema-v2 record has exactly three dockets, 73 entries, and 383 unique anchors.
   JA1–JA149 and PA1–PA234 are each contiguous. All B entries use only the
   counterfactual docket and carry both `never_filed` and
   `never_occurred_on_actual_docket`.
6. Actual and counterfactual grounded-question digests independently recompute from the
   exact authority propositions/provenance and record asset/anchor bytes.

These gates establish provenance and internal consistency. The separately frozen workflow
establishes its stated schema, reachability, replay, and branch-isolation properties. Neither
slice establishes legal correctness, accessibility conformance, installability, deterministic
export, or independent professional review.

## Completed workflow gates and pending release gates

Before any final release statement, replace every `PENDING_*` value only from the exact
accepted artifacts and verify all of the following:

1. the frozen workflow contains exactly 16 stages, 90 operations, 13 filing routes, 24 exact
   filing bindings, 31 document bindings, two structured-disposition bindings, and 14 concrete
   deadline IDs representing 12 logical concepts; all 90 operations are runtime-reachable;
2. each of the nine named canonical journals is frozen, complete, and replays through the production
   engine, with branch coverage and command/event totals recorded rather than inferred;
3. actual-history operations, actual disposition, and actual-record questions cannot use
   PA136–PA234, while counterfactual operations remain on their separate docket;
4. all nine resources, 73 blobs, 16 capabilities, and three exact dependencies close in
   the manifest without undeclared or missing members;
5. deferred validation, dependency installation, resolved validation, two deterministic
   exports, and archive-byte comparison pass against the same root revision;
6. the final realism review binds the exact manifest, dependencies, journals, record
   checks, and selected authorities without claiming independent review; and
7. focused case/UI tests pass locally and under `TZ=UTC` against the same root and archive.

Until those steps complete, the current tree is an authoring candidate with accepted
record/render evidence, not an installable or release-ready pack.

## Procedural and merits consistency gates

Final validation must preserve these authored boundaries:

- The permit is content neutral on its face and triggered by paying speakers; free
  admission and bookselling do not eliminate the First Amendment issue, while neutral
  City traffic and staffing interests remain genuine rather than pretextually erased.
- The fairs have operated peacefully since 2019 with no violence, two minor medical
  calls, and routine congestion. The parties continue to dispute whether paid speakers
  produce materially distinct burdens and whether less restrictive coordination would
  address the City's risks.
- The 2026 fair dates are February 14, April 11, June 13, August 8, October 10, and
  December 12. One expired date cannot silently become proof that the operative permit
  regime or recurring dispute has ended.
- The actual Rule 8 motion follows district-court stay practice. B01–B07 are mutually
  exclusive threshold, impracticability, grant, and dissolution alternatives; none
  occurred on the actual docket.
- B02, B05, and B07 are intentionally scoped nonterminal endpoints in the appellate-stay
  stage. They cannot enter the actual A08/A10/A20 record and merits lineage; the workflow
  does not claim complete counterfactual notice, docketing, briefing, or mandate histories
  for those three teaching exercises.
- The actual appellate result preserves preliminary-relief entitlement and remands only
  for Rule 65(c)/(d) correction. The structured plan cannot independently encode
  non-vacatur or continued protection, so narrative documents and evidence must remain
  aligned.
- Emergency stay work is serialized before merits briefing. The model makes no general
  concurrency, automated mootness, arbitrary-filing classification, or actor-identity
  authorization claim.

## Reproduction scaffold

Run final commands only after their corresponding pins are supplied. Do not substitute a
generic successful test for the missing case-specific evidence.

```sh
jq empty content/m4/norvale-injunction/metadata/render-inventory-successor.json
jq empty content/m4/norvale-injunction/pack-candidate/resources/record.json

# Frozen workflow audit: 1b285f65a38c4be2a7bc8dbe29d3822aee2963d05019fbe5f79d5917272cc74a
# PENDING_TRACE_REPLAY_COMMANDS
# PENDING_MANIFEST_VALIDATION_COMMANDS
# PENDING_RESOLVED_VALIDATION_COMMANDS
# PENDING_DETERMINISTIC_EXPORT_COMMANDS
# PENDING_FOCUSED_TEST_COMMANDS
```

Schema-v2 thin roots with dependency-owned references normally require deferred export,
exact dependency installation, and resolved validation. That general contract is not a
claim that those gates have passed for Norvale.

## Independent-review gate

Automated checks and authoring review cannot establish First Amendment accuracy, current
Fourth Circuit practice, practical realism, or level 3. A qualified First Amendment and
appellate practitioner must review an exact finalized root. Any change to the record,
authority set, questions, workflow, traces, manifest, or dependency topology invalidates
earlier review evidence.
