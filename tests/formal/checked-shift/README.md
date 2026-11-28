# Checked shift flow validator

Validate named, unoptimized plain-i64 functions produced for left, logical-right and arithmetic-right shifts. The current native backend uses unsigned comparison against 64 before emitting a shift (reviewed `LlvmCodegen.cpp` blob e2be5780ccd4c68a3e3f75e52edf5f5013b9c03e). The shared mathematical contract lives in atto-common-source/formal/checked-shifts. This validator requires the exact correct operand identities, unsigned interval guard, terminal failure edge, and unflagged operation and return on the admitted path. It admits only the documented direct and single-assignment stack-slot shapes; unknown instructions/attributes are rejected.

Run with llvmlite 0.47.0:

```sh
python tests/formal/checked-shift/test_shift.py
python tests/formal/checked-shift/check_shift.py actual.ll symbol ashr
```

Local LLVM 20.1.8 parse/verify: 6 accepted representative shapes and 16 rejected mutations. Exact checker and test Git blobs a3dc5d425b6d4666c4d8eaed2242d0fe6bbb1fc8 and 4513625af395245f0dad02a1204a06dd07bea412. Actual compiler output, source correspondence and native guest execution must be qualified separately. The checker trusts LLVM instruction and trap semantics and the intended native target stack assumptions. It does not prove optimization, language-level Error identity, or complete compiler/runtime correctness.
