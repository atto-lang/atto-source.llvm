# Tensor2D native/guest admission — exact evidence and remaining gates

Status: **native C++ host checks executed; LLVM Oreslang guest compilation NOT verified**. October 28, 2026.

## Verified native host execution

Private carrier run:
https://github.com/honeypot-r-us-test/hnpt-lib-core/actions/runs/40788647157

- Repo: `honeypot-r-us-test/hnpt-lib-core` (private).
- Branch: `probe/oreslang-llvm-tensor2d-host-20261028`.
- All steps completed successfully, including checkout, exact Git blob assertions,
  GCC -O2, Clang -O2, Clang -O1 with ASan+UBSan, and job completion.
- No source-org or cross-org checkout token or secret was used; exact source
  files were copied by authenticated GitHub connector into a private carrier.
- Exact source Git blob SHAs verified in carrier checkout:
  - `src/main/cpp/oreslang/runtime/Tensor2DAdmission.hpp`:
    `5a15fd21b9bffbd48bb6309248e1bc75e7592981`
  - `tests/native_tensor2d_admission_test.cpp`:
    `edc82b58306b58b919ff9ec33c9c44d282fffda3`
- Carrier evidence covers **host-only** rank-2 geometry (14,700 test
  configurations, byte bounds and writable injection tests).
  The carrier has **not** compiled any Oreslang `.ores` guest program.

## Awaiting real guest execution

- `tests/fixtures/tensor2d-scalar-native.ores` blob:
  `fad9c650f1623e5a9bbe27f6fea526d616996347`.
- The exact-head `.github/workflows/tensor2d-native-guest.yml` first builds
  `oreslang-llvmc`, then emits IR, links with Clang and requires executable
  exit code **42**. It also demands diagnostic-only rejection of unsupported
  GPU and untyped `int/int` division, and publishes source/compiler/binary hashes.
- The native source compiler explicitly rejects `/` on `int` because the
  Java semantics yield floating-point, not native signed-i64 division.
  **Do not change that policy to make this tensor fixture work.** The native
  scalar fixture uses an explicit 2^20 bound on dimensions/strides/scalar widths
  for nontrivial cases; this guarantees arithmetic fits signed-i64 without
  `/`. One-element and empty tensor cases admit wider values as proven
  by their branches.
- This **bounded LLVM guest subset is more restrictive** than Java's
  full signed-i64 tensor descriptor helper and is not yet complete parity.
- No runner-admitted exact-head LLVM guest execution has been observed.
  A zero-step Actions failure is infrastructure/admission, **not** a parser,
  source, LLVM IR or runtime pass/failure.

## Follow-up to true cross-compiler parity

1. Execute pinned `oreslang-llvmc` against the `.ores` fixture in a private
   carrier containing an exact cryptographically verified compiler tree, or
   an admitted native source-org runner. Verify the actual emitted IR and
   executable digest and exit status.
2. Execute Java PR #2227's real guest module. Compare fixtures and negative
   diagnostics with explicit allowance for the current native bounded subset.
3. Implement a typed checked-integer multiply/size effect that safely admits
   full signed-i64 strides without silently changing `int/int` division.
4. Introduce one identical `.ores` guest source case usable by both compilers
   (requires aligning module/linking/entry support), hash the exact bytes and
   prove observed values/diagnostics. Only then claim same-source tensor parity.

No GPU hardware, QPU, real physical tensor storage, actor ownership or real-time
deadline evidence is implied by the successful native host carrier test.
