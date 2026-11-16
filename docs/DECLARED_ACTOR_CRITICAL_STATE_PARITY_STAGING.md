# Declaration-only actors and sealed state: Java ⇄ LLVM staging

**Status: DRAFT — LLVM has no executable cross-file `declare actor` lowering.**
Tracks Java PR [#2901](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/2901).
Do not mark Java↔LLVM source actor parity complete from host-only gates.

## Implemented LLVM changes

1. The native parser now recognizes both `namespace foo;` and contextual
   `use namespace foo;` as the same **flat namespace identity**.
2. For the first AOT namespace execution slice, a standalone zero-argument
   `main(): int` whose *whole body is a literal int return* emits LLVM IR.
   The CTest AOT gate compiles that IR with Clang and checks OS exit code 42.
   Namespace calls, imports, classes, actors and multi-file guest linkage
   remain forbidden, so this does not create a namespace spoofing vector.
3. Native `declare actor` source rejects with
   `DECLARED_ACTOR_CODEGEN_UNSUPPORTED` and produces **no partial IR**.
   This is intentional until the native parser, source-set builder, typed
   external methods, state-layout ABI and checked scheduler mailbox dispatch
   are linked together.
4. A native C11 test invokes the existing `OresActorStateRwAdmission.h`
   gate on one scalar Counter state. It performs two authenticated exclusive
   writes and an authenticated read (result 2), overlapping readers,
   queued-writer fairness, cross-incarnation/owner rejection, denied effects,
   suspended-loan exclusion, STOP sealing and physical quiescence.
   This is a **native runtime host test**, not the execution of a compiled
   `.atto` actor or a general scheduler proof.

## Shared physical source contract

Five literal `.atto` fixture files under
`tests/fixtures/declared-actor-namespace-v1/` express one actor contract,
one declaration-only actor, one sealed state attachment and separate
`increment()` and `get_count()` external `mtd` methods.

Every file repeats `namespace foo;` or `use namespace foo;`. The
declaration, source files and method selectors have to be resolved by a
**complete, manifest-pinned, closed source set**, never by filename or
ambient filesystem coincidence. The Java compiler draft implements that
static source-set proof, but does not execute declared actors.

## Next mandatory LLVM guest admission

- Implement manifest/source snapshot and exact package identity, with
  immutable digests and source-file provenance.
- Represent nominal actor contract, external receiver methods and sealed
  state in the native AST; validate signatures, effects, const/let layout,
  method bodies and critical callback AST without string-based name spoofing.
- Lower an actor-owned state image with exclusive writer/read tickets,
  checked arithmetic, exception-safe release and trusted generation-fenced
  scheduler turn context. Do not expose raw state pointers to guests.
- Generate native LLVM code for the five physical fixtures; use the actual
  actor scheduler/mailbox to call `increment()` twice and `get_count()`
  once. Check result 2, cancellation/STOP, stale generation and sanitizer
  behavior at both optimized and unoptimized builds.
- Add exact-head Java/GraalVM ↔ LLVM differential receipts with the same
  fixtures. Reuse the shared conformance contract with Wasm, Forge and .NET.

No private sources were mirrored to another GitHub org in this stage.
If the source org cannot assign CI runners, record zero-step failures as
**runner admission failures**, not as executed test passes or compiler
regressions.
