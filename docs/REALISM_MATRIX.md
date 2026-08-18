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
| Preliminary injunction / stay | Civil | 2 | 2 | 2 | 2 | 2 | 2 | 2 | Installable level-2 evidence complete; qualified independent review pending |
| Section 1983 qualified immunity | Civil | 2 | 2 | 2 | 2 | 2 | 2 | 2 | Installable level-2 evidence complete; qualified independent review pending |
| Post-trial JMOL | Civil | 2 | 2 | 2 | 2 | 2 | 2 | 2 | Installable level-2 evidence complete; qualified independent review pending |
| Sealed FOIA | Civil | 2 | 2 | 2 | 2 | 2 | 2 | 2 | Installable level-2 evidence complete; qualified independent review pending |
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

The Norvale injunction `1.2.0` root is an installable level-2 civil authoring candidate. Its
accepted record contains 24 lower-record
PDFs/149 JA pages, 23 actual appellate PDFs/135 PA pages, and 26 separately docketed,
never-filed PDFs/99 PA pages, for 73 PDFs/383 anchors. Its frozen workflow SHA-256 is
`1b285f65a38c4be2a7bc8dbe29d3822aee2963d05019fbe5f79d5917272cc74a`, with
16 stages, 90 operations, 13 filing routes, 24 exact filing bindings, 31 document bindings,
two structured dispositions, and 14 concrete deadline IDs representing 12 logical concepts.
Its nine canonical traces contain 316 commands and 334 events with exact 90-operation coverage.
The final root is `a51383c0c1edcd56153b36291177425b09846ab607c39c28030820ef700df05f`;
its evidence closure is `1170d682b46773d09b63b5dcfcd5b7c485c2f792881c94027b76550ef021d82c`.
The review remains `independent_review_pending`; automated evidence does not substitute for
qualified independent First Amendment/appellate review or make Norvale a gold candidate.

The Ellison immunity `1.2.0` authoring candidate records level 2 in every dimension with
`independent_review_pending`. Its frozen corpus contains 37 lower-record PDFs/275 JA pages,
15 actual appellate PDFs/91 PA pages, and 20 separately docketed, never-filed counterfactual
PDFs/83 PA pages, for 72 PDFs/449 anchors. Workflow
`cd69b276a63ae508ba0d98bbee15585847a405b5b55a48df69fffe45811ca23a` and successor plan
`82f1afa17e4d15a192cc6567ff3ffaa3415d2dd95f609aa58c760c026c78273d` produce six canonical
traces with 229 commands/events, exact 77-operation coverage, 14 same-ID filing recoveries,
24 court documents, 11 named deadlines, and five terminated plus one stayed endpoint. The
final manifest `8f0d614a73a4850a93170a7338229b64e2b1d042134e678785eaab481fd8ca42`
closes nine resources, 72 blobs, 16 capabilities, and three exact dependencies. Review
`5545977962535b58f029de10959cf2f9e49348a12fb4f7bff17574aa688b8867` binds evidence closure
`8032c5547dd522cad241b9c816bd611d198dbe7e007b0cd29781b8b471de41ac`: four packs, 44
non-review resources, 72 blobs, six traces, two record checks, 35 authorities, 159 unique
non-pack evidence IDs, and dimension-reference counts 49/19/75/33/17/4/114. Exactly one
successful authoring invocation produced root
`c2a4f3bc07f05eb1429257320ed839ebaea837da7aa7330f4669bbb157168ce0`; the deterministic
4,230,462-byte archive is
`59f32f521644bac61865cf1e59444fc98dbb9007461a1709272ffe261cbad1d0`. Fresh resolved install
and runtime checks made 458 command redecisions, 229 prefix replays, 12 full replays, and 66
hostile-mutation rejections. This is installable level-2 evidence, not level 3 or gold. The
PDF-only corpus contains no MP4, the engine makes no video inference, and synthetic PA92–PA174
remain isolated from actual-history grounding.

The Blue Ember JMOL `1.2.0` authoring candidate records level 2 in every dimension with
`independent_review_pending`. Its frozen corpus contains 42 lower-record PDFs/430 JA pages,
16 actual appellate PDFs/108 PA pages, and 25 separately docketed, never-filed counterfactual
PDFs/118 PA pages, for 83 PDFs/656 anchors. Actual Rule 50(a) preservation remains fixed to
causation only; B01 alone supplies the isolated counterfactual express-mitigation premise.
Workflow `7c2356718286505eee16d62b48ca281f92eee367c9e21319ddcae02d87c1a120` and successor plan
`664b8632be87d885cebc4625282f0b452c5d376be607bc889ee86012c3ddcee5` bind six canonical
traces with 270 commands/events, exact 93-operation coverage, 18 unique same-ID filing
recoveries across 61 rejection instances, 25 court documents, 14 deadlines, and five
terminated plus one stayed endpoint. Manifest
`2b545cee1aaba7a1475b2f5085ae93d50ec9e3255a68f9762d3bec63492a8dac` closes nine resources,
83 blobs, 16 capabilities, and three exact dependencies. Review
`8fe8d9b06f38ca16fe535c917c3da4b2c6d92a5ee2f17924d49a945d2e5e0688` binds evidence closure
`3f38cd1a12f7f61c037f338fef4f1600ab83434208aa13a8ff5cf56a52fe5d5a`: four packs, 44
non-review resources, 83 blobs, six traces, two record checks, 39 authorities, 174 unique
non-pack evidence IDs, and dimension-reference counts 53/19/86/37/20/4/129. Exactly one valid
successful authoring call produced root
`08d88e4811e8ed8ad6e642cc041365508808f7158862aa93199de867f31431ec`; the deterministic
5,326,158-byte archive is
`c6332ae33e351ccb27ed17b5576b147a47f9f5f0b44583365212b1781a288ed2`. Fresh four-revision
resolved installation and the 78-mutation installed hostile audit passed. Frozen focused-test
commit `1d497aecd9e9fdf2ab5ffa4f47ad994052cd304f` passed normal and UTC integration/UI runs with
zero skips. This is installable level-2 evidence, not level 3 or gold. The generated PDF-only
corpus contains no MP4 or raw instrument-data file; automated checks do not substitute for
qualified independent civil-trial, posttrial-motions, and appellate review.

The Open Grid FOIA `1.2.0` authoring candidate records level 2 in every dimension with
`independent_review_pending`. Its frozen corpus contains 37 lower-record PDFs/290 JA pages,
19 actual appellate PDFs/155 PA pages, and 28 separately docketed, never-filed counterfactual
PDFs/209 PA pages, for 84 PDFs/654 anchors. The record closes 17 public/sealed disclosure pairs
through 172 bijective page mappings, including 22 named stable-anchor subjects. Workflow
`66361faed640a8e274834b49595061b51d5fc41b1ef34b4d494f0ca49de67503` has 35 stages,
101 operations, 17 routes, 19 public filing bindings, 22 public court-document bindings,
12 deadlines, a 41-entry public provenance union, and no sealed binding. Five canonical traces
contain 221 commands/events, cover all 101 operations, include 39 rejection events, and end at
four terminated plus one mandate-stayed endpoint; the plan-order trace closure is
`9247eb77b22dc166db60ef7d984ac70114b00af4387cd61ebdc1d377654b02ae`.

Manifest `94ff5a6f3e89686c6119578033cbec7218afe5b0543ddbebba6e54e787bf3f70`
closes nine resources, 84 blobs, 17 capabilities, and three dependencies. Review
`2f0b0b06fd7de51dd2fa53466458b3e3e4ba7bc1c181ea0342c543e65b36ba34` binds evidence closure
`1acce5699c11f75736f2f611e0eae6158f37f26970bf1f7d8b99ddbd85956534`: four packs,
44 non-review resources, 84 blobs, five traces, two record checks, 33 authorities, 168 unique
non-pack evidence IDs, and dimension counts 46/20/87/32/18/4/124. Exactly one successful
authoring call produced root
`9cb2879b1cc27e98d8def7c926a38e9f4eb2cbec90785be74c009156b4a1e4c5`; the deterministic
5,244,039-byte archive is
`1efa067767f3c729bbd67c40b3faa239673025f421133bddf32ec6b090231b09`. Fresh four-revision
resolved validation, 221 command redecisions and prefix replays, ten full replays, and 65 hostile
mutation rejections passed. The authored record checks bind exact record/blob descriptors and
anchor-bearing record bytes; separate installed runtime evidence establishes grant/revoke,
cross-root denial, per-open CAS/PDF/page verification, and snapshot isolation. Focused-test commit
`f2192e7462f12faf58d67b297216fecedba53a28` passed normal and `TZ=UTC` integration/UI runs
2/2 with zero skips in 85.43 and 85.29 seconds. This is installable level-2 evidence, not
qualified independent review, level 3, or gold.
