# Borrow safety verification gate (2026-10-13)

This document names exactly what is and is **not** verified by this repository's
`verification/borrow_protocol_model.py`. It is intentionally backend-neutral;
both the GraalVM Java checker and the LLVM compiler carry the same bounded model.

## Proof obligations

For every addressable owner root, at every source evaluation boundary:

1. An exclusive (`borrow mut`) loan cannot overlap a shared loan or another
   exclusive loan. Multiple shared loans may coexist.
2. An owner cannot be moved/dropped/reinitialized through an alias with a live
   loan; after a move it is not readable until a valid owned replacement exists.
3. No caller-owned ordinary loan survives a suspension or continuation handoff.
4. A called function's temporary argument loans remain active during evaluation
   of *all* later call arguments and are released at the synchronous call exit.
5. Borrowed values cannot be smuggled into owned containers, constructors,
   generators, channels, actor messages or escaping captures without an
   explicit, checked lifetime contract.
6. LLVM lowering must implement precisely the admitted ownership effects;
   unsupported reference, projection, lifetime and suspension constructs fail
   *before* emitting LLVM IR.

## Executed abstract model

The `borrow_protocol_model.py` transition system has two distinct owner roots
(one mutable, one immutable), a shared-loan count bounded by 2, an exclusive-loan
flag, an owner live/moved bit, and a suspension flag. It explores **every
reachable state until a fixed point**, not sampled test paths, and checks:

- moved roots have zero active loans;
- shared/exclusive loans never coexist;
- suspension never exposes live loans.

Three mutation controls deliberately violate each rule. The checker *must*
discover a concrete shortest counterexample for every mutation. This is a
finite-state model check with a bounded loan count and root count; it is **not**
a proof of unbounded checker soundness or an equivalence proof for Java/C++.

## Refinement and implementation tests

- **GraalVM:** `BorrowFormalBoundaryRegressionTest` exercises actual parsed
  AST ownership checking of direct and delegated `yield`, constructor escape,
  and a valid owned-scalar yield. Run with the full Maven/JUnit suite, GraalVM
  Native Image, actor continuation and re-entry tests.
- **LLVM:** CTest `native_borrow_admission_formal_refinement` runs the real
  `oreslang-llvmc` front end, checking that overlap errors yield **no IR** and
  two legal shared/sequential loan cases emit LLVM IR. Run the full native
  test suite plus ASan/UBSan where supported.

These tests are *finite samples* of the refinement obligation. They do not yet
establish the necessary simulation relation between all AST/CFG transfer rules
and abstract model steps. No unbounded machine-checked theorem for the entire
Oreslang borrow checker is claimed.

## Follow-up formal program

Prove (using a proof assistant or a checked transition-system/refinement proof)
that each compiler transfer and every CFG join conservatively simulates this
root-loan automaton. Then extend the model to projections/alias provenance,
interprocedural calls, exceptions and `defer`, futures/generators, actor
cancellation and carrier handoff, and LLVM region generations. Until a safety
rule is proven and source-to-IR validated, keep that feature fail-closed.
