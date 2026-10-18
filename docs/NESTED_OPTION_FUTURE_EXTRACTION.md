# Nested Option and Future extraction: proposed cross-backend contract

Status: design and conformance specification; **not an implementation claim**. Existing single-level operations remain unchanged. Both GraalVM and LLVM must pass executable conformance before any new intrinsic is advertised.

## Semantics and types

* `Option<T>.unwrap(): T` removes **exactly one** option layer, panics on `None`, and transfers any affine payload exactly once.
* `await e` where `e: Future<T>` resolves **exactly one** future layer, yielding `T`. Nested `Future` values are returned intact.
* Proposed `Option<Option<T>>.flatten(): Option<T>` joins **one** layer, mapping `None` and `Some(None)` to `None`; `Some(Some(v))` to `Some(v)`. No panic.
* Proposed `Future<Future<T>>.flatten(): Future<T>` composes **one** layer. It is nonblocking and preserves inner failure/cancellation; whether cancelling the derived future propagates to either producer is an explicit ownership decision, never an implicit cancellation of shared work.
* Proposed `Option<...Option<T>...>.unwrap_all(): T` extracts every statically known option layer, panicking at the first absent layer.
* Proposed `await_all e` where `e: Future<...Future<T>...>` sequentially awaits every statically known future layer, yielding `T`. Each successive future can only be observed after its producer yields it. This is an explicit operator, not a change to ordinary `await`.

The compiler must calculate nesting from the **static type**, not from runtime object identity or a while-loop based on host-language `Future` interfaces. If the innermost `T` is itself `Any`, a host future, or a user-defined awaitable, stop at that static boundary. Reject raw/erased types that cannot prove nesting; no dynamic recursive unwrapping. Do not implicitly traverse `Result`, `Option` from inside `Future`, or vice versa.

For a nested option, `None`, `Some(None)`, and `Some(Some(v))` are distinct representations. `unwrap()` preserves these distinctions; `flatten()` intentionally joins them. `trap expr` always adds exactly one `Option` layer and must not turn an `unwrap()` panic into `None`.

## Safety and lowering

1. Type-check every layer and instantiate generic return types accurately, including zero-layer rejection for `unwrap_all` and `await_all`.
2. Avoid evaluation duplication. An operand is evaluated once; an option layer is moved/consumed once; a future completion is subscribed to once per logical observer.
3. Preserve `None` panic, typed failure, panic, `raise`, ordinary throw, and cancellation as separate outcomes; never swallow a failure by trying to flatten.
4. On derived-future cancellation, detach observers and release retained continuations. Do **not** cancel an aliased source or shared inner future without proven exclusive ownership and documented transfer.
5. Preserve actor dispatcher nonblocking behavior. Completion callbacks enqueue continuations; never run guest code on arbitrary producer threads. Depth must not cause synchronous recursive stack overflow: compile sequential suspend states / trampoline for deeply nested types.
6. Respect lexical borrow/actor ownership across **each** suspension; release temporary values on terminal paths exactly once. Do not expose references that outlive an actor scope.
7. Do not flatten `Future<void>` beyond its static type, or treat a runtime `null` / `undefined` as an extra future/option layer.
8. Do not publish new syntax on a backend until its parser, type checker, IR, runtime, and native guest tests support it. Unsupported syntax must fail closed.

## Required conformance examples

```ores
val Option<Option<int>> n = Some(Some(42));
val Option<int> one = n.unwrap(); // if n is copyable
val int two = n.unwrap_all();
val Option<int> joined = n.flatten();

val Option<Option<int>> inner_none = Some(None);
inner_none.unwrap();               // returns None: Option<int>
inner_none.unwrap_all();           // PANIC
inner_none.flatten();              // returns None: Option<int>

val Future<Future<int>> f = nested_async();
val Future<int> inner = await f;
val int result = await_all f;
val Future<int> composed = f.flatten();
```

The examples are independently compiled in appropriate ownership contexts; they are not intended to reuse an affine `n` or `f` after move.

## Test matrix

* Option depth 1/2/3, outer/inner `None`, `Some(None)`, panic non-catchability, `Result` boundaries.
* Future depth 1/2/3, pending and already-resolved producer, inner failure and cancellation, outer failure, producer race, shared alias and observer-detach behavior.
* Compile-time inferred and explicit result types; non-Option/non-Future operands; erased/dynamic values; functions returning nested types.
* Affine payload single transfer, ownership violation rejection, actor suspension and continuation affinity, cancellation/cleanup once, deep nested chains without recursion overflow.
* The exact same source-level fixtures on Java/GraalVM and LLVM; report pending tests rather than claiming parity.
