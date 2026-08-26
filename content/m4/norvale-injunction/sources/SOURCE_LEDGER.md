# Norvale injunction source ledger

Checked through 2026-08-12. Legal sources below are official primary sources. Synthetic record and appellate documents are governed by the local planning ledger and must not be mistaken for external evidence.

## Dependency pins

| Pack | Version | Required revision |
|---|---:|---|
| `foundation.us-federal` | `2025.12.01` | `866c90996c15e2076b9508a297ffce1a4e766b1432a9e11d08e8138c57e363c9` |
| `foundation.us-ca4` | `2026.03.23` | `449d75c77e5c47883f750377450f2d1ec1fc0e42e20b1f247446b208661d3262` |
| `foundation.us-ca4-fictional-bench` | `1.0.0` | `cee0bf93309cc9ad800f215a47d734b20a9fdf5dc889f2f440e4382b942d332d` |

## Root authority inventory

| Authority ID | Citation and official source | Locator | Frozen proposition |
|---|---|---|---|
| `ca4m4.norvale.authority.usc-1292a1` | [28 U.S.C. § 1292(a)(1)](https://uscode.house.gov/view.xhtml?edition=prelim&num=0&req=granuleid%3AUSC-prelim-title28-section1292%28a%29%281%29) | § 1292(a)(1) | Interlocutory appellate jurisdiction extends to orders granting, refusing, continuing, modifying, or dissolving injunctions and refusals to dissolve or modify them. |
| `ca4m4.norvale.authority.local-rule-8-district-first` | [Fed. R. App. P. 8(a); 4th Cir. R. 8](https://www.ca4.uscourts.gov/rules/Rule08.html) | Rule 8(a)(1)–(2), Local Rule 8 | District-first practice; specific impracticability or prior-denial showing; relevant materials and sworn support. |
| `ca4m4.norvale.authority.billups-protected-paid-speech` | [*Billups v. City of Charleston*, 961 F.3d 673 (4th Cir. 2020)](https://www.ca4.uscourts.gov/opinions/191044.P.pdf) | Official slip pp. 14–19 | Paid expressive activity remains protected speech, including in public streets and sidewalks. |
| `ca4m4.norvale.authority.billups-narrow-tailoring` | [*Billups*](https://www.ca4.uscourts.gov/opinions/191044.P.pdf) | Official slip pp. 19–30 | The government bears the evidentiary burden on narrow tailoring and substantially less restrictive alternatives. |
| `ca4m4.norvale.authority.pashby-winter-review` | [*Pashby v. Delia*, 709 F.3d 307 (4th Cir. 2013)](https://www.ca4.uscourts.gov/opinions/Published/112363.p.pdf) | Official slip pp. 11–15, 32–33 | Each Winter factor must be clearly shown; mixed standards govern legal, factual, and ultimate preliminary-relief rulings. |
| `ca4m4.norvale.authority.pashby-rule65-cd-remand` | [*Pashby*](https://www.ca4.uscourts.gov/opinions/Published/112363.p.pdf) | Official slip pp. 33–35 | Rules 65(c)/(d) require an express security determination and specific injunction terms; a limited remand can preserve warranted interim protection. |
| `ca4m4.norvale.authority.nken-stay-factors` | [*Nken v. Holder*, 556 U.S. 418 (2009)](https://www.govinfo.gov/content/pkg/USREPORTS-556/pdf/USREPORTS-556-418.pdf) | U.S. Reports pp. 425–436 | Four discretionary stay factors apply, with likelihood of success and irreparable injury most critical. |
| `ca4m4.norvale.authority.robinson-recurring-mootness` | [*Robinson v. National Collegiate Athletic Ass'n*, No. 25-2003 (4th Cir. Apr. 3, 2026)](https://www.ca4.uscourts.gov/opinions/252003.P.pdf) | Official slip pp. 10–15 | A short recurring event does not necessarily moot an operative-rule dispute likely to recur for the same party. |

The machine-readable authority wording, dates, statuses, and URLs are canonical in `resources/authority-set.candidate.json`. This ledger is explanatory and must be changed with that resource if a locator or proposition changes.

## Synthetic-source inventory

- `metadata/successor-document-plan.json` is the sole page-allocation and filename contract: 24 lower PDFs/149 JA pages, 23 actual appellate PDFs/135 PA pages, and 26 never-filed branch PDFs/99 PA pages.
- Source directories are `documents/lower-record`, `documents/appellate-actual`, and `documents/appellate-branches`. Planned output paths are under `objects/`.
- Every synthetic row begins as `pending_source`. A Markdown file's presence does not change that status; source review, pagination, rendering, hash capture, and independent render review must close it.
- No PDF SHA-256 is asserted in the planning resource. A later render inventory and record resource must use hashes computed from actual accepted bytes.
- The counterfactual docket and PA136–PA234 are training branches that never occurred on the actual docket.

## Current-law and authorship risks

- The 2026 *Robinson* mootness proposition is recent and must receive practitioner review before any realism level above 2 or any legal-reliance claim.
- `Pashby` supports the intended corrective-remand narrative, but the current structured-disposition schema cannot separately encode non-vacatur or continued protection; the opinion, judgment, fact canon, and review uncertainty must remain aligned.
- Rule 8 mechanics are represented by exact authored routes and dates. The workflow does not infer impracticability, district-first compliance, or sworn-record sufficiency from arbitrary filing text.
- No automated check establishes external authenticity, legal completeness, or accessibility of the generated PDFs. Exact hashes establish only repository provenance for accepted bytes.
