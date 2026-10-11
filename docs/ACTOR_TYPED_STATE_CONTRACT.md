# Actor persistent state declaration contract (proposal, not yet parsed)

Actor state belongs to an actor *instance*. Its schema belongs to the actor type.
Guest code cannot read `self.state`, override `handle_state`, or receive a raw
native pointer. All access is through the sealed `critical handle_state` method, invoked as
`await self.handle_state(|access| { ... })` when the callback suspends.

## Finite typed keys (preferred)

```oreslang
actor Counter {
    state {
        count: int = 0;
        enabled: bool = true;
    }
    pub fnc increment(): int {
        return self.handle_state(|s| {
            s.count += 1;
            return s.count;
        });
    }
}
```

The declaration is a schema, not a guest-visible field. Field names are unique;
field types must be storable and owned. Defaults are evaluated separately for
each actor instance at initialization, before the ready barrier completes.
Required fields cannot be deleted. Unknown keys and mismatched value types are
compile errors. Dynamic string lookup is not permitted on a finite schema unless
statically proven to select a declared key. No mutable alias or state-backed
pointer may escape the callback.

## Dynamic map (explicit opt-in)

```oreslang
actor Cache {
    state: Map[string, int];
    pub fnc put(key: string, value: int) {
        self.handle_state(|s| { s.set(key, value); });
    }
}
```

The map has homogeneous, statically known key/value types. It remains hidden
behind the same callback capability. An actor without a state declaration has
no implicit `Map[any, any]`; the compiler must diagnose use of state access
until a declaration is provided. This avoids implicit unbounded native storage.

## Required compiler/runtime gates

1. Reserve `state` actor declarations and `critical handle_state` as a
   non-overridable actor intrinsic. Reject direct `self.state` and user-defined overrides.
2. Type-check the callback against a schema-specific, ephemeral capability.
3. Permit await/select within the callback using scheduler-owned async critical
   admission and continuation fencing. Reentrant calls must follow explicit
   critical ownership semantics (not blanket rejection); never hold a physical
   mutex or JVM monitor across suspension. Reject capture/return/store of the
   state capability and borrowed aliases escaping their valid scope.
4. Lower finite fields to a stable typed layout with explicit alignment,
   initialization and finalization; lower dynamic maps to bounded storage.
5. ActorGroup owns allocation, ready initialization, cancellation, panic,
   finalization and retirement. Never tie ownership to carrier-thread identity.
6. Run identical positive/negative source fixtures through LLVM and GraalVM.

The native `ActorStateScope.h` guard is a partial host ABI only. This document
must not be taken as evidence that source-level state syntax compiles yet.
