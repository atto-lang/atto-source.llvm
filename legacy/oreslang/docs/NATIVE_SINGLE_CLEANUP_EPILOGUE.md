# Audit: single native return cleanup epilogue

The LLVM compiler previously cloned each lexical owner-drop ladder,
each `defer` call sequence and each region close into *every* guarded
return block and the final return block. That made code size proportional
to `(early_return_count + 1) * owned_local_count`, even though all
admitted locals are constructed before the first return guard.

Functions with guarded returns or deferred cleanup now generate one `exit.cleanup` block. A function with only a final return and no registered defers emits its one cleanup sequence inline to preserve direct-return IR compatibility. Every
taken/fallthrough return evaluates its result **before** cleanup,
captures the value into a private scalar return snapshot and branches
to the shared epilogue. The epilogue runs LIFO deferred calls, reverses
lexical slot cleanup, tests native owned-region `i1` live flags, drops
live native allocations and closes the region once before returning
the captured value. Void functions skip the return snapshot.

## Safety properties

- A deferred mutation of an object cannot change a return value
  already evaluated on an earlier CFG edge.
- Cleanup is emitted once per local, independent of the number of
  return guards; a taken `rt take` still retires the old region slot
  immediately and disarms its flag before entering the epilogue.
- Later region allocations may reuse an earlier moved slot without
  a stale owner dropping the new allocation, since flags belong to
  source owners rather than native allocation addresses.
- LLVM LifetimeEnd remains only a lexical lifetime marker for stack
  classes; this does **not** implement general nontrivial class drops.
- Source exception unwinding, cancellation, pinned continuations,
  nontrivial branch-local class construction and class-valued ABI moves remain
  unsupported/fail closed.

## Executable release gates

`native_single_cleanup_epilogue` and its Clang ASan/UBSan version:
source-to-native programs for 1, 8 and 64 owned locals with eight
return guards, `rt take` on each possible return, owned region slot
reuse, and a two-call deferred class mutator preserving evaluated
returns. Assert exactly one generated native region-close call per
function, rather than copied cleanup per return, plus correct exit
codes. Continue requiring complete CMake/CTest, shared 11-fixture
LLVM semantic conformance and Wasm32 admission.
