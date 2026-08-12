# Realism release matrix

Scores are evidence, not estimates. `0` means missing, incorrect, contradictory, or placeholder;
`1` means plausible but generic; `2` means source-grounded, internally consistent, and
test-backed; `3` means branch-complete and independently expert-reviewed.

Every shipped case must score at least 2 in every dimension. The designated gold case for each
procedure profile must score 3 throughout. A material legal error, impossible state, invented
off-record fact, unresolved citation, or incompatible bench role blocks release regardless of
the numeric scores.

An installable authoring candidate may appear below at level 2 while independent review remains
pending. That status is useful technical/content evidence, but it is not a release-ready gold
designation and does not satisfy the level-3 gate.

| Case family | Procedure | Law | Deadlines / authority | Record | Consequences | Argument | Bench differentiation | Provenance | Review state |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| Employment retaliation | Civil | 2 | 2 | 2 | 2 | 2 | 2 | 2 | Authoring evidence complete; qualified independent review pending |
| Rule 54(b) finality | Civil (gold candidate) | 2 | 2 | 2 | 2 | 2 | 2 | 2 | Installable successor level-2 evidence; qualified independent review pending |
| Preliminary injunction / stay | Civil | 0 | 0 | 0 | 0 | 0 | 0 | 0 | Source/render/record and workflow evidence frozen; traces, review, root, archive, and tests pending |
| Section 1983 qualified immunity | Civil | 0 | 0 | 0 | 0 | 0 | 0 | 0 | Not authored |
| Post-trial JMOL | Civil | 0 | 0 | 0 | 0 | 0 | 0 | 0 | Not authored |
| Sealed FOIA | Civil | 0 | 0 | 0 | 0 | 0 | 0 | 0 | Not authored |
| Criminal sentencing / waiver | Criminal (gold) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | Not authored |
| Immigration agency review | Agency (gold candidate) | 2 | 2 | 2 | 2 | 2 | 2 | 2 | Authoring evidence complete; qualified independent review pending |
| Privileged-discovery mandamus | Writ (gold) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | Not authored |

For each nonzero score, the case review file must link the exact authority version, automated
trace, record-consistency report, reviewer identity or status, review date, uncertainty notes,
and remedial issue for any exception. Self-authored content cannot claim level 3 until an
independent qualified reviewer records attributable review metadata. That metadata is a declared
social-trust assertion, not a cryptographic signature or proof of identity. New schema-version-2
candidates use the capability-gated exact evidence contract in `docs/spec/PACKS.md`; changed
descriptors, replay journal data, record-check data, authority IDs, pack metadata, or dependency
topology invalidate that evidence.

The checked-in Asterglen `0.1.0` pack predates the schema-version-2 contract. Its EDVA
`SYN-25-0117` / appellate `SYN-26-1427` bytes and revision digest remain frozen. The new `0.2.0`
candidate instead owns an independent NDWV `SYN-NDWV-25-CV-0618` / appellate
`SYN-CA4-26-CV-4102` record: 37 lower-court PDFs/234 JA pages, 13 actual appellate PDFs/70 PA
pages, and 25 isolated branch PDFs/73 PA pages, for 75 PDFs/377 anchors. It contains nine resources,
16 capabilities, three disposition plans, and eight production traces. Its authoring review is
level 2 throughout and `independent_review_pending`; final root
`7e77bc0fbe02dc9e108681df73852859d6d0f577acdcb65fcfb7678eac78b728` and closure
`445c3f11dcc8046eedfc233407699cbbb3ea4e39425d22c976808959350ca62c` are installable. A future
level-3 claim must be a detached review pack that exact-pins the finalized successor and supplies
qualified independent reviewer metadata; neither immutable root is rewritten to manufacture
level 3.

The A.R.M. `1.2.0` root is the agency gold candidate, not the agency gold pack. Its exact
level-2 evidence closes four packs, 44 non-review resources, 54 blobs, seven production-engine
traces, two record checks, and 32 authorities. The root review remains
`independent_review_pending`; a qualified immigration/appellate review must be recorded in a
detached pack that exact-pins the reviewed root before every dimension may become 3.

The Benton retaliation `1.2.0` root is a non-gold civil authoring candidate. Its exact level-2
evidence closes four packs, 44 non-review resources, 67 blobs, seven production-engine traces,
two record checks, and 28 authorities, with 148 unique non-pack evidence IDs. Its review remains
`independent_review_pending`; replay and author review do not substitute for qualified independent
employment/appellate review or change Asterglen's designation as the civil gold candidate.

The Norvale injunction `1.2.0` authoring candidate remains level 0 in this release matrix even
though its source/render/record slice is frozen. That accepted slice contains 24 lower-record
PDFs/149 JA pages, 23 actual appellate PDFs/135 PA pages, and 26 separately docketed,
never-filed PDFs/99 PA pages, for 73 PDFs/383 anchors. Its frozen workflow SHA-256 is
`1b285f65a38c4be2a7bc8dbe29d3822aee2963d05019fbe5f79d5917272cc74a`, with
16 stages, 90 operations, 13 filing routes, 24 exact filing bindings, 31 document bindings,
two structured dispositions, and 14 concrete deadline IDs representing 12 logical concepts.
All 90 operations are reachable across nine planned paths, but the checked-in canonical trace
and release evidence remain pending. `PENDING_TRACE_PINS`,
`PENDING_MANIFEST_SHA256`, `PENDING_REVIEW_SHA256`, `PENDING_ROOT_REVISION`,
`PENDING_ARCHIVE_SHA256`, and `PENDING_TEST_PINS` must be replaced from exact accepted bytes
before the row may claim level 2. Automated record checks do not substitute for qualified
independent First Amendment/appellate review or make Norvale a gold candidate.
