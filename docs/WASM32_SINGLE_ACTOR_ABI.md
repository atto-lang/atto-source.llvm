# Experimental source actor Wasm32 ABI v1

This is a narrowly scoped first step toward a compiler-generated, isolated
browser actor. A module instance owns **one** synchronous source actor.
The host creates exactly one Wasm instance per actor and serializes invocations
on a dedicated Worker. This restriction is essential: a Wasm global is not
isolated between actors sharing the same module instance.

For `define actor Counter ...`, the compiler now emits the following exports:

- `oreslang_actor_Counter_init() -> void`: host-owned initialization, called
  once before the first message (or on a newly instantiated actor incarnation).
- `oreslang_actor_Counter_on_message(i64) -> i64`: source-compiled handler
  executing a single non-suspending turn, retaining private scalar field state.

The Wasm target uses the normal `--target=wasm32` flag. The linker must
explicitly export both symbols. Neither method imports a native heap, source
scheduler, supervisor, or host capability.

**Not yet supported:** compiling source `spawn`, `send` and `stop` to
browser operations; source actor suspension/await; multiple actors sharing an
instance; typed MessagePack envelopes other than i64; ActorGroups;
supervision and lifecycle handling inside the guest. The Worker host must
validate the manifest, never execute two turns at once, and terminate/recreate
the instance after a panic. Source-level cancellation cannot be assumed
to preempt a CPU-bound guest handler.

Security warning: `init()` and `on_message()` are an internal ABI. Calling
these exports from arbitrary concurrent host code without the host control
protocol voids actor isolation. This target is experimental, not general
Java/Graal actor parity.
