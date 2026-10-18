# Enum case parameter specialization — audited extension proposal (2026-10-17)

**Status: opt-in design / compiler-only proof, NOT executable Oreslang syntax.** This document proposes an extension to [NOMINAL_ENUM_OVERLOAD_CONTRACT.md](NOMINAL_ENUM_OVERLOAD_CONTRACT.md). That existing contract is the current normative target; it *explicitly rejects* `Foo.A` and `Foo.B` as overload types. Do not enable this proposal by modifying parser admission alone.

## Source syntax and type meaning

```ores
define enum Foo as A, B end

pub fnc foo(Foo.A a, int b): void { /* A */ }
pub fnc foo(Foo.B b, int n): void { /* B */ }
pub fnc foo(Foo value, int n): void { /* optional parent fallback */ }
```

* Inside a **type** position, `Foo.A` denotes a nominal **case refinement** of the resolved `Foo` declaration; in an **expression** position `Foo.A` remains the nullary case value. These namespaces must be distinct. For payload-bearing `Foo.C: int`, `Foo.C` as a type denotes *all* `Foo.C(payload)` values, **not** a singleton value or the payload type.
* `Foo.A <: Foo` and `Foo.B <: Foo`; `Foo.A` and `Foo.B` are disjoint. The case identity is a symbol resolved under the exact declaration identity, **not** the numeric wire tag, short spelling, payload shape, or host class.
* `Foo` and `Bar` are distinct nominal types even with identical cases or wire values. Imported aliases must canonicalize to their resolved declaration IDs; separate declarations may not collide because they share short names.
* A declared `Foo` variable remains statically `Foo`. A call with it must use the parent fallback if present, or be rejected until narrowed by an exhaustive `match`. Flow-sensitive narrowing, when supported, must be justified by the control-flow proof, not the current runtime value.
* **No implicit runtime tag switch** for `foo(v)` where `v: Foo`. An explicit `match v over on Foo.A ... on Foo.B ... end` has ordinary explicit pattern semantics; the compiler can still lower that match to an AOT jump table. This deliberately corrects an earlier suggestion to dynamically select among overloads: implicit runtime multimethod dispatch would break the canonical preselected-target rule.
* This proposal does **not** change constructor overloads: constructors remain selected by caller-visible arity only (optional initial `self` excluded). Ordinary class methods, static methods, free `fnc` and `routine` are proposed; actors' private abstract lifecycle hooks and sealed `receive` are not new overload entrypoints.

## Admission: safe overlap lattice

Resolve lexical owner, callable domain (MODULE, INSTANCE, STATIC), name, exact value arity, aliases, imports, and ownership first. `fnc` and `routine` occupy one MODULE domain. A same-arity family may vary only at **eligible enum positions**, and any unchanged ordinary positions must have precisely the same canonical declared type ID. No primitive/class/record/return/effect/generic-count overloading is introduced.

Each enum position is either a *whole resolved enum* `Foo` or a *resolved case refinement* `Foo.A`. For two candidate signatures at a fixed arity:

* **Disjoint**: at least one corresponding enum position has different enum declarations, or two different cases of the same enum. This is legal.
* **Strict containment**: all positions are identical or one candidate is a case-refined subset of the other, with at least one strict refinement. This is legal; the more refined candidate wins only when the **static argument proof** establishes it.
* **Identical**: duplicate definition, illegal irrespective of parameter names or declaration order.
* **Overlapping but incomparable**: illegal at declaration time, not ranked by source order. E.g. `f(Foo.A, Bar)` and `f(Foo, Bar.B)` intersect on `(Foo.A, Bar.B)` and neither is wholly more specific.
* **Different non-enum ordinary parameter IDs**: illegal even if an enum position is disjoint. Do not smuggle in `int` vs `string` overloading.
* **Whole enum plus cases**: a whole-enum fallback is legal; when calling with a static case-refined argument it loses to the matching specialization. Calling through static `Foo` binds to `Foo`, **even when the runtime value happens to be `Foo.A`**. With no whole-enum overload the call is rejected.
* **Payloads**: no payload value/shape/range tests are involved. Initial eligibility follows the already proposed nominal-enum policy: nullary and proven primitive-owned payload layouts only. A case with a nested, borrowed, dynamic, recursive or capability-carrying payload is not a specialization discriminator until separately reviewed.
* **Multiple discriminating enum positions** are allowed only when every pair is disjoint or nested. Never enumerate all tuples of cases to determine ambiguity: compare per-position relationships (bounded quadratic candidate checking).

Function/method values extracted from an overload family require a known **complete** target signature, not an erased `AnyCallable` call or an arity-only guess. Generic substitutions, unions, `Any`/`Unknown`, imported unproven identities and dynamic receivers cannot establish a case refinement by assertion.

## Binding, method vtables and ABI

1. The checker proves a single selected declaration ID from **static refined types** and records it against the exact call AST identity. Ownership/effects/visibility must check that *same* selection, not reselect.
2. For instance virtual dispatch, select the full enum+case-aware method slot **before** runtime; dispatch may choose only an override of that exact slot. A child `f(Foo.B)` does not override parent `f(Foo.A)`; a child `f(Foo.A)` must satisfy normal override visibility/return/effect contracts. Static vs instance domains never alias.
3. ABI keys must include canonical owner, domain, name, visible arity, **resolved enum declaration IDs and case symbol IDs** at refined positions, with unambiguous encoding. Do **not** use case wire numbers or payload contents. Stable source-level module/link compatibility and sealed enum evolution must be tested.
4. Preserve dispatch provenance across imports, inheritance, interfaces, closures, bound method references, `await`/suspendable calls, optimizer specialization, interpreter, JIT, Native Image and LLVM. No fallback from an invalid compiler binding to runtime name/arity lookup.
5. No runtime overload-registry scans, reflection, widening, dynamic tag-based target selection, or structural shape ranking. Pattern matching is distinct from overload resolution.

## Host reference policy (not an implementation claim)

`src/main/java/dev/oreslang/types/EnumCaseOverloadPolicy.java` is an isolated executable declaration-pair and static-proof oracle. Its caller is assumed to provide **trusted, already-resolved** enum declarations, eligibility decisions, and canonical ordinary IDs. It deliberately does not parse guest syntax, modify TypeChecker admission, or change vtables. It rejects malformed/oversized candidate sets and partial static proofs.

## Mandatory staged gates

1. Parser and AST: distinguish type-position `Foo.A` from expression-position `Foo.A`, disallow unknown cases/generic/dynamic payloads, preserve type aliases and `match` arm refinements.
2. Java checker + ownership checker + Graal runtime: identity-bound direct calls and case-aware vtables; separate static/instance namespaces; inherited/diamond override proof; no implicit dynamic fallback.
3. LLVM compiler: same source-level checks and case-aware symbol ABI; binary execution and negative diagnostics. .NET/WASM/Forge must reject until they can implement, not silently miscompile.
4. Tests: disjoint and nested candidates, fallback vs narrowed calls, same-spelling enums in two modules, identical wire tags, malformed UTF, duplicate cases, payload-bearing case value vs type, cross-position ambiguity, primitive mismatches, Any/Unknown, imports/visibility, ownership, callback extraction, polymorphic overrides, serial actor hooks, AOT, link-time evolution, and fuzzed candidate ordering.
5. Full exact-head JDK 25 Maven, GraalVM Native Image, LLVM CTest/compiled guest, and cross-backend conformance **must pass before claiming language support**. Do not mirror private source into another org absent explicit authorized destination approval.

**Current status:** this proposal supplies static model and negative/positive oracle tests only. `Foo.A` parameter specialization remains unsupported by the current production compiler.
