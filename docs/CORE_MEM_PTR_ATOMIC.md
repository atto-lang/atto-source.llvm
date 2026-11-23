# LLVM-owned Atto `core/mem`, `core/ptr`, `core/atomic`

LLVM maintains these locally at `stdlib/core/*.atto` without sharing a
mandatory canonical implementation with the Java/.NET/Wasm/Forge compilers.

**Source-only candidate**: checked layout and byte-span arithmetic
(`core/mem`), region-relative arithmetic/strict-indexed bounds
(`core/ptr`), and legal memory-order/CAS-failure admission
(`core/atomic`). Invalid integer preflights return -1, invalid predicates
false; memory orders encode relaxed=0, acquire=1, release=2, acq_rel=3,
seq_cst=4. The portable default is seq_cst.

This does NOT construct or dereference `Pointer<T>`, grant actor/region
authority, perform load/store or drop memory, or implement real atomic
operations. Real LLVM guest atomic AOT remains in the independent
[atto-source.llvm #801](https://github.com/atto-lang/atto-source.llvm/pull/801);
do not confuse this policy source with its compiler/runtime implementation.

`node --test tests/core-memory-policy-source.test.mjs` checks the exact
restricted Atto arithmetic subset, including 63 boundary vectors. This is
not real LLVM import linking or compiled guest execution. LLVM's current
native import/path activation requires separate resolution and compiler tests.
Keep this PR draft until the actual compiler accepts and executes guest
fixtures with explicit negative/fail-closed cases and exact-head CI.

Coordinate [Java #1771](https://github.com/atto-lang/atto-source.java/pull/1771),
[.NET #208](https://github.com/atto-lang/atto-source.net/pull/208),
[Wasm #269](https://github.com/atto-lang/atto-source.wasm/pull/269),
[Forge #221](https://github.com/atto-lang/atto-source.forge/pull/221),
[Rust #419](https://github.com/atto-lang/atto-source.rs/pull/419),
[Atomic spec #175](https://github.com/atto-lang/atto-spec/pull/175),
and conflicting [pointer specs #18](https://github.com/atto-lang/atto-spec/pull/18)
and [#154](https://github.com/atto-lang/atto-spec/pull/154).
