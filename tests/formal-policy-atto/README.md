# LLVM-native AttoLang policy guest M0

This lane **compiles real .atto source** with `build/oreslang-llvmc`,
passes emitted LLVM IR through the no-tracing-GC auditor and Clang at
both O0 and O2, executes the resulting ten native binaries, and rejects
six malformed sources at the real compiler boundary with **no IR output**.
The negative set includes undeclared function and value references, forbidden
legacy `async` and both orders of `critical` + `suspends` function modifiers,
and implicit string-to-integer conversion. Each negative is pinned by a Git blob,
requires a normal nonzero compiler exit and diagnostic (not a crash), and is
recorded in the receipt. There is no host-model fallback and no permissive "skip if
unavailable".

The five independent *pure predicates* correspond to:
- `lease-fencing.atto`: exact positive authority triple and equal fence.
- `nats-token.atto`: deny empty, dotted, wildcard, whitespace or control tokens.
- `auth-destination.atto`: require TLS, admitted DNS, pinned socket and no redirects.
- `epoch-atomicity.atto`: necessary preconditions for a staged epoch commit.
- `dnd-drop-admission.atto`: exhaustively checks a pure six-boolean DnD provider/drop gate against all 64 source-level states. Exactly two states pass; actual transferred payload bytes, current UI target and host side effects remain external responsibilities.

Each `main(): int` returns 0 only when both its admitted case and the
explicit forbidden mutations have the expected boolean results (or, for DnD,
when every one of the 64 selector/authorization states matches its independent
expected verdict). These
source programs do **not** call product Rust implementations, perform DNS,
access storage, prove NATS transport isolation, or exercise actor scheduling.
They are the first compiled guest *scalar projection* milestone, not complete
cross-backend conformance. Java, .NET, Wasm and Cranelift remain
`NOT_EXECUTED` until those independent backends run equivalent fixtures.

## Locally executable (never a synthetic success)

Build the compiler and native IR auditor from the exact checkout:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --target oreslang-llvmc oreslang-no-gc-ir-audit-build --parallel 2
ATTO_POLICY_EXPECTED_HEAD="$(git rev-parse HEAD)" \
ATTO_POLICY_RECEIPT="/tmp/atto-policy-native-receipt.json" \
bash scripts/test-formal-policy-atto-native.sh
```

The workflow `.github/workflows/atto-formal-policy-native-guest.yml`
retains a machine-readable receipt with the exact source head/tree, guest
source Git blobs, compiled native binary SHA256 hashes, compiler and
IR-auditor hashes, run ID/attempt and guest exit status. Negative-case source blobs,
compiler exit statuses, compiler-diagnostic SHA-256 digests, the exact
harness Git blob, and case count are also retained. All six negative cases
must be rejected; a legacy grammar accepted by the compiler is a real failure. A workflow with
`jobs=[]` or `steps=[]` is runner admission failure, not proof; no
backend is marked verified merely because fixture files exist.

Production status remains blocked until the toolchain really runs the
fixture and an independent reviewer checks compiler output and negative
admission. Do not mirror private source to another org without security
review and explicit authorization.
