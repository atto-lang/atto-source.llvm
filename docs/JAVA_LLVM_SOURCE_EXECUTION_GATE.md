# Strict Java/GraalVM ↔ LLVM source-execution qualification

This is a **required release qualification**, not a statement that the full
Oreslang language already has Java/LLVM parity. Its first scope is the merged
native class/String source-execution corpus at
[tests/native_class_string_parity.rs](../tests/native_class_string_parity.rs).

## Execution contract

The existing LLVM-only CTests run nine positive programs, five times each,
through source → verified LLVM IR → Clang → native binary; 14 negative
programs and two target boundaries must reject without partial IR. **Those
LLVM-only tests are not differential evidence.**

The strict differential runner adds two independent Java executions per
positive source program and byte-compares each to the expected native output.
A Java launcher must be supplied; there is **no** skip or fallback when Java
cannot execute a fixture. Both source trees must be checked out independently
at known Git commits, with no tracked **or untracked** local edits. The gate additionally rejects
Git `assume-unchanged` and `skip-worktree` index hints and ignored untracked
inputs outside the designated generated-build directories. For every tracked
source file, it validates an ordinary Git file mode (100644/100755), checks
staged bytes against `HEAD`, and hashes on-disk bytes against the indexed Git
blob before **and after** running the compilers. In this first strict profile,
tracked symlinks and submodule gitlinks are rejected.
The launcher and both source fixtures must be tracked by the pinned checkouts;
the runner checks byte-for-byte fixture equality and performs fresh
compiler/runtime builds. The JSON receipt records per-program native and Java
stdout as hexadecimal bytes (including embedded NUL), individual exit status,
repeat counts and exact source commit IDs.
A successful run creates a
**new** JSON evidence file containing both exact source SHAs, counts and the
sanitizer flag. An existing receipt is never overwritten.

The native-only negative rejections are recorded separately, **not** counted
as Java negative parity. Broader type/diagnostic, exception, actor and async
conformance must add independent differential test suites.

## Run on an authorized machine

Obtain the Java and LLVM private repositories through access already granted
to the runner. The source may not be exported to another organization without
explicit private-source export authorization. Set up and build LLVM normally:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
    cmake --build build --parallel 2
    ctest --test-dir build --output-on-failure

Provide an **executable launcher inside the independently checked-out Java
repository** that accepts the .ores source filename as its last argument,
executes that file using the real Java/GraalVM guest runtime, and preserves
stdout, stderr and process exit status. The companion tracked Java launcher
is scripts/run-source-parity.sh (Java PR pending merge); build its classpath
before invoking it:

    cd /authorized/checkouts/oreslang-source.java
    mvn -B -ntp -DskipTests package
    mvn -B -ntp -q dependency:build-classpath -Dmdep.outputFile=target/conformance-classpath.txt

The launcher must not fabricate expected output or invoke the LLVM executable.
From the LLVM checkout, invoke:

    bash scripts/verify-java-llvm-source-parity.sh \
      ./build \
      /authorized/checkouts/oreslang-source.java \
      /authorized/checkouts/oreslang-source.java/scripts/run-source-parity.sh \
      clang

The command runs the actual compiled Rust harness and refuses to issue a
receipt if a Java fixture fails, stdout diverges, a pinned SHA is malformed,
a tracked or untracked checkout is modified, or a previous receipt already
exists. It requires `jq` to independently verify the versioned receipt schema,
both exact checkout SHAs, the complete nine-case corpus, execution counts,
identical hexadecimal stdout bytes, **the independently pinned expected
stdout bytes for every named case**, and exit statuses before it publishes a
receipt. A forged receipt in which both backends supposedly print the same
incorrect bytes is rejected. JSON is slurped as **exactly one** document; concatenated receipts
are rejected. A nonempty file or a process exit zero is not sufficient.

The Java reference launcher must be reviewed to ensure it really executes
guest Oreslang source. The manifest includes checkout commit IDs, but the
runner is still responsible for recording the launcher build/toolchain and
attaching the complete executed job logs. Source SHAs by themselves are **not**
proof of provenance. Keep runner permissions scoped to the source organization;
do not place source or tokens in public logs/artifacts.

## Source-free admission regression proof

Run `bash tests/java_llvm_parity_gate_admission.sh` and
`bash tests/java_llvm_parity_receipt_admission.sh` before attempting any
private-source runner. The latter uses mocked toolchains to prove that malformed
JSON, a forged Java revision, missing fixture results, divergent guest stdout,
concatenated JSON documents, malformed hex output, and equal-but-incorrect
backend output **cannot** publish a success receipt. These admission
tests provide no real compiled source evidence.

The first suite creates disposable **synthetic** Git repositories to
exercise denial of untracked LLVM and Java sources, a modified tracked source,
committed Java/LLVM fixture divergence, an untracked launcher, hidden worktree edits via `assume-unchanged`, ignored
Java/LLVM sources, unrelated committed symlinks, and **committed
symlinks to identical external fixture data**. Guest fixtures must be regular
Git blobs (mode `100644`), and the Java launcher must be a tracked executable
Git blob (mode `100755`); neither may be an escape through a tracked symlink. It also
syntax-checks the actual strict runner. These tests are safe to execute without
LLVM, GraalVM, Maven or a private source export; they are **not** guest
execution or cross-backend conformance evidence.

## Admission policy

- Release qualification **requires** a strict receipt plus complete executed
  LLVM and Java logs; Java-optional local CTests never count.
- Parser acceptance, LLVM host-only tests, CTest registrations, and failed
  zero-step GitHub Actions runs cannot establish source-level parity.
- This initial receipt covers class/String execution only: not source actor
  execution, await, supervision, ownership across suspension, HTTP, or Spin.
- Use a distinct new receipt for every run; pin both exact trees and record
  the sanitizer/compiler configurations. Do not generalize nine programs into
  complete backend parity.
