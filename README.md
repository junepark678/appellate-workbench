# Appellate Workbench

Appellate Workbench is a local-first native desktop simulator for realistic appellate
practice. It is being rebuilt from first principles in C++26 mode with Qt 6.

The application has no required server, account, institution, instructor, or browser
runtime. A local SQLite database will be the authoritative working store. Optional cloud
sync will replicate encrypted immutable objects; it will never sync a live SQLite file.

## Current status

This repository is a pre-MVP foundation, not a usable simulation yet. The first vertical
slice compiles a Qt Widgets shell, validates a small data-only content pack, rejects unsafe
paths and invalid hashes, and displays a fictional/composite bench profile. The GitHub
milestones define the route to the MVP.

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
- Qt 6.8 or newer with Core, Gui, Widgets, and Test;
- a compiler toolchain recognized by CMake as supporting C++26 mode.

The reference development environment is Qt 6.11.1, CMake 4.3.4, Ninja 1.13.2, and GCC
16.2. “C++26” here means the compiler's C++26 language mode is required; it is not a claim
that every C++26 library facility is implemented on every platform.

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Run the shell with the included fixture pack:

```sh
./build/dev/src/app/'Appellate Workbench' tests/fixtures/minimal-pack
```

See [the product contract](docs/PRODUCT.md) and
[the architecture](docs/ARCHITECTURE.md) before expanding the codebase.
