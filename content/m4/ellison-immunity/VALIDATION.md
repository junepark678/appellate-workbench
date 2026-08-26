# Ellison v1.2.0 validation record

Final assembly verification as of **2026-08-18**: all 72 synthetic sources, accepted PDFs,
schema-v2 record entries and anchors, nine resources, workflow, six canonical traces, manifest,
level-2 realism evidence, root revision, and deterministic archive are frozen. Fresh exact-
dependency installation, four-revision resolved validation, and the installed hostile replay
audit passed. The review's `reviewed_on` date remains 2026-08-12 and its state remains
`independent_review_pending`; no level-3 or gold claim is made.

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
| Historical realism-review scaffold SHA-256 | `ad4b08d350f5cf843b57970e7b5d0342cd95edda08ba2d91f575196c8786c28f` | Frozen pre-authoring input; level 2 throughout; no `evidence` field |
| Workflow resource SHA-256 | `cd69b276a63ae508ba0d98bbee15585847a405b5b55a48df69fffe45811ca23a` | Frozen: 20 stages / 77 operations / 11 routes / 14 filing bindings / 24 court-document bindings / 2 disposition-discriminated branches / 11 named deadlines |
| Successor appellate plan SHA-256 | `82f1afa17e4d15a192cc6567ff3ffaa3415d2dd95f609aa58c760c026c78273d` | Frozen six-path allocation and trace contract |
| Canonical trace set | Six exact file pins below | Frozen: 6 traces / 229 commands / 229 events / exact 77-operation union / 5 terminated + 1 stayed |
| Manifest SHA-256 | `8f0d614a73a4850a93170a7338229b64e2b1d042134e678785eaab481fd8ca42` | Final: 9 resources / 72 blobs / 16 capabilities / 3 dependencies / exact 82-member allowlist |
| Promoted realism-review SHA-256 | `5545977962535b58f029de10959cf2f9e49348a12fb4f7bff17574aa688b8867` | Level 2 in all seven dimensions; `independent_review_pending` |
| Exact evidence-closure digest | `8032c5547dd522cad241b9c816bd611d198dbe7e007b0cd29781b8b471de41ac` | Exact 4 packs / 44 non-review resources / 72 blobs / 6 traces / 2 checks / 35 authorities; 159 unique non-pack evidence IDs |
| Dimension-evidence references | `49 / 19 / 75 / 33 / 17 / 4 / 114` | Procedural law / deadlines-authority / record consistency / consequences / oral argument / bench differentiation / provenance; all scores are 2 |
| Root revision | `c2a4f3bc07f05eb1429257320ed839ebaea837da7aa7330f4669bbb157168ce0` | Final installable revision over four exact packs |
| Deterministic archive | 4,230,462 bytes; `59f32f521644bac61865cf1e59444fc98dbb9007461a1709272ffe261cbad1d0` | Independent A/B and frozen exports byte-identical |
| Authoring provenance | Transcript SHA-256 `55f5393d73b2d9505fd9fa85332c43690cbfaef25fed87115c1d824db80b5e4b` | Exactly one successful `author-realism-evidence-multi` invocation; no retry or transaction residue |
| Resolved installation | Root `c2a4f3bc07f05eb1429257320ed839ebaea837da7aa7330f4669bbb157168ce0` / 4 revisions | Fresh dependency-only catalog, root install, and exact resolved validation passed |
| Focused installed runtime audit | `229 / 77 / 14 / 24 / 11 / 5+1`; 66 hostile mutations rejected | 458 deterministic decisions, 229 prefix replays, 12 full replays; all filing recoveries, court documents, deadlines, and endpoints exact |

## Canonical trace pins

| Trace | Commands / events | Final stage | File SHA-256 | Journal SHA-256 | Trace digest |
| --- | ---: | --- | --- | --- | --- |
| Actual ordinary mandate | 41 / 41 | `terminated` | `1de19773f90e9a5c440a8ec79d4f726d4eb2c9b2d0817293e185ef145357928e` | `b2fd82908689c742be0f375a3307e107b5dfb7686c3233ba8beb2e3e1d8e76ef` | `2ddd609848aedd2cc17ef41b3cf9533326c133e24fdea0de5c8eb662fe130046` |
| Adverse ordinary mandate | 30 / 30 | `terminated` | `7bdc9a1469efd3c3a899769f61833176d56bd81e2b34a4920cb3e34b73848ac0` | `9dbf9038a0e191d4a85ec95873930e17c6ec8f88bb2e27fc02c3e5ce75764fed` | `c379f538713d1a82bac4491afdfc3cc80e98532aeaf07d5729dcd76a78261af4` |
| Adverse rehearing denial | 36 / 36 | `terminated` | `6abc5c4bad493e38ff985de9760618aea0101b501715b0becbdd87172f0f6550` | `86beba025bb3e7c398325c1c30f0aecca08fd1714d9fe9b8620c50a619bac26c` | `7b1a2104c2bbf1f267adc8353590520528ce1ce1c47e50bc67d0f8e041e19db4` |
| Mandate-stay denial | 41 / 41 | `terminated` | `b93fd95c53ba444c52acc7f2554cf47ce2a93d9e3636e17f272b5e20a5578352` | `7bb7f009e50a08894238eb684773c7d4c3bcde65dad51d2dd793978b7b82e1d4` | `8fce82b56d0534556ff39896e9ce48118f635e54c69d92481a85fdbaa5128661` |
| Mandate-stay grant blocked | 39 / 39 | `actual-mandate-stayed` | `ef13313ee3210b83f213b7ff6a4442f52a289609c4d768ee6e63e822e56fdd10` | `ad66809ad0a71608c2267b2899207fe286ea2dd54313862a4b893061a4a1b3ae` | `404db726c651dfe4a935445c43b16fef74dee2379699910d0aeecf993e756266` |
| Mandate-stay grant/dissolution | 42 / 42 | `terminated` | `bb487adce5391e60d4c14f4a863c2cbb1dce265eeeb6d84985cd370c3bc78e40` | `59ea3a57185d9e200436ceafb618ffbc6de8ccfcdb5e75dd43c45fea33a18a1e` | `94161b19dd755bdbdcb3cdd0a8293035b3a8358a798ae5d078b2d1377d0df55e` |

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

These source gates establish provenance and internal consistency. The completed workflow and
release gates below establish exact technical closure and installability. Neither set establishes
accessibility conformance, legal correctness, practical realism, or independent professional
review.

## Completed workflow and release gates

The final artifacts verify all of the following:

1. the frozen workflow contains exactly 20 stages, 77 operations, 11 filing routes, 14 exact
   filing bindings, 24 exact court-document bindings, two disposition-discriminated order
   branches, and 11 named deadline calculations; all 77 operations occur in the trace union;
2. all six named canonical journals round-trip through the production codec, independently
   redecide every command, replay every prefix and two full passes, recover every bound filing
   after an exact-ID `nonconforming_filing` rejection, and reach five terminated plus one
   mandate-stayed endpoint;
3. actual-history operations, disposition grounding, and actual-record questions exclude
   synthetic PA92–PA174, while all B-coded artifacts remain on the separate never-filed docket;
4. all nine resources, 72 blobs, 16 capabilities, and three exact dependencies close in the
   manifest with an exact 82-member allowlist and no undeclared, missing, symlink, or special
   entry;
5. the final level-2 review binds the exact four-pack closure, 44 non-review resources, 72 blobs,
   six traces, two record checks, 35 authorities, 159 unique non-pack evidence IDs, and all seven
   dimension-evidence partitions without claiming independent review;
6. two independent deferred exports and a frozen re-export are byte-identical; fresh exact-
   dependency installation and four-revision resolved validation pass for the same root; and
7. exactly one successful multi-trace authoring invocation changed only the manifest and review,
   with no retry or transaction residue, and the installed hostile audit rejected all 66 tested
   replay mutations.

The result is an installable level-2 authoring candidate. It is not a level-3 independent-review
or gold claim.

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

The pack-closure gate can be reproduced from the repository root:

```sh
jq empty content/m4/ellison-immunity/metadata/successor-appellate-plan.json
jq empty content/m4/ellison-immunity/pack-candidate/manifest.json

ellison_check_root="$(mktemp -d)"
ellison_catalog="$ellison_check_root/catalog"
ellison_archive="$ellison_check_root/us-ca4-m4-ellison-immunity-1.2.0.awpack"

./build/dev/src/cli/appellate-pack export-deferred \
  content/m4/ellison-immunity/pack-candidate "$ellison_archive"
./build/dev/src/cli/appellate-pack install \
  content/foundations/us-federal/foundation-us-federal-2025.12.01.awpack "$ellison_catalog"
./build/dev/src/cli/appellate-pack install \
  content/foundations/us-ca4/foundation-us-ca4-2026.03.23.awpack "$ellison_catalog"
./build/dev/src/cli/appellate-pack install \
  content/foundations/us-ca4-fictional-bench/foundation-us-ca4-fictional-bench-1.0.0.awpack \
  "$ellison_catalog"
./build/dev/src/cli/appellate-pack install "$ellison_archive" "$ellison_catalog"
./build/dev/src/cli/appellate-pack validate-resolved \
  "$ellison_catalog" us.ca4.m4.ellison-immunity 1.2.0 \
  c2a4f3bc07f05eb1429257320ed839ebaea837da7aa7330f4669bbb157168ce0
```

Schema-v2 thin roots with dependency-owned references require deferred export, exact dependency
installation, and resolved validation. Those gates passed for the exact root above; ordinary
standalone validation is not a substitute. The trace-specific codec/replay/tamper audit uses the
six frozen journals pinned above.

Exact dependency revisions are:

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
