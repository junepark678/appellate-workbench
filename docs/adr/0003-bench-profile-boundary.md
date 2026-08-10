# ADR 0003: Bench profiles describe interaction, not outcomes

- Status: accepted
- Date: 2026-08-11
- GitHub issue: #2

## Context

Realistic argument requires judges who do not all ask the same question in the same voice.
That variability must not turn a training simulator into unsupported psychological profiling
or judicial-outcome prediction.

## Decision

The MVP ships fictional or composite bench profiles only. A profile may configure observable
interaction: issue-focus weights, directness, formality, preamble and question length,
interruption rate, follow-up depth, hypothetical frequency, concession recall, record-pin
demands, and time strictness.

Profiles declare a court role and compatible jurisdictions. A bench contains one or more
typed seats; incompatible roles are rejected. Engine code never branches on a profile ID.

Three authorities remain separate:

1. The legal kernel owns facts, rules, deadlines, valid actions, and authored dispositions.
2. The bench planner selects a typed act from permitted issues, authorities, briefs, and
   record anchors.
3. The renderer turns that act into text but cannot add facts or mutate legal state.

Changing interaction-only profile data may change question choice, sequencing, and prose. It
cannot change filing validity, deadlines, record facts, canonical scoring, or an authored
disposition.

## Forbidden fields

Schemas reject ideology, party favorability, win probability, likely vote, outcome weight,
protected-trait inference, private biographical inference, and cloned voice data.

## Future sourced public profiles

A future pack may describe publicly observable courtroom behavior with citations, an as-of
date, persistent non-predictive labeling, and court-role compatibility. Such a pack is not in
the MVP and may affect interaction only.
