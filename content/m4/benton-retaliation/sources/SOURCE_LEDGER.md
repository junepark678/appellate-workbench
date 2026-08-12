# Benton source ledger

Checked on 2026-08-11 and updated on 2026-08-12. This ledger separates official legal authority
from the synthetic fact record. Official sources supply only the stated legal propositions; they
supply no Benton fact.

## Official authorities

| Authority ID | Source version | Exact locator and proposition | Official URL |
| --- | --- | --- | --- |
| `ca4m4.benton.authority.title-vii-retaliation` | 2026-08-11 preliminary U.S. Code snapshot | 42 U.S.C. § 2000e-3(a): an employer may not discriminate because an individual opposed an unlawful Title VII practice or participated in a Title VII investigation, proceeding, or hearing. | <https://uscode.house.gov/view.xhtml?req=%28title%3A42+section%3A2000e-3+edition%3Aprelim%29> |
| `ca4m4.benton.authority.foster-framework` | 2015-05-21 | *Foster v. University of Maryland-Eastern Shore*, No. 14-1073, Part III.B, PDF pages 17–20: *Nassar* does not alter the prima-facie causation prong or displace McDonnell Douglas in a pretext retaliation case. | <https://www.ca4.uscourts.gov/opinions/published/141073.p.pdf> |
| `ca4m4.benton.authority.foster-pretext` | 2015-05-21 | *Foster*, Parts III.B–C, PDF pages 18–23: at pretext the plaintiff must show retaliation was the real reason and thus a but-for cause; timing and evidence making the stated reasons questionable can create a jury issue. | <https://www.ca4.uscourts.gov/opinions/published/141073.p.pdf> |
| `ca4m4.benton.authority.frcp-56-summary-judgment` | 2025-12-01 | Fed. R. Civ. P. 56(a), (c): judgment requires no genuine dispute of material fact, on materials the rule permits the parties to cite. | <https://www.uscourts.gov/sites/default/files/document/federal-rules-of-civil-procedure.pdf> |
| `ca4m4.benton.authority.frcp-26e-supplementation` | 2025-12-01 | Fed. R. Civ. P. 26(a)(1)(A)(i), (e)(1): initial disclosures identify persons likely to have discoverable information and their subjects; a party must timely supplement or correct a materially incomplete or incorrect disclosure or response when the additional information has not otherwise been made known. | <https://www.uscourts.gov/sites/default/files/document/federal-rules-of-civil-procedure.pdf> |
| `ca4m4.benton.authority.frcp-37c1-nondisclosure` | 2025-12-01 | Fed. R. Civ. P. 37(c)(1): absent substantial justification or harmlessness, undisclosed information or witnesses may not be used on a motion, at a hearing, or at trial, with listed additional sanctions available. | <https://www.uscourts.gov/sites/default/files/document/federal-rules-of-civil-procedure.pdf> |
| `ca4m4.benton.authority.benjamin-disclosure-sanction` | 2021-01-19 | *Benjamin v. Sparks*, 986 F.3d 332 (4th Cir. 2021), No. 19-2041, official opinion lines 197–200 and 259–300: disclosure violations and Rule 37(c)(1) exclusions are reviewed for abuse of discretion; surprise, ability to cure, disruption, importance, and explanation guide justification and harmlessness; the nondisclosing party bears the burden. | <https://www.ca4.uscourts.gov/opinions/192041.P.pdf> |

Shared FRAP, Fourth Circuit local rules, calendar rules, and common civil authorities remain
dependency-owned. The root does not copy or shadow them. The exact 1.2.0 evidence closure selects
28 canonical authorities: the seven root-owned entries above and 21 dependency-owned entries
actually referenced by the case, workflow, routed filing types, or grounded argument resources.
That exact selection is evidence scope, not an assertion that automated checks establish legal
accuracy or completeness.

The House preliminary-code page is a dynamic official compilation. Its `source_version` is the
checked snapshot date, 2026-08-11, not a claim that Congress enacted or amended § 2000e-3(a) on
that date. The Foster `source_version` is the official opinion date, and the civil-rule version is
the rules' 2025-12-01 effective compilation; every source was checked on 2026-08-11.

## Fictional-name collision review

On 2026-08-11 an exact-string public-web review covered the case caption, employer name, and the
named employee witnesses. It found unrelated real-world uses of several ordinary names and of
“Blue Cedar,” including a mobile-security business; it found no match for the full fictional
caption, synthetic docket numbers, employer legal name `Blue Cedar Compliance, Inc.`, or the
combined case facts. The names are therefore not represented as globally unique. The blockquoted
artifact warning on every source page, `SYN-` docket and agency references, fictional actor flags,
and this canon define every person and entity in this pack as fictional/composite and unrelated to
any real person or organization.

## Synthetic source classes

Every source under `documents/batch-1/`, `documents/batch-2/`,
`documents/appellate-actual/`, and `documents/appellate-branches/` was written for this exercise.
Each page advances fixed allegations, admissions, communications, data, testimony, docket events,
or authored outcomes. No source was scraped from a real person, employer, charge, case, or docket.

The lower-court sources render to JA1–JA262. The actual appellate sources render to PA1–PA70.
The branch sources render to PA71–PA127 on the separate counterfactual docket and are marked as
never having occurred on the actual appeal. The documents' references to FRAP, Fourth Circuit
rules, and common civil procedure use the exact dependency-owned authority entries; the synthetic
PDFs are not themselves official legal sources.

The source chronology and identity vocabulary are fixed in `FACT_CANON.md`. The record controls
when witnesses disagree: a pleading allegation is not converted into an admitted fact, and a
deposition answer is not silently reconciled with a scorecard. The late Wynn declaration remains
in the certified record for review but is excluded from the summary-judgment merits evidence; the
joint chart and opinion preserve that boundary rather than deleting or silently accepting it.

The accepted successor render inventory has SHA-256
`c9887d3c15b51cf278d18e1c1c160f48c03b1153181465f493a9fbaf4ebaa972` and binds all 30 new
sources to their PDF hashes, sizes, page ranges, semantic plans, and assembly provenance. Those
generated-artifact hashes document repository provenance only; they do not establish external
authenticity, byte reproducibility across future renderer environments, or independent legal
review.
