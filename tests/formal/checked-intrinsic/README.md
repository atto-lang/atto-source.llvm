# Checked intrinsic flow validator

This is an independently executable experimental checker for a named, unoptimized LLVM function with the plain i64 ABI. The current native code generator uses signed overflow intrinsics for addition, subtraction, multiplication and negation (reviewed main 8c4aaa1da4af931f199660b08fa2146ea884fce7, LlvmCodegen.cpp blob 1776353ff848965db1d7fcc3e1ca2e4ea62af65f).

Run with llvmlite 0.47.0:
```sh
python3 tests/formal/checked-intrinsic/test_intrinsic.py
python3 tests/formal/checked-intrinsic/check_intrinsic.py emitted.ll exact_symbol mul
```

The checker validates the LLVM module, restricted function header, exact operand order (including zero minus argument for negation), single-assignment local stack slots, matching signed intrinsic and extraction indices, overflow-true terminal trap, and overflow-false return of the exact intrinsic payload. No unknown instructions, attributes or metadata in the modeled function are accepted. The caller must identify the intended function and operation independently.

Local llvmlite LLVM 20.1.8: eight valid representative forms (four operations, direct and stack-slot arguments) and sixteen negative mutations passed. Exact tested blobs: checker 8b136d872ea7af6e8ce2b930e698ff88b7cda678, test dbd053816b3d360b147cbb62f1c4c53faf50c458. This is control/data-flow validation conditional on trusted LLVM intrinsic, stack and trap semantics in the intended native target, not an independent proof of the intrinsic, compiler-generated source correspondence, optimized IR, complete module behavior, guest Result ABI, or machine code. Actual Atto compiler output has not yet been validated with this checker. The test tool is standalone; no production build behavior changes.
