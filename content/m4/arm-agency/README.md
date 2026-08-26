# A.R.M. agency-review authoring pack

This tree authors the installable schema-v2 root `us.ca4.m4.arm-agency@1.2.0` for the
fictional exercise *A.R.M. v. Attorney General of the United States*
(`SYN-BIA-25-0113`; `SYN-CA4-25-AG-4301`). Its immutable root revision is
`ae33933c7cf18f77e662eb302d563afd860e8e900bac8debb081b81b35404edb`; the
deterministic archive SHA-256 is
`a150903c6c3332d8de582a8ef46e7fd1dd17cee0ac52c93c0ebaf51313cf54d2`.
This is a synthetic authoring candidate, not legal advice or an independently reviewed gold
pack.

All people, addresses, facts, events, documents, dockets, and the Republic of Kalyra are
fictional. A.R.M. remains anonymized by initials; the exercise does not invent a full name,
street address, birth date, registration number, or other unnecessary personal identifier.
Real United States courts and agencies appear only as institutional procedural actors. The
three shared bench profiles are fictional/composite profiles.

## Exact dependencies and closure

- `foundation.us-federal@2025.12.01`
  (`866c90996c15e2076b9508a297ffce1a4e766b1432a9e11d08e8138c57e363c9`)
- `foundation.us-ca4@2026.03.23`
  (`449d75c77e5c47883f750377450f2d1ec1fc0e42e20b1f247446b208661d3262`)
- `foundation.us-ca4-fictional-bench@1.0.0`
  (`cee0bf93309cc9ad800f215a47d734b20a9fdf5dc889f2f440e4382b942d332d`)

The root owns nine resources and 54 PDF blobs and declares 14 capabilities. It owns the
case-specific facts, authorities, record, workflow, argument configurations, structured
disposition, and realism review while reusing the shared court, roles, filing catalog, forms,
rules, and fictional/composite profiles. The resolved realism-evidence closure is exactly four
packs, 44 non-review resources, 54 blobs, seven traces, two record checks, and 32 authorities;
its closure digest is
`5718e65aff5986c9640dc75ca99995482bf0065b1191a5154c4d934de785aa2c`.

## Record and appellate-docket boundary

The certified administrative record remains exactly 18 PDFs and 238 substantive searchable
pages with continuous labels AR1–AR238. The successor also contains 36 separately classified
post-agency PDFs and 177 PA-labeled pages:

- PA1–PA8 is T.R.'s post-order declaration and proffer. It was never before the immigration
  judge or Board, is not part of the administrative record, and cannot ground CAT-merits facts.
- PA9–PA127 is the actual Fourth Circuit docket, including the petition and cure, record motions
  and rulings, three briefs, argument, opinion, judgment, and mandate.
- PA128–PA177 is a separately docketed, explicitly counterfactual training bank. Those thirteen
  documents were never filed and cannot ground the actual disposition or actual-record bank.

The complete root therefore contains 54 PDFs and 415 searchable pages/anchors, while the
certified-record floor remains 18 PDFs and 238 pages. The itemized ranges and classifications are
in `metadata/record-inventory.md`; exact asset hashes are in the pack record and manifest.

Agency Exhibit P-7 is A.R.M.'s pre-hearing sworn declaration at AR33–AR50. AR117, AR138, AR139,
AR161, and AR164 record its admission without an authenticity or timeliness objection.
AR219–AR226 preserve the initial transmission and its unexplained eighteen-label gap. The exact
Rule 16(b) stipulation, receipt tables, file audit, P-7 digest, itemized index, and corrected
certification at AR227–AR238 establish that the same admitted file was omitted by a stale status
field and restored without adding evidence. The court grants correction of that record and
denies supplementation with PA1–PA8.

P-4, P-5, and P-6 remain distinct admitted logical exhibits within the one physical ten-page
file at AR105–AR114. That file contains complete controlled searchable source and translation
bytes, their exact hashes, and an exhaustive page/object map. It contains no raster image object
and makes no contrary representation.

## Runtime, disposition, and realism boundary

The workflow has 13 stages, 67 operations, and 11 filing routes. Exact role subsets,
filing/order-instance guards, a statically addressable deficiency deadline, and bound
order/judgment/mandate documents distinguish the actual path from six adverse branches. Seven
canonical journals replay through the production engine: actual mandate, uncured-deficiency
dismissal, invoked day-31 dismissal, day-31 government forfeiture, rehearing denial followed by
mandate, stay denial followed by mandate, and stay grant blocking mandate.

The authored disposition has seven nonoverlapping components. It grants correction, denies
supplementation, rejects a threshold dismissal of the actual day-29 petition, grants the petition
in part, vacates the BIA order for the aggregate-risk and official-acquiescence errors, remands,
and denies a direct appellate CAT award. The actual and counterfactual grounded banks contain
15 and 10 questions respectively, with exact authority and AR/PA grounding.

These contracts authenticate authored identities and bytes; they do not infer the legal meaning
of a novel filing, independently classify a final order, or support parallel stay-and-merits
work. Those are recorded limitations, not hidden engine claims.

The root review `ca4m4.arm.review.authoring-2026-08-12` assigns level 2 to all seven realism
dimensions and records seven typed uncertainties. Its state is
`independent_review_pending`. A qualified immigration/appellate reviewer must review the exact
root revision before any level-3 or gold claim; that future review belongs in a detached pack and
must not rewrite this immutable root.

## Render boundary

The accepted batch-1 invocation remains provenance for unchanged documents 01, 02, 04, 06, and
the separate PA1–PA8 proffer. Corrected batch-1 documents 03 and 05 and all twelve batch-2
documents were emitted by the single canonical-repair invocation described by
`render-plan-canonical-repair.json`; its inventory SHA-256 is
`aa6d2cd94ad41e0f3ad6f4a220083792036d1c5f972ba86eeef902267d0236cf`.

The 35 successor documents were emitted together by one clean invocation of
`render-plan-successor.json` into a fresh directory. Their canonical inventory is
`metadata/render-inventory-successor.json` (SHA-256
`b29e419b9b92dc60c2b014381d9172bd753931825a48266368e0ed2472b7669c`).
The invocation produced exactly 169 searchable PA9–PA177 pages with no raster images; source,
inventory, record, manifest, and repository bytes close exactly. Re-running a renderer is not
claimed to reproduce these exact PDF bytes.
