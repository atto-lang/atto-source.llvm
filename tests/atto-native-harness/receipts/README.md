# LLVM native guest receipt smoke (partial)

The two AttoLang `.atto` programs each perform a real integer assertion,
decide their own outcome, print their own ordered `ORES_TEST` event and
summary, and return a matching OS exit. The host uses LLVM IR, Clang O0/O2,
`NativeStringRegion.c`, and a real stdout pipe. The verifier does not
generate any test events.

These two independently authored, one-case programs are **not** the full
`ores.std/testing` module or Java `TestCase`/`TestSuite` parity. Native
LLVM import of `ores.std/testing` must remain rejected until the runtime
owns correct generic counting, names, skip/filter/fail-fast, and nested suites.
A source-only verifier self-test proves formatting rejection, not guest
execution. Zero-step GitHub Actions jobs remain `NOT_EXECUTED`.


## Two-case probes

`multi-pass.atto` and `multi-fail.atto` each emit two per-case status records
and one final summary. Together with one-case pass/fail, these require eight
native LLVM/Clang processes at O0/O2. The verifier binds actual native exit,
ordered bytes, and a no-follow bounded read; this does not mean full TestSuite
or Actor TestKit parity.

The `bounded_testkit.atto` experiment adds four M0 suites using nominal `TestCase` and `TestSuite` values, runtime-computed outcomes and bounded two-case summary accounting. It deliberately does not expose unrestricted case-name input or claim full `ores.std/testing` parity. These must pass the real production compiler and Clang O0/O2 before being counted as executable evidence.

## Compiler-owned AttoLang M1 ledger (source-authored, native evidence pending)

`stdlib/testing_m1.atto` is the canonical source for the compiler-owned
`ores.std/testing/m1` module. It defines bounded scalar `TestCase`,
`TestSuite`, and `RunSummary` classes, plus `should_run`, `record`,
and `finish`. A byte-exact source contract ensures the embedded compiler
source cannot diverge from the checked-in `.atto` file.

Five additional guest programs exercise actual scalar assertions, two passes,
fail-fast, filter/skip accounting, skipped-only and filtered-only exits.
The non-first-class API makes the caller explicitly check `should_run`
before computing a body and supplies its outcome to `record`. Status
receipts are printed by each compiled guest after verifying the returned
counts. This is a **partial native ledger**, not the Java `run_suite`
contract: callable TestBody, arbitrary names and escaping, duplicate-ID
fencing, nested suites and arbitrary-size test lists are still unsupported.

The combined runner now requires 13 guest programs at both O0/O2 =
26 native processes, in addition to the 80 scalar processes. **106 required,
0 verified until stepful exact-head native CI**; 10 negative compiler
admission checks also remain unexecuted. Hosted-only import of M1
remains forbidden in freestanding targets.

## LLVM M2: restricted registered-body execution

`stdlib/testing_m2.atto` is a compiler-owned, hosted, source-level AttoLang
module that accepts `RegisteredCase` descriptors. Cases choose one of four
pure scalar assertion operations through a numeric body opcode. M2 runs the
assertion *inside* `run_one` after filtering, skipping, fail-fast,
duplicate-admission, ID and quota gates. The `run_pair` convenience routine
prechecks two integer IDs and admits at most two cases. No indirect function
values, user closures, actor transport or runtime JIT is introduced.

The six `m2-*.atto` native candidates test ordered dispatch, failure and
fail-fast, skip/filter non-execution, invalid-body atomic rejection, duplicate
ID rejection, and once-only admission; each checks invocation counts and
returns a truthful native exit before emitting static scoped case identities.
This is NOT the Java `TestBody` callable contract or dynamic suite hierarchy.
Canonical `std/testing` remains deliberately unsupported.

Combined authored requirement: **40 scalar × two + 20 receipt suites × two
= 120 native executions**, plus **11 compiler-negative scenarios**. Run
classification remains `NOT_EXECUTED` until real, source-pinned compiler
and O0/O2 executable jobs produce stepful receipts under an authorized runner.


## Replay-fencing regression cases (Nov 4, 2026)

`m1-replay.atto`, `m2-replay.atto` and `m2-out-of-order.atto`
cover the previously missing distinct-instance replay gap. M1 `record`
uses `TestSuite.last_id` to enforce strictly increasing integer IDs for
selected, skipped **and filtered** cases. M2 `run_one` checks this before
invoking a scalar body; `run_pair` performs complete identity and visited
preflight before either body executes. All rejected paths preserve the
previous counters and accepted ID. This scope deliberately does not claim
string-name uniqueness or arbitrary callable registration.

**Latest authored gate:** 23 receipt programs × Clang O0/O2 = **46**
native guest processes, plus 80 scalar executions = **126 required**,
and 11 compiler-negative scenarios. No exact-head native pass is counted
without a source-authorized, nonzero-step compiler-backed CI job.


## M1/M2 accounting-integrity hardening — November 4, 2026

This section supersedes older numeric gate totals in this evolving PR.

**Authored native acceptance:** 40 scalar .atto programs at O0/O2 (80
native executions) plus 25 receipt-producing .atto guests at O0/O2
(50 executions) = **130 native processes**, plus **11 negative compiler
scenarios**. None counts as a pass without exact-head compiler/IR-auditor/
Clang/native execution receipts from a real runner.

M1 previously admitted structurally inconsistent public counters into
record and finish. M1.valid_state now checks bounded nonnegative totals
and per-status counters BEFORE their addition, preventing signed overflow;
it checks accounting reconciliation, monotonic-ID coherence and fail-fast
halt invariants. The pure preflight is shared by should_run, record and
finish; finish leaves the old summary untouched on invalid state. M1
quota-probe behavior preserves an explicit -2 result even with synthetic
full ledgers. M2.run_one validates the M1 suite BEFORE scalar body
execution in addition to its existing ID/visited/quota checks.

The m1-finish-integrity.atto guest exercises negative, oversized,
inconsistent and forged fail-fast counters. The new m2-corrupt-state.atto
guest verifies those counters cannot trigger body invocation and that
corrected state recovers. Public writable guest fields still do not
constitute tamper-proof receipt attestation.

The native guest-output runner now caps stdout WHILE CAPTURING with a
4,097-byte reader (head -c 4097), instead of writing unbounded guest
stdout to disk before verification. Payloads above 4,096 bytes,
SIGPIPE/nonzero process exits, stderr, wrong bytes or wrong exit status
fail closed. The source-contract mutation test removes the capture
bound and must reject it. This is a resource guard, not an executable
test pass.

A separate private gha-indie-worker source-free compiler canary admitted
real GitHub-hosted steps. Cross-organization copying of private compiler
Git trees remains prohibited by unresolved independent security approval,
destination controls and incident/retention disposition. No private
source or credentials were mirrored to another organization.
