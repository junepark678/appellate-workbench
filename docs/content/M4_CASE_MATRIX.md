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
| Norvale injunction | Civil appeal | 24 | 140 | No |
| Ellison immunity | Civil appeal | 37 | 275 | No |
| Blue Ember JMOL | Civil appeal | 42 | 430 | No |
| Open Grid FOIA | Civil appeal | 37 | 250 | No |
| Serrano waiver | Criminal appeal | 28 | 260 | Criminal |
| A.R.M. CAT | Agency review | 18 | 220 | Agency |
| Cinder Lake privilege | Original writ | 23 | 180 | Writ |
| **Total** | **Four profiles** | **283** | **2,245** | **Four** |

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
- **Limits:** a serialized stay-before-merits trace fits. Concurrent clocks and inferred
  mootness require a later trusted capability.

### 4. Ellison immunity — `ca4m4.case.ellison-immunity`

*Mara Ellison v. Officer Nolan Rusk* — `SYN-CA4-26-CV-4104`,
`SYN-WDVA-25-CV-0733`. Alder presides with Reed and March.

- **Wedge:** collateral-order review of qualified-immunity denial after a welfare-check seizure;
  obstructed body-camera footage leaves resistance and threat facts disputed.
- **Record:** 4 pleadings, 6 report/dispatch/medical, 6 bodycam transcript/frame/authentication,
  8 deposition/expert, 7 immunity/SUMF, 1 order, and 5 docket/initiation PDFs. A root-owned MP4
  is optional and does not count toward 37.
- **Argument:** assumed facts, why video is not dispositive, clearly established law, and relief.
- **Disposition:** dismiss factual-sufficiency targets; affirm legal denial on assumed facts.
- **Limits:** automated classification of argument type or video facts is out of scope.

### 5. Blue Ember post-trial — `ca4m4.case.blueember-jmol`

*Blue Ember Biologics, LLC v. Granite Heron Logistics, Inc.* — `SYN-CA4-26-CV-4105`,
`SYN-WDNC-24-CV-0520`. March presides with Rowan and Slate.

- **Wedge:** Rule 50(a) challenges causation only; Rule 50(b) first raises mitigation. The court
  grants JMOL on mitigation and conditionally grants a Rule 59 damages retrial.
- **Record:** 7 pleading/contract, 7 discovery/expert, 4 pretrial, 6 trial transcript, 7 trial
  exhibit, 4 Rule 50(a)/verdict/judgment, 5 Rule 50(b)/59, and 2 docket/notice PDFs.
- **Argument:** exact preservation language, renewed-motion boundaries, Rule 50 versus Rule 59
  review, and permitted relief. Counterfactual: mitigation expressly preserved.
- **Disposition:** reverse mitigation JMOL; affirm the conditional new-trial order; vacate amended
  judgment; remand for a damages retrial.
- **Limits:** generic Rule 4(a)(4) tolling and inferred Rule 50 preservation require later engine
  work; the MVP uses a case-specific authored trace.

### 6. Open Grid FOIA — `ca4m4.case.opengrid-foia`

*Open Grid Archive v. United States Department of Energy* — `SYN-CA4-26-CV-4106`,
`SYN-DMD-25-CV-0914`. Vale presides with Quill and Reed.

- **Wedge:** FOIA Exemptions 7(E) and 7(F), Vaughn specificity, segregability, and paired
  public/sealed filings concerning cyber-response playbooks.
- **Record:** 5 request-chain, 2 pleading, 4 search-declaration, 4 public/sealed Vaughn,
  6 public/sealed sample-record, 6 cross-summary-judgment, 5 seal/in-camera, and 5
  judgment/docket/initiation PDFs.
- **Argument:** law-enforcement purpose, technique versus guideline/risk, Vaughn detail,
  segregability, 7(F), and least-restrictive sealing.
- **Disposition:** affirm 7(E) for operational decision trees; vacate categorical 7(F),
  segregability, and overbroad-sealing rulings; remand.
- **Confidentiality boundary:** public/sealed twins, per-document session grants, revocation,
  stable-anchor projection, deferred CAS verification, and seal-motion deficiencies are now
  implemented by `workbench.pack.sealed-record-twins@1`. Open Grid still must author and replay
  those exact policies before it can claim level-2 confidentiality evidence.

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
| Ellison | [42 U.S.C. § 1983](https://uscode.house.gov/view.xhtml?edition=prelim&num=0&req=granuleid%3AUSC-prelim-title42-section1983); [Barricks](https://www.ca4.uscourts.gov/opinions/251250.P.pdf); [Armstrong](https://www.ca4.uscourts.gov/opinions/published/151191.p.pdf) |
| Blue Ember | FRCP 50 and 59; [Unitherm](https://www.govinfo.gov/app/details/USREPORTS-546/USREPORTS-546-394); [Wiener](https://www.ca4.uscourts.gov/opinions/241316.P.pdf); [Boley](https://www.ca4.uscourts.gov/opinions/231493.u.pdf) (nonprecedential) |
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

The four gold candidates require, respectively, an appellate-finality reviewer, criminal
sentencing/waiver reviewer, immigration reviewer with an operative-CFR refresh, and
appellate-privilege/writ reviewer. Open Grid additionally blocks on sealed/public document access
behavior.

After #23–#28 and the three shared foundations freeze, isolated root authoring may proceed in
parallel without shared-file overlap:

- Benton + Asterglen + A.R.M.: 92 lower-tribunal/certified-record PDFs / 710 floor pages.
- Ellison + Open Grid + Cinder Lake: 97 lower-tribunal/certified-record PDFs / 705 floor pages.
- Norvale + Blue Ember + Serrano: 94 lower-tribunal/certified-record PDFs / 830 floor pages.

Per-case commit boundaries are source/schema skeleton, record batch 1, record batch 2,
workflow/adverse traces, disposition/argument/bench, and level-2 evidence. Only the integrator
changes shared indexes or build registration.
