# Encrypted immutable-object sync protocol

Status: protocol version 1. The envelope and identity codec are implemented; provider adapters,
OS-key-store integration, logical object payload codecs, and branch import remain separate slices.

Sync is an optional replica layer. It is never a database, a requirement for simulation, or a
channel for executable code. The application remains fully usable when this target is disabled,
when no provider is configured, and when every remote operation fails.

## Allowed and forbidden objects

Version 1 has exactly four logical object kinds:

| Code | Kind | Canonical payload |
| ---: | --- | --- |
| 1 | `pack_revision` | One deterministic `.awpack` plus its exact pack revision descriptor |
| 2 | `authored_revision` | One immutable user-authored document revision and its parent reference |
| 3 | `session_event_segment` | A bounded canonical sequence of complete committed command/event batches |
| 4 | `checkpoint` | A canonical session/branch head, projection digest, pins, and parent heads |

The kind-specific payload codecs will be versioned independently inside the encrypted envelope.
An authored revision is a user-produced document revision, not a pack-supplied record asset. Its
local model must assign a stable revision ID, an optional parent revision ID, a media type, and the
content digest before it is eligible for sync. The current raw asset store is not itself such a
revision catalog and must not be swept into sync.

The following are never logical sync objects:

- a SQLite database, database backup, WAL page, journal, table dump, or row mutation;
- a plaintext record, record page, pack member, authored document, event, or checkpoint;
- a cache, projection table, temporary file, log, crash report, credential, or key;
- an executable, script, plugin, model, or provider-specific control record.

Pack archives and authored documents may contain sensitive text, but only the complete encrypted
logical object described here can leave the local device. Local SQLite and the local
content-addressed stores remain authoritative.

## Dependencies and primitive choices

The wire codec uses libsodium 1.0.14 or newer and calls `sodium_init()` before cryptographic work.
The reference build currently uses libsodium 1.0.22.
`APPELLATE_ENABLE_SYNC` defaults to `ON`; an enabled configure fails if the dependency is absent.
An explicitly offline/core-only build may pass `-DAPPELLATE_ENABLE_SYNC=OFF`, in which case no sync
target or sync test is configured and the rest of the product has no libsodium dependency.

- Object streams use `crypto_secretstream_xchacha20poly1305`. Libsodium documents that this API
  detects modification, removal, reordering, duplication, and truncation; encrypting the same
  stream twice produces different ciphertext; and the final message must carry `TAG_FINAL`.
- Remote object IDs use keyed `crypto_generichash` with a 32-byte key and 32-byte BLAKE2b output.
  Libsodium documents keyed generic hashing as a PRF and recommends its standard key/output sizes.
- Canonical logical object IDs remain unkeyed SHA-256 so they are portable content identities.
  They occur only inside authenticated ciphertext and local state.
- Recovery capsules will use XChaCha20-Poly1305 and explicit Argon2id parameters. Libsodium warns
  that password-hash algorithm, operations limit, memory limit, and salt must be stored with the
  derived-key format rather than replaced later by whatever defaults happen to be current.

Primary references:

- <https://doc.libsodium.org/secret-key_cryptography/secretstream>
- <https://doc.libsodium.org/doc/hashing/generic_hashing>
- <https://doc.libsodium.org/password_hashing/default_phf>

No bespoke cipher, MAC, password hash, random generator, or nonce allocator is permitted.

## Canonical identity

Integers are unsigned big-endian. Text constants below include their terminating zero byte. For a
kind code `K`, payload schema `S`, payload byte length `L`, and exact canonical payload `P`:

```text
canonical_id = SHA-256(
    "appellate-workbench-sync-object-v1\0" ||
    K:u8 || S:u16 || L:u64 || P
)
```

For the canonical test vector `K = 1`, `S = 1`, `P = "abc"`:

```text
canonical_id = 206e6f9ad620996c78eb3cc120de5477977d3ae0b097f4e2d1a9a447d5825173
```

Publishing a bare SHA-256 would permit a provider to test guesses for known packs. Each vault
therefore has an independent random 32-byte object-ID key. Its provider-visible name is:

```text
remote_id = BLAKE2b-256(
    key = vault_object_id_key,
    message = "appellate-workbench-sync-remote-id-v1\0" || canonical_id
)
```

With key bytes `00, 01, ..., 1f`, the vector above yields:

```text
remote_id = bc1fc006d361b6041c8161fa0ef04bdbc1199dadd93adac9698f2c28325c5527
```

Providers receive only lowercase hexadecimal remote IDs. Every kind shares one namespace:

```text
objects/<first two remote-ID characters>/<complete remote ID>.awobj
```

Separate type directories, case names, session IDs, plaintext digests, descriptive content types,
tags, and user-defined metadata are forbidden. A receiver recomputes both IDs after authenticated
decryption and rejects an object moved to a different remote key.

## Version 1 envelope

The public fixed header is 48 bytes:

| Bytes | Meaning |
| ---: | --- |
| 8 | `41 57 53 4f 00 01 00 00` (`AWSO`, major version 1, zero flags) |
| 16 | Random key-slot ID |
| 24 | libsodium secretstream header |

The encrypted inner stream begins with this 52-byte record:

| Bytes | Meaning |
| ---: | --- |
| 8 | `41 57 4f 42 4a 00 01 00` (`AWOBJ`, format 1) |
| 1 | Exact object-kind code |
| 2 | Kind-specific payload schema |
| 1 | Reserved zero byte |
| 32 | Canonical logical object ID |
| 8 | Unpadded payload length |
| variable | Exact canonical payload |
| variable | Cryptographically random padding |

The inner stream is split into plaintext messages of at most 65,536 bytes. Each wire frame is a
four-byte big-endian ciphertext length followed by the secretstream ciphertext. Ciphertext length
must be between `ABYTES` and `65,536 + ABYTES`. All nonfinal frames use `TAG_MESSAGE`; the last
uses `TAG_FINAL`. Version 1 rejects `PUSH` and caller-requested `REKEY` tags. Secretstream performs
its own internal rekeying for long streams.

Each frame authenticates this additional data:

```text
"appellate-workbench-sync-envelope-v1\0" ||
public_header || remote_id:32 raw bytes || frame_index:u64
```

This binds ciphertext to its protocol, key slot, stream header, provider key, and position. A
receiver rejects an unknown slot, malformed length, invalid tag, missing final tag, early final
tag, trailing byte, payload over its configured limit, canonical-ID mismatch, or remote-ID
mismatch. It decrypts only into an owner-only auto-removing quarantine file. Nothing is installed,
imported, or exposed as a successful payload until the complete stream and both identities verify.

## Size leakage and bounds

The 52-byte inner header plus payload is padded as follows:

1. Up to 4 KiB, pad to 4 KiB.
2. Above 4 KiB through 1 MiB, pad to the next power of two.
3. Above 1 MiB, pad to the next whole MiB.

Padding is random and authenticated. The default logical payload ceiling is 3 GiB, matching the
pack contract's total declared-blob ceiling; callers and providers may enforce a smaller limit.
Frame lengths and arithmetic are checked before allocation. Provider integration must stage
ciphertext on local disk and stream it; it must not buffer a multi-gigabyte pack in memory.

Encryption is randomized, so two devices can produce different ciphertext for the same remote ID.
Create-only provider writes make either valid ciphertext representation acceptable. On an
already-present response, the client downloads, authenticates, and checks the logical identity;
the object name alone is never proof of valid content.

## Metadata disclosure

Encryption does not make object storage anonymous. The provider can observe:

- its account, bucket and configured opaque prefix, the connecting IP, and request timing;
- object count, blinded equality/deduplication, and retry/list/download access patterns;
- the padding bucket, protocol version, and which ciphertexts share a public random key-slot ID.

The provider cannot obtain from compliant object paths, headers, or ciphertext:

- object kind, plaintext or canonical digest, filename, media type, pack or case identity;
- session identity, event type, branch ancestry, checkpoint relationships, or legal content;
- provider secrets, encryption keys, recovery material, or local SQLite structure.

The first version accepts equality, coarse size, traffic, and key-slot grouping leakage. Hiding
those properties would require an oblivious service or cover traffic, which is outside the local,
provider-neutral MVP. Product documentation must disclose these limits.

## Keys, rotation, and recovery

A vault keyring contains a stable random 32-byte object-ID key, one or more independently random
32-byte data-encryption keys indexed by random 16-byte slot IDs, and the current write slot. The
production implementation must keep this keyring in locked memory only while needed, wipe working
copies, and persist it only through the OS secret store. Provider access key, secret, and session
token are separate OS-secret-store entries. SQLite, settings, logs, command lines, environment
variables, and provider objects hold only nonsecret configuration and opaque key-store references.

On Linux the planned adapter is QtKeychain with insecure fallback disabled. If Secret Service,
KWallet, or another supported OS store is unavailable, sync setup and invocation fail closed;
simulation, pack use, local save/resume, and local backup continue unchanged. Tests inject keys and
never require a desktop keyring.

Routine rotation creates a fresh random encryption key and slot for new objects. Old read slots
remain until every reachable object has been verified under a replacement. Objects are immutable,
so rotation never rewrites ciphertext at an existing remote ID. A suspected compromise requires a
new vault object-ID key, new encryption keys, and a new remote prefix. Verified plaintext objects
are re-encrypted into that namespace; the old namespace is retained until the new graph passes a
complete restore drill and is never deleted automatically.

Recovery export will create a versioned capsule containing the vault ID, object-ID key, encryption
slots, and current slot. The capsule is authenticated-encrypted under a key derived from a user
recovery passphrase with Argon2id and a random salt. It stores the exact algorithm/version,
operations limit, memory limit, salt, and AEAD format used for that export. Restore decrypts and
validates the complete capsule in memory before writing a new OS-key-store item. Wrong passwords,
modified capsules, unsupported parameters, duplicate slots, or invalid vault identifiers produce
no key-store mutation. Export and restore require an explicit user action and a rehearsal from a
clean local profile.

## Provider and branch invariants for later slices

The provider interface will expose only paged list, stat, create-if-absent upload, and download.
It will have no overwrite, delete, rename, SQL, or plaintext metadata operation. A local-folder
adapter will atomically publish owner-only ciphertext files. The S3-compatible adapter will use
Signature Version 4, HTTPS outside explicit loopback tests, and `If-None-Match: *`; a provider that
cannot enforce create-only writes is incompatible. Retries operate on the same remote ID and must
authenticate a preexisting object before treating it as deduplicated.

Event segments form an immutable parent chain. Checkpoints name their segment tip and zero, one, or
multiple parent checkpoints. Listing and decrypting checkpoints reconstructs the directed acyclic
graph: concurrent children of one parent are visible branches, and no timestamp or device ID wins.
Resolution creates a new checkpoint naming every resolved head plus the explicitly selected base;
it does not splice, rewrite, or delete either history. Remote downloads remain quarantined until
the full ancestor chain, event sequence, pack pins, engine revision, content identities, and
referenced authored revisions validate. Only then may an explicit user selection be imported in a
single local transaction. Wrong keys, missing ancestors, cycles, digest mismatch, corruption, and
interrupted transfers leave the valid local store byte-for-byte unchanged.

There is no polling requirement, live cursor, remote lock, central branch authority, user account,
server coordination, or real-time collaboration protocol.
