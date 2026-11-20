# LLVM StateGraph source parity — staged admission (2026-11-03)

**Status: incomplete; no production `SuperstepGraph<S>` LLVM guest execution.**
This is an executable parity plan, not a release waiver or a claim that
scalar probes implement the real graph engine.

## Observed native LLVM capabilities and gaps

| Required behavior | LLVM current evidence | Proof required for parity |
|---|---|---|
| i64/bool scalar expressions and guarded routing | Native source subset exists | Exact guest running and Java/LLVM outputs compared |
| Canonical scalar superstep merge and route | Shared authored test, Git blob `054d4e9fe297c520513686feae076dc921f581ac` | Actual Java/JVM and LLVM/Clang native runs return 42 |
| Generic `GraphState<T>` constructor, field, return | Canonical fixture blob `c3a67b837a73f427a6157c446cc6e3989e9a66da`; no executed result | Build actual compiler and run unchanged guest with exit 42 |
| Owned generic node/patch arrays | No supported `Array<GraphNode<S>>`, `Array<StatePatch<S>>` lowering proven | Implement and test owned array lifetime and destructor/borrow laws |
| First-class `Fnc<StatePatch<S>(S)>` stored in node objects | Unsupported escaping callback layout in current documented native subset | Typed callback ABI, captures/loan safety and AOT lowering |
| Generic graph-state snapshots and class-valued returns | Stack-only scalar class subset; heap/object graph not proven | Owned/copy-snapshot semantics and non-aliased returns |
| Exact `SuperstepGraph<S>` guest source | Not compiled on LLVM | Compile the real `src/supersteps.ores` plus all native guests |
| Actor-native scheduling, child supervision, async | Source/runtime parity incomplete | Native actor safety, scheduling, cancellation and lifecycle receipts |
| Generic `Result<T,E>` for graph failures | Scalar payload variants only | Ownership-aware class payloads and typed exception effect parity |

The scalar superstep fixture checks two independent node computations from
one committed snapshot, a deterministic sum merge, refusal of zero-step budget,
conflicting-key admission and post-merge route. It uses *direct functions*,
not the production graph API. It does **not** prove snapshot deep-copy or
generic reducer enforcement.

## Positive lane

`scripts/test-stategraph-native-llvm.sh` builds/verifies the native compiler
before executing:

1. Canonical scalar source (42);
2. NEW canonical scalar-superstep source, byte-identical with Java and
   StateGraph source repositories (42, no stdout/stderr);
3. Canonical generic `GraphState<T>` source (42);
4. Three compiler guest fixtures: scalar, guard, nominal (42).

A successful compilation emits real LLVM IR, then Clang creates and executes a
native executable. IR generation or source checks alone **are not guest parity**.

## Fail-closed negative lane

`tests/fixtures/stategraph-parity-generic-frontier-unsupported.ores` describes
generic class-valued arrays with first-class `Fnc<T(T)>` fields. Until they
are correctly implemented, the LLVM compiler **must reject them with a nonzero
exit and empty IR**. If the compiler starts accepting the source, this negative
test must migrate into the positive native guest lane only when a typed
ownership/typecheck/ABI/runtime test also passes. Rejection is safety, not
feature parity.

## Source provenance and private CI

The LLVM compiler source stays in its own private repository. Its
`docs/security/CI_PRIVATE_SOURCE_MIRRORING.md` requires protected source main,
independent approval, reviewed allowlists and closed incident gates before any
private source export. A source-free canary or private writable destination
does NOT authorize copying the compiler. No source-bearing mirror was used
for this increment.

Native receipts must include exact LLVM PR head/root tree, compiler checksum,
Clang binary/version, the canonical guest Git blob, CMake build result, source
compiler exit, native executable exit, stdout/stderr and stepful runner logs.
The latest observed LLVM PR workflow for head
`a708bca7138a4fc432d51b7e9d2f01813ac2f490` failed with no established
guest execution; updated heads require fresh evidence.

## Engineering exit criteria

Full LLVM StateGraph parity can only be declared when the same production
`src/stategraph.ores` and `src/supersteps.ores` and corresponding native
source tests compile/link/run on Java/Graal and LLVM with identical semantics,
with adversarial cases for conflicts, callback purity, invalid routing,
overflow, cancellation/ownership and step exhaustion. No host-language
StateGraph reimplementation or source-only static contract qualifies.

## Second shared scalar cycle probe

`contracts/stategraph-portability/shared-bounded-cycle.ores` has identical
Git blob `3c69cdf00c4e1390898ef0594eba76c54c0e2d7f` in the StateGraph,
Java, and LLVM repositories. A recursive bounded state transition, zero and
negative budget guards, and conditional termination all must execute and exit
42 with empty stdout/stderr on both compilers. This is not actor or generic
StateGraph execution. The source uses `if ...; do`, which the current native
LLVM parser admits; a source-only check does not establish runtime acceptance.

## Native canonical conditional parser parity (November 3)

`src/main/cpp/oreslang/frontend/Parser.cpp` now accepts the canonical
`if condition then ... fi` spelling in both guarded-return and nested
structured conditional arms, while preserving `if condition; do ... fi`
and braced forms. The byte-identical positive fixture
`contracts/stategraph-portability/shared-canonical-then.ores` has Git blob
`ad42a13d1542d8df6215bff42d056a489015c842` in StateGraph, Java and
LLVM and requires a native LLVM executable exiting 42 with no stdout/stderr.
The negative fixture `tests/fixtures/stategraph-canonical-then-assignment-reject.ores`
requires the compiler to reject `if x = y then` with a nonzero exit and
zero emitted LLVM IR. **Parser source changes and authored tests have not yet
been compiler-executed on the new LLVM head.**
