# Per-actor native resource retirement pins (stacked follow-up)

This change is intended to land **after** draft PR #1158. It adds a
host accounting substrate, not an actor scheduler, epoll/kqueue integration,
Linux sandbox, or proof of physical OS deregistration.

## Contract

An IsolateVM owns bounded native-resource tickets, each associated with
one specific actor identity and incarnation. Resource kinds currently cover
continuations, native I/O registrations, callbacks and mailbox loans.
The resource ID never repeats within an isolate generation.

OresVM::pin_actor_resource accepts new pins only while the isolate is Running
and the actor ticket is current. The per-isolate resource-pin budget is
configured by IsolateConfig::max_resource_pins (default 1024). A full quota
fails closed.

When an actor is stopped, its ticket cannot be retired while resource loans
remain. When an isolate begins quiescing, it admits no new actors, guest calls
or resource pins; retiring resources remains permitted. A tenant cannot
complete shutdown until all actors, native guest calls **and** resource pins
have retired. Foreign, replayed and stale tickets are rejected. A restarted
tenant uses a new generation, invalidating all previous tickets.

**Critical invariant:** call retire_actor_resource *only after* the
underlying operation is physically retired (e.g., kernel event registration
removed, callbacks cannot be invoked, outstanding wake tokens revoked).
An arbitrary actor or guest must not gain authority to retire its own
outstanding I/O prematurely. The present OresVM host interface does not
physically dereference or deregister OS resources; integration with
NativeContinuationStore and the platform I/O backend is a separate gate.

## V8 and Oreslang scheduling

Do not turn this ticket bookkeeping into a single native thread per tenant.
OresVM supervises tenant-owned IsolateVM instances and their actor runtimes.
The existing synchronous scalar guest ABI uses a temporary per-tenant turn
mutex, but production actor scheduling must use per-actor exclusion,
separate fairness budgets and correctly pinned asynchronous continuations.

## Validation

New CTest: native_oresvm_actor_resource_retirement. The test exercises:
- Per-tenant pin quotas and rejection of invalid actor owners
- Cross-tenant and cross-actor token rejection
- Actor retirement denial with outstanding callback/I/O tickets
- Quiescence admission closure and draining after shutdown
- Replay rejection, tenant reincarnation and stale ticket denial
- Independent progress by a second tenant

The isolated test harness passes four tests in GCC C++17 and four under
Clang ASan/UBSan. Those results validate the host contract only, not actual
kernel lifecycle integration or the full LLVM compiler build. CI must run
real steps against exact committed tree SHA before declaring acceptance.

## Next integration gate

Wrap each physical native I/O or continuation registration with a resource
pin minted by the OresVM host. Ensure the callback completion and deregistration
paths retire it exactly once. Verify cancellation vs completion races with
ASan/UBSan and real epoll/kqueue tests, then add actor-per-carrier scheduling.
