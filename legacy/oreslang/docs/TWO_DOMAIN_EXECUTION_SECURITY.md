# Two execution domains: GraalVM host vs LLVM native OresVM

Decision: 2026-10-12. Security audit / integration contract.

## Topology

The default *application* topology has two trust domains, **not** one VM per actor:

1. Trusted application: the GraalVM/Truffle host or Native Image runtime executes
   trusted MAIN, SHARED and PRIVATE actors with bounded OresVM scheduling.
   This is the host JVM / native runtime, **not necessarily a native-image isolate**.
2. UNTRUSTED execution: **one shared Graal polyglot Engine configured with
   SandboxPolicy.UNTRUSTED and spawnIsolate(true)**, with bounded Contexts for
   separate untrusted actor groups. All group Contexts share the isolate's
   physical GC/JIT/code-cache and aggregate isolate heap limit, while retaining
   per-Context CPU/memory quotas and per-actor logical ownership/quotas.

The existing Java implementation is being developed in
`oreslang-source.java` PR #558 (`SharedUntrustedIsolatePool`). PR #569,
which creates an Engine per actor, conflicts with this default topology and
must not merge without being rewritten to use the pool.

Graal isolates and actor scheduler domains are different concepts. There
are **four OresVM scheduler domains** but only **two intended execution trust
domains**. The number of carriers, threads, scheduler pools, actor arenas,
Context instances and `ActorRef.scheduler_domain` IDs are **not** physical
isolate counts or proof of a security boundary.

## LLVM boundary contract

`NativeOresVM` currently invokes host C callbacks inside one process, in
all four scheduler domains, including the scheduler labeled `UNTRUSTED`.
The native VM does **not** spawn Graal polyglot isolates or isolate its
callback heaps. `ores_native_vm_has_hard_security_boundary(...)` returns
false for **every** domain. A higher-level supervisor must refuse source
UNTRUSTED placement on this raw native callback backend.

Do not execute LLVM-compiled arbitrary untrusted machine code in the host
and then call it sandboxed. Supported future integrations must use one of:

- an authenticated, bounded, data-only bridge from trusted LLVM host code
  into Oreslang/Wasm interpreted by GraalWasm in the **single shared**
  untrusted Graal Engine, or
- a separately declared stronger OS-process sandbox deployment tier,
  outside the fixed default two-domain architecture.

The LLVM source-actor `main` / `spawn` slice currently runs trusted MAIN
actors only. `UNTRUSTED` source lowering, typed cross-boundary messages,
force cancellation and independent sandbox attestation are not implemented.
They must fail closed rather than execute on the in-process native
UNTRUSTED scheduler and claim isolation.

## Invariants for parity

- Every actor has **one execution lease**: at most one carrier may execute
  its initialization, mail, cleanup or resumed source turn at a time.
- No mutable host or foreign `Value` objects, FFI pointers, `ActorRef`s,
  borrowed references or actor-owned region handles cross the trust boundary.
  Use bounded, serialized copied values and a generation-checked routing ID.
- Compile/admit capabilities in the correct destination trust domain, with
  no ambient filesystem, network, native/FFI, reflection, thread creation or
  host Java access in untrusted source.
- Maintain separate admission limits at **three levels**: global Engine
  isolate memory; per-Context cumulative CPU/heap/output/threads; per-actor
  fuel, wall deadline, state bytes, mail and outstanding continuations.
  Graal `sandbox.MaxCPUTime` is cumulative Context CPU time, not an
  independent per-message wall clock watchdog.
- A shared untrusted isolate has one failure/GC/JIT domain. An isolate OOM,
  crash or hard termination can affect every untrusted actor; it does not
  offer tenant-level crash independence or per-actor hard-kill guarantees.
  For mutually distrusting clients requiring distinct native fault domains,
  use a separately priced/declared process isolation tier.
- A malicious guest must not hold a host monitor needed to revoke/close its
  Context; cancellation is initiated by trusted host code and the whole
  group/isolate may require retirement. Do not reclaim host/native memory
  until all accepted continuations and transferred leases are quiescent.
- No source actor may select a new Graal isolate by choosing PRIVATE,
  UNTRUSTED or a new ActorGroup. The pool is created once by the trusted host.

## Release gates

1. Build and install an Oreslang language-isolate artifact matching the
   pinned GraalVM version. Verify real UNTRUSTED guest source executes in it,
   with no non-isolated fallback (a skipped native-isolate test is not proof).
2. Connect production LLVM/Graal source actor spawn to a single shared
   engine with a generation-fenced message/supervision protocol. Direct
   `ActorRuntime.spawnUntrusted` in the host remains a compatibility/testing
   path and cannot be counted as an isolated placement.
3. Execute tests that reject arbitrary native `UNTRUSTED` as a security
   boundary, attempt cross-tenant Context state access, close/race/reopen
   groups, and verify actual Engine identity stays one across groups.
4. Verify bounded memory, CPU, output, thread count, revoked handle replay,
   cancellation, actor lifecycle and host survival under guest failure.
   Verify host/guest collector separation and *shared* untrusted GC identity.
5. Run both GraalVM and LLVM executable CI, including ASan/UBSan for native
   messages and lifetime handling. Keep work draft until actual test commands
   execute, rather than interpreting runner zero-step failures as success.

Reference: https://www.graalvm.org/dev/security-guide/sandboxing/
