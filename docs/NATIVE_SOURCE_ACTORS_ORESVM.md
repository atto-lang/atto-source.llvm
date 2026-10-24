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

### Host finalization receipt before terminal CONTROL

`ores_native_vm_confirm_supervised_finalized()` records a one-shot trusted-host
completion receipt for an exact registered child after scheduler STOP and DONE.
Publication through `ores_native_vm_publish_supervised_failure()` additionally
requires that receipt; STOP or callback quiescence alone cannot authorize it.
The host must close non-scheduler resources and release ownership **before**
confirming. The VM checks receipt ordering, not the actual correctness of any
external destructor. A duplicate, stale, live, unregistered, wrong-domain or
post-retirement confirmation is rejected. Shutdown with unreported terminal
notices still requires explicit host reconciliation/escalation; this change
does not imply full source-level ActorGroup finalization.


## Bounded runtime capability slots

The hosted source-actor bootstrap now has a runtime-only capability-slot
foundation for future compiler lowering. A source actor may be created through
`oreslang_source_actor_spawn_with_capabilities` with at most eight already-live
source actor references from the same active group. The bootstrap validates the
inputs while holding trusted group metadata, then copies only each target's
`OresNativeActorRef` (scheduler domain, actor slot and generation) into the
new actor. The caller-owned pointer array and target `SourceActor*` addresses
are not retained.

During an admitted message handler, the current actor may use
`oreslang_source_actor_send_capability(heap, slot, payload)` or the nonblocking
`oreslang_source_actor_try_send_capability` form. The heap must be exactly the
current actor's private heap and the slot must be in range. The target is sent
through the copied generation-fenced scheduler reference; stale/terminated or
otherwise invalid capabilities fail closed. The `try_send` form reports zero
only for mailbox saturation.

The branch now also contains the first **compiler-lowered source slice** for
constructor-owned child capabilities. In the current native actor grammar an
immutable type-first field

```ores
val ActorRef<Consumer> child = spawn Consumer();
```

is lowered by the generated actor initializer to
`oreslang_source_actor_spawn_child_capability`. A synchronous native
`on_message(int)` handler may then call `child.send(value)`, which lowers to
the generation-fenced capability slot rather than an integer/pointer field.
Mutable ActorRef fields, recursive self-spawn, mismatched `ActorRef<T>`/spawn
types, unknown target actors and Wasm32 capability fields fail closed.

This is **not yet full Java/LLVM shared-source parity**. Java executes the richer
actor surface using `receive(ActorMail<T>)`, and common-source issue #141 tracks
the unchanged cross-backend fixture and suspension/lifecycle semantics. LLVM
does not invent actor startup arguments such as `spawn Actor(peer)`; child
ownership comes from an actor-private field initializer, matching Java's
existing nested-spawn model.

`source_actor_capability_runtime` proves both externally copied capability
identity and constructor-owned child drain. The compiler-produced
`source-actor-child-capability.ores` fixture additionally checks that generated
IR contains the child-spawn/send capability ABI and contains no `ptrtoint`.


## Canonical receive/output/end compatibility

The native actor frontend now admits the Java-compatible mailbox header
`receive(ActorMail<int> mail): void`. The compiler treats `mail.value` as the
scheduler-delivered scalar payload without materializing an ActorMail object in
guest integer state. Zero-source-field actors reserve one hidden physical heap
slot so a source actor is not forced to declare dummy state; physical capacity
also includes compiler-owned handler locals for both top-level and child actors.

Within this synchronous receive slice:

- `self.child.send(value)` lowers to the generation-fenced child capability;
- `self.send(value)` appends to a separate bounded 16-entry actor output
  stream; it never recursively sends to the actor's own mailbox;
- `self.end()` sets a handler-only end request. After the guest callback
  releases its actor heap turn, the runtime nonblockingly marks the scheduler
  lane stopping; it never waits on the carrier;
- trusted-host `oreslang_source_actor_take_output` may block for output, but
  explicitly rejects any OresVM carrier execution context;
- source metadata, including unconsumed output values and capability refs, is
  volatile-byte scrubbed before native reclamation.

The compiler preserves capability-send/output/end effects in source order and
runs output payloads through scalar typing and ownership checks.

The exact common-source v1 fixture is checked into the LLVM test tree as
`shared-actorref-child-capability-v1.ores`. It must currently fail before IR
with `native receive await child output requires continuation lowering`.
Therefore the remaining blocker for that fixture is now explicit: a receive
handler must suspend on `await child.outputs.next()`, preserve the actor's
single-writer state lease/logical turn, and resume from a runtime-owned
continuation without parking a carrier. Top-level async/output/done syntax must
then consume the same continuation machinery. No unchanged-source parity is
claimed until that gate executes.
