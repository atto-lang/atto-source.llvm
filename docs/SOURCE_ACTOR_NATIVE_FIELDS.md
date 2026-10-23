# Source actor scalar fields on the current native ABI

The preserved Counter fixture has immutable base/delta and mutable total fields.
It exposes only `pub fnc on_message(int n): int`, the compiler's single admitted
ingress. The older public read/add/plus methods are not part of this native slice.
The host harness calls compiler-emitted init/on_message symbols during admitted
turns, checks totals 45 and 89 across separate turns, then retires the heap.

Use generation-fenced `OresActorSlot` and `OresActorLoan` values for host storage.
The field ABI validates domain, epoch, mutability and outstanding loans before
access. This fixture does not claim general actor method, await, or exception
lowering. `refreshed_actor_persistent_fields` compiles, links and executes it.
