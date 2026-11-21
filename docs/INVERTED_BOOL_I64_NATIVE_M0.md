# Restricted LLVM mixed bool/i64 inverted guest (M0)

This change is deliberately narrower than the intended complete AttoLang data-oriented programming ABI. It extends the original main-based two-i64 compiler prototype with a **second verified source profile**, exactly one local `define inverted struct` containing **two scalar fields in order: `bool`, then `i64`**. The canonical `.atto` file is still compiled by the default production `oreslang-llvmc`; there is no experimental parser or interpreter.

## Accepted source and guarantees

A single zero-argument `fnc main(): int` may create `InvertedArray<T>.with_capacity(1..256)`, then call literal-only `push(bool, i64)`, `set(index, field, correctly typed literal)`, `erase_swap(index)`, and return an **i64** field through `get`. Boolean source literals must be exactly `true` or `false`; numeric `0`/`1` do not implicitly convert. Immutable fields cannot be modified. Returning a bool field as int is rejected rather than implicitly converted.

The normal production parser constructs `Program.inverted_main`; the compiler resolves/verifies full operations through typed HIR and independently validated MIR. The new emitter allocates actual stack-local `[capacity x i1]` and `[capacity x i64]` columns, plus the existing i64 live-marker lane. It guards owner, logical length and live-row marker before row operations, traps out-of-bounds rows, swap-erases both typed lanes, and volatile-scrubs all cells on owner close. This is **dense-index** storage: the marker is *not* a stable/generational row handle. No guest pointer escapes.

The prior two-i64 emitter remains in place and retains its own regression suite. Mixed source is dispatched only when the nominal schema exactly matches bool/i64, never by inferencing or converting an i64.

## Tests and evidence

`native_inverted_bool_i64_guest.sh` requires the real default compiler, source-built Rust no-GC IR auditor, and Clang. It runs the two positive `.atto` cases under Clang O0/O2 (four expected process exit codes 42), a real out-of-bounds signal at both optimization levels, five negative source diagnostics that emit no LLVM IR, and a known collector rejection. No-GC audit binds exact pre/post SHA-256 of IR and auditor. The separate strict C++17 test validates typed MIR and emitted IR and rejects forged boolean payloads.

Run on an authorized source-org runner:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel 2 --target oreslang-llvmc oreslang-inverted-bool-i64-mir-test oreslang-no-gc-ir-audit-build
ctest --test-dir build --output-on-failure -R '^(native_inverted_bool_i64_source_to_native|inverted_bool_i64_mir_cpp17)$'
```

These tests are **authored, not qualified as guest passes** until actual stepful exact-head execution and independent receipt review; GitHub `steps=null` is RUNNER_NOT_ADMITTED. A manually authored LLVM IR example compiled by Clang locally at O0/O2 (exit 42) demonstrates syntax feasibility only, not compilation of this emitter or an AttoLang guest.

## Deliberate fail-closed exclusions

No `f64` source literals, general mixed-schema arity/order, generational handles, borrow expressions in `.atto` source, async/suspension, actors, SIMD, GPU, AoSoA/SoAoS conversion or native FFI/linked-image no-GC certification. Those require separate typed front-end, effect/ownership, ABI and native execution work. The existing schema may admit `f64` as compiler-only preflight, but this source-to-native slice **must reject it**.

Independent review and runner-admission repair are prerequisites to draft promotion or merging. No cross-org private-source transfer is authorized by this branch.
