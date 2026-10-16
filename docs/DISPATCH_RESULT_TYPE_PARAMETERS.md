# Proposal: result type parameters for match, switch, and select

Status: **proposed language contract, not certified compiler support** (2026-10-13). This document describes desired observable behavior across independent Java/GraalVM and LLVM compilers. Existing partial draft implementations are not evidence of parity.

## Motivation

The user-facing value forms should admit a single explicit result type, written as a positional parameter, independent of the inspected/selected value's type:

~~~ores
pub fnc use_match(Any v): int {
    return match<int> v over
        on String as s -> { return 5; }
        on !String as other -> { return 4; }
    end;
}

pub fnc use_switch(Any v): string {
    return switch<string> v over
        case 5, 4, 3 -> string { return "1"; }
        default -> string { return "3"; }
    end;
}

pub fnc use_select(Channel<string> ch): string {
    return select<string> over
        when readch ch: string m -> string { return "1"; }
    end;
}
~~~

The two changed details relative to the original proposal are intentional: the switch/select result is 'string' rather than 'int'; a read operation must use a statically proven 'Channel<string>' (not an unrestricted 'Any' channel capability). 'String' may remain a recognized compatibility spelling for the immutable 'string' scalar.

## Syntax and names

- Value-producing forms: 'match<T> subject over ... end', 'switch<T> subject over ... end', 'select<T> over ... end'.
- A source expression MAY omit '<T>' when contextual/inferred result type is unambiguous.
- The positional '<T>' is the **preferred canonical spelling** for an explicit result type. An existing '<ret=T>' spelling MAY remain accepted as compatibility syntax; if it does, it must carry exactly the same semantic result type. No other named parameters may be silently accepted.
- Type parameters describe the **result value**, never the subject type, channel payload, pattern discriminant, or generic specialization of the dispatch mechanism.
- Arm type assertions such as 'case 5 -> string { ... }' and 'when readch c: string x -> string { ... }' assert the **arm result** type. They do not override the outer '<T>'. '-> { ... }' is equally valid and preferred when an arm type annotation would merely repeat '<T>'.
- Following 'readch c:', 'string x' is a typed channel-payload binding. Its type must match the proven channel payload type and is **independent** of the arm result annotation that follows '->'.
- Parse '-> Type {' unambiguously as an annotated arm only in the arm-header position; do not greedily reclassify a value expression or nominal aggregate constructor. If the grammar cannot distinguish a constructor expression, require a braced block rather than silently changing expression meaning.
- Accept 'on Type as variable' and the complement type pattern 'on !Type as variable' only when the runtime type test is reifiable. The latter binds a value that is known not to have 'Type'; it must not be treated as a 'Type'. 'String' followed by '!String' is exhaustive for valid runtime values of 'Any' when the type-test system defines them as logical complements.

## Result typing and control flow

Let the explicit result parameter be 'T', and let each reachable arm's value result have type 'Ri'. Require 'Ri' assignable to 'T' under Oreslang's ordinary safe assignment/coercion rules. Reject an incompatible arm even if it would be selected only at runtime. Explicit '<T>' is a constraint, **not** a cast or permission for erased/unsafe conversion. When omitted, contextual return/assignment type may provide 'T'; otherwise infer a sound join or reject ambiguous joins. Treat the invariant result of the dispatch expression as 'T'.

Each selected value arm's 'return expression;' completes the **dispatch expression**, represented as 'ARM_RESULT'. It does not immediately return from the enclosing callable. Thus 'return match<T> ... end' has two boundaries: the inner arm returns to the match, then the outer 'return' returns the resulting 'T' from the function. An arm's nested function/closure return belongs to that nested callable and does not satisfy the arm-result proof. Nested procedural dispatch must never capture or overwrite the outer value arm's result destination.

For procedural 'do match', 'do switch', and blocking 'do select', a winning arm's 'return expression;' targets the **enclosing callable** ('FUNCTION_RETURN'), not a discarded arm-local value. These forms produce no value; do not interpret a value-context 'return do match/switch/select' as if it were a value dispatch. Legacy 'do match<ret=T>' source, where admitted, denotes an enclosing callable return requirement, not a value produced by 'do match'; avoid introducing that ambiguity in new canonical examples.

A value 'match' must be exhaustive over its proven subject domain, taking guards and reachability into account. A value 'switch' needs exhaustive coverage or a 'default' (including for open 'Any' subjects). A value 'select' is **availability-driven rather than domain-exhaustive**: it may suspend without a ready arm. Every arm that can win and complete normally must produce 'T'; closure, timeout, cancellation and failure paths must be explicitly handled or propagate an appropriate typed effect/error, never manufacture a 'T'. No value is obtained from a losing arm.

## Static and runtime safety

1. Check the subject's type, discriminant equality and type-pattern semantics independently of 'T'. Keep String/"5", integer 5, and Symbol #foo distinct. Preserve exact large-integer equality and duplicate-case checks; do not compare integers through lossy floating-point conversion.
2. Readch requires a statically verified Channel<Payload> and matching binding type. An arbitrary 'Any' expression cannot be used as a capability without a safe, proven narrowing; neither 'Unknown' nor unchecked erased generics authorize a channel read.
3. The borrow/ownership checker must prove all captures and arm results safe across select suspension, cancellation, actor crossing and exceptional cleanup. No borrowed result may escape its lifetime merely because its nominal type matches 'T'.
4. Once a select arm wins, commit exactly one operation and publish at most one 'ARM_RESULT'. Cancel/detach losing registrations before exposing results; late callbacks, timeouts and cancellation cannot replace a published result. No arm result is published before its deferred/finally cleanup obligations complete.
5. Compile-time diagnostics must distinguish: invalid result parameter; incompatible result; incompatible arm annotation; missing result; nonexhaustive match/switch; invalid type-test complement; invalid/erased channel capability; payload binding mismatch; unsafe escaping borrow; unsupported backend value lowering.
6. An unsupported backend must reject the feature before execution. It must never lower value syntax to procedural statements that accidentally exit the function.

## Conformance matrix (fixtures to implement, NOT test evidence)

| Case | Expected |
| --- | --- |
| match<int> on String / !String returns 5 / 4 | Accept; runtime selects correct value |
| switch<string> on {5,4,3}, default | Accept; produce "1" or "3" |
| select<string> reading Channel<string> | Accept if effect/capability permitted; one selected result |
| switch<int> with return "1" | Compile-time result mismatch |
| select<int> with return "1" | Compile-time result mismatch |
| select<string> with unrestricted Any channel | Compile-time unsafe capability error |
| switch<string> arm annotated '-> int' | Compile-time conflicting arm annotation |
| match<int> with only String arm over Any | Reject: missing complement/default |
| switch<string> of Any with no default | Reject unless exhaustiveness can be proven |
| match<int> arm captures short-lived borrow escaping after suspension | Reject ownership violation |
| nested match in an arm and explicit outer function return | Correct separate ARM_RESULT / FUNCTION_RETURN |
| nested procedural do select in value match | Correct enclosing return or fail closed if not implemented |
| failing/deferred select callback, cancellation and stale continuation | No duplicate publication; cleanup guaranteed per effect contract |
| formatter on explicit <T>, arm annotation and 'when readch' | Idempotent; never drops/corrupts type arguments |

## Integration/verification plan

- **Java/GraalVM:** reconcile value-expression AST and ARM_RESULT across draft #900, #709, #634 and procedural select #886, plus typed read arms #815 and static select readiness #433/#906. Preserve branch-local bindings, typechecking, ownership, cleanup, asynchronous resumption, and Native Image metadata.
- **LLVM:** add/verify grammar, typed AST/HIR, borrow-checker obligations, expression-result phi/merge lowering, proper nonlocal procedural returns, and fail-closed unsupported select variants.
- **oreslang-format:** preserve 'match<T>', 'switch<T>', 'select<T>', typed '-> T {' arms, negative type patterns and typed readch bindings. A formatting rewrite must never change return destination.
- **Shared contract tests:** commit exact-source positive/negative guest fixtures, deterministic error categories and cross-backend manifests; run parser, type, borrow, JVM, GraalVM AOT/hybrid, LLVM/native and suspension/cancellation stress tests against pinned Git tree SHAs.
- **Formal checks:** model two noninterchangeable return destinations and one-shot select arm publication under retry, cancellation and cleanup. State/model checking of an abstract protocol is not a proof that Java/LLVM runtimes refine it: require linked runtime traces and real executable tests.

### Dependencies and precedence

This proposal complements common-source PR #20's four-form return-destination contract and Java PR #886's procedural select semantics. Older PR body summaries that describe procedural returns as arm-local/discarded must not be treated as authoritative. No pending implementation PR may advertise value-form or native parity on the strength of this document alone.

### Rollout

1. Land shared grammar/semantic contract and rejection fixtures first.
2. Implement match<T> value expression on both backends, then switch<T>, then blocking select<T> while keeping unsupported paths rejected.
3. Coordinate the formatter and exact-head CI before declaring any syntax supported.
4. Preserve '<ret=T>' compatibility if already shipped; normalize it to the preferred '<T>' only after the parser/formatter/source-compatibility tests agree.
5. Roll back feature admission, not return semantics, if a backend cannot prove cleanup or borrow safety.

## Select result modes (proposed, not yet implemented)

- `select<T> over ... end` is a **blocking/suspendable value expression**. It returns a `T` only after one selected arm completes normally. It does not require every operation to be simultaneously ready, and it may wait indefinitely without an explicit timeout or cancellation.
- `nb select<T> over ... end` is the proposed nonblocking-start form yielding `Future<T>`. Exactly one arm completion or failure settles the Future. Detached fire-and-forget behavior is **not** suitable for a value expression.
- `try select<T> over ... end` is the proposed immediate readiness probe yielding `Option<T>`: `None` indicates no ready arm; `Some(value)` indicates that an arm was selected and completed normally. A ready `default` arm can supply `Some(T)`.
- Cancellation, channel closure, and arm exceptions must preserve their established typed error/effect behavior; they are **not** silently converted into `T` or the no-readiness `None` case.
- The annotation `-> T { ... }` always constrains the **arm-produced value**. On `when readch channel: Payload item -> T { ... }`, `Payload` is independently checked against `Channel<Payload>`.
- A blocking select expression cannot be lowered to a procedural `do select` whose selected-arm return exits the enclosing function. Suspension must retain distinct `ARM_RESULT` continuation ownership.

## Current implementation tracking

- **Java/GraalVM draft [#911](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/911)**, stacked on value-match [#900](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/900), proposes actual `match<T>` / `match<ret=T>` AST, parser, type and ownership-preserving lowering, plus complement type patterns and focused JUnit tests. It is **not CI-verified**, and `switch<T>` / `select<T>` remain unimplemented in that branch.
- **Java draft [#886](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/886)** preserves the procedural `do select` return destination, a prerequisite but not value-select support.
- The compiler's `Any` and `Unknown` runtime/static boundaries need independent reconciliation with [Java #844](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/844) before asserting that `match<T>` examples over `Any` compile on both backends.
- Rust formatter, LLVM frontend/codegen, Java Native Image AOT/hybrid, synchronous and suspendable source runtime, and shared conformance verification all remain gating work. Passing parser-only tests does not certify production semantics.
