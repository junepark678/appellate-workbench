# ADR 0001: Native, offline-first MVP

- Status: accepted
- Date: 2026-08-11
- GitHub issue: #1

## Context

The previous prototype mixed a browser application, hosted identity, a managed backend,
institution workflows, two product audiences, and legal simulation content. That shape made
deployment and product behavior inseparable and obscured the actual simulation loop.

## Decision

The MVP is a native Qt 6 Widgets workstation compiled in C++26 mode with CMake. It is fully
functional with outbound networking blocked. Linux x86_64 is the sole binary release target
for the MVP; source portability to macOS and Windows is desirable but is not a release gate.

The reference development toolchain is Qt 6.11, CMake 3.30 or newer, Ninja, and GCC in strict
C++26 mode. Clang is a required portability and warning check. “C++26 mode” means the
compiler exposes a C++26 language mode; every new language or library facility still requires
an SD-6 feature-test guard or a compile probe.

The authoritative working state is local SQLite plus a local content-addressed asset store.
There is no embedded web runtime, application server, account, organization, instructor,
assignment, telemetry, or network-required model in the MVP loop.

Optional asynchronous sync is a later replica layer. It transfers encrypted immutable
objects and never makes a remote service authoritative for simulation behavior.

## Release form

The MVP release produces a relocatable Linux x86_64 bundle and a checksum manifest. The
release process must document the glibc floor, Qt runtime components, and applicable licenses.
Packaging details may evolve without revisiting the product boundary.

## Consequences

- Every core scenario must run, save, resume, export, and replay with networking disabled.
- Local data recovery and migrations are product features, not operations work.
- Multi-user administration, live collaboration, and SaaS tenancy are out of scope.
- Platform expansion requires its own tested packaging and accessibility evidence.
