# Borrow checker audit and shared Java contract

`tests/fixtures/borrow-contract.tsv` is byte-identical to the Java backend's
`src/test/resources/borrow-contract.tsv`. Eighteen source cases exercise shared
and exclusive loans, aliasing versus distinct owners, nested argument calls,
loan release and reborrow permissions. Admitted cases compile to LLVM IR and
execute to 42. Rejected cases must fail with a borrow diagnostic and emit no IR.
The test harness remains effective under `python -O`.

The C++ checker acquires loans in lexical argument order and retains all of them
until the call finishes. Generated instance calls insert the receiver as the
first explicit loan. Shared-to-exclusive escalation is forbidden; an incoming
exclusive loan can be temporarily reborrowed. Loan checking happens before IR
emission. The audit corrected an obsolete comment that incorrectly said
reborrows were unsupported; it does not introduce an alternative ownership path.

The corresponding Java audit found and fixed a missing loan around projected
method receivers. Native LLVM currently admits only named scalar/class receiver
storage; projected method receivers and escaping references remain rejected.
Do not broaden this boundary until storage provenance, destruction and loan
lifetime lowering are implemented and tested together.

Other existing native tests exercise field-sensitive ownership CFG fixed points,
conditional drops, moved/partially moved owners, owner regions, multiple fields,
receiver privacy, deferred effects and ASan/UBSan execution. Native ownership uses
deterministic destruction and no GC. These tests establish bounded properties;
they do not prove all source programs safe or establish hardware memory isolation.
No tracing collector, interpreter or JIT is added by this audit.

Validation on macOS: 154 Java tests across 23 ownership/borrow/lifetime/escape
suites passed on Temurin 25; 421 native CTests passed, including native execution
and sanitizer cases. The common 18-case corpus SHA-256 is
`cc42b9bd3c4ff27033eecb526aae25e29bf8d176915ff0fd9e6abe33167f9cbe`.
This audit's new changes have local evidence; earlier fallback CI results apply
only to their recorded source commits, not automatically to this head.


## 2026-10-26 static escape regression gate (draft)

`native_compile_time_borrow_escape_admission` registers the existing native
source harness in CTest, adds by-value call/constructor loan rejection cases,
and adds a negative generator feature gate. It requires each rejected source
file to produce nonzero compiler status, no stdout/LLVM IR, and a
**specific ownership or unsupported-generator diagnostic**.
It also requires three supported positive controls (shared borrowed parameter,
owned scalar call and owned class constructor) to produce LLVM IR, preventing
broken-parser or unconditional-rejection false positives.

This is **not** evidence that native generator lifetime checking is
implemented: generators are deliberately unsupported by the native
frontend. Nor does this gate prove Java/Graal parity or runtime zero overhead.
Exact-head CMake/CTest execution and sanitizer runs are required before merge.

### Mainline Task/Future pipeline reconciliation

The source admission gate is registered on the latest `main` CMake
configuration without replacing its Task/Future compiler, runtime, or test
entries. The current native parser explicitly rejects `generator` source
with the diagnostic `native generator v1 requires iterator-frame, ownership
and LLVM continuation lowering`; the regression fixture matches that
specific unsupported-feature admission rather than the earlier generic
actor/async rejection. This documents **fail-closed native generator support**,
not a proof that native generator lifetime tracking is implemented.

New native Task/Future support must be separately tested for loan lifetime
and ownership parity; these fixture checks do not certify that newer source
subset. CI is required on the merged exact tree.

### Native positive-control anti-vacuity check

Positive admission fixtures require a line beginning with the LLVM IR
`define` instruction, not merely the substring `define` somewhere in
compiler stdout. The three valid scalar controls prevent a parser-only
rejection path from being mistaken for ownership verification. This
strengthens the *test harness*; it is not a substitute for running it
against the production compiler on the pinned source tree.

### Rejected-source artifact isolation

Negative compiler admissions now run with their current working directory set
inside the disposable fixture root, compare the directory contents before
and after compilation, and verify the input source bytes were not changed.
A rejected program only passes when its compiler status is nonzero, stdout
contains no IR, the ownership-specific diagnostic appears, and the compiler
has not written extra files or modified the fixture. These checks are
harness hardening, not evidence of native compiler execution.

### Rust admission harness on current main

`tests/native_borrow_admission.rs` replaces the earlier Python runner. It
preserves the seven original fixtures and three positive controls, with
per-process time/output bounds, rejected-source directory/file checks and
input-byte integrity. An additional canonical `gen*` borrowed-yield fixture
checks the current literal-yield profile's rejection; the legacy generator
fixture now expects the current grammar diagnostic. Neither generator
rejection proves general generator loan lifetime analysis. CTest builds both
the real-compiler runner and predicate tests with Rust warnings denied.
