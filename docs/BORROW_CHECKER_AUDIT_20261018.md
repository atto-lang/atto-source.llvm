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
