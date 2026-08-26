# Asterglen v0.2.0 primary-source ledger

Checked on **2026-08-12**. Official authorities supply only the legal propositions stated below;
they supply no Asterglen, Copper Kestrel, Meridian Silt, BeaconRoute, docket, or record fact. All
case facts come from the reviewed synthetic sources under `documents/v0.2-lower-record/`,
`documents/v0.2-appellate-actual/`, and `documents/v0.2-appellate-branches/`.

## Root-owned official authorities

| Authority ID | Source version | Exact locator and limited proposition | Official URL |
| --- | --- | --- | --- |
| `ca4r54b.authority.usc-1291` | 2026-07-25 preliminary U.S. Code snapshot | 28 U.S.C. § 1291: courts of appeals have jurisdiction over final decisions of district courts, subject to statutory exceptions. | <https://uscode.house.gov/view.xhtml?edition=prelim&req=granuleid%3AUSC-prelim-title28-section1291> |
| `ca4r54b.authority.usc-2107a` | 2026-07-07 preliminary U.S. Code snapshot | 28 U.S.C. § 2107(a): unless another part of § 2107 applies, a civil notice of appeal must be filed within thirty days after entry of the judgment, order, or decree. | <https://uscode.house.gov/view.xhtml?edition=prelim&req=granuleid%3AUSC-prelim-title28-section2107> |
| `ca4r54b.authority.frcp-58-entry` | 2025-12-01 | Fed. R. Civ. P. 58(a), (c): a judgment ordinarily must be set out in a separate document; when one is required, entry occurs upon docket entry and the earlier of a compliant separate document or 150 days after docket entry. | <https://www.uscourts.gov/sites/default/files/document/federal-rules-of-civil-procedure.pdf> |
| `ca4r54b.authority.frap-39-costs` | 2026-03-23 | Fed. R. App. P. 39(a)(1): when an appeal is dismissed, costs ordinarily are taxed against the appellant unless the parties agree otherwise or the court orders otherwise. | <https://www.ca4.uscourts.gov/rules/Rule39.html> |
| `ca4r54b.authority.kinsale` | 2022-04-20 | *Kinsale Insurance Co. v. JDBC Holdings, Inc.*, 31 F.4th 870, 873–76 (4th Cir. 2022), majority Parts I–II, official PDF pages 3–11: Rule 54(b) requires ultimate disposition of an individual claim and a case-specific no-just-reason determination; certification is tilted against fragmentation, and relationship, mootness, repeated-review, setoff, and equitable considerations inform review. Published and precedential. | <https://www.ca4.uscourts.gov/opinions/211754.P.pdf> |
| `ca4r54b.authority.mcpherson` | 2024-10-15 | *McPherson v. Patton*, No. 23-1938, slip op. at 4–7 (4th Cir. Oct. 15, 2024): an unpublished disposition treating a Rule 54(b) grant and partial judgment as nonfinal when the express no-just-reason determination was omitted. Nonprecedential secondary support, not binding authority. | <https://www.ca4.uscourts.gov/opinions/231938.U.pdf> |
| `ca4r54b.authority.bowles` | 2007-06-14 | *Bowles v. Russell*, 551 U.S. 205, 209–14 (2007): the statutory time limit for a civil notice of appeal is jurisdictional and courts may not create equitable exceptions. Precedential. | <https://www.govinfo.gov/content/pkg/USREPORTS-551/pdf/USREPORTS-551-205.pdf> |
| `ca4r54b.authority.primov-condition-precedent` | 2018-08-23 | *Primov v. Serco, Inc.*, 296 Va. 59, official opinion pages 4–7, 10–11 (2018): Virginia enforces clear contractual conditions precedent; interpretation is reviewed de novo, while the consequence of noncompliance depends on the contract and circumstances. Precedential. | <https://www.courts.state.va.us/static/opinions/opnscvwp/1171381.pdf> |

The House pages are dynamic official compilations. Their `source_version` values are the checked
snapshot dates, not claims that Congress amended the statutes on those dates. The civil-rule
version is the rules' December 1, 2025 compilation; the Fourth Circuit rule page uses the shared
March 23, 2026 source baseline.

## Dependency-owned authority

The exact-pinned `foundation.us-federal@2025.12.01` and
`foundation.us-ca4@2026.03.23` packs retain common procedural authorities instead of allowing the
Asterglen root to copy or shadow them. The successor uses, among others:

- Federal Rule of Civil Procedure 54(b) for ultimate claim disposition, the express
  no-just-reason determination, and revision of nonfinal partial decisions;
- Federal Rules of Appellate Procedure 4 and 26 for the civil notice and calendar calculations;
- Federal Rules of Appellate Procedure 28 and 31 for principal-brief content and timing;
- Federal Rules of Appellate Procedure 36, 40, and 41 for judgment, rehearing, release, and
  mandate timing; and
- the shared Fourth Circuit filing, service, brief, rehearing, and mandate authorities selected
  by the final case, workflow, and grounded argument resources.

The final realism evidence lists the exact canonical selection of 31 authorities and closes it,
with the other evidence partitions, to
`445c3f11dcc8046eedfc233407699cbbb3ea4e39425d22c976808959350ca62c`.
The complete non-pack evidence envelope contains 160 unique IDs: 44 resources, 75 blobs, eight
traces, two record checks, and those 31 authorities.

## Synthetic source classes

Every v0.2.0 Markdown source was written specifically for this exercise and begins with an
unambiguous synthetic-record warning. No source was scraped from or adapted from a real litigant,
company, contract, docket, deposition, or court record.

- `documents/v0.2-lower-record/` contains 37 NDWV sources rendered at JA1–JA234.
- `documents/v0.2-appellate-actual/` contains 13 actual-history sources rendered at PA1–PA70.
- `documents/v0.2-appellate-branches/` contains 25 mutually exclusive, never-occurred training
  sources rendered at PA71–PA143.

The actual sources may cite the synthetic district record and the official authorities above.
The branch sources are exact workflow and training inputs only. They cannot establish a fact
about the actual appeal, cure a defect in an actual record, or ground the actual disposition.

The frozen source plans record `source_review_clear` and direct page ranges for all 75 sources.
The accepted lower inventory is
`6a035dc4e431496d3eadd994926d6a383a26606bef67c1d4dd5d5ac89e5afe46`, the accepted appellate
inventory is `debe1315e98116a5b9f5552d5c11eeb98badb2579e28d75663d5b822e5c3059d`, and the
final record resource is
`429603e0b7b49ff25e8a444a411b9c257cfbf0009fbfe1fc103ae8ac80e52f84`. The framed ordered
PDF corpus closes to `7c9c70733dcf87d2bc9d2fca02cbbce28c485d6712516caadbaad71f58c43fbb`.
Those pins establish the authored source-to-render provenance and emitted-byte identity; they do
not establish external authenticity or independent legal review.

## Predecessor separation

The v0.1.0 authoring sources and official-source notes remain historical predecessor material.
The predecessor was built around the EDVA docket `SYN-25-0117` and appellate docket
`SYN-26-1427`; its 18 PDFs/124 pages are frozen at root
`ff7a2e1195f9bd006e7df46c19675a3e07a4bd8975b1643a01adbc9cc4fd3424` and archive SHA-256
`ce0ebffb92942e85e02658d11846af70ebb5fdc287f99a4c683a48f381e39227`.

The v0.2.0 NDWV/`SYN-CA4-26-CV-4102` sources were authored anew. Similar party names, legal
questions, dates, or procedural shapes do not make a predecessor page part of the successor
record. No v0.1.0 PDF is relabeled, copied into the new record, or counted toward the M4 floor.

## Review limit

The authoring review remains `independent_review_pending`, with level 2 recorded in all seven
realism dimensions. Official-source checking and internal source consistency do not establish
qualified independent legal review, and no level-3 claim is made. A future detached review must
pin the exact final root revision
`7e77bc0fbe02dc9e108681df73852859d6d0f577acdcb65fcfb7678eac78b728` rather than rewriting
either the predecessor or the successor root.
