# Fourth Circuit source ledger

This ledger fixes the source baseline used while authoring the launch content. It is not a
substitute for rule text or independent legal review. Every rule-driven pack transition must
identify a proposition, source version, and stable public URL; a general link to this page is
not enough.

## Current baseline

Baseline checked on **2026-08-11**:

| Priority | Source | Version represented | Use |
| --- | --- | --- | --- |
| 1 | [Federal and Fourth Circuit rules with IOPs](https://www.ca4.uscourts.gov/docs/pdfs/rules.pdf?sfvrsn=25ab209_1) | March 23, 2026 | Controlling FRAP/local-rule and IOP text for all four launch profiles |
| 1 | [Current Federal Rules of Appellate Procedure](https://www.uscourts.gov/sites/default/files/document/federal-rules-of-appellate-procedure.pdf) | Rules amended through December 1, 2025 | National rule cross-check independent of the combined circuit publication |
| 1 | [Fourth Circuit local rules and IOPs](https://www.ca4.uscourts.gov/docs/rules/localrules.pdf?sfvrsn=6e5ab209_1) | March 23, 2026 | Local-rule cross-check |
| 2 | [Fourth Circuit Appellate Procedure Guide](https://www.ca4.uscourts.gov/rules-and-procedures/resources) | March 2026 pages, with revision dates shown per page | Clerk workflow, forms, and practical sequence; rules control if the guide differs |
| 2 | [Appellate deadlines guide](https://www.ca4.uscourts.gov/AppellateProcedureGuide/General_Provisions/APG-appellatedeadlines.html) | Page marked revised December 1, 2024 | Deadline inventory and cross-check; each deadline still cites its controlling authority |
| 2 | [New appeals and petitions guide](https://www.ca4.uscourts.gov/AppellateProcedureGuide/Initial_Requirements/APG-newappealsandpetitions.html) | March 2026 | Finality, civil/criminal initiation, agency review, and writ workflow orientation |
| 2 | [Forms by category](https://www.ca4.uscourts.gov/court-forms-fees/forms-by-category) | Live court catalog checked 2026-08-11 | Current form identity and presentation reference; court forms are not redistributed blindly |

The combined March 23 rulebook is the primary authored baseline. The procedure guide is used
to model realistic clerk-facing sequence, but never to override the rulebook, statute, or a
case-specific court order.

## Baseline consequences already selected

These are inventory anchors, not a complete legal model:

- a civil notice of appeal ordinarily uses the 30-day period in FRAP 4(a)(1), with the
  60-day federal-party rule and separate-document/tolling behavior represented explicitly;
- a criminal defendant's notice ordinarily uses the 14-day FRAP 4(b) period and remains
  distinguished from the jurisdictional civil deadline;
- Fourth Circuit counsel appearance, applicable disclosure, docketing statement, and transcript
  ordering are separate initial requirements with their own 14-day triggers;
- ordinary deadline computation counts calendar days and rolls a weekend or legal-holiday last
  day forward under FRAP 26(a); service-based extra time is not added to electronic delivery;
- agency-record timing, mandamus answer timing, briefing, rehearing, and mandate behavior remain
  distinct typed routes rather than being copied from the civil-appeal defaults;
- mandate issuance is modeled from FRAP 41's trigger, including a timely rehearing petition or
  stay motion, rather than as a fixed number of days after judgment in every branch.

Every one of these consequences needs a case trace and an exact authority object before its
realism score can exceed zero.

## Change and review gate

The [April 2026 federal rules package](https://www.uscourts.gov/sites/default/files/document/2026_congressional_package_final.pdf)
includes an amendment scheduled for **December 1, 2026**. The published appellate change
concerns Form 4, but the source baseline must still be refreshed before any release on or after
that date. A refresh compares the federal rules, Fourth Circuit local rules/IOPs, standing
orders, procedure guide, and relevant forms, then creates a new pack version when an operative
proposition changes.

Automated tests and author self-review can establish at most realism level 2. Level 3 requires
an attributable, independent qualified reviewer to sign the exact pack revision digest and
source ledger. Until then, review state remains `pending` even when all executable traces pass.
