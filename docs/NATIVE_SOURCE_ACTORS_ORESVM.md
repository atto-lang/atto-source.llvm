# LLVM generated source actors routed through OresVM

The restricted LLVM `define actor` / `spawn` / `send` / `stop`
bootstrap now opens the real four-domain native OresVM, not an unrelated
private `OresNativeActorScheduler`.

* A trusted zero-argument `main` allocates one OresVM per live compiled
  source-actor group. Every domain initializes metadata with **zero threads**;
  MAIN receives 64 bounded actor slots, mailbox budget 128 and up to four
  lazy carriers. The other three domains remain dormant, with one metadata
  slot and no spawned guest actor.
* Compiled actors spawn through `ores_native_vm_spawn(MAIN)`, receive the
  compiler-generated constructor as priority CONTROL via
  `ores_native_vm_publish_control(MAIN)`, and deliver messages through
  generation-checked `ores_native_vm_try_send(MAIN)`.
* Source `stop` uses OresVM's trusted host stop and per-actor done barrier
  before reclaiming the actor's owned i64 heap exactly once. The last actor
  closes the four-domain VM after all accepted work drains.
* `tests/source_actor_e2e.py` compiles real `.ores` into LLVM IR, links
  the actual native VM + scheduler and native-owned heap, and executes
  through Clang in normal and ASan/UBSan configurations. The test verifies
  cold zero-carrier VM construction, constructor on a carrier, message
  results, and physical heap reclamation.

This is a **trusted MAIN-domain vertical slice**, not general actor
placement or typed ActorGroup source capability lowering. Supervisor
relationships, guest `await`, continuation parking, cross-domain sends,
and sandbox isolation still fail closed or require separate implementation.
No actor is given its own OS scheduler.
