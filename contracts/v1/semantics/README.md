# Oreslang Semantic Conformance — deeper runtime and diagnostic proofs

**Separate Java and LLVM compiler + ownership checker code.** These cases
extend [the admission suite](../README.md) with stronger oracles.

- Each diagnostic case must return semantic rejection **exit code 1** and
  identify a normalized *class*, not merely any compiler error.
- Each LLVM native case must emit valid module IR, compile using Clang,
  actually execute the binary, and match the declared process exit code.
- Native test values are restricted to 0–125. Process exit status truncation
  makes this unsuitable for arbitrary integer results or exceptions.
- A separate normalized comparator verifies report manifest integrity,
  exact case inventory, diagnostic classes, runtime values and backend readiness.
- A missing compiler, signal, timeout, linker failure, wrong result, or
  infrastructure error **always fails a required case**.
- Graal native-result fixtures first pass Java/Graal `--check`, then run the
  same source through real `OresMain` and verify its exit status and empty
  stdout/stderr. The `--run-command` must be configured explicitly; missing
  guest execution is a failure, never a pass.
- Pending indicates missing implementation or verification, *not* conformance.
  Ownership and actor/async feature parity remain separately gated.
- TypeSpec defines portable interchange models; JSON Schema 2020-12 validates
  constrained cases. TypeSpec and schemas are both compiled/validated in CI.

## Run in LLVM source checkout

```sh
python3 -m pip install 'jsonschema>=4.20,<5'
python3 contracts/v1/semantics/test_runner.py -v
python3 contracts/v1/semantics/run.py --validate
./contracts/v1/node_modules/.bin/tsp compile contracts/v1/semantics
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j2
python3 contracts/v1/semantics/run.py \
  --backend llvm --command './build/oreslang-llvmc {source}' \
  --clang clang --revision "$(git rev-parse HEAD)" \
  --report build/semantics-llvm.json
```

## Run in Graal source checkout

```sh
mvn -B -ntp -DskipTests package
mvn -B -ntp -q dependency:build-classpath -Dmdep.outputFile=target/conformance-classpath.txt
python3 contracts/v1/semantics/run.py \
  --backend graal --command 'python3 contracts/v1/java_probe.py {source}' \
  --run-command 'python3 contracts/v1/java_probe.py {source} --execute' \
  --revision "$(git rev-parse HEAD)" --report target/semantics-graal.json
```

After both reports exist, use:

```sh
python3 contracts/v1/semantics/compare.py \
  --llvm build/semantics-llvm.json --graal target/semantics-graal.json
```

**Current limitations:** no shared runtime value ABI beyond a small native
process exit code; the Graal native-exit adapter only covers bounded 0–125 process exit values; LLVM has only a
restricted i64 ownership checker; no automatic source-level actor/preemption conformance yet. Proposed functionality
is recorded as pending instead of turned into false passing tests.

Follow-up work: integer width/overflow specification, stable diagnostic IDs
rather than regular-expression mappings, canonical Oreslang JSON stdout,
actor message event traces, cycle-safe reference/ownership cases, replay-fenced
cancellation and fair preemption under strict resource budgets.


## Expanded seeded regression suite

The independent LLVM conformance workflow additionally verifies:

- 512 deterministic arithmetic generator pairs *without invoking a compiler*
  (generator integrity; not counted as backend execution).
- Two additional **64-pair native Clang-executed** suites with seeds
  20261009 and 314159. These run alongside the original 32-pair suite:
  160 actual source-pair comparisons, 320 independently compiled native
  executables in all when each test completes.
- 48 reproducible valid three-actor trace samples, each with 24
  sends/receives and an exclusive-resource lifecycle.
- 64 adversarial mutations covering duplicate send/delivery, wrong target
  or owner, use-after-termination, incomplete trace and exclusive lease
  violations. Schema mutations test untrusted event fields as well.

These properties test the **backend-neutral trace validator** and a
narrow integer arithmetic subset. They do not claim that compiled
Oreslang actors already emit production traces, or that LLVM supports
verified borrow/no-GC or Linux hard-realtime execution.


## Exact-head, fail-closed fixture parity gate

The default comparator is an **incremental conformance inventory check**. Its
exit code is zero when required tests pass and declared pending tests remain
pending; **it does not certify source, feature, runtime, or semantic parity**.
Inspect `SUITE_COMPLETE=no` and `BACKEND_PENDING` in its summary.

A release/merge job that intends to claim **full coverage of this fixture
suite** must run the strict gate with independently verified commit hashes:

```sh
python3 contracts/v1/semantics/compare.py \
  --llvm build/semantics-llvm.json \
  --graal target/semantics-graal.json \
  --llvm-revision "$LLVM_SOURCE_SHA" \
  --graal-revision "$GRAAL_SOURCE_SHA" \
  --require-suite-parity
```

The strict gate rejects any pending/failing/mismatched fixture, missing or
fabricated diagnostic/native evidence, and reports for source commits other
than the pinned heads. Both heads must be supplied; do not use arbitrary
hard-coded revisions or presume the files correspond to a current branch.

Even a passing **strict suite** covers only its enumerated fixtures. It must
not be labeled *language-wide feature parity*, since actors, source-level
supervision, asynchronous continuation frames, cancellation, select,
untrusted-isolate boundaries, cyclic graphs and general heap ownership are
not all represented by identical, end-to-end executable fixtures today.
LLVM's small native i64 checker implements a subset of ownership operations,
not general actor/heap/async borrow parity.

## Externally admitted actor messages

The actor trace protocol now defines an `admit` event: a trusted producer
successfully enqueues a message into a live actor's native mailbox. It carries
`actor` (the receiver) and `message_id`, without inventing an actor identity
for the external host thread. Subsequent `receive` must have the same ID
and can occur at most once. All existing `send` semantics remain unchanged.

LLVM's `oreslang-actor-trace-test` and `traces/native_probe.py` drive a
real `HungryCarrier` with 24 accepted messages, wait for corresponding
single-threaded handler callbacks, verify `CarrierStats` and lifecycle,
then validate the complete causally reconstructed trace via JSON Schema and
the backend-neutral state machine. The funded workflow repeats this 12 times.

**Limits:** This tests the native continuation carrier, not actors compiled
from `.ores`, not realtime isolation, and not cancellation traces or a
production tracing API. The sequence is reconstructed after join from real
admission and callback evidence, not a timestamped total order. Java/Graal
shares the schema and adversarial validator but does not yet emit native
carrier traces.


## Proxy request / completion packed-i64 guest ABI checkpoint

`fixtures/proxy-packed-receipt.ores` is a **shared, required, native-execution** fixture (`native.proxy-packed-receipt`, expected process exit 42) in both Java/GraalVM and LLVM semantics manifests. It verifies the same arithmetic used by host request tickets and receipt claims:

- Request ticket: `incarnation * 4294967296 + row` (16-bit-bounded physical row).
- Receipt: `serial * 4294967296 + (row * 8 + lane + 1)`, with up to 8 bounded receipt lanes per request.
- The fixture uses serial values 33,554,433 and 67,108,865, so packed words exceed `2^53` but remain inside signed i64. It compares each packed value with its aligned 32-bit base and expects the exact 16-bit row / bounded receipt index to survive. IEEE-754 rounded arithmetic would produce process exit **48** rather than **42**.
- Because the LLVM source frontend intentionally rejects integer `/` and `%` until their semantics are implemented, the fixture relies only on supported signed-i64 `+`, `-` and `*`. Decoding by division/remainder is a **separate pending language capability**, not silently interpreted by a test shim.

**Scope:** this proves only arithmetic performed by actual compiled guest source *once the required conformance workflow executes*. It does **not** expose a guest `ProxyRequestSlab`, allocate actor arenas, send a callback, or establish owner-turn authorization. The host-level slab API and actual guest lowering require a second native-execution contract that admits, pins, retires, settles and recycles under real Java/GraalVM and native OresVM actor turns.

The semantic suite's `cases.json`, normative schemas, TypeSpec manifest and all fixture Git blobs are now identical in the two branches; the three defer/finally cases that Java implements remain explicitly `llvm: pending`. The existing `contracts/v1/semantics/run.py` executors still differ by backend as intended.

**Evidence limitation:** both source-organization conformance workflow runs on the new exact heads failed **before recording any steps**; a local Python arithmetic reference check passed, but **no checked-in Oreslang source was executed by a compiler** in this round. Do not mark this conformance case verified until the exact guest-result report is produced on an authorized runner.
