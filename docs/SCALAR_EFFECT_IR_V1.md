# Native LLVM scalar source → effect-IR v1 (model only)

The command `oreslang-llvmc --emit-scalar-effect-ir source.ores` runs
ordinary native source parsing and verified LLVM IR lowering **before**
emitting JSON instead of LLVM IR. Only a bounded, closed scalar subset is
admitted: at most 1024 zero-argument int-returning free functions, each
containing a single return expression of signed integer literals and
binary `+`, `-`, `*`. The exporter walks each admitted AST node,
emitting one `compute(units=1)` fact per literal or operator; the
total cannot exceed 100,000 operations and nesting cannot exceed 64.

It rejects imports/module namespaces, classes, actors, local variables,
parameters, calls, unary negation, ownership operators, annotations/effects,
throws, await, loops, non-scalar statements, malformed operands,
duplicates, invalid identifiers and unsupported CLI profiles. Neither
unrecognized constructs nor parser recovery may silently become zero-cost
work. All input gets compiled/validated before any effect JSON prints.

The result for `fnc answer(): int { return 40 + 2; }` is equal to the
`oreslang-static-analysis.rs/examples/rust-scalar-expected.json`
fixture (SHA-256 `8ee1adbf6faef704c1eb6d2b2075c3b2d723097037f646f9e6e518a69d005463`).

Verification:
- `native_scalar_effect_ir_v1_admission` CTest checks AST emission and
  negative edge cases without executing guest code.
- `native_scalar_effect_ir_source_compiler_v1` CTest parses actual
  source with the native CLI and verifies the exact golden JSON and
  fail-closed negatives.
- These checks **must execute** on an authorized CI runner before
  making a parity claim; a pending/failed runner startup does not prove them.

**Not proved:** full LLVM language coverage, that Oreslang source syntax is
identical across backends, timing, instruction counts, stack bounds,
authenticated compiler provenance, or hard-real-time certification.
JSON is a forgeable model, not a signed statement about an executable.
