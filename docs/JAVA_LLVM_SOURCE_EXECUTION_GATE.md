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
at known Git commits, with no tracked local edits. A successful run creates a
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
a tracked checkout is modified, or a previous receipt already exists.

The Java reference launcher must be reviewed to ensure it really executes
guest Oreslang source. The manifest includes checkout commit IDs, but the
runner is still responsible for recording the launcher build/toolchain and
attaching the complete executed job logs. Source SHAs by themselves are **not**
proof of provenance. Keep runner permissions scoped to the source organization;
do not place source or tokens in public logs/artifacts.

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
