# Option-typed formal omission v2 — cross-backend staged contract

Status: PROPOSED NEXT LANGUAGE MODE ONLY (2026-10-18). Does NOT enable
any guest syntax in Java/GraalVM or LLVM, and does NOT replace the existing
v1 62-case checker while executable backends retain legacy semantics.

## Decision and syntax

In **v2 mode**, a formal whose statically resolved nominal type is the
intrinsic `Option<T>` is omittable. `T? parameter` desugars to the exact
same formal type and omission policy, and exists only as shorthand.

```oreslang
pub fnc foo(int a, Option<string> b, bool c): int {
    // b : Option<string>
    return 5;
}

foo(3, "", true);            // b = Some("")
foo(3, None, true);          // b = None, supplied
foo(a: 3, c: true);         // b = None, unbound
foo(3, _, true);            // b = None, explicit omitted call item
foo(3, omit, true);         // same as _ (provisional spelling)
foo(c: true, a: 3);         // b = None, reorders only ABI temporaries

pub fnc other(int a, string? b, bool c): int { return 5; }
// other and foo expose the same optional-parameter signature shape.

// Presence is intentionally required even if a None value is acceptable:
pub fnc mustDeclare(required Option<string> token): void { return; }
mustDeclare(None);          // valid; explicitly present None
mustDeclare();              // error: token missing
```

`required` is a contextual modifier in *formal-parameter declarations*,
not a new runtime type or an ordinary expression. It is valid only before
a statically proven intrinsic `Option<T>`. Reject `required T?`,
`required int`, or repeated/ambiguous qualifiers. Do not confuse this
with existing unrelated keywords or visibility modifiers.

## Compile-time admission

- Resolve the nominal type through aliases, generic substitutions, imports,
  constraints and namespace lookup BEFORE inferring optionality.
  Never use the text of `Option` or a user-defined imitation to classify.
- `Option<T>` (non-variadic, non-required) and `T? parameter` are omittable
  in v2. In legacy mode, only explicitly marked `T? parameter` is staged
  as omittable; existing `Option<T> parameter` remains required.
- `required Option<T>` requires a supplied, type-checked `Option<T>` value.
  `None` counts as supplied. Omitted value, `_` and `omit` are rejected.
- A supplied `T` to an omittable `Option<T>` formal is checked against
  exactly `T` and lifted once to `Some<T>(value)`. An already typed
  `Option<T>` is passed unchanged. A required `Option<T>` does not accept
  this implicit `T` lifting. Generic inference cannot depend on absent
  values; `None` still needs a known expected Option element type.
- `Option<Option<T>>` is omittable because its OUTER container is Option.
  Missing -> OUTER None. An explicitly supplied `Some(None)` is distinct.
  Do not implicitly lift an inner `Option<T>` into an outer Option; require
  an explicit, well-typed outer constructor. `Option<T>?` is rejected,
  not translated to `Option<Option<T>>`.
- Ordinary `Option<T>` expressions/fields/returns do NOT gain call omission,
  default evaluation, implicit conversions, or null semantics. `None` is
  the canonical guest variant expression; `Option.None` is not presumed
  supported by existing syntax.
- `...Option<T> rest` is a rest sequence of Option-typed ELEMENTS. Zero tail
  items -> empty sequence, not missing Option. Reject `...T? rest`.
- An omitted mutable/borrowed/structural Option formal must fail closed
  until ownership and loan lowering is proven. No nullable pointer sentinel.

## Source-to-formal lowering invariants

Keep separate semantic states:

1. Implicitly unbound optional slot: inject typed None; zero user Expr.
2. Explicit `_` / `omit` call item: inject typed None; zero user Expr;
   preserve its original call-item position.
3. Supplied typed `None`: pass Option<T>.None; one provided expression
   (a real value), distinct presence bit from cases 1 and 2.
4. Supplied `Some(T)` or arbitrary Option<T>: pass unchanged.
5. Supplied T: construct Some(T) after checking types, effects and ownership.

For `foo(c: effectC(), a: effectA())`, evaluate effectC then effectA,
exactly once each. The source index map is [1,-1,0] for formals a,b,c;
the ABI must still carry all three formal slots. Neither omission nor
typed None may accidentally move, borrow, or suppress a supplied Expr.

## Overloads, methods, constructors and compatibility

The compiler must reject overlapping conservative effective-arity ranges
for candidate overloads whenever any slot is omittable. That now includes
intrinsic Option<T> formals. Do not use supplied runtime types, parameter
labels, declaration order or return types to break ambiguity. All import,
override, interface and callable-reference signatures must preserve each
slot's resolved type and presence contract. All ordinary class explicit
constructors use the same binder and preserve immutable-field initialization
and construction-scoped self safety. Actors remain constructor-free.
Synthesized field-order constructors are not implicitly rewritten.

**Do not globally flip existing code.** Changing `Option<T> parameter`
from required to omittable alters call arities and overload admission, so
v2 mode needs a versioned module feature gate and imported signature
metadata. Existing pre-v2 modules remain legacy even when consumed from a
v2 module; source and binary boundaries must not infer rules from call-site
spelling. The proposed contextual `required` modifier enables a v2 author
to preserve required-Option signatures deliberately.

## Delivery gates

1. Parse `required Option<T>` and resolve Option identity; add explicit
   presence metadata to AST/checked signatures/ABI version fingerprints.
2. Wire checked call plans into TypeChecker, ownership/effect passes,
   constructor initialization, imported signatures, and both backends.
3. Keep correct evaluated-source indexes across named reorder and `_` /
   `omit` items. Exact compile-time errors must precede guest side effects.
4. Test legacy/v2 modules together, alias and generic Option forms,
   shadowed Option lookalikes, nested Option, rest and mutable rejects.
5. Run the same positive/negative guest fixtures in interpreter,
   GraalVM Native Image and LLVM -O0/-O2 and prove exact-tree build parity.

Implementation checkpoint: Java `OptionFormalPresencePolicy` is a pure
type-verified-input classifier with standalone Java 21 smoke coverage.
It is NOT connected to guest source parsing or runtime lowering.
