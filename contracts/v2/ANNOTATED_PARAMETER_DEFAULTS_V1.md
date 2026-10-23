# Oreslang parameter defaults via annotations — staged cross-backend v1

Status: PROPOSAL + PURE POLICY, NOT EXECUTABLE GUEST SYNTAX.
The Java parser now retains strictly closed @Default literals as staged
parameter AST annotations and rejects their execution in TypeChecker and
OwnershipChecker. The LLVM parser must still fail closed until correct
annotation AST, type checking and lowering exist.
This contract extends contracts/v2/OPTION_FORMAL_OMISSION_V2.md.

## Canonical grammar, not JavaScript's equals syntax

~~~oreslang
pub fnc connect(
    string host,
    @Default(5432) int port,
    @Default("automatic") Option<string> protocol,
    bool secure
): void { }

connect(host: "localhost", secure: true);
// port = 5432 (int), protocol = Some("automatic")

connect("localhost", 7777, None, false);
// explicit supplied None wins; protocol = None

pub fnc optional(Option<string> label): void { }
optional(); // label is typed Option<string>.None, not an Option type object

pub fnc mustSupply(required Option<string> credential): void { }
mustSupply(None); // valid
mustSupply();     // error

pub fnc nick(@Default("anon") string? nickname): void { }
nick();           // nickname : Option<string> = Some("anon")
~~~

The annotation belongs to an individual parameter, precedes its type,
and must use PARENTHESES. Do not accept @Default<...>, int x = 5432,
or silently apply an annotation attached to a method or declaration.
Name-first compatibility syntax can be added only after formatter proof.

## Exact omission behavior

- Required T: omission is an error; callee type is T.
- @Default(v) T: omission yields v of type T, not Some(v).
- Option<T> in proposed v2: omission yields typed OUTER None.
- @Default(v) Option<T>: omission yields Some(v), provided v has type T.
- @Default(None) Option<T>: omission yields typed OUTER None.
- T? is sugar for an omittable Option<T>; @Default(v) T? -> Some(v).
- required Option<T>: caller must supply a typed Option value, even
  if that value is None; default annotation + required is invalid.
- Explicitly supplied None always wins over any default: it is not
  omission. A syntactic _ or omit call item IS omission and uses default.
- A supplied T to an omittable Option<T> is wrapped in Some(T).
  A supplied Option<T> is passed unchanged.
- Option<Option<T>>: omission -> outer None. Do not implicitly infer
  a nested Option default or lift an inner Option value without proof.
  Option<T>? is not shorthand for Option<Option<T>>.

## Static constants: first release

Default payloads are closed, typechecked literals only: integer,
boolean, string, or None. Range-check integer constants for the target
type. A default on a non-Option must match its exact resolved T; None
only belongs to intrinsic Option<T>. User-defined Option lookalikes,
unresolved aliases, unconstrained generic T, arbitrary expressions,
callbacks, environment reads and mutable collection factories must
not be accepted through textual similarity or unchecked coercions.
Reject duplicates, zero/multiple arguments, wrong placement, angle
annotation forms and nonconstant defaults.

A future dynamic default factory requires a separate effect/ownership
contract; JS runtime evaluation of default expressions is not assumed.
No mutable shared default objects or sentinel null/undefined/void.

Reject defaulted rest/variadic, mutable/borrowed/structural parameters,
constructor self binders, and actor constructors until independently
verified. Zero rest items produce an EMPTY rest pack, not None.
Constructor default arguments use the same binder and must preserve
immutable-field exactly-once initialization and no partial-self escape.

## Immutable call plan and overload safety

Resolve nominal types and parameters first. Evaluate callee/receiver
and every PROVIDED argument expression exactly once in lexical source
order. Default literals never evaluate guest expressions. Only after
checking effects, borrow/move/lifetimes and argument presence does
the compiler put verified supplied temporaries and constant defaults
into ABI FORMAL order. Preserve raw call item to source-value indexes
across omission tokens; keep explicit typed None distinct from
implicit/explicit omission even if final values are equal.

A defaulted T becomes omittable but its body type stays T. Thus an
@Default on a non-Option must enter the same conservative effective
supplied-arity interval collision checker as automatically omittable
Option<T>. Never pick an overload by argument runtime type, label,
return type or declaration order. The selected declaration keeps full
ABI arity. Imported signatures, overrides and interfaces must preserve
parameter names/order/type, requiredness, normalized default value,
and versioned feature mode; do not use runtime virtual method selection
to change a caller-visible default.

## Versioning, safety and gating

Existing legacy Option<T> parameters remain required unless a
versioned signature explicitly declares otherwise. Do not silently
change imported APIs in a v2 compilation. Signatures must serialize
presence/omission metadata, typed literal digest, outer Option depth,
and versioned ABI fingerprint. Public default changes are API changes.
Unknown annotations cannot be silently treated as inert metadata.

The Java draft includes a compiler-only DefaultArgumentPolicy, an
OptionalCallShapeBinder bridge, parser-preserved formal-local literal metadata,
an AST-to-checked-literal bridge and source JUnit tests. Java parameter syntax
is **parse-only** and rejected by the semantic admission gate. LLVM is
specification-only. Neither compiler proves executable @Default behavior.

Enable guest syntax only after parameter-local annotation parser/AST,
closed typed constant evaluator, type/effect/ownership admission,
constructor and override checking, formatter and imported signature
support, interpreter + Native Image + LLVM lowering, and positive/
negative exact-tree execution tests. Pure reference semantics are NOT
proof of guest-language runtime conformance.
