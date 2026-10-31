# Verified MIR to native LLVM IR: strict local two-i64 slice (2026-10-30)

## Implemented

`src/main/cpp/oreslang/types/InvertedMirLlvmI64.hpp` adds a **C++17**, LLVM-SDK-independent emitter from the already-validated `dop::mir::Module` into actual LLVM IR for precisely two local `i64` columns. This is a distinct executable IR lowering step beyond the earlier host `InvertedColumns` MIR interpreter.

The IR allocates two physically separate fixed-size local i64 columns, a live-slot marker column, owner-open state and length. It emits checked growth, fresh dense-index lookup, bounds comparisons, live-slot comparisons, owner-open guards and `llvm.trap()` failure paths. Supported operations: push, read, checked write, snapshot, swap-delete and close; borrowing is proven by the verified straight-line MIR and has **no escaping reference**, so checked loan operations generate no guest pointers. The local store is bounded to 1–256 rows. The generated main returns the last loaded i64 truncated to i32, a narrow test ABI only.

**Explicit exclusion:** this implementation does **not** expose external row handles or generational references. The live-slot marker is *not* a stale-handle generation check. There is no actor heap, task pinning, dynamic userland array, production `oreslang-llvmc` entrypoint, cross-backend parity, hard-real-time guarantee, or general purpose Oreslang MIR→LLVM backend codegen admission. Normal compiler source admission remains fail-closed.

## Exact locally executed evidence

The committed header's Git blob hash `1a4cc742a6d3bdc31c7e48eb8712d74ee8fa8196` was confirmed byte-for-byte identical to the locally tested file. The test driver Git blob is `eb5cc1edd0f8986b86b6b4fcb56a58602e9b07a9`. The final guard-asserting shell test blob is `6e54a1b07c181c1ec4250e30277d6e95e1886b0f`.

In a local isolated build, GCC 14.2 and Clang 17 independently compiled the **actual new C++17 emitter** with `-Wall -Wextra -Wpedantic -Werror`, linked its emitted LLVM IR with Clang 17 on Linux x86-64 and ran guests:
- Positive two-row append / mutable write / read loan and release / swap-delete / row read: native exit **42**, once for each C++ compiler.
- Invalid row access: emitted native `llvm.trap`, process exit **132** on this Linux host.
- Immutable-field write and missing typed push operands: rejected before outputting IR.
- The check script scans the generated IR for unsigned bounds guards, owner-live loads, live-slot checks and `llvm.trap`.

**Crucial limitation:** the local build used parser/HIR/MIR **stand-in interfaces** to compile the header, not an exact checkout of all of the private repo's original dependencies. Thus this is real LLVM IR/native executable evidence **for the exact newly authored emitter header**, but not an exact-head full-repo build, actual checked native owner CTest, or `oreslang-llvmc` execution. A GCC sanitizer build did not finish within the local execution limit; ASan/UBSan is NOT_EXECUTED.

The full source-org Actions runner continues to fail before executing steps; do not treat the job failures as test results.

## Checked-in CTest

The standalone and full CMake configurations register `inverted_verified_mir_ir_native_guest`. It builds `tests/native_inverted_mir_llvm_guest.cpp` under **strict C++17** and runs `tests/native_inverted_mir_ir_guest.sh`, which checks IR guards and launches the generated binary through Clang.

For approved build environments:

```bash
cmake -S tests/inverted-i64-standalone -B build/inverted-i64-standalone -DCMAKE_CXX_COMPILER=clang++ -DBUILD_TESTING=ON
cmake --build build/inverted-i64-standalone --parallel 2
ctest --test-dir build/inverted-i64-standalone --output-on-failure -R inverted_verified_mir_ir_native_guest
```

CI must execute the *exact private source Git tree* and report stepful GCC/Clang and ASan/UBSan receipts before this can be promoted as full repository evidence.

## Next hard gates

1. Obtain exact-head full-repo C++17 compile/CTest and fix incompatibilities against the real Parser/HIR/MIR headers; then build full `oreslang-llvmc`.
2. Have the production compiler emit verified MIR operands from actual `.ores` AST and lower those MIR operations via backend-owned LLVM IR; add true runtime row handle validity and generation checks before enabling them.
3. Add CFG loan joins, checked owner-drop and task pinning/actor quiescence, arena allocation policy and cross-backend executable fixtures. Keep normal inverted source admission fail-closed until these proofs exist.
