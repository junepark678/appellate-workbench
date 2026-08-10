# Fourth Circuit parity inventory

This inventory freezes the useful breadth of the prior prototype without treating its code or
placeholder documents as a migration source. The new application has one launch jurisdiction,
`us-ca4`, and four procedure profiles—not four jurisdictions.

## Case and lower-tribunal record floor

| Case family | Procedure profile | Prior docket/document pairs | MVP treatment |
| --- | --- | ---: | --- |
| Employment retaliation summary judgment | Civil appeal | 37 | Rewrite substantive record |
| Rule 54(b) finality | Civil appeal | 37 | Gold civil candidate; rewrite |
| Preliminary injunction and stay | Civil appeal | 24 | Rewrite substantive record |
| Section 1983 qualified immunity | Civil appeal | 37 | Rewrite substantive record |
| Post-trial judgment as a matter of law | Civil appeal | 42 | Rewrite substantive record |
| Sealed FOIA record | Civil appeal | 37 | Rewrite substantive record |
| Criminal sentencing and appeal waiver | Criminal appeal | 28 | Gold criminal; rewrite |
| Immigration agency review | Agency review | 18 | Gold agency; source review required |
| Privileged-discovery mandamus | Original writ | 23 | Gold writ; rewrite |
| **Total** | **Four profiles** | **283** | **Substantive assets required** |

Every prior docket row had a corresponding one-page synthetic PDF asset record. Those files
were boilerplate metadata artifacts and are not accepted as MVP content. The count is a breadth
and relationship floor; new assets must contain the facts, orders, exhibits, and page anchors
actually used by the engine.

## Appellate behavior floor

The prior source catalog contained 53 distinct appellate filing-event definitions; 26 were
exercised by authored expected paths. The
[complete event catalog and MVP treatment](APPELLATE_EVENT_CATALOG.md) accounts for every
definition as a required port, adverse/optional port, deliberate deferral, or superseded
state transition. The new procedure packs must exercise every event required by the nine
shipped cases. Raw definition count alone is not an acceptance criterion.

Prior profile inventories were:

| Profile | Cases | Cataloged filing events |
| --- | ---: | ---: |
| Civil appeal | 6 | 40 |
| Criminal appeal | 1 | 36 |
| Agency review | 1 | 34 |
| Original writ | 1 | 18 |

## Port, rewrite, discard

| Prior element | Decision |
| --- | --- |
| Deterministic transition and replay expectations | Port as tests and typed invariants |
| Synthetic case concepts and docket relationships | Rewrite into versioned data packs |
| Source/provenance ledger | Rewrite and require per rule-driven consequence |
| Placeholder one-page PDFs | Discard |
| Static compile-time pack registry and ID-substring inference | Discard |
| Convex schema, cloud operations, migrations, and materializations | Discard |
| Clerk/instructor/firm tenancy and hosted identity | Discard |
| Unbounded dual school/firm product surface | Discard |
