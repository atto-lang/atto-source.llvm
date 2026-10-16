# Shared contracts before shared compiler source

Status: immediate collaboration model; future common-source integration plan.

Keep the Java/Graal and LLVM compilers and borrow checkers independent. Share the specification and evidence of behavior first. Backend-specific test suites remain necessary.

## Contract layers

| Layer | Purpose |
| --- | --- |
| Language specification | Syntax, types, ownership, effects, actor/isolation and execution semantics |
| TypeSpec | Versioned interchange models for suites, cases, backend readiness and reports |
| JSON Schema | Validate committed manifests and result structures, including unknown/missing fields |
| Source fixtures | Identical .ores programs with explicit expected admission or observable behavior |
| Backend adapters | Invoke the actual compiler/runtime and normalize outcomes |
| Differential tests | Compare required behavior on the supported intersection of backends |
| Backend-specific tests | Native memory/layout, isolation, JVM interop, JIT/AOT and realtime claims |

TypeSpec and JSON Schema define data shapes, not the language grammar or its semantics. Generated schemas and committed schemas need compatibility checks. Neither replaces compiler, ownership or runtime tests.

## Results and parity

Track feature maturity explicitly:

- Proposed, not implemented.
- Implemented and tested in Java/Graal only.
- Implemented and tested in LLVM only.
- Implemented and tested in both.

Also record readiness for each individual case: required or pending, with a reason for pending support. A feature can be implemented without its full guarantee being certified; retain that distinction.

Reports should identify the suite/schema version, fixture identities, compiler and adapter revisions, execution profile, toolchain, observed result and test evidence. Never turn a timeout, crash, missing compiler or unavailable runner into successful rejection.

Admission tests compare accept/reject outcomes. Diagnostic tests require the intended diagnostic category. Execution tests compare values, exceptions and observable effects. Concurrency tests use deterministic or explicitly constrained traces rather than requiring unrelated internal schedules to match.

## Existing starting points from the supplied discussion

The transcript describes these integration PRs:

| Track | LLVM | Java/Graal |
| --- | --- | --- |
| Initial admission contracts | [#4](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/4) | [#488](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/488) |
| Semantic/diagnostic expansion | [#5](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/5) | [#489](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/489) |

These are historical starting points, not claims about their current heads, merge status or complete backend parity. Recheck the exact revisions before reuse. Focused compiler checks do not establish full Java JUnit, Native Image or native runtime correctness.

Leave the working contract integrations in their backend repositories for now. If this repository later distributes a canonical contract package, pin its version and verify fixture identity in both consumers; avoid an unversioned three-way copy of normative files.

## Memory safety and performance gates

The [ownership-first memory and performance contract](MEMORY_SAFETY_AND_PERFORMANCE.md) sets separate targets for collector-free LLVM native execution and low-garbage Java/Graal execution, alongside identical safe-source behavior and C++/Rust comparison workloads. Semantic admission alone does **not** prove deterministic drops, zero tracing collector reachability, independent heaps or benchmark parity. Each backend must provide real executed release-mode evidence for allocation, ownership, actor/async quiescence, FFI reachability, cleanup and GC/pause tails. Unsupported operations reject without output; a draft native arena implementation is not proof of complete source-language safety.

## Ownership syntax compatibility

The [ownership syntax contract](OWNERSHIP_SYNTAX.md) makes by-value ownership implicit and specifies that `owned` is not a standard keyword. The experimental LLVM `owned int` heap allocator marker must reject in normal compilation, and its test-only opt-in cannot count toward Java/LLVM source parity. Ordinary scalar bindings must not require an allocator or GC just to satisfy ownership semantics.

## Native I/O API naming

Source-visible native synchronous I/O operations **must** end in `_blocking`. Async I/O operations may (but need not) end in `_async`; names alone never replace execution/effect validation. See [the normative naming contract](NATIVE_IO_NAMING.md) for scope, existing Java/Graal migration gaps, LLVM support status, negative tests and versioned rollout.

## Executed guest result comparison

Use `scripts/executed_parity_gate.py` after each backend has compiled and
executed the **same exact Git-blob source fixture**. Inputs are two JSON
records with `backend`, `fixture_blob`, `source_tree`, `compiler_sha`,
positive `run_id`/`job_id`/`runner_id`, `conclusion: success`, positive
`executed_tests`, zero `skipped_tests`/`failed_tests`/`errored_tests`,
and integer `observed` results keyed by case. The gate rejects source
drift, missing execution evidence, divergent case sets or outputs.

For the canonical HTTP counter fixture, the parity gate also validates each
backend against an **independent observable oracle**, rather than merely
comparing two potentially identically-wrong outputs. It requires `main()`
and `counter_response(-1)`, `counter_response(0)`,
`counter_response(1)`, and `counter_response(2)` with results
`43`, `-43`, `0`, `43`, and `86` respectively. Other probes
may be provided, but must use canonical signed i64 argument notation and
have a nonoverflowing exact mathematical result `43 * count`. The gate
fails closed on unknown cases, missing mandatory probes, boolean results
or wrong arithmetic. Both backends must actually execute these cases and
record their observed results on pinned source trees; hardcoded JSON does
not constitute proof of execution.

These JSON records are **not authenticated by the script**. The caller must
first fetch actual Git trees, workflow jobs and logs from GitHub and check
their source identity and runner execution using `exact_source_gate.py`.
Never invent or assume CI observations from passing compilation alone.
The first common executable fixture is
`fixtures/http-guest-counter-handler.ores` (Git blob
`a0b3fc658d1460eab62c85b3cc897143ea77a7ff`); it establishes a
public guest-handler slice, **not** actor, HTTP socket or AOT feature parity.

## Parsing actual backend stdout

The two backend tests emit **different machine-observation shapes**. The
versioned adapter `scripts/http_guest_log_adapter.py` converts *captured raw
guest-test stdout*, not GitHub's rendered page or a manually fabricated list,
into a canonical `{backend, fixture_blob, observed}` JSON fragment:

```sh
python3 scripts/http_guest_log_adapter.py --backend graal --log graal-stdout.txt > graal-observed.json
python3 scripts/http_guest_log_adapter.py --backend llvm --log llvm-stdout.txt > llvm-observed.json
```

It requires exactly one backend-specific execution marker, rejects duplicate
JSON fields, checks the LLVM case count and source identity marker, and
applies the independent HTTP guest arithmetic oracle. The two backend
companion PRs are [Java #608](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/608)
and [LLVM #116](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/116).
They request 131 observations (main plus -1, 0, and 1..128). Their code
and logs still require funded exact-source runtime execution before use as
parity proof.

**This adapter does not authenticate logs or create CI provenance.**
Its output is deliberately only a fragment; it cannot be fed directly to
the full executed-parity gate without an independently verified binding
to each actual source tree, compiler revision, successful runner/job,
executed test count and zero skips/failures. The producer must verify those
facts against GitHub and attach them before invoking
`scripts/executed_parity_gate.py`. Never manufacture a run ID or assume
that the presence of a marker alone proves guest execution.

## Next contract groups

1. Move/copy/borrow/aliasing rules, lifetime violations, recursive types and cyclic references.
2. Shared/isolated actor ownership, single-writer guarantees, mailbox behavior and supervision.
3. Suspended borrows, futures/channels, wake epochs, cancellation and automatic preemption.
4. Actual return values, errors and effects from a Graal execution adapter.
5. Observable equivalence across supported AOT/JIT/hybrid profiles.
6. Hot-loading generations, stale handles, state migration and failed-activation cleanup.
7. Resource/effect contracts and backend-specific capability rejection.

For a future shared HIR → MIR pass, run this corpus before and after adoption on each backend. Keep independent implementations until the common path proves its supported semantics, diagnostics and runtime behavior without erasing useful backend-specific tests.
