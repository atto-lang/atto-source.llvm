# Experimental source → common HIR v1 (LLVM frontend)

**Non-production opt-in output.** The `oreslang-llvmc` executable supports
`--experimental-emit-hir-v1`, which invokes the native **real**
`parse_program` and serializes a strictly bounded scalar AST subset to
`ores-hir/1.0.0-experimental`. Default native LLVM IR generation remains
unchanged; the exporter intentionally bypasses normal code generation.

```sh
oreslang-llvmc --experimental-emit-hir-v1 example.ores > llvm-hir.json
```

This version admits a root-level set of non-void i64 functions with value
parameters, a final return expression, i64 literals, identifiers,
addition/subtraction/multiplication and positional direct calls.
Named arguments, ownership, actor/class declarations, effects, array/FFI
operations, loops, early returns, mutable local statements, explicit target
overrides and other unsupported features **fail closed**.

Source and JSON output each have a 1 MiB limit. All identifier names are
restricted to ASCII with strict length bounds to avoid JSON injection.
Unannotated native `int` is **only experimentally** mapped to the
common `i64` contract; checked host-native codegen is independently exercised in the opt-in profile;
this export alone is not evidence of AOT target or runtime parity.

A standalone test requires no LLVM link:

```sh
c++ -std=c++20 -Isrc/main/cpp tests/experimental_hir_v1_exporter_test.cpp -o /tmp/experimental-hir-v1-exporter
/tmp/experimental-hir-v1-exporter
```

For real exact-source comparison, use the shared
`oreslang-common-source/scripts/experimental_hir_v1_source_parity.py`
harness after **both** compilers are built. The common contract must verify
both outputs independently. The resulting comparison is HIR-equivalence
evidence **only**, not execution or runtime parity.

Any GitHub Actions run without executed steps must remain blocked.

### Checked-i64 source-unit execution (new opt-in)

The experimental **host-only** checked integer mode is selected by the
exact first source line (LF terminator):

```oreslang
// oreslang:checked-i64-v1
pub fnc scale(int count): int { return count * 43; }
pub fnc main(): int { return scale(1); }
```

Marked source executes signed-64-bit addition, subtraction, multiplication
and unary negation with overflow detection. Java/GraalVM uses
`Math.*Exact`; LLVM uses `llvm.sadd/ssub/smul.with.overflow.i64`, with
checked negation lowered as zero-minus-operand. LLVM's experimental host
failure path prints `ORES_I64_OVERFLOW` and exits with status 86; the
GraalVM guest raises a failure carrying the same diagnostic marker. This
is **not yet a unified guest `Error`/supervisor ABI**.

Unmarked source units preserve ordinary runtime arithmetic, and their HIR
exporters continue to reject expressions such as `count * 43` unless a
conservative whole-domain interval proof establishes overflow freedom.
Marked source can emit the same expression as `checked-i64.v1` only because
its runtime now actually detects overflow. This opt-in does not enable
actor-wide, embedded, Wasm or GraalVM native-image AOT checked arithmetic.

The shared [experimental checked-i64 execution contract](https://github.com/ores-truffle-oreslang/oreslang-common-source/blob/feat/experimental-hir-mir-v1-20261014/docs/EXPERIMENTAL_CHECKED_I64_V1.md)
defines the precise source directive, tested boundaries and remaining
production error-policy limitations. Real compiler tests include safe
arithmetic, runtime overflow, legacy wrapping, identical frontend HIR and
reference MIR checks.
