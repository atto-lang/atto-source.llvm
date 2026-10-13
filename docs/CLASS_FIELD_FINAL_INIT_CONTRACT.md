# Canonical concrete field declarations (pre-1.0 migration)

Design decision: a concrete class or named struct field has a binding-slot
capability independent of the referent's mutable-access capability.

| Canonical field | Reassign member slot | Mutate referent through member |
| --- | --- | --- |
| `T x: expr;` | yes, with mutable owner | no |
| `mut T x: expr;` | yes, with mutable owner | yes, if ownership allows |
| `final T x: expr;` | no | no |
| `final mut T x: expr;` | no | yes, if ownership allows |

`const` on a concrete member is a compatibility alias for `final`.
`val` / `const mut` alias `final mut`; `let` aliases the unqualified
slot. Canonical storage/member initializers use `:`, while local binding
initializers keep their existing `=` syntax.

`final` describes slot rebinding, **not** deep freezing, uniqueness, safe
cross-actor sharing, data-race freedom, or mutable-borrow exemption. A mutable
receiver/owner and the appropriate referent capability are both necessary for
deep mutation. An inherited final slot cannot be overridden to become writable.

## Initialization model

The intended final contract requires *all* final fields to have an explicit
initializer expression at their declaration. The constructor body may validate
parameters but must not perform final-field initialization, even once.
Construction must not expose partly initialized objects. Field expressions
execute in deterministic declaration order, including failure cleanup.

Constructor parameters in field initializers (for example
`final int n: input;`) are a **separate** compiler/runtime capability and
must not be accepted without type checking, evaluation order, and failure
cleanup verified on both backends. No synthesized positional initialization
is permitted in the final pre-1.0 contract.

Class fields are distinct from Oreslang actor state (fixed-key `state { ... }`
accessed only through the sealed actor state capability). Traits declare
contracts, not implicitly allocated instance storage. Closed inferred
structural values remain deeply readonly under their existing type contract.

## Migration and verification

The existing Java implementation permits some legacy `val` constructor
initialization and synthesized positional arguments. This migration does not
remove those behaviors by silently rewriting compiled code; separate breaking
changes must update the fixtures, compiler, runtime, and native parity gates.

Reject invalid field assignments and destructive takes **before** LLVM IR
emission; run JVM type checker + interpreter/AOT/hybrid as well as LLVM
native execution, ownership and sanitizer tests. The formatter must be
idempotent and never touch local `let`/`val`/`const` declarations.

## Implementation slice in this PR

The native subset accepts literal `int` slot initializers and the existing
owned `Array<int>` initialization path. It preserves fixed/mut field metadata
and rejects assignment and destructive take of fixed fields in the native
ownership checker before IR. General class members, arbitrary expressions,
constructor parameters, methods and broad heap class support remain unsupported.
