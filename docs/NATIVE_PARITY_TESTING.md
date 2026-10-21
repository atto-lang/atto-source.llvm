# Native parity test expansion — 2026-10-21

This suite tests the common host memory and actor-runtime contracts. It does
not certify full guest AOT, zero GC for arbitrary programs, Forge guest actor
lowering, or Java/LLVM language completeness.

## Executed scope

| Check | Result |
| --- | --- |
| Independent byte-arena and scalar-heap models | 32 seeds; 57,978 observed transitions per API path |
| Optimized cross-backend differential comparison | Four paths: LLVM byte/scalar and Forge byte/scalar; identical observations |
| Shared negative lifetime/authority contract | 28 distinct misuse cases per API/backend; all 112 optimized child processes terminated with SIGABRT |
| Actor FIFO and isolated state | 32 seeds; 23,616 messages per runtime, identical OresVM/Forge observations |
| Ordinary-function vs actor-handler ownership | LLVM: 9 valid + 13 invalid expression pairs; expected admission/diagnostic on both entrypoints |
| ASan/UBSan | Both native runtime paths pass; LLVM source expression-pair checks also pass |

Each memory seed alternates two heaps with identical numeric owner/incarnation.
The model tracks live slots, expected values, loan modes, generation changes,
and retirement pins independently of the runtime. It exercises allocation,
read/write/drop, shared/exclusive loans, loan I/O/release, turn handoff and
physical-carrier migration. Every seed must cover every operation category.
The final retirement check requires live turns/loans to prevent free and
quiescent storage to be reclaimed.

Misuse comparisons require SIGABRT specifically. A parser error, arbitrary
nonzero status, missing runner, timeout, or empty receipt is not a pass.
Cases include stale/foreign slots and turns, stolen-carrier access, nested
actor turns, owner access through an exclusive loan, conflicting loans,
replayed/forged loans, and access after retirement.

The actor corpus invokes trusted native callbacks through the real
SourceActorBootstrap/OresVM scheduler or Forge ScalarActor. It interleaves
streams across eight actors, varies per-actor message counts (including empty
streams), checks an order-sensitive bounded checksum, shuffles stop order,
and repeats construction/draining across three groups per seed. It compares
accepted-message counts and final per-actor state; it does not equate differing
overload APIs or startup-cancellation policies.

LLVM's source-level expression pairs compare ordinary function parameters and
actor message parameters using the actual parser and ownership checker.
They cover Copy/Share/Take, moved-value reuse, named argument order, temporary
borrow rejection, shared/exclusive aliasing and nested call-loan lifetimes.
These tests do not execute newly generated guest machine code.

## Reproduce

Run from either repository root:

```bash
bash scripts/verify_native_actor_memory.sh optimized
ASAN_OPTIONS=detect_leaks=0 bash scripts/verify_native_actor_memory.sh sanitized
```

The source-local native-memory workflow invokes the same scripts. It compiles
the new runners and executes the model driver alongside the earlier lifecycle
and arena tests. Linux/GCC optimized builds retain the existing allocator
guards; the randomized test harness itself uses ordinary test-side allocations.

The common C++ model/actor corpus and Python driver are byte-identical in the
two repositories. To compare separately built runners directly:

```bash
python3 scripts/test_native_memory_parity.py \
  --runner /path/to/llvm-memory-runner --runner /path/to/forge-memory-runner \
  --actor-runner /path/to/llvm-actor-runner --actor-runner /path/to/forge-actor-runner
```

All checks use explicit failure paths under NDEBUG; the Python driver does not
rely on removable assertions. Local sanitizer runs disable LeakSanitizer
because this environment blocks its /proc thread inspection. Full LLVM
compiler linking, generated .ores execution, CMake integration and hosted
runner execution remain separate checks.
