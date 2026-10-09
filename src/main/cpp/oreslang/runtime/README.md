# Future LLVM native runtime

Track execution-domain/actor ABI work in [issue #1](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/1).
This directory intentionally contains no executable actor or memory allocator.

Java reference classes: `ActorRuntime`, `OresScheduler`, `OresFuture`, `RuntimeGarbageCollector`.
Native phases: structured cancellation, generational wake IDs, pinned/relocatable async frames,
actor mailbox lease, supervisor trees, independent physical heap regions and bounded arenas.

No RT scheduler policy, zero-GC or actor isolation is currently implemented.

## Dedicated hungry actor idle/I/O semantics — design only

Read [Hungry isolated actor idle/I/O contract](../../../../../docs/HUNGRY_ACTOR_IDLE_IO.md).
A dedicated thread is **not** a permanently busy-spinning thread: an unresolved
`await` or blocking `select` suspends an actor continuation and, if no eligible
work remains, the carrier ordinarily parks after a bounded polling window.
A long (100–200 ms) I/O operation belongs on an async I/O path/broker; the
actor retains its thread/core reservation, and a generation-fenced completion
wakes it. Strict no-GC hungry actors do not run arbitrary GC while idle.
No actor, I/O, affinity, reservation, or GC behavior described by the contract
exists yet in this C++ runtime scaffold.
