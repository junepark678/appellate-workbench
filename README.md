# Appellate Workbench

Appellate Workbench is a local-first native desktop simulator for realistic appellate
practice. It is being rebuilt from first principles in C++26 mode with Qt 6.

The application has no required server, account, institution, instructor, or browser
runtime. A local SQLite database will be the authoritative working store. Optional cloud
sync will replicate encrypted immutable objects; it will never sync a live SQLite file.

## Current status

This repository is an active pre-MVP implementation, not a complete simulation yet. It now
contains strict declarative-pack validation and immutable installation, a native case/profile
browser, a deterministic legal workflow engine, crash-safe SQLite/event-log and content-addressed
storage, a searchable native PDF record workspace, and exact save/replay tests. Installed pack
PDFs are verified into local content-addressed storage and can be reopened without the original
archive. The GitHub milestones define the remaining route to the content-complete MVP.

The content target deliberately retains the useful breadth of the earlier prototype:

- one implemented jurisdiction at launch: the U.S. Court of Appeals for the Fourth Circuit;
- four proceeding profiles: civil appeal, criminal appeal, agency review, and original writ;
- nine substantive synthetic case families with rich lower-tribunal records;
- versioned data-only packs for future jurisdictions, procedures, cases, records, and bench
  profiles without recompiling the application when existing engine capabilities suffice.

Judge profiles configure observable interaction—issue focus, directness, formality,
interruptions, follow-ups, hypotheticals, and time management. The MVP ships only
fictional/composite profiles. Profiles cannot change facts, procedural validity, deadlines,
or an authored disposition.

## Build

Prerequisites:

- CMake 3.30 or newer;
- Ninja;
- Qt 6.8 or newer with Core, Gui, Pdf, PdfWidgets, Sql, Widgets, and Test;
- libarchive;
- a compiler toolchain recognized by CMake as supporting C++26 mode.

The reference development environment is Qt 6.11.1, CMake 4.3.4, Ninja 1.13.2, and GCC
16.2. “C++26” here means the compiler's C++26 language mode is required; it is not a claim
that every C++26 library facility is implemented on every platform.

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Run the dedicated local desktop end-to-end test after building the development preset:

```sh
cmake --build --preset dev --target \
  tst_oral_argument_desktop_e2e tst_desktop_command_line tst_local_folder_provider
ctest --preset e2e
```

The E2E preset runs the real Qt desktop shell offscreen against an exported and installed
fixture pack, local SQLite session storage, and content-addressed assets. It also exercises the
encrypted local-folder provider from atomic publication through authenticated quarantine restore.
It requires no server, cloud service, browser driver, account, or GitHub Actions runner.

Run the shell with the included fixture pack:

```sh
./build/dev/src/app/'Appellate Workbench' tests/fixtures/full-resource-pack
```

Create, validate, export, and install a complete declarative starter pack:

```sh
./build/dev/src/cli/appellate-pack template /tmp/my-appellate-pack
./build/dev/src/cli/appellate-pack validate /tmp/my-appellate-pack
./build/dev/src/cli/appellate-pack export /tmp/my-appellate-pack /tmp/my-pack.awpack
./build/dev/src/cli/appellate-pack install /tmp/my-pack.awpack /tmp/appellate-pack-catalog
./build/dev/src/cli/appellate-pack list /tmp/appellate-pack-catalog
```

Complete a manifest-declared schema-version-2 realism-review scaffold (scores 0-1) from a canonical
executed trace after installing its exact dependencies in the local catalog:

```sh
./build/dev/src/cli/appellate-pack author-realism-evidence /tmp/my-appellate-pack \
  /tmp/appellate-pack-catalog my.pack.review /tmp/canonical-trace.json
```

The CLI emits one compact, schema-versioned JSON object to stdout on success and stderr on
failure. `template` never overwrites an existing destination. See the
[pack contract and CLI reference](docs/spec/PACKS.md) before editing a generated pack.

Start with [the product contract](docs/PRODUCT.md), [the architecture](docs/ARCHITECTURE.md),
and the accepted decisions:

- [native offline MVP](docs/adr/0001-native-offline-mvp.md);
- [declarative pack trust boundary](docs/adr/0002-declarative-pack-trust-boundary.md);
- [bench-profile boundary](docs/adr/0003-bench-profile-boundary.md).

Content work is measured against the [frozen parity inventory](docs/PARITY_INVENTORY.md) and
[realism release matrix](docs/REALISM_MATRIX.md).
