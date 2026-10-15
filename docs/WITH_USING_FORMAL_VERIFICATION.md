# Actor state scope formal verification

This adds machine-checked evidence for the synchronous scalar actor state
contract audited in [the hardening report](WITH_USING_HARDENING_AUDIT.md).
It combines universal bit-vector checks of two compiled production predicates
with bounded temporal model checking of the surrounding lifecycle protocol.
It does not establish a formal refinement of the complete runtime or prove
whole-language LLVM/Graal equivalence.

## Production predicate equivalence with SMT

`scripts/verify_state_scope_smt.py` compiles the probes in
`spec/formal/state-scopes/` to optimized LLVM IR. The probes call the unchanged
production code; the verifier does not replace those predicates with a model.
A deliberately small SSA translator converts supported pure instructions to
SMT-LIB QF_BV. Unsupported instructions, memory operations, unknown calls,
loops, poison/undef, flagged arithmetic and malformed control flow fail the job.
The only allowed effect is terminal `abort`, modeled as rejection.

For each production predicate, Z3 must find **UNSAT** for all three proposed
counterexamples: an uncovered execution path, a result outside `{0,1}`, or
acceptance differing from the independently written contract. These are
universal finite-bit-vector queries, not sampled test inputs.

| Compiled production code | Inputs quantified | Acceptance theorem |
| --- | --- | --- |
| `admit_critical_handle_state` in `CriticalHandleStateContract.hpp` | 88 bits: mode, callback flags, declared effects, inferred effects, serial-actor flag | Accept iff metadata is valid; intrinsic, lexical, one-parameter and nonescaping; no live loan across suspension or inferred suspension; inferred effects fit declared effects; READ declares read authority and infers no writes; WRITE declares write authority and marks the callback critical. All other modes reject. |
| Private `require_turn` in `ActorOwnedI64Heap.c` | 320 bits: four 64-bit domain/epoch values and two 32-bit active/retiring flags | Accept iff the heap is active, is not retiring, and both domain and epoch match. |

The state theorem also covers unused flag bits, all unknown effect bits, invalid
mode discriminants and both serial-actor settings. Neither a serial actor nor a
continuation fence upgrades read authority or permits suspension in the loan.
The result theorem concerns Accept versus reject, not which diagnostic is chosen.
The theorem is about the production policy helper given its metadata; it does
not prove that frontend effect inference is sound or that every caller supplies
truthful metadata. Parser/lowering admission remains separately regression-tested.

The turn probe supplies valid local storage and initializes the metadata under
examination. This proves the actual metadata predicate for every bit pattern;
it does not prove null-pointer handling, allocation, locking or authentication
of trusted native callers. Generated-method entry placement remains covered by
the compiled native entry-fence regressions.

A separate 64-bit theorem checks that `old != UINT64_MAX` implies
`old + 1 > old` using unsigned bit-vector arithmetic. It proves nonwrapping
increment **under that guard**, not that every runtime increment executes the
guard or that the allocator/scheduler is correct.

Six negative controls weaken private copies of the actual production sources:
remove read/write separation, suspend rejection, no-op acquisition permission,
epoch matching, retirement rejection, or active-turn rejection. Each must yield
a SAT unsafe-acceptance witness. Each witness is then replayed against a native
executable compiled from the exact mutated source. Production files are never
mutated by the verifier. Z3 queries, UNSAT proof objects, SAT models, LLVM IR,
source hashes and replay harnesses are retained in the selected output folder.

`tests/test_state_scope_smt_translator.py` independently exercises branch/phi,
switch/default, signed comparison, select, truncation and bit operations with
universal solver checks, plus 15 fail-closed rejection cases. This reduces
translator risk; it is not a proof of that translator.

## Bounded TLA+ lifecycle check

`spec/formal/state-scopes/StateScopes.tla` describes a single actor with two
logical jobs (reader and writer) and two physical workers. Workers can alternate
between turns, while each admitted turn has at most one carrier. A live state
loan prevents yielding; logical single-threadedness alone is insufficient.

TLC exhaustively explores this finite model: two actor incarnations, two turn
epochs and two loan nonces per incarnation, and two data values. The successful
baseline explores **28,488 distinct reachable states**, **91,089 generated
states**, with maximum search depth **33**. It checks eight invariants:

- Type correctness and at most one active physical worker.
- Agreement between the admitted lane and physical worker.
- Guest entry requires a current domain/epoch ticket.
- The sole lexical loan has the current owner, domain and nonce; no nesting,
  release by another job, or yielding/leaving while it remains live.
- Reader actions never mutate actor data.
- Final retirement is quiescent.
- A completed acquired scope releases exactly once.

The temporal property is `cancelled[j] ~> terminal[j]`: cancellation eventually
reaches a terminal phase. It is checked under weak fairness of each job's
Unwind, Release and Leave actions. Fairness assumes an executing callback can
finish and cleanup remains schedulable. This is not a guarantee of termination
for arbitrary guest code, worker failure or an unfair scheduler.

Ten model mutations must produce the named safety or liveness counterexample:
unfenced entry, read mutation, yielding with a loan, foreign release, early
retirement, double release, nesting, double physical admission, early leave,
and leaked cleanup. The leak mutant must specifically fail the temporal
property. Seven expected-failing reachability invariants supply witness traces
for reader/writer loans, cancellation during a loan, recreation, interleaving
between turns, cleanup and a real data write. These controls check that the
baseline properties are exercised, rather than holding through unreachable
interesting states.

The runner pins the official [TLA+ tools 1.7.4 release](https://github.com/tlaplus/tlaplus/releases/tag/v1.7.4)
jar by SHA-256:
`936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88`.
Unexpected success of a mutant, the wrong counterexample, tool failure,
timeout, missing state counts or the wrong jar fails the verification job.

## Traceability and remaining proof obligations

| Model step | Implementation connection | Strength of connection |
| --- | --- | --- |
| Claim / Enter | Scheduler single-flight protocol; heap enter and generated method turn validation | SMT proves the actual `require_turn` decision; runtime and code-generation placement are execution-tested, not formally refined. |
| Acquire / Write | `CriticalStateLoan`, `CriticalHandleStateContract`, `ActorOwnedScalarState` | SMT proves policy admission; scope acquisition and actual data access enforcement have native regression coverage. |
| Unwind / Release / Leave | Lexical lowering, `CriticalStateLoan` cleanup and continuation-scoped state fencing | Abstract lifecycle safety/liveness checked by TLC; generated/native cleanup implementation is not mechanically refined to the model. |
| Stop / Retire / Recreate | Sealing, draining and new-incarnation protocol | Abstract final retirement requires quiescence. This is **not** an assertion that the raw C `ores_actor_i64_heap_retire` API itself waits for an active turn. |

The model's Yield is a conservative abstraction of interleaving *outside* a
loan; the current synchronous source profile does not support await. The
single-loan rule is the sealed actor scope contract, not a claim that every
low-level heap borrow API allows only one borrow. Return, throw and cancellation
share an abstract Unwind path; this does not introduce source exception support.
Model nonce exhaustion disables new acquisition instead of wrapping.

The trusted computing base includes Clang's compilation, the probe setup,
the small SSA translator, the independent SMT contracts, Z3, TLC, Java and the
model itself. Z3 native proof objects are saved but have not been checked with
an independent proof checker. TLC uses state fingerprints; the baseline run
reports an optimistic collision probability around `1e-10`, not a zero-risk
mathematical enumeration. There is no unbounded inductive proof of the entire
scheduler, C/C++ memory model, lock implementation, allocation/reclamation or
compiler lowering. ABI-name collision rejection and parser resource limits
remain regression-tested hardening, not newly formalized theorems.

JVM comparison evidence remains the separate 55-case parser/state/effect gate
comparison documented in [native parity](WITH_USING_NATIVE_PARITY.md). These
formal checks do not increase that comparison to complete Graal execution
parity or add reference/container state, async methods, general callback CFGs
or composable loans to LLVM.

## Reproduce

Install Z3 and Clang, and provide the pinned TLC jar with a working Java runtime.
No proof tools or jars are downloaded automatically by these scripts.

```sh
python3 tests/test_state_scope_smt_translator.py /path/to/z3
python3 scripts/verify_state_scope_smt.py --root . --output /tmp/state-smt \
  --clang /path/to/clang --cxx /path/to/clang++ --z3 /path/to/z3
python3 scripts/verify_state_scope_tlc.py --root . --output /tmp/state-tlc \
  --jar /path/to/tla2tools-1.7.4.jar
```

CTest registers the translator and compiled-predicate jobs when Z3, Clang and
Clang++ are found and `ORE_ENABLE_STATE_SCOPE_SMT=ON` (the default). Set
`ORE_STATE_SCOPE_TLC_JAR` to register the TLC job. Missing tools are reported
at configure time and the corresponding tests are absent, never green skips.
Use a dedicated formal CI job with the tools installed to require this evidence.

```sh
cmake -S . -B build -DORE_STATE_SCOPE_TLC_JAR=/path/to/tla2tools-1.7.4.jar
ctest --test-dir build -L formal --output-on-failure
```

Every runner removes any previous success summary before work and writes a new
`summary.json` only after every required proof, mutation and witness passes.
