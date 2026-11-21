# Canonical Counter V2 — native LLVM frontend admission status

**Status: source syntax and typed-plan admission only, NOT executable guest parity.**

This branch carries the exact seven source units from the common
`actor-counter-identical-source-v1` manifest. The source files have matching
Git blob SHA-1 identities in LLVM, Java #3008 and .NET #502; each source unit
is materialized under `contracts/actor-counter-identical-source-v1/`.

The compiler-owned `CounterV2Source.hpp` supports a bounded, independent
native C++ lexical/type verifier. `oreslang-llvmc --counter-v2-check ROOT`
checks all package files together and emits a receipt explicitly containing
`guest_executed=false` rather than any pretend LLVM IR or native program.

Checks cover:
- nominal actor, contract, external receive, state attachment, method linkage;
- authenticated-source critical-write of signed i64 actor state;
- detached `val int snapshot = self.handle_state(critical-read |s| -> s.total)`;
- `self.send(snapshot)` **after** that lexical read scope ends;
- four typed mailbox sends, early/terminal STOP, checked i64 accumulation;
- four awaited output reads and ordered writes to stdout;
- bounded UTF-8 source ingestion, symlink denial, nested-comment budget,
  immutable state field naming, no unverified executable suffix.

The unit test `native_counter_v2_declared_source_admission` uses the same
verifier, checks the canonical source plan, and rejects eleven malformed-source
mutations. The Rust source-manifest pin independently verifies the full JSON
Git blob, all seven materialized Git blob hashes and tamper refusal. The new
CMake test `llvmc_counter_v2_source_only_check` invokes the production
native compiler CLI, not a host simulation.

**Missing before executable parity can be claimed:**
1. Real parser integration into LLVM's ordinary compilation-unit and typed
   actor MIR, with borrow/suspend/effect checking, including private
   external receive ingress.
2. Actor-owned state layout and checked arithmetic lowered to native LLVM IR,
   with runtime lifetime tokens and guaranteed read/write loan release.
3. Actual async root continuation, observable mailbox replies and terminal
   future completion, without thread parking or host-side replay.
4. An executable compiled guest with exact `1|3|6|10` stdout, STOP/generation
   fencing, two-actor isolation, malformed-mail rejection and heap reclamation.
5. Exact-head stepful CI: LLVM O0/O2 plus ASan/UBSan, Java JVM/GraalVM and
   .NET NativeAOT, each compiling the identical package. A source-only check
   or a failed job with zero executed steps never certifies parity.

Until these gates exist, this work remains a **draft compiler admission
milestone**, and the shared conformance manifest stays
`CANDIDATE_NOT_EXECUTED`, with `verifiedBackends=[]`.
