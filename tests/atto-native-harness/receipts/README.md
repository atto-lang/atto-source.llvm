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
