# Bench-profile contract

## Trust and labeling

MVP bench profiles are fictional or composite and must say so in their metadata. The label is
shown in selection, live argument, transcripts, and exports. A profile is interaction data,
not a claim about a real judge and not a merits model.

## Compatibility

Each profile declares:

- `court_roles`: one or both of district and appellate;
- one or more explicit compatible jurisdiction IDs;
- profile schema version and source kind;
- interaction controls and issue-focus weights.

Panel construction validates every seat. A district profile cannot occupy an appellate seat,
and a profile restricted to another jurisdiction cannot silently enter a Fourth Circuit panel.

## Interaction controls

All continuous controls are finite values in `[0, 1]`. Version 1 supports:

- directness, formality, and question length;
- interruption frequency and follow-up depth;
- hypothetical frequency and concession recall;
- record-pin demand and time strictness;
- normalized weights for jurisdiction, preservation, standard of review, record, remedy,
  limiting principle, and merits issues declared by the case.

Structured voice data supplies cadence, register, question framing, address convention,
verbosity, sentence complexity, and one to eight distinct literal phrases for each of questions,
interruptions, and clarifications. Braces and control characters are rejected rather than treated
as an open-ended template language. Version 1 has no separate preamble-length control: bounded
lead phrases and question framing are the operative preamble controls.

Controls influence the deterministic planner and renderer. They never select facts outside the
permitted case graph. A record-pin demand can be planned only when the selected issue has a
permitted record-page anchor.

## Typed bench acts

A bench act contains a stable ID, seat ID, intent opcode, issue ID, permitted source anchors,
clock effect, and optional link to the advocate act it follows. Rendered text is stored beside
the act for exact replay.

Required intent opcodes include clarification, premise test, record pin, authority challenge,
jurisdiction, preservation, standard of review, remedy, limiting principle, hypothetical,
concession confirmation, interruption, time notice, and yielding the floor.

## Invariants

- The same pinned definitions and advocate acts produce the same typed bench-act sequence.
- Renderers may vary prose only within the selected act and permitted source anchors.
- Profile-only changes do not alter legal state or authored disposition.
- Engine and renderer code contain no branch keyed to a particular profile ID.
- Profile IDs and display names are excluded from the behavior digest; every operative control,
  enum, phrase inventory, seat, and compatibility boundary is covered.
