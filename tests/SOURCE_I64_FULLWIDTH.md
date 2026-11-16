# Native LLVM source-i64 full-width conformance

This test implements the source corpus from `oreslang-common-source`
`contracts/v1/source-i64/portable-i64.v1.json` and pins both its Git blob
SHA-1 and literal byte SHA-256.

The native LLVM compiler generates an exported `portable_i64_probe(): int`
for each reviewed expression, **with no OS main wrapper**. Clang compiles
the actual generated LLVM IR, links it to a reviewed C harness, and the
native executable prints an exact 64-bit signed return value using
`PRId64`. Seven distinct source programs must compile and execute.
The suite also keeps main's six strict negatives: source division, the
out-of-range integer literal and four programs using the reserved legacy
`%` operator must exit 1 with an `oreslang-llvmc:` diagnostic and zero
emitted IR (the `%` cases must name the reservation).

A passing CTest proves this bounded scalar compiler / native codegen case
set, not general compiler parity, JVM equivalence, Wasm correctness,
actor ownership, or AOT package reproducibility. No green CI runner job
has been observed yet for this change, so the PR must remain draft.

The former four-case process-exit check lost negative and >255 results.
This replacement does not transform LLVM IR, forge compiler outputs or
substitute reference arithmetic for actual generated native execution.
