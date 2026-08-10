# CA4 Rule 54(b) synthetic civil appeal

Status: installable, author-self-reviewed synthetic pack **v0.1.0** as of **2026-08-11**.
The manifest-governed directory is `pack/`, and the equivalent distributable archive is
`us-ca4-rule54b-asterglen-0.1.0.awpack`. Both load as pack
`us.ca4.rule54b.asterglen` at revision
`ff7a2e1195f9bd006e7df46c19675a3e07a4bd8975b1643a01adbc9cc4fd3424`.
The revision contains 19 declarative resources and 18 hash-pinned, searchable PDFs totaling
124 pages.

Every case name, party, lawyer, district judge, circuit-panel identity, docket number, address,
agreement, fact, and filing in this tree is fictional. The documents identify themselves as
synthetic training material so they cannot reasonably be mistaken for court records. Official
sources are used only for legal propositions and procedural shape; no real case record was
adapted. The three judge profiles are fictional composites and do not model a named real judge.

## Authored result

Asterglen Freight Software, Inc. timely notices an appeal from a separate partial judgment that
disposed of Count I while Count II and a counterclaim remain. The district court's Rule 54(b)
order grants the motion and directs entry of judgment but never expressly determines that there
is "no just reason for delay." The synthetic Fourth Circuit panel therefore dismisses for lack of
appellate jurisdiction and remands without reaching the contract merits. No rehearing petition or
stay motion is filed, and mandate issues under FRAP 40 and 41.

This is intentionally an adverse finality path. `traces/expected-traces.json` separately records
authoring expectations for a supported Rule 54(b) certification, a late notice, a missing
separate judgment, a rehearing petition, and a stay motion. Those counterfactual expectations are
not facts in the authored docket and are not all executable workflow branches.

## What is tracked

- `documents/` contains the UTF-8 Markdown sources for the district and appellate documents.
- `render-plan.json` is the strict 18-output batch-render plan.
- `rendered/` contains the rendered reference artifacts and `inventory.json`, including source,
  assembly, semantic-render, PDF, byte-size, page-count, provenance, and page-label evidence.
- `metadata/` records the synthetic dockets, the PDF set, and joint-appendix assembly.
- `resources/` contains the 19 authoring-side declarative resources; the record includes all 18
  final asset SHA-256 digests and page counts, distinct district and appellate dockets, true
  exercise ECF labels and filing metadata, and stable `JA1`–`JA47` page anchors.
- `pack/` is the exact manifest-governed installable directory, with 19 resources and 18 PDF
  blobs. It contains no Markdown authoring sources or undeclared members.
- `us-ca4-rule54b-asterglen-0.1.0.awpack` is the corresponding installable archive. Its archive
  SHA-256 is `ce0ebffb92942e85e02658d11846af70ebb5fdc287f99a4c683a48f381e39227`;
  this is distinct from the canonical pack-revision digest above.
- `sources/` is the official-source and research-limit ledger. `traces/` contains authored and
  counterfactual branch expectations with authority IDs.

## Rendering and the joint appendix

All PDFs use renderer contract `appellate.markdown-pdf.semantic-layout.v2`. The semantic render
plan is repeatable for the same sources, plan, and resolved render environment. The emitted PDF
bytes are deliberately **not** represented as cross-run deterministic: Qt's `QPdfWriter` adds
wall-clock information dates. The exact emitted artifacts in this revision are instead pinned by
their PDF SHA-256 and byte size in `rendered/inventory.json`, the record resource, and the
manifest.

The joint appendix is assembled from ten ordered source segments. Its 47 pages carry printed,
searchable labels `JA1` through `JA47`. Only page 1 of `documents/d59-rule-54b-order.md` is
included; its page-2 exercise annotation is excluded from both the standalone operative-order
PDF and the appendix.

## Executable workflow coverage

The canonical engine integration trace executes 20 commands and emits 21 authority-bearing
events from the notice deadline through mandate. Clerk-issued notices, deadline administration,
ordinary stage administration, and mandate use an explicit fictional clerk actor; judgment-stage
submission and disposition use the fictional composite panel. It accepts the notice, docketing
statement, opening brief, and response brief; records six calculated deadlines; and exactly
replays the final state. Isolated adverse decisions exercise:

- a docketing-statement deficiency for missing fields and service, without inventing a cure
  deadline;
- rejection of a nonconforming opening brief; and
- rejection of a response brief filed after its route-enforced deadline.

Four of the six catalog filing types are executable routes: notice of appeal, docketing
statement, opening brief, and response brief. Transcript-order and rehearing-petition forms are
catalogued but not executable routes in v0.1.0. Only the response-brief deadline is linked to
filing eligibility and enforced by a route. The notice, initial-document, opening-brief,
rehearing, and mandate deadlines are explicitly calculated, recorded, and replayed, but are not
currently route-enforced.

## Known limits

This revision is an author self-review only. No independent qualified legal reviewer has reviewed
it, every realism dimension remains at level 2 or below, and **no level-3 realism claim is made**.
Other intentional v0.1.0 limits are:

- no executable appearance/disclosure filing, reply brief, tolling motion, extension, reopening,
  or conditional Rule 41 later-of branch;
- no general predicate evaluator for the Rule 54(b) finality and abuse-of-discretion alternatives;
- no general conditional treatment of party status, Rule 58 entry, FRAP 4(a)(4), 4(a)(5), or
  4(a)(6), amended decisions, rehearing petitions, or stay motions;
- a counterfactual oral-argument configuration for training even though the authored appeal is
  submitted and decided on the briefs.

## Reproducing the technical checks

The following commands are examples from the repository root. They require the documented Qt 6,
CMake, compiler, and library prerequisites; their presence here does not imply that a reader has
already built the targets.

```sh
cmake --preset dev
cmake --build --preset dev --target appellate-render appellate-pack tst_gold_case_trace tst_pack_catalog

./build/dev/src/cli/appellate-pack validate content/ca4-rule54b/pack
./build/dev/src/cli/appellate-pack validate content/ca4-rule54b/us-ca4-rule54b-asterglen-0.1.0.awpack
ctest --test-dir build/dev --output-on-failure -R '^(gold_case_trace|pack_catalog|markdown_pdf_renderer|render_cli)$'
```

To rerender into a fresh, nonexisting output directory without overwriting the pinned pack:

```sh
./build/dev/src/content/appellate-render \
  "$(pwd)/content/ca4-rule54b/render-plan.json" \
  "$(pwd)/content/ca4-rule54b" \
  /absolute/path/to/new-ca4-rule54b-render
jq empty /absolute/path/to/new-ca4-rule54b-render/inventory.json
```

Rerendered PDFs may have different byte digests because of `QPdfWriter` wall-clock metadata. A
pack update must deliberately pin the new inventory, record, manifest, archive, and revision
digests together; a rerender is not an in-place verification of the existing PDF bytes.
