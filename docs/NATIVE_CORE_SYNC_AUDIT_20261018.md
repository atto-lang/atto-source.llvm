# Native Oreslang synchronization hardening — October 18, 2026

## Implemented native layers

1. **core:** `src/main/cpp/oreslang/runtime/OresCoreSync.h`
   is allocation-free, freestanding C11 mutex and RW atomic admission.
   It requires lock-free target atomics, detects stale/duplicate releases
   and generation exhaustion, and never parks an actor carrier.
2. **stdlib scheduler substrate:** `OresStdSyncAdmission.h`
   implements a bounded 64-slot FIFO logical mutex/RW gate. It returns
   typed continuation tickets and wake/failure cohorts to the scheduler
   without executing guest code in the synchronization metadata section.
   It tracks `QUEUED`, `ADMITTED`, `RUNNING` separately and does not
   release active callbacks during STOP, cancellation or PANIC.
3. **actor state:** `OresActorStateRwAdmission.h` couples exactly one
   actor-owned state RW gate to a trusted actor ID/incarnation and verified
   `critical` and `using state.read/state.write` effects. It refuses
   state-backed suspension while a continuation ticket is still live.

All three remain native runtime/compiler ABI substrates. They do not yet
provide executable source-guest `Mutex<T>`, `RwLock<T>` or compiled
`handle_state` lowering, and the actor runtime is not yet permitted to
execute max=3 guest carriers merely because these gates compile.

## ABA, cancellation and quiescence

- A ticket contains the gate address, a trusted nonzero instance generation
  and a monotonic sequence. It cannot release a reincarnated gate at the
  same address when that gate is recreated with a fresh generation.
- The compiler/scheduler—not user source—must mint and protect ticket
  authority. Raw C release functions are not public Oreslang APIs.
- STOP atomically seals admission, reports queued/dormant tickets as failures,
  and leaves physically executing callbacks attached until they end/release.
  Memory reclamation must wait for `quiescent` on the sealed gate.
- The native stdlib gate uses a nonparking metadata try-lock. The host-only
  stress fixture spins to poll; production scheduler integration must consume
  wake tickets and use bounded async queueing rather than polling.
- A continuation with a live state-admission ticket fails the
  `ready_to_suspend` check. This is not a substitute for the source borrow
  checker prohibiting escaped aliases into closures, Futures or FFI.

## Verification

Private alternate-org [GitHub Actions run #39150053787](https://github.com/sagitta-stack/sagitta-docs/actions/runs/39150053787)
is **green** for both GCC and Clang. Each job verified the source Git blob
IDs, compiled the freestanding core object, and passed Release/NDEBUG and
ASan/UBSan tests for core, native stdlib admission and actor-state RW.
The GCC job additionally passed ThreadSanitizer on all three suites.

The **exact individual source and test blobs** mirrored and checked:
- `OresCoreSync.h` `b0f355d4c1364962cef2550ef2d17237ddaf63d8`
- `native_core_sync_test.c` `20953f29edb24e6ecbd87bb19b20d2380035ccf1`
- `OresStdSyncAdmission.h` `4f4a6ead7da145238172e0d8178e4ec8a385ef6e`
- `native_std_sync_admission_test.c` `0a05ca3d53341aa63a951f41328e42b10ff6f1eb`
- `OresActorStateRwAdmission.h` `8cfa94836dd660c002cba819d65b975e1d2f67f8`
- `native_actor_state_rw_admission_test.c` `64dce7b7d23902e9aef3ad161efabd4b14b1b2e8`

This was a focused exact-blob mirror of the affected synchronization source,
NOT an exact full-tree copy, full LLVM CMake/CTest run, ARM/RISC-V embedded
build, or actual Oreslang guest-source test. Those are distinct release gates.

## Still required for production actor max>1

- Source parser/types/HIR/MIR and native LLVM codegen for sealed linear
  `Mutex<T>`/`RwLock<T>`, guard drops, `with_lock` and `handle_state`.
- Unforgeable scheduler-minted logical continuation IDs, correct loan
  retirement/reentry after await/preemption and verified transitive effects.
- Supervisor CONTROL integration, lock hierarchy/deadlock proof, panic/STOP/
  restart cancellation under multiple physical carriers, and resource
  reclamation after physical callback quiescence.
- Source-level borrow-checker negative fixtures and end-to-end Java/Graal
  and LLVM runtime parity tests, plus full-tree GitHub CI and freestanding
  target link tests.
