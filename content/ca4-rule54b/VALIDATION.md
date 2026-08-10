# Validation evidence and remaining realism limits

Evidence refreshed on **2026-08-11** from the repository root. This document distinguishes
pack-specific checks from generic trust-boundary tests and from legal review.

## Pack-specific evidence

- The current pack CLI accepted both `content/ca4-rule54b/pack` and
  `content/ca4-rule54b/us-ca4-rule54b-asterglen-0.1.0.awpack`. Each reported pack ID
  `us.ca4.rule54b.asterglen`, version `0.1.0`, 19 resources, 18 blobs, and the same canonical
  revision digest:
  `ff7a2e1195f9bd006e7df46c19675a3e07a4bd8975b1643a01adbc9cc4fd3424`.
- The archive SHA-256 is
  `ce0ebffb92942e85e02658d11846af70ebb5fdc287f99a4c683a48f381e39227`.
  The archive digest identifies the archive bytes; it is not the canonical pack-revision digest.
- `pack/manifest.json` declares the exact 19-resource and 18-blob member set, with resource
  digests and PDF byte sizes/digests. Pack validation checks the manifest, exact member set,
  schema, cross-resource references, blob sizes, and blob hashes for both directory and archive
  inputs.
- `pack/resources/record.json` and its authoring mirror contain 18 docket entries, all with final
  64-character `asset_sha256` values and positive page counts. Every record asset resolves to one
  declared PDF blob, and the page-count sum is 124. The record structurally distinguishes the
  synthetic district and appellate dockets, preserves their exercise ECF labels, actors,
  descriptions, and tags, and declares unique page anchors for `JA1` through `JA47`.
- `rendered/inventory.json` contains 18 entries under renderer contract
  `appellate.markdown-pdf.semantic-layout.v2`. Every entry records assembly/semantic provenance,
  actual page count, byte size, and pinned PDF SHA-256. The renderer contract produces native
  searchable text rather than rasterized pages.
- The joint-appendix inventory records a ten-segment composite, 47 logical and rendered pages,
  and exact printed/searchable page labels `JA1` through `JA47`. The segment for
  `documents/d59-rule-54b-order.md` selects page 1 only; the page-2 exercise annotation is not in
  either rendered operative-order use.
- The source/metadata audit still resolves all document paths, continuous JA ranges, record
  anchors, case authority IDs, trace authority IDs, and workflow authority copies against the
  canonical authority set. The ledger contains 22 authority objects and explicitly marks the
  unpublished/nonbinding source used as secondary support.

## Automated executable evidence

The current gold-case integration test loads the exact directory revision and resolves every
runtime link, including all record blobs and the three fictional-composite bench profiles. Its
canonical run executes 20 commands, emits 21 events, and replays exactly to the same terminal
state after mandate. It verifies:

- explicit court-role authority for all court commands, with clerk and composite-panel actor IDs
  distinguished across administrative and decisional acts;
- accepted notice, docketing statement, opening brief, and response brief;
- calculated dates of 2026-03-06 (notice), 2026-03-19 (initial documents), 2026-04-29 (opening
  brief), 2026-05-29 (response), 2026-06-22 (rehearing), and 2026-06-29 (mandate);
- a deficiency event for missing docketing-statement fields and service, with no invented cure
  deadline and no mutation of the canonical journal;
- a nonconforming-opening-brief rejection and a late-response rejection, likewise isolated from
  the canonical journal; and
- the pinned judgment and mandate document hashes in terminal workflow state.

Observed focused test results in the reference workspace were:

- `GoldCaseTraceTest`: 4 passed;
- `PackCatalogTest`: 12 passed;
- `MarkdownPdfRendererTest`: 10 passed; and
- `RenderCliTest`: 9 passed.

The catalog tests are generic trust-boundary evidence rather than a second legal review of this
case. They cover immutable exact-revision installation, archive-integrity detection, verified
content-addressed blob materialization, rehydration of a missing object from a verified archive,
deduplication, refusal to overwrite corrupt or linked objects, missing/corrupt archive behavior,
descriptor tampering, and storage migration. The renderer tests separately cover searchable
PDFs, repeatable semantic identity, honest byte nondeterminism, printed/searchable labels,
batch rollback, no overwrite, and traversal/symlink/resource rejection.

## Rendering reproducibility boundary

The strict render plan, source hashes, assembly-plan hashes, semantic-plan hashes, semantic-render
hashes, resolved fonts, Qt versions, and layout contract are recorded in the inventory. That
semantic plan is repeatable for the same source, plan, and resolved render environment.

The PDF byte stream is not promised to repeat across invocations. `QPdfWriter` writes wall-clock
information dates even though the renderer provenance fixes its own semantic timestamp. The
current PDF files are therefore immutable pinned artifacts: their manifest hashes verify those
specific bytes, but a fresh correct render may have different PDF hashes. Any intentional
rerender requires coordinated record, manifest, archive, and revision updates.

## Exact executable-scope limits

The filing catalog has six types, but only four have workflow routes:

| Catalog filing | Executable in v0.1.0 | Current behavior |
| --- | --- | --- |
| Notice of appeal | Yes | conforming acceptance and nonconforming rejection; direct deadline recorded, not route-enforced |
| Docketing statement | Yes | acceptance or deficiency for missing fields/service; no fixed cure period invented |
| Transcript order | No | form/catalog entry only |
| Opening brief | Yes | acceptance or nonconforming rejection; acceptance creates the response deadline |
| Response brief | Yes | acceptance or rejection after the route-enforced response deadline |
| Rehearing petition | No | form/catalog entry and counterfactual trace evidence only |

Only the response deadline is currently connected to a filing route's eligibility decision.
Notice, initial-document, opening-brief, rehearing, and mandate deadlines are explicitly
court-calculated, recorded, authority-bearing, and replayed; the current routes do not reject a
filing merely for violating those direct deadlines.

Workflow schema v1 and this pack revision do not generally evaluate:

- private/federal-party selection, Rule 58 entry, FRAP 4(a)(4) tolling, FRAP 4(a)(5) extension,
  FRAP 4(a)(6) reopening, amended decisions, or service-mode predicates;
- ultimate disposition of an individual claim, the express Rule 54(b) determination,
  case-specific findings, claim separateness, or abuse-of-discretion predicates;
- appearance/disclosure filings, reply briefing, rehearing-petition execution, stay motions, or
  Rule 41's conditional later-of logic; or
- alternate finality/statutory routes and merits outcomes after a supported certification.

`traces/expected-traces.json` preserves those sourced counterfactual expectations as authoring
evidence. It does not make them executable or claim that one result applies to every real case.

The authored docket records submission and decision on the briefs. Its argument configuration is
counterfactual moot-court training, not a historical docket event. The record represents the
district and appellate entries as distinct typed dockets and binds each issue to precise JA page
anchors; cross-filed documents retain their second exercise filing number in authored metadata.

## Legal-review status and release gates

The realism resource remains `independent_review_pending`, with every dimension at level 2 or
below. No independent qualified legal reviewer has reviewed the procedural choices, documents,
consequences, oral-argument configuration, or source application. The successful technical and
engine checks above do **not** establish realism level 3.

Before any later release claims level 3, it must obtain attributable independent qualified
review and incorporate or explicitly resolve that review. Broader executable claims additionally
require sourced route/predicate support for the limits listed above. Future cases must preserve
the same exact asset, docket, entry-label, and page-anchor consistency evidence.

## Reproducible commands

These examples assume the repository prerequisites have been installed and the targets have been
built; they do not claim that a reader's checkout already contains build products.

```sh
cmake --preset dev
cmake --build --preset dev --target appellate-render appellate-pack tst_gold_case_trace tst_pack_catalog

jq empty content/ca4-rule54b/metadata/intended-pdfs.json
jq empty content/ca4-rule54b/metadata/joint-appendix.json
jq empty content/ca4-rule54b/render-plan.json
jq empty content/ca4-rule54b/rendered/inventory.json
jq empty content/ca4-rule54b/pack/manifest.json

./build/dev/src/cli/appellate-pack validate content/ca4-rule54b/pack
./build/dev/src/cli/appellate-pack validate content/ca4-rule54b/us-ca4-rule54b-asterglen-0.1.0.awpack
ctest --test-dir build/dev --output-on-failure -R '^(gold_case_trace|pack_catalog|markdown_pdf_renderer|render_cli)$'
```

The two `validate` results should identify the same canonical pack revision. Archive SHA can be
checked independently with:

```sh
sha256sum content/ca4-rule54b/us-ca4-rule54b-asterglen-0.1.0.awpack
```
