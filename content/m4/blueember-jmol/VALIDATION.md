# Blue Ember v1.2.0 validation record

Source/render/record/core/workflow verification as of **2026-08-19**: all 83
synthetic sources, accepted PDFs, schema-v2 record entries and anchors, seven
non-workflow core resources, and the workflow are frozen. Production traces,
realism-evidence authoring, manifest assembly, root revision, deterministic archive,
resolved install, and Blue Ember tests remain **PENDING**. This file does not claim
an installable pack, independent review, level 3, or gold status.

## Frozen evidence pins

| Evidence item | Current value | Status |
| --- | --- | --- |
| Path-framed source closure | `c5e843bfc968a726ee86a0d130cd8b85e89c74e76dc75239191a20048a9054a5` | Frozen: 83 sources / 656 logical pages |
| Successor document-plan SHA-256 | `0d831d6537114a880d20bc0529b26364f331cfa4e4b8bf60958cb3f5440139f1` | Frozen planning/allocation contract |
| Render-plan SHA-256 | `3499d2fbd1f0ced9f0efe8c09158303e92bc0fbc5901a8bb8ea8672e7e6c81d0` | Frozen: schema v1 / 83 rows |
| Accepted checked-in render-inventory SHA-256 | `c52eab8d01e68ec08f5f43e07e8ef2fdc7996ddc15cace7ab4b818518b51f89a` | Frozen: 83 PDFs / 656 physical pages |
| Canonical compact inventory SHA-256 | `17d8e5b10960a3a1d3ea7a287d5edb31808e38e3fa06b3b46710714948fedf4e` | Checked-in inventory after `jq -S -c` canonicalization |
| Record-resource SHA-256 | `080ff7772d73131a5471f2fc530b4d63c6215831a82ffcd671ef50beff8d1c7a` | Frozen: 3 dockets / 83 entries / 656 anchors |
| Ordered raw PDF corpus SHA-256 | `c42d64a0d047c19dc9d17828df36f993a695ac2b662e803171000c73bf860406` | Frozen: accepted inventory output-path order |
| Framed PDF corpus SHA-256 | `9e06320285f67157b81fb2ec756ce55ec162de482bbca5655e110f950fc88d26` | Frozen: output path + NUL + bytes + NUL |
| PDF corpus size | `3,462,685` bytes | Frozen: 83 accepted PDFs |
| Actual argument resource SHA-256 | `9d983eb1270280497795e24d487a3b0fad510c55eeeb4b4a6b70d5a93e364404` | Frozen: 12 actual-record questions |
| Counterfactual argument resource SHA-256 | `4502a5f3184bc953db27f9524d750af6968355f7111ea6d9aceca4e11e5a447e` | Frozen: 12 counterfactual questions |
| Authority-set resource SHA-256 | `34f7f611377e8bfd666bf261bd9e538169ce88f99b333f0414e8e1d8a99db530` | Frozen: 15 authority entries |
| Bench-configuration resource SHA-256 | `e331949939abbfc1000b92ab1b01066a3be544289e5f5fd2f9e159bc5b9a05b5` | Frozen: three fictional/composite seats |
| Case resource SHA-256 | `c63d4399b31294205e1d95f82ed0944433c58ae378969de80e502b587545fccb` | Frozen: four issues / two asymmetric disposition plans |
| Procedure-profile resource SHA-256 | `f19aa58510d03b915c07da24aea464dfd9163e0b19a63778cce073bb00db722e` | Frozen core resource |
| Workflow resource SHA-256 | `7c2356718286505eee16d62b48ca281f92eee367c9e21319ddcae02d87c1a120` | Frozen: 24 stages / 93 operations / 15 routes / 18 filing bindings / 25 court-document bindings / 14 named deadlines |
| Actual question-bank grounding digest | `2d8dcb81c7b66c5e48f6cb8dc1f12dbfc6875b973c8d601e498475ecf39732bd` | Independently recomputed from frozen authority/record bytes |
| Counterfactual question-bank grounding digest | `b9260ac4bb7d70ebea11970a791635dc3e49df92ccbe572360d49adbfc97d733` | Independently recomputed from frozen authority/record bytes |
| Actual disposition digest | `e1569b1519f425703fdd80738814ba88d610837372eb234937176a6b0b3e6a67` | Independently recomputed from the four-component actual plan |
| Counterfactual disposition digest | `a89405ee0112d1879b2ed9765624966d4ae32289e1f4a6e27c4fc23c80892f40` | Independently recomputed from the two-component preserved-mitigation plan |
| Realism-review scaffold SHA-256 | `fd9040612bd9257e7b0a7486349d0659cdddb27f6c448f4363c90dba0e1cbbb1` | Frozen pre-authoring scaffold; level 2 throughout; no `evidence` field |

## Explicitly pending pins and gates

| Artifact or gate | Current value | Status |
| --- | --- | --- |
| Successor appellate/trace plan SHA-256 | **PENDING** | Not authored or frozen |
| Canonical trace set | **PENDING** | No production journals or trace digests are claimed |
| Manifest SHA-256 | **PENDING** | No final resource/blob/capability closure is claimed |
| Promoted realism-review SHA-256 | **PENDING** | Evidence authoring must follow workflow/trace freeze |
| Exact evidence-closure digest | **PENDING** | Scaffold deliberately omits `evidence` |
| Root revision | **PENDING** | Candidate is not yet installable |
| Deterministic archive | **PENDING** | No archive size or digest is claimed |
| Resolved installation audit | **PENDING** | Requires a frozen root and exact dependencies |
| Blue Ember focused/full-suite tests | **PENDING** | No Blue Ember test-completion claim is made |

The workflow's **24 stages / 93 operations / 15 filing routes / 18 exact filing
bindings / 25 court-document bindings / 14 named deadlines** are now frozen. Those
structural counts do not become trace or release evidence until production journals
are authored, independently replayed, and hostile-tested.

## Completed source, render, record, core, and workflow gates

The following claims are confined to the frozen source/render/record/core/workflow bytes:

1. The source allocation resolves 42 lower sources to JA1–JA430, 16 actual appellate
   sources to PA1–PA108, and 25 never-filed counterfactual sources to PA109–PA226.
   The arithmetic is 42 + 16 + 25 = 83 documents and 430 + 108 + 118 = 656 pages.
2. A production render emitted exactly 83 PDFs and 656 physical pages. Every accepted
   PDF passed structural validation; inventory/output hashes, byte sizes, page counts,
   and JA/PA labels match the frozen record. Each page is searchable and ends with its
   exact page label. The accepted corpus contains no MP4.
3. The schema-v2 record has exactly three dockets, 83 entries, and 656 unique anchors.
   JA1–JA430 and PA1–PA226 are each contiguous. Every B entry uses only the
   counterfactual docket and carries `counterfactual_appellate_branch`, `never_filed`,
   and `never_occurred_on_actual_docket` tags.
4. The 83 accepted PDFs have unique hashes and match the checked-in render inventory.
   Every record asset link resolves to the corresponding accepted PDF hash.
5. The seven promoted non-workflow resources are byte-identical to their frozen
   candidates, validate against their schema-v2 resource schemas, and pass resolved-
   reference checks against the frozen record and authority set.
6. Actual and counterfactual grounded-question digests recompute from the exact
   authority propositions/provenance and record asset/anchor bytes. Actual questions
   exclude PA109–PA226; the counterfactual bank may use those separately docketed pages.
7. Actual and counterfactual disposition digests recompute from the case plans. The
   four-component actual plan reverses mitigation JMOL, affirms the conditional Rule 59
   ruling and damages-only scope, and vacates the amended judgment with remand. The
   two-component counterfactual plan affirms mitigation JMOL and the amended judgment
   without deciding the conditional Rule 59 ruling or scope.
8. The actual record consistently fixes both Rule 50(a) motions as causation-only and
   mitigation as first raised under Rule 50(b). B01 alone supplies the isolated premise
   that both pre-verdict motions expressly preserved mitigation; it does not rewrite
   the actual district record.
9. The authority inventory distinguishes *Plyler*'s unavailable new Rule 50(b) basis,
   *Wiener*'s preserved-issue/appellate-theory variation, and *Gautier*'s same-issue
   incorporation. All 15 entries carry exact source versions, locators, checked-on
   dates, URLs where applicable, precedential status, and propositions.
10. The schema-v2 workflow freezes 24 stages, 93 operations, 15 filing routes,
    18 exact filing bindings, 25 court-document bindings, and 14 named deadlines at
    SHA-256 `7c2356718286505eee16d62b48ca281f92eee367c9e21319ddcae02d87c1a120`.
    This topology is an authoring boundary; no trace-coverage or replay claim follows
    from structure alone.

These gates establish provenance and internal consistency. They do not establish
accessibility conformance, legal correctness, practical realism, workflow correctness,
installability, or independent professional review.

## Pending trace and release verification

Before any evidence-bearing realism review may be authored, the project must still:

1. author production-engine traces that cover every operation and intended actual,
   counterfactual, rehearing, mandate-stay denial, mandate-stay grant, dissolution,
   and blocked endpoint;
2. independently redecide commands, replay every prefix and full journal, and reject
   legal-time, sequence, document, service, order, disposition, state, and journal
   mutations;
3. author an exact realism-evidence envelope only from the frozen dependency, core,
   PDF, workflow, trace, record-check, and authority bytes;
4. assemble and validate an exact manifest and root, export deterministic archives,
   install from a dependency-only catalog, and run focused plus full-suite tests.

The review state remains `independent_review_pending`. Automated source, render,
schema, digest, and future replay checks cannot substitute for qualified independent
review of Rule 50 preservation, Rule 59 consequences, damages-only retrial scope,
North Carolina mitigation law, current Fourth Circuit practice, or overall trial and
appellate realism.
