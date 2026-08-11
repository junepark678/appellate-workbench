# A.R.M. agency-review authoring pack

This tree authors the incomplete, pre-release schema-v2 root
`us.ca4.m4.arm-agency@1.0.0` for the fictional exercise *A.R.M. v. Attorney General
of the United States* (`SYN-BIA-25-0113`; `SYN-CA4-25-AG-4301`). It is an authoring
checkpoint, not a releasable pack and not legal advice.

The current boundary contains source/schema infrastructure and record batch 1 only:
six AR-labeled agency PDFs totaling 72 substantive searchable pages (AR1–AR72), plus one
separately classified generated appellate proffer totaling eight
substantive searchable pages (PA1–PA8). The proffer is not part of the administrative
record and is not counted toward the 18-PDF/238-page certified-record contract.

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

Agency Exhibit P-7 is A.R.M.'s pre-hearing sworn declaration, and the present pages show that it
was lodged before the merits hearing. Whether it was later admitted, omitted from an initial
certification, and restored by correction remains the pack's disputed full-record theory rather
than a fact proved by batch 1. The deferred hearing receipt, transcript, initial index, file audit,
and corrected certification must establish or defeat that theory. AR33–AR50 are the planned
corrected labels if those later materials support correction.

The PA1–PA8 cousin declaration instead describes events after the BIA order. It was never
presented to the immigration judge or Board, never admitted, and never receives an AR label.
It is retained only as an `extra_record_proffer` in the appellate docket. Its existence does
not establish its truth and does not make it reviewable record evidence.

## Runtime and deferred resources

Two minimal argument configurations make this incomplete case root runtime-loadable. The
actual-record bank and counterfactual-training bank each cover all five current issue topics and
ground every question only in canonical authorities and existing AR1–AR72 or PA1–PA8 anchors.
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

The seven Markdown sources are rendered in one clean invocation with the freshly
built `appellate-render` utility into a fresh directory. The exact emitted PDF bytes are pinned
in the manifest. A later render is not claimed to reproduce those bytes. The render inventory
records that canonical invocation's semantic page inventory.
