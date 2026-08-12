# Norvale injunction fact canon

This file freezes the authored facts and chronology for the synthetic level-2 successor `us.ca4.m4.norvale-injunction@1.2.0`. It is an authoring contract, not evidence that any synthetic event occurred in the real world.

## Identity and docket separation

- Case: *Piedmont Booksellers Guild v. City of Norvale*.
- District docket: `SYN-DSC-26-CV-0107`; record resource `ca4m4.norvale.record`; JA1–JA149.
- Actual Fourth Circuit docket: `SYN-CA4-26-CV-4103`; PA1–PA135.
- Counterfactual training docket: `SYN-CA4-26-CV-4103-CF-NEVER-OCCURRED`; PA136–PA234.
- Every B-coded document is a never-filed counterfactual artifact. No B-coded fact may be described as part of the actual docket, and no actual-history question may use a B-coded page as proof of what occurred.
- The City is the initiating/appellant party; the Guild is the responding/appellee party. The district court, Fourth Circuit clerk, and fictional composite panel are synthetic institutional actors.

## Merits facts that must remain consistent

- The Guild is a nonprofit association whose members organize recurring, free-to-attend public book fairs at Cedar Commons. Books are sold and authors receive modest honoraria, but admission and author programs are free.
- Norvale's Paid-Speaker Permit Ordinance applies when an event organizer pays a speaker. The City asserts pedestrian-flow, vehicle-circulation, emergency-access, sanitation, and staffing interests. It disclaims viewpoint review.
- The Guild challenges advance application, fee, and permit burdens on paid literary presentations. Its evidence includes recurring reservations, site materials, organizer and member declarations, and less restrictive coordination alternatives.
- The City relies on its legislative history, pilot-permit experience, parks declaration, and police-logistics declaration. The parties dispute whether the record demonstrates a materially different crowd or safety burden caused by paid speakers.
- The district court grants preliminary protection on January 23, 2026. The merits entitlement remains protected in the actual appellate disposition; the only actual remand is for a superseding order that complies with Rules 65(c) and 65(d).
- The actual opinion and judgment must state that the existing protection remains in force until the district court enters a superseding compliant order. The current structured-disposition schema has no separate machine-readable `non_vacatur` or `protection_remains` field.
- The Guild's events recur. Expiration of one identified fair does not establish that the operative permit regime or the Guild's exposure to it has ended.

## Frozen chronology

| Date | Actual lower-record or appellate event |
|---|---|
| 2025-08-18 | The City adopts Ordinance N-2025-18. |
| 2025-11-03 | The standardized ordinance materials, application, fee schedule, and legislative-history packet are issued. |
| 2025-12-15 | The City completes the pilot-permit and enforcement-history packet. |
| 2026-01-05 | The Guild files the verified complaint and preliminary-injunction motion. |
| 2026-01-07 | The Guild files three declarations and the site/photo/calendar/reservation exhibit. |
| 2026-01-12 | The City answers. |
| 2026-01-14 | The City files its opposition and two declarations. |
| 2026-01-16 | The Guild replies; the parties file stipulated facts and the alternatives chart. |
| 2026-01-19 | The district court holds the preliminary-injunction hearing. |
| 2026-01-23 | The district court enters the preliminary-injunction order. |
| 2026-01-26 | The City first seeks a stay in district court. |
| 2026-01-27 | The Guild opposes the district stay. |
| 2026-01-28 | The City replies and the district court denies a stay. |
| 2026-01-29 | The City files its notice of appeal. |
| 2026-01-30 | The district docket is certified; the Fourth Circuit dockets the appeal; both parties file appearance/disclosure documents. |
| 2026-02-02 | The City files the actual Rule 8 motion after district-court denial. |
| 2026-02-04 | The Guild responds to the actual stay motion. |
| 2026-02-05 | The City replies. |
| 2026-02-06 | The panel denies the actual appellate stay. |
| 2026-02-09 | The City files its docketing statement and no-additional-transcript certificate; the clerk enters the record-complete/briefing order. |
| 2026-03-23 | The City files its opening brief and the joint-appendix cover/index/certification. |
| 2026-04-22 | The Guild files its response brief. |
| 2026-05-13 | The City files its reply brief. |
| 2026-05-15 | The City files its one-date mootness motion. |
| 2026-05-18 | The Guild files its recurring-events response. |
| 2026-05-20 | The panel denies the mootness motion. |
| 2026-05-22 | The clerk enters the oral-argument calendar notice for June 10. |
| 2026-06-10 | Argument is held and the appeal is submitted. |
| 2026-06-18 | The panel issues the actual opinion and judgment. |
| 2026-07-02 | The ordinary 14-day rehearing period reaches its authored due date; no actual petition or mandate-stay motion is filed. |
| 2026-07-06 | The actual no-petition workflow may advance after the due date. July 3 is treated as a Fourth Circuit calendar holiday. |
| 2026-07-09 | The clerk enters the ordinary release order and issues the actual mandate. |

## Counterfactual branch dates

- B01/B02 model a Rule 8 filing that omits both prior district relief and a specific impracticability showing, followed by threshold denial.
- B03/B04/B05 model a specific impracticability showing, response, and merits denial. B03/B04/B06 model the alternative grant, and B07 dissolves that temporary stay. These alternatives never coexist with the actual A04–A07 history.
- B23 (May 14), B24 (May 15), and B08 (May 18) create the expedited on-briefs submission lineage. B09/B10 issue the adverse opinion and judgment on May 22. That lineage does not use A18–A21.
- The adverse rehearing deadline is June 5; its no-petition advance occurs June 8; B25/B26 release and issue the adverse mandate on June 12.
- B11, B15, and B19 are mutually exclusive filings on July 2 and each belongs only to the actual A20/A21 disposition lineage. B12, B16, and B20 are alternative orders entered July 6.
- B13/B14 issue after rehearing denial on July 13. B17/B18 issue after mandate-stay denial on July 13. B21/B22 dissolve the granted stay and issue the mandate on August 5.
- A calculated 90-day outer date following the July 6 stay grant falls on October 5, 2026 after calendar handling; the authored August 5 dissolution occurs before that outer date.

## Disposition canon

The actual plan `ca4m4.norvale.disposition.authored-limited-remand` exercises § 1292(a)(1) jurisdiction, denies the City's mootness request, affirms preliminary-injunction entitlement, and grants only a limited Rule 65(c)/(d) corrective remand. It does not vacate or dissolve interim protection.

The adverse plan `ca4m4.norvale.disposition.counterfactual-reversal` exercises jurisdiction, denies mootness, reverses preliminary relief and remands, and denies the separate corrective-remand target. It exists only on the B08–B10 counterfactual lineage.

## Modeling boundary

- Emergency stay steps are serialized before merits briefing; the pack does not claim concurrent-clock modeling.
- Mootness, finality, and trigger selection are authored facts and exact workflow predicates, not conclusions inferred from arbitrary documents.
- Court authorization is enforced by role. The distinct originating-court role separates district operations, but the shared Fourth Circuit court role cannot distinguish clerk from panel authorization by actor identity.
- All dates, pages, source paths, and branch classifications must match `metadata/successor-document-plan.json`. Rendered hashes remain unset until deterministic source review and rendering finish.
