# Typed SoA HIR and native checked-owner milestone — 2026-10-30

## Implemented on draft PR #1213

The production `InvertedStructDecl` AST is now consumed by `src/main/cpp/oreslang/types/InvertedTypedPlan.hpp`, a bounded, compiler-owned **linear HIR admission layer**. The production parser's `resolve_and_validate_program` constructs validated one-level layouts, checks nominal collisions with classes/actors/type-only declarations, and **still explicitly rejects guest codegen**. The experimental i64 guest adapter remains separate. This work does not pretend that creating a HIR plan authorizes native compiler lowering.

Each layout records a nominal name, ordered immediate columns, `I64`/`Bool`/`F64` kind, width, packed-row offset and the default-immutable `val` versus explicit mutable `let` permission. One-level physical columns are never inferred from ordinary `Array<T>`. A row must have 1..64 fields and fit in a 64 KiB packed record budget; a proposed owner capacity must be positive and its payload layout fit in 16 MiB. The latter is a **source-admission cap**, not an allocation or real-time guarantee (allocator metadata overhead is not counted).

A straight-line `LinearPlan` records checked symbolic operations for allocation, insertion, field read/write, field read/write borrow and release, swap deletion, detached snapshot and close. It enforces:
- immutable columns cannot receive post-insertion field writes or mutable loans
- same-field mutable/read conflicts are rejected while disjoint fields may be borrowed separately
- structural growth, erase and close are rejected while **any** loan is active
- bounds and capacity are checked, and unmatched releases rejected
- even loan-free owner suspension remains rejected until pinned actor/task lifetime is proved
- close revokes further symbolic operations
- the compiler-owned operation stream is capped at 4096 operations with failure-atomic rejection on exhaustion

**Critical limits:** this is a straight-line compile-time operation-admission component, **not** complete control-flow borrow analysis, task/actor pinning, runtime handle minting, HIR→MIR generation, LLVM IR lowering, hard-real-time allocation proof, or compiler type-checking of arbitrary source expression values. Row-handle issuance, generation rollover, storage scrubbing and checked runtime loans are implemented in the separate `DopInvertedColumns.hpp` C++ native host layer and exercised independently in the new test; they are NOT yet exposed as guest methods.

## Tests and CI

New C++20 test `tests/native_inverted_typed_plan_test.cpp` covers layout widths/offsets, per-field mutability, field alias conflicts, loan cleanup, row bounds, capacity exhaustion, failed structural mutation, no suspension, closed-owner failures and **actual** `DopInvertedColumns` native row generation/slot reuse/foreign handle/loan behavior. It is registered as:
- `inverted_typed_hir_and_native_owner` under `tests/inverted-i64-standalone/CMakeLists.txt` (does NOT require LLVM development libraries).
- `inverted_typed_hir_and_checked_owner` under full root CMake (requires the full LLVM SDK due to global `find_package(LLVM)`).

The exact-head `.github/workflows/inverted-layout-typed-hir-exact-head.yml` provides Ubuntu GCC and Clang C++20 builds, normal and ASan/UBSan jobs, read-only `GITHUB_TOKEN`, no cross-org source transfer, **SHA-pinned checkout** and `git rev-parse HEAD` verification. It executes the standalone native HIR test plus exact experimental source adapter `.ores`→IR→Clang guest cases. CI results must report actual step execution; zero-step failure means **NOT_EXECUTED**.

Run locally when the repo files are present:

```sh
cmake -S tests/inverted-i64-standalone -B build/inverted-i64-standalone -DCMAKE_CXX_COMPILER=g++ -DBUILD_TESTING=ON
cmake --build build/inverted-i64-standalone --parallel
ctest --test-dir build/inverted-i64-standalone --output-on-failure --timeout 180
```

Full production parser test and true compiler integration additionally require `LLVMConfig.cmake` and complete `oreslang-llvmc` compilation.

## Verification boundaries

Current work was inspected by fetching exact GitHub files and checking the expected integration and test anchors. **Do not label this as compiled or executed C++20 HIR / full compiler testing until a stepful run exists.** The prior isolated experimental i64 guest execution evidence remains scoped to its exact earlier header/fixture blobs and does not prove this new typed HIR component.

Next implementation dependency: lower typed operations into explicit backend MIR owner/column primitives with generation-checked row handles, restrict branch/control-flow effects with dataflow joins, and run full exact-head compiler and native sanitizer tests. Extend Java next, then Wasm/.NET/Forge with shared executable fixtures, never with host-only model substitutes.

### Real native-host differential gate (staged, not executed)

The C++20 test additionally performs the **same** admitted push, field-write/read, two disjoint field loans, loan release, swap-delete and stale-handle checks against an actual `InvertedColumns<std::int64_t, std::int64_t>` owner. A blocked structural mutation is checked in both the typed plan and the native owner. It also verifies that the 4096-op compiler budget cannot publish a partial extra operation.

This host differential **does not** execute source-language generated LLVM IR, and the typed IR operation sequence is not yet a lowered MIR instruction stream. For this reason the test may only advance the *host HIR/native owner* gate when actually executed, never the production guest parity gate. Full compiler and GitHub Actions evidence remain pending.

### Experimental .ores-to-LLVM-IR path now consumes the same typed HIR admission

The flagged `ExperimentalInvertedI64.hpp` adapter now constructs an `InvertedStructDecl` from its parsed guest source, then calls `dop::hir::Layout::from_decl` and `LinearPlan` before emitting physical LLVM IR. Positional `push`, field `set` and final `get` pass through this **same compiler-owned type/field and operation checker**, rather than independent handwritten field mutability rules. The adapter still emits explicit bounds trap instructions for runtime indexes; its HIR records `read_runtime_checked` and `write_runtime_checked` obligations rather than proving indexes with compile-time row counts. This is an **experimental** HIR→IR bridge, not production MIR or actor-owned lowering.

The last locally executed experimental adapter was Git blob `43fb26ec69974d8148b9d4142d738f17bbbefa14`. Its historical local native exit-42 proof **does not automatically apply** to the newly integrated source adapter blob `d8c16d7057de9d3292a72423147e27db492d3c23` or current typed plan blob `e04b19c37460a5b859d7ad2497583d296cb395b2`; both are unexecuted at this newer head. The shared conformance experiment manifest records the distinction with explicit current-head `NOT_EXECUTED` statuses.

Proof still required: stepful exact-head C++20 build of the adapter and typed plan, `.ores` compilation through the flagged adapter and actual `oreslang-llvmc`, native guest exits, UBSan/ASan, production no-flag denial, and a codegen-level operation verifier proving all runtime-checked HIR indexes become guards.
