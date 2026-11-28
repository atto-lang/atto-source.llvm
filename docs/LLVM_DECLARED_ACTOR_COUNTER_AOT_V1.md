# LLVM AOT Counter: four independent actor-owned turns (v1)

This milestone proves **real native LLVM-generated actor code**, with **separate
actor-owned persistent native heaps**, without claiming full namespace-actor
`declare` / `define state on actor` source parity.

## Executable native source

- `tests/fixtures/source-actor-counter-four.ores` is compiled by
  `oreslang-llvmc` to LLVM IR; Clang links the emitted actor initializer and
  `on_message` against `ActorOwnedI64Heap.c` and the native C turn driver.
- `tests/source_actor_counter_four_harness.c` enters separate actor-owner
  turns for **Counter 1001** and **Counter 1002**. It checks the first actor's
  `1,2,3,4` prefix sums **1,3,6,10** and the second's `7,8` results
  **7,15**, re-reads both, then retires and destroys both heaps.
- `tests/fixtures/source-actor-counter-four-mailbox.ores` is additionally
  compiled, linked and run under the **actual native actor scheduler** in the
  preexisting `source_actor_e2e.py` release/sanitizer CTest. Since STOP may
  cancel queued DATA before processing, receipt assertions validate the
  **processed FIFO prefix**, not an invented guarantee of four completed turns.
- `tests/source_actor_counter_four_aot.py` provides both optimized and
  ASan/UBSan verification and requires a nonempty compiler IR body, valid
  generated actor symbols, physical execution, and an exact result receipt.

## Checked state-loan substrate

`ActorOwnedScalarState::Access::add_checked()` validates the active
generation-fenced critical loan, exact i64 field type, signed overflow
bounds and strict no-mutation-on-failure. Its native tests prove an independent
four-message Counter total **10**, disjoint second actor storage, forbidden
foreign actor authority, read-only effect rejection, negative/positive
overflow rollback, and loan retirement on thrown exceptions.

This runtime substrate is **not** substituted for userland compiler codegen.

## Known unsupported boundary

The **new** namespace declarative syntax, including
`declare actor Counter;`, `define state on actor Counter`, and
`self.handle_state(critical-write |s| -> ...)`, is intentionally rejected
by the current restricted native LLVM frontend. The negative fixture
`declare-state-counter-native-unsupported.atto` requires an error and
**zero usable LLVM IR**. LLVM's legacy `let total` actor-field ABI is *not*
sealed-state/critical-loan MIR.

Full release requires a manifest-pinned multi-file linker for declarative actor
identity, a distinct typed state-access AST/MIR with nonescaping loans,
scheduler-proven acquire/release integration, guest-loaded state schema and
same-source Java+LLVM execution. This work makes measurable forward progress
on the actual LLVM compiler/runtime, not a claim those missing steps exist.
