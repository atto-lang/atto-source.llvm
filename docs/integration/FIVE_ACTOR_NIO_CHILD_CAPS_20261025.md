# Five-PR actor NIO + child-capability semantic integration (2026-10-25)

**Status: review-only DRAFT, NOT a mainline merge or executable parity receipt.**

Source inputs and actual dependency order:

1. [#967](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/967), `d4dd483bc72fab796b545677e40014eaa73e8655`: C++ reactor watch ownership with exact NativeActorRef and physical loans; stacked on **#951** terminal-loan seal.
2. [#977](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/977), `c632eed0042824a94f41c79e6dac4b6e9596a99f`: C VM resource-loan count and physical DONE on the #967 substrate.
3. [#985](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/985), `459c46b084462f427bbb66bb7599473b1bc7e897`: actor-authorized, generation-fenced VM-owned epoll watch bridge on #977.
4. [#989](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/989), `0ec8dc0e72c7169b04aba3b919abda26a00b125b`: block standard-I/O name laundering while unimplemented source I/O remains fail-closed, stacked on #985.
5. [#965](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/965), `147b748f2f7484a2ff9ffa8b55ff4901d400eed6`: immutable generation-fenced actor child-capability fields; applied on top of #989.

The branch inherits the first four PRs directly through the #989 Git ancestor
chain, including pre-existing **#951**. The #965 delta is the only
additional feature forward-port. No earlier C scheduler/actor runtime source
was replaced to impose that feature; the source and destination byte-identical
main-base blobs were verified for `SourceActorBootstrap.c/.h`,
`LlvmCodegen.cpp`, `Parser.hpp`, native scalar/ownership checkers, and the
source actor E2E harness.

## Concrete semantic collision resolved

The #989 parser adds a closed set of 18 standard-I/O callable names that user
functions/methods cannot redeclare. The #965 parser adds actor child ActorRef
fields, checked child spawn/cycle/footprint, and capability message syntax.
Overwriting either entire parser would silently discard the other contract.
All **seven** #965 parser hunks matched an exact, unique old-context window
in #989's parser; this composed both sets of rules without changing the
reserved I/O list or diagnostic. The one #965 CMake hunk likewise matched
exactly once, adding the capability C runtime CTest without removing the two
IO-name-laundering negatives or NIO lifecycle CTests.

Other #965 production files and source E2E tests were imported as their exact
Git blob identities because the prior branch content matched the #965 original
main-base blob. Six child-capability guest fixtures and the C host regression
were added as new files.

## Nonnegotiable ownership and lifecycle rules

- Reactor/VM watch capabilities must authenticate the **exact actor
  incarnation and watch claim generation**, not an FD, numeric actor ID, or
  name alone. The single retained scheduler resource loan is released once
  on true receipt completion; STOP/cancellation cannot invent an early DONE.
- Actor child references are immutable capability fields, not ordinary guest
  scalar/pointer state; construction is checked against the actor-owned
  heap, child graph cycles/size fail closed, and handler sends authenticate
  live ownership. Reject mutable or unknown child refs with no emitted IR.
- Child stop/join and reactor loan retirement are distinct obligations.
  Parent reclamation waits for its own physical DONE and child supervision
  cleanup; source-level NIO remains unavailable until lowering can prove
  the ownership path.
- The 18 standard-I/O callable spellings remain compiler-owned, including
  qualified method declarations. No fake user-defined synchronous
  `read_text_async` / `connect_async` / HTTP/stdio callable should pass.
- At most one guest turn per actor, CONTROL and USER resources remain
  separated, and none of these changes authorize cross-actor mutable access.

## Integration checks performed (structural only)

- Exact source/blob identity preserved for 14 #965 files, with two
  uniquely reconciled textual overlaps (CMake and parser).
- CMake registers `source_actor_capability_runtime`,
  `native_vm_nio_lifecycle_and_rt_prestart`, and reactor generation
  tests plus both I/O-name laundering negative compiler fixtures.
- `tests/source_actor_e2e.py` includes the positive child-capability
  program and rejects mutable, mismatched, unknown, cyclic and excessive
  spawn-graph capability fixtures.
- Existing C VM NIO fixture and source actor ownership/generation gates
  remain in the tree.

These are structural assertions, **not actual compilation or execution**.
No successful exact combined-head GitHub Actions runner job was observed in
the source repository; sampled #965 and #989 jobs failed with `steps=null`.
Pre-step failures cannot certify or refute the compiler changes.

## Merge/CI requirements

1. Independently review the exact combined head and dependency base, especially
   #951's terminal loan policy and any changes to current `main`.
2. Run CMake full build and CTest inventory; execute the native
   `source_actor_capability_runtime`,
   `native_vm_nio_lifecycle_and_rt_prestart`, and reactor generation
   CTests with normal GCC/Clang and applicable ASan/UBSan/TSan.
3. Run positive actor-child source compile to LLVM IR to Clang execution;
   assert negative child-capability and I/O-laundering fixtures emit no IR.
4. Stress STOP/watch completion/ABA/resource loan and constructor-owned child
   cleanup (including combined lifetime interactions) before asserting DONE.
5. Capture exact Git tree/commit, allocated runner and executed steps,
   test outputs, code review and merge status. Keep draft if missing.

Private source **must not** be mirrored to other GitHub organizations without
separate reviewed destination allowlist/carrier authorization, protected refs,
and resolution of source-exposure incidents. All staging writes here remain
within the original private LLVM repository.
