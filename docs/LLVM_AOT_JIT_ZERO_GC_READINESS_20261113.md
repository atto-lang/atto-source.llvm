# LLVM full-AOT, governed JIT, and zero-GC readiness audit

Snapshot: `main` `a1b31507eb66dbfde69522e0d4081d235118674e`, 2026-11-13.
The accompanying audit PR only hardens one native AOT LLVM IR screen. It does
**not** certify process-wide zero-GC, full guest AOT, a JIT, or OTLP delivery.

## Target: AOT first, instrumentation optional

The default native artifact must be executable with the compiler, interpreter,
JIT, and source files absent from the deployment host. No reachable guest
construct may silently fall back to a host AST evaluator. Publish distinct,
authenticated **execution profiles**, not one ambiguous `full-aot-with-jit`
label:

1. **sealed-guest-aot**: complete reachable guest code generated before launch;
   no runtime code generation or source parsing, and no interpreter fallback.
   Precompiled, bounded probes may be activated if the artifact explicitly
   permits them. Turning tracing on does not change the compiled guest code.
2. **governed-aot-upgrade**: the baseline remains native AOT. A supervisor can
   install a separately validated, precompiled, compatible code image at an
   admitted turn/continuation boundary. The current production UHL-1 C adapter
   intentionally advertises zero supported features.
3. **governed-jit-instrumentation**: an explicitly enabled non-hard-real-time
   capability may use a bounded LLVM ORC code-generation worker to create
   versioned trace-enabled code images. JIT failure leaves the baseline route
   intact, not a mandatory compilation dependency.

A separate **hard-real-time** execution domain must reject deep JIT, unbounded
compiler or exporter work, allocator growth, mapping, blocking locks, and
unbounded destruction on its execution path. Do not infer hard-real-time
guarantees from ASan/UBSan success or a latency average.

## Current implementation/evidence boundaries

| Area | Current evidence | Release blocker |
| --- | --- | --- |
| Native AOT | `scripts/compile-aot.sh` compiles the admitted LLVM guest subset, runs the mandatory Rust IR screen, and links selected native runtime sources. O0/O2 guest jobs are defined. | Whole-program reachable guest closure, actor/async/standard-library parity, signed build manifest and interpreter-free deployment receipt |
| No tracing GC | `scripts/audit_no_gc_ir.rs` rejects known GC IR constructs; this PR rejects multiline GC attributes as well. Actor-owned heaps and deterministic arenas exist. | This is **IR SCREENED ONLY**. A transitive linked-image/dynamic library/FFI audit and actual long-lived guest destruction evidence are missing |
| Guest memory | Actor-owned native regions, generational handles, pinned loans and reclamation tests exist in specialized subsets. | Generic source objects, async frames, cancellation, ownership transfers and all heap types need compiler-backed drop/borrow proofs, not only host-substrate tests |
| Hot upgrades | Main's UHL-1 adapter is fail-closed. [LLVM PR #1771](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1771) prototypes versioned tenant code swaps on the stacked in-process tenant VM. | Signed provenance, effect/ownership/ABI verification, persistent continuation pins, safe schema migration and independently authorized rollout. PR is draft; no production deployment proof |
| JIT tracing | [LLVM PR #1767](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1767) prototypes a versioned host-native dispatch slot with authorization verdict inputs, generation quotas and synchronous-call leases. Its reported alternate-org CI tested this host substrate. | No LLVM ORC compilation, verified manifest signatures, guest trace probes, async continuation retirement, or OTLP exporter. Keep host evidence separate from guest evidence |
| OTEL | Native logical trace and await trace helpers are present. | Propagation and actual asynchronous OTLP spans with typed semantic attributes, parent/child relationships, bounds, drops and tenant privacy policy are not proven |
| Real time | Native bounded arenas, prefault and scheduler tests exist. | No worst-case execution-time, allocator-path or page-fault proof. `shared_ptr` lifetime work and instrumentation publication must remain off hard-real-time turns |

## Additional linked-binary admission in audit PR #1780

The native Linux `scripts/compile-aot.sh` path now invokes
`scripts/audit-aot-linked-image.sh` **after Clang linking and before atomic
output publication**. It rejects known collector entrypoints and common
runtime-loader APIs, unknown ELF `DT_NEEDED` libraries, `RPATH`/`RUNPATH`
and dynamic auditing/relocation flags, missing `.symtab` evidence and
uninspectable/non-ELF output. This protects the sealed Linux AOT build
from several concrete linkage regressions, beyond the earlier IR-only screen.
The ELF gate now also refuses readelf diagnostics even when the utility
returns status zero; a malformed or empty spoofed `.symtab`, relocatable
object and ET_DYN shared library are not acceptable substitutes for a
populated executable symbol table. Regression controls intentionally include
GNU objcopy's malformed-symbol-table case.

A future governed hot-upgrade/JIT artifact needs a separate signed-mode
admission contract rather than weakening this sealed baseline.

The symbol and dependency scanner does **not** inspect all transitive libc,
allocator internals, startup objects, dynamically generated machine code,
arbitrary syscalls, TLS destructors or out-of-process services. It is not
proof of process-wide zero-GC or hard real-time performance. On non-Linux
targets this ELF check explicitly reports `NOT_RUN`; the IR gate still runs,
but an equivalent native Mach-O/PE gate needs separate implementation.

Initial synthetic controls were compiled and exercised locally with Clang
17: 2 admitted (clean explicit malloc/free, collector name present only as
string data) and 6 rejected (collector symbol variants, `dlopen`, unknown dynamic library,
runtime search path and stripped evidence). These are **not** actual
`.atto` guest tests. Exact-head source-org Actions still report failure
without executable job steps; source-side governance forbids unapproved
cross-organization private-source mirroring.

## Required invariants

- **No guest tracing collector** means no collector roots, statepoints, marking,
  sweeping or stop-the-world cycle. It does not mean no explicit native
  allocation; host `malloc/free` can still incur stalls. Label process-wide
  zero-GC only when the entire actual linked closure is audited and measured.
- **Ownership is independent of code version.** An actor-owned allocation
  keeps its actor incarnation, generation, quota and release authority while
  code images are replaced. Reject raw pointers and borrowed references across
  tenants, ownership domains, and unaudited FFI.
- **No arbitrary strong cycles.** Require explicit non-owning generational
  references, or a statically bounded region with deterministic wholesale
  reclamation. A long-lived actor must recycle released objects, not grow
  until shutdown.
- **Old executable code stays alive** for all running turns, suspended
  continuations, callbacks, native return addresses, unwind tables, and FFI
  users. An atomic entrypoint swap alone does not prove executable retirement.
- **Upgrade is a supervisor transaction**: authenticate and stage, validate
  ABI/effects/layout/state/capabilities, reserve bounded budget, quiesce,
  atomically publish, drain and retire. Reject stale epochs and unbounded
  version accumulation. Schema-changing updates require an explicit migration.
- **Tracing is bounded and optional**: preallocated per-actor or per-carrier
  buffers, constant-time probe admission, no exporter network calls from the
  guest turn, no pointers escaping actor heaps, and accounting for overflow,
  dropped spans, exporter failure and version skew. OTLP export runs outside
  the hard-real-time domain.
- **No cross-tenant native code injection.** In-process native actor domains
  are logical ownership isolation, not a security boundary for hostile code.
  Preserve separately protected untrusted execution until verified confinement
  and provenance checks exist.

## CI and release evidence that must come next

1. Link a full program and report exact source/tree, canonical `.atto` bytes,
   compiler/toolchain, target, code image and linked dependency digests.
   Prove a fresh production host runs the executable with compiler/JIT absent.
2. Negative compiler tests for unsafe borrowing, cycles without lifetime
   plans, unsupported reflection/eval and accidental fallback: nonzero exit,
   named diagnostic, **no executable IR**.
3. O0/O2 real guest execution and ASan/UBSan/TSan for ownership, actor churn,
   suspension, cancellation, panic, timed-out turns and deterministic teardown.
   Separate LLVM sanitizer builds of C/C++ host tests from real Atto guest runs.
4. Verify both GC IR screening **and** collector/link closure, instrumented
   allocation/retirement receipts, bounded RSS under long-lived actor churn,
   and tail-latency tests with traces off/on. A Rust token scanner cannot
   certify unrecognized collector APIs or dynamic native dependencies.
5. Test upgrades AOT v1 -> v2 -> v1, rejected semantic/ABI changes, stale
   version replays, continued old-version async callbacks, failed/aborted
   staging and eventual exactly-once unload. Require real generated guest code.
6. Test OTEL trace ID/span ID and context propagation across actor mail,
   `await`, send/receive and carrier migration; prove bounded ring overflow
   and exporter backpressure without blocking or allocating in hard-real-time
   turns. Separate native probe tests from real OTLP delivery.
7. Produce CI receipts with **nonzero executed steps** at the exact PR head.
   Source-org billing failures and host-only CI successes are not guest AOT,
   zero-GC, JIT or OTEL certification. Any private alternate-runner mirroring
   requires the source-side authorization and exact-tree/blob checks described
   in `AGENTS.md`.

This document is a gap audit and release checklist, not an implementation
claim. Do not merge unverified experimental tenant VM or JIT branches merely
to satisfy a status matrix.
