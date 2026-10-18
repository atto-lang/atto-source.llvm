# Optional value parameters — cross-backend v1 contract

Status (2026-10-18): SPECIFICATION / REFERENCE-BINDER TESTS ONLY; not executable guest-language support. Do not claim acceptance on either compiler until parser, type checker, ownership checker, interpreter/native lowering and conformance are green.

This contract extends named value arguments from mandatory-only slots to omittable slots for functions (fnc), routines, class instance and static methods, explicit constructors, and eventually actor methods/interface methods. It does not add default-expression evaluation or nullable references.

## Canonical declaration and calls

    pub fnc foo(int a, string? b, bool c): int {
      // Inside foo, b is Option<string> (Some supplied string, or None).
      return 5;
    }

    foo(3, "", true);           // OK: all slots supplied positionally
    foo(a: 3, c: true);         // OK: middle slot omitted
    foo(3, c: true);            // OK: positional prefix + named tail
    foo(c: true, a: 3);         // OK: named order independent
    foo(3, true);              // ERROR: positional true binds b, c absent
    foo(3);                    // ERROR: mandatory c missing
    foo(a: 3, b: "", c: true); // OK

    define class Box as
      val int value;
      constructor(int value, string? label, bool active) {
        self.value = value;
      }
    end
    val box = new Box(value: 7, active: true);

## Meaning of question mark

- In a parameter declaration only, a postfix question mark on a type means that the parameter is omittable. Example: string? label.
- The declared value type is string, and the body-visible local type is Option<string>. An omitted parameter is None (not null, empty string or uninitialized storage). An explicit plain string is wrapped as Some(string) by the checked call plan; an explicit Option<string> can be passed as-is. This lifting is limited to this optional-parameter boundary and is never a general implicit nullable conversion.
- Do not silently broaden the meaning of question mark in general type positions, record fields, or casts. Ordinary Option<string> parameter spelling without the question mark is NOT omittable.
- For v1, disallow applying the optional marker to an already-optional Option<T> value type to prevent ambiguous nested-option conversion; reject it explicitly.
- An ordinary required parameter can follow an optional one. A caller wishing to skip an interior optional slot must use named arguments for the later slots.
- Optional parameters do not implicitly have arbitrary defaults. A future default-expression feature requires its own evaluation order, dependency, side-effect and ownership contract; reject parameter declarations containing both postfix ? and = for now.
- All parameters still have unique exact, case-sensitive labels. No runtime dictionary or record allocation is required.

## Checked call binding

1. Resolve the static lexical owner, visible name and overload family. Dynamic/unlabeled callable references and foreign calls cannot accept omission or named calls without explicit metadata.
2. Parse and preserve an ordered list of argument items: optional label, expression, source span and spread flag. Positional items form a prefix; a positional item following a named item is an error.
3. Match positional items to formal slots in declaration order, WITHOUT skipping optional slots, then named items by exact label. Reject duplicate binding (including positional/named collision), unknown labels and extra positional items.
4. Reject every unbound required slot. Inject a typed None for each unbound optional slot. Convert supplied T to Some<T> for an optional slot; already-typed Option<T> stays unchanged. No implicit null.
5. Before reordering, evaluate the callee/receiver and supplied expressions EXACTLY ONCE in source order. Preserve move, borrow, lifetime, exception cleanup, suspension and cancellation. An omitted slot has no evaluation.
6. Record a checked immutable call plan containing the selected declaration ID, slot permutation, optional-slot injection/lifting, generic instantiations and ownership/effects. Lower ABI values in FORMAL order. Do not re-evaluate expressions after reordering.
7. Omitted optional generic parameters contribute no type inference; require inference from other supplied arguments, explicit type arguments or separately supported generic type defaults.

## Overload hardening — conservative pre-1.0 rule

The existing dispatch policy uses declaration name and arity, with eligible nominal enum distinctions separately staged. Optional parameters extend call *effective supplied arities* beyond declaration arity; simply keeping an arity-sized lookup is incorrect.

For v1, within each existing overload domain (fnc and routine share one; class instance/static have separate domains), reject optional-capable overload families whose conservative supplied-argument-count intervals intersect: [number of required formals, total formals]. This errs on the side of rejecting potentially unambiguous call shapes, but ensures no call can select a different declaration in the type/ownership/runtime passes without a stronger proof. Keep existing ordinary distinct-arity overloads when intervals do not intersect. Do NOT use labels, runtime values or nullable conversions to break ties. Overrides and interface implementations keep exact parameter labels, types and optionality at each ordinal slot; changing optionality is source/API breaking. Constructors remain a single explicit declaration.

A subsequent version may relax interval collisions only with a sound proven call-shape disjointness algorithm and a selected declaration ID carried through all backend paths.

## Constructors and records

- Explicit class constructor parameters follow the SAME binder as ordinary calls, including optional omission, named labels and source-order evaluation for new Class(...).
- Constructor visibility, field initialization exactly once, required field assignment, immutable/final field rules, inheritance/super chaining and effect restrictions are unchanged.
- Legacy synthesized field-order constructors are NOT silently converted to optional parameter lists. Only an explicit constructor opts in.
- Named arguments resemble a record structurally at the source level but are statically bound and erased into ordered ABI slots. Object/record literals remain distinct and are not automatically spread as calls.
- Keep actor message payloads and serialization distinct from value argument omission; remote ABI/schema evolution requires a versioned compatibility rule.

## Backends audited on 2026-10-18

Java: Parser.parseParametersUntil builds Ast.Param(TypeRef,name,structural,mutable), without optionality; parseCallArgumentsUntil returns plain Expr values with no labels; Ast.CallExpr/NewExpr carry positional expressions; TypeChecker puts functions into name + declared-arity slots. Existing docs/NAMED_ARGUMENTS_CONTRACT.md itself says named value arguments are proposed. Java therefore needs named-argument support before optional calls can be fully implemented.

LLVM: frontend/Parser.cpp accepts named call arguments and rejects duplicates or positional-after-named, and tests/fixtures/named-call-* provide native scalar coverage. frontend/Parser.hpp Parameter does not carry optionality and native constructor parsing is constrained to scalar fields; the full Option<T> runtime and constructor coverage is NOT established. Do not model omitted values as integer zero.

## Required implementation sequence and fail-closed tests

- Introduce optionality metadata for EVERY callable parameter form, including public imported signatures, method overrides and class constructors; parse postfix ? only in parameter declarations initially.
- Bring Java named call AST/binding to parity; preserve source spans and source-order argument temporaries.
- Add a single checked binder reused by all callable kinds per backend; arity-only lookup must not silently resolve optional arity aliases.
- Add explicit Option<T> lowering, None injection and T-to-Some lifting with exact ownership/effect behavior; stop in the frontend if a backend lacks that representation.
- Validate class synthesized-versus-explicit constructor paths and public/imported constructor metadata.
- Add positive/negative fixtures across fnc, routine, static and instance methods, constructors, inheritance, nested expressions, generic inference, borrow/move, exactly-once effects, spreads, and type errors. Include Graal interpreter/native image and LLVM -O0/-O2, plus formatter idempotence.
- Reject unknown/duplicate labels, missing required slots, extra arguments, positional after named, wrong types, unchecked dynamic calls, inherited label or optionality drift, overlapping overload intervals, and omission in an unsupported backend.
- The adjacent JSON cases + Python reference checker validate only binder rules and overload admissibility. They DO NOT demonstrate guest-language compiler or runtime conformance.

## Explicit absence vs. no supplied argument (hardening amendment, 2026-10-18)

Canonical proposal: a bare `omit` in a call-argument slot is a **source-level omission item**, never a value expression; it occupies one formal slot and is valid ONLY for an optional parameter. Its final runtime local result is `None`, but the compiler must preserve a separate `supplied` presence bit in the checked call plan.

    foo(3, "", true);           // b = Some("")
    foo(3, None, true);         // b = None, explicitly supplied
    foo(3, omit, true);         // b = None, not supplied
    foo(3, c: true);            // b = None, not supplied
    foo(3,,true);               // reject: empty argument syntax is never a slot
    foo(3, void, true);         // reject: void is a return/type marker, not a value
    foo(3, null, true);         // reject: standalone null is forbidden in Oreslang
    foo(omit, "x", true);        // reject: required a cannot be omitted
    foo(a: 3, b: omit, c: true);// optional label may be explicitly omitted

`omit` is reserved in a *call argument item* grammar, including named argument values (e.g. `label: omit`), not available as an ordinary expression or a runtime value, not serializable, not captured, not returned, not spread and not accepted as a record literal field. Do not silently reinterpret an identifier currently bound to a value named `omit`; require an explicit reserved-context token policy before parser rollout. No comma-hole `foo(3,,true)` syntax. No `void` literal. A required `Option<T>` formal can accept explicit `None` but cannot accept `omit` unless explicitly marked optional; optionality and Option-typedness are orthogonal.

The compiler must distinguish:
- **Provided value**: source expression evaluated once, typechecked; ordinary `T` becomes `Some(T)` at a `T?` boundary.
- **Provided `None`**: real `Option<T>` value, explicitly supplied; no evaluation of arbitrary default expressions.
- **Implicitly omitted**: caller leaves an optional slot unbound; no source expression or borrow/move happens.
- **Explicitly omitted (`omit`)**: positional/named slot is consumed, but no runtime expression or borrow/move happens.

All four have source-span and presence/omission metadata in the checked call plan. The two omitted cases lower to typed `None` in v1. Future explicit defaults (not in v1) apply only to omitted slots, **never** to provided `None`.

An optional call argument is not an `Option` value in an arbitrary expression: no `foo(3, void_returning_function(), true)` rewriting; `void` functions cannot satisfy `Option<T>`.

### Parser/runtime enforcement

Use an `ArgumentItem = Value(label?, expression, span) | Omit(label?, span) | Spread(expression, span)` tagged AST; do not fake omission with `Ast.NameExpr("None")`, `null`, `void` or a zero-sized expression. Track bound slot index, supplied/not-supplied, explicit-vs-implicit omission, and argument source ordinal. Validate kinds BEFORE evaluating source expressions, and avoid constructing option wrappers until all type/ownership checks are satisfied. For a positional `omit`, increment the positional formal index exactly once; never let it remove an argument and shift subsequent positional values left. Reject omissions of mandatory, borrowed-required and variadic-rest slots. Require exact, generic-instantiated `Option<T>` for an explicit `None` or `Some(T)`; never use string matching of expression source such as `startswith("Some(")` for semantic type checking.

## Variadic declaration and call-spread audit

Existing Java `...tuple` call-site spread is supported for statically known tuples (see `ArgumentsTupleSpreadTest`); runtime-length collections cannot currently be spread into a fixed-arity callable. LLVM's current scalar-native parser does not provide a general variadic declaration/rest-packing ABI. Neither backend may claim this proposed variadic declaration syntax already runs.

Canonical future **variadic declaration** spelling follows existing prefix-spread notation:

    pub fnc emit(int id, string? label, bool active, ...string tags): void {
      // tags: readonly borrowed contiguous Slice<string>, possibly length zero
      return;
    }

    emit(3, None, true);                  // explicit empty option; tags=[]
    emit(3, omit, true);                  // no label; tags=[]
    emit(3, omit, true, "a", "b");       // tags=["a","b"]
    emit(3, omit, true, ...tagList);      // variadic tail may expand a checked List<string>
    emit(id: 3, active: true);            // tags=[]
    emit(id: 3, active: true, tags: ["a", "b"]); // named rest PACK only

The declared `...T rest` must appear ONCE, at the FINAL parameter position, and cannot also be marked optional with `?`, be a defaulted parameter, or be passed `None`/`omit` directly. Rest has zero-or-more elements, each of exact/assignable `T`; an empty variadic rest is a length-0 collection, **not** `None`. Keep separate `RestParameter` metadata rather than encoding rest as `T?`, Option<Array<T>>, or a generic `Array<T>` field. Rest named argument is one `List<T>`/`Array<T>` pack rather than a single `T` element; mixing it with trailing positional rest elements is a duplicate binding. The two forms `tags: [..]` and `...tagList` must produce the same element sequence but retain different source binding kinds.

V1 constraints for safety:
- No middle-position rest or multiple rest declarations. No runtime-spread into fixed-arity signatures. Fixed-arity tuple spread retains its existing independent semantics.
- Static parameter binding proceeds in declaration order for positional arguments. `omit` may only mark an optional fixed slot and consumes that slot; it MUST NOT skip a rest element or act like an empty rest.
- After a named value/pack/omit argument, no positional argument and no spread argument are allowed (existing named/spread mixing fails closed). `emit(id: 3, active: true, tags: pack)` is allowed when its pack type is proven.
- A variadic tail may accept a single final runtime collection spread `...items` when its element type and lifetime are proved; arbitrary unbounded iterator/async stream/foreign spread is NOT implicitly admitted. Tuple expansion for fixed-arity calls stays unchanged.
- Rest is a borrowed read-only `Slice<T>` view in the callee; source-order evaluation, temporary lifetimes and cleanup are compiler-checked. No mandatory heap allocation in freestanding core; if stack/continuation-frame packing is unsupported (including suspend/async/actor pipelines), fail closed. To retain/mutate values, callers explicitly create and transfer an owning collection under normal ownership rules.
- A named rest pack supplies zero-or-more rest elements as **one** named argument. Empty `tags: []` is provided-empty, not absent and not `None`.
- Type inference from a **zero-element** rest is impossible unless another explicit argument or generic default constrains `T`.
- Effective arity is `[required_fixed_count, +infinity)` for any variadic signature, so it conservatively conflicts with any sibling overload in the same existing domain whose admitted call arity intersects. Reject the ambiguous overload family, without selecting by element runtime types, names or omission markers.
- Keep actor-message transport, constructor field initialization, FFI, overrides, public metadata and binary/serialized interfaces fail closed until they carry explicit rest shapes and feature-negotiated ABI. Constructors may adopt a trailing variadic rest only after those invariants are proven.
