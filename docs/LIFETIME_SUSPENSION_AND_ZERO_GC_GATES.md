# Native lifetimes, suspension, and zero-tracing-GC gates

**Status:** compiler *ownership CFG* hardening; not guest-source lowering, not a
lifetime-parameter syntax implementation, not a full zero-GC or hard-real-time
certificate. Stacked on the task-bound region prototype in LLVM PR #1192.

## Lifetime model

- Values have one logical owner. `rt take` invalidates the source, `rt borrow`
  grants a scoped read-only loan, and `rt borrow mut` requires exclusivity.
  Ordinary Copy scalars do not require heap allocation.
- Compiler-inferred lifetimes are initially sufficient for locally scoped
  borrows. Do **not** introduce Rust-like `'a` source syntax until there is a
  documented need for lifetime relationships that inference cannot express.
- Each non-lexical loan has a stable compiler-issued site/loan identity; the
  CFG verifies its release against every predecessor and exit. A loan on a
  projected place cannot alias a whole owner without a proven projection tree.
- Region-backed loans retain a symbolic *region binding*, must bind to the
  authenticated current logical execution, and require an execution generation
  fence. Guest code never supplies actor IDs, region handles, or generations.
- `await`, `select`, channel waits, generator suspension, and similar events
  must lower to an explicit **Suspend** ownership-CFG operation. The current
  verifier admits suspension only when **no loan is live**. A future positive
  cross-suspension path must verify the same pinned actor-owned frame stores
  both the owner and borrow, their generation and drop order, and has a
  physical retirement acknowledgment. The current `Suspend` operation has
  no bypass flag, forged provenance or implicit pin privilege.
- Every path (normal return, error, `throw`/`raise`/`panic`, timeout, cancel,
  actor STOP) must run exact-once release/drop glue before its owner region
  is reclaimed. This path-sensitive source/LLVM integration remains pending.
- Shared immutable data crossing domains needs separately proven lifetime
  and transport ownership; `rt share` does not create a tracing GC or
  permit a borrowed native pointer to escape an actor.

## Native zero-GC architecture

Use stack values where possible; otherwise allocate owned native values in
deterministically retired lexical, Task or actor-owned arenas. A native actor
arena is a region owner, **not** a garbage collector. Cycles wholly within
an actor region must not prevent its eventual bulk reclamation.

The strict LLVM no-tracing-GC contract means no transitive tracing collector
is required in the linked guest execution path. This does **not** mean there
are no `malloc` calls, OS page faults, allocations during startup, or hidden
allocations in arbitrary FFI dependencies. Hard-real-time entry is a strictly
stronger claim: bounded/approved memory use, no unbounded locks/blocking,
no unexpected page faults after pre-fault/locking, budgeted work and bounded
destruction must be separately demonstrated.

**Retirement invariant:** stop accepting new borrows/tasks -> request
cancellation -> wait for *physical* descendants, FFI callbacks and active
region tickets to retire -> observe zero actor resource loans -> zeroize
and reclaim the arena once -> publish actor `get_done()`. Logical Future
completion alone is not a physical reclamation witness. The existing
`StateRegionActorLifetimePin` holds a generation-fenced physical resource
loan while a task-bound ticket lives and must not release it before the
ticket's authenticated retirement.

## Evidence and required next implementation

| Gate | Present status |
| --- | --- |
| Host-only native actor arena, ticket generation, pin retirement | Foundations exist, not full guest program proof |
| CFG loan identity, joins, exclusive/shared aliases, exit checks | Prototype verifier; source event emission incomplete |
| CFG explicit unpinned suspension rejection | Added by this stacked patch, pending exact-head execution |
| Source Task/Future i64 and ActorRef lifecycle awaits -> checked CFG suspension | **Wired in restricted source ownership pass**, pending exact-head build and native execution |
| General `select`/channel/generator suspension -> checked CFG | **Not wired**; unsupported paths must fail closed |\n| Positive pinned-frame borrow across suspension | **Not implemented** |
| Finalizer/unwind/cancellation drop glue after all tickets retire | **Not verified end-to-end** |
| Real `.ores` -> LLVM IR -> linked native binary without tracing GC | **Not verified for this feature** |
| ASan/UBSan, NDEBUG, allocation/latency and no-tracing-GC audit | **Required exact-head tests** |

Run the current **host-only** ownership test with independent GCC and Clang
builds (run each binary; `-DNDEBUG` must never disable the checks):

```sh
c++ -std=c++17 -O2 -DNDEBUG -Wall -Wextra -Wpedantic -Werror \
  -Isrc/main/cpp tests/ownership_cfg_dataflow_test.cpp -o /tmp/ores-ownership-cfg
/tmp/ores-ownership-cfg
c++ -std=c++17 -O1 -g -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Wall -Wextra -Wpedantic -Werror \
  -Isrc/main/cpp tests/ownership_cfg_dataflow_test.cpp -o /tmp/ores-ownership-cfg-san
/tmp/ores-ownership-cfg-san
```

Next: extend trusted lifetime events beyond restricted Task/Future/ActorRef awaits, add source-positive and
source-negative fixtures, then build and execute native binaries against real
actor/scheduler shutdown with allocation instrumentation and sanitizers.
The prior standalone zero-`operator new` 100,000-mutation smoke does not prove
whole-program no-GC, no-heap-allocation, or real-time guarantees.

**Security:** do not export private repository source to a carrier organization
until the protected-main destination allowlist and exact snapshot are
independently approved. More GitHub Actions minutes are not permission to copy.

## 2026-10-30 source-wiring follow-up on stacked PR #1226

The actual LLVM source ownership pass now translates its currently accepted
`Task<int>` and `Future<int>` result-await bindings, and direct/typed
`ActorRef.get_ready/get_done` lifecycle awaits, into a checked synthetic
ownership CFG `Suspend` boundary **at the source admission point**. It
captures active loans and conservatively treats borrowed function parameters
as live external loans. This denies unproven borrowed parameters across awaits
until a source NLL/outlives pass can prove their actual last use. The pass
revokes old suspension authority before revalidation and publishes an
internal snapshot fingerprint only after full compilation-unit ownership
validation. LLVM emission rejects a missing/stale approval *before* IR setup.

This is not generalized MIR edge emission, nor positive pinned-frame support:
`select`, channel waits and generators are still unsupported. The new
`native_source_suspension_lifetime_authority` CTest checks that fingerprint
invalidates on await/parameter changes. Existing native source actor/task/
Future compilation and executable fixtures provide a regression gate for the
admitted no-loan path. The test is checked in, not an exact-head executed pass.
