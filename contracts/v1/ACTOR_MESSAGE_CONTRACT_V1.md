# Actor Message Contract V1 — draft cross-backend admission protocol

This contract describes the guest-language API shared by Java/GraalVM and
LLVM. It is **not** a claim of runtime or wire-format feature parity.

## Actor declaration and accepted payloads

An actor declares the messages it accepts through an **actor-local**
`receive(ActorMail<T> mail): void` handler. The type `T` is a
closed-to-unknown-type **typed payload contract**: it can be a concrete
sendable type, structural record, or discriminated union of record variants
in backends that implement those forms.

Example syntax supported by the Java/Graal type checker:

```ores
define actor Counter as
  receive(ActorMail<{delta: int} | {reset: int}> mail): void {
    // The actor handles the delivered message, within its admitted DATA turn.
    return;
  }
end

const counter = spawn Counter();
await counter.get_ready();
counter.send(infer struct{delta: 20});
counter.send(infer struct{reset: 1});
```

An implementation must reject `counter.send(infer struct{other: 5})`.
Each payload's declared required keys, primitive field types and all other
actor-callable-boundary sendability rules must be checked before admission.
Structural record width subtyping currently permits extra fields in
Java/Graal; exact wire schema validation and discriminant serialization
remain a separate contract to finalize.

## Sole user-message dispatch operation

`ActorRef.send(message)` is the **only** actor reference method that
dispatches a user-defined message. There is no
`await actor.increment(20)`, `actor.receive(mail)`, or
`actor.handle_state(...)` direct user-code invocation.
`receive` is actor-local and runs only after DATA mailbox admission.
Other ActorRef operations such as `get_ready()`, `get_done()` and
administrative stop/cancel observe/control lifecycle; they are not
business-method dispatch.

An actor's internal state is actor-owned. The long-term state design
restricts mutable state access to the actor's explicitly authorized
`handle_state` critical section; a receive hook must not create another
public read/write capability to that state.

## Envelope and concurrency

`ActorMail<T>` carries the typed `value` plus host-managed envelope
metadata such as `sender`, `recipient` and sequence. Guests cannot forge
turn tokens, supervisor CONTROL events, actor heap pointers or sender
identity through an `infer struct` body.

Send admission is bounded and nonblocking on a carrier. Saturation,
cancellation and actor termination must give explicit failure/acknowledgement
semantics; they must never silently discard a completed promise. Actor
user-code runs on its own exclusive logical execution lane. Supervisor
CONTROL runs on a distinct exclusive lane and remains available when DATA
is saturated. Completions resume admitted continuations, not arbitrary
guest code on foreign callback threads. A send is **not** a user-defined
method return; reply messages, output streams or reply channels are
separate capabilities.

`await ref.get_done()` is a physical lifecycle barrier: cleanup,
outstanding native loans and callback quiescence must finish first.

## Implementation gates

- Java/Graal: typed `ActorMail<T>` and `ActorRef.send` checking,
  including structural unions, plus guest interpreter delivery exist;
  this does not certify the full native field memory ABI.
- LLVM pilot: one i64 field structural payload with
  `ActorMail<{by: int}>` and `send(infer struct{by: ...})` has
  compiler and generated-native execution tests; arbitrary union,
  multi-field message types, multiple actors in a single source program,
  and general event-driven source continuations are not yet implemented.
- Release: run the **same** source fixtures through both backends for
  accepted and rejected message variants, bounded mailbox delivery,
  cancellation, supervision, async resumption, result/output transport
  and native resource finalization.
