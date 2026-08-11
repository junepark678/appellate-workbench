# A.R.M. agency-review authoring pack

This tree authors the incomplete, pre-release schema-v2 root
`us.ca4.m4.arm-agency@1.1.0` for the fictional exercise *A.R.M. v. Attorney General
of the United States* (`SYN-BIA-25-0113`; `SYN-CA4-25-AG-4301`). It is an authoring
checkpoint, not a releasable pack and not legal advice.

The current boundary contains the complete certified-record inventory: eighteen AR-labeled
agency PDFs totaling 238 substantive searchable pages (AR1–AR238), plus one separately
classified generated appellate proffer totaling eight substantive searchable pages
(PA1–PA8). The proffer is not part of the administrative record and is not counted toward the
18-PDF/238-page certified-record contract. The record inventory is complete, but the case remains
incomplete because disposition, executable traces, and realism review are still absent.

All people, addresses, facts, events, documents, dockets, and the Republic of Kalyra are
fictional. A.R.M. remains anonymized by initials; the exercise does not invent a full name,
street address, birth date, registration number, or other unnecessary personal identifier.
Real United States courts and agencies appear only as institutional procedural actors. The
three shared bench profiles are fictional/composite profiles.

## Exact dependencies

- `foundation.us-federal@2025.12.01`
  (`866c90996c15e2076b9508a297ffce1a4e766b1432a9e11d08e8138c57e363c9`)
- `foundation.us-ca4@2026.03.23`
  (`449d75c77e5c47883f750377450f2d1ec1fc0e42e20b1f247446b208661d3262`)
- `foundation.us-ca4-fictional-bench@1.0.0`
  (`cee0bf93309cc9ad800f215a47d734b20a9fdf5dc889f2f440e4382b942d332d`)

The root owns a complete agency-review procedure and workflow because schema v2 has no
workflow inheritance. It reuses the shared court, actor roles, filing catalog, forms, and
shared rule authorities.

The shared motion type is polymorphic. Filing presence can establish only that a motion exists;
it cannot prove whether that motion seeks correction or supplementation. The two court-only
record orders therefore remain separately authored operations, and a later trusted trace must
bind the proper motion to each operation before claiming the eventual result.

## Record-composition boundary

Agency Exhibit P-7 is A.R.M.'s pre-hearing sworn declaration at AR33–AR50. AR117, AR138, AR139,
AR161, and AR164 record and confirm its admission without an authenticity or timeliness objection.
AR219–AR226 preserve the contemporaneous initial transmission, including an unexplained
eighteen-label discontinuity but no retrospective diagnosis. The exact Rule 16(b) stipulation,
April and September receipt tables, file audit, P-7 digest, itemized eighteen-document index, and
corrected certification at AR227–AR238 establish that the same admitted file was omitted by a stale
status field and restored without adding evidence.

P-4, P-5, and P-6 remain separate admitted logical exhibits within the one physical ten-page file
at AR105–AR114. That file contains complete controlled searchable source and translation bytes,
their exact hashes, and an exhaustive page/object map. It contains no raster image object and makes
no contrary representation.

The PA1–PA8 cousin declaration instead describes events after the BIA order. It was never
presented to the immigration judge or Board, never admitted, and never receives an AR label.
It is retained only as an `extra_record_proffer` in the appellate docket. Its existence does
not establish its truth and does not make it reviewable record evidence.

## Runtime and deferred resources

Two minimal argument configurations make this incomplete case root runtime-loadable. The
actual-record bank and counterfactual-training bank each cover all five current issue topics and
ground every question only in canonical authorities and existing AR1–AR238 or PA1–PA8 anchors.
The day-31 petition prompt is a counterfactual oral-argument question only; it is not an
executable workflow trace because the current engine cannot faithfully represent government
invocation of the nonjurisdictional filing rule.

Structured disposition plans, executable counterfactual traces, and realism review/evidence
remain deliberately absent. No appellate result or realism level, especially level 3, is claimed.

The workflow also records separate court-entered briefing-complete and argument-held orders;
neither one principal-brief filing nor a scheduled argument alone advances the case, and the
argument-held order cannot precede the scheduled date. Its mandate path is only the default
no-rehearing-petition/no-stay branch: the named 45-day Rule 40 clock is based on the recorded
judgment date, and the separately named seven-day Rule 41 clock is based on that first clock's
exact due date. A later boundary must add any rehearing-petition or stay routes before claiming
those branches.

## Render boundary

The accepted batch-1 invocation remains provenance for unchanged documents 01, 02, 04, 06, and the
separate appellate proffer. Corrected batch-1 documents 03 and 05 and all twelve batch-2 documents
were emitted by exactly one clean invocation of `render-plan-canonical-repair.json` into a fresh
directory. The resulting fourteen PDFs and
`metadata/render-inventory-canonical-repair.json` (SHA-256
`aa6d2cd94ad41e0f3ad6f4a220083792036d1c5f972ba86eeef902267d0236cf`) are pinned as the canonical
bytes. Earlier batch-2 and affected batch-1 candidates were rejected and are not provenance for the
current sources; a later render is not claimed to reproduce these exact PDF bytes.
