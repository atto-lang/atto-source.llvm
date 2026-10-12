# Core Assert cross-backend conformance v1

This directory defines **portable scalar fixtures**, not a replacement for the
Oreslang language semantics or a claim that LLVM can execute `std/assert`.

- `main.tsp` is the TypeSpec shape of fixtures and outcomes.
- `cases.schema.json` is the independently maintained JSON Schema 2020-12.
- `cases.json` lists scalar predicate, mismatch, untrapped failure and
  `trap -> None` golden scenarios.
- `verify.py` checks schema validity, malformed-input mutations, canonical
  scalar/arity constraints and a deterministic scalar-only oracle.

The `same` examples intentionally use **primitive ints only**. Heap-object
identity, structural equality, cyclic data and `Any` layout require separate
backend execution fixtures and must not be inferred from JSON serialization.
The oracle never captures actual Java/GraalVM or LLVM runtime evidence.

**Backend status (2026-10-12):** Java/GraalVM `Assert` has been merged and
verified with Maven, GraalVM Native Image AOT and hybrid. LLVM has tested
canonical import parsing and fail-closed IR rejection; native module linking,
`Any`/`bool`, `rt fail`, and `trap` lowering are still pending.

Native parity is established only when both compilers execute the *same* source
cases and produce matching typed outcomes and failure channels, rather than
when their bundled `.ores` files are merely byte-identical.
