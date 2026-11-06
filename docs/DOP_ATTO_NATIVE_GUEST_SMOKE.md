# Native AttoLang SoA checksum guest (first runnable candidate)

**Status: source fixture and an explicit AOT verification script. Execution is
NOT_VERIFIED until an actual LLVM compiler/Clang run passes on this head.**

`examples/dop-cache/soa-int-update.atto` allocates separate native
`Array<int>` position and velocity columns, initializes 32,768 rows,
runs 10 update sweeps, computes the checksum and checks first/last
positions. This is a small representative field-sweep **source-language**
program rather than a fake C++ stand-in.

The current native integer-array runtime has a strict **65,536-element per
array** limit, with a shared 262,144-element reservation budget. Do not
silently increase the guest workload to 100,000 per column before
implementing and testing a larger bounded native allocation domain.
The separately existing native actor-owned `ActorShardColumns` benchmark
supports 100,000 rows (three fields, within its own native budget) and
measures AoS/SoA/AoSoA — but is **host C++**, not a compiled `.atto` program.

Expected result:

- `sum(0..32767) + (32768 * 2 * 10) = 537509888`.
- Position at row 0 equals 20; at row 32,767 equals 32,787.
- Velocity at last row remains 2.
- Native process exits **0** only on the expected result.

## Ordered lexical locals and LLVM parity

The native frontend now admits a **bounded** ordinary function-local
declaration after an earlier effect or indexed loop via existing
`ScopedLocal` lowering, rather than falsely requiring every local to be
declared before the first statement. The 32,768-row fixture intentionally
declares `let int checksum` after its initialization/integration loops to
exercise that capability in a real LLVM → Clang executable.

The native restriction is currently **64 aggregate root locals per function**, counting both the initial declaration prefix and later ordered declarations. The same ceiling is checked regardless of where the first effect occurs.
A new workflow step builds and runs a small positive ordered-local program
and checks three compiler-negative cases: use before declaration, duplicate
binding, and the 65th root binding. Ownership, cleanup and lexical scoping
are handled by the existing root `ScopedLocal` path. This is a narrow
syntactic/semantic parity slice, **not** full Java local-owner or
actor-owned `Region<T>` parity.

These steps are specified on the branch, but **NOT_VERIFIED** until
the exact-head workflow receives a real runner and executes its steps.

## Actual AOT gate

After building `oreslang-llvmc` with the repository's real CMake/LLVM
configuration and ensuring Clang is installed:

```sh
ORESLANG_LLVMC=/absolute/path/oreslang-llvmc \
  CLANG=clang bash scripts/verify-dop-atto-aot.sh
```

The verifier uses `scripts/compile-aot.sh`, which emits LLVM IR from
the `.atto` source, links a real native binary and executes it.
A missing LLVM compiler, missing Clang, parser failure, linking failure,
resource limit, crash, wrong checksum or nonzero exit is a **failure**.
It must not be replaced by an AST parser, source-text oracle, JavaScript,
or handwritten C++ kernel and declared guest parity.

## Exact-head receipt-step contract

The source-local `.github/workflows/dop-atto-soa-native.yml` job now
exposes **three distinct actual GitHub Actions steps** with identifiers matching
the portable DOP v1 receipt validator in platform-conformance #466:

- `checkout-head`: verifies Git commit/tree and clean checkout.
- `compile-guest`: pins the exact `.atto` fixture Git blob
  `aedc778e3e8dd2940600f047540c159b0129830a`, invokes the real
  `oreslang-llvmc`/IR auditor/Clang path and hashes the produced binary.
- `execute-guest`: checks the pinned native binary hash, executes the
  generated binary under a timeout and checks the hash again, then rechecks
  the source tree. The guest checks the expected numerical checksum and
  exits zero only when the runtime result matches.

The existing complete `scripts/verify-dop-atto-aot.sh` step remains a
separate second full pipeline check. All actions are *authored but not
executed* until the private source-org runners admit a job. A receipt must
also independently match the recorded GitHub run, job ID, runner ID,
timestamps and actual step identities; writing a JSON receipt alone is
not evidence.

## Important limitations

The first guest program uses two independently allocated primitive columns;
it does **not** demonstrate compiler-selected SoA/AoSoA storage for
`InvertedArray<T>`, actor-owned guest arenas, Task sharding, GPU
dispatch, NUMA placement, cache-line reuse or source-to-source parity with
Java, Wasm, .NET or Cranelift. It is primarily a source-to-native scalar
pipeline gate.

Next: land a guest-language primitive `Region<T>`/inverted-class lowering
with proven borrow/ownership capability; bind actor-owned state to bounded
shard storage and admit disjoint Task compute; run the same `.atto`
fixture across backends, retaining backend-specific performance counters
without conflating semantics with throughput.

See also draft LLVM PR #1305 for the native actor host benchmark's
strict cache-profile comparator. The two gates are intentionally distinct.
