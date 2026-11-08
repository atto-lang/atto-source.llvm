# LLVM compiler metering verification — draft release gate

The experimental native scalar-frame continuation uses two independent
budgets: (1) a bounded physical dispatch fuel allowance no larger than 512
source-loop iterations, and (2) a non-refillable logical tenant fuel balance
stored in the exclusively host-owned 32-byte v1 frame.

The separate `SourceCooperationQuantum` checks soft scheduling fairness at
exact 128-backedge sampling boundaries. It is **not** hard untrusted quota
enforcement. In particular, do not batch or skip mandatory physical/logical
fuel charging in order to amortize watchdog and clock calls.

## Compiler's current structural admission boundary

`ScalarLoopLowering.cpp` generates the bounded, opt-in scalar loop only.
The compiler confirms that its generated fuel-debit Store belongs to the
committed loop body and applies `every_cycle_contains_meter` to the actual
LLVM basic-block CFG. Every cyclic control-flow route must pass through that
designated metered block; an alternative unmetered cycle fails compilation.
The graph verifier is independent of LLVM for deterministic unit testing.

This is not a general-purpose untrusted guest verifier. It does not yet
establish safety of arbitrary indirect calls, recursion, host FFI, mutable
actor stacks, critical/borrow scopes, inter-tenant isolation, or compiler
optimizations after module emission. Those paths must remain separately
gated until a real guest execution / continuation correctness proof exists.

## Reproduce the standalone verifier without LLVM libraries

```sh
cmake -S . -B build-metered-cfg -G Ninja \
  -DORESLANG_METERED_CFG_HOST_ONLY=ON \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-metered-cfg
ctest --test-dir build-metered-cfg --output-on-failure \
  -R '^native_metered_cfg_cycles_fail_closed$'
```

Execute the portable CTest under both GCC and Clang, with warnings as errors,
and at least one `-fsanitize=address,undefined` runtime. Keep the test live
when `NDEBUG` is defined. An assert-disabled test is not an executed proof.

## Required full compiler and compiled guest gate

```sh
cmake -S . -B build-llvm -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-llvm --target oreslang-llvmc \
  oreslang-native-metered-cfg-verifier-test \
  oreslang-native-reduction-quantum-test
ctest --test-dir build-llvm --output-on-failure \
  -R '^(native_metered_cfg_cycles_fail_closed|native_reduction_quantum_watchdog_host_only|native_rt_scalar_frame_aot_two_tasks)$'
```

The native frame AOT Python CTest compiles **generated** LLVM IR with Clang
`-O0`, `-O2`, `-O3`, then runs the C11 harness. Require terminal logical
fuel exhaustion across repeated 127-step carrier handoffs, zero-fuel calls,
exact-budget successful completion, 32 post-exhaustion replays, stale/invalid
frame traps, and healthy peer progress. Report individual actual executable
results, not merely a successful parse or an unrelated host test.

For a private-runner mirror, retain the original source PR head SHA and
verify all source tree blobs against that head before using the results as
`EXACT_HEAD_PASS`. A stale carrier `CMakeLists.txt` or partial source copy
is **not** sufficient provenance. Do not mark skipped/pending/failed-to-start
jobs as successful.

## Evidence and release status

- 2026-11-08 UTC: exact Git blob hashes for the portable verifier source and
  CTest: `f213db2ef89df9d0e47f7771e4ef65bd5955cae2` and
  `ab88c7a92131d9ad7fa81b79fa9c9f629463d202`.
- Portable verifier: local GCC, Clang, `NDEBUG`, and GCC
  AddressSanitizer/UndefinedBehaviorSanitizer executions **passed**.
- The source-org PR GitHub Actions jobs failed before usable job steps;
  compiled LLVM scalar AOT exact-head O0/O2/O3 and full guest runtime
  certification remain **NOT_EXECUTED**.
- The accessible private `numerics-atto/mirror-oreslang-source.llvm`
  default branch has a different `CMakeLists.txt` blob from source `main`;
  it cannot be assumed to be an exact-head mirror without additional
  full-tree reconstruction and SHA verification.

**Do not merge this PR or enable untrusted general source-loop execution
until the remaining gates are green.**
