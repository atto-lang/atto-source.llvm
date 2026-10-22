# RFC: orthogonal Option types, omittable parameter slots, and annotated defaults

Status: **proposed alternative**, 2026-10-19. Design-only. NOT an adopted grammar or executable guest-language feature. Existing v1/v2 contracts and runtime admission guards remain in force until this RFC is approved, version-gated, implemented, and tested. This document deliberately does not rewrite prior 62-case v1 fixtures.

## Recommended syntax: two independent postfix question marks

```oreslang
pub fnc requiredOption(string? name): void { }    // name: Option<string>; caller MUST supply
pub fnc defaulted(@Default("anon") string name?): void { } // name: string; omitted -> "anon"
pub fnc optionalOption(string? name?): void { }    // name: Option<string>; omitted -> None
pub fnc optionalLong(Option<string> name?): void { } // equivalent to optionalOption

@DefaultArgVals(a: 5, b: "charlie")
pub fnc run(int a?, string b?): void { }           // a: int, b: string

run();                       // run(5, "charlie")
run(b: "alice");             // run(5, "alice")
run(b: "alice", a: 7);       // run(7, "alice")
run(a: _, b: "alice");       // explicit omission uses default for a
run(a: omit);                // both default values used
```

- `T?` is **type syntax** for intrinsic `Option<T>` wherever types are allowed (parameters, locals, fields, returns, generic positions). It is NOT `null`, `undefined`, or an omittable slot.
- `name?` is a **parameter-presence marker**. It does NOT alter the callee-visible type. It is invalid on local variable names, ordinary fields, and return declarations.
- `T? name?` means both: an Option-typed slot that may be absent.
- `T name?` is only legal when a safe compile-time default is provided; without a default there is no value of type `T` to bind. Reject rather than introduce an uninitialized variable or sentinel.
- A parameter-level `@Default(v)` and declaration-level `@DefaultArgVals(name: v, ...)` are alternative spellings for one canonical per-slot *closed constant default*. A default itself grants omittability even when `name?` is absent; the marker may be used for clarity, but it is not required when a default exists. Thus `@Default(5) int a` and `@Default(5) int a?` have the same normalized signature.
- `Option<T> name` and `T? name` require a supplied argument in this RFC's strict mode, even if that argument is `None`. For a default-free omittable Option, use `Option<T> name?` or `T? name?`.

## Normalized signature semantics

Every formal carries **independent** metadata: resolved nominal value type; presence REQUIRED/OMITTABLE; optional typed constant default; original name and source span; ownership/variadic flags; and language-mode version.

| Declaration | Callee-visible type | Missing argument | Provided `None` |
| --- | --- | --- | --- |
| `int a` | `int` | ERROR | ERROR |
| `int a?` | invalid declaration | ERROR: missing default | ERROR |
| `@Default(5) int a` | `int` | `5` | ERROR |
| `@Default(5) int a?` | `int` | `5` | ERROR |
| `int? a` | `Option<int>` | ERROR | `None` |
| `int? a?` | `Option<int>` | typed outer `None` | `None` (supplied) |
| `@Default(5) int? a?` | `Option<int>` | `Some(5)` | `None` (supplied) |
| `@Default(None) int? a?` | `Option<int>` | typed outer `None` | `None` (supplied) |

`@Default(None) int? a` is also omittable by virtue of its default, despite the absence of `a?`. Presence metadata tracks supplied `None` versus implicit omission versus syntactic `_`/`omit` even when they materialize the same `None` value.

A supplied `T` to a statically known `Option<T>` *formal* may be explicitly lowered by the checked argument adaptor to `Some(T)` independent of presence. A supplied resolved `Option<T>` passes unchanged. This is NOT a universal implicit conversion for assignments or fields. Nested `Option<Option<T>>` is never implicitly lifted from `Option<T>`: require an explicit typed outer constructor. Types/aliases/generics must be nominally resolved, not recognized by the string `Option`. A required Option means **presence**, not that its contained value must be Some.

## Annotation admission and precedence

- `@DefaultArgVals` is **declaration-level** and uses exactly the same `name: closedConstant` label convention as named call arguments; it is permitted on functions, routines, class methods, and explicit ordinary-class constructors once each has supported lowering. It is not a function-call expression and is not a method dispatch annotation.
- `@Default` is **parameter-level** only; `@DefaultArgVals` is **declaration-level** only. Enforce this at parsing and again at semantic resolution. A duplicate default through any combination is an error, even when literal values match. Unknown, repeated or misspelled parameter names in `@DefaultArgVals` are errors.
- For a first executable release, only closed int64-range checked integer, Boolean, string and typed `None` literals are allowed. No symbolic references, expressions, calls, lambdas, mutable objects, environment reads, heap-sharing aliases or compile-time effects. Integer width is checked against the resolved destination. `None` default only applies to intrinsic `Option<T>`.
- Reject defaults on rest arguments, construction-scoped `self`, mutable/borrowed/structural slots, actor constructors, and unproved generic/unresolved type positions. Reject invalid defaults before running user code. Future richer constant/default factories require independent immutable/effect and ownership proofs.
- The annotation is merged into normalized metadata before overload admission. Literal equality is checked after type resolution. The presence marker may not mask a malformed annotation. No annotation may be quietly ignored by unsupported targets.

## Deterministic binding and evaluation

For a candidate callable signature:
1. Resolve declaration identity, import/visibility and nominal types before guest argument evaluation.
2. Parse source call items with immutable original order: optional label, supplied expression OR standalone `_`/`omit`, and source span. A syntactic omission is never an Expr. `None` is a supplied real Option value. Reject double commas, null, void, omitted required slots and omitted rest elements.
3. Positional values bind to contiguous leading formal slots; a later named item binds by exact case-sensitive formal label. Reject positional-after-named, unknown/duplicate labels, collisions with positional prefix, and inferred positional holes.
4. Evaluate the callee/receiver once, then every *provided expression* once in lexical call-item order. Preserve exceptions, borrow/move effects and cancellation; omission/defaults perform zero guest-expression evaluations. Build an immutable source-item -> evaluated-value -> formal-slot proof and preserve explicit-omit origins.
5. After type/effect/ownership validation, place supplied temporaries and defaults into fixed **formal-order ABI slots**. Defaults never affect runtime ABI arity, selection, or source evaluation order.

```oreslang
@DefaultArgVals(a: 5, b: "charlie")
pub fnc f(int a?, string b?, bool c): void { }
f(c: effectC(), b: effectB()); // effectC, then effectB; a=5
f(c: true, a: _, b: None);     // ERROR: b is string, not Option<string>
```

Overload admission conservatively rejects intersecting effective supplied-arity ranges, including intervals widened by annotation defaults; no fallback to label-only, type-only, return-only, declaration-order or runtime guessing. This is aligned with existing arity-only overload rules. Explicit constructors use effective user arity (excluding leading constructor `self`); constructors must still initialize required immutable fields exactly once and cannot leak `self` during construction. Overrides/imported signatures must preserve normalized labels/order, presence bits, defaults and feature-mode/ABI fingerprints.

## Compatibility with staged Oreslang drafts

The current staged v1 syntax `T? name` means **an omittable formal whose body sees Option<T>**. The proposed v2 contract additionally makes ordinary resolved `Option<T>` formals omittable and adds contextual `required Option<T>`. This RFC chooses a different, more orthogonal rule: type-level postfix `?` is always Option sugar and omission is explicit in the *parameter* or a default annotation. Consequently do NOT silently adopt this RFC into the existing v1/v2 parser, callers, imported metadata or 62-case fixture oracle.

If accepted, add an explicit versioned `ORTHOGONAL_OPTION_PRESENCE` language/signature mode; parse `T?` in all type positions, represent `name?` on the Param separate from type, and require `Option<T> name?` to opt into omitted None. Translate old `T? name` signature metadata to Option-typed + omittable **only across an explicit compatibility boundary**. `required Option<T> name` in old v2 translates to required `Option<T> name` in this mode, not the other way around. Do not reinterpret an old binary signature by reparsing printed source.

The existing Java branch has staged parser+AST and pure binder/default policy; it has a narrow executable **direct named primitive literal exact-arity call** subset, but no executable omission/default/Option formal lowering. LLVM has narrower named exact-arity behavior and staged contracts. These mechanisms must not be considered proof of this proposal.

## Required acceptance matrix (no runtime enablement before green proof)

Positive: all eight table rows that are valid; `@DefaultArgVals` vs `@Default` equivalence; non-trailing omissions with subsequent named arguments; supplied None defeating a default; explicit `_`/`omit` triggering a default; reordered side effects; nested Option with explicit Some(None); method/constructor/indirect call metadata, generic/alias resolution, formatter roundtrips and legacy/v2/strict-mode imports.

Negative: plain `T name?` lacking default; omitted required `T? name`; wrong-typed/overflowing defaults; duplicate/conflicting/unknown defaults; mutable/borrowed/rest/self defaults; Option lookalikes; nullable/null/void sentinel attempts; double comma/positional-after-named; call-plan provenance forgery; overlapping arity intervals; outdated metadata/override mismatch; exception/ownership/cancellation changes introduced by ABI reorder.

Parity gate: run identical positive/negative guest programs on Java interpreter, GraalVM Native Image and LLVM -O0/-O2 with exact source-tree/CI evidence. Keep fail-closed semantic admission until that full pipeline is verified.
