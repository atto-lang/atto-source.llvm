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

## 3. Actor acceptance and value/procedural dispatch semantics

```ores
define actor Counter as
    receive(ActorMail<MessagePatterns> mail): void {
        do match mail.value over
            on MessagePatterns.Reset as m {
                // Handle m.reset without producing a result.
            }
            on is MessagePatterns.Delta as m {
                // Handle m.delta. 'is' is optional.
            }
        end
    }
end
```

Every canonical `match subject over` arm begins with `on`;
`on MessagePatterns.Reset as m { ... }` and
`on is MessagePatterns.Reset as m { ... }` are identical checked
type/case patterns. The `as m` binding narrows the value to that
variant. The canonical body is braced with no arrow, and `on _`
is the unguarded catch-all.

There are four execution forms, identically for `match` and `switch`:

| Form | Value? | Exhaustive? | Meaning of an arm-local `return` |
|---|---|---|---|
| `match / switch` | yes | required | yield a value to the expression |
| `return match / return switch` | yes | required | yield a value, then outer return exits the function |
| `do match / do switch` | no | optional | return directly from the containing function |
| `return do match / return do switch` | no | required | return directly from the containing function, and every reachable arm must be terminal |

Every value expression requires a value-yielding return on every
normal path through every arm, with compatible result types.
Any result may be discarded if the expression is used as a statement;
dead result construction may be optimized away, but all side effects,
owned-value finalizers, and exceptions must be preserved.
No selected arm falls through into another arm.

The `return` prefix on a value expression is the ordinary containing
function return of the collected value. In a terminal `return do`
statement, the prefix adds an exhaustive, must-terminate proof obligation:
every possible value must select an arm, and every selected arm must
exit the containing function (or have a proven non-returning path).
Neither `do` form allocates a match/switch result slot.

A catch-all is unnecessary when a compiler can prove that the arms
cover a finite closed domain, such as both Boolean literals or every
case of a declared enum/type-match subset. Guards must be accounted
for; a guarded wildcard alone does not prove exhaustiveness. An open
`int`/`string` ordinarily requires `on _` or `default`.
Unknown cases are not silently accepted across versioned actor
message boundaries.

```ores
pub fnc classify(string v): string {
    return match v over
        on "foo" { return "bar"; }
        on _ { return "other"; }
    end
}

pub fnc classifyDirectly(string v): string {
    return do match v over
        on "foo" { return "bar"; }
        on _ { return "other"; }
    end
}
```

The first `return` in each arm yields to the *value match*; its
enclosing `return match` exits the function. The second form's
arm-level `return` exits the function directly. A nested `do match`
inside a value arm keeps that function-return boundary; a nested
lambda creates a new lexical function-return boundary. `try/finally`,
defer cleanup, borrowing, cancellation and future suspension must
respect these boundaries.

For backwards compatibility, the Java/GraalVM branch currently
keeps old direct arrow-spelled statement arms (`on ... -> { ... }`
and `case ... -> { ... }`) with their historical function-return
behavior. Mixed arrow/arrowless arms in that direct-statement
context are rejected. This compatibility surface should be migrated
and eventually deprecated, not silently converted to value semantics.

Current implementation status: Java/GraalVM has lexical arm-return
semantics for synchronous value expressions and procedural forms,
tested in a funded JVM run. LLVM has a limited native `int` expression
and function-statement lowering with result PHI and direct do-arm
returns; nominal enum cases and arbitrary guest actor types are not
implemented by that fragment. Resumable suspension from within a
value arm is a separate compiler/runtime parity gate and must not
be considered certified yet. The tagged `MessagePatterns` syntax
above remains a proposal until nominal enum/import lowering lands.

Only the actor-local `receive(ActorMail<T>)` defines accepted user
message cases. No arbitrary ActorRef business methods may be called.
The approved user-message dispatch operation is `ref.send(value)`,
with lifecycle and administrative operations separately typed.

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
