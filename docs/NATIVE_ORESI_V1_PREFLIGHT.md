# Native Oreslang `.oresi` v1 interface admission

**Status: compiled C++17 metadata preflight, not a real AttoLang import linker or guest-code execution.**

`src/main/cpp/oreslang/frontend/NativeOresiV1Admission.hpp` independently decodes Java's experimental `.oresi` v1 public-interface wire format, verifies a SHA-256 lock pin, compiler version, exact target ABI, feature policy and namespace identity, and rejects unexported/orphan module symbols. The underlying package ID remains pinned and namespace-qualified.

Native SHA-256 is implemented without OpenSSL/LLVM SDK so this small boundary can be independently compiled by GCC/Clang. Checks cover bounded 16 MiB envelopes, bounded UTF-8 strings, invalid Unicode encoding, invalid/duplicate export kinds, canonical unsigned-UTF8 sort, duplicate non-function symbols, trailing bytes, missing modules, and wrong namespaces/ABI. The frozen Java-compatible `math/algebra` fixture digest is `8db5681e1e41c389e2358f9e0cd5c46e78b5b33519ed4769056268a240847f3d`.

**No archive loader, compiler parser hook, HIR import binding, object linker or native guest execution exists in this new component.** Native code from another target is not made executable by matching an interface digest. `.oresi` v1 hash includes target ABI and cannot be called cross-target-neutral. The caller must independently authenticate package release source and compiler provenance; providing an expected hash is not proof that it came from a trusted registry.

Host-only verification (NO LLVM SDK required):

```sh
cmake -S . -B build-oresi -DORESLANG_ORESI_HOST_ONLY=ON -DBUILD_TESTING=ON
cmake --build build-oresi --parallel 2
ctest --test-dir build-oresi -R native_oresi_v1_import_preflight_host_only --output-on-failure
```

Independent compiler flags should include GCC and Clang `-std=c++17 -Werror`, plus ASan/UBSan. Exact-head stepful receipts are still mandatory. See [shared #268](https://github.com/ores-truffle-oreslang/oreslang-common-source/pull/268) and [native source identity PR #1378](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1378).

Next: add a verified lock/binary format, connect `pkg:` to real native module ownership and type checking, then compile and link a two-unit `.atto` consumer returning 42 under O0/O2 with sanitizer negatives and strict full-AOT mode. Existing private CI mirror security holds remain in force.

## 2026-11-11 audit and hardening addendum

- Fixed dependency-alias admission to match Java's lowercase-only `[a-z][a-z0-9_]{0,127}` profile and the shared reserved-name exclusions (`constructor`, `prototype`, `__proto__`, `__root__`).
- Reject impossible `.oresi` declared export counts before native `vector::reserve` and add 16 MiB per-interface / 64 MiB aggregate-byte limits.
- Deny bidi/format/control/path-separator public symbols while preserving well-formed ordinary Unicode exports.
- Make `Verified` constructor private and callable only from checked `admit()`, store immutable package identity and namespace export tables, and expose const-only lookup methods. C++ compile-time assertions reject public construction and assignment.
- Clone bounded caller-supplied interface bytes before hashing and decoding so subsequent input mutation cannot change the verified view. Concurrent unsafe writes *during* the clone are still a caller-side data race and must be prevented by the compiler driver.
- Extend host tests with malformed counts, hostile names, uppercase aliases, Unicode fixture digests, SHA256 padding boundaries, and oversized metadata.

**Evidence status:** source-org Actions still reports zero executed job steps. A local GCC14/Clang17 isolated smoke compilation exercised only the extracted immutable `Verified`/friend access *pattern*, not this full header, CTest target, sanitizer tests, LLVM compiler or any `.atto` guest. This addendum makes **no** claim that the full native PR compiled or passed its tests. Independent CI and source/ABI provenance review are mandatory.

Standalone SHA256 remains an experimental host preflight implementation, not a substitute for an approved production cryptographic library review. Package trust requires independently authenticated release records, not caller-supplied matching hashes.
- Added 64 MiB-bounded immutable owned byte snapshots and made `Verified` impossible to construct or assign outside the validated admission path.
