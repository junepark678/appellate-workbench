# Norvale v1.2.0 validation scaffold

Evidence status as of **2026-08-12**: all 73 synthetic sources are frozen; the fresh
production render and schema-v2 record are accepted; seven non-workflow core resources
have been copied byte-for-byte into the pack candidate; and the workflow is frozen after
schema, resolved, reachability, replay, and hostile review. The canonical traces, manifest,
realism review, root, deterministic archive, and resolved install are final. Only the focused
test result remains to be recorded below.

## Evidence pins

| Evidence item | Current value | Status |
| --- | --- | --- |
| Path-framed source closure | `e85ec3ff6c0fefabec9b1884c1256af09ed431c0f1c19f0a0d7bafe8d1ba0780` | Frozen: 73 sources / 383 logical pages |
| Successor document-plan SHA-256 | `b950388515198f489b00857965c3db053421b811b7bd7675cf26b1ddf3100ea4` | Frozen planning/allocation contract |
| Render plan SHA-256 | `24b1a198889301f3b294d2d865479f1821a9bb0ede4e5ae8965ea09b1efed773` | Frozen: schema v1 / 73 rows |
| Accepted render-inventory SHA-256 | `12de3cd55506d0d3d11f2140381e129013995db5f615c7ead987b1e2e8786470` | Frozen: 73 PDFs / 383 physical pages |
| Renderer-emitted raw inventory SHA-256 | `5b9ed49acaab2d4f3ae9d8d3615e6c37cb3c5217309ec8858a2555e608f5e78b` | Same JSON value as accepted inventory; byte formatting differs |
| Record-resource SHA-256 | `a25bb89f96b78bbf7b084b50c4327953ed0af602359e49460dd1e10ef48306c3` | Frozen: 3 dockets / 73 entries / 383 anchors |
| Ordered raw PDF corpus SHA-256 | `09478456855e46cd3bf4a8ba6abb44a14d38761f4be7b7dc8aa0d87cb28e47f1` | Frozen: accepted inventory output-path order |
| Framed PDF corpus SHA-256 | `9f3ec6f5843e067562363883636b14867c1b9dd55447fd6744b291178d1ac448` | Frozen |
| Raw-inventory-plus-PDF closure SHA-256 | `c33d42fc122a969ffe11b9d95f7dd32ce8683330b3722a122366aa8117917034` | Frozen render-audit closure |
| PDF corpus size | `2,639,080` bytes | Frozen: 73 unique output hashes |
| Actual argument resource SHA-256 | `b99581558b2dfc53a294cf6bf357104b5eeb610fbf479aaef78e398b774200fc` | Frozen core resource |
| Counterfactual argument resource SHA-256 | `8fb8c1348b1f03afc45e8335e2f3ff84f055f875e60a247e907014ae871040f9` | Frozen core resource |
| Authority-set resource SHA-256 | `2f046241ffb7b802b54bd98b5079256da708f4f25ce2bfd770c333c5e84d713d` | Frozen: eight authorities |
| Bench-configuration resource SHA-256 | `89f52481e722dac8dddc310010fe43308fe03ce72357b0d0d67ac14f6ba9853f` | Frozen: three seats |
| Case resource SHA-256 | `2f24762e0548d6e2c62544f6bf0918ba08c472d97677271a2251ee67e0c261e1` | Frozen: four issues / two disposition plans |
| Procedure-profile resource SHA-256 | `4bb5fee6613a9a1e300d12d9a4d30248ee8478d1076d9f712e2f768bb852bdee` | Frozen core resource |
| Actual question-bank grounding digest | `a7b9d3f45093cd389d57fea1522b1c2ae80fac29bf5705dd5b24033694c6f4ea` | Recomputed from frozen authority/record bytes |
| Counterfactual question-bank grounding digest | `5b5559db07537e94046dffc733b8f3f104f09533ff9ffede190f4e1758260ecb` | Recomputed from frozen authority/record bytes |
| Workflow resource SHA-256 | `1b285f65a38c4be2a7bc8dbe29d3822aee2963d05019fbe5f79d5917272cc74a` | Frozen: 16 stages / 90 operations / 13 routes / 24 filing bindings / 31 document bindings / 2 dispositions / 14 concrete deadline IDs (12 logical concepts) |
| Workflow hostile audit | `056370216e48f8cd04e0f078e31706b18d17c228e2df9ec1b7abc176fd7d4ea2` | Clear: 90/90 operations reachable; 2,212/2,212 replay mutations rejected |
| Canonical trace bundle | `829b060c1fbda0ecdb09ee7624ed6a2ab178f4ad44216b04558403b1754af709` | Frozen: 9 traces / 316 commands / 334 events / exact 90-operation union |
| Manifest SHA-256 | `e89b902b39d22bcc4f5b1aa407d754e665e1243d196dc4af3703816c355f46e4` | Final: 9 resources / 73 blobs / 16 capabilities / 3 dependencies |
| Realism-review SHA-256 | `8870e5c8c10e956552f99c5069a2fbc6874402cc07e062bca38706ba565bc4e8` | Level 2 in all seven dimensions; `independent_review_pending` |
| Exact evidence-closure digest | `1170d682b46773d09b63b5dcfcd5b7c485c2f792881c94027b76550ef021d82c` | Exact 4 packs / 44 resources / 73 blobs / 9 traces / 2 checks / 35 authorities |
| Root revision | `a51383c0c1edcd56153b36291177425b09846ab607c39c28030820ef700df05f` | Final installable revision |
| Deterministic archive | 4,744,009 bytes; `a4b993aa3cc6582d1d0f6ca9a7203109378f4f1c1b2e6ce32efbfe82b6a48e19` | Two byte-identical deferred exports; fresh four-revision resolved install passed |
| Focused tests | Core `a610b8cf47a9bdc3d1ad2cbf0b4979e59b2bbbe296c9c05b52e9385eaebd8aec`; UI `d959c1ecaec43bf8294fa698749a997c80dab391f7f0064ad7a5515fd37264ed` | Local 2/2 passed in 39.37 seconds; `TZ=UTC` 2/2 passed in 39.15 seconds |

## Canonical trace pins

| Trace | Commands / events | Final stage | File SHA-256 | Journal SHA-256 | Trace digest |
| --- | ---: | --- | --- | --- | --- |
| Actual through mandate | 56 / 59 | `terminated` | `e013c167acaaf0100eb5c4c23f40fe86592cc475380e928e23cb91bc6ed7e219` | `f4e5b154b7e50eb6a39ca85f3053b0871ddc8cb9e5277d0822313746572f55fd` | `b1d4843ba9c10145b319eacade64d1919ec1a2ccdd95345e43539253e7772af3` |
| Rule 8 threshold | 4 / 4 | `appellate-stay` | `9e40a2379c8f8f57b98a20b7a3e0eaad2ff50cbaa2b0e232a8e8c6ec0ff15f33` | `6c60a78ce1b2d373b15a0929e744385bce135fab850af55a627f53923f75681e` | `6b72e66b0c3c4894edb9a9a5dcf6cdd6ab8d5bbfb6d0aba34afb95a5155f261e` |
| Impracticability denial | 5 / 5 | `appellate-stay` | `03e23116c63243599db88316658d8dc9b9a5e7c7e4e221218ea3f5e89b4fd047` | `1ddfe2c88bfae561a1f8a93cc0e0a4da77dc17ce2424417caddb448000ad379d` | `c647ef86b841c961c5c4967a9895b6e6cf9638c017f8823d165652a2000a3304` |
| Stay grant and dissolution | 6 / 6 | `appellate-stay` | `bc717657a552719a306cd59777c086db90a50e3e49b3f0c8bae07c75cbad2ae0` | `3010ceb30e9cfc7711ca3668d170d5016a60bb4cedb00f336233efea44435ee3` | `d593561719b425be28d0e85413b7649e91373888fd4956370f628388f79f73f6` |
| Adverse on briefs | 44 / 47 | `terminated` | `d90d83aa967c0f38510aa894eee3a3314c3315810b472382cc3e771f0e11918c` | `73e784c4cd2f318c134c4f9c1afc797ffb2e7f71e18dbeeb53761c10f0668b40` | `9d651e70ada761957b933e2880ecc4fba0f43556d29337a09db39848bff2f97a` |
| Rehearing denial | 52 / 55 | `terminated` | `af6ff0861cf3404dbd7c4d4929b68e852983e8e003d02c5008594ed12f36bcff` | `d1960e280650ae5839d7d8a52e76a209b2cea05f265ca0ec42efb3ed9225ee71` | `81eca7eed467449d0f05e7d363cec25b7a0c58180001e1b9efe15e2c59191bc1` |
| Mandate-stay denial | 51 / 54 | `terminated` | `866e114af890a59c529bfa963dbb78d99055e6c29cfbcf8ddbcecf0b0881d048` | `04bb68e861b89f97b9012dc36865e512fcff6abe73a409c7d4ef94fbe342a032` | `d9cc719f41a8d1f925a9af784f04878bb4fb62698684bdcfc3ba76e966639732` |
| Mandate-stay grant blocked | 47 / 50 | `mandate-stayed` | `b59c112aea175bdd13479f68e46a9aff9808804bdec52fe112d9c22974d115a9` | `cd2d92ab6e6f01e608c73221ab75484fbbf01843810d550eda80ac9728ed29e5` | `dbb507b1f2299aec12a1cb264bc2d9e92ffb468166d390adc1649d0cd62662bf` |
| Stay dissolution release | 51 / 54 | `terminated` | `3992654f2876e9106f306a07da29409148c94fda54b717c38e9f2abe1301f954` | `3a6fa31312092c2b55723880dbdcb9e7b95c2d7b13d2930014b2c7b51d5b2f34` | `feff28559f694de81b8112b34da82e849eed5247b5278aa98e44558980d4acf8` |

## Completed source, render, and record gates

The following claims are confined to the frozen source/render/record bytes:

1. The source plan resolves 24 lower sources to JA1–JA149, 23 actual appellate
   sources to PA1–PA135, and 26 never-filed branch sources to PA136–PA234. The
   arithmetic is 24 + 23 + 26 = 73 documents and 149 + 135 + 99 = 383 pages. All
   73 row statuses and the top-level source status are `source_review_clear`; the
   top-level render status is `rendered_accepted`.
2. All 383 source pages meet the authoring density floor after headings and synthetic
   front matter are excluded. Duplicate substantive-paragraph and cross-page 30-word
   sequence scans are clear.
3. A fresh invocation of the production `appellate-render` utility emitted exactly 73
   PDFs and 383 physical pages. Every PDF passed `qpdf --check`, `pdfinfo` page count,
   US-Letter/unencrypted checks, searchable extracted-text checks, and exact JA/PA footer
   checks. L16 is exactly 22 physical pages.
4. Inventory source hashes, assembly provenance, plan hashes, output hashes, byte sizes,
   page counts, and labels match accepted bytes. All 73 PDF hashes are unique.
5. The schema-v2 record has exactly three dockets, 73 entries, and 383 unique anchors.
   JA1–JA149 and PA1–PA234 are each contiguous. All B entries use only the
   counterfactual docket and carry both `never_filed` and
   `never_occurred_on_actual_docket`.
6. Actual and counterfactual grounded-question digests independently recompute from the
   exact authority propositions/provenance and record asset/anchor bytes.

These gates establish provenance and internal consistency. The separately frozen workflow
establishes its stated schema, reachability, replay, and branch-isolation properties. Neither
slice establishes legal correctness, accessibility conformance, installability, deterministic
export, or independent professional review.

## Completed workflow and release gates

The final artifacts verify all of the following:

1. the frozen workflow contains exactly 16 stages, 90 operations, 13 filing routes, 24 exact
   filing bindings, 31 document bindings, two structured-disposition bindings, and 14 concrete
   deadline IDs representing 12 logical concepts; all 90 operations are runtime-reachable;
2. each of the nine named canonical journals is frozen, complete, and replays through the production
   engine, with branch coverage and command/event totals recorded rather than inferred;
3. actual-history operations, actual disposition, and actual-record questions cannot use
   PA136–PA234, while counterfactual operations remain on their separate docket;
4. all nine resources, 73 blobs, 16 capabilities, and three exact dependencies close in
   the manifest without undeclared or missing members;
5. deferred validation, dependency installation, resolved validation, two deterministic
   exports, and archive-byte comparison pass against the same root revision;
6. the final realism review binds the exact manifest, dependencies, journals, record
   checks, and selected authorities without claiming independent review; and
7. focused case/UI tests pass locally and under `TZ=UTC` against the same root and archive.

The result is an installable level-2 authoring candidate. It is not a level-3 independent-review
or gold claim.

## Procedural and merits consistency gates

Final validation must preserve these authored boundaries:

- The permit is content neutral on its face and triggered by paying speakers; free
  admission and bookselling do not eliminate the First Amendment issue, while neutral
  City traffic and staffing interests remain genuine rather than pretextually erased.
- The fairs have operated peacefully since 2019 with no violence, two minor medical
  calls, and routine congestion. The parties continue to dispute whether paid speakers
  produce materially distinct burdens and whether less restrictive coordination would
  address the City's risks.
- The 2026 fair dates are February 14, April 11, June 13, August 8, October 10, and
  December 12. One expired date cannot silently become proof that the operative permit
  regime or recurring dispute has ended.
- The actual Rule 8 motion follows district-court stay practice. B01–B07 are mutually
  exclusive threshold, impracticability, grant, and dissolution alternatives; none
  occurred on the actual docket.
- B02, B05, and B07 are intentionally scoped nonterminal endpoints in the appellate-stay
  stage. They cannot enter the actual A08/A10/A20 record and merits lineage; the workflow
  does not claim complete counterfactual notice, docketing, briefing, or mandate histories
  for those three teaching exercises.
- The actual appellate result preserves preliminary-relief entitlement and remands only
  for Rule 65(c)/(d) correction. The structured plan cannot independently encode
  non-vacatur or continued protection, so narrative documents and evidence must remain
  aligned.
- Emergency stay work is serialized before merits briefing. The model makes no general
  concurrency, automated mootness, arbitrary-filing classification, or actor-identity
  authorization claim.

## Reproduction scaffold

The following commands reproduce the pack-closure gate from the repository root:

```sh
jq empty content/m4/norvale-injunction/metadata/render-inventory-successor.json
jq empty content/m4/norvale-injunction/pack-candidate/manifest.json

norvale_check_root="$(mktemp -d)"
norvale_catalog="$norvale_check_root/catalog"
norvale_archive="$norvale_check_root/us-ca4-m4-norvale-injunction-1.2.0.awpack"

./build/dev/src/cli/appellate-pack export-deferred \
  content/m4/norvale-injunction/pack-candidate "$norvale_archive"
./build/dev/src/cli/appellate-pack install \
  content/foundations/us-federal/foundation-us-federal-2025.12.01.awpack "$norvale_catalog"
./build/dev/src/cli/appellate-pack install \
  content/foundations/us-ca4/foundation-us-ca4-2026.03.23.awpack "$norvale_catalog"
./build/dev/src/cli/appellate-pack install \
  content/foundations/us-ca4-fictional-bench/foundation-us-ca4-fictional-bench-1.0.0.awpack \
  "$norvale_catalog"
./build/dev/src/cli/appellate-pack install "$norvale_archive" "$norvale_catalog"
./build/dev/src/cli/appellate-pack validate-resolved \
  "$norvale_catalog" us.ca4.m4.norvale-injunction 1.2.0 \
  a51383c0c1edcd56153b36291177425b09846ab607c39c28030820ef700df05f

ctest --test-dir build/dev --output-on-failure \
  -R '^m4_norvale_injunction(_ui_e2e)?$'
TZ=UTC ctest --test-dir build/dev --output-on-failure \
  -R '^m4_norvale_injunction(_ui_e2e)?$'
```

Schema-v2 thin roots with dependency-owned references require deferred export, exact dependency
installation, and resolved validation. Those gates passed for the exact root above; ordinary
standalone validation is not a substitute.

## Independent-review gate

Automated checks and authoring review cannot establish First Amendment accuracy, current
Fourth Circuit practice, practical realism, or level 3. A qualified First Amendment and
appellate practitioner must review an exact finalized root. Any change to the record,
authority set, questions, workflow, traces, manifest, or dependency topology invalidates
earlier review evidence.
