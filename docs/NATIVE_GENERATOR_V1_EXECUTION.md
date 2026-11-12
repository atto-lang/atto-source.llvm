# LLVM native generator v1 — bounded execution slice

This document describes the **actual narrow compiler path** in `src/main/cpp/oreslang/frontend/Parser.cpp` and `src/main/cpp/oreslang/backend/LlvmCodegen.cpp`. It does not claim the general AttoLang iterator protocol is implemented.

## Accepted source

```atto
pub gen* inner(): Iterator<int> {
  yield 20;
  yield 22;
  return;
}

pub gen* outer(): Iterator<int> {
  yield* inner();
  return;
}
```

Native v1 accepts a maximum of 512 generator declarations, each with no parameters, literal signed-i64 yields or `yield* name()` delegating to another zero-argument **pure literal generator in the same source unit**, optional bare `return;`, and no other statements. All referenced generators are resolved before codegen, including forward references. The resolver rejects cycles, missing or non-generator targets, parameters/expressions, and any post-expansion sequence exceeding 4,096 yields. No host object/loan/defer/call/actor authority is retained through a yield.

This `yield*` subset can be statically expanded because it has **no observable side effects, captures, cleanup hooks, or dynamic iterator ownership**. Do not extend flattening to general source generators without correct delegation frames and exactly-once close propagation.

## Native source-level iteration slice

The compiler now admits a bounded synchronous `for const item of values() { ... }` (or `for val item of values() { ... }`) form when `values` is a same-file zero-parameter literal `gen*`. The function can appear before the generator declaration. The source-level consumer creates and zeroes its own frame, calls `init`, repeatedly calls `next`, binds each i64 result as a scoped immutable loop element, and invokes `close` after DONE. Invalid ABI statuses trap rather than continuing with corrupt state.

```atto
pub fnc main(): int {
  let int total = 0;
  for const value of values() {
    total = total + value;
  }
  return total;
}
pub gen* values(): Iterator<int> {
  yield 20;
  yield 22;
  return;
}
```

The native `main` wrapper converts a checked i64 guest return into an OS process exit status; this program must exit with **42** when compiled and executed. In contrast to the existing ABI C harness, the Rust E2E compiles the **whole Atto program** and links its emitted LLVM IR directly into a runnable native executable.

**Fail-closed restrictions:** The first loop form binds only immutable scalar i64 values. The generator must be named, zero-argument, pure, static-resolved and declared in the same source. Body statements are limited to scalar writes; early return, break, deferred cleanup, nested loops, calls, borrowing and dynamic iterator creation are rejected until the backend can prove exactly-once close on every exit. The generated iterator state is private to this invocation; no guest `IteratorResult<T>` object or first-class iterator can escape yet.

## Native frame ABI

For each generator `name`, LLVM emits `oreslang_rt_gen_name_init(i64*)`, `next(i64*,i64*)`, and `close(i64*)` with caller-owned two-word `[state, index]` frames. No stack survives the next call. `next` returns 1 for a yield, 0 for terminal DONE, -1 for invalid input. Closing retires state and zeroes the index; repeated DONE/close is idempotent. The output argument is rejected if it aliases either word of the frame, preventing state or cursor corruption. Frames still require valid, aligned memory supplied by an authorized native caller.

**This is a synchronous compiler-generated ABI, not yet the public `Iterator<T>` source consumption lowering**. Rust E2E invokes a real compiled `.ores` declaration, emits LLVM IR, links it with Clang to a native C harness, and verifies delegation order, independent activation, early close, sticky DONE, malformed frames, alias rejection, cycles, unresolved delegates, and expansion quotas.

## Not yet supported

- General `yield` expressions/variables, named or generic parameters and arbitrary source iterables;
- Dynamic `yield*` with separate child activation/cleanup/borrow semantics;
- General dynamic `for of`, explicit Iterator<T> variables and arbitrary iterator method calls (only the bounded same-file source for-of above is lowered);
- `async gen*`, `suspend gen*`, `await`, `Future<IteratorResult<T>>`, async close/cancellation and actor-owned scheduling;
- Wasm32 or Cortex-M generator lowering (must remain explicit rejects).

For full native async generators the future compiler must create a scheduler-owned, generation-fenced activation frame with a **bounded one-outstanding-pull** ticket, typed Future result, cancellation and close physical-retirement receipts, and actor-kind pool affinity. Reusing this synchronous ABI on a detached thread is not semantic parity.

## Verification policy

The Rust/Clang native harness is registered by CMake as `native_generator_i64_compiled_execution`; passing requires **compiler source admission, emitted IR, native compile/link and actual execution**. The test source itself, an AST parser acceptance, a source-free model or zero-step GitHub Actions job is not runtime proof. Record the exact Git commit/tree, executed job steps and test output before upgrading parity. As of this change, delegation, alias and source for-of main-exit tests have been authored but **not executed** against an exact-head LLVM build.
