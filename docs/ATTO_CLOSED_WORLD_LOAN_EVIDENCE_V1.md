# AttoLang ownership: closed-world NLL and write-ticket fences

**DRAFT. Production LLVM native CTest and compiler-backed guest tests have not executed. This is not sound source-level NLL.**

## Production CFG hardening

`OwnershipCfg.hpp` now carries a separate optional `write_id` on `Operation`. The `Reinitialize` transition rejects absent/zero write-site IDs, mixed loan-ID metadata, duplicate write tickets anywhere in the CFG (including currently unreachable blocks), and write metadata on any other operation. The existing move-state and live-loan preconditions still apply. **This ticket is only a required compiler-generated claim, not a proof that a guest field assignment actually occurred.** No source-to-guest lowering is permitted to invoke this transition without a separately established MIR write and the matching emission receipt.

`OwnershipLastUse.hpp` rejects all unclassified `Read` and `ReadObject` operations that follow a tracked lexical borrow, including owner reads that might be safe. This deliberate loss of precision prevents an omitted borrowed-use annotation from allowing premature release. `lower_closed_world_loan_tokens_v1` derives the last `UseSharedLoan`/`UseExclusiveLoan` offsets from the bounded one-block token operation stream; it rejects duplicate, missing, mismatched, unminted or manually released token histories. Its result is passed through `lower_explicit_last_use_evidence_v1`, which inserts releases and reruns the regular ownership CFG verifier. This derived stream is **not authenticated SSA completeness** and is not called by guest lowering.

## Tests

`tests/native_ownership_nll_closed_world_16_test.cpp`: 16 new host C++ negative/positive cases for distinct write tickets, zero/missing ticket rejection, duplicate write-site denial, loan-token impersonation, unclassified ordinary reads and eventual typed release.

`tests/native_ownership_derived_last_use_12_test.cpp`: 12 host C++ cases for token-derived final use, repeated uses, multi-token streams, wrong modes, mixed releases, loops and opaque reads.

The original 30 C++ last-use cases have been reconciled with required explicit write tickets and the conservative read fence. CMake registers both new tests, and the exact-head source-owned LLVM workflow now requires **eight focused native CTests**, in addition to previously authored scoped `.atto` compiler fixtures and the real 50-program LLVM/Clang guest matrix.

**Evidence limitation:** all these native compiler/test cases are authored but **NOT_EXECUTED** because the source-org Actions jobs have zero steps. Never promote a CTest inventory check or source-only analysis to source-to-guest parity. Require immutable head/tree, nonzero executed steps, C++ compilation against real production headers, actual negative semantic diagnostics, LLVM IR+Clang guest receipts, and independent review before merging. No private source copied to another organization.

## Next compiler work

Generate the complete tokenized use-def stream from typed SSA; fence opaque call/capture/exception/cancellation edges; emit actual writes for restored fields; prove ActorRef/tenant/suspension generation ownership. General NLL through branch/loop joins remains unimplemented.
