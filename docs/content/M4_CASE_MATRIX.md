# M4 Fourth Circuit case matrix

Checked against official primary sources on **2026-08-11**. This is an authoring contract, not
legal advice or independent legal review.

## Frozen breadth and evidence floor

The launch content has one jurisdiction, `us-ca4`, and four procedure profiles: six civil
appeals, one criminal appeal, one agency review, and one original writ. The nine root packs have
an aggregate floor of exactly **283 unique substantive, searchable lower-tribunal or certified-
agency-record PDFs** linked by 283 docket-to-asset relationships:

| Case | Profile | PDFs | Minimum substantive pages | Gold candidate |
| --- | --- | ---: | ---: | --- |
| Benton retaliation | Civil appeal | 37 | 260 | No |
| Asterglen Rule 54(b) | Civil appeal | 37 | 230 | Civil |
| Norvale injunction | Civil appeal | 24 | 149 | No |
| Ellison immunity | Civil appeal | 37 | 275 | No |
| Blue Ember JMOL | Civil appeal | 42 | 430 | No |
| Open Grid FOIA | Civil appeal | 37 | 250 | No |
| Serrano waiver | Criminal appeal | 28 | 260 | Criminal |
| A.R.M. CAT | Agency review | 18 | 220 | Agency |
| Cinder Lake privilege | Original writ | 23 | 180 | Writ |
| **Total** | **Four profiles** | **283** | **2,254** | **Four** |

Reusable forms, shared authorities, generated appellate filings, counterfactual training
documents, and optional audio/video blobs do not count toward 283. Blank or padding pages do not
count toward the page floor. A root may therefore contain more total blobs than its row without
changing this breadth floor. The existing Asterglen v1 `0.1.0` pack remains immutable on its EDVA
`SYN-25-0117` and appellate `SYN-26-1427` identities. Its expanded schema-v2 successor is a new
`0.2.0` revision with no reused or relabeled predecessor PDF. The successor's independent NDWV
record is 37 PDFs/234 JA pages, satisfying the 37-PDF/230-page floor; its complete root
adds 13 actual appellate PDFs/70 PA pages and 25 isolated branch PDFs/73 PA pages, for 75 PDFs/377
anchors. The final root is
`7e77bc0fbe02dc9e108681df73852859d6d0f577acdcb65fcfb7678eac78b728`; its evidence closure is
`445c3f11dcc8046eedfc233407699cbbb3ea4e39425d22c976808959350ca62c`.

The finalized A.R.M. `1.2.0` candidate demonstrates that distinction: its certified agency
record is 18 PDFs/238 AR pages, satisfying the 18-PDF/220-page floor, while its complete root is
54 PDFs/415 pages after adding 36 post-agency PDFs/177 PA pages. PA1–PA127 belong to the actual
appellate docket (with PA1–PA8 still extra-record), and PA128–PA177 is an isolated, never-filed
counterfactual bank.

The finalized Benton `1.2.0` candidate follows the same counting rule. Its lower-court record
remains 37 PDFs/262 JA pages, satisfying the 37-PDF/260-page floor, while its complete root is
67 PDFs/389 pages after adding 13 actual appellate PDFs/70 PA pages and 17 separately docketed,
counterfactual PDFs/57 PA pages. The PA71–PA127 documents never occurred on the actual appeal and
do not change the frozen 283-PDF lower-tribunal/certified-agency breadth total.

The frozen Norvale source/render/record bytes likewise refine only its page floor, not the
283-PDF breadth total. Its lower-court record is 24 PDFs/149 JA pages, while its complete record
is 73 PDFs/383 pages after adding 23 actual appellate PDFs/135 PA pages and 26 separately
docketed, never-filed branch PDFs/99 PA pages. PA136–PA234 never occurred on the actual appeal.
The independently hostile-reviewed workflow, nine traces, manifest, review, root, deterministic
archive, resolved install, and focused tests now form an installable level-2 authoring candidate.

The frozen Ellison source/render/record/core slice likewise refines only its page floor, not the
283-PDF breadth total. Its lower-court record is 37 PDFs/275 JA pages, while its complete record
is 72 PDFs/449 pages after adding 15 actual appellate PDFs/91 PA pages and 20 separately
docketed, never-filed counterfactual PDFs/83 PA pages. PA92–PA174 never occurred on the actual
appeal. The frozen workflow and six traces, final nine-resource/72-blob manifest, promoted
level-2 review, deterministic archive, dependency-only resolved install, and hostile runtime
audit now form an installable authoring candidate. Synthetic PA92–PA174 remain excluded from
actual-history and actual-disposition grounding.

The frozen Blue Ember source/render/record/core slice also leaves the 283-PDF breadth total
unchanged. Its lower-court record is 42 PDFs/430 JA pages, while its complete record is
83 PDFs/656 pages after adding 16 actual appellate PDFs/108 PA pages and 25 separately docketed,
never-filed counterfactual PDFs/118 PA pages. PA109–PA226 never occurred on the actual appeal.
The frozen workflow and six traces, final nine-resource/83-blob manifest, promoted level-2
review, deterministic archive, dependency-only four-revision install, and 78-mutation hostile
runtime audit now form an installable authoring candidate. Frozen focused integration/UI tests
also pass normally and under UTC with zero skips. Synthetic PA109–PA226 remain excluded from
actual-history, actual-disposition, and actual-record question grounding.

The frozen Open Grid source/render/record/core slice also leaves the 283-PDF breadth total
unchanged. Its lower-court record is 37 PDFs/290 JA pages, while its complete record is
84 PDFs/654 pages after adding 19 actual appellate PDFs/155 PA pages and 28 separately docketed,
never-filed counterfactual PDFs/209 PA pages. PA156–PA364 never occurred on the actual appeal.
Seventeen public/sealed disclosure pairs close through 172 bijective page mappings, including
22 named stable-anchor subjects, and the exact record capability gate covers public-default
projection, disclosure-specific session grant, revocation, exact-closure binding, and deferred
per-open CAS verification. Workflow, traces, realism evidence, manifest, root, archive, resolved
install, and tests remain pending; the level-2 scaffold is not authored evidence.

Every brief proposition, oral-argument question, and disposition reason must resolve to a
rendered `JA`, `SJA`, `AR`, or `PA` page. All parties, people, dockets, addresses, facts, and
records are fictional. Simulated docket numbers begin `SYN-`. Real courts, sovereigns, and
agencies appear only as institutional procedural actors.

## Exact shared dependencies

Each v2 root exact-pins:

1. `foundation.us-federal@2025.12.01`: national rules, calendar, and common federal authorities.
2. `foundation.us-ca4@2026.03.23`: the court, four procedure profiles, local rules, and shared
   filing/form catalogs.
3. `foundation.us-ca4-fictional-bench@1.0.0`: reusable generic fictional/composite profiles.

Facts, case-specific authorities, records/blobs, grounded questions, workflow deltas, and
dispositions remain root-owned. No fact or record asset is shared across cases. Dependency
resolution must satisfy issues #23–#28 before bulk authoring begins.

Reusable fictional/composite profiles are Alder (clipped/direct), Rowan
(technical/measured/Socratic), Vale (plain/measured), Fen (expansive/hypothetical), Quill
(clipped/plain), March (formal/technical), Reed (clipped/technical), and Slate
(measured/formal). Their focus uses reusable topics, never a particular case's issue ID.

## Case definitions

### 1. Benton retaliation — `ca4m4.case.benton-retaliation`

*Leora Benton v. Blue Cedar Compliance, Inc.* — `SYN-CA4-26-CV-4101`,
`SYN-EDVA-25-CV-0412`. Rowan presides with Alder and Fen.

- **Wedge:** Title VII retaliation summary judgment. Protected opposition and EEOC
  participation precede a reduction in force by six weeks. Comparator scores and shifting
  explanations remain admitted; one late comparator declaration is excluded.
- **Record:** 5 pleadings/initiation, 4 exhaustion/policy, 8 fact exhibits, 7 depositions,
  6 summary-judgment papers, 3 exclusion papers/order, and 4 judgment/docket/initiation items.
- **Argument:** but-for causation in the pretext framework, comparator similarity, the record
  remaining after exclusion, and relief. Counterfactual: no decisionmaker knowledge.
- **Disposition:** affirm exclusion; vacate retaliation summary judgment; remand.
- **Limits:** exact role subsets, instance predicates, document bindings, disposition-plan
  bindings, and replay enforce the authored actual and adverse branches. The engine still does
  not infer protected activity, comparator similarity, knowledge, pretext, the legal meaning of a
  novel filing, or a clerk-versus-panel classification within the shared court role.

### 2. Asterglen finality — `ca4r54b.case.asterglen`

*Asterglen Freight Software, Inc. v. Copper Kestrel Logistics, LLC and Meridian Silt Holdings,
LLC* — `SYN-CA4-26-CV-4102`, `SYN-NDWV-25-CV-0618`. Vale presides with Rowan and Alder.

- **Wedge:** live counterclaims remain. The district court grants a Rule 54(b) motion and enters
  partial judgment without expressly determining that there is no just reason for delay.
- **Record:** 6 pleadings/counterclaims, 8 contract/change-order items, 7 discovery/testimony,
  7 partial-summary-judgment items, 4 Rule 54 items, and 5 judgment/docket/initiation items.
- **Argument:** claim scope, overlap, the missing express determination, fragmentation, and why
  section 1292 is no substitute. Counterfactual: supported express findings.
- **Disposition:** dismiss for lack of appellate jurisdiction and remand; do not reach the
  contract merits, do not vacate the January 12 partial-summary-judgment order, and expressly
  direct each side to bear its own appellate costs.
- **Migration:** preserve the audited EDVA/appellate v1 pack byte-for-byte and author the new NDWV
  37-PDF/234-page lower record as `0.2.0`; add 13 actual PA PDFs and 25 never-occurred branch PDFs
  without counting either class toward the frozen lower-tribunal breadth total.
- **Evidence status:** nine resources, 16 capabilities, three structured dispositions, and eight
  production traces form the installable successor. Authoring review is level 2 throughout and
  `independent_review_pending`; the exact render, resolved install, and evidence closure pass.

### 3. Norvale injunction — `ca4m4.case.norvale-injunction`

*Piedmont Booksellers Guild v. City of Norvale* — `SYN-CA4-26-CV-4103`,
`SYN-DSC-26-CV-0107`. Quill presides with Fen and Vale.

- **Wedge:** preliminary relief against a content-neutral paid-speaker permit for recurring
  public book fairs, plus district-first appellate-stay practice.
- **Record:** 5 pleading/ordinance/history, 6 declaration/photo/calendar, 4 preliminary-
  injunction, 2 hearing/order, 4 district-stay, and 3 docket/initiation items.
- **Consequences:** failure to seek district relief or explain impracticability defeats the Rule
  8 motion. Recurring events prevent an artificial one-date mootness premise.
- **Disposition:** deny appellate stay; affirm entitlement to interim relief; remand only for a
  corrected Rule 65(c)/(d) order without dissolving protection.
- **Evidence status:** the accepted record contains 24 lower PDFs/149 JA pages, 23 actual
  appellate PDFs/135 PA pages, and 26 never-filed branch PDFs/99 PA pages, for 73 PDFs/383
  anchors. The frozen workflow SHA-256 is
  `1b285f65a38c4be2a7bc8dbe29d3822aee2963d05019fbe5f79d5917272cc74a`:
  16 stages, 90 operations, 13 filing routes, 24 exact filing bindings, 31 document bindings,
  two disposition bindings, and 14 concrete deadline IDs representing 12 logical concepts.
  Nine canonical traces contain 316 commands and 334 events with exact 90-operation coverage.
  The 16-capability root is
  `a51383c0c1edcd56153b36291177425b09846ab607c39c28030820ef700df05f`,
  with evidence closure
  `1170d682b46773d09b63b5dcfcd5b7c485c2f792881c94027b76550ef021d82c`.
- **Limits:** stay practice is serialized before merits briefing. The model does not infer
  concurrent clocks, impracticability, mootness, or the legal meaning of arbitrary filings;
  shared court-role authorization cannot itself distinguish clerk from panel. The structured
  disposition also has no separate non-vacatur/continued-protection field, so that limitation
  remains an exact narrative contract. The B02, B05, and B07 Rule 8 exercises are scoped
  appellate-stay endpoints; no complete alternate notice, record, merits, or mandate docket is
  claimed for them.

### 4. Ellison immunity — `ca4m4.case.ellison-immunity`

*Mara Ellison v. Officer Nolan Rusk* — `SYN-CA4-26-CV-4104`,
`SYN-WDVA-25-CV-0733`. Alder presides with Reed and March.

- **Wedge:** collateral-order review of qualified-immunity denial after a welfare-check seizure;
  obstructed body-camera footage leaves resistance and threat facts disputed.
- **Record:** 4 pleadings, 6 report/dispatch/medical, 6 bodycam transcript/frame/authentication,
  8 deposition/expert, 7 immunity/SUMF, 1 order, and 5 docket/initiation PDFs. The complete
  PDF-only corpus adds 15 actual appellate and 20 never-filed counterfactual PDFs. No MP4 is
  included.
- **Argument:** separate unreviewable factual sufficiency from reviewable legal questions on
  plaintiff-favorable assumed facts; address why obscured camera evidence is not dispositive,
  incident-date clearly established law, intervening *Barricks*/*Zorn*, and exact relief.
- **Disposition:** actual—dismiss the factual-sufficiency target, affirm the Fourth Amendment
  legal ruling on assumed facts, and affirm the clearly-established-law denial of qualified
  immunity. Adverse—dismiss the factual target, assume constitutional excessiveness without
  deciding it, and reverse only the qualified-immunity target for summary judgment to Rusk.
- **Evidence status:** source, render, and the 72-entry/449-anchor record are frozen. Workflow
  `cd69b276a63ae508ba0d98bbee15585847a405b5b55a48df69fffe45811ca23a` and successor plan
  `82f1afa17e4d15a192cc6567ff3ffaa3415d2dd95f609aa58c760c026c78273d` bind six canonical
  traces with 229 commands/events, all 77 operations, 14 same-ID filing recoveries, 24 court
  documents, 11 deadlines, and five terminated plus one stayed endpoint. Manifest
  `8f0d614a73a4850a93170a7338229b64e2b1d042134e678785eaab481fd8ca42` closes nine resources,
  72 blobs, 16 capabilities, and three exact dependencies. Review
  `5545977962535b58f029de10959cf2f9e49348a12fb4f7bff17574aa688b8867` binds closure
  `8032c5547dd522cad241b9c816bd611d198dbe7e007b0cd29781b8b471de41ac`; exactly one successful
  authoring invocation produced root
  `c2a4f3bc07f05eb1429257320ed839ebaea837da7aa7330f4669bbb157168ce0` and the deterministic
  4,230,462-byte archive
  `59f32f521644bac61865cf1e59444fc98dbb9007461a1709272ffe261cbad1d0`.
  The review remains level 2 and `independent_review_pending`.
- **Limits:** the engine does not classify arbitrary argument as legal or factual, inspect video,
  infer disputed movement/resistance/threat/credibility, treat post-incident *Barricks* or *Zorn*
  as February 2025 notice, combine mutually exclusive Rule 41 paths, or model real judges.

### 5. Blue Ember post-trial — `ca4m4.case.blueember-jmol`

*Blue Ember Biologics, LLC v. Granite Heron Logistics, Inc.* — `SYN-CA4-26-CV-4105`,
`SYN-WDNC-24-CV-0520`. March presides with Rowan and Slate.

- **Wedge:** Rule 50(a) challenges causation only; Rule 50(b) first raises mitigation. The court
  grants JMOL on mitigation and conditionally grants a Rule 59 damages retrial.
- **Record:** 7 pleading/contract, 7 discovery/expert, 4 pretrial, 6 trial transcript, 7 trial
  exhibit, 4 Rule 50(a)/verdict/judgment, 5 Rule 50(b)/59, and 2 docket/notice PDFs: 42 lower
  PDFs/430 JA pages. The complete PDF-only corpus adds 16 actual appellate PDFs/108 PA pages and
  25 separately docketed, never-filed counterfactual PDFs/118 PA pages, for 83 PDFs/656 anchors.
- **Argument:** exact preservation language, renewed-motion boundaries, Rule 50 versus Rule 59
  review, and permitted relief. On the actual record, both Rule 50(a) motions challenge causation
  only and mitigation first appears under Rule 50(b). B01 alone supplies the isolated,
  never-filed counterfactual premise that both pre-verdict motions expressly preserve mitigation.
- **Disposition:** actual—reverse mitigation JMOL, affirm the conditional Rule 59 ruling and
  damages-only scope, vacate the amended judgment, and remand for a damages retrial.
  Counterfactual—affirm mitigation JMOL and the amended judgment without deciding the conditional
  Rule 59 ruling or retrial scope.
- **Evidence status:** source, render, the 83-entry/656-anchor record, and seven non-workflow core
  resources are frozen. Source closure
  `c5e843bfc968a726ee86a0d130cd8b85e89c74e76dc75239191a20048a9054a5`, render inventory
  `c52eab8d01e68ec08f5f43e07e8ef2fdc7996ddc15cace7ab4b818518b51f89a`, and record
  `080ff7772d73131a5471f2fc530b4d63c6215831a82ffcd671ef50beff8d1c7a` bind that slice. Workflow
  `7c2356718286505eee16d62b48ca281f92eee367c9e21319ddcae02d87c1a120` and successor plan
  `664b8632be87d885cebc4625282f0b452c5d376be607bc889ee86012c3ddcee5` bind six traces with
  270 commands/events, all 93 operations, 18 unique filing IDs and 61 same-ID recoveries,
  25 court documents, 14 deadlines, and five terminated plus one stayed endpoint. Manifest
  `2b545cee1aaba7a1475b2f5085ae93d50ec9e3255a68f9762d3bec63492a8dac` closes nine resources,
  83 blobs, 16 capabilities, and three exact dependencies. Review
  `8fe8d9b06f38ca16fe535c917c3da4b2c6d92a5ee2f17924d49a945d2e5e0688` binds closure
  `3f38cd1a12f7f61c037f338fef4f1600ab83434208aa13a8ff5cf56a52fe5d5a`; exactly one valid
  authoring call produced root
  `08d88e4811e8ed8ad6e642cc041365508808f7158862aa93199de867f31431ec` and the deterministic
  5,326,158-byte archive
  `c6332ae33e351ccb27ed17b5576b147a47f9f5f0b44583365212b1781a288ed2`. Fresh four-revision
  resolved installation and the 78-mutation hostile audit passed. Exact three-file focused-test
  commit `1d497aecd9e9fdf2ab5ffa4f47ad994052cd304f` passed normal and UTC integration/UI runs with
  zero skips. The review remains level 2 and `independent_review_pending`.
- **Limits:** the engine does not infer Rule 50 preservation, distinguish a new ground from a
  variation on a preserved issue, classify arbitrary posttrial filings, evaluate mitigation or
  damages evidence, combine mutually exclusive Rule 41 paths, or model real judges. The generated
  corpus has no MP4 or raw logger, assay, laboratory, or device-data file. Synthetic PA109–PA226
  cannot ground the actual history, actual disposition, or actual-record question bank.

### 6. Open Grid FOIA — `ca4m4.case.opengrid-foia`

*Open Grid Archive v. United States Department of Energy* — `SYN-CA4-26-CV-4106`,
`SYN-DMD-25-CV-0914`. Vale presides with Quill and Reed.

- **Wedge:** FOIA Exemptions 7(E) and 7(F), Vaughn specificity, segregability, and paired
  public/sealed filings concerning cyber-response playbooks.
- **Record:** 5 request-chain, 2 pleading, 4 search-declaration, 4 public/sealed Vaughn,
  6 public/sealed sample-record, 6 cross-summary-judgment, 5 seal/in-camera, and 5
  judgment/docket/initiation PDFs: 37 lower-record PDFs/290 JA pages. The complete PDF-only
  corpus adds 19 actual appellate PDFs/155 PA pages and 28 separately docketed, never-filed
  counterfactual PDFs/209 PA pages, for 84 PDFs/654 anchors.
- **Argument:** law-enforcement purpose, technique versus guideline/risk, Vaughn detail,
  segregability, 7(F), foreseeable harm, and least-restrictive sealing. The actual bank excludes
  PA156–PA364; the counterfactual bank uses only the isolated corrected premise and docket.
- **Disposition:** actual—affirm 7(E) only for the operational decision-tree target; vacate the
  categorical 7(F), segregability, and overbroad-sealing targets; remand. Counterfactual—hold the
  operational 7(E) showing constant, assume item-specific 7(F), documented line review, and
  narrow sealing findings, and affirm all four targets.
- **Confidentiality boundary:** the schema-v2 record closes 17 equal-page public/sealed pairs
  through 172 bijective mappings, including 22 named stable-anchor subjects. Public-default
  projection, exact-disclosure session grant, revocation, exact-closure binding, and deferred
  per-open path/size/digest/readability/page-count verification implement
  `workbench.pack.sealed-record-twins@1` for the exact synthetic bytes. No controlled copy
  contains actionable real cyber-response information.
- **Frozen closure:** source
  `ec7ab1ce6ff02b2005f672f1da03c75fb51aac5eb3b95b97ac3ddeeeab6fb1c1`; PDF
  `ab5dcc9df062758bc9d58eddb24657fbdeea71dc7afe47a1a04ef1a29072a9fe`; record
  `4fb13f25af4e06234cfa0ffbb0c0f77b7476ad7c65ed9365cc9642cb38f27f5a`; seven-resource core
  `8d245455f20b896d9333e414db5a6d30520c948166bb9797c99cd916fd1ebb4f`.
- **Workflow/release status:** source chronology supports five preflight labels only—actual
  ordinary; counterfactual rehearing denial; counterfactual mandate-stay denial after rehearing
  denial; counterfactual rehearing grant ending at mandate-stayed; and rehearing grant through
  supplemental merits, revised judgment, and mandate. The sources do not support a sixth
  execution. Workflow, traces, realism evidence, manifest, root, archive, resolved install, and
  tests remain **PENDING**; the schema-valid level-2 scaffold has no `evidence` field and makes no
  installability, level-3, or gold claim.

### 7. Serrano waiver — `ca4m4.case.serrano-waiver`

*United States v. Mateo Serrano* — `SYN-CA4-26-CR-4201`, `SYN-MDNC-25-CR-0328`.
Slate presides with Alder and March.

- **Wedge:** a knowing waiver covers Guidelines calculation but expressly reserves complete
  denial of allocution; Serrano also contests a section 3B1.1 enhancement.
- **Record:** 4 charging/initial, 4 pre-plea, 4 plea/Rule 11, 4 PSR/objection, 4 sentencing
  submission, 3 transcript/order, and 5 judgment/docket/initiation PDFs.
- **Consequences:** a day-15 notice is nonjurisdictionally late; prompt government invocation
  produces dismissal. The waiver motion dismisses only the enhancement target.
- **Disposition:** dismiss the enhancement appeal under the waiver; vacate sentence and remand
  for resentencing with allocution.
- **Confidentiality boundary:** realistic PSR treatment must use and replay the implemented
  `workbench.pack.sealed-record-twins@1` policy; the Serrano pack has not authored that evidence.

### 8. A.R.M. agency review — `ca4m4.case.arm-agency`

*A.R.M. v. Attorney General of the United States* — `SYN-CA4-25-AG-4301`,
`SYN-BIA-25-0113`. Rowan presides with Reed and Quill.

- **Wedge:** a final BIA removal order fails to aggregate CAT risk or address admitted official-
  acquiescence evidence. The certified record omits one admitted declaration; petitioner also
  tenders a genuinely new declaration.
- **Record:** 2 NTA/pleading, 4 application/declaration/family, 3 country/medical/translation,
  2 IJ transcript, 1 IJ decision, 3 BIA notice/brief/response, 1 BIA final order, and
  2 certified-index/omission PDFs.
- **Consequences:** restore admitted omitted material; reject genuinely new extra-record material.
  A day-31 petition is nonjurisdictionally late and requires government invocation.
- **Disposition:** grant correction for the admitted declaration, deny supplementation, grant the
  petition in part, vacate the BIA order, and remand.
- **Limits:** exact role subsets, filing/order-instance guards, static deficiency deadlines, and
  bound order/judgment/mandate documents now enforce the authored actual and adverse branches.
  The engine still does not infer final-order status or the legal meaning of novel filings, and
  parallel stay-and-merits work remains out of scope. The day-31 invocation and record-motion
  semantics are therefore authored exact-byte document bindings validated by replay, not
  universal legal classifiers or claims of independent legal review.

### 9. Cinder Lake writ — `ca4m4.case.cinderlake-writ`

*In re Cinder Lake Health Network, Inc.* — `SYN-CA4-26-WR-4401`,
`SYN-EDNC-25-CV-0882`. Alder presides with Vale and Fen.

- **Wedge:** a compliance response letter is treated as broad subject-matter waiver, and the
  district court orders privileged investigation files produced within seven days.
- **Record:** 3 pleading, 2 protective-order/ESI, 4 audit/legal-communication, 2 privilege-log,
  3 compel, 2 hearing/order, 3 reconsideration, 3 stay, and 1 district-docket PDF.
- **Consequences:** missing service or essential materials produces deficiency or summary denial;
  an answer is not due unless ordered. Actual-record argument requires an express expedited-
  argument order because Local Rule 21 ordinarily omits argument.
- **Disposition:** grant in part; vacate categorical waiver/production provisions; deny a blanket
  bar on related discovery; remand for document-specific review.
- **Limits:** emergency parallel work and conditional summary-denial/answer branching need later
  trusted capabilities.

## Official source baseline

Common sources:

- [Fourth Circuit FRAP, local rules, and IOPs (2026-03-23)](https://www.ca4.uscourts.gov/docs/pdfs/rules.pdf)
- [Federal Rules of Appellate Procedure (2025-12-01)](https://www.uscourts.gov/sites/default/files/document/federal-rules-of-appellate-procedure.pdf)
- [Federal Rules of Civil Procedure (2025-12-01)](https://www.uscourts.gov/sites/default/files/document/federal-rules-of-civil-procedure.pdf)
- [Federal Rules of Criminal Procedure (2025-12-01 official compilation; last substantively amended in 2023)](https://www.uscourts.gov/sites/default/files/document/federal-rules-of-criminal-procedure.pdf)
- [Fourth Circuit sealed/confidential materials guide](https://www.ca4.uscourts.gov/appellateprocedureguide/General_Provisions/SealedConfidMem.html)

Case-specific primary sources:

| Case | Authorities |
| --- | --- |
| Benton | [42 U.S.C. § 2000e-3](https://uscode.house.gov/view.xhtml?req=%28title%3A42+section%3A2000e-3+edition%3Aprelim%29); [Foster v. UMES](https://www.ca4.uscourts.gov/opinions/published/141073.p.pdf) (published, 2015-05-21) |
| Asterglen | [28 U.S.C. § 1291](https://uscode.house.gov/view.xhtml?edition=prelim&num=0&req=granuleid%3AUSC-prelim-title28-section1291); [28 U.S.C. § 2107](https://uscode.house.gov/view.xhtml?edition=prelim&req=granuleid%3AUSC-prelim-title28-section2107); FRCP 54(b) and 58; [Kinsale Insurance](https://www.ca4.uscourts.gov/opinions/211754.P.pdf) (published, 2022-04-20); [McPherson](https://www.ca4.uscourts.gov/opinions/231938.U.pdf) (unpublished, 2024-10-15); [Bowles](https://www.govinfo.gov/content/pkg/USREPORTS-551/pdf/USREPORTS-551-205.pdf); [Primov](https://www.courts.state.va.us/static/opinions/opnscvwp/1171381.pdf) |
| Norvale | [28 U.S.C. § 1292(a)(1)](https://uscode.house.gov/view.xhtml?edition=prelim&num=0&req=granuleid%3AUSC-prelim-title28-section1292%28a%29%281%29); FRCP 65; [Billups](https://www.ca4.uscourts.gov/opinions/191044.P.pdf); [Pashby](https://www.ca4.uscourts.gov/opinions/Published/112363.p.pdf) |
| Ellison | [42 U.S.C. § 1983](https://uscode.house.gov/view.xhtml?edition=prelim&num=0&req=granuleid%3AUSC-prelim-title42-section1983); [Graham](https://www.govinfo.gov/content/pkg/USREPORTS-490/pdf/USREPORTS-490-386.pdf); [Johnson](https://www.govinfo.gov/content/pkg/USREPORTS-515/pdf/USREPORTS-515-304.pdf); [Barricks](https://www.ca4.uscourts.gov/opinions/251250.P.pdf); [Armstrong](https://www.ca4.uscourts.gov/opinions/151191.P.pdf); [Smith](https://www.ca4.uscourts.gov/opinions/121503.P.pdf); [Meyers](https://www.ca4.uscourts.gov/opinions/112192.P.pdf); [Yates](https://www.ca4.uscourts.gov/opinions/151555.P.pdf); [Zorn](https://www.supremecourt.gov/opinions/25pdf/25-297_bqm2.pdf) |
| Blue Ember | FRCP 50 and 59; [Unitherm](https://www.govinfo.gov/app/details/USREPORTS-546/USREPORTS-546-394); [Plyler](https://www.ca4.uscourts.gov/opinions/241445.P.pdf); [Wiener](https://www.ca4.uscourts.gov/opinions/241316.P.pdf); [Gautier](https://www.ca4.uscourts.gov/opinions/241401.P.pdf); [Boley](https://www.ca4.uscourts.gov/opinions/231493.U.pdf) (nonprecedential) |
| Open Grid | [5 U.S.C. § 552](https://uscode.house.gov/view.xhtml?edition=prelim&req=granuleid%3AUSC-prelim-title5-section552); [Grey](https://www.ca4.uscourts.gov/opinions/231910.P.pdf); [Company Doe](https://www.ca4.uscourts.gov/Opinions/Published/122209.P.pdf) |
| Serrano | [18 U.S.C. § 3742](https://uscode.house.gov/view.xhtml?edition=prelim&num=0&req=granuleid%3AUSC-prelim-title18-section3742); [Boutcher](https://www.ca4.uscourts.gov/opinions/204248.P.pdf); [Jennings](https://www.ca4.uscourts.gov/opinions/244027.P.pdf); [Marsh](https://www.ca4.uscourts.gov/Opinions/184609.P.pdf); [2025 Guidelines Manual](https://www.ussc.gov/guidelines/2025-guidelines-manual) |
| A.R.M. | [8 U.S.C. § 1252](https://uscode.house.gov/view.xhtml?edition=prelim&f=treesort&num=0&req=%28title%3A8+section%3A1252+edition%3Aprelim%29+OR+%28granuleid%3AUSC-prelim-title8-section1252%29); [8 C.F.R. pt. 1208 (2025-01-01)](https://www.govinfo.gov/content/pkg/CFR-2025-title8-vol1/pdf/CFR-2025-title8-vol1-part1208.pdf); [Riley v. Bondi](https://www.supremecourt.gov/opinions/24pdf/23-1270_6j37.pdf); [Rodriguez-Arias](https://www.ca4.uscourts.gov/opinions/172211.P.pdf) |
| Cinder Lake | [28 U.S.C. § 1651](https://uscode.house.gov/view.xhtml?edition=prelim&num=0&req=granuleid%3AUSC-prelim-title28-section1651); [Murphy-Brown](https://www.ca4.uscourts.gov/opinions/181762.P.pdf); [In re Fluor](https://www.ca4.uscourts.gov/opinions/201241.U.pdf) (nonprecedential); [Cheney](https://www.govinfo.gov/app/details/USREPORTS-542/USREPORTS-542-367) |

Before release, each pack pins the exact source version, checked-on date, locator, official URL,
precedential status, proposition, evidence closure, and trace required by #26 and #27.

## Realism and sequencing gates

Cases begin at realism level 0. Automated traces, page/authority resolution, chronology checks,
and author review establish at most level 2. Level 3 requires a detached, exact-dependent review
pack signed off by a qualified independent reviewer; any record, question, authority, or workflow
change invalidates that review.

The A.R.M. `1.2.0` root currently records level 2 in every dimension with exact closure over four
packs, 44 non-review resources, 54 blobs, seven traces, two record checks, and 32 authorities.
Its state is `independent_review_pending`, so it remains the agency gold candidate rather than the
agency gold pack.

The Benton `1.2.0` root currently records level 2 in every dimension with exact closure over four
packs, 44 non-review resources, 67 blobs, seven traces, two record checks, and 28 authorities.
Its 148 unique non-pack evidence IDs remain bound to an `independent_review_pending` root review.
Benton is therefore an installable non-gold civil authoring candidate; Asterglen retains the civil
gold-candidate designation.

The Asterglen `0.2.0` successor currently records authoring level 2 in every dimension with
`independent_review_pending`. It contains nine resources, 16 capabilities, 75 PDFs/377 anchors,
three structured dispositions, and eight production traces. Exact root
`7e77bc0fbe02dc9e108681df73852859d6d0f577acdcb65fcfb7678eac78b728`, review
`e16caac5226fdb26fb8acead14ef0a0bfd4d569af5ba84b9da65389e5fb0c905`, and closure
`445c3f11dcc8046eedfc233407699cbbb3ea4e39425d22c976808959350ca62c` bind the accepted render and
final replay. It therefore remains the installable civil gold candidate rather than a civil gold
pack. The v0.1.0 predecessor
stays immutable and is not silently upgraded into schema v2 or level 3.

The Ellison `1.2.0` authoring candidate records level 2 in every dimension with
`independent_review_pending`. Its exact evidence envelope covers four packs, 44 non-review
resources, 72 blobs, six traces, two record checks, and 35 authorities: 159 unique non-pack
evidence IDs. The seven dimension-reference counts are 49/19/75/33/17/4/114. The installed
hostile audit made 458 command
redecisions, 229 prefix replays, 12 full replays, and 66 mutation rejections. The final root is
`c2a4f3bc07f05eb1429257320ed839ebaea837da7aa7330f4669bbb157168ce0`, with evidence closure
`8032c5547dd522cad241b9c816bd611d198dbe7e007b0cd29781b8b471de41ac`.
Ellison is therefore an installable level-2 authoring candidate, not a level-3 or gold pack.
Its PDF-only corpus contains no MP4; the engine makes no video inference, and separately
docketed synthetic PA92–PA174 cannot ground actual-history claims.

The Blue Ember `1.2.0` authoring candidate records level 2 in every dimension with
`independent_review_pending`. Its exact evidence envelope covers four packs, 44 non-review
resources, 83 blobs, six traces, two record checks, and 39 authorities: 174 unique non-pack
evidence IDs. The seven dimension-reference counts are 53/19/86/37/20/4/129. The installed
audit redecided 270 commands, replayed all 270 prefixes and 12 complete journals, and rejected
78 hostile mutations. The final root is
`08d88e4811e8ed8ad6e642cc041365508808f7158862aa93199de867f31431ec`, with evidence closure
`3f38cd1a12f7f61c037f338fef4f1600ab83434208aa13a8ff5cf56a52fe5d5a`.
Focused-test commit `1d497aecd9e9fdf2ab5ffa4f47ad994052cd304f` passed normal and UTC runs with zero skips.
Blue Ember is therefore an installable level-2 authoring candidate, not a level-3 or gold pack.
Actual Rule 50(a) preservation remains causation-only; B01's express-mitigation premise exists
solely on the separate never-filed docket. The PDF-only corpus contains no MP4 or raw instrument
data, and synthetic PA109–PA226 cannot ground actual-history claims.

The Open Grid `1.2.0` authoring scaffold records level 2 in every dimension with
`independent_review_pending` but no `evidence` field. Its frozen source/render/record/core slice
contains 84 PDFs/654 anchors across the 37/290 lower, 19/155 actual, and 28/209 isolated
counterfactual allocations. The record's 17 disclosures, 172 bijective stable mappings, and
22 named mapped subjects pass the exact public/grant/revoke/closure/CAS capability boundary.
Source chronology supports five workflow preflight labels, not a standard six-path contract.
Workflow promotion, traces, realism evidence, manifest, root, archive, resolved install, and
tests remain pending. The scaffold is therefore a documentation checkpoint, not installable
level-2 evidence, level 3, or gold; synthetic PA156–PA364 cannot ground actual-history claims.

The four gold candidates require, respectively, an appellate-finality reviewer, criminal
sentencing/waiver reviewer, immigration reviewer with an operative-CFR refresh, and
appellate-privilege/writ reviewer. Open Grid's exact sealed/public record behavior is now frozen,
but its workflow/release evidence and qualified independent FOIA/appellate review remain pending.

After #23–#28 and the three shared foundations freeze, isolated root authoring may proceed in
parallel without shared-file overlap:

- Benton + Asterglen + A.R.M.: 92 lower-tribunal/certified-record PDFs / 710 floor pages.
- Ellison + Open Grid + Cinder Lake: 97 lower-tribunal/certified-record PDFs / 705 floor pages.
- Norvale + Blue Ember + Serrano: 94 lower-tribunal/certified-record PDFs / 839 floor pages.

Per-case commit boundaries are source/schema skeleton, record batch 1, record batch 2,
workflow/adverse traces, disposition/argument/bench, and level-2 evidence. Only the integrator
changes shared indexes or build registration.
