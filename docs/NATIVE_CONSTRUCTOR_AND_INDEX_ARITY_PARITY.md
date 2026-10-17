# Native constructor and indexing arity parity

## Constructor overload identity (implemented in the restricted scalar subset)

Select a constructor by **caller-visible value arity** only. `constructor(int value)` and
`constructor(self, int value)` conflict: the optional leading bare `self`
is a construction receiver marker, not a parameter and not an ABI slot.
Parameter names and types cannot produce additional overload identities.
Return type is not a constructor dispatch dimension.

A class may declare up to one constructor per scalar value arity (0–16).
The compiler rejects duplicate arities and selects an exact arity before LLVM
lowering. An implicit zero-argument constructor exists only when the class has
**no** explicit constructor at all.

Current executable LLVM subset is intentionally narrower than Java:
constructors accept only typed scalar `int` arguments and empty/`return;`
bodies, and class fields must already have declaration-site initializers.
Every constructor overload must independently type-check every class field
initializer. An initializer referencing a parameter absent from any overload
is invalid, even when calls to that overload would not need the field.
Constructor argument expressions must be pure supported scalar trees.
Owned `Array<int>` fields with explicit constructors remain unsupported
until cleanup-on-construction-failure is proven. Actor constructors remain
forbidden.

No constructor runtime method or value-type-based overload selection is
introduced: selected scalar arguments are bound to lexical compiler-private
parameter slots before any class field is published.

## Multidimensional indexing (NOT executable in native LLVM yet)

Canonical Java semantics are one index operation per bracket pair:

- `m[i, j]`: one `index-get/2`, **not** nested unary indexing.
- `m[i][j]`: two successive `index-get/1` operations.
- `m[i,j] = value`: one `index-set/2`; `value` is not an index.
- Getter and setter are separate sealed operation families.
- Indexed reads evaluate receiver then indices left-to-right exactly once.
- Indexed writes evaluate receiver, then indices, then RHS, then setter.
- Selection is by operation + index count, not index types or return type.
- Mutable writes require exclusive receiver authority; custom getter projections
  are not borrowable until alias/provenance semantics are proven.
- Built-in collections retain unary indexing unless explicitly extended.

The present restricted native parser does not admit bracket indexing or
symbol-declared class methods. Three negative executable CTest fixtures
enforce **fail-closed** diagnostics rather than accidental unary lowering.
Replace these denials with positive compiled guest conformance only when all
layers land together: lexer/parser, multi-index AST, type inference, ownership
and effects, sealed symbol slot selection, class-method ABI/vtables, LLVM IR
lowering, and runtime/AOT fixtures. Do not claim native indexing parity from
the Java parser or host-only simulation.

A future internal ABI may encode a slot as operation + arity, but no new public
mangled symbol or cross-backend ABI is standardized by this milestone.

## Validation and release gate

The constructor branch adds two positive compile-to-LLVM-to-Clang CTests and
six rejection CTests, while retaining the earlier constructor and native
array/ownership regression suites. Native comma indexing adds three negative
CTest admission gates. Until step-bearing Actions / CTest on the exact branch
tree completes, treat this as committed implementation plus **unexecuted**
native regression tests, not a verified release.

The LLVM repository's current private-source security restrictions govern
whether an external Actions org can receive source blobs. Spare Actions
minutes alone do not authorize transfer.
