# Fixed product argument spreading: tuple, record, and Entry (pre-1.0)

Status: **normative design draft; NOT an implementation-complete feature**.
Scope: Java/GraalVM frontend/checker/evaluator/native image, LLVM source frontend/native lowering, `oreslang-format`, and conformance fixtures.

## The essential distinction

A tuple is a fixed **positional** product. A record is a fixed **labeled** product. Both are ordinary first-class values; neither is intrinsically a function's argument list. Expansion into call arguments is **explicit only** with `...`.

```ores
pub fnc foo(int a, bool b, string c): void { }

foo(5, true, "hi");                   // three positional arguments
foo(a: 5, c: "hi", b: true);          // three named arguments (separate named-call feature)
foo(...(5, true, "hi"));             // one tuple expression, expanded positionally
foo(...(a: 5, c: "hi", b: true));     // one record expression, expanded by label
foo((5, true, "hi"));                // ONE tuple-valued argument; different arity
foo((a: 5, b: true, c: "hi"));        // ONE record-valued argument; different arity
```

The two parenthesized literal forms are deliberately distinct. A parenthesized `IDENT : expression` field starts a record literal; positional elements start a tuple literal. Grouped expressions remain grouped, not implicitly spread. The exact grammar must preserve `(x)` as grouping; singleton tuple, zero-field record, trailing comma, ternary, and lambda ambiguity are separate parser gates, not inferred by a formatter.

Proposed declarations (not currently admitted by existing Java/LLVM parser simply because this document mentions them):

```ores
define tuple Values as (int, bool, string);
define record Args as (a: int, b: bool, c: string);
define record Entry<K, V> as (key: K, value: V);
```

Keep existing `Tuple[int, bool, string]`, `[int, bool, string]`, structural record type `{a: int, b: bool}`, `infer struct{...}`, and declared `struct` semantics intact until explicitly migrated. Records are labeled products, not a synonym for class/struct storage and mutable field identity. Never permit aggregate kinds to become interchangeable solely because they share an implementation representation.

## Call binding and evaluation contract

1. Establish a statically resolved callee and its parameter labels. For each lexical argument, evaluate its expression **once, left-to-right**, without reordering or reevaluating a record literal. In a record, field initializer expressions also evaluate left-to-right in source order. Only completed values are permuted to ABI parameter order.
2. A fixed tuple spread yields positional slots in tuple element order. A fixed record spread yields labeled slots; bind case-sensitively by the callee's declared parameter names, **not** by hash-map iteration, declaration order of the record, or structural member searches at runtime.
3. Determine effective arity after finite spread expansion. Select the callable by existing owner/name/arity rules, then the separately specified eligible nominal-enum discriminator if implemented. Record names and field shapes do **not** select an overload or affect virtual-dispatch identity. Validate labels only **after** selecting a unique callable. Return type never disambiguates.
4. Ordinary positional arguments fill leading unclaimed parameters. After the first named argument or record spread, subsequent ordinary positional arguments are prohibited in v1; any supported earlier fixed tuple spread is positional. Reject duplicate assignments, including positional + record and multiple records. Exact spread is required: reject unknown/missing/extra record fields.
5. Only a statically known **closed** tuple or record is spreadable into a fixed-arity callable. A runtime map, open structural view, `Any`, dynamic/FFI value, union without a provably identical closed product shape, or runtime-length sequence cannot be spread into fixed arity. A structural record's *assignability* may allow extra members, but that does **not** mean its value is an eligible closed argument pack.
6. Opaque callables lacking reliable labels may accept positional tuple spread, but MUST reject record spread. Overrides and trait/interface implementations must preserve declared parameter labels for supported named invocation.
7. Methods, module functions, constructors, higher-order callables, actor entrypoints, async/suspendable calls, and builtins may opt in **only when** their signature/ABI, ownership checking, and all execution paths implement this contract. Unsupported surfaces must reject at compile time; no runtime fallback to arbitrary iteration order.
8. The spread operator is a call-argument construct, not arbitrary iterable expansion; `foo(record)` never silently expands. Applying `...` outside supported call positions fails closed.

### Call plan representation

Carry ordered `CallArgument` inputs (positional, named, tuple spread, record spread; source span) through parsing and static analysis. Produce an immutable checked call plan:
- chosen callable identity and expanded arity;
- source-ordered evaluation steps, each with ownership/effect/suspension/cleanup information;
- for each evaluated scalar slot, target ABI parameter index;
- compile-time origin and precise duplicate/missing/extra diagnostics.

Backends must not re-run a record expression to produce reordered arguments. Temporary values are arranged after evaluation. A spread of a variable evaluates/moves/borrows the aggregate once; the checker verifies that projecting its fields/elements does not duplicate linear/move-only resources. A borrowed aggregate cannot smuggle mutable loans into a callee. Preserve cancellation, defer/finally and panic/raise cleanup at every evaluation boundary, including mid-literal suspension.

### Examples

```ores
define record Args as (a: int, b: bool, c: string);
define tuple Values as (int, bool, string);

pub routine main(): void {
    const Args r = (c: "hi", a: 5, b: true);
    const Values t = (5, true, "hi");
    foo(...r);  // named binding: a=5, b=true, c="hi"
    foo(...t);  // positional binding: a=5, b=true, c="hi"
}
```

`(c: effect1(), a: effect2(), b: effect3())` must invoke effects 1,2,3 (exactly once), and still bind call slots a,b,c. Reordering field evaluation to parameter order is a semantic bug.

A record is unordered *for label identity, field access, and structural equality*, but its literal's **source order of evaluation remains observable**. To convert a tuple to a record, provide names; to convert record to tuple, provide explicit ordering. A callee's declared parameter list supplies that ordering **only** for record spread at that call site.

## Record, struct, optional values, and Entry

- A record is a labeled, fixed, closed product **when used as an argument pack**. Normal structural record compatibility and `struct` ownership/layout semantics remain distinct. Do not silently reinterpret `infer struct{...}` as record spreading without a separate reviewed compatibility decision.
- `Entry<K,V>` is a conventional standard-library record with fields `key` and `value`; its two labels matter. `Entry<K,V>` is **not** automatically a `Tuple<K,V>`; no conversion by map iteration.
- `T?`, if adopted, denotes `Option<T>`, **not null and not omission**. Defaults/omitted parameters require distinct rules and collision detection against every effective callable arity. Neither is enabled by this spread feature.
- `Some` and `None` stay tagged sums; avoid sentinel nulls in runtime payload and serialized record shapes.

## Test gates (must fail closed until implemented)

**Positive executable parity:** direct tuple and record literal spread; named record-variable spread; order-independent field matching; effectful initializer evaluation order; mixed positional-prefix plus record-tail; generic call; independent module/constructor/method invocations once enabled; class inheritance with stable labels; correct arity in interpreter and suspendable continuations; Native Image AOT/hybrid; LLVM -O0/-O2; formatter idempotence.

**Negative static diagnostics:** one aggregate argument is not spread; unknown/extra/missing/duplicate labels; positional after record/named; duplicate slot via positional and label; duplicate record literal keys; dynamic/open/union product spread; runtime-length spread; borrowed/mutable/move-only elements whose projected ownership is unsound; overloaded functions that differ only by record names or shape; opaque function labels; record expression used with positional-only nonlabeled ABI; invalid `T?` omission; formatting must not rewrite `...`, reorder record fields, or turn a named call into a record literal.

**Differential requirements:** compare Java type diagnostics against LLVM source diagnostics and actual guest output, not parser acceptance alone. Assert the same fixed ABI slot mapping in synchronous, async, suspended, virtual, module, and imported execution routes before enabling them. Never claim LLVM parity from codec/transport tests alone.

## LLVM first executable Copy-only slice (experimental)

The LLVM draft branch now implements a **restricted native guest** subset:
closed scalar-i64 record locals, ordinary copyable record field reads,
inline tuple and record spreads, stored record spreads, and
positional-prefix/named-tail call binding. Source-order evaluation happens
once, with formal-slot permutation applied to values afterward. A record
local is represented by an LLVM fixed-layout stack struct with
`llvm.lifetime.start/end`; no runtime dictionary or dynamic binder is used.

This is **not** general product parity. Source-only record literals remain
one value, not implicit arguments. Nested records, owned classes/arrays,
borrow-bearing members, partial `rt take`, unknown shapes and dynamic
spreads remain rejected without IR. Java still rejects named record
argument spread until its sync/suspendable checker and invocation paths
share a checked ownership plan. Named record and generic Entry declarations
are not implemented.

Exact-source LLVM CI demonstrates 13 negative no-IR cases, nine real
guest programs compiled/executed at -O0 and -O2, lexical order in emitted
IR, no guest GC/allocator calls for this scalar subset, full CTest
(373/373) and compiler ASan/UBSan product-matrix verification. These do
**not** certify the entire linked OresVM or owned heap actor lifecycle.

## Integration status and sequencing

At proposal creation, Java `Parser.parseCallArgumentsUntil` constructs `Ast.SpreadExpr`; `TypeChecker.expandFixedCallArguments` explicitly admits *only* `TupleKind.TUPLE` and constructs positional `ExpandedCallArgument` types. The Java evaluator's `evaluateArguments` expands an argument as a sequence; other source-task/suspendable paths independently evaluate arguments. `OwnershipChecker.checkArguments` also assumes tuple-only spread. These must be changed **together**, rather than teaching just the parser to accept record syntax. `docs/NAMED_ARGUMENTS_CONTRACT.md` describes still-proposed named calls; this contract extends but does not itself enable them.

For LLVM, audit its source parser, type checker, native call preparation and per-target lowering before admitting the new syntax. For `oreslang-format`, preservation of source-order fields and explicit spread is mandatory; formatting must not claim the compiler accepts a proposal merely because it can display it.

Recommended migration:
1. Add first-class parenthesized record literal and named/spread argument AST, preserving lexical source order; refuse ambiguous/malformed shapes.
2. Implement a checked, reusable compile-time call-plan binder (positional + named + fixed product spread) with negative tests, initially on a narrow direct-call surface.
3. Feed identical plans to *all* Java invocation paths and check aggregate ownership/borrows, suspension and cleanup; test JVM/AOT/hybrid.
4. Implement LLVM AST/binder/native lowering with matching diagnostics and exact guest behavior.
5. Add parser-aware formatter fixtures and ratchet source parity gates; only then change documentation status to **supported**.
