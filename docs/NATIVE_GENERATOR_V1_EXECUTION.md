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

## Native frame ABI

For each generator `name`, LLVM emits `oreslang_rt_gen_name_init(i64*)`, `next(i64*,i64*)`, and `close(i64*)` with caller-owned two-word `[state, index]` frames. No stack survives the next call. `next` returns 1 for a yield, 0 for terminal DONE, -1 for invalid input. Closing retires state and zeroes the index; repeated DONE/close is idempotent. The output argument is rejected if it aliases either word of the frame, preventing state or cursor corruption. Frames still require valid, aligned memory supplied by an authorized native caller.

**This is a synchronous compiler-generated ABI, not yet the public `Iterator<T>` source consumption lowering**. Rust E2E invokes a real compiled `.ores` declaration, emits LLVM IR, links it with Clang to a native C harness, and verifies delegation order, independent activation, early close, sticky DONE, malformed frames, alias rejection, cycles, unresolved delegates, and expansion quotas.

## Not yet supported

- General `yield` expressions/variables, named or generic parameters and arbitrary source iterables;
- Dynamic `yield*` with separate child activation/cleanup/borrow semantics;
- Guest `for of` consumption directly lowered against this native ABI;
- `async gen*`, `suspend gen*`, `await`, `Future<IteratorResult<T>>`, async close/cancellation and actor-owned scheduling;
- Wasm32 or Cortex-M generator lowering (must remain explicit rejects).

For full native async generators the future compiler must create a scheduler-owned, generation-fenced activation frame with a **bounded one-outstanding-pull** ticket, typed Future result, cancellation and close physical-retirement receipts, and actor-kind pool affinity. Reusing this synchronous ABI on a detached thread is not semantic parity.

## Verification policy

The Rust/Clang native harness is registered by CMake as `native_generator_i64_compiled_execution`; passing requires **compiler source admission, emitted IR, native compile/link and actual execution**. The test source itself, an AST parser acceptance, a source-free model or zero-step GitHub Actions job is not runtime proof. Record the exact Git commit/tree, executed job steps and test output before upgrading parity. As of this change, the new delegation and alias tests have been authored but **not executed** against an exact-head LLVM build.
