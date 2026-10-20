# Native ownership-loan bounded model (2026-10-19)

`NativeOwnershipLoans.tla` and `NativeOwnershipLoans.cfg` define a two-loan
TLA+ state model: shared/exclusive admission, identity-sensitive release,
move/restore and suspend/resume.

Safety invariants:
- No exclusive loan overlaps with any shared loan.
- A moved value carries no active loan.
- Suspension cannot retain an active shared/exclusive loan.

`tests/formal_ownership_loan_model.py` independently exhausts the same finite
transition system and injects an intentionally invalid suspend-with-borrow
transition as a negative control. CTest runs the Python checker by default.
Run manually with `python3 tests/formal_ownership_loan_model.py`.

For a separate TLC check, use a pinned TLA+ tools installation and invoke
`java -cp tla2tools.jar tlc2.TLC -config NativeOwnershipLoans.cfg NativeOwnershipLoans.tla`
from `docs/formal`. **No TLC execution is claimed** by this branch.

Both models are *abstract* invariants; neither proves that C++ ownership CFG
lowering implements the transitions faithfully, that all heap/field projections
share canonical loan identity, or that async continuation lifetimes are safe.
Add executable LLVM source-to-CFG refinement cases and backend parity before
raising the proof status.

## Concrete CFG suspension refinement gate

The audit branch adds the explicit `Operation::Kind::Suspend` event to the
LLVM ownership CFG checker. The event has **no place**, and rejects any live
shared/exclusive loan in the full current CFG state, including projected fields.
Positive and negative native C++ tests exercise the source-independent CFG
transition, establishing a testable refinement obligation against the TLA+ model.
The LLVM source frontend does **not** yet lower all await/suspend constructs
through this gate; therefore this is not evidence of guest Task/await safety.
