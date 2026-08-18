# Open Grid v1.2.0 validation scaffold

Evidence status as of **2026-08-19**: all 84 synthetic sources are frozen; the
production render and schema-v2 record are accepted; the sealed-record capability
contract has been exercised against the exact record and PDFs; and seven non-workflow
core resources have been copied byte-for-byte into the pack candidate and pass schema,
digest, and resolved-reference checks. Workflow authorship, canonical replay traces,
manifest closure, promoted realism-review evidence, root revision, deterministic
archive, resolved installation, and focused release tests remain **PENDING**.

## Frozen evidence pins

| Evidence item | Current value | Status |
| --- | --- | --- |
| Source-freeze commit | `695e97cc50d08154f41931408e235d06edb7a4e6` | Frozen source chronology |
| Plan-order path-framed source closure | `ec7ab1ce6ff02b2005f672f1da03c75fb51aac5eb3b95b97ac3ddeeeab6fb1c1` | Frozen: 84 sources / 654 logical pages |
| Successor document-plan SHA-256 | `6140a4a12e6541caecc8f723dc627c687ef07a6946ccc12674b12af14befc554` | Frozen planning/allocation contract |
| Render-plan SHA-256 | `6d295f20cb9603bc15a9533ffac80b28d51988f3aa80d24e68cdacb06c73abf2` | Frozen: schema v1 / 84 exact projection rows |
| Renderer binary SHA-256 | `7ff87b82adf939ac2015039c77ed68f5e106b797db5434e6c88905c9622ace16` | Accepted first-render executable |
| Accepted checked-in render-inventory SHA-256 | `ed2f3b38bd36cbdd3bd6cf06a81e3690c23bc22cba743d713b5a0ee1c2049430` | Frozen: 84 PDFs / 654 physical pages |
| Canonical compact inventory SHA-256 | `1b6178ef21f0076a2b56ad0ecf458ba8e323c4b3f6d936cbf13602ee9107a216` | Checked-in inventory after `jq -S -c` canonicalization |
| Record-resource SHA-256 | `4fb13f25af4e06234cfa0ffbb0c0f77b7476ad7c65ed9365cc9642cb38f27f5a` | Frozen: 3 dockets / 84 entries / 654 anchors / 17 disclosures / 172 mappings |
| Docket closure | `bac655da9d5608b920628128f06ad60549417a767a47d5c0d1745b2004916aa2` | Frozen exact three-docket projection |
| Docket-entry closure | `5d901c0f5f0a7e353c9d2017167a23e9c0b9766470c81b23d89a09d6607955dd` | Frozen exact 84-entry projection |
| Page-anchor closure | `4cd964c86938c2f9678a37fe8ae0ad38982d3a78c87e337c0b40df9ae9e9b0b1` | Frozen exact 654-anchor projection |
| Disclosure closure | `8b1bee4b0223df7046472724b2cd6134a40458cc57f0b2cc43dab2cff3ef5b60` | Frozen exact 17-disclosure projection |
| Stable-mapping closure | `4d781e1e3e086d32c8a0844869519bbe7842019de5b3950494fdd01f072c33e1` | Frozen exact 172-mapping projection, including 22 named anchors |
| Inventory-order raw PDF corpus SHA-256 | `e7deff244cc7d180c9726bd8580ff335cd2754cb34d5ddbc0c3ae044c1ac695f` | Frozen accepted inventory order |
| Object-path-framed PDF corpus SHA-256 | `ab5dcc9df062758bc9d58eddb24657fbdeea71dc7afe47a1a04ef1a29072a9fe` | Frozen: object path + NUL + bytes + NUL |
| PDF corpus size | `3,553,597` bytes | Frozen: 84 accepted PDFs |
| Render-plan commit | `1c434fa299512e18a605d5a782c79f224eb2db92` | Exact one-file render-plan scope |
| Rendered-record commit | `23acdf633e5c8e5b29bbe2165f7cb49820d14e78` | Exact inventory + record + 84-PDF scope |
| Actual argument resource SHA-256 | `c403426fbe41b17bc5c32d6215625f8470a4d859e38cb0c63f417fcae4ba8cc1` | Frozen: 12 actual-record questions |
| Counterfactual argument resource SHA-256 | `17c3e9e0751ac5ebca9499e19d360993250b6bda5fdc1f838001ac72468572e1` | Frozen: 12 counterfactual questions |
| Authority-set resource SHA-256 | `5d05989cdfbe0b845aa33f48754b711c4541fece66481c29430b03869ecb9a8a` | Frozen: 9 case-specific primary-source entries, cutoff 2026-08-19 |
| Bench-configuration resource SHA-256 | `4943c36f07b46c3a365e0d6c31d8e98a9b1f48caee3ba434fb1232945b1de237` | Frozen: Vale / Quill / Reed |
| Case resource SHA-256 | `472ea3d8d67c6bc4d2cada60ea98e69529ed6a19749833d1db218d5448f444f2` | Frozen: four issues / two asymmetric disposition plans |
| Procedure-profile resource SHA-256 | `d570c49a8c8e26845228305d0df63909e950c1192b5ece5a429693b07dcece24` | Frozen core resource |
| Pack-relative path-framed seven-resource core closure | `8d245455f20b896d9333e414db5a6d30520c948166bb9797c99cd916fd1ebb4f` | Frozen order: actual argument, counterfactual argument, authority, bench, case, procedure, record |
| Promoted-core commit | `5d24fcf01620fc90a0a43623e2345120a2097dd3` | Frozen exact core promotion |
| Actual question-bank grounding digest | `bdbfefea95255ef6d323ce0baebf27a5bb0f93f73484cc68df1e6e5e59b1c23b` | Independently recomputed from frozen authority/record bytes |
| Counterfactual question-bank grounding digest | `68f4d32cb47237aea128c33d638cafd3529a8cc1a807d43dd970b4b70d49b3f8` | Independently recomputed from frozen authority/record bytes |
| Actual disposition digest | `523c7bc9574fd76f87c4b67955d7366a30a9cc6c30847c9fbffbf6df87d6b8d0` | Independently recomputed from the four-component actual plan |
| Counterfactual disposition digest | `6309ed33d1d12a0dd64c88b1be3951368fc9e15a6586b065d7886e60bb7428e6` | Independently recomputed from the four-component full-affirmance plan |
| Realism-review scaffold SHA-256 | `d5a1b604bee39e299674ecd56196195e0629cb78bfc5e21dd083f1605bb7045f` | Schema-valid level-2 scaffold; `independent_review_pending`; no `evidence` field |

## Explicitly pending artifacts and gates

| Artifact or gate | Current value | Status |
| --- | --- | --- |
| Workflow resource SHA-256 | **PENDING** | Candidate authorship, hostile review, and promotion are not frozen |
| Canonical trace set | **PENDING** | No production journal, command/event count, or trace digest is claimed |
| Realism evidence | **PENDING** | Scaffold is not authored evidence and deliberately omits `evidence` |
| Manifest SHA-256 | **PENDING** | No final resource/blob/capability closure is claimed |
| Promoted realism-review SHA-256 | **PENDING** | Evidence authoring must follow workflow and trace freeze |
| Exact evidence-closure digest | **PENDING** | No evidence envelope exists |
| Root revision | **PENDING** | Candidate is not yet installable |
| Deterministic archive | **PENDING** | No archive size or digest is claimed |
| Resolved installation audit | **PENDING** | Requires a frozen root and exact dependencies |
| Open Grid focused/full-suite tests | **PENDING** | No case-test completion claim is made |

The source-faithful workflow preflight identifies five labels only: actual ordinary;
counterfactual rehearing denial; counterfactual mandate-stay denial after rehearing
denial; counterfactual rehearing grant ending at mandate-stayed; and counterfactual
rehearing grant through supplemental merits, revised judgment, and mandate. The
documents do not support a standard sixth execution. Those labels do not reserve a
future workflow hash, structural count, trace count, replay result, or success claim.

## Completed source, render, record, and core gates

The following claims are confined to the frozen source/render/record/core bytes:

1. The source allocation resolves 37 lower sources to JA1–JA290, 19 actual appellate
   sources to PA1–PA155, and 28 never-filed counterfactual sources to PA156–PA364. The
   arithmetic is 37 + 19 + 28 = 84 documents and 290 + 155 + 209 = 654 pages.
2. The lower-record categories are exactly 5 request-chain, 2 pleading,
   4 search-declaration, 4 public/sealed Vaughn, 6 public/sealed sample-record,
   6 cross-summary-judgment, 5 seal/in-camera, and 5 judgment/docket/initiation PDFs.
   The 37 PDFs and 290 JA pages exceed the matrix floor without padding.
3. The accepted production render emitted exactly 84 US Letter PDFs and 654 physical
   pages. Every PDF passed structural and page-count checks; source and output hashes,
   byte sizes, titles, JA/PA ranges, and searchable footer labels match the accepted
   inventory and record. The 84 PDF hashes and semantic-render hashes are unique.
4. The schema-v2 record has exactly three dockets, 84 entries, and 654 unique anchors.
   JA1–JA290 and PA1–PA364 are each contiguous. Every B entry uses only the
   counterfactual docket and carries `counterfactual_appellate_branch`, `never_filed`,
   and `never_occurred_on_actual_docket` tags.
5. Seventeen equal-page public/sealed pairs resolve through exactly 172 bijective
   mappings, including the 22 named stable-anchor subjects. Public projection excludes
   all 17 sealed entries. Exact-disclosure grant adds only the authorized counterpart;
   revocation restores public-only projection; another record closure is rejected.
6. Deferred per-open verification rejects missing, symlinked, truncated, wrong-digest,
   malformed-PDF, and wrong-page-count controlled objects. A successful authorized open
   preserves leased-snapshot isolation. These capability results apply only to the exact
   record and PDF bytes; they are not workflow-trace or release evidence.
7. The seven promoted non-workflow resources are byte-identical to their frozen
   candidates, validate against schema-v2 resource schemas, and pass resolved-reference
   checks against the frozen record and exact dependencies.
8. Actual and counterfactual grounded-question digests recompute from exact authority
   propositions/provenance and record asset/anchor bytes. Actual questions exclude
   PA156–PA364; the counterfactual bank may use those separately docketed pages.
9. Actual and counterfactual disposition digests recompute from the case plans. The
   actual plan affirms the narrow operational-decision-tree 7(E) target, vacates the
   categorical 7(F), segregability, and overbroad-sealing targets, and remands. The
   isolated corrected-premise plan affirms all four targets.
10. Search/production arithmetic is consistent across the record: 16,842 raw hits,
    1,284 deduplicated candidates, 46 custodian-reviewed items, 12 responsive records,
    200 source pages, and 146 produced images comprising 68 unredacted and 78 redacted.
    The later release is exactly 17 passages on 11 replacement images—six double and
    five single—and does not change the responsive universe or image count.
11. All nine case-specific authorities carry exact official-source versions, locators,
    checked-on dates, precedential status, and propositions as of the 2026-08-19 cutoff.
    Automated checking does not itself establish their legal application or currency
    after that cutoff.

These gates establish provenance, access-control behavior, and internal consistency for
the completed slice. They do not establish workflow reachability, trace replay,
release closure, deterministic export, resolved installability, accessibility
conformance, legal correctness, practical realism, or independent review.

## Pending workflow and release gates

Before promotion, exact evidence must establish all of the following:

1. a schema-valid workflow whose operation, filing-route, exact public-document binding,
   disposition, deadline, stage, actor, authority, and branch references resolve against
   the frozen resources without binding a sealed entry;
2. production-engine replay and hostile mutation rejection for the five source-faithful
   paths, with actual/counterfactual and mutually exclusive counterfactual stages fenced;
3. actual-history exclusion of PA156–PA364 from actual operations, disposition grounding,
   and actual-record questions;
4. exact enforcement of the Rule 40 response boundary and the rehearing-grant/revised-
   judgment representation without inventing an unsupported source document or sixth
   route;
5. manifest closure over every finalized resource, blob, capability, and exact dependency,
   with no undeclared or missing member;
6. a promoted realism review with exact evidence references, while retaining
   `independent_review_pending` unless a qualified reviewer supplies attributable review;
7. two byte-identical deferred exports, fresh exact-dependency installation, resolved
   validation, and focused case/UI tests locally and under `TZ=UTC`.

No pending label above reserves a future hash, count, or success result.

## Procedural and merits consistency gates

Final validation must preserve these authored boundaries:

- The operational 7(E) conclusion is confined to nonpublic trigger order and
  implementation relationships, with foreseeable-circumvention harm addressed where
  required; general purpose, headings, administration, and other nonexempt matter do not
  become protected merely by appearing in a playbook.
- The actual 7(F) showing is categorical and insufficiently tied to particular withheld
  segments; the actual segregability account does not demonstrate the required contextual
  line review; and the actual district sealing order is overbroad.
- B01 alone supplies the isolated corrected 7(F), segregability, and sealing proof while
  holding the operational 7(E) evidence constant. It creates no second district judgment
  and no actual-history fact.
- The 17 public/sealed pairs use independently worded public redactions and harmless inert
  labels in controlled copies. They disclose no real technique, condition, sequence,
  channel, protective assignment, infrastructure detail, credential, or vulnerability.
- Rule 40 and Rule 41 dates are branch-specific authored consequences. Rehearing denial,
  mandate-stay denial, rehearing grant, mandate-stayed, supplemental merits, and revised
  mandate documents cannot be combined into one chronology.
- Vale, Quill, and Reed are synthetic/composite profiles. Deterministic questioning does
  not model a real judge or predict an actual outcome.

## Reproduction scaffold

The completed-slice gates can be rechecked from the repository root with the repository's
schema validator, reference-audit harness, `sha256sum`, `jq`, `qpdf`, `pdfinfo`, and PDF
text extraction tools. The exact workflow and release commands remain intentionally absent
until those artifacts freeze. The future root must use deferred export because its
schema-v2 references depend on exact-pinned foundation resources.

Expected dependency revisions are:

- `foundation.us-federal@2025.12.01` —
  `866c90996c15e2076b9508a297ffce1a4e766b1432a9e11d08e8138c57e363c9`;
- `foundation.us-ca4@2026.03.23` —
  `449d75c77e5c47883f750377450f2d1ec1fc0e42e20b1f247446b208661d3262`;
- `foundation.us-ca4-fictional-bench@1.0.0` —
  `cee0bf93309cc9ad800f215a47d734b20a9fdf5dc889f2f440e4382b942d332d`.

## Independent-review gate

Automated checks and authoring review cannot establish FOIA, 7(E), 7(F), Vaughn,
segregability, sealing, or current Fourth Circuit accuracy, practical realism, or level 3.
A qualified FOIA, cybersecurity-records, sealing, and appellate practitioner must review
an exact finalized root. Any change to the record, authority set, questions, workflow,
traces, manifest, or dependency topology invalidates earlier exact evidence.
