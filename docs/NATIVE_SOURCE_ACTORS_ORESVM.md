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

The host OresVM facade also provides opt-in **typed supervisor parent
registration** for same-domain actors. A supervisor is an immutable actor ref,
not a new thread; the registered parent handles a typed terminal failure on
its own exclusive CONTROL lane after the child has stopped and drained.
Duplicate publication, stale refs, cross-domain parents and premature failure
notices fail closed. A supervisor cannot stop while a registered child remains
live or has not yet had its terminal event admitted; this prevents ordinary
STOP from orphaning a child. Domains without typed failure callbacks reject
supervised spawn without consuming a child slot. A saturated parent CONTROL slot rejects publication so
the trusted runtime must retain and retry/escalate the notice. Host ownership
of resource finalization remains explicit; a drained turn alone does not
reclaim non-scheduler actor resources.

This is a **trusted MAIN-domain vertical slice**, not general actor
placement or typed ActorGroup source capability lowering. Supervisor
relationships, guest `await`, continuation parking, cross-domain sends,
and sandbox isolation still fail closed or require separate implementation.
No actor is given its own OS scheduler.
