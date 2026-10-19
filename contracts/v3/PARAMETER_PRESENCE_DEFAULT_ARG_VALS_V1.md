# Oreslang v3 proposal: type optionality versus argument presence

Status: **staged Java parser/AST plus pure contract; no executable v3 guest calls**.
A tooling-only `Parser.parseStagedV3` entry point recognizes this spelling and
retains independent `Ast.Param.argumentOmittable` and declaration-local
`Ast.Annotation.namedDefaults` metadata. The normal compiler entry point keeps
the legacy grammar. `V3DefaultArgValsPolicy.validateDeclaration` checks closed
literal/default/presence shape only, not nominal Option identity or ownership.
The v3 selector is a source-free binder preview; existing runtime ABI and
legacy binder reject v3 presence until complete lowering. LLVM's scalar parser
rejects v3 syntax with explicit diagnostics. This is NOT Java/GraalVM/LLVM
executable language parity.
This is a proposed *revision* of the staged v1/v2 T? parameter contract.
Do not silently reinterpret an old module's `T? name` declaration.
The current Java parser uses `T? name` for an omittable Option parameter;
the LLVM parser and optional/default/runtime lowering have not reached parity.

## Two distinct postfix question marks

```oreslang
pub fnc a(int? x): void {}   // x: Option<int>, argument REQUIRED unless defaulted
pub fnc b(int x?): void {}   // x: int, omittable; requires verified default
pub fnc c(int? x?): void {}  // x: Option<int>, omittable; missing gives outer None
pub fnc d(int x): void {}    // required int unless defaulted

@DefaultArgVals(a: 5, b: "charlie")
pub fnc run(int? a, string? b): void {}
// run() -> (Some(5), Some("charlie"))

@DefaultArgVals(a: 5, b: "charlie")
pub fnc plain(int a?, string b?): void {}
// plain() -> (5, "charlie"); neither formal is an Option
```

**Type suffix `?`:** `T?` is type sugar for the compiler-intrinsic
`Option<T>`, in ALL type positions when v3 is enabled (not a nullable pointer).
A type cannot be made omittable by this syntax alone. The resolved intrinsic
Option identity, rather than spelling or an impostor named Option, is authoritative.
Nested `Option<T>?` explicitly denotes `Option<Option<T>>`; implicit inner
Option lifting is forbidden. The `T??` spelling is reserved/rejected in v3
for readability; use explicit nesting.

**Formal-name suffix `?`:** `T x?` is a parameter-presence declaration;
the body still sees exactly T. An omitted non-Option formal MUST have an
explicit, typed, closed default. `T x?` without a default is a compile error,
never a zero/null/undefined substitution. `Option<T> x?` may omit to typed
outer None. Bare `T? x?` combines both independent properties.

**Default metadata independently grants omittability**, so `T x` plus
`@DefaultArgVals(x: value)` is permitted and has the same omission behavior as
`T x?` with that default. A declared name-suffix question mark documents
intent; the formatter MUST NOT invent or erase it when round-tripping source.
A `required` presence modifier (where supported) conflicts with both `x?`
and default metadata. Previously compiled signatures retain their declared
feature mode and are not reinterpreted.

## Declaration-scoped annotation

```oreslang
@DefaultArgVals(port: 5432, secure: false)
pub fnc connect(string host, int port?, bool secure?): void {}
```

The annotation applies to exactly one callable declaration: fnc, routine,
method, explicit class constructor and eventually typed callback signatures.
Entries are compile-time **named metadata**, NOT guest call expressions.
Every key must be an exact existing formal name; duplicate labels, unknown
labels, duplicate `@DefaultArgVals`, conflicting per-formal `@Default`,
defaults on receiver/self/rest, unknown annotation targets and illegal
placement are errors. An unannotated formal remains unaffected.
Default metadata must survive serialization, imports, interface matching,
overrides, reflection and hot reload. Defaults and formal labels contribute
to a versioned *source-contract* fingerprint. Existing full declaration-arity
runtime ABI is preserved.

First version accepts only closed, well-typed int/bool/string/None literals,
with integer range checks and canonical typed representation.
`None` is accepted only for nominal Option<T> parameters and means *outer*
None. A non-None literal v for an Option<T> formal means Some(v) after exact
underlying T type checking; for T means v directly. No arbitrary expressions,
calls, mutable object factories, symbolic references, unresolved generics,
implicit numeric narrowing, environment reads, callbacks, borrowed/defaulted
resources or side-effecting defaults. Support for richer immutable constants
requires its own deterministic constant evaluator and allocation proof.

Per-formal `@Default(v) T name` remains an alternative authoring syntax;
either spelling normalizes into the same single immutable default map.
Duplicate definitions of the same formal are rejected, even if values match.

## Presence and value are orthogonal

Without a declaration default:

| Formal | Call omitted | Supplied exact T | Supplied Option<T>/None |
|---|---|---|---|
| T x | ERROR | pass T | ERROR |
| T? x (Option<T> x) | ERROR | Some(T) (checked boundary only) | pass Option<T> |
| T x? | ERROR at declaration | n/a | n/a |
| T? x? | outer None | Some(T) | pass Option<T> |

With `@DefaultArgVals(x: v)`, missing `T x` or `T x?` inserts T=v;
missing `T? x` or `T? x?` inserts Some(v) (or outer None for v=None).
Explicit, well-typed supplied None **never** triggers a default. An explicit
`omit` or bare `_` call item consumes one omittable fixed formal slot,
evaluates nothing and DOES trigger its default. A missing labelled slot
likewise triggers its default. Positional holes (`f(1,,3)`), `void` and
`null` are not omission values. Type coercion T -> Some(T) is permitted
only on a statically resolved Option formal boundary, not for arbitrary
assignments or nested implicit lifting. Required presence does not prevent
passing explicit None.

## Binding and execution contract

1. Resolve the callable's owner and *versioned* signature before evaluation.
   Validate default keys against declaration formals, types, ownership and
   closed constants. Reject shadowed Option impostors.
2. Map a positional prefix and subsequent named call items to formal slots.
   Labels are case-sensitive; reject duplicate, unknown, collided and late
   positional items. Only optional fixed slots can receive `omit`/`_`.
3. Evaluate callee/receiver exactly once, then all *supplied* guest arguments
   exactly once in lexical order. Omitted slots evaluate no guest expression.
   Borrow/move/effect/exception ordering is source ordering, not formal order.
4. Materialize checked Option wrappers and constant defaults exactly once
   only after source evaluations, then permute into full formal ABI order.
   Preserve the raw call-item position, dense evaluated-source index,
   selected declaration identity and explicit vs implicit omission metadata.
5. Admit execution only with a compiler-owned, exact-AST binding proof.
   Interpreter, GraalVM AOT/Native Image, LLVM AOT and async paths must
   enforce identical ordering and ownership/cleanup semantics.

Non-trailing omittable positions require labelled subsequent supplied slots
or an explicit standalone omission marker. Variadic rest must remain final
and is never a defaultable/omittable scalar; an absent rest is an empty pack.
Overload families conservatively reject intersecting effective-arity ranges
including defaulted and name-suffix optionals. Labels, return type, runtime
values and declaration order never break overload ties.

## Integration and migration blockers

- Java staged `Ast.TypeRef.omittableFormal` must be split into a resolved
  Option type node vs an independent `Ast.Param.presence` field. Existing
  `Param.optional()` does not represent the v3 semantics.
- Java `Ast.Annotation` positional TypeRef arguments cannot represent
  `@DefaultArgVals(a: 5, b: "charlie")`; add a distinct ordered,
  immutable named-constant metadata node with source spans.
- Java `OptionalCallShapeBinder`, `OptionFormalPresencePolicy`,
  `DefaultArgumentPolicy` and `NamedCallBindingPlan` must converge on one
  versioned, proof-carrying call plan; no parallel dispatch semantics.
- LLVM `Parser.cpp` must parse the feature in a gated mode, with typed
  Option and fixed ABI slot lowering; current fail-closed behavior remains.
- Until all these checks and real guest execution tests exist, v3 input
  must be rejected rather than partially executed or silently downgraded.
- CI tests must cover parse/format/parse, imported signatures/overrides,
  hot-reload mismatch, default collisions, missing required Option, explicit
  None versus omit, named side-effect ordering, borrow/move/cancellation,
  constructor initialization, and Java/LLVM parity at exact commit trees.
