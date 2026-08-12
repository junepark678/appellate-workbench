# Asterglen v0.2.0 validation status

Evidence status as of **2026-08-12**: source authoring and hostile source review are complete;
the two corrected-layout renders are accepted, and the 13-stage workflow is frozen. The final
manifest, archive, exact evidence closure, dependency-resolved validation, production replay,
deterministic export, and predecessor/successor co-installation gates pass. The successor is an
installable level-2 authoring pack; qualified independent review remains pending.

## Final release pins

| Evidence item | Current value | Status |
| --- | --- | --- |
| Canonical root revision | `7e77bc0fbe02dc9e108681df73852859d6d0f577acdcb65fcfb7678eac78b728` | Frozen |
| Manifest SHA-256 | `dcfac3e4cb8d60fe41843c15732d802b9dd5d689b6de7395f4f86339676dfa49` | Frozen |
| Realism-review SHA-256 | `e16caac5226fdb26fb8acead14ef0a0bfd4d569af5ba84b9da65389e5fb0c905` | Frozen |
| Deterministic archive size | `3,974,147` bytes | Two exports byte-identical |
| Deterministic archive SHA-256 | `10739c149a3bf2617d8af6dd131caee7ea6639a9d97e26cdf2974fa176c82819` | Frozen |
| Exact evidence-closure digest | `445c3f11dcc8046eedfc233407699cbbb3ea4e39425d22c976808959350ca62c` | Frozen |
| Accepted lower render-inventory SHA-256 | `6a035dc4e431496d3eadd994926d6a383a26606bef67c1d4dd5d5ac89e5afe46` | Frozen: 37 PDFs / 234 JA pages |
| Accepted appellate render-inventory SHA-256 | `debe1315e98116a5b9f5552d5c11eeb98badb2579e28d75663d5b822e5c3059d` | Frozen: 38 PDFs / 143 PA pages |
| Framed 75-PDF corpus SHA-256 | `7c9c70733dcf87d2bc9d2fca02cbbce28c485d6712516caadbaad71f58c43fbb` | Frozen lower-then-appellate plan order |
| Final record-resource SHA-256 | `429603e0b7b49ff25e8a444a411b9c257cfbf0009fbfe1fc103ae8ac80e52f84` | Frozen |
| Final workflow-resource SHA-256 | `949626bcbe0046bbabf615f62df8376a4fcfa463e30fc9dd5c322d56d4428f21` | Frozen: 13 stages / 81 operations / 11 routes |
| Final post-render hash set | lower inventory `6a035dc4e431496d3eadd994926d6a383a26606bef67c1d4dd5d5ac89e5afe46`; appellate inventory `debe1315e98116a5b9f5552d5c11eeb98badb2579e28d75663d5b822e5c3059d`; record/workflow as above | Frozen |
| Final focused test results | `asterglen_rule54b_v02` and `asterglen_rule54b_v02_ui_e2e`: local 2/2 in 53.82s; `TZ=UTC` 2/2 in 54.09s | Frozen on exact root; Werror build, clang-format, and diff checks pass |

The focused Asterglen v0.2 integration and UI test result is filled only after those exact targets
finish against these pins; it is not inferred from generic pack validation.

## Frozen source, render, and workflow evidence

The frozen plans, accepted inventories, emitted objects, and workflow currently establish the
following authoring facts. These checks do not substitute for validating the final resolved pack.

- The v0.2.0 lower-record plan resolves 37 distinct reviewed Markdown sources to 234 planned
  substantive pages with continuous JA1–JA234 labels.
- The appellate plan resolves 13 actual sources to PA1–PA70 and 25 expressly counterfactual
  sources to PA71–PA143. The two PA classes use distinct docket IDs and the branch docket numbers
  end in `-CF-NEVER-OCCURRED`.
- The aggregate contract is 75 source documents and 377 planned page anchors. JA and PA are
  separate namespaces, every range is continuous within its namespace, and no joint-appendix
  container is counted as another record PDF.
- The plans distinguish the new NDWV and Fourth Circuit identities
  (`SYN-NDWV-25-CV-0618`; `SYN-CA4-26-CV-4102`) from the frozen predecessor's EDVA and appellate
  identities (`SYN-25-0117`; `SYN-26-1427`). No predecessor source, PDF, or blob content is
  carried into the successor. Labels and resource/anchor IDs are version-scoped, so matching
  label text across revisions does not identify shared content.
- All 75 successor sources carry synthetic-artifact warnings. Their plan entries currently record
  `source_review_clear` / `clear`; this is authoring review, not independent legal review.
- `metadata/render-inventory-lower-successor.json` accepts 37 emitted PDFs/234 JA pages, and
  `metadata/render-inventory-appellate-successor.json` accepts 38 emitted PDFs/143 PA pages. Both
  plans record `rendered_after_layout_correction`; the inventories pin per-file source,
  assembly-plan, semantic-render, PDF, byte-size, page-count, and page-label evidence.
- The frozen workflow has 13 stages, 81 operations, and 11 filing routes. Its three judgment
  operations bind the accepted rendered judgment digests to the three structured disposition
  plans. Eight canonical traces contain 205 commands and 221 events, cover all 81 operations, and
  replay twice through the production engine. Exactly seven recoverable nonconforming submissions
  exercise all seven rejection operations.
- The supported-certification branch intentionally omits separate branch-specific notice,
  docketing, and briefing PDFs. Its B01–B03 district predicates and B04–B07 disposition/mandate
  sequence abstract those intermediate appellate steps. This limits the branch's evidence to the
  exact authored certification and merits path; it is not evidence of a complete alternate docket.

## Completed release gates

The final evidence change verified and recorded all of the following:

1. zero undeclared pack members, zero missing or duplicate blob paths, and exact agreement among
   the two accepted inventories, repository PDF bytes, the final record resource, and manifest;
2. every JA1–JA234 and PA1–PA143 record anchor resolving to the correct accepted asset and
   physical page, with the 37/234, 13/70, and 25/73 classifications preserved;
3. nine schema-v2 resources and 16 required capabilities, with all dependency revisions,
   authority IDs, issue anchors, grounded questions, operation bindings, and disposition-plan
   bindings resolving;
4. actual-history grounding that excludes PA71–PA143 and counterfactual grounding that never
   mutates the actual docket;
5. all eight canonical journals replaying exactly through the production engine, including the
   Rule 58 150-day branch and Rule 41 rehearing/stay later-of branches;
6. the final realism resource closing the exact manifest, dependencies, journals, record checks,
   and selected authorities without claiming independent review; and
7. deferred export, exact installation of the three dependencies and root, resolved-closure
   validation, deterministic archive construction, predecessor/successor coexistence, and
   preservation of every v0.1.0 byte and identity; and
8. focused case and UI integration tests run against this exact root, including immutable v0.1/
   v0.2 coexistence, all eight canonical journals, persisted replay/reopen, and actual/branch PDF
   navigation and search.

Items 1–8 pass on the exact pinned bytes. The focused test result does not imply an accessibility,
performance, or independent legal-review claim.

## Procedural and merits consistency gates

Final review must preserve these distinctions:

- Count I received an ultimate disposition, but Count II and Copper Kestrel's $120,000 amended
  counterclaim remained pending through June 8, 2026.
- The February 4 order granted the Rule 54(b) motion and directed a separate judgment without an
  express no-just-reason-for-delay determination. The separate judgment did not add it.
- The March 3 notice precedes the conditional March 6 thirty-day date, but a timely notice cannot
  create a final decision under 28 U.S.C. § 1291.
- The actual judgment dismisses and remands, reaches no contract merits, does not vacate the
  January 12 partial-summary-judgment order, and directs each side to bear its own appellate costs.
- The missing-separate-document branch treats 150 days after February 4 as Saturday, July 4,
  rolls the entry date to Monday, July 6 under the authored calendar rule, and calculates the
  conditional notice deadline as August 5.
- A timely June 22 rehearing petition followed by a July 1 denial yields a July 8 mandate date;
  a June 26 stay motion denied July 10 yields July 17; a granted stay blocks mandate until an
  exact release event.
- The Pilot License's Virginia choice-of-law clause and written-notice/15-calendar-day cure
  condition are merits inputs. The actual appellate court expresses no view on their application.
- *McPherson* is identified as nonprecedential secondary support; *Kinsale*, *Bowles*, and
  *Primov* retain their stated precedential classifications and limited propositions.

## Reproducing the final gates

The following commands reproduce the pack-closure gate from the repository root:

```sh
cmake --preset dev
cmake --build --preset dev --target appellate-render appellate-pack tst_gold_case_trace tst_pack_catalog

jq empty content/ca4-rule54b/metadata/render-inventory-lower-successor.json
jq empty content/ca4-rule54b/metadata/render-inventory-appellate-successor.json
jq empty content/ca4-rule54b/pack-v0.2.0/manifest.json

asterglen_check_root="$(mktemp -d)"
asterglen_catalog="$asterglen_check_root/catalog"
asterglen_archive="$asterglen_check_root/us-ca4-rule54b-asterglen-0.2.0.awpack"

./build/dev/src/cli/appellate-pack export-deferred \
  content/ca4-rule54b/pack-v0.2.0 "$asterglen_archive"
./build/dev/src/cli/appellate-pack install \
  content/foundations/us-federal/foundation-us-federal-2025.12.01.awpack "$asterglen_catalog"
./build/dev/src/cli/appellate-pack install \
  content/foundations/us-ca4/foundation-us-ca4-2026.03.23.awpack "$asterglen_catalog"
./build/dev/src/cli/appellate-pack install \
  content/foundations/us-ca4-fictional-bench/foundation-us-ca4-fictional-bench-1.0.0.awpack \
  "$asterglen_catalog"
./build/dev/src/cli/appellate-pack install "$asterglen_archive" "$asterglen_catalog"
./build/dev/src/cli/appellate-pack validate-resolved \
  "$asterglen_catalog" us.ca4.rule54b.asterglen 0.2.0 \
  7e77bc0fbe02dc9e108681df73852859d6d0f577acdcb65fcfb7678eac78b728

ctest --test-dir build/dev --output-on-failure \
  -R '^asterglen_rule54b_v02(_ui_e2e)?$'
TZ=UTC ctest --test-dir build/dev --output-on-failure \
  -R '^asterglen_rule54b_v02(_ui_e2e)?$'
```

Schema-v2 thin roots with dependency-owned references intentionally fail standalone `validate`
and ordinary `export`. `export-deferred` validates local structure while recording unresolved
references; only exact dependency and root installation followed by `validate-resolved` validates
the executable closure. The final command uses the exact root digest emitted by deferred
export/install.

The two exact case-specific tests pass locally and under `TZ=UTC`; the result and durations are
pinned above. Generic catalog or renderer tests remain trust-boundary evidence, not a second legal
review of Asterglen.

## Frozen predecessor evidence

The v0.1.0 evidence is already final and is not governed by the successor placeholders. The
checked-in directory and archive identify root revision
`ff7a2e1195f9bd006e7df46c19675a3e07a4bd8975b1643a01adbc9cc4fd3424`; the archive is
729,511 bytes with SHA-256
`ce0ebffb92942e85e02658d11846af70ebb5fdc287f99a4c683a48f381e39227`. It contains 19
resources and 18 PDFs/124 pages on the EDVA `SYN-25-0117` / appellate `SYN-26-1427` identities.

Final successor validation must test coexistence without altering that directory or archive.
The new NDWV/`SYN-CA4-26-CV-4102` PDFs are independent blobs, not relabeled predecessor material.

## Legal-review gate

The current authoring review is `ca4r54b.review.authoring-2026-08-12` with
`independent_review_pending` and level 2 in all seven dimensions. Source review, schema
validation, hash closure, production replay, and automated tests do not establish legal accuracy,
currency, completeness, or practical realism. A qualified civil-appellate and Virginia-contract
reviewer must review the exact final root in a detached review pack before any level-3 or civil
gold-pack claim.
