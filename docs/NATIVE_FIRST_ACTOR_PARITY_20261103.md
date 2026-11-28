# AttoLang LLVM native first-actor carrier parity (Nov 3, 2026)

**Status:** draft-only executable host substrate. This does not establish full
Java/GraalVM or source-language parity until an exact-source runner executes
the new tests and the compiler-generated actor fixtures.

## Purpose and compatibility

Java draft [#2493](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/2493)
requires the first actor in a pool to have a live carrier *before* ActorRef
publication, with zero actor workers at runtime boot and bounded growth under
runnable pressure. Native LLVM already has bounded per-domain pthread schedulers
but `ores_native_vm_spawn()` was intentionally metadata-only and first
message admission created its first worker. Breaking that established ABI
would break host integrations, deterministic tests and cold VM behavior.

This change introduces an explicit **opt-in** ready admission path:

- `ores_native_actor_scheduler_spawn_ready()` creates one worker before
  publishing a slot/generation ActorRef and waits for at least one real
  worker-loop check-in receipt, with a 30-second POSIX condition-variable
  deadline rather than an unbounded wait. It does **not** wait for unrelated
  additional workers still starting. First creation/entry failure consumes no
  actor identity and can retry. A late-entering worker remains scheduler-owned
  until normal retirement; it is never detached or freed by a timeout.
- `ores_native_vm_spawn_ready()` serializes VM retirement against the
  above startup and reference publication under the VM CONTROL lock.
- `ores_native_vm_spawn_supervised_ready()` installs parentage only after
  the actor is both identity-admitted and has a ready carrier.
- Compiler-generated source actors in `SourceActorBootstrap.c` use the
  checked-in ready spawn. The compiler-generated ROOT metadata actor
  deliberately remains cold until its first runnable turn.
- Existing `spawn()` and `spawn_supervised()` remain metadata-only for
  legacy hosts. `open_realtime()` retains its separate all-worker
  preallocation path, and the new APIs do not redefine hard-RT guarantees.

The ready API creates **one pthread per cold scheduling domain, not one
per actor**. Additional idle actor identities reuse that worker. When
independent runnable actor lanes require capacity, the existing
`ensure_workers_locked()` grows up to `carrier_workers`, with failed
pthread creation rejecting that additional admission atomically.
UNTRUSTED is a scheduling bulkhead only, **not** an isolation boundary.

## Real-source regression and CI plan

`tests/native_vm_lazy_first_actor_admission_test.c` links actual
`NativeOresVM.c`, `NativeActorScheduler.c`, and
`NativeActorFuture.c` with a test-only pthread creation fault injector.
It asserts:

1. All four domains have zero workers before first actor admission.
2. A failed first `pthread_create` publishes no actor and retry works.
3. 64 SHARED idle actors reuse one native worker.
4. Domain worker counts are independent and UNTRUSTED reports **no**
   hard security boundary.
5. One blocked actor and a second independent actor require a second
   carrier; failed growth rejects admission and successful retry executes.
6. A supervised-ready child fails closed if first carrier creation fails,
   does not publish parentage, and retries with the exact parent generation.
7. VM retirement drains and joins accepted work.

CTest name: `native_vm_lazy_first_actor_carrier_parity`. The source-org
GCC/Clang workflow `.github/workflows/native-lazy-actor-parity.yml` pins
the checkout SHA and root Git tree, does not persist credentials, and
compiles the production C modules with `-Werror`, ASan and UBSan.
The test is repeated ten times per compiler.

**Do not mark these tests green based on CI workflow registration alone.**
Source-org Actions has repeatedly returned no-run failures, and an
independent local/source-free simulator is not an executable proof of the
LLVM production implementation. Any private cross-org mirror requires
protected-source governance and an approved destination; this branch
does not authorize source export.

## Remaining parity gates

- JVM/GraalVM: PR #2493 lifecycle, JNI reaper and native-thread fault
  injection must pass JDK25 and GraalVM Native Image.
- LLVM: actual CTest, GCC/Clang ASan+UBSan, compiled actor `.atto`
  source lowering, root Future/CONTROL, and deterministic fail-injection
  must execute at the final reviewed source SHA.
- VM CONTROL topology: LLVM [#923](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/923)
  implements a draft one-thread prestarted service; reconciling its safe
  control-plane code into current main and validating Java's **two boot
  infrastructure workers** is separate from actor-pool first-admission
  parity. This new PR does not silently claim that target is met.
- No native-domain actor callback is a secure tenant sandbox. Process,
  WASM or verified isolate boundaries are still required for untrusted
  arbitrary native code.
- LLVM may be considered eligible for merge only after same-source
  required checks execute, independent review approves, and the full VM
  lifecycle regression suite shows no behavior changes to legacy spawn.
