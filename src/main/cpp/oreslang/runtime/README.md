# Future LLVM native runtime

Track execution-domain/actor ABI work in [issue #1](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/1).
This directory intentionally contains no executable actor or memory allocator.

Java reference classes: `ActorRuntime`, `OresScheduler`, `OresFuture`, `RuntimeGarbageCollector`.
Native phases: structured cancellation, generational wake IDs, pinned/relocatable async frames,
actor mailbox lease, supervisor trees, independent physical heap regions and bounded arenas.

No RT scheduler policy, zero-GC or actor isolation is currently implemented.
