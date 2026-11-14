# LLVM actor-owned memory: native substrate and real guest conformance v1

**Status:** draft acceptance tests, not release certification. Keep the production
C actor arena, compiled AttoLang actor semantics, linked no-GC image closure,
and process-wide zero-GC claims distinct.

## Evidence boundaries

| Gate | What it uses | What passing would establish | Does not establish |
| --- | --- | --- | --- |
| `scripts/test-actor-arena-lifecycle-v1.sh` | Real `src/main/cpp/oreslang/runtime/ActorArena.c`, Clang O0/O2, ASan+UBSan | 8,193 authenticated actor turns, two separate native arenas, persistent slot survival, per-turn scratch zeroing, allocation/drop reuse, foreign and stale handle SIGABRT, loan-delayed destruction, surviving sibling, direct native allocator calls intercepted during actor turns, and 10 successful startup calloc calls paired with 10 physical frees | Atto source lowering, guest allocator calls, zero GC across FFI, scheduler or full process |
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
