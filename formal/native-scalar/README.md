# Native LLVM source-to-SMT scalar proof pilot

This is a **new source-linked native frontend adapter**, intentionally restricted
to the signed-i64 scalar min/max/clamp contracts. It starts with the native
Oreslang `parse_program()` entrypoint, which performs parser and
`validate_i64_ownership` admission. It does **not** pretend that the current
native subset supports the full Java/Graal stdlib `core.ores` grammar.
The checked `tests/fixtures/formal-core-scalar-checked.ores` instead spells
equivalent contracts using LLVM's guarded-return syntax.

The experimental `NativeScalarSmt` adapter admits only public i64 functions
with exactly 2 or 3 unique by-value i64 parameters, ordered pure
parameter-comparison return guards, a final direct parameter return, and no
owned locals, statements, actor operations, declared effects, pending
suspension, generic parameters, class objects or other unsupported features.
It rejects unsupported operations and calls before constructing a proof.
A per-guard bound and pre-concatenation SMT size limit defend against
unbounded expansion. Because this adapter calls `parse_program` directly
rather than the production launcher's outer UTF-8 preflight, it also restricts
this initial proof subset to **ASCII-only source** and rejects NUL/non-ASCII
bytes before native parser admission. This is a conservative pilot limitation,
not a proposal to prohibit Unicode throughout Oreslang.

The exporter constructs its implementation BV64 expression directly from
the checked native AST and compares that against an **independent** mathematical
contract for `min_int`, `max_int`, and `clamp_int` (including
the current reversed-bounds behavior). Each valid proof requires exactly
`unsat` then `sat` (a deliberately incorrect contract variant).
Two source mutation fixtures (min comparator and inverted clamp branch) must
each yield `sat` then `sat`.

## What is and is not verified

The exact-head Actions job compiles **real `Parser.cpp` and
`OwnershipChecker.cpp`**, runs the native fixture through both, generates
SMT and requires the external Z3 solver's expected results. It records source
Git blob, compiler version, Z3 version/hash, exact commit and raw SMT/solver
outputs for replay. It does not require LLVM code generation for this
bootstrap proof-exporter gate.

**These obligations do not establish** that LLVM optimization or generated
machine code preserves the AST, that Java and LLVM compiled output is
equivalent, that all native code is checked, or that actor/ownership memory
safety and GC contracts are proven. They also do not prove the type or
ownership checkers correct. No AOT parity claim is supported by this model.

The native source shape is **different** from Java core's source shape:
any actual source equivalence requires a separately reviewed semantic
bridge or executable differential tests. Solver output alone cannot
establish it.

## Exact-head promotion gates

1. A real CI runner must compile the C++17 native parser/ownership sources and
   execute the pilot with zero unsupported skips; the job fails on compiler
   errors, missing solver, solver `unknown`, missing mutants, and mismatched
   results.
2. Independently review the SMT/AST translation and the assumption that
   native `int` uses signed-i64 ordering.
3. Execute both Java/Graal and LLVM AOT binaries on the same integer corpus
   (`INT64_MIN/MAX`, equal values, inverted clamp bounds and permutations)
   and compare outputs and exceptional paths.
4. Version the shared proof IR and source identity, then attach evidence at
   the same backend source SHA, not just matching mathematical contracts.
5. Pin a reviewed compiler/solver distribution or trace their digests in a
   reproducible build. Current tool versions/digests are **recorded**, not
   hermetically pinned.

Repository Actions currently show pre-runner failures in related Oreslang
repos. Until an actual run records executed steps and proof transcripts,
**this is a draft implementation, not verified native proof evidence**.

Related: [Java checked-AST draft #1885](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/1885),
[common-source proof prototype #126](https://github.com/ores-truffle-oreslang/oreslang-common-source/pull/126),
[cross-backend program #150](https://github.com/ores-truffle-oreslang/oreslang-platform-conformance/issues/150).
