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
