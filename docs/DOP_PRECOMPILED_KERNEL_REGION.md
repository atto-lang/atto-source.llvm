# Precompiled AoS / SoA / AoSoA kernel dispatch (native host substrate)

**Status: tested C++ native-host component; NOT source-compiled `.atto` guest support, actor-runtime authority, a true JIT, runtime code generation, or process-wide zero-GC proof.**

`DopPrecompiledKernelRegion.hpp` owns a real `CheckedRegion` and selects one of **three compiler/linker-produced** `u32` read/write function pairs. The hot path calls fixed function pointers after checking an opaque, per-store issuer + nonwrapping generation + physical storage epoch + actual layout. It uses the production `DopLayout.hpp` geometry and `DopCheckedRegion.hpp` checked-byte storage rather than maintaining a second addressing implementation.

The store's `Ticket` is constructible only by the owner. A ticket from another store or pre-migration generation is rejected. A row handle from another store or earlier physical epoch is independently rejected by `CheckedRegion`. The selected function additionally rejects a mismatched physical layout. The wrapper is noncopyable and nonmovable, preventing dangling references to its owned storage.

**Sealed/full-AOT default** refuses *all* relayouts. A trusted, non-sealed owner can explicitly request a new `Plan`; a validated copy completes before the new kernel pointer and generation are published. Allocation/copy during migration is **not** suitable for a hard-real-time turn and is never implicitly triggered by a telemetry hint. Failed migrations preserve original rows, tickets, kernel selection and generation. There is no `eval`, ORC JIT, dynamic loading, executable memory patching, hidden interpreter fallback, implicit SIMD or GC. A JIT implementation must be a future separately governed hybrid feature, not a runtime dependency of AOT images.

## Intended source/compiler integration

1. Admit the static `define inverted class` / ".atto" field ABI and `u32`/`int` type distinctions through parser, checker, ownership/effect MIR and actual LLVM IR; unsupported syntax must fail closed before emission.
2. Lower precompiled region reads/writes with **trusted scheduler actor identity** and compiler-validated borrow/loan scopes. `Ticket` is only a host reference guard, **not** an actor capability or a guest-provided proof of permission.
3. Connect to actor-owned heaps using compressed, checked row offsets and deterministic drop; the current `CheckedRegion` uses C++ vector allocation at construction and relayout and makes no promise of native-heap quota integration, secure zeroization or per-actor process isolation.
4. Emit opt-in AOT probe sites (see LLVM draft #1846) and offline, reproducible profile-guided rebuilds; never mutate layout based solely on advisory statistics. A future ORC JIT must separately verify finite MIR identity, code-publish generation, stack/continuation pins and quiescent retirement.
5. Run identical canonical `.atto` guests under LLVM O0/O2 and Java/Graal; prove actor confinement, stale/foreign handles, borrow/critical suspension limits, OOM, rollback, allocator churn, ASan/UBSan and process closure before promoting parity or zero-GC claims.

The native regression uses 17 rows, three scalar columns, all three starting layouts, six explicit transitions, field read/write, stale and foreign tickets/rows, bounds, unsupported width, sealed mode and failed migration rollback. It registers `native_dop_precompiled_kernel_region_host_only` under CTest. Run locally with GCC/Clang at O0/O2 and ASan/UBSan; the Rust-only runner is `tools/test-dop-precompiled-kernel.rs`.

**Evidence contract:** exact Git blob inputs and compiled-executable results are native-host component evidence only. Complete LLVM compiler / real generated `.atto` guest, Java differential, hard-RT latency and true process-wide zero-GC remain NOT_EXECUTED. `steps=[]`/`steps=null` GitHub Actions do not count as test execution. No private source may be exported to a different organization without separate approved security authorization and verified exact tree.
