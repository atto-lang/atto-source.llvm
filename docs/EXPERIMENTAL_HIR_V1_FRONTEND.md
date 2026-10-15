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
common `i64` contract; this export does *not* prove LLVM codegen implements
checked-overflow semantics or has native AOT parity.

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


### Checked i64 admission (audit update)

**Do not equate the experimental checked-i64 IR contract with the production
integer runtime's wraparound behavior.** The actual Java/GraalVM and LLVM
backends currently perform ordinary signed 64-bit addition/subtraction/
multiplication without overflow traps. Both experimental exporters therefore
run a conservative interval proof over each function's *entire* signed-i64
parameter domain. A possible overflow, including `count * 43`, is rejected
before exporting `checked-i64.v1`. Literals and genuinely safe operations
such as `count * 1` and `40 + 3` remain admissible.

The test fixture `fixtures/ir-v1/checked-safe-counter.ores` is intended for
source-to-HIR parity. A passing guest result on this fixture is a **smoke test**
and not evidence that production runtimes trap overflow. Remove this
restriction only after both backends implement verified checked arithmetic,
or introduce a separate explicitly negotiated wrapping-i64 IR contract.
