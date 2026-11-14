# Guarded mutable sharing: `rt mut borrow` and `rt mut share`

Status: Java/Graal JVM-backed `SharedMutex<T>`-only slice; LLVM/native fails closed.
This document is a compiler admission contract, **not** a general actor-heap sharing
or allocation-domain transfer implementation.

## Semantics

- `rt borrow x` creates a scoped read-only loan; no allocation or object copy.
- `rt mut borrow x` creates an exclusive scoped mutable loan. Borrow conflicts,
  owner capability, escape, cancellation and suspension rules still apply.
- `rt share x` shares only values already approved by the existing copyable
  handle rules. It does not turn move-only object graphs into aliases.
- `rt mut share x` retains a **builtin `SharedMutex<T>` handle only**.
  This operation neither clones `T` nor yields a `borrow mut T`.
  It returns the *same synchronized handle* with its existing lock/guard API.
  Every mutable access to `T` must acquire an exclusive guard. Plain `Foo`,
  `Mutex<T>`, primitives, borrowed `SharedMutex<T>` views and unresolved
  generic capabilities must be rejected.

The underlying `SharedMutex.new(ownedValue)` establishes the shared-memory
domain and performs existing shared-safe type checks. Reusing the handle does
not authorize importing actor-private pointers, crossing a tenant boundary,
or serializing live mutable graphs into a mailbox. A shared allocation is not
a copy and is not both a stack-owned object and a heap-owned object.

`val` freezes the name, not necessarily the referent: legal field mutations
remain subject to binding permissions, exclusive borrows and guards. Mutable
sharing does not turn immutable primitive values into writable cells.

## Compilation

The AST distinguishes `mut_share` from ordinary `share` and `&mut`.
The type checker requires precisely the nominal builtin
`SharedMutex<T>` result; the ownership checker additionally denies shadowing
nominal class/type aliases, non-copy handles and borrowed values. The JVM
evaluator verifies an actual runtime `OresMutex.Shared` before returning the
same object. The existing lock/guard machinery owns synchronization and release.

No implicit conversion, `rt copy`, raw `&` or `*` operation is introduced.
No new actor message transport is implied. An explicit authorization for
shared-memory capability remains mandatory under the runtime's isolate policy.

## Compiler tests

Java: `MutableShareCompilerTest` has 36 authored JUnit tests covering distinct
AST forms, runtime handle identity (including nested sharing), exclusive-loan
non-upgrade, invalid mutable objects/local mutexes/borrows/generics, forged
nominals, restricted-operand admission order, nested capability denial, and
malformed spellings. These tests must run under Maven/JUnit; source-only
assertions do not establish execution.

LLVM: `native_borrow_admission` contains 23 `rt mut share` compiler-negative
contexts (31 native borrow denials total) and four canonical `.atto` real-IR positive controls,
including a canonical `rt mut borrow` control. Negative cases demand the
dedicated `rt mut share` diagnostic, **zero emitted IR**, no artifacts inside
or beside the scratch fixture, and a normal nonzero compiler exit (not a crash).
The Rust harness also has 28 authored source-free adversarial/predicate unit tests, including stderr IR, fast-exit output-budget, signal-termination, and parent-scratch artifact canaries.
These additions require exact-head execution; earlier successful CI runs
do not verify later commits. Native guarded `SharedMutex<T>` lowering remains
unimplemented; a parser rejection is *not* native shared-mutable support.

## AttoLang canonical source migration (2026-11-03)

**Public name:** AttoLang. **New source files:** `.atto`. The existing
`ores` Graal guest language ID, Java package names, native binary names,
old repository names, private CI receipt schema and protocol ABI remain
compatibility identifiers; changing them is not part of this ownership gate.

- Java positive and negative ownership fixtures now live in
  `src/test/resources/atto/ownership/*.atto` and are compiled with the
  actual Java parser/type/ownership checker. The positive case is also
  evaluated with Graal using its preserved `ores` language ID and a
  canonical `.atto` source name. A single explicit legacy `.ores`
  source-name regression remains. This JUnit path does **not** independently
  prove CLI source file admission or import resolution.
- LLVM's native borrow harness now writes every new compiler input as
  `input.atto` (31 no-IR/no-artifact denials plus four valid IR controls).
  Exactly one separate positive `input.ores` compiler control protects
  legacy compatibility. Two new source-free predicates prove the compiler
  harness passes the intended suffix; they are not real compiler passes.
- Canonical CLI/import source admission is being developed independently
  in Java PR #2460 and LLVM PR #1312. These source-branch implementations
  have **not been integrated or validated against these ownership heads**.
  Their exact-head code must be reconciled before an AttoLang guest parity
  or release claim.
- The latest exact-head ownership workflows did not record executed job
  steps. Compiler-backed/JUnit results remain `NOT_EXECUTED` at the new
  source commits. Older successful runs cannot be carried forward.
- The cross-organization private-source transfer hold remains active.
  Source-free test plans can pin Git blobs, but they do not authorize
  private compiler-source transfer into an alternate organization.

## Future admission gates

1. Shared native `SharedMutex<T>` storage/ABI, lifetime and drop ownership,
   memory-order and fairness proof, and guard-only write paths.
2. Complete borrow/guard liveness over `await`, exceptions, cancellation,
   actor turnover, hot reload and runtime suspension.
3. Cross-isolate/tenant boundary policy: ordinary actor heaps remain isolated;
   shared arenas need explicit runtime-minted cross-domain authority.
4. Matching positive/negative Java, LLVM, .NET, WASM and Forge guest fixtures,
   with executed exact-head native evidence before reporting backend parity.
