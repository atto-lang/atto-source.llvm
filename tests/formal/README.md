# Experimental scalar division translation validation

Stacked on #911, not a compiler behavior change. This tool requires Python with llvmlite (locally LLVM 20.1.8) and a separately trusted Z3 4.13.3. It accepts only a deliberately narrow unoptimized two-i64-argument, three-block function. LLVM parses and verifies the input; a restricted SSA interpreter extracts the actual branch predicate and selected divisor, follows scalar stack slots, and checks the trap edge and returned signed quotient. Unsupported shapes fail closed. The model assumes defined scalar arguments, ordinary LLVM target layout, and intrinsic trap semantics. It is not general LLVM verification, optimized IR equivalence, Result/error ABI verification, or a machine-code correctness proof.

Representative fixture replay:
```sh
python tests/formal/validate_scalar_division_ir.py tests/formal/representative_division.ll quotient > proof.smt2
z3 -smt2 proof.smt2
```
Require exactly unsat, unsat, unsat, sat, sat on separate lines and successful solver exit, no diagnostics. The first three negate guard equivalence, safe divisor and admitted divisor identity. The final two are reachability controls. The script emits obligations, not an attestation.

Local representative IR: exact validator Git blob a5cf5e15d3d8d332171ecb0279900631edcd813f; real libz3 4.13.3.0 returned the expected sequence. 10 defective variants detected: three SAT discrepancies (missing overflow, zero substitution, changed valid divisor), seven rejected IR shapes (wrong edge, unsigned division, exact flag, wrong numerator/return, bad alignment, volatile load). The Atto source fixture is unexecuted.

Next gate: build actual oreslang-llvmc on the exact candidate tree; compile tests/fixtures/formal-dynamic-division.atto to LLVM IR; identify the exact emitted quotient symbol, feed it to the checker, replay solver obligations and execute the real native guest at O0/O2. Require exit 42. Also test invalid dynamic operands separately and extend to the trapped Result path. If emitted IR is outside the accepted slice, record unsupported rather than replacing it with a handwritten fixture or dropping instructions. No private source export.

## Hardened replay (2026-11-26)

Run `python tests/formal/verify_division.py --solver /path/to/trusted/z3`. To require actual compiler output, add `--compiler /path/to/oreslang-llvmc --symbol quotient` (use the exact emitted symbol). The optional compiler lane has no fallback to representative IR. Its output does not claim machine-code correctness.

The original extractor accepted two defective instructions when their quoted SSA names contained misleading instruction text. It now matches opcode RHS and plain scalar signatures, rejects quoted result names and unsupported attributes/conventions, and caps intermediate expression size before concatenation. The input byte budget is 256 KiB. Local LLVM 20.1.8 parsing + actual libz3 4.13.3.0 (CLI-compatible adapter) yielded 18/18 regression scenarios: two accepted correct forms, three SMT-disproved mutants and thirteen rejected shapes/budgets. Exact tested blobs: validator `ceef9af1cc3dfd5b1d50627252a20f385363baa0`, fixture `9eb79eee1e975044d7dc1211017314b7a906c65e`, runner `925211f476b925cb0d9b98abe33aac291880581f`.

Separately, the exact representative IR fixture was compiled with local Clang 17.0.0 into native shared libraries at O0 and O2. An independent Python integer oracle compared 10,155 valid dynamic quotient inputs at each level, including signed extrema and seeded random inputs. Four invalid cases at each level terminated in isolated processes with SIGILL. Total 20,318/20,318 checks. These are representative fixture execution results, **not** an Atto compiler run. Official Z3 CLI, actual compiler-generated IR and guest execution remain unverified until their separate lanes execute. The prototype assumes trusted compiler/solver executables; subprocess capture is not a hostile-process sandbox.
