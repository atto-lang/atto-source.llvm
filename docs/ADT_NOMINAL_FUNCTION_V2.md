# AttoLang nominal enum internal helper v2 — LLVM native guest

The optional `oreslang-llvmc --experimental-nominal-enum-v1` entrypoint
now accepts one bounded source-defined nullary enum and one private typed
helper `fnc next(Light state): Light`, followed by a parameterless
`main` that constructs one named variant and matches the returned enum.

The independent source checker enforces source-declared enum owner identity
at the argument/return boundary and for every helper match/return case.
Integer tags are private compiler representation; the guest source cannot
pass raw integers, enter via a public enum-typed function, import an owner,
or create payload-bearing variants. A checked lowering generates a private
ordinary `fnc next(int state): int` with native scalar control flow and a
real static call; actual `oreslang-llvmc` then emits LLVM IR and Clang
produces the native guest. This is not yet a general nominal type in LLVM IR.

The canonical source fixture
`tests/fixtures/adt-nominal-function-v2.atto` is byte-for-byte
identical to .NET's v2 fixture. The host C++ test exercises a checked
helper, three compiler-private constructor tags, and 16 unsafe source
mutations, at GCC/Clang O0/O2. The real LLVM guest verification compiles
each input variant with `oreslang-llvmc`, confirms a static call is present,
runs Clang binaries at O0/O2 (expected 0, 30, 10), and rejects foreign
type owners/forged numeric arguments/nonexhaustive matches with no LLVM IR.

All off-org CI evidence is confined to an explicitly allowlisted private
subset whose source Git blobs are pinned to the PR head; it is not the
whole original source Git tree, ASan/UBSan guest proof, imported nominal
ABI, payload ownership, or actor-message isolation. Independent review
is still required before promotion.
