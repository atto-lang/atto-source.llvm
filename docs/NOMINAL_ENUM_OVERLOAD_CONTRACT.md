# Oreslang overload selection: arity first, nominal enums second

**Status: normative target design, NOT enabled in current Java or LLVM compilers.**
Decision: 2026-10-13. Applies to module `fnc`, `routine`, instance methods, and
`static fnc` (subject to their existing namespace and actor/effect rules).

This contract supersedes the *proposed future* same-arity portion of
`METHOD_OVERLOAD_ARITY_CONTRACT.md`. The **currently implemented** arity-only
admission and dispatch remain in force until both the nominal enum runtime
representation and overload tables implement this extension together.

## Selection invariant

1. Resolve **scope/receiver, callable kind, exact name, and argument count**
   before considering any type. The implicit `self` is not counted in arity.
2. Different arities are different overloads, unconditionally.
3. For the **same arity**, multiple declarations are admitted only if at
   least one *corresponding parameter position* contains distinct **eligible
   nominal enum types** (different canonical enum symbol IDs). All other
   parameter positions must be the same declared type; they are not used
   to choose an overload. Non-discriminator parameter positions may be
   ordinary primitives or unambiguous nominal symbols with the **exact same
   symbol ID**; any structural, generic, union, dynamic, inferred or borrowed
   parameter makes the same-arity enum-overload family ineligible during
   the initial phase. There is no record-shape equality test even to
   establish that two non-discriminator positions "match".
4. The compiler chooses among these declarations using **only nominal enum
   type identity in their parameter positions**, never enum **case**, payload
   values, payload shape, primitive type differences, classes, interfaces,
   records, traits, inferred shapes, implicit conversions, effects or returns.
   A fully qualified enum name is a readable diagnostic; identity is the
   resolved declaration symbol, not the spelling of a short name.
5. If any same-arity candidate's applicability is not **uniquely provable at
   compile time**, reject the call and require an explicit nominal narrowing
   or checked cast. Never defer overload selection to runtime.
6. A same-arity pair differing only by `int` vs `string`, `A` vs `B`
   where `A/B` are classes/interfaces, or any record/structural shape is
   an **invalid overload declaration**, even if the shapes are disjoint.
   Enum case names, wire tags, and associated labels do not disambiguate.
7. No structural comparisons are allowed during candidate lookup **at
   compile time or runtime**. Normal assignability validation of the *one
   already-selected* callable's arguments is a separate operation and may
   honor Oreslang's structural typing rules.

## Eligible enum discriminator

A parameter's enum is eligible only when its **complete declared set of
cases** consists of nullary cases or cases whose payload **fields are all
primitive scalar values** (e.g. `bool`, `int`, `string`, subject to
explicitly supported scalar widths). A fixed tuple/product of primitive
fields may be admitted only when each leaf is directly declared primitive
and the representation is fixed, owned and fully checked; this eligibility
is prevalidated at the enum declaration, **not discovered by structural
comparisons during overload lookup**. No map, arbitrary array, record,
class, actor reference, nested enum, generic, `Any`, `Unknown`, borrow,
capability, recursive, or dynamic/interop payload qualifies in the
initial phase.

This is an **overload eligibility restriction**, not a restriction on
algebraic data types generally: `Success(User)`, `Option<T>`, and
recursive payloads remain usable for ordinary enum/match operations.

The constructor/case identity does not participate in overload lookup:
`Status.Ready` and `Status.Error("x")` have the **same enum type**
`Status`. Two enums with identical case names/payload shapes are still
**distinct enum types** when declared separately.

## Example

```ores
define enum Color as
    Red
    Blue
end

define enum Mode as
    Fast: int
    Slow: int
end

fnc render(Color color, int count): string { return "color"; }
fnc render(Mode mode, int count): string { return "mode"; }
fnc render(bool detailed): string { return "detailed"; }
```

All three signatures coexist: arity 1 versus 2 is distinct; the two
arity-2 declarations differ at parameter 0 by eligible enum symbol ID.
`render(Color.Red, 5)` is bound to the `Color` declaration at compile time.
The compiler does **not** inspect `Red` or `Fast`, compare payloads, or
search a runtime overload collection.

Reject:

```ores
fnc bad(int x): void {}
fnc bad(string x): void {} // same arity, primitive type distinction

fnc same(Status.Ready x): void {} // case is not a nominal type
fnc same(Status.Error x): void {} // variant-only overloading forbidden
```

## Runtime/ABI requirement

The compiler emits a **preselected overload ID** at each direct call. For
virtual instance calls, a virtual table indexes that resolved overload ID
and dynamic dispatch may select only a subclass override **of that exact
signature**. A child method using another eligible enum ID is a new
overload, not a replacement for a parent's slot.

The runtime NEVER selects an overload from value fields, enum payloads,
variant tags, dynamic type inspection, reflection, structural shapes, or
coercion ranking. Actor wire envelopes and FFI may validate nominal enum
identity at their **boundary**; this is type safety, not overload search.
A decoded raw primitive, erased `Any`, or imported value without a proven
nominal enum identity does not qualify to select a same-arity overload.

Function/method references to a multi-overload name need a uniquely known
signature from context or an explicit specialization. Constructor selection,
method ownership, visibility, effects, `trap`, `suspend` and overload
override checks remain separate invariants.

## Implementation requirements (not claims of completion)

- Java: implement nominal enum symbol identities and primitive-payload
  eligibility after integrating/validating tagged enums from draft PR #623.
  Replace arity-only slot maps (`TypeChecker`, `CallableSelector`,
  `OresEvalRootNode`) with compiler-proven enum signature IDs; preserve
  no-dynamic-overload invariant across generics, lambdas, imports, calls,
  inheritance, and actor boundaries. Do not alter the slot key in isolation.
- LLVM: use the same symbol/ABI key and compile-time proof; unimplemented
  enum overloads MUST be rejected rather than dropped to arity-only runtime
  dispatch.
- Include positive and negative fixtures for arity-first selection,
  same-arity nominal disambiguation, equal-shaped distinct enums,
  primitive/non-enum/structural disallowed overloads, enum payload
  eligibility, `Unknown`/`Any`/union ambiguity, imports, inheritance,
  virtual overrides, functions-as-values, serialization/interop, and
  cross-backend diagnostics. Prove no runtime shape/payload-based search.
- Keep the compiler's current arity-only admission **fail closed** until
  these components and tests land atomically on each backend.

## Non-goals

No structural or duck-typed overload selection, no type-erasure fallback,
no implicit runtime multimethods, no overload selection by return type,
no variant-payload comparison, and no runtime candidate ranking.
