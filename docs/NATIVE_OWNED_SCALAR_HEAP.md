# LLVM native scalar regions: experimental ownership fixture

**Status:** Test-only LLVM feature behind `--experimental-owned-int`. It is **not**
part of the public Oreslang language grammar. In Java/Graal and LLVM,
ordinary `int` is `Copy`; a variable or routine may be called `owned`.
Non-Copy values require the language's `rt borrow` / `rt take` / `rt copy`
contracts, which are **not** implemented by this scalar allocator.

## Normal source (no forced heap allocation)

```ores
pub fnc main(): int {
  const owned: int = 6;
  return owned * 7;
}
```

Ordinary scalar code uses LLVM register/stack storage and lifetime hints.
The compiler does not declare `oreslang_owned_i64_region_*` ABI functions
for programs with no explicitly experimental region-backed locals. The
`native_owned_identifier` and `native_owned_lexical_lifetime` CTests
guard this. The parser rejects `const x: owned int = 6;` by default with
an explicit opt-in diagnostic and emits **no IR** on rejection.

## Experimental test invocation

```sh
./build/oreslang-llvmc --experimental-owned-int tests/fixtures/owned-native-region-main.ores
```

Only this opt-in compiler mode supports `owned int`, a native bounded
function-activation region using `oreslang_owned_i64_region_open/new/drop/close`.
It does **not** make `owned` a keyword in the shared language. Each
initialized experimental slot must drop exactly once on each supported
normal/guarded return path; the region must close before function exit.
The native ABI fail-closes on invalid slot/region/provenance state.

This is not an actor heap, a source-level general object model, a
tracing collector, a proof of zero allocation or of hard real-time bounds.
`calloc/free` and platform allocators can still block. Compiler-emitted
source actors have a separate bounded i64 field ABI. Guest `await`,
cancellation, exception unwind, FFI lease quiescence and arbitrary
graph/class destructors are **not** covered by this region fixture.

## Equality token parity

Java/Graal accepts both `eq` and one contiguous `==` token. LLVM now
admits the same alternatives in the bounded guarded-return subset, while
rejecting spaced `= =` and `===` as malformed syntax. These cases execute
in the generated stress corpus; acceptance is not inferred from lexical
text matching alone.

## Verification and the old PR stack

The mainline already implements default opt-in gating, accepted ordinary
`owned` identifiers, lazy scalar region declarations and guarded cleanup.
The stale stacked ownership branch #72 must not overwrite current actor,
WASM, class ownership, or scheduler code. Its useful generated ownership
stress test has instead been ported independently.

`native_owned_scalar_cfg_generated_stress` is a deterministic 77-case
compiler→LLVM IR→Clang native-execution and fail-closed matrix: ordinary
scalar/identifier checks, seeded 1–8-local guarded-return programs,
experimental region allocation and per-edge drop/close counts, and
negative syntax. It checks that neither GC intrinsics nor an unwanted
heap ABI enter ordinary source. **Registering the test is not evidence
that it ran**: require exact-head full CMake/CTest, Clang and sanitizers
before merging or claiming regression certification.
