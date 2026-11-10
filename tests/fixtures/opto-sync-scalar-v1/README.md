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
