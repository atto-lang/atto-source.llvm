# Native OresVM four-domain scheduler contract

The Java OresVM is the lifecycle authority for CONTROL and the three
ActorRuntime carrier domains. The native backend now has an **opt-in host-side
C facade** `NativeOresVM.h/.c` that owns four NativeActorScheduler instances:

| Native domain | Meaning | Default max pthread workers |
| --- | --- | ---: |
| MAIN | Root main and top-level supervisor CONTROL domain | 5 |
| SHARED | Explicitly shared actors | 5 |
| TRUSTED_PRIVATE | Trusted private actors | 5 |
| UNTRUSTED | Separate carrier admission bulkhead | 5 |

Each spec may override the maximum carrier budget (zero selects five).
VM open and actor spawn **eagerly create bounded scheduler/mailbox metadata
but start zero pthread carriers**. Each scheduler starts carriers lazily under
its mutex on the first admitted DATA or CONTROL work, up to its configured
budget. Worker growth tracks the number of distinct actor lanes with
outstanding turns, not mailbox depth: queued work from the same serialized
actor cannot cause unnecessary OS threads. Thus four default domains allow up to 20 carriers under sufficient
workload; creating 1,000 ActorGroups does not create 1,000 schedulers/threads.
These four domains remain a host-side proof, not universal tenant quotas.
Every domain has its own scheduler capability domain, mailbox limits, and
worker array. An ActorRef
created in MAIN is rejected in UNTRUSTED even if slot and generation match.
An actor has a *single logical turn lease* on top of shared OS carriers,
not a dedicated pthread. Scheduler/cooperate and await continuations are
not equivalent to `sched_yield()`.

Created workers remain resident until domain retirement; idle worker
reclamation and dynamic pool rebalancing are future work. OresVM coordinates
domain lifetimes without a dedicated master OS thread; worker dispatch is local
to each NativeActorScheduler, not centralized through OresVM.

The VM closes domains only after producer/API callers are fenced; it first
retires admission, then drains accepted DATA/CONTROL work, then joins only
carriers that actually started and destroys pthreads. A worker-originated
close request retires all four domains but returns failure without joining;
an external host must complete close after fencing producer API calls. Never close a VM
from inside its carrier callback. Callback code that never yields can still
prevent drain; a host-managed process/isolate termination boundary is needed
for hard adversarial cancellation.

**Security limitation:** four native thread pools are not four virtual-memory
isolation boundaries. In particular, this C facade **does not** create or
enforce Graal sandboxes and cannot grant untrusted code direct access to host
callbacks. The host's UNTRUSTED callback must route through a separately
enforced sandbox, preferably one per hostile tenant or trust domain. Passing
hostile bytecode directly to an in-process C callback is not safe.

**Integration limitation:** the current LLVM compiler's actor source lowering
still uses the earlier NativeActorScheduler path. Adding a host facade and
passing native lifecycle tests does not complete production OresVM wiring,
source `define supervisor`, actor-group admission to parent supervisors,
or suspension/continuation parity with Java. Those remain explicit gates.
