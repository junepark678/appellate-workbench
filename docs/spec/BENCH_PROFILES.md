# Bench-profile contract

## Trust and labeling

MVP bench profiles are fictional or composite and must say so in their metadata. The label is
shown in selection, live argument, transcripts, and exports. A profile is interaction data,
not a claim about a real judge and not a merits model.

## Compatibility

Each profile declares:

- `court_role`: district or appellate;
- compatible court IDs or an explicit fictional wildcard;
- profile schema version and source kind;
- interaction controls and issue-focus weights.

Panel construction validates every seat. A district profile cannot occupy an appellate seat,
and a profile restricted to another jurisdiction cannot silently enter a Fourth Circuit panel.

## Interaction controls

All continuous controls are finite values in `[0, 1]`. Version 1 supports:

- directness, formality, preamble length, and question length;
- interruption frequency and follow-up depth;
- hypothetical frequency and concession recall;
- record-pin demand and time strictness;
- normalized weights for jurisdiction, preservation, standard of review, record, remedy,
  limiting principle, and merits issues declared by the case.

Controls influence a deterministic planner through documented weights and a recorded seed.
They never select facts outside the permitted case graph.

## Typed bench acts

A bench act contains a stable ID, seat ID, intent opcode, issue ID, permitted source anchors,
clock effect, and optional link to the advocate act it follows. Rendered text is stored beside
the act for exact replay.

Required intent opcodes include clarification, premise test, record pin, authority challenge,
jurisdiction, preservation, standard of review, remedy, limiting principle, hypothetical,
concession confirmation, interruption, time notice, and yielding the floor.

## Invariants

- The same pinned inputs, seed, and advocate acts produce the same typed bench-act sequence.
- Renderers may vary prose only within the selected act and permitted source anchors.
- Profile-only changes do not alter legal state or authored disposition.
- Engine and renderer code contain no branch keyed to a particular profile ID.
