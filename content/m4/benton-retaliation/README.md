# Benton retaliation authoring pack

This new tree authors the incomplete, pre-release schema-v2 root
`us.ca4.m4.benton-retaliation@1.0.0` for the fictional exercise *Leora Benton v. Blue
Cedar Compliance, Inc.* (`SYN-EDVA-25-CV-0412`; `SYN-CA4-26-CV-4101`). It is an
authoring checkpoint, not a releasable pack and not legal advice.

The current boundary contains source/schema infrastructure, record batch 1, and two grounded
argument configurations: 19 unique substantive searchable PDFs totaling 125 pages, continuously
labeled JA1–JA125. Batch 2 will add the remaining 18 PDFs and JA126–JA262. Until that boundary
lands, the pack must not be published, reviewed for realism, or represented as satisfying the M4
record floor.

All people, companies, addresses, communications, dates, dockets, testimony, and record facts
are fictional. The real courts and EEOC appear only as institutional procedural actors. The
three shared bench profiles are fictional/composite profiles.

## Exact dependencies

- `foundation.us-federal@2025.12.01`
  (`866c90996c15e2076b9508a297ffce1a4e766b1432a9e11d08e8138c57e363c9`)
- `foundation.us-ca4@2026.03.23`
  (`449d75c77e5c47883f750377450f2d1ec1fc0e42e20b1f247446b208661d3262`)
- `foundation.us-ca4-fictional-bench@1.0.0`
  (`cee0bf93309cc9ad800f215a47d734b20a9fdf5dc889f2f440e4382b942d332d`)

The root owns a complete civil-appeal procedure and workflow because schema v2 has no workflow
inheritance. It reuses the shared court, actor roles, filing catalog, forms, and applicable
authority sets. The generic principal-brief route records a principal brief by either party and
requires service on every other modeled party, but filing presence does not prove that both an
opening and response brief exist. The court-only `briefing-complete` marker is an explicit human
confirmation that the required brief inventory is complete; a trace must record both party filings
before entering it.

The executable root workflow uses stable court-event IDs:

- orders: `ca4m4.benton.order.record-complete`, `ca4m4.benton.order.briefing-complete`,
  `ca4m4.benton.order.argument-held`, `ca4m4.benton.order.rehearing-disposition`,
  `ca4m4.benton.order.mandate-stay`,
  `ca4m4.benton.order.mandate-shortening`, and `ca4m4.benton.order.mandate-release`;
- deadlines: `ca4m4.benton.deadline.docketing-statement`,
  `ca4m4.benton.deadline.rehearing`,
  `ca4m4.benton.deadline.mandate-no-petition`, and
  `ca4m4.benton.deadline.mandate-after-rehearing-denial`.

Record completeness is required before briefing. Briefing completeness is required before either
submission branch. The submitted-on-briefs advance atomically commits the unscheduled branch; after
that stage advance, argument scheduling is no longer eligible. The argued branch requires a
scheduled argument and an argument-held marker after the scheduled date has been reached. The two
judgment operations are mutually exclusive through the persisted argument-scheduled state and the
argument-held marker. The ordinary post-judgment trace calculates a named 14-day rehearing deadline
from the recorded judgment occurrence under FRAP 40. A timely petition satisfies that exact
deadline, while a petition after its due date is rejected. If no petition is present, the workflow
calculates `mandate-no-petition` seven days from the rehearing deadline's due date. If a petition is
present and the exact rehearing-disposition operation enters a denial order, the workflow instead
calculates `mandate-after-rehearing-denial` seven days from that order's recorded occurrence. The
two branch deadlines and issue operations are distinct; their filing-presence and order guards
prevent cross-branch issuance. The `reached` boundary permits issuance on—not one day after—the
applicable mandate due date. Mandate issuance also requires a release order.
Schema v2 has no first-class mandate-stay or variable-shortening opcode, so stay, shortening, and
final release are explicit court orders: an ordinary trace must withhold release while a stay
operates, while the separate shortened-mandate operation requires both a granted shortening order
and a granted release order.

The court records `ca4m4.benton.deadline.rehearing` as the named output of the judgment-anchored
calculation. The rehearing route satisfies that exact deadline and rejects a late petition. A
complete no-petition trace must reach the still-open deadline with no rehearing filing before its
dependent mandate calculation; a denial trace must instead preserve the timely petition and exact
denial-order provenance.

Both grounded argument configurations cover both current issues. The second issue is only a
batch-1 disclosure-boundary hypothetical: it asks what Rules 26(e)(1) and 37(c)(1) would require if
a party later learned a materially different witness subject. This boundary authors no such later
event, witness statement, use ruling, or outcome. Every declared issue/topic pair has an authored
question grounded only in that issue's exact current JA anchors and authorities; the topic union
intersects the positive focus vocabulary of Rowan, Alder, and Fen. The counterfactual changes only
Pike's pretermination knowledge and does not add a record fact or predict an engine answer.

## Deferred resources

The structured disposition and realism review remain deliberately deferred until every cited
batch-2 page exists. No planned witness-use fact or ruling is a current pack resource. The reserved
disposition and review IDs are:

- `ca4m4.benton.disposition.authored`
- `ca4m4.benton.realism.review`

No `realism_review` resource or realism-evidence capability is present. The final review slot
must be populated only by the exact-evidence utility after the complete record, workflow traces,
questions, and disposition have frozen.

## Render boundary

The Markdown sources were rendered once with the checked-in `appellate-render` utility into a
fresh directory. The emitted PDFs are pinned by exact bytes in the manifest and are not expected
to reproduce byte-for-byte on a later render. `metadata/render-inventory-batch-1.json` records
the one render invocation's semantic inventory.
