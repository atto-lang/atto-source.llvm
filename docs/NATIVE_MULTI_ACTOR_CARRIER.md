# Experimental Native MultiActorCarrier

`MultiActorCarrier` is a **separate opt-in C++ runtime prototype** from the
existing one-actor `HungryCarrier`. It has one real OS worker, at most eight
pre-registered actor contexts, a bounded 64-message mailbox per actor, and a
single capacity-eight FIFO of runnable actor tickets.

- **Physical CPU fairness:** one callback gets a fresh 1..512 reduction budget
  (32 by default); `Outcome::cooperate` appends the same actor behind other
  runnable actors without completing its logical turn.
- **Per-actor serial turns:** only one callback at a time runs on the worker.
  New mail never overtakes a continuation belonging to that actor. An actor
  can have at most one runnable queue ticket.
- **CONTROL:** per-actor cancel requests and carrier stop bypass bounded
  user mailboxes. The worker checks them between dispatch slices and invokes
  the owner-thread cancel callback **before** advertising retired.
- **Lifetime:** `Producer` holds a weak lifetime reference. Actor
  generations are minted from a process-wide, non-wrapping atomic namespace:
  a ticket from a prior carrier instance cannot name an actor in a newer
  instance even when the 1-based slot matches. Stale tickets, completed
  cancellation tickets, and sends after carrier teardown are rejected.
  The caller retains every actor state and frame until join. Registered
  slots are never reused within this prototype. ActorToken is a checked
  routing identity, **not** an unforgeable security capability.
- **Accounting:** admitted messages are counted once; completing a physical
  slice does not imply logical message completion. After join,
  `abandoned = accepted - completed`.

The actor callback must be nonthrowing, bounded and cooperative. It cannot
carry a live unpinned loan or arbitrary FFI across a yield. This carrier does
**not** implement source actor lowering, independent native actor-owned heaps,
IO broker wakeups, live loan proofs, cancellation of a non-returning native
stack, GC isolation, hard WCET, or migration between OS workers.

The integrated test compiles **two separate Oreslang source loops** through the
experimental LLVM backend, renames each module's external frame ABI symbols
to avoid linker collisions, and runs A's billion-step loop concurrently with
64 ordered messages handled by B's short loop **on one real carrier thread**.
B must finish while A remains active. A is then canceled and acknowledged on
that same worker. The gate runs native Release with `-O3 -DNDEBUG` and
ASan/UBSan with always-active checks. This demonstrates bounded CPU-slice
fairness for trusted host-registered actors, **not** general `define actor`
source semantics. Parent: issue #321.
