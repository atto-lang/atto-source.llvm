# Unresolved callable effects: native fail-closed contract

Date: 2026-10-20. Companion Java hardening is stacked on Java formal
PR #1604, on branch `audit/effect-call-resolution-boundary-20261020`.

Native `FunctionEffects` is a bitset for verified, executable effect
permissions: state.read/write, suspend, io, alloc, block, ffi, throw,
raise and panic. It does **not** yet implement Haskell `IO` action values,
algebraic handlers, effect-polymorphic HIR, or complete dynamic-dispatch
effect inference.

## Problem

An unresolved callback, unknown overload, dynamic dispatch, or unverified
external call must not be assigned an empty native effect row. Empty means
an independently proven effect-free operation; lack of evidence is not
the same thing as purity.

## Hardening slice

`FunctionEffects::unresolved_call()` produces the internal marker
`UnresolvedCallBit` (bit 31). This marker is deliberately **outside**
`KnownBits` and is not parsed as a guest `with` effect.

- `valid()` rejects an unresolved effect row.
- `permits(unresolved_call())` returns false for **every** user-declarable
  permission set.
- A forged declaration with the same unknown bit also fails, even when
  the bitwise subset relation would otherwise be true.
- `united()` conserves the marker through effect-row unions.
- Parsing `unresolved_call` as a source-level grant fails.

This is a static enforcement primitive, not executable source codegen.
When a typed LLVM caller cannot supply verified target/effect evidence,
its compiler effect summary **must** carry `unresolved_call()`. An
open-ended dispatch or absent imported metadata cannot be silently
downcast to `FunctionEffects{}` or a guessed pure target. Error reporting
should preserve a separate diagnostic witness/target label; the bitmask
is not a substitute for a provenance graph.

The existing native test exhausts 2^10 × 2^10 known permission pairs.
New negatives check that all 1,024 declared rows reject unresolved
calls, unions do not erase the marker, and matching forged bits cannot
self-authorize.

## Remaining blockers

1. Derive complete effect facts from the typed LLVM HIR, including indirect
   calls, callbacks, trait dictionaries, implicit runtime operations,
   macro expansion and imported/FFI boundaries.
2. Separate static effect rows, actor `using` capability loans, and
   `critical` admission. A `with` clause cannot grant authority.
3. Carry latent effects in callable, Future, Task, Observable, Stream and
   Haskell-style IO action values. Execution and subscription must recheck
   those effects, not just construction.
4. Prove Java/Graal and LLVM agreement for source acceptance and runtime
   effects. Java's `UNKNOWN_CALL` category and native's invalid-bit marker
   are intentionally different internal representations of the same
   fail-closed policy.
5. Preserve cancellation, `raise`, `panic` and actor supervision across
   handlers; only compiler-verified `trap` masks ordinary `throw`.

**Verification:** compile and execute the exact source revision via
`cmake --build <build> --target ...` / CTest. The standalone
`tests/function_effects_test.cpp` may also be built with
`c++ -std=c++20 -Isrc/main/cpp tests/function_effects_test.cpp -o /tmp/effects-test`
and executed. Exact-source compiler/CI parity is a separate gate.
