# Target ABI v1 (cross-backend)

This file defines the same target descriptors in Java/Graal and LLVM:
`contracts/architecture/v1/targets.tsv`.

**Important:** This is a conservative *descriptor and bounds contract*, not a
declaration that all targets have executable compiler/runtime support. An
`unimplemented` row must not be accepted for explicit cross-compilation.
LLVM currently supports default host IR (without cross-target certification)
and the restricted `wasm32-unknown-unknown` IR subset. No explicit CPU
features or ABI overrides are supported yet.

## Invariants

- Oreslang signed `int` retains i64 semantics irrespective of pointer width.
- Actor IDs, region handles and ownership generations are opaque logical values,
  not pointers; do not reinterpret handles as `size_t` or addresses.
- Logical offsets are checked in signed-64-bit range; before any contiguous
  addressable allocation, additionally require `bytes <= PTRDIFF_MAX` and
  `bytes <= SIZE_MAX` on the *actual host*, and enforce the selected target's
  conservative signed-address offset limit (2^31-1 on 32-bit).
- File/wire representations use separately versioned little-endian encodings;
  do not reinterpret native structs or assume `memcpy(float)` is a portable
  cross-endian serialization format.
- ABI, vector ISA, cache-line size, and atomic availability must be obtained from
  verified target machine capabilities. 64-bit pointer width alone does not
  imply any SIMD instruction set or lock-free atomic width.
- No implicit CPU fallback for explicit GPU placement or unsupported target IR.
- Changing physical layout or CPU architecture must preserve Oreslang logical
  ownership, actor confinement, observable integer semantics and safety errors.

## Current admission and verification

`oreslang-llvmc --target=wasm32` and
`oreslang-llvmc --target=wasm32-unknown-unknown` emit only the existing
restricted wasm32 IR. `--target=host` selects the historical default
backend. Known cross-target names in the TSV fail closed until native
TargetMachine/DataLayout and ABI linkage are implemented. `--cpu=...`
also fails closed.

The target descriptor regression suites run on x86-64 and ARM64 runner types,
but are **not** full native 32-bit conformance, actor runtime or end-to-end
cross-architecture performance certification.

Next: verify host ABI from LLVM TargetMachine, bind target profiles to all
native allocators/FFI boundaries, integrate Java DOP PRs, run full native
and GraalVM ARM64 suites, then introduce target-checked SIMD variants.
