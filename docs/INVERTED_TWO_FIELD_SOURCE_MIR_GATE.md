# Two-i64-field .ores source through verified MIR — staging and safety gate

## What changed

The opt-in `ExperimentalInvertedI64.hpp` frontend now routes **exactly two** signed-i64 columns through the same `InvertedStructDecl` → `dop::hir::LinearPlan` → independently verified `dop::mir::Module` → `InvertedMirLlvmI64.hpp` LLVM IR emitter.

The frontend preserves original row insertion operands and field writes in source order, validates immutable fields in typed HIR, then returns **MIR-owned LLVM IR**. This removes the separate handwritten IR path for the two-i64 positive fixtures while leaving the older one- and 3..8-column experimental IR path intact. The CLI `--experimental-inverted-i64` remains required. Normal production source compilation still rejects inverted declarations before code generation.

The MIR emitter remains a bounded, **stack-local** implementation: two i64 data columns, one live-row marker column, length and owner-open flag. It emits row/capacity guards, owner-live guards, trap paths and dense swap deletion. Its live-row marker is NOT an externally usable generation-checked row handle. No pointer/loan escapes, actor allocation, task suspension, nested records or general `InvertedArray<T>` are enabled.

## Close hardening

The experimental MIR `Close` lowering now revokes `%alive` and uses **volatile stores** to scrub all bytes represented as i64 data cells in both local columns and the live-row marker, then clears length. The maximum declared capacity is 256 and every scrub is statically bounded. These volatile stores are intended to prevent compiler dead-store elimination of the erasure; they do not claim protection against physical memory remnants outside this function or substitute for the full runtime's owner lifetime proof.

`tests/native_inverted_mir_ir_guest.sh` checks that Clang -O2 can link and execute the closure program returning 42 and asserts the exact number of emitted volatile erasures for the two-slot test. Closing with outstanding field loans remains rejected by MIR.

## Required exact-head guest test

The existing guest test now requires that both `positive-column-get.ores` and `positive-column-bounded.ores` produce the typed MIR header, three physical arrays (two data + one liveness), and explicit owner-live checks. It executes each native guest expecting exit 42. Two separate bounds fixtures must compile to native IR and then trap: the original one-column fallback and `negative-two-column-out-of-bounds.ores` through the new MIR path. An added negative two-column fixture checks immutable `id` write rejection.

Commands from the repository root in an authorized environment:

```sh
cmake -S tests/inverted-i64-standalone -B build/inverted-i64-standalone -DCMAKE_CXX_COMPILER=clang++ -DBUILD_TESTING=ON
cmake --build build/inverted-i64-standalone --parallel 2
ctest --test-dir build/inverted-i64-standalone --output-on-failure
```

Repeat with GCC and with ASan/UBSan. A full `oreslang-llvmc` build and production fail-closed tests are also required.

## Evidence and restrictions

All above source/fixture updates have been committed to LLVM draft PR #1213. Static current-source anchors and Git blobs were cross-checked against GitHub. **The new HEAD has not been compiled or executed against the full private repo**. Earlier isolated x86-64 native exit-42 results belong to a previous MIR emitter blob; their results do NOT carry forward to this code change. The conformance experiment in PR #249 pins the new blobs under `current_branch_mir_llvm_codegen` with `NOT_EXECUTED` statuses.

Source-org GitHub Actions has repeatedly failed with zero executed job steps. Treat that as an admission/billing/runner blocker, NOT as a compiler regression test result. No cross-organization private source mirror, merge, production activation, Java parity claim or hard-real-time certificate is authorized by this change.
