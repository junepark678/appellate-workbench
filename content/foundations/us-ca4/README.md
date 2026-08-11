# Fourth Circuit procedure foundation pack

This directory authors the immutable schema-v2 pack `foundation.us-ca4@2026.03.23`.
It exact-pins `foundation.us-federal@2025.12.01` and supplies the shared Fourth Circuit
procedure graph required by `docs/content/M4_CASE_MATRIX.md`.

The pack owns:

- the United States Court of Appeals for the Fourth Circuit and its 2025–2027 operational
  holiday calendar;
- a shared filing catalog and declarative forms;
- reusable civil-appeal, criminal-appeal, agency-review, and original-writ workflows;
- one procedure profile for each of those four proceeding types; and
- provenance-complete Fourth Circuit local procedural authorities.

The shared actor vocabulary is intentionally procedural: initiating party, responding party,
and court. Root packs give those roles their case-appropriate labels. This pack contains no
party, case, docket, fact, record, issue, grounded question, bench assignment, outcome,
disposition, or case-specific authority.

## Workflow template contract

Schema v2 does not merge or inherit workflows. The four procedure profiles and workflows in
this pack are reusable generic baselines. A root pack may reference one directly when the whole
baseline fits its authored trace. When a case needs different operations, routes, deadlines, or
preconditions, the root must own a uniquely identified, complete procedure profile and complete
workflow while reusing this pack's court, filing catalog, forms, and applicable authority sets.
It must not shadow a shared resource ID. In the M4 matrix, “workflow delta” describes authoring
lineage from one of these templates; it is not a runtime patch or composition mechanism.

Authority exposure is proceeding-specific. Civil appeals use the appellate, civil, and local
sets; criminal appeals use the appellate, criminal, and local sets; agency review and original
writs use the appellate and local sets. A root-owned replacement profile adds only the
case-specific authority sets its complete workflow requires.

The executable filing routes in the generic baselines are intentionally limited:

- civil appeal: civil notice of appeal and principal brief;
- criminal appeal: criminal notice of appeal and principal brief;
- agency review: petition for review, agency record, and principal brief; and
- original writ: writ petition, writ response, and motion.

The writ-response accept operation is executable only after the court enters a granted
`us.ca4.writ.order.response-requested` order. The baseline therefore does not imply that an
answer is due merely because a writ petition was accepted.

The agency-petition form names every party seeking review as required by FRAP 15(a)(2)(A) and
makes Local Rule 15(b)'s agency-order copy and respondent name/address list explicit. Its
executable route records the responding party as a required service recipient; under FRAP
15(c), that recipient may be served by the circuit clerk rather than the petitioner.
Service on nonrespondent participants admitted in the agency proceeding is not representable in
the shared three-role vocabulary; a root needing that trace must add complete roles and a
replacement workflow.

Civil and criminal notice routes likewise record the responding party as a required notice
recipient even though FRAP 3(d) assigns that service to the district clerk. For a writ, modeled
trial-court parties map to the initiating- and responding-party roles and both roles are required
service recipients, with the filing petitioner excluded by the engine. The writ-petition form
separately requires the issues, necessary facts, reasons for issuance, essential materials, and
certification that a copy was provided to the trial-court judge under FRAP 21(a).

Civil and criminal notice forms separately name the appealing parties, designate and date the
judgment or appealable order, and name the destination court as required by FRAP 3(c). Each
principal-brief route, the agency-record route, and the writ petition, response, and motion
routes require service on both procedural party roles. Roles identify individual actors rather
than collective sides, and the engine excludes the filing actor, so these lists represent
service on every other modeled party rather than self-service.

The shared catalog and forms are a broader reusable inventory. Appearance, docketing statement,
transcript order, rehearing petition, and appendix have no executable route in these baselines.
In particular, the civil, criminal, and agency workflows expose a court-event-driven operation
that calculates the docketing-statement deadline but do not bind that operation to petition or
notice acceptance and do not accept that filing. A root must invoke the calculation from its
authored docketing event. The calculation is court-authorized, as are the detached stage
transitions. Civil and criminal workflows require a Local Rule 31(b) advance-stage operation
after the notice is present and the record is received or complete; accepting a notice does not
advance to briefing. Agency petition acceptance advances to the record stage, where the court
can record the actual docketing event and its deadline; a separate court transition advances to
briefing only after the agency record is present. Writ-petition acceptance advances directly to
the submitted stage. A root must supply a complete replacement profile/workflow before claiming
executable support for an inventory-only filing or for a catalog filing outside the baseline
route listed above.

Deficiency operations emit a notice without inventing a generic cure clock. The reusable
workflows do not bind any deficiency deadline: a root trace must author the date supplied by its
case-specific notice or order. That distinction preserves Local Rule 45's notice-dependent
15-day remedy period without projecting it onto original writ practice, where Local Rule 21
supplies no generic cure period.

## Provenance freeze

Local rules, Federal Rules of Appellate Procedure, and Internal Operating Procedures are frozen
to the court's official `2026-03-23` compilation and were checked on `2026-08-11`:
<https://www.ca4.uscourts.gov/docs/pdfs/rules.pdf>.

The reusable sealed-material filing reference was checked on `2026-08-11` against the official
Fourth Circuit procedure guide:
<https://www.ca4.uscourts.gov/appellateprocedureguide/General_Provisions/SealedConfidMem.html>.

Calendar dates match the federal pack's observed-date baseline. Case-specific closure orders,
emergency orders, and confidentiality determinations remain root-owned and must not be inferred.
