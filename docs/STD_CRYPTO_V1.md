# std.crypto — portable encryption and cryptographic services, v1

## Status

This repository owns its own `stdlib/std/crypto.atto` source. The `.atto` functions in this first slice are **pure admission policy**,
not a complete guest-callable encryption implementation.
A module path of `"std.crypto"` is the intended core import (never
`pkg "std.crypto"`), but **the import resolver and host crypto bindings still
need implementation and an executed compiler-backed test**. Do not promote
the fixture or this PR as proof of functioning encryption.

## LLVM / OpenSSL trusted-host prototype

This branch adds C++17 files
`src/main/cpp/oreslang/runtime/CryptoAeadHostV1.{h,cpp}`.
They call OpenSSL EVP `EVP_aes_256_gcm` and
`EVP_chacha20_poly1305` (no handwritten cipher), generate 256-bit
keys with `RAND_bytes`, use a random 4-byte nonce prefix and a
synchronized 64-bit counter capped at 2^32 encryptions per key,
and scrub key arrays on explicit close/destruction. Keys cannot be
copied or moved. `open` returns authenticated plaintext only
after EVP final tag verification; failed outputs are blanked.
Only a trusted host caller may acquire these handles; this does
not create Atto guest cryptographic authority.

Opt-in **host-only** test runner:

```sh
sh tests/crypto-aead-host-v1.sh
```

This builds a small OpenSSL-linked C++17 test independently of the
full LLVM SDK. It includes the NIST AES-256-GCM empty-message
known-answer vector, nonce uniqueness, tamper, wrong key/AAD/nonce,
zero-length messages, and key retirement. **An isolated host test executed and PASSED** with the exact
committed files under C++17 with OpenSSL 3.5.5:
`CryptoAeadHostV1.h` blob
`9f766489f3f01d3a5ff921ff9b6cb7aa5c5bc508`,
`CryptoAeadHostV1.cpp` blob
`2c48d62ad8d23e029799f315861d7728ab2daafd`,
and `crypto_aead_host_v1_test.cpp` blob
`716d80248c2f31d55811def90104d1d935335df7`.
The hardened implementation now scrubs prior plaintext in the caller-provided
`open()` output buffer before any new authentication outcome; a
`std::vector::clear()` alone does not erase memory. It was rebuilt and
executed with C++17 and OpenSSL 3.5.5; the exact new blob SHA above
passed. The isolated output was `ATTO_CRYPTO_AEAD_OPENSSL_HOST_ONLY_PASS`.
It was not the GitHub Actions runner or the complete LLVM compiler.
These code additions are not
a working `std.crypto` import, actor capability, or hard-real-time
compiler proof. Fail closed until these are independently tested.

## Portable Atto source envelope helpers

The additional `stdlib/std/crypto_envelope.atto` file is implemented
in this compiler's own Atto source tree. It defines pure, overflow-safe
admission and offsets for **nonce[12] | ciphertext[n] | full_tag[16]**;
minimum valid envelope length 28 bytes and maximum 16 MiB + 28.
A negative sentinel indicates malformed lengths and MUST be checked
before indexing. The algorithm and key identity are **not** embedded
in this v1 envelope and must be authenticated by the higher-level
protocol; do not treat the payload as self-describing or derive a key
from unauthenticated metadata.

`tests/fixtures/crypto_envelope_v1.atto` covers min/max, truncated
tags, AAD budget, ciphertext length and tag offset. **Neither the
envelope fixture nor the existing `crypto_policy_v1.atto` has been
executed by a full Atto stdlib source linker yet**. Host-only crypto
tests do not satisfy that missing guest conformance proof.

## Namespace-first Atto standard library layout

The source declaration in this repository is **`use namespace crypto;`**,
not `define module crypto` and not a dotted namespace declaration.
The Atto parser currently accepts a flat namespace header; public library
identity and import paths are resolved separately:

```atto
// stdlib/std/crypto.atto or stdlib/std/crypto_envelope.atto
use namespace crypto;

// in a consumer source file
import * as crypto from "std.crypto";
```

Two separately maintained `.atto` files can contribute distinct pure
functions to the `crypto` source namespace; namespace metadata alone does
**not** prove a multi-file linker correctly merges them. The Java compiler
currently treats the exact `std.crypto` import as an intrinsic namespace
for its supported crypto host methods, not yet as a verified source export
graph for `crypto.atto` and `crypto_envelope.atto`. LLVM, .NET and Wasm
still require their own namespace import-resolution and guest source tests.

For the **strict Atto source profile**, comparisons in these files and
fixtures use `eq` / `neq`; legacy `==` / `!=` must not be reintroduced.
Every compiler can own different `.atto` implementation files while
maintaining the agreed public AEAD envelope and error contract. No shared
canonical source dependency is required.

## Public API to implement

- `hash.sha256(bytes) -> Bytes` and `mac.hmac_sha256(secret, bytes) -> Bytes`.
- `kdf.hkdf_sha256(ikm, salt, info, output_bytes) -> SecretBytes`.
- `kdf.pbkdf2_hmac_sha256(password_bytes, salt, iterations, output_bytes)`
  for bounded protocol compatibility only; Argon2id is a later audited
  password-storage service, not a substitute for HKDF.
- `random.secure_bytes(n) -> SecretBytes` and `random.key_256() -> SecretKey`.
- `aead.aes_256_gcm.seal(key, aad, plaintext) -> Sealed{nonce,ciphertext}` and
  `aead.aes_256_gcm.open(key, nonce, aad, ciphertext) -> Result<Bytes, CryptoError>`.
  Sealing reserves a nonce through the opaque key handle; callers do not
  choose a nonce for managed encryption.
- `aead.chacha20_poly1305.seal/open(...)` with equivalent authenticated
  semantics where vetted support exists.
- `constant_time.equals(left, right) -> bool` implemented by backend code,
  not a guest-language early-return byte loop.

These are **target API signatures**, not runnable exports in this first PR.
The current usable pure functions are the named predicates in `crypto.atto`.

## Wire and security contract

Algorithm ids are 1 = AES-256-GCM and 2 = IETF ChaCha20-Poly1305.
Both require 32-byte keys, **unique 12-byte nonces for each key**, and
**full 16-byte authentication tags**. AAD is authenticated but not encrypted.
Ciphertext size in this proposal is plaintext length + 16 tag bytes
(without any nonce or envelope prefix); any future envelope uses an explicit
version, algorithm, nonce, and lengths, without silently altering this shape.
Do not expose an unauthenticated cipher as an encryption convenience API.

Use an internal nonce manager with per-key counters and crash-safe
reservation, or another reviewed nonce-generation strategy. A random nonce
by itself is not an unlimited uniqueness guarantee. Never auto-generate a
nonce with an untrusted/noncryptographic RNG or let untrusted code seed the
CSPRNG. Keys are opaque, move-only or sealed handles with no accidental
serialization/logging and explicit zeroization on retirement where possible.
No secret-dependent branching or timing guarantees are made by policy
predicates; trusted backends must supply those guarantees.

Open authenticates **before** returning any plaintext. Tampering, unknown
key, bad tag and wrong AAD produce the same externally observable
authentication failure. All error paths drop buffers and wipe secret
temporary memory where possible. Enforce permission/effect and memory
quotas again in the host; validation functions are advisory and cannot
authorize a resource operation.

Per-call budgets: 16 MiB message, 64 KiB AAD, 1 MiB HMAC key/IKM,
HKDF-SHA-256 output ≤ 8,160 bytes (RFC 5869), bounded PBKDF2 work
≤ 100,000 rounds **for protocol compatibility**, entropy requests ≤ 4,096
bytes. Higher-cost password storage must use a separate async/memory-hard
and explicitly budgeted API.

## Implementation ownership and rollout

1. Keep this compiler's `.atto` code local. Separate compilers may use
   different algorithms for policy, memory layout and host library bindings
   while preserving public protocol behavior and test vectors.
2. Bind import `"std.crypto"` and guest types to the compiler; map
   trusted primitives to platform vetted implementations. Java can reuse
   its existing SHA/HMAC/PBKDF2 draft PR #1367 (where applicable);
   other backends need independently authenticated implementations.
3. Add AES-256-GCM and ChaCha20-Poly1305 behind capabilities and
   ownership-safe key/nonce types. Do not implement AES/GCM/Poly1305
   manually in a new guest loop as a stopgap for vetted primitives.
4. Execute `tests/fixtures/crypto_policy_v1.atto` with this compiler
   after importer integration, then run NIST/RFC known-answer vectors,
   tamper/wrong-key/wrong-AAD tests, random quality integration checks,
   arena scrub/lifetime tests, cancellation and zero-GC/AOT admission.
   Record the exact head SHA and steps; source-only checks and
   zero-step Actions are NOT_EXECUTED for guest conformance.

References: RFC 8439 (ChaCha20-Poly1305), RFC 5869 (HKDF),
NIST SP 800-38D (AES-GCM). NIST's current 38D revision work does
not justify weakening nonce-uniqueness or tag requirements.
