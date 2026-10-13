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

Native LLVM supports a bounded scalar constructor form:

```ores
define class Pair as
  final int a: start + 1;
  final mut int b: start * 2;
  constructor(int start) { return; }
end
```

Constructor parameters must be typed `int`, the body must be empty or
`return;`, and constructor calls must have the exact positional arity.
Arguments are checked as side-effect-free i64 scalar expressions, evaluated
exactly once, and stored in private temporary slots. Declaration-site field
expressions may reference only these parameters or i64 literals and may use
addition, subtraction, multiplication, and negation. Each expression is checked
for lexical names and accepted syntax before native IR generation. The native
backend emits ordered, stack-owned i64 field stores before the constructed
object is observable.

The existing owner checker still rejects final/const/val slot reassignments
and destructive moves. Owned Array<int> fields with an explicit constructor,
effectful field/argument expressions, arbitrary constructor body statements,
reference-valued parameters, inheritance, and general object field expression
lowering remain **rejected before IR**. Unqualified and pub scalar constructors
share the same bounded admission.

CTest now includes a source-identical scalar constructor guest shared with
Java/GraalVM (expected result 40), explicit/public ctor guests, duplicate and
unknown params, arity failures, effectful expressions, and owned-array denials.
These fixtures are currently committed but **not validated by a running CI
worker**. This implementation must not be represented as backend parity until
real exact-tree compiled native and GraalVM results pass.

## Branch integration and source identity

Main has advanced with overlapping parser, codegen, ownership and CMake
changes. GitHub currently reports this draft PR as merge-conflicted.
A conflict-aware rebase is required; neither replacement of main files
with stale versions nor force-merge is acceptable. A source-identical
native Pair constructor guest has been added to the Java test suite and
the LLVM CTest fixtures (expected result 40); it is not yet executed.

## Constructor argument ownership boundary

Pure integer syntax is insufficient by itself: an experimental owned-int
variable lowers to a native region-read operation and can fail. Native
scalar constructors therefore accept only ordinary stack i64 operand
sources; owned-region reads, class owners and array handles are rejected
by the ownership checker even when the syntax is a single variable name.
A negative CTest fixture covers this before LLVM IR generation.

The PR remains merge-conflicted and its current native checks lack
executed CI evidence.
