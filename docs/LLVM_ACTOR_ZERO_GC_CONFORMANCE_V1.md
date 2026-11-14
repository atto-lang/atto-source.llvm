# LLVM actor-owned memory: native substrate and real guest conformance v1

**Status:** draft acceptance tests, not release certification. Keep the production
C actor arena, compiled AttoLang actor semantics, linked no-GC image closure,
and process-wide zero-GC claims distinct.

## Evidence boundaries

| Gate | What it uses | What passing would establish | Does not establish |
| --- | --- | --- | --- |
| `scripts/test-actor-arena-lifecycle-v1.sh` | Real `src/main/cpp/oreslang/runtime/ActorArena.c`, Clang O0/O2, ASan+UBSan | 8,196 successful parent-process turns (8,193 churn/post-retirement turns plus two startup and one loan turn), two separate native arenas, persistent slot survival, per-turn scratch zeroing, allocation/drop reuse, foreign and stale handle SIGABRT, loan-delayed destruction, surviving sibling, direct native allocator calls intercepted during actor turns, and 10 successful startup calloc calls paired with 10 physical frees | Atto source lowering, guest allocator calls, zero GC across FFI, scheduler or full process |
| `scripts/test-actor-arena-guest-churn-v1.sh` | Actual `oreslang-llvmc --target=host` lowering of `.atto`, Rust IR GC screen, `compile-aot.sh`, real O0/O2 ELF execution | 128 actor incarnations per native program (four runs at O0 and O2), sealed state reset, one response per incarnation, joining all actors; source actor ABI call sites actually present in generated IR (constant-output success strings are insufficient); direct sealed-state access rejected with **no IR** | Independent actual allocation ledger, lack of leaks, suspension retention, process-wide zero-GC |
| `scripts/audit_no_gc_ir.rs`, `scripts/audit-aot-linked-image.sh` from security PR #1780 | IR and linked ELF screens | Known LLVM tracing-GC constructs and known collector/loader links screened | Transitive allocator tracing, dynamically loaded code, out-of-process services, all shared dependencies |

The new `.atto` program uses the same actor declarations,
`handle_state(critical-write ...)`, `spawn`, `ActorMail`,
`get_ready()`, `outputs.next()` and `get_done()` vocabulary already
present in `tests/fixtures/actor-state-e2e-v1/positive.atto`.
It is a **compiler acceptance candidate**; do not claim the frontend
currently supports the complete source shape without an actual executed run.
Neither C/C++ host emulation nor a generated shell marker may substitute for
a native Atto executable. Native allocator interception covers direct
`malloc`/`calloc`/`realloc`/`free` linkage from the compiled C test and
production ActorArena objects; it is not an entire process allocator trace.
The compiler-backed fixture additionally insists on generated calls to
the actor ABI to reject a constant-output-only executable.

## Compiler-selected source actor heap hardening (2026-11-14)

The source actor compiler runtime selects
`src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c`, **not**
`ActorArena.c`. The two allocators have distinct ABIs; native ActorArena
success alone never establishes source actor heap correctness.

This PR also changes the production compiler-selected scalar heap:

- Bind `ores_actor_i64_heap_enter` to a physical carrier via C11
  `_Thread_local` authority. Matching a copied domain/epoch on another thread
  is no longer sufficient. One carrier cannot enter a second actor heap
  while a first turn is still active.
- `leave` revokes thread-local admission. A fresh `enter` on another
  thread after the first turn ended still supports actor migration.
- Cross-turn read loans remain releaseable by a trusted host without
  re-entering a turn; retiring a heap still waits for loans to drain.
- Physical teardown uses the existing volatile `zeroize_bytes` instead of
  an O2-deletable dead `memset` immediately before `free`.

The separate `scripts/test-source-actor-heap-carrier-v1.sh` compiles
production `ActorOwnedI64Heap.c` and the original
`tests/actor_owned_heap_lifecycle_test.cpp` at O0/O2 with Clang
ASan/UBSan. It adds fail-stop negative cases for a stolen active
turn token on another native carrier and a nested admission to another
actor heap. A linker-level `calloc/free` wrapper tracks the actual
scalar heap storage and requires every tracked byte to be zero at the
`free` boundary. The wrapper is limited to direct linkage from the
compiled runtime object, not other process allocations.

The original scalar-heap lifecycle regression now also runs **100,000
allocate/read/write/drop iterations on one long-lived native heap** while a
fixed-layout mutable field remains alive through 1,000 actor turns. Every
reused transient must have a monotonically increasing slot generation. At
shutdown, retiring the heap during its last active turn cannot reclaim memory;
even after that turn leaves, a shared loan held across turn boundaries must
block destruction until a different trusted carrier releases the loan. The
test checks two allocations are scrubbed before physical free (the existing
lifecycle heap and the new long-lived churn heap).

### Source actor churn compiler limitation

The `.atto` `churn-128.atto` fixture remains a future **acceptance
target**, not a supported guest case. Current LLVM codegen exposes its
scalar `while` feature only when `source.actors.empty()`. Source actor
references are lowered from statically enumerated `actor_spawns` into
`actorRefs`, while `SourceActorBootstrap.c` allocates source actor
slots sequentially up to `SOURCE_GROUP_ROOT_SLOT=63` per live group.
Dynamic actor spawn inside a source `while` therefore requires compiler
IR/HIR lowering and runtime lifecycle proof before the 128-actor test can
legitimately pass. Do not replace the fixture with a constant stdout marker,
synthetic C actor construction, or silently skipped CI step to claim guest
parity.

These are **new test requirements, not executed test results**. Source
GitHub Actions still reports failure with `steps=null`, and no
complete local private-source checkout has been tested. This is not
proof that the guest compiler lowers all types into an owned heap,
that 128 actor incarnations actually run, or that full process no-GC
has been achieved.

## Release criteria still unproven

1. **Ownership lowering:** production compiler emits correct actor-owned
   allocation/drop and closure capture operations with 32-bit checked
   actor-relative addresses (not implicit host heap or tracing GC).
2. **Per-turn allocation proof:** instrument all allocator entrypoints,
   real-time system calls and host library callbacks. Verify no unexpected
   allocation during realtime turns. Startup/shutdown allocation is explicitly
   budgeted, not hidden.
3. **Suspension and cancellation:** pin actor generations and native AOT code
   while async tasks, FFI callbacks, timers or borrows remain reachable. Check
   quiescence and bounded drop latency.
4. **Churn and quotas:** verify bounded memory over repeated turns in a *single
   long-running native guest process*. Eight process launches alone do not
   prove lack of accumulation.
5. **External closure:** enumerate and attest static and dynamic dependencies,
   startup and transitive allocator behavior. Zero guest tracing GC is not
   identical to process-wide no-GC or hard real-time certification.
6. **Executed independent CI:** source-org workflows are currently blocked
   before runner execution. Any empty or null step log is NOT_EXECUTED.
   `docs/security/CI_PRIVATE_SOURCE_MIRRORING.md` forbids ad-hoc private
   source export to alternate organizations; obey source-side approvals,
   branch protection and reviewer/incident gates.

## Run locally (only with an authorized, complete source checkout)

```sh
EXPECTED_HEAD="$(git rev-parse HEAD)" CLANG=clang \
  bash scripts/test-actor-arena-lifecycle-v1.sh

# Separately build source-owned oreslang-llvmc and Rust no-GC IR auditor:
EXPECTED_HEAD="$(git rev-parse HEAD)" \
ORESLANG_LLVMC=/path/to/oreslang-llvmc \
ORES_NO_GC_IR_AUDIT=/path/to/oreslang-no-gc-ir-audit \
CLANG=clang \
  bash scripts/test-actor-arena-guest-churn-v1.sh
```

No merged-release status should be set until both test jobs execute and the
broader lifetime, allocator and transitive-binary proof gates are satisfied.
