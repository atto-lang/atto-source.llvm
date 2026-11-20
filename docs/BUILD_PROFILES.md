# LLVM build-profile admission

`oreslang-llvmc` recognizes the Oreslang v1 build-profile vocabulary:
`--release`, `--strict-aot`, `--hard-rt`, and orthogonal `--freestanding`.

The compiler currently emits verified LLVM IR, not the final optimized executable.
Therefore release/strict-AOT/hard-RT are deliberately rejected until the driver can
bind semantic reachability/tree shaking and final-artifact evidence. Producing LLVM IR
alone is not accepted as proof of a release or hard-real-time profile.

Runtime reflection is not part of the native language subset and remains forbidden by
the v1 contract.

`--freestanding` is currently admitted only for the existing
`thumbv7m-none-eabi` path, whose source admission already rejects hosted dependencies.
The active native realtime work (PR #694) is an integration dependency for hard-RT, but
hard-RT additionally needs the profile's WCET/deadline evidence gate before it may pass.
