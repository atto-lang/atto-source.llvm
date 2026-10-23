# Actor-local carrier limits — MaxActorThreads v1

Status (2026-10-16): **language contract, incomplete implementation**. The
Java front-end recognizes `@ActorMaxThreads(MaxActorThreads.LOW)` and retains
the typed policy on `Ast.ClassDecl`. The general `define enum` syntax shown
below is **not yet supported** by that parser. Java MEDIUM/HIGH currently fail
closed; native LLVM does not yet parse or lower the attribute.

## Proposed source syntax

```ores
define enum MaxActorThreads as
    LOW int = 1
    MEDIUM int = 2
    HIGH int = 3
end

@ActorMaxThreads(MaxActorThreads.HIGH)
define actor HungryActor as
end
```

The canonical `MaxActorThreads` members map to *physical simultaneous
execution turns for one actor*: LOW=1 (default), MEDIUM=2, HIGH=3.
An implementation may ship the enum as a compiler/stdlib built-in; the exact
source definition above must only be enabled when enum declarations and
constant-value resolution work in both compilers. Until then, unsupported
syntax and levels must fail at compilation, not silently degrade to one turn.

## Independent limits

- `maxActiveTurns`: per actor, 1..3 (source `ActorMaxThreads`).
- `ActorGroup.maxParallelTurns`: aggregate physical turns across all members.
- Outstanding messages, suspended tasks and mailbox capacity: independent;
  five unresolved network reads do not consume five actor carriers.

Each actor's effective physical capacity is at most its declared limit and
any group/domain capacity. A limit is not a reservation of threads and is
never a request to create a dedicated actor thread pool.

## Safety admission

Parallel DATA turns require compiler-enforced per-turn lexical/continuation
frames, no escaping mutable actor-owned aliases, and synchronized actor state.
Immutable state snapshots or certified concurrent read loans can be shared;
writes require an exclusive `critical` loan that cannot cross `await`.
CONTROL, stop, panic, hot-replacement and finalization must fence admission
and wait for all active claims to quiesce. Every claim must authenticate a
distinct physical owner and generation. A Truffle guest context must be
proven safe for multi-threaded use or isolated per turn. No reactor may
execute guest continuations directly.

For LLVM, real-time mode also requires preallocated turn slots, queues,
arenas and carrier threads, generation-fenced cancellation, no tracing GC,
and bounded nonparking admission. None of this constitutes a hard-real-time
WCET guarantee without an explicit platform qualification.

## Release gates

1. Java parser/typechecker preserves the exact actor-local policy and rejects
   invalid/duplicate annotations and unsupported HIGH/MEDIUM.
2. Implement generic typed enum declarations/constant resolution and cross-
   backend agreement on the annotation's serialized compiler metadata.
3. Separate actor-wide mutable execution slots into per-turn contexts, connect
   the currently host-only actor credit gates, and enforce state loan rights.
4. Execute real compiled `.ores` tests proving 1, 2 and 3 simultaneous
   callbacks, never 4, including group caps, STOP/panic/cancel and await.
5. Prove LLVM RT preallocation, bounded latency and sanitizer cleanliness;
   use exact-commit/tree runtime evidence, never zero-step CI summaries.

**Do not enable HIGH/MEDIUM from documentation or a host credit gate alone.**
