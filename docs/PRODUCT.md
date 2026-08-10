# Product contract

The platform boundary in [ADR 0001](adr/0001-native-offline-mvp.md) is binding on this
contract; changes that add a server, account system, browser runtime, or network requirement
must replace that accepted decision explicitly.

## MVP promise

Appellate Workbench is a single-user, local-first native workstation in which a learner can
take realistic synthetic appellate matters from initiation through disposition, including
record work, filing consequences, briefing, and a text-based oral argument. It remains fully
usable with networking disabled.

The launch jurisdiction is the Fourth Circuit. The parity floor inherited from the useful
parts of the prototype is nine case families across civil appeal, criminal appeal, agency
review, and original writ. The previous prototype's 283 docket/document pairs are a breadth
reference, not permission to ship boilerplate records: every shipped document must be
substantive, internally consistent, and actually used by the simulation.

## Explicit exclusions

The MVP does not include:

- accounts, organizations, cohorts, instructors, assignments, or institutional reporting;
- an application server, web UI, SaaS control plane, or real-time collaboration;
- executable third-party plugins;
- network-required models or services;
- judicial-outcome prediction, likely-vote fields, ideology scores, or voice cloning;
- a WYSIWYG authoring studio.

Users expand the application by installing and exchanging versioned data-only packs. The
MVP includes schemas, validation, import/export, and preview tooling. New legal semantics
require a later, trusted application release rather than downloaded executable code.

## Realism contract

Each case is scored from 0 to 3 for procedural law; deadlines and actor authority; record
consistency; consequences; oral argument; bench differentiation; and provenance:

- 0: wrong, contradictory, or placeholder;
- 1: plausible but generic;
- 2: source-grounded, internally consistent, and test-backed;
- 3: branch-complete and independently expert-reviewed.

Every shipped case must score at least 2 in every category. One gold case in each proceeding
profile must score 3. A material legal error, impossible state, invented off-record fact, or
unresolved citation blocks release regardless of averages.

## Bench profiles

The simulation separates legal state from presentation:

1. The legal kernel owns facts, authorities, deadlines, valid actions, and authored outcomes.
2. The bench planner emits typed, record-grounded question and interruption acts.
3. The dialogue renderer realizes those acts in a configurable voice without adding facts or
   changing legal consequences.

The MVP ships fictional/composite profiles. A future public-judge pack may describe sourced,
publicly observable courtroom interaction with an as-of date and persistent non-predictive
labeling, but it is not part of the MVP and cannot affect outcome logic.

## Sync boundary

Cloud sync is optional and asynchronous. It replicates encrypted, content-addressed pack
revisions, session event segments, and checkpoints through a provider-neutral object API.
Local SQLite remains the working database. Concurrent histories become visible branches;
neither silently wins. Core simulation behavior never depends on sync availability.
