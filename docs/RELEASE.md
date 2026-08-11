# Native release procedure

The MVP binary target is a relocatable Linux x86_64 bundle. macOS and Windows source
portability remain useful checks, but they are not release targets for version 0.1.0. The bundle
does not require a server, account, browser runtime, or network connection.

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
pack/persistence versions, source revision, and exact bundled pack hashes. A release must be
built and smoke-tested on the declared floor before publication.

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
runtime/plugins discovered by Qt's deployment API, the immutable Asterglen pack, compatibility
metadata, and operator documentation. Extracting the top-level directory is installation;
removing that directory is uninstallation. User databases, catalog objects, and settings live
under the platform data/config directories and are deliberately not deleted with the program.

The MVP bundle deliberately contains only the X11/XWayland (`qxcb`) and headless (`qoffscreen`)
Qt platform plugins, plus the SQLite driver. Native Wayland and all other Qt plugin families are
outside this exact artifact contract. The automated gate proves the plugin file allowlist and
the offscreen load path. A release image must additionally launch the real desktop through
`qxcb` under Xvfb or a clean X11/XWayland session; that check cannot be claimed on a build host
without either service.

The automated `linux_bundle_smoke` test installs into a fresh prefix, validates the bundled pack,
launches the real desktop offscreen through `--smoke-test`, relocates the whole prefix, launches
again, and removes the temporary prefix. The normal local desktop E2E separately exercises pack
installation, SQLite save/resume, record CAS materialization, workflow judgment, actual and
counterfactual oral argument, restart/replay, and tamper refusal.

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
