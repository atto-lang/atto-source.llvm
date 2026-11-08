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

## Exact scope of frame integrity, untrusted tenant isolation

The v1 scalar continuation uses a **32-byte caller-owned native frame**.
A *trusted* carrier must zero-initialize it, retain exclusive ownership
for its entire lifetime, execute at most one step/cancel/result at a time,
and never expose its pointer or writable mapping to untrusted guest code.

The compiler now detects *malformed* frames before normal step/cancel/result:
nonaligned pointers (8-byte alignment), counter outside the admitted
source-loop bounds, lifetime fuel above the maximum, forged DONE with
counter below the upper bound, forged EXHAUSTED with nonzero fuel or
completed counter, and nonpristine reinitialization. The C11 native
harness exercises **27 fatal-trap scenarios**. A corrupt pointer can still
reference an invalid allocation; the caller must supply at least 32 bytes
of valid accessible memory. These checks are defense in depth for a
trusted carrier, not an authorization or memory-safety guarantee.

**A guest capable of arbitrary writes to the full native frame can still
zero all 32 bytes and call init again, or forge an in-range counter and
fuel value.** No in-frame magic or redundant checksum protects against
that attacker. A true untrusted tenant must use a host-resident, opaque
capability handle / quota ledger, with controlled revocation and ownership
checks, or memory isolation enforced outside the guest's writable address
space. A per-actor heap or scheduler pool alone is insufficient.

Until that broker/protection-domain mechanism and an executed inter-tenant
corruption test exist, **untrusted-tenant execution on this v1 ABI is
NOT_SUPPORTED and NOT_CERTIFIED**. Do not promote this experimental scalar
continuation as an untrusted-tenant runtime. A compiled guest AOT test
proves bounded execution for a trusted host-created frame only.

## Executed local evidence versus required hosted evidence

- Exact individually SHA-verified portable CFG files tested with GCC/Clang
  C++17 Release and GCC/Clang ASan+UBSan: **PASS**.
- New LLVM source changes (frame alignment, pristine init, state range
  checks, named struct layout) and all 27 native trap scenarios:
  **NOT_EXECUTED** against the generated guest. They need the LLVM SDK
  and a full compiler run at `-O0`, `-O2`, `-O3`.
- This container has Clang 17 but no `llvm-config` or LLVM development
  headers; attempts to refresh the Debian package index could not
  resolve the package host. Do not confuse installed `libLLVM19`
  runtime libraries with a buildable LLVM C++ SDK.
- Source GitHub Actions runner admission has repeatedly failed before
  job steps were allocated. Obtain a real nonzero-step exact-tree private
  runner receipt before considering approval/merge.

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

## Independent exact-head runner (new fail-closed gate)

`tests/native_rt_scalar_certify.py` is the one-command entrypoint for a
trusted, exclusive **full Git checkout** on an approved private runner.
Do not invoke it on a partial source export or a stale carrier mirror.
The operator must independently obtain the *full* exact commit SHA from
the current reviewed PR (not by blindly using local `git rev-parse HEAD`).

```sh
# From the root of a private, complete, clean checkout of the reviewed PR:
: "${APPROVED_PR_HEAD_SHA:?set exact SHA independently from GitHub PR}"
python3 tests/native_rt_scalar_certify.py \
  --repo "${PWD}" \
  --expected-head "${APPROVED_PR_HEAD_SHA}" \
  --llvm-config llvm-config-19 --clang clang-19 \
  --out "../llvm-scalar-cert-${APPROVED_PR_HEAD_SHA}"
```

Use the LLVM and Clang majors actually installed on the approved runner;
the example uses 19. The script **does not clone, mirror, or upload**
source. It verifies every tracked worktree blob against the exact commit,
including executable bits and symlink targets. It rejects untracked/dirty
files, unsupported submodule entries, `assume-unchanged` content mismatch,
missing LLVM CMake SDK, and major-version-mismatched Clang.

It configures/builds the compiler and native test targets and requires
all eight mandatory CTest cases (six core and two integrity gates) to appear in the executed JUnit report.
Any missing, skipped, failed or duplicate case fails certification.
Crucially it verifies the complete tree and SHA **again after execution**,
to detect intervening source mutations. The isolated runner must exclude
concurrent source writers even though pre/post checks defend against
accidental checkout races.

All generated output is outside the source checkout and includes
`receipt.json`, build/configure/CTest logs, JUnit XML, toolchain versions,
commit/tree SHA, per-blob count, and SHA-256 hashes of native binaries
and evidence files. Only a full positive result can carry
`EXACT_HEAD_EXECUTED_PASS`. A preflight failure, missing toolchain,
zero-step runner failure or absent guest execution never carries that label.

The 14 independent negative/preflight tests are registered as
`native_rt_scalar_exact_tree_preflight_negatives` in CTest on Unix.
They exercise clean Git fixtures, forged SHAs, malicious symlinks, altered
executable modes, untracked changes, `assume-unchanged`, submodule/dirty
checkouts, missing/duplicate JUnit cases, LLVM-Clang mismatch, and ensuring
child Python imports cannot introduce source-tree `__pycache__` dirt.

The separate `native_rt_scalar_synthetic_certification_mechanics` test
creates **disposable fake CMake targets and fake LLVM/Clang version probes**
to exercise the entire certificate machinery on an isolated Git fixture.
It proves that eight CTest/JUnit cases can be genuinely discovered and recorded,
that their logs are SHA-256 hashed, and that a test modifying the source
checkout causes mandatory post-execution denial. This is strictly a
**synthetic wrapper-mechanics test**: it does not contain AttoLang source,
the LLVM SDK, or compiler-generated guest code and is **NOT AOT CERTIFICATION**.
CTest accepts a standard grouping regex `^(foo|bar)# LLVM compiler metering verification — draft release gate

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

## Exact scope of frame integrity, untrusted tenant isolation

The v1 scalar continuation uses a **32-byte caller-owned native frame**.
A *trusted* carrier must zero-initialize it, retain exclusive ownership
for its entire lifetime, execute at most one step/cancel/result at a time,
and never expose its pointer or writable mapping to untrusted guest code.

The compiler now detects *malformed* frames before normal step/cancel/result:
nonaligned pointers (8-byte alignment), counter outside the admitted
source-loop bounds, lifetime fuel above the maximum, forged DONE with
counter below the upper bound, forged EXHAUSTED with nonzero fuel or
completed counter, and nonpristine reinitialization. The C11 native
harness exercises **27 fatal-trap scenarios**. A corrupt pointer can still
reference an invalid allocation; the caller must supply at least 32 bytes
of valid accessible memory. These checks are defense in depth for a
trusted carrier, not an authorization or memory-safety guarantee.

**A guest capable of arbitrary writes to the full native frame can still
zero all 32 bytes and call init again, or forge an in-range counter and
fuel value.** No in-frame magic or redundant checksum protects against
that attacker. A true untrusted tenant must use a host-resident, opaque
capability handle / quota ledger, with controlled revocation and ownership
checks, or memory isolation enforced outside the guest's writable address
space. A per-actor heap or scheduler pool alone is insufficient.

Until that broker/protection-domain mechanism and an executed inter-tenant
corruption test exist, **untrusted-tenant execution on this v1 ABI is
NOT_SUPPORTED and NOT_CERTIFIED**. Do not promote this experimental scalar
continuation as an untrusted-tenant runtime. A compiled guest AOT test
proves bounded execution for a trusted host-created frame only.

## Executed local evidence versus required hosted evidence

- Exact individually SHA-verified portable CFG files tested with GCC/Clang
  C++17 Release and GCC/Clang ASan+UBSan: **PASS**.
- New LLVM source changes (frame alignment, pristine init, state range
  checks, named struct layout) and all 27 native trap scenarios:
  **NOT_EXECUTED** against the generated guest. They need the LLVM SDK
  and a full compiler run at `-O0`, `-O2`, `-O3`.
- This container has Clang 17 but no `llvm-config` or LLVM development
  headers; attempts to refresh the Debian package index could not
  resolve the package host. Do not confuse installed `libLLVM19`
  runtime libraries with a buildable LLVM C++ SDK.
- Source GitHub Actions runner admission has repeatedly failed before
  job steps were allocated. Obtain a real nonzero-step exact-tree private
  runner receipt before considering approval/merge.

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

## Independent exact-head runner (new fail-closed gate)

`tests/native_rt_scalar_certify.py` is the one-command entrypoint for a
trusted, exclusive **full Git checkout** on an approved private runner.
Do not invoke it on a partial source export or a stale carrier mirror.
The operator must independently obtain the *full* exact commit SHA from
the current reviewed PR (not by blindly using local `git rev-parse HEAD`).

```sh
# From the root of a private, complete, clean checkout of the reviewed PR:
: "${APPROVED_PR_HEAD_SHA:?set exact SHA independently from GitHub PR}"
python3 tests/native_rt_scalar_certify.py \
  --repo "${PWD}" \
  --expected-head "${APPROVED_PR_HEAD_SHA}" \
  --llvm-config llvm-config-19 --clang clang-19 \
  --out "../llvm-scalar-cert-${APPROVED_PR_HEAD_SHA}"
```

Use the LLVM and Clang majors actually installed on the approved runner;
the example uses 19. The script **does not clone, mirror, or upload**
source. It verifies every tracked worktree blob against the exact commit,
including executable bits and symlink targets. It rejects untracked/dirty
files, unsupported submodule entries, `assume-unchanged` content mismatch,
missing LLVM CMake SDK, and major-version-mismatched Clang.

It configures/builds the compiler and native test targets and requires
all eight mandatory CTest cases (six core and two integrity gates) to appear in the executed JUnit report.
Any missing, skipped, failed or duplicate case fails certification.
Crucially it verifies the complete tree and SHA **again after execution**,
to detect intervening source mutations. The isolated runner must exclude
concurrent source writers even though pre/post checks defend against
accidental checkout races.

All generated output is outside the source checkout and includes
`receipt.json`, build/configure/CTest logs, JUnit XML, toolchain versions,
commit/tree SHA, per-blob count, and SHA-256 hashes of native binaries
and evidence files. Only a full positive result can carry
`EXACT_HEAD_EXECUTED_PASS`. A preflight failure, missing toolchain,
zero-step runner failure or absent guest execution never carries that label.

; a noncapturing
`(?:...)` pattern returned zero tests and was caught by the fail-closed
JUnit check during development. The production wrapper now uses the
CTest-supported syntax.

## Required full compiler and compiled guest gate

```sh
cmake -S . -B build-llvm -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-llvm --target oreslang-llvmc \
  oreslang-native-metered-cfg-verifier-test \
  oreslang-native-reduction-quantum-test \
  oreslang-native-scalar-call-verifier-test
ctest --test-dir build-llvm --output-on-failure \
  -R '^(native_metered_cfg_cycles_fail_closed|native_scalar_guest_calls_fail_closed|native_scalar_ir_surface_scan|native_scalar_ir_surface_scan_python_optimized|native_reduction_quantum_watchdog_host_only|native_rt_scalar_frame_aot_two_tasks)$'
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

## Matching LLVM SDK and Clang toolchain

The scalar AOT CTest now requires `--llvm-major "${LLVM_VERSION_MAJOR}"` and
prefers `clang-${LLVM_VERSION_MAJOR}` over a generic PATH `clang`.
Before running the source compiler or linking any generated IR,
`native_rt_scalar_step_aot.py` checks `clang -dumpversion` and rejects
major-version mismatches, invalid output, and failed probes. This prevents
calling a run against an unrelated Clang version a compiler AOT proof.

The portable `native_scalar_ir_surface_scan` regression also checks
positive/mismatched version probes and malformed version output, including
execution with `PYTHONOPTIMIZE=1`.

This rule is intentionally stricter than LLVM's general cross-version IR
compatibility. If your compiler SDK and Clang differ, provision a
matching toolchain: do **not** weaken the test, change the expected major,
or mark the run certified.

## Portable IR scanner verification

The emitted IR scanner rejects missing/extra v1 frame functions, imports
other than `llvm.trap`, and unverified direct/indirect/inline-assembly/
invoke/callbr transfers. CTest runs the negative fixtures normally and with
`PYTHONOPTIMIZE=1`. The full scalar AOT harness aborts if Python optimization
would disable its legacy end-to-end assertions. These are *scanner* proofs,
not compiled guest execution proofs.

## 2026-11-08 audit: frame ABI, verifier and portable evidence

Two source-level hazards were addressed:

- The scalar v1 entrypoint now takes a pointer to the actual four-field frame
  type, not an `i64*` that is incompatible with typed-pointer LLVM struct
  field GEPs. On opaque-pointer LLVM the external pointer ABI remains `ptr`.
- `ScalarGuestCallVerifier.hpp` now requires exactly four v1 definitions
  with correct returns, arity, parameter types, linkage and C calling
  convention. Frame pointer types must agree across methods; globals,
  aliases, ifuncs, unapproved imports, indirect calls and inline assembly
  remain rejected. C++ negative fixtures include wrong signatures, linkage,
  calling conventions and extraneous globals.

The source compiler does **not** yet have certified general actor loops,
recursion, FFI, borrow/critical-safe resumption, or tenant OS isolation.
A caller that can mutate raw frame memory can bypass the quota; the C ABI
is only meaningful inside a trusted, exclusive owner/protection boundary.

### Verified individual Git blobs (not a verified full source tree)

- `ScalarLoopLowering.cpp`: `bebeb66931d0745d94aa028b95df4d581e6c222a`
- `ScalarGuestCallVerifier.hpp`: `4308dc2333a7a835f1edf08117f1d154ae03d7b4`
- `native_scalar_guest_call_verifier_test.cpp`: `2523edace8bc03f8df34fa9e8f36b51756f3040f`
- `native_rt_scalar_step_aot.py`: `5196a13c1849b505f91bd4c21a1ac512dcc56d41`
- `native_scalar_ir_surface_test.py`: `7f4fca8eeeae5e992aec19c587f85ee672f5a6a0`
- Portable CFG verifier header/test: `98dcd686f4df7b7e8e11906d1a74da69b1a0492e`
  and `2ee535e649ad423611d9778e82be3b9430909678`

### Executed portable checks

- GCC 14 and Clang 17 at `-O3 -DNDEBUG`: **PASS**.
- The complete local portable verification receipt is stored with this
  ChatGPT run; its scope is individual Git blobs, not a checked-out full tree.
  GCC/Clang `-O3`, GCC/Clang ASan+UBSan and both Python scanner modes all pass.
- GCC and Clang ASan/UBSan in a clean container shell: **PASS**.
- 3,584 exhaustive three-node directed CFG/meter cases, resource limits and
  malformed edges: **PASS**.
- Python normal and `PYTHONOPTIMIZE=1` IR-surface negative checks, including
  wrong return types, arity, pointer arguments, unverified calls, incorrect
  toolchain versions, and a correct typed struct-pointer positive: **PASS**.
- These tests were executed using the exact individual Python/C++ blobs
  identified above, not a full source checkout of the PR head.

### Mandatory unexecuted gates

- **NOT_EXECUTED:** full compiler with LLVM C++ SDK,
  `native_scalar_guest_calls_fail_closed` CTest and source->generated native
  scalar guest execution under `-O0`, `-O2`, and `-O3`.
- **NOT_CERTIFIED:** private-runner exact-tree mirroring, token-free
  independent blob/provenance validation, and guest security isolation.
- Source-org workflow job failures with `steps=[]` are admission failures,
  not evidence of a failed or passing guest test.

**Keep PR #1571 draft, with no merge/security-certification claim.**
