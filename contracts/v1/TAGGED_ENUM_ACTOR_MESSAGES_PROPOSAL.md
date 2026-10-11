# Tagged Enum Actor Messages — PROPOSAL (not implemented)

This is a proposed cross-backend language contract. The CURRENT Java/Graal
compiler supports structurally typed `ActorMail<T>` and `send(infer struct{})`,
but does not yet parse the proposed `define enum`, `define type match`, or
`struct{...} as Qualified.Variant` syntax. LLVM currently supports an
experimental structural `ActorMail<{delta:int}|{reset:int}>` receiving
match with a tagged native handler but not nominal enum declarations,
imports, or this proposed constructor syntax.

## 1. One nominal enum defines message variants and stable numeric tags

```ores
define enum Messages as
    Reset struct{reset: int} 1
    Delta struct{delta: int} 2
end

define type match MessagePatterns as
    Messages.Reset
    Messages.Delta
end
```

`Messages` is a nominal enum: both a type containing all declared variants
and a namespace for constructing values of those variants.
`Messages.Reset` and `Messages.Delta` are **case types** with stable
positive integer wire tags 1 and 2. Compile-time validation rejects
duplicate names/tags, out-of-range tags, and invalid unsendable payloads.
Numeric tags may not silently change after publication.

`define type match MessagePatterns` produces a **type-only closed sum**
of qualified cases. It is not itself a new enum, runtime class, or tag
namespace with independent identities. `MessagePatterns.Delta`
is a type-level projection of `Messages.Delta`; it resolves to exactly
the *same* nominal case identity, fingerprint, tag, and payload schema.
A subset match may omit enum cases without reassigning numeric tags.
The first version requires all its cases originate from a single nominal
enum, avoiding accidental cross-enum tag collision. Multi-enum sums
are a later, explicitly versioned extension.

## 2. Type-only import and explicit variant construction

```ores
import type MessagePatterns from './my-actor.ores';

a.send(struct{delta: 20} as MessagePatterns.Delta);
b.send(struct{delta: 30} as MessagePatterns.Delta);
a.send(struct{reset: 0} as MessagePatterns.Reset);
```

`import type` loads compile-time exported type metadata, including the
referenced nominal enum ID, case ID, integer tag and record shape. It
must not evaluate the imported module at runtime. The alias's
`MessagePatterns.Delta` case projection must resolve even if the
underlying `Messages` symbol itself is not value-imported. A type alias
cannot rebind a case to another enum or change its numeric tag.

`struct{delta: 20}` is an immutable structural **value literal** in
the expected expression position, not a new type declaration. Followed
by `as Qualified.Case`, it is a compile-time checked *tagged construction*
operation. This is NOT a reinterpretation/unsafe cast. The compiler:
  1. Resolves the target case nominal ID and payload type.
  2. Checks required fields, types, and recursive sendability/ownership.
  3. Checks the case is permitted by the target ActorRef mailbox type.
  4. Emits an owned tagged value containing the enum ID, stable tag
     and encoded payload (with no arbitrary user-supplied tag bytes).

Reject an unknown field or wrong field type for the strict tagged
constructor in V1. Existing Java structural-width subtyping for
`infer struct{}` must not accidentally loosen this nominal wire schema.
A future intentional `..extra` syntax can make extensions explicit.

Allow optional canonical `Messages.Delta(struct{delta: 20})` value
construction later; both spellings must lower to the same tagged IR.
Legacy `infer struct{}` remains valid for structural mail types, but
must never ambiguously choose between nominal tagged variants.

## 3. Actor acceptance and exhaustive matching

```ores
define actor Counter as
    receive(ActorMail<MessagePatterns> mail): void {
        match mail.value
            is MessagePatterns.Reset m -> {
                // m.reset: int
            }
            is MessagePatterns.Delta m -> {
                // m.delta: int
            }
        end
    }
end
```

Only the actor-local `receive(ActorMail<T>)` defines accepted user
message cases; sending an unlisted case fails compilation (and native
admission checks still defend against forged external payloads).
Compile-time `match` resolves tags by **nominal case identity**, not
structural field shape or arm order. It rejects overlapping cases,
duplicate arms, and missing cases unless a deliberately specified
fallback explicitly handles them. No arbitrary ActorRef business
method call is allowed. The user-message API remains `ref.send(value)`;
lifecycle observers and administrative stop/cancel are not message sends.

## 4. Portable binary envelope (future implementation gate)

Proposed ordered fields:
- protocol identifier + version
- nominal enum ID (stable compiler/linker ABI identity, not a local pointer)
- schema fingerprint / compatible version
- numeric case tag
- payload byte length and owned binary field encoding
- runtime-authenticated envelope metadata (sender, recipient, sequence)
  separate from the guest's payload

The decoder authenticates the enum identity, checks case acceptance,
tag uniqueness, payload bounds, schema compatibility, recursive
serializability and ownership before admitting the message to the
actor's bounded DATA lane. Payloads crossing process/supervisor
boundaries use binary encoding; zero-copy shared-actor transport is a
later optimization preserving the same ownership semantics.
`get_done()` must wait for in-flight receive turns, outstanding native
field loans, finalizers and supervisor CONTROL quiescence.

## 5. Compiler/backend acceptance gates

- Java/Graal: parse and resolve `define enum`, `define type match`,
  type-only default imports and qualified variant projections;
  implement explicit checked tagged construction and compiler-enforced
  exhaustiveness, typed send/reject and runtime decoding/dispatch.
- LLVM: carry the same enum IDs/tags and payload schema through source
  AST, nominal type checker, generated tagged dispatch and binary
  serializer; lower several actors/repeated sends to scheduler-owned
  continuations. No general guest->actor method invocation is allowed.
- Differential fixtures: accepted and rejected variants, duplicate
  wire tags, wrong enum, bad shape, disallowed subset case, exhaustive
  match, forged tag, payload truncation, stale schema version, nested
  ownership/serialization, cancellation and physical finalization.
- Full Git-tree provenance, ASan/UBSan, JVM tests and GraalVM
  native-image AOT/hybrid required before release.

This file specifies desired semantics and does **not** assert that any
of the proposed syntax or wire format has been implemented.
