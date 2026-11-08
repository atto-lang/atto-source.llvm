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
The verifier rejects graphs exceeding 262,144 blocks or 1,048,576 edges.
Its portable tests exhaustively compare the algorithm with an independent
topological-sort oracle on 3,584 small graph/meter combinations.

`ScalarGuestCallVerifier.hpp` also enforces a deliberately closed scalar
guest call surface on the **actual emitted LLVM Module**: four v1 frame
functions, no recursion, FFI, indirect calls, inline assembly, or additional
imports beyond `llvm.trap`. The new LLVM-backed test
`native_scalar_guest_calls_fail_closed` constructs positive and adversarial
LLVM Modules. This is call rejection, not general interprocedural metering.

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
  oreslang-native-reduction-quantum-test \
  oreslang-native-scalar-call-verifier-test
ctest --test-dir build-llvm --output-on-failure \
  -R '^(native_metered_cfg_cycles_fail_closed|native_scalar_guest_calls_fail_closed|native_reduction_quantum_watchdog_host_only|native_rt_scalar_frame_aot_two_tasks)
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

- 2026-11-08 UTC: the portable CFG verifier source and test are Git blobs
  `98dcd686f4df7b7e8e11906d1a74da69b1a0492e` and
  `2ee535e649ad423611d9778e82be3b9430909678`. Local GCC and Clang
  Release/NDEBUG tests **PASSED**; GCC ASan/UBSan also **PASSED**. The tests
  include exhaustive 3-node graphs, malformed CFGs and resource ceilings.
- LLVM-backed call verifier source and test Git blobs are
  `0f6359bec88447aab7d1537400384440a926d7b7` and
  `2a78c98f0f4492f8b45f4711fe98496f18db65bc`.
  **LLVM-backed negative CTest NOT_EXECUTED**: local LLVM C++ SDK is absent.
- Exact-head LLVM AOT O0/O2/O3 guest execution **NOT_EXECUTED**. Source Actions
  jobs fail without usable executed steps. Do not treat admission failures,
  metadata-only jobs, pending or skipped jobs as successful evidence.
- The accessible private `numerics-atto/mirror-oreslang-source.llvm` default
  branch has a different `CMakeLists.txt` blob from source `main`. It cannot
  be used as exact-head evidence without full-tree mirroring and independent
  SHA verification.

**Do not merge this PR or enable untrusted general source-loop execution
until the remaining gates are green.**
