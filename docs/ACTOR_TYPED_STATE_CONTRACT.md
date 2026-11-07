# Actor persistent state declaration contract (proposal, not yet parsed)

Actor state belongs to an actor *instance*. Its schema belongs to the actor type.
Guest code cannot read `self.state`, override `handle_state`, or receive a raw
native pointer. All access is through the sealed `critical handle_state` method, invoked as
`await self.handle_state(critical |access| { ... })` when the callback suspends.

## Finite typed keys (preferred)

```oreslang
actor Counter {
    state {
        count: int = 0;
        enabled: bool = true;
    }
    pub fnc increment(): int {
        return self.handle_state(critical |s| {
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
        self.handle_state(critical |s| { s.set(key, value); });
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
3. Permit await/select within the callback using scheduler-owned logical single-flight admission, compiler-approved
   reentrancy boundaries and continuation fencing. A nested same-continuation
   state callback temporarily fences the outer state loan, but a distinct
   continuation must wait for its own lane turn. On await, revoke/segment
   state-backed loans before releasing the actor lane; do not hold a physical
   mutex or JVM monitor across suspension. Reject capture/return/store of the
   state capability and borrowed aliases escaping their valid scope.
4. Lower finite fields to a stable typed layout with explicit alignment,
   initialization and finalization; lower dynamic maps to bounded storage.
5. ActorGroup owns allocation, ready initialization, cancellation, panic,
   finalization and retirement. Never tie ownership to carrier-thread identity.
6. Run identical positive/negative source fixtures through LLVM and GraalVM.

The native `ActorStateScope.h` guard is a partial host ABI only. This document
must not be taken as evidence that source-level state syntax compiles yet.

For effect inference, concrete proposed `using`/`requires` signature syntax,
and current `pure`/`nlex`/`trap` limitations, see
[function-effect contract](FUNCTION_EFFECTS_AND_SIGNATURE_QUALIFIERS.md).
For bounded preemption and strict real-time admission, see
[reduction-region scheduling](REDUCTION_REGION_SCHEDULING.md).

## Compiler proof-of-concept status (October 11, 2026)

**Target user experience (source lowering remains unfinished):**

```oreslang
define actor Counter from Countable as
    state {
        count: int = 0;
        enabled: bool = true;
    }

    pub get_count(): int using state.read {
        return self.handle_state(critical |s| -> s.count);
    }

    pub increment(): int using state.read, state.write {
        return self.handle_state(critical |s| -> {
            s.count = s.count + 1;
            return s.count;
        });
    }

    critical pub audit(): void using state.read {
        self.handle_state(critical |s| -> { stdio.println(s.count); });
        return;
    }
end
```

Here `state` is the **one and only actor-owned persistent storage
declaration**. The `handle_state` callback is the sole access path; guest
code may not access `self.state`, return the state capability, capture it
into another deferred closure, or retain a conflicting mutable loan across
a reentrancy boundary. Nested **same-continuation** critical methods inherit
the logical admission ticket, but nested callbacks have separate ephemeral
state-access loans.

Map alternatives:

```oreslang
state: Map<string, int>;

state: Map<string, int> {
    hits: int = 0;
    errors: int = 0;
}
```

For a map with declared keys, those keys have exact known types; dynamic
keys use the map's declared key/value types. A heterogeneous typed-key
schema may later constrain each declared key to a specific subtype of a
dynamic value union. Runtime dynamic `get`/`set` and mixed-type schema
lowering are NOT yet implemented.

`handle_state` **permits** `await`, `select`, other suspension points,
and explicitly blocking calls. Prefer I/O outside it when possible, but
do not ban it at the language level. `*_blocking` call effects must come
from trusted function signatures, not from guessing based on method names.
In an actor, blocking on a carrier must obey scheduler isolation rules;
strict RT admission may reject unbounded blocking/FFI. Each admitted slice
remains logically exclusive; yielding can allow another actor continuation
to enter, so callback-wide atomicity across `await` is NOT implied.

`from` is a proposed contextual actor interface/trait conformance
spelling. Existing Java parser recognizes `from` at actor declarations,
but full interface/trait contract validation across backends is pending.
`using` describes effects and `requires` needs verified proofs; the
prototype performs conservative syntactic checks but no cross-call-graph
effect inference. The existing `pure`, `nlex`, `trap` contracts remain
separate, and the existing `with` keyword is not reused.

**Current boundaries:** Java/GraalVM has parser/AST and conservative
source-checker scaffolding for schema, sealed state access and `using`.
It still uses a synthetic, non-public field marker in the AST; this MUST
be lowered to actor-runtime-owned storage, not an ordinary class field.
Existing legacy actor fields are temporarily allowed only for migration
when no `state` schema is declared. New schema actors reject other fields.
Java's synchronous FFM host map is not a source-level `handle_state`
implementation; source `critical` methods currently fail closed pending
scheduler tickets. LLVM has matching typed effect bitsets and narrow
`using` parsing but intentionally rejects source `state` pending
compiler/runtime lowering. Neither backend yet provides source-level
suspendable `handle_state` or hard-real-time-certified reduction regions.

Compound `+=` assignment remains unsupported by the inspected Java
source parser; examples use `s.count = s.count + 1`. Lowering either
spelling must guarantee no reentrancy between the read and commit of an
actor-local primitive read-modify-write.


## Mandatory critical callback amendment — October 11, 2026

Every state callback in this document now uses `critical |s| -> ...`.
The sealed `self.handle_state` intrinsic itself is always critical; the
containing actor method need not be marked critical to invoke it. The caller's
`using` effects bound the transitive callback effects, while the critical
execution ticket belongs to the actor scheduler. Neither backend yet has
end-to-end suspendable state-loan source runtime parity. See
`CRITICAL_HANDLE_STATE_USING_PARITY.md`.
