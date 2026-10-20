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

## Current LLVM implementation

The native subset supports up to 16 scalar constructor parameters with an
empty or return-only body. Pure declaration-site integer arithmetic reads only
constructor parameters or literals; argument values are evaluated once into
private slots before field publication. Owned/effectful arguments and explicit
constructors containing Array<int> fields remain rejected before IR.

`const` and `final const` retain the existing literal scalar initializer rule.
`val T` is the fixed-slot, mutable-referent alias `final mut T`. Repeating `mut`
after `val` is rejected. For owned arrays, mutation still requires a mutable
owner and no conflicting loans; fixed slots cannot be reassigned or taken.
The class metadata records slot fixedness and referent capability separately.

Free functions require explicit `pub` for scalar and array projections,
including bulk filled-array mutation. Owning a class does not bypass privacy.
The native subset has no instance methods, inheritance or general effectful
construction.

## Verification identity

Merged #403 and #356 established the constructor/visibility baseline with 403
Linux CTests, sanitizers and source-identical Java/LLVM/Wasm constructor result
40. The #369 reconciliation adds val-array mutation, const/val/final mutation
and take rejection controls, while preserving those newer guards and generic
signatures. Results for the reconciled head are recorded in its PR; historical
branch snapshots are not current evidence. Firmware execution and full Java
feature parity remain separate work.
