# Native OresVM-owned Bytes: lifecycle integration (trusted runtime only)

**Scope:** opt-in C11 native runtime substrate for independently owned actor
byte inboxes. This is NOT compiler-generated `.atto` `ActorMail<Bytes>`,
not a general actor heap, not shared-memory `rt borrow`, not a Kafka broker
and not an I/O stack. No cross-organization source mirror is authorized.

## Ownership and lock order

`NativeOresVM.c` now preallocates a distinct, atomic owned-resource slot for
every actor slot in each of four independent scheduler domains. It does
not add a second supervisor, actor pool, or shared byte heap. The owned
resource hook is generic to avoid injecting dependencies on Bytes into the
20+ existing VM test linkages; the only current supported binding is the
trusted adapter in `NativeOresVMBytesOwned.h`.

The trusted caller allocates one private inbox, then atomically binds
it through the OresVM lifecycle master lock -> exact native scheduler
lock -> private Bytes-inbox try-lock. No first user DATA may precede
binding, including scalar legacy DATA. Once bound, the VM, not the
calling test, owns that resource's physical finalizer.

All VM-level Bytes sends use the **nonparking VM master try-lock** and then
the scheduler's existing atomic `try_send_committed` path. If the VM
master lock is busy they report RESOURCE_EXHAUSTED with the caller's
input untouched. A send that wins before STOP is counted as admitted;
a STOP that wins first must reject the send. This is not a hard-RT
claim: the existing scheduler uses synchronization that may park under
contention. Actual source continuations must suspend/requeue rather
than spinning on BUSY.

The physical actor's receiving path performs the scheduler's exact
current DATA-mail authorization before it loads the owned slot.
It neither takes the VM master lock nor exposes the private slab
address. A physical active callback excludes DONE; therefore
`wait_done`/`poll_done` and VM-close finalizers cannot race a running
callback with storage reclamation. The actor's logical stack remains
separate from this per-actor byte inbox.

## What triggers cleanup

- `ores_native_vm_wait_done`: after the scheduler's physical DONE, the
  VM invokes the owned finalizer exactly once (retire -> zeroize ->
  physically free). A failed finalizer leaves ownership installed
  and returns failure for safe retry.
- `ores_native_vm_poll_done`: performs the same cleanup if the actor is
  already DONE; does not park on the VM master lock and reports
  not-ready if that lock is busy.
- `ores_native_vm_confirm_supervised_finalized`: first requires the
  exact actor's completed owned cleanup, then retains the previous
  independent external resource-finalization contract.
- `ores_native_vm_close`: after all four schedulers and NIO loans drain,
  sweeps every remaining owned actor slot. It refuses to destroy the
  VM when an owned finalizer cannot complete; no partial unverified
  physical free is reported as success. The external host must fence
  **all** API submitters before VM close, as in the existing VM ABI.

A sealed STOP can abandon scheduler DATA and leave payloads charged in
the inbox until its final active actor turn completes. This is by
design: freeing queued bytes on STOP while the owner is running or
a producer is still in-flight would be unsafe. The cleanup occurs
automatically upon DONE observation or VM close, *not* via a manual
test-only call to `try_retire`.

## Native verification gate

`tests/native_vm_owned_bytes_lifecycle_test.c` constructs the REAL
four-domain VM with the native actor scheduler and actor-private
Bytes inbox. It verifies:

1. Stale-generation and cross-domain install failures; no duplicate bind
2. Owner-only physically authenticated DATA receipt; external read denial
3. Binary exactness, copied payloads and per-actor byte credit
4. One already-active actor turn versus queued DATA and sealed STOP
5. `poll_done` cannot release the running actor's slab
6. `wait_done` automatically scrubs and frees the stopped actor's slab
7. Terminal accepted=processed+abandoned and no post-DONE handle reuse
8. A peer actor continues operating on its independent slab
9. Fresh VM shutdown with no explicit STOP/wait_done also frees slabs

The test-only native finalizer additionally asserts that the *entire*
private slab has become zero before physical destruction. Host test
threads may hold a callback or use `nanosleep` to expose races; these
are NOT guest actor operations and DO NOT establish zero-blocking I/O.

A source SHA/tree/blob-pinned script compiles the actual runtime using
GCC O0/O2 and Clang O0/O2, repeats each 8 times, and runs Clang
ASan/UBSan. Native CTests are registered on Linux. As of this
submission, these exact-head tests remain **NOT_EXECUTED** on the source
org due to zero-step GitHub Actions failures. Do not infer a green
test result from code presence or any old host-only receipt.

## Outstanding engineering and security gates

- Compiler type-checking and lowering for real source `Bytes` actor
  messages, `rt take`, checked lifetime across await/select and typed
  actor heaps/logical stacks
- Rollback, cancellation/panic and STOP/DONE conformance under real
  supervisor CONTROL, fault injection, 16-way producer stress and OS
  sanitizer/TSan evidence
- Aggregate VM-wide byte budget, plus scheduler/mailbox credit alignment
- True async TCP and durable WAL/fsync with no blocking I/O inside actor
  carriers; Kafka official-client crash recovery and ≥70% baseline
- Exact-head approved native CI, source branch protection, incident
  resolution and independent security review before any merge

**Caller rule:** only the trusted VM binding may hold the inbox address,
and its callbacks must not reenter the VM or persist pointers after
a callback. The VM's original external-producer fence remains required.
Untrusted guests cannot invoke these host lifecycle APIs directly.
