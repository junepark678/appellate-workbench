# Encrypted immutable-object sync protocol

Status: protocol version 1. The envelope/identity codec, canonical session-event-segment and
checkpoint payload codecs, pure checkpoint-graph/restore planner, create-only local-folder
provider, guarded vault keyring and routine rotation, recovery-capsule codec, and injectable
secret-store boundary are implemented. Pack-revision and authored-revision payload codecs, a
production OS-key-store adapter, S3-compatible transport, application import transaction, and
branch-selection UI remain separate slices.

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

Kind-specific payload codecs are versioned independently inside the encrypted envelope. The
implemented schema version for `session_event_segment` and `checkpoint` is 1. An authored revision
is a user-produced document revision, not a pack-supplied record asset. Its local model must assign
a stable revision ID, an optional parent revision ID, a media type, and the content digest before
it is eligible for sync. The current raw asset store is not itself such a revision catalog and
must not be swept into sync.

The following are never logical sync objects:

- a SQLite database, database backup, WAL page, journal, table dump, or row mutation;
- a plaintext record, record page, pack member, authored document, event, or checkpoint;
- a cache, projection table, temporary file, log, crash report, credential, or key;
- an executable, script, plugin, model, or provider-specific control record.

Pack archives and authored documents may contain sensitive text, but only the complete encrypted
logical object described here can leave the local device. Local SQLite and the local
content-addressed stores remain authoritative.

## Canonical session payloads

Both implemented logical payloads are strict, unsigned-big-endian records. A text field is a
`u16` byte length followed by 1 through 512 bytes in the printable ASCII range `21` through `7e`;
the decoder does not trim, case-fold, or otherwise normalize it. A UTC field is exactly
`YYYY-MM-DDTHH:MM:SSZ`. An object identity or digest is exactly 32 nonzero bytes. A binary payload
is a `u32` byte length followed by 1 through 1,048,576 uninterpreted bytes. Unsupported versions,
reserved flags, noncanonical order, unknown or trailing fields, truncation, and zero references
are rejected.

The canonical schema-1 `session_event_segment` payload is:

```text
41 57 53 47 00 01 00 00 ||
session_id:text || engine_revision:text || authority_contract:u8 ||
base_sequence:u64 || parent_present:u8 || [parent_segment_id:32] ||
batch_count:u16 ||
batch_count * (
    expected_sequence:u64 || command_id:text || recorded_at_utc:text ||
    command_payload:binary || event_count:u16 ||
    event_count * (event_type:text || authority_id:text || event_payload:binary)
)
```

Authority-contract values are exactly 1 (`legacy_v1`) and 2 (`canonical_v2`). A segment contains
1 through 1,024 complete committed command batches, each with 1 through 4,096 events, and at most
65,536 events in total. Command identifiers are unique inside one segment. The first batch's
expected sequence equals the segment base; every later batch starts after every preceding event.
A root has base sequence zero and no parent. A child has a nonzero parent and nonzero base, and the
graph validator requires that base to equal its parent's final sequence. Sequence values may not
exceed signed 64-bit maximum. The complete encoded segment is at most 64 MiB. Encoding preserves
the committed command bytes, event bytes, authority, identifier, timestamp, order, and batch
boundary exactly; it never reconstructs a batch from a projection.

The canonical schema-1 `checkpoint` payload is:

```text
41 57 43 50 00 01 00 00 ||
session_id:text || engine_revision:text || authority_contract:u8 ||
session_created_at_utc:text || head_segment_id:32 || head_sequence:u64 ||
projection_digest:32 || pin_count:u16 ||
pin_count * (pack_id:text || version:text || revision_digest:32) ||
parent_count:u16 || parent_checkpoint_id:32 * parent_count ||
selected_base_present:u8 || [selected_base_checkpoint_id:32] ||
authored_revision_count:u16 || authored_revision_id:32 * authored_revision_count
```

There are 1 through 128 revision pins, at most 128 checkpoint parents, and at most 4,096 authored
revision identities. Pins are strictly sorted and unique by pack ID. Parent and authored identity
lists are each strictly sorted and unique. A checkpoint with fewer than two parents has no
selected base; one with two or more parents names exactly one of those parents as its selected
base. The complete encoded checkpoint is at most 4 MiB. All count and byte ceilings are checked
before reserve or payload allocation.

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

A vault keyring contains a stable random 16-byte vault ID, a stable random 32-byte object-ID key,
one or more independently random 32-byte data-encryption keys indexed by random 16-byte slot IDs,
and the current write slot. The implemented owner uses libsodium guarded, best-effort locked
memory and wipes it on release. The value-shaped compatibility copy required by the envelope
codec is scoped and wiped immediately after synchronous use. Serialized plaintext keyrings and
password-derived keys also use guarded memory rather than implicitly shared Qt buffers.

The strict plaintext keyring format is big-endian and has no optional or trailing fields:

```text
41 57 4b 52 00 01 00 00 ||
vault_id:16 || object_id_key:32 || current_slot_id:16 ||
slot_count:u16 || reserved_zero:u16 ||
slot_count * (slot_id:16 || data_encryption_key:32)
```

The exact encoded size is `76 + 48 * slot_count` bytes and version 1 accepts 1 through 32 unique,
nonzero slot IDs. It rejects zero key material, an all-zero vault ID, duplicate slots, a current
slot not present in the list, unsupported flags or versions, truncation, and trailing bytes.

Keyring persistence is available only through the injected `SecretStore` boundary. With no
adapter, every load, save, and recovery restore fails closed and no alternate file, environment,
settings, or command-line path is attempted. Opaque store references are 1 through 128 ASCII
characters, begin with an alphanumeric character, continue only with alphanumerics or `._:-`, and
cannot contain adjacent separator characters. Provider access key, secret, and session token are
separate OS-secret-store entries. SQLite, settings, logs, command lines, environment variables,
and provider objects hold only nonsecret configuration and opaque key-store references.

The secret-store `write` operation is synchronous and all-or-nothing. Success replaces the whole
value; any error must leave the prior value byte-for-byte readable. An OS adapter that cannot
guarantee this postcondition is unsupported and must fail closed before mutation. Recovery still
performs authentication and strict keyring validation before making that single atomic store
call.

The production OS-store adapter remains pending. On Linux the planned adapter is QtKeychain with
insecure fallback disabled. If Secret Service, KWallet, or another supported OS store is
unavailable, sync setup and invocation fail closed; simulation, pack use, local save/resume, and
local backup continue unchanged. Tests inject an in-memory fake and never require a desktop
keyring.

Routine rotation creates a fresh random encryption key and slot for new objects. Old read slots
remain until every reachable object has been verified under a replacement. Objects are immutable,
so rotation never rewrites ciphertext at an existing remote ID. A suspected compromise requires a
new vault object-ID key, new encryption keys, and a new remote prefix. Verified plaintext objects
are re-encrypted into that namespace; the old namespace is retained until the new graph passes a
complete restore drill and is never deleted automatically.

Recovery export creates a versioned capsule containing that exact keyring encoding. Its fixed
72-byte, big-endian public header and ciphertext are:

```text
41 57 52 43 00 01 00 00 ||
kdf:u16=1 || aead:u16=1 || opslimit:u64 || memlimit:u64 ||
salt:16 || nonce:24 || ciphertext_length:u32 || ciphertext
```

KDF code 1 is Argon2id v1.3 and AEAD code 1 is XChaCha20-Poly1305-IETF. The ciphertext includes
the 16-byte authentication tag. Associated data is
`"appellate-workbench-sync-recovery-v1\0" || fixed_header`, authenticating the algorithms,
parameters, salt, nonce, and length. Export defaults to Argon2id operations limit 2 and 64 MiB;
the minimum accepted values are operations limit 1 and 8 KiB. The default import safety ceiling
is operations limit 4 and 64 MiB, before any password hashing or allocation; a caller must
explicitly raise it to open a deliberately higher-cost legacy capsule, and values must still fit
libsodium's `unsigned long long`/`size_t` inputs. Passwords are exact nonempty byte strings of at
most 1,024 bytes, and capsules are at most 4,096 bytes by default.

Restore authenticates and validates the complete capsule in guarded memory before the first
OS-key-store write. Wrong passwords, modified or truncated capsules, unsupported or excessive
parameters, duplicate slots, or invalid vault identifiers produce no key-store mutation. A
recovery capsule is user-managed sensitive ciphertext: it contains every vault key and its safety
depends on the recovery password. It is not a sync object, provider credential, loggable value,
or automatic cloud backup. Export and restore require an explicit user action and a rehearsal from
a clean local profile.

## Local-folder provider

The implemented local-folder adapter exposes the common provider operations only: bounded paged
list, stat, create-if-absent upload, and download. Remote IDs are revalidated as exactly 64
lowercase hexadecimal characters before path construction. Ciphertext is staged in an owner-only
temporary file under the final two-character prefix, flushed, and atomically hard-linked into its
final path without replacement. A complete pre-existing regular object returns `AlreadyPresent`;
the adapter never authenticates that object and the caller must download and decrypt it before
treating it as deduplicated. Interrupted writes are never published under a final object name.

Listing is lexicographically stable and uses the last returned opaque ID as its continuation
token. Symlinked/nonregular objects, malformed prefix directories, wrong-prefix names, oversized
objects, and unexpected namespace entries fail closed. Stale hidden upload files are ignored and
contain ciphertext only. The interface intentionally has no overwrite, delete, rename, plaintext
metadata, or recursive filesystem operation. The provider root is canonicalized once when opened;
callers must choose a dedicated directory protected by their local account.

The local-folder E2E test encrypts a session segment, publishes it through the provider, downloads
it into a new buffer, authenticates/decrypts it into quarantine, and compares the exact plaintext
and identity. Separate tests freeze no-overwrite behavior, pagination, owner-only permissions,
limits, malformed namespace rejection, and absence of partial final objects.

## Remote provider and branch handling

The provider interface exposes only paged list, stat, create-if-absent upload, and download. It has
no overwrite, delete, rename, SQL, or plaintext metadata operation. The implemented local-folder
adapter atomically publishes owner-only ciphertext files. The pending S3-compatible adapter must
use Signature Version 4, HTTPS outside explicit loopback tests, and `If-None-Match: *`; a provider
that cannot enforce create-only writes is incompatible. Retries operate on the same remote ID and
must authenticate a preexisting object before treating it as deduplicated.

The implemented graph builder accepts at most 4,096 identified segments and 4,096 identified
checkpoints with ancestry depth at most 4,096. Before admitting an object, it re-encodes the
logical value and recomputes its canonical `ProtocolCodec` identity for the exact kind and schema.
It requires nonzero, unique identities; exactly one segment root and checkpoint root; complete
references; acyclic, connected checkpoint ancestry; and no segment that is unreferenced by a
checkpoint. Every segment and checkpoint has the same session ID, engine revision, and authority
contract. Checkpoints also share the exact session-creation timestamp and pin set. Every segment
base and checkpoint head sequence must match the validated event chain.

One-parent checkpoints extend their parent's segment history. For a checkpoint with multiple
parents, the parents must be pairwise concurrent, and the result's segment head must extend its
explicitly selected parent. The current heads are the checkpoints that no checkpoint names as a
parent, sorted by canonical identity. No timestamp, insertion order, or device identity chooses a
winner.

The pure resolution builder requires a real conflict and an explicitly selected current head. It
returns a new checkpoint value whose parents are every current head, whose selected-base field is
that chosen head, and whose session metadata, segment head, head sequence, projection digest,
pins, and authored-revision list are copied exactly from it. It performs no I/O and does not
splice, rewrite, or delete either history. The caller must encode and authenticate the value as a
new immutable checkpoint before rebuilding the graph.

The pure restore planner likewise performs no filesystem, database, provider, or UI work. It
accepts only a current head and exact expected session metadata, pin set, and projection digest.
The caller supplies a sorted unique list of available authored-revision identities. Every authored
identity referenced anywhere in the selected checkpoint ancestry must be present; additional
available identities are tolerated. On success the plan contains the current heads, checkpoint
ancestry and segment union in deterministic parent-before-child order, the exact root-to-selected
segment path, and the sorted referenced authored identities. Duplicate command IDs on that
selected path reject the plan.

Remote downloads must remain quarantined until this validation and all content-identity checks
complete. The pending application layer may then show the branches and apply one explicit plan in
a single local transaction. Wrong keys, missing ancestors, cycles, digest mismatch, corruption,
and interrupted transfers must leave the valid local store byte-for-byte unchanged.

There is no polling requirement, live cursor, remote lock, central branch authority, user account,
server coordination, or real-time collaboration protocol.
