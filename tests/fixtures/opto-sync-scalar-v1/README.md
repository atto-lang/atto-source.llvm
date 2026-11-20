# Opto Sync exact-byte native AttoLang scalar admission

These five `.ores` sources must have byte-identical Git blobs to
`opto-sync/opto-sync-clients` PR #165 scalar fixture sources.

The real `oreslang-llvmc` compiler emits LLVM IR and `clang` links
executables for `scalar-order=14`, `scalar-parens=42`, and
`scalar-boundary=1`. Process exit status is checked for each.
`reject-overflow` and `reject-invalid` must be compile-denied,
without IR, native artifact or internal compiler crash.

Run: `cmake -S . -B build && cmake --build build --target oreslang-llvmc`
then `bash scripts/test-opto-sync-scalar-v1.sh`.
The workflow pins the exact triggering commit and source SHA-256s.

This is only the scalar floor: not an AttoLang actor/HTTP client,
structured `sync_admission.ores` support, five-backend parity,
NativeAOT on other backends, or an SDK publication claim.
Workflows without admitted runners and executed steps are **NOT_EXECUTED**.

## Corrected common scalar admission

All five sources now use the existing common-subset `pub fnc main()`
declaration spelling. `reject-overflow` is an out-of-range integer
literal (`9223372036854775808`) rather than an in-range expression
whose checked addition may fail at runtime. Compile-time denial here
must not be cited as a checked-runtime-overflow semantic proof.

The five Git blobs must match current Opto Sync clients #165 and
Java/GraalVM #2751 exact-head source files; this is a candidate suite
until native `oreslang-llvmc`, Clang, and guest processes execute in a
runner with nonempty steps.
