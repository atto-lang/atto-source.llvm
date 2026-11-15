# AttoNATS LLVM AOT execution gate

The application owns the **canonical Atto** source, unit and integration
fixtures, compiler-JUnit test, and byte-exact provenance pins in
[`atto-poc/atto-nats-clone.atto` PR #21](https://github.com/atto-poc/atto-nats-clone.atto/pull/21).
This LLVM repository owns a separate backend-side execution entrypoint:
`scripts/test-atto-nats-pretokenized-aot.sh`.

**Source and security:** no cross-org private source transfer is performed.
The test requires pre-existing, separately authorized private checkouts of
AttoNATS and oreslang-source.llvm. It rejects missing app, wrong SHA, dirty
tracked source and missing canonical AttoNATS gate. It invokes the application
gate with the **actual current LLVM compiler checkout SHA**; no fabricated
native-compiler stub or Rust/C++ NATS matcher is permitted.

## Run on an authorized Linux LLVM/Clang machine

Build the real backend on the host (requires LLVM toolchain and native runtime):

```sh
cmake -S /authorized/oreslang-source.llvm \
  -B /authorized/oreslang-source.llvm/build \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build /authorized/oreslang-source.llvm/build --parallel 2
ATTO_NATS_APP_CHECKOUT=/authorized/atto-nats-clone.atto \
ATTO_NATS_APP_EXPECTED_SHA=36aea5983ebd86a0f6d86870e9eff771467e1242 \
bash /authorized/oreslang-source.llvm/scripts/test-atto-nats-pretokenized-aot.sh
```

The app gate validates exact unchanged core/guest Git blobs, reports its
single documented transformation (removing the two top-level
`use namespace` headers, which native LLVM cannot currently lower for
executable source), then invokes `oreslang-llvmc` and its `no-gc` IR
auditor. Clang compiles the guest at both O0 and O2. Both binaries must
execute, return zero and print exactly
`ATTO_NATS_TOKEN_MATCH_PASS\n`. Runtime compilation, LLVM IR emission
and native binary execution are separate mandatory checks. A source-only
unit test or missing runner must never be called a native compiler pass.

Run the fail-closed, source-free admission test on any machine:

```sh
bash tests/atto_nats_aot_gate_admission.sh
```

This exercise checks two failures and shell syntax only; it is **not** AOT proof.

## Backend limitations

This currently targets the bounded **pretokenized**, synchronous String-array
and class-method match kernel. The main NATS application `subject_matches`
still uses `split_literal`/`contains_literal` that LLVM does not yet lower
to its native String region. Verbatim `use namespace` executable source,
compiled sealed actor `state{}`, capability-authenticated route admission,
network.listen/read/write byte streams, actor fanout, queue group transport
and production NATS/JetStream compatibility are separate blockers.

Do not claim an LLVM-native AttoNATS broker or performance improvement from a
passing kernel. Collect paired end-to-end metrics only after both backends
run the same NATS PUB/SUB/MSG protocol, apply identical ACLs and budgets,
and enforce generation-safe actor state ownership.

**Current CI status:** the source LLVM org has produced zero-step runner
failures; these are infrastructure admission failures, not execution of this
AttoNATS gate. Approved private CI export policies must be honored, and no
new LLVM private-source mirroring is authorized by this test script.
