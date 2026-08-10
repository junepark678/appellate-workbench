# CA4 Rule 54(b) synthetic civil appeal authoring set

Status: source-grounded authoring candidate as of **2026-08-11**. This directory is not a
distributable pack. It intentionally contains no root `manifest.json`, no generated PDF, and no
content or blob digest.

Every case name, party, lawyer, district judge, circuit-panel identity, docket number, address,
agreement, fact, and filing in this tree is fictional. The documents repeatedly identify
themselves as synthetic training material so that source files or later renderings cannot be
mistaken for court records. Official sources are used only for legal propositions and procedural
shape; no real case record was adapted.

## Authored result

Asterglen Freight Software, Inc. timely notices an appeal from a separate partial judgment that
disposed of Count I while Count II and a counterclaim remain. The district court's Rule 54(b)
order grants the motion and directs entry of judgment but never expressly determines that there
is "no just reason for delay." The synthetic Fourth Circuit panel therefore dismisses for lack of
appellate jurisdiction and remands without reaching the contract merits. No rehearing petition or
stay motion is filed, and mandate issues under FRAP 40 and 41.

This is intentionally an adverse finality path. `traces/expected-traces.json` also states the
expected consequences of a supported Rule 54(b) certification, a late notice, a missing separate
judgment, a rehearing petition, and a stay motion. Those alternatives are expectations, not facts
in the authored docket.

## Tree

- `documents/`: deterministic UTF-8 Markdown source for the district and appellate record.
- `metadata/intended-pdfs.json`: intended PDF titles, paths, page counts, and render constraints;
  it does not assert that any PDF exists.
- `metadata/dockets.json`: normalized district and appellate docket metadata.
- `metadata/joint-appendix.json`: deterministic JA assembly and continuous page ranges.
- `resources/`: version-1 resource candidates authored against the current schemas where the
  present data model permits. Eighteen digest-independent candidates pass local v1 JSON Schema
  validation; `record.candidate.json` deliberately omits required asset digests. Pack-level
  cross-reference, semantic, and runtime validation have not been claimed.
- `sources/`: official-source ledger and research limits.
- `traces/`: authored and adverse branch expectations with authority IDs on every consequence.

## Important limits

This work is an author self-review only. It does not claim independent legal review or realism
level 3. The realism candidate remains `independent_review_pending`, with every score at 2 or
below. The exact schema and implementation blockers are listed in `VALIDATION.md`.
