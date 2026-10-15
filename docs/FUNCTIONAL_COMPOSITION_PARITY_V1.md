# Functional sum composition and pure actor transitions: native parity gate

Status: **LLVM guest source not implemented**. Cross-language semantics are
specified at [common-source functional actor composition v1](https://github.com/ores-truffle-oreslang/oreslang-common-source/blob/feat/elm-functional-contracts-20261015/docs/FUNCTIONAL_ACTOR_COMPOSITION_V1.md).

## Current proof boundary

LLVM `main` supports a restricted source subset and various separate
**host-only** Result/native actor substrate tests. None of those demonstrate
compiled Oreslang `Option<T>.map<U>`, `Result<T,E>.and_then<U>`, typed
decoders, an Elm-style actor reducer, or replay. Do not promote host support
into guest source-level parity.

This PR adds a native compilation admission test with a *positive compiler
control* and negative cases for Java-targeted Option/Result composition.
It requires unsupported source to fail with a nonzero exit and no LLVM IR.
This is **rejection parity**, not execution parity.

## Required future implementation order

1. Closed nominal `Option` / `Result` tagged data layout in guest LLVM IR,
   active-payload destruction, exact ownership move/borrow/drop checks.
2. Compile source-specified unary closures or callable references with
   correct capture/effect contracts and static output type selection.
3. Add synchronous `map`, `and_then`, and `map_err` lowering with
   absence/error callback bypass, failure propagation, and no implicit copy.
4. Differential Java/LLVM scalar guest tests for execution output, errors,
   callback invocation count, ownership and variant tags.
5. Integrate pure transition snapshots, one actor turn at a time, commit
   before effect dispatch; no held state loan across suspend. Only then
   introduce typed `Forward`, `Backward`, subscriptions and replay.

Canonical effect syntax remains `with` for upper bounds, `using` for
lexical resource acquisition and `critical` for exclusive state write.
`self.handle_state` stays sealed. A Future remains executed async work;
an effect description is inert until a capability-checked runtime executes
it. Native actor carrier exclusivity and cancellation ordering must survive.

## Review/CI

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure -R native_functional_composition_admission
```

Command only; no passing-run assertion. Build and exact-tree CI must pass
before marking this draft ready. Once LLVM admits any of the fixtures,
*replace its negative gate with positive executable differential tests in
the same change*; never silently remove a negative test.
