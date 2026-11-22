# Native OresVM master CONTROL wake coordination

The four host-side OresVM scheduler domains (MAIN, SHARED, TRUSTED_PRIVATE
and UNTRUSTED) have one bounded, coalesced CONTROL *wake bit* per domain.
The coordinator is **metadata-only**, with no master pthread or guest code.

* `ores_native_vm_control_notify(vm, domain)` sets a bit under the master
  mutex and returns quickly. Repeated notifications are coalesced.
* `ores_native_vm_control_pump(vm)` drains the pending bitmask at one
  serialization point and signals the existing scheduler's condition
  variables. The host event loop explicitly calls it. It does not start
  carriers, enqueue actor mail, run callbacks, or create supervisor receipts.
* `ores_native_vm_publish_control(vm, domain, parent, payload)` remains
  the **actual** generation-fenced actor CONTROL mailbox admission. CONTROL
  beats DATA when the scheduler next chooses an eligible actor turn.
* Retirement fences notifications, clears pending wakes and retires pools
  under the master-to-pool lock order. Close requires host-side fencing of
  all producers/API callers before memory reclamation, exactly as before.

This intentionally extracts the useful *bounded CONTROL wake orchestration*
from old draft PR #204 without its eager master thread, pool-scheduler thread,
or dispatch-ticket rewrite. ActorGroups are membership, not OS schedulers.
This does not supply guest source actor lowering, await continuation state,
hard preemption or OS-process isolation for untrusted callbacks.

Validation: `native_vm_master_control_wake_coalescing` covers invalid
domains, duplicate wake coalescing, four-domain parallel producers, zero
carrier cold start, separation of wake from actual CONTROL delivery, and
retirement fencing. The unchanged native OresVM regression still covers
cross-domain callback close while MAIN is blocked.
