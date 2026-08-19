# Native release procedure

The current pre-MVP binary target is a relocatable Linux x86_64 bundle. macOS and Windows source
portability remain useful checks, but they are not release targets for the current application
release. The bundle does not require a server, account, browser runtime, or network connection.
Its primary case payload is the dependency-resolved Asterglen v0.2.0 pack. Asterglen v0.1.0 is
also retained byte-for-byte as compatibility evidence. The bundle additionally carries all eight
M4 v1.2.0 roots as non-default payloads. All nine finalized v2 roots are level 2 with qualified
independent review pending; none is level 3 or gold.

## Build identity

A release is built from a clean, signed `vMAJOR.MINOR.PATCH` tag. Configure the release preset
with the exact tag commit and with the oldest glibc version used by the release build image:

```sh
cmake --preset release \
  -DAPPELLATE_RELEASE_SOURCE_REVISION="$(git rev-parse HEAD)" \
  -DAPPELLATE_GLIBC_FLOOR=<oldest-tested-glibc-version> \
  -DAPPELLATE_RELEASE_SIGNING_FINGERPRINT=<40-hex-signing-key-fingerprint>
cmake --build --preset release
ctest --preset release
ctest --test-dir build/release -L packaging --output-on-failure
cpack --config build/release/CPackConfig.cmake
```

The example glibc value is not a promise for arbitrary builds. The compatibility manifest in
each artifact records the configured floor, compiler, Qt version, application version, supported
pack/persistence versions, source revision, and exact bundled pack hashes and byte sizes. A
release must be built and smoke-tested on the declared floor before publication.

CPack emits one `.tar.zst` bundle and a sibling SHA-256 checksum. Package publication also
requires a detached signature made outside the build tree with the maintainer's protected
release key. Signing credentials never belong in this repository or a CMake cache.

CPack independently refuses a dirty tree, a source-revision mismatch, a missing or misplaced
version tag, an invalid tag signature, or a signature from a key other than the configured
fingerprint. For local packaging mechanics only,
`-DAPPELLATE_ALLOW_UNVERIFIED_DEVELOPMENT_PACKAGE=ON` produces an artifact and manifest marked
`development-unverified`; that artifact name is forced and it must never be published.

## Installed layout and relocation

The archive contains the desktop workstation, pack and renderer command-line tools, the Qt
runtime/plugins discovered by Qt's deployment API, 13 exact immutable pack archives,
compatibility metadata, and operator documentation. The `bundled_packs` array records these in
the following fixed order:

| Index | Pack/version | Archive SHA-256 | Root revision SHA-256 | Bytes |
| ---: | --- | --- | --- | ---: |
| 0 | `us.ca4.rule54b.asterglen` 0.2.0 | `10739c149a3bf2617d8af6dd131caee7ea6639a9d97e26cdf2974fa176c82819` | `7e77bc0fbe02dc9e108681df73852859d6d0f577acdcb65fcfb7678eac78b728` | 3,974,147 |
| 1 | `foundation.us-federal` 2025.12.01 | `69736648f78376a6d85cde32148337edbf5af2a289de6070734c5454cc6b411b` | `866c90996c15e2076b9508a297ffce1a4e766b1432a9e11d08e8138c57e363c9` | 21,002 |
| 2 | `foundation.us-ca4` 2026.03.23 | `5c9098d76012891ab2cb1f04c48bdcb3101c64253fdaab1608de789d0f5aa6ef` | `449d75c77e5c47883f750377450f2d1ec1fc0e42e20b1f247446b208661d3262` | 66,512 |
| 3 | `foundation.us-ca4-fictional-bench` 1.0.0 | `e2758217f5ba9b987cc9e9920af65f762263f420e1698b12732d4f02b0121137` | `cee0bf93309cc9ad800f215a47d734b20a9fdf5dc889f2f440e4382b942d332d` | 14,131 |
| 4 | `us.ca4.rule54b.asterglen` 0.1.0 | `ce0ebffb92942e85e02658d11846af70ebb5fdc287f99a4c683a48f381e39227` | `ff7a2e1195f9bd006e7df46c19675a3e07a4bd8975b1643a01adbc9cc4fd3424` | 729,511 |
| 5 | `us.ca4.m4.cinderlake-writ` 1.2.0 | `eeefbbbe84cf4addbf91a68447281217226c6a08c7e0e3e1294947d5e5dc8956` | `020517571a6c15f90765e12b94ab53d8598be3bc3081d47caecdf5950bacd05c` | 2,519,053 |
| 6 | `us.ca4.m4.arm-agency` 1.2.0 | `a150903c6c3332d8de582a8ef46e7fd1dd17cee0ac52c93c0ebaf51313cf54d2` | `ae33933c7cf18f77e662eb302d563afd860e8e900bac8debb081b81b35404edb` | 3,286,508 |
| 7 | `us.ca4.m4.benton-retaliation` 1.2.0 | `9515bdde1e3405e6e82488abd73314a31c33a2062f9e34b4cecdaaff8b634a05` | `59467350af5f381ef429ecf210d38de5503d40fb2e9baf02f56b2ef5023ced28` | 3,408,701 |
| 8 | `us.ca4.m4.norvale-injunction` 1.2.0 | `a4b993aa3cc6582d1d0f6ca9a7203109378f4f1c1b2e6ce32efbfe82b6a48e19` | `a51383c0c1edcd56153b36291177425b09846ab607c39c28030820ef700df05f` | 4,744,009 |
| 9 | `us.ca4.m4.ellison-immunity` 1.2.0 | `59f32f521644bac61865cf1e59444fc98dbb9007461a1709272ffe261cbad1d0` | `c2a4f3bc07f05eb1429257320ed839ebaea837da7aa7330f4669bbb157168ce0` | 4,230,462 |
| 10 | `us.ca4.m4.blueember-jmol` 1.2.0 | `c6332ae33e351ccb27ed17b5576b147a47f9f5f0b44583365212b1781a288ed2` | `08d88e4811e8ed8ad6e642cc041365508808f7158862aa93199de867f31431ec` | 5,326,158 |
| 11 | `us.ca4.m4.opengrid-foia` 1.2.0 | `1efa067767f3c729bbd67c40b3faa239673025f421133bddf32ec6b090231b09` | `9cb2879b1cc27e98d8def7c926a38e9f4eb2cbec90785be74c009156b4a1e4c5` | 5,244,039 |
| 12 | `us.ca4.m4.serrano-waiver` 1.2.0 | `d76686cec2053f78334c73f1c3aac415b637e733f0494b527001368597a1c243` | `9b4941e97292faa0fceda1f1c719f6e38ce8478c82350c7fbbb74a010c27d344` | 3,453,568 |

Configure fails if any of those checked-in bytes differ. Both the installed-prefix and CPack
archive gates require the exact 13-path `.awpack` allowlist, so neither a generated starter nor
an undeclared pack can enter the artifact. Extracting the top-level directory is installation;
removing that directory is uninstallation. User databases, catalog objects, and settings live
under the platform data/config directories and are deliberately not deleted with the program.

The Linux bundle deliberately contains only the X11/XWayland (`qxcb`) and headless (`qoffscreen`)
Qt platform plugins, plus the SQLite driver. Native Wayland and all other Qt plugin families are
outside this exact artifact contract. The automated gate proves the plugin file allowlist and
the offscreen load path. A release image must additionally launch the real desktop through
`qxcb` under Xvfb or a clean X11/XWayland session; that check cannot be claimed on a build host
without either service.

## Local session and backup boundary

The production desktop places its workflow/oral SQLite database and paired content-addressed
asset store under Qt's application-local data path. It does not accept a network endpoint,
credential, or secret for this path. One provider retains the validated database lease and gives
its in-process workflow/oral controllers explicit child connections. A second application process
fails promptly as state-in-use, leaving record browsing available instead of blocking startup.

This is cooperative single-owner protection, not a filesystem sandbox. Anchored descriptors and
no-follow identity checks reject ordinary path replacement, but a malicious process running as the
same user can still race SQLite's pathname-based reopen behavior outside the cooperative lock. Do
not place the state directory where another same-UID process is untrusted.

`SessionStore::backupTo` is a database-only, session-metadata backup operation. It rejects a
snapshot containing asset references; the MVP does not yet provide a paired database-plus-CAS
backup format. A current-schema, asset-free restore receives a fresh authoritative store identity
and can bind only to a fresh empty asset store. A backup must therefore never be described as a
document-bearing case export.

The automated `linux_bundle_smoke` test installs into a fresh prefix and independently checks all
13 manifest entries against their exact IDs, versions, archive hashes, root revisions, sizes,
and relative paths. Using the shipped CLI, it then installs the federal, CA4, and fictional-bench
foundations followed by Asterglen v0.2.0. `validate-resolved` must return the exact four-revision
closure before the ordinary desktop smoke loads v0.2.0 from that catalog. The smoke output is a
compact schema-version-1 JSON object; the gate requires the exact command, status, pack ID,
version, revision digest, one-case count, and case ID. Asterglen v0.2 remains the primary/default
payload.

For each of the eight M4 roots, the same gate creates a separate catalog, installs exactly those
three foundations followed by that root, validates the ordered four-revision closure, and loads it
offscreen with a unique XDG state root. The gate requires the aggregate executed-root token set to
contain exactly Asterglen v0.2 and the eight M4 pack/revision/case identities. It then relocates the
whole prefix and repeats every catalog, resolved-closure, JSON-identity, and desktop check using
only relocated binaries and archives. This proves exact archive integrity, dependency resolution,
runtime projection, and desktop loading for all nine finalized v2 roots. It does not establish
qualified independent review, level 3 or gold status, or complete the MVP.

The existing deterministic offline workflow E2E remains pinned to the immutable Asterglen v0.1.0
archive. Its UI driver knows that predecessor's single visible transition; the current generic UI
transition affordance cannot supply the first order/filing required by v0.2.0. This compatibility
flow therefore must not be cited as v0.2 workflow-progression evidence. The ordinary desktop gate
above is the packaging proof that the resolved v0.2 runtime is the content that loads by default.

The v0.1 compatibility pack does not contain a grounded oral-argument question bank. The
installed-flow gate therefore invokes the installed artifact's own `appellate-pack template`
command and exports that embedded schema-2 grounded starter to an exact-hash archive outside the
install prefix. The same sequence is repeated with the relocated CLI; both template extraction and
export run inside a user/network namespace. Through the real shipped executable and user import
path, the gate persists and reopens one workflow filing, its CAS bytes, and one grounded oral answer
while proving the workflow rows are unchanged by oral practice. Generated starter archives remain
temporary verification output and are rejected by the artifact allowlists. This proves
artifact-alone functional workflow/oral persistence plumbing and v0.1 compatibility, not the
complete Asterglen v0.2 legal simulation required for MVP acceptance.

The normal local desktop E2Es separately exercise pack installation, SQLite save/resume, record
CAS materialization, workflow judgment, actual and counterfactual oral argument, restart/replay,
and tamper refusal.

## Offline and clean-system gate

Before release, run the extracted artifact in a disposable machine matching the declared glibc
floor with all network interfaces disabled. Repeat the installed smoke test and one complete
simulation/save/reopen flow. Record the image digest, commands, artifact checksum, and result in
the release evidence. The application contains no telemetry path; optional sync must remain
unconfigured and disabled for this drill.

## Licensing gate

Qt deployment copies shared runtime libraries and plugins rather than statically linking them.
The release must include the exact Qt and third-party license texts and notices corresponding to
the binaries in that artifact, plus the means required by the selected Qt license. Qt 6.8 and
later publish SPDX SBOM data that should be archived with the release evidence. No artifact may
be published until the project owner records the project's own distribution license and verifies
the resulting Qt, libarchive, compiler-runtime, PDF/image/ICU, and other transitive obligations.

This document describes the engineering gate; it is not legal advice.
