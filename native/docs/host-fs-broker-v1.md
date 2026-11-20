# Linux host filesystem broker — security substrate v1

**Status: C11 host-only implementation, not an executable Atto guest, not a multi-tenant sandbox.**

The Linux broker is in native/include/atto/host_fs_broker.h and native/src/host_fs_broker.c. The compiler does not call it. There is no native Atto standard-library binding, signed permission manifest, or TenantVM sandbox here.

- Host policy creation takes a trusted root directory and a trusted policy snapshot (allow-read, allow-write, explicit deny overrides, tenant ID and generation). No grants is deny-all; matching deny always overrides allow. There is one prefix scope per operation in this v1 prototype.
- Resolve paths only relative to the already-open root descriptor with Linux openat2 and RESOLVE_BENEATH, RESOLVE_NO_SYMLINKS, RESOLVE_NO_MAGICLINKS and RESOLVE_NO_XDEV. Unsupported kernels fail closed, with no weakened fallback.
- Read accepts only regular single-link files, with a 1 MiB maximum and no output-data mutation when authorization fails.
- Write stages into a private random temporary file and renames in the pinned parent directory. Symlink and hardlink targets are rejected. Parent directory durability (fsync) is not guaranteed.
- Host I/O requires the *trusted scheduler* to authenticate the actor/tenant identity and generation. Raw integer fields are not cryptographic capabilities and must never be treated as guest-provided authorization.
- The trusted host must independently intersect the signed artifact ceiling, launch request, OS/host ceiling, TenantVM/actor scope and delegation ceiling. No such signed-manifest/CLI verification exists in this C library.

**Adversarial tests:** allowed read/write, explicit denies, scope-prefix confusion, path traversal, symlink escape, hardlink aliases, tenant/generation mismatch, atomic replacement and root-directory rename pinning. The exact uploaded C source/test blobs were compiled and executed locally on Linux using GCC and Clang with -fsanitize=address,undefined. These are host tests, NOT an Atto guest or security certification.

**Limitations:** A hostile native guest with direct syscalls, raw FFI or memory corruption can bypass this C broker. The trusted root must reside in OS directories which adversaries cannot rename or hard-link into; ancestor directory movement and external inode mutation races are not fully proven. Actor/worker-ticket authenticity, manifest integrity, trusted transport, cancellation/lease revocation beyond this broker, multi-process security, portable Windows/macOS adapters, and async standard-library guest lowering remain unimplemented. The new `atto_fs_broker_revoke()` invalidates the broker generation under the same mutex used for permission admission, file reads and final atomic write commit; post-revocation operations fail. It does not revoke unrelated OS handles, and close/init must not race with active workers. Never put this broker's pointers or root file descriptor in guest memory. Do not treat it as host syscall sandboxing.

Run on Linux:

    cc -std=c11 -Wall -Wextra -Wconversion -Werror -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer -I native/include native/src/host_fs_broker.c native/tests/host_fs_broker_test.c -o /tmp/atto-fs-test
    ASAN_OPTIONS=detect_leaks=1 /tmp/atto-fs-test

The source workflow also tests Clang and CMake/CTest, but a source-org Actions run with zero steps is not test execution. Real guest certification requires a compiled .atto fixture, permission manifest validation, denied-before-open evidence, and exact-head verified runner steps.

Related: atto-spec #122, atto-platform-conformance #106, atto-cli #7, hosted std.fs contract atto-spec #117. Do not copy private source to unauthorized CI carriers.

## Host-side guest-memory adapter (candidate, not compiler-wired)

The new `native/include/atto/host_fs_guest_bridge.h` and
`native/src/host_fs_guest_bridge.c` expose `atto_fs_guest_read/write` to
a future **trusted scheduler**, not to guest code. A scheduler-created frame
carries the private cage memory, exact length, authenticated tenant/generation,
and a private broker reference. The guest only supplies `uint32_t` offsets
and byte lengths. Every offset/length pair is range-checked using subtraction
(no arithmetic overflow), and input paths/data are copied into private host
buffers before filesystem access. No raw FD, guest-crafted host pointer,
thread-local env grant or host object is returned to the guest.

The initial prototype deliberately **rejects non-ASCII paths** until the
full UTF-8 normalization and typed `std.fs` API is specified. It currently
performs synchronous host operations, so its caller must schedule them on a
bounded host I/O worker; direct use from an actor carrier would violate the
asynchronous/suspension guarantee.

`native/tests/host_fs_guest_bridge_test.c` uses a **synthetic cage frame**,
not a compiled Atto module, and exercises a successful read-then-write copy,
denied operations, bytes-left-unchanged, forged offsets, NUL/traversal
injection, tenant/generation mismatch and stale frame rejection after revoke.
GCC and Clang sanitizer builds ran locally against exact Git blobs. CMake
exposes `linux_guest_offset_host_fs_bridge` as a separate HOST-ONLY CTest.
The source org Actions must still execute admitted runner steps on the
current head.

### Required work before enabling compiled guests

1. Agree and version the typed source/runtime ABI in `atto-common-source`
   and compiler contract; update Guest ABI v4 only through an explicitly
   reviewed version increment, never by changing the existing struct in place
2. Bind compiler-emitted `.atto` `std.fs` operations to **authenticated**
   host actor-turn context; never trust the frame fields when sourced from
   guest memory or an unauthenticated entrypoint
3. Implement bounded async completion, cancellation and resource ledgers,
   and a separately verified artifact/run/TenantVM policy handoff
4. Enforce syscall/FFI boundaries for mutually untrusted native tenants;
   this standalone C adapter is not a sandbox
5. Re-run real compiled guest positive/negative conformance and exact-SHA
   CI before changing any backend status from `NOT_EXECUTED`


## Tightened admission and read-completion boundary (2026-11-20)

The candidate host-side guest bridge now enforces **actor-local** guest
memory ranges. The host-owned frame must provide `actor_region_start`
(nonzero) and `actor_region_bytes` as well as the committed cage size.
All path, input and output spans are rejected if they enter a sibling
actor's region, including when both actors share the same TenantVM cage.
Guest callers cannot supply these two trusted region fields.

Previously the bridge called the broker with a private intermediate
buffer and copied that buffer into guest memory *after* releasing the
broker mutex. That allowed a revocation to return between host read and
guest-memory completion. The broker itself stages read bytes privately
and copies only on successful validation while holding its admission
mutex. The bridge now passes a pre-validated actor-region output span
to the broker; the read, final copy and permission generation check are
serialized with `atto_fs_broker_revoke`. After revocation returns,
a pending read cannot complete an earlier authorized guest-memory copy.
The negative test races host reads and revoke, sets a sentinel after
revoke and proves no late writer modifies it.

### Six mandatory permission ceilings

`atto_fs_broker_init_layers()` accepts six **host-created, independent**
read/write allow/deny policies:

1. Host/OS ceiling
2. Independently verified artifact ceiling
3. Explicit operator runtime launch request
4. TenantVM ceiling
5. Actor capability ceiling
6. Delegation/child/group ceiling

For an operation/path, **all six allow entries must match** and **none
of the six deny entries may match**. A missing allow entry means denied.
Each scope string is snapshotted on initialization so later mutation
of the caller's policy struct cannot change an admitted broker. Every
layer is checked *on each read/write* while the broker mutex is held;
there is no flattening operation that might accidentally discard
explicit deny entries. The guest-memory bridge rejects the old
single-layer constructor entirely. That old constructor continues to
exist only for isolated host-only compatibility tests.

The C struct is **not a signed permission descriptor**. The trusted
loader must authenticate the artifact bytes, independently verify its
permission ceiling, and derive actor/tenant identities before it creates
these policy layers; a guest-forged pointer or policy descriptor must
never be admitted. This implementation neither authenticates those
inputs nor verifies that they originated in the compiler or operator.

### Executed test evidence vs unresolved work

Exact-source local host tests (not Atto guest execution) passed on
Linux with GCC and Clang, each at `-O1` and `-O2`,
`-Wall -Wextra -Wconversion -Werror -pedantic -pthread`, and
`-fsanitize=address,undefined`. Both test binaries passed in all
four toolchain/optimization combinations (eight successful
executions), including an active symlink-swap race, read/revoke
completion race, sibling actor memory-range rejections, and **24
separate missing-grant/explicit-deny checks** covering every
read/write policy dimension in every layer. Successful local host tests
do not establish exact-head stepful GitHub Actions or compiled guest
security.

**Compiler integration still missing:** review and version a new
guest host-call ABI without changing TenantVmGuestAbi v4; only the
trusted carrier/actor scheduler may create the host frame or enqueue
bounded async FS operations. A host-owned permission manifest must
be authenticated against the executable and intersected with
per-TenantVM policies. The existing Atto compiler has not lowered a
real `.atto` `std.fs` call through this bridge. Any untrusted native
guest able to issue arbitrary syscalls/FFI remains outside the proven
security boundary.


## Candidate guest ABI v5, signed artifact maximum and host worker (2026-11-20)

This draft now also includes:

* `src/main/cpp/oreslang/runtime/TenantVmIoAbiV5.h` — a **separate** proposed
  ABI v5 with `fs_submit_read`, `fs_submit_write`, `fs_poll`, and
  `fs_cancel`. These consume guest Offset32 ranges and return opaque one-shot
  tickets / typed status candidates. ABI v4 is **unchanged**. The existing
  TenantVM loader admits only ABI v4; it intentionally does not accept v5
  until a trusted actor-turn dispatch and compiler emitter are implemented.
* `native/include/atto/host_fs_signed_ceiling.h` and
  `native/src/host_fs_signed_ceiling.c` — an executable **host-only**
  Ed25519 verifier for the proposed signed artifact maximum. Its canonical
  format contains the fixed ASCII domain marker, SHA-256 of the **exact
  artifact bytes**, a required explicit-denies marker, and length-prefixed
  ASCII allow/deny scopes. The public key is supplied by the trusted host,
  not the artifact. No production signing key or loader integration exists.
  A raw, mutable verified-ceiling C struct is not a capability; a trusted
  loader still must bind it to `atto_fs_broker_init_layers` and the actual
  execution image and derive all other permission ceilings independently.
* `native/include/atto/host_fs_async.h` and
  `native/src/host_fs_async.c` — a **host-only** bounded single-worker
  async queue (1–16 outstanding tickets). Submission snapshots inputs into
  owned buffers, the worker performs blocking Linux FS calls **off the
  actor carrier**, and polling never waits for I/O. Tickets are one-shot,
  generation-numbered and tied to a host-provided actor incarnation. The
  worker does not hold a guest cage pointer. When a read completes, the
  broker reauthorizes the original path and copies its staged bytes under
  the same lock as permission revocation. The caller must validate a
  live actor and its output region before passing a destination pointer.
* `native/tests/host_fs_async_test.c` tests queue/ticket/cancellation
  behavior using an intentionally mocked broker. A passing mock test
  **does not prove filesystem isolation**.
  `native/tests/host_fs_async_broker_test.c` exercises the queue against
  the actual Linux rooted broker, but is awaiting source-owned CI execution.

**Executed local evidence:** the exact source/test blobs of the signed
ceiling verifier and mocked async queue passed GCC and Clang at -O1/-O2,
with AddressSanitizer and UndefinedBehaviorSanitizer (four runs of each).
The real-broker async integration CTest, v5 header layout CTest, full
native CMake/CTest suite and source-owned Actions remain **NOT EXECUTED**
as of this checkpoint.

**Cancellation and privilege limits:** a queued write can be cancelled
before admission. Cancellation of a *running* write cannot undo committed
side effects; it must not be blindly retried. When an actor terminates,
the trusted runtime must revoke its effective capability and drain/forget
its tickets before reusing its memory. This standalone queue does not
authenticate the `atto_fs_host_actor` IDs, track live TenantVM state,
enforce aggregate process-wide resource budgets, or integrate suspension
and exactly-once guest continuation into the scheduler. The v5 header's
callbacks are deliberately not installed in `TenantVm.cpp` yet.
Do not expose this worker's host-only C API to untrusted code.

**Release gates:** finish the trusted versioned v5 loader, actor-derived
context, signed artifact-to-broker admission path, per-actor revocation,
nonblocking guest suspension/continuation, native compiler lowering of
real `.atto` `std.fs` source, and independently observed positive and
negative compiled-guest tests. Hostile native guests remain rejected
without a proven syscall and memory isolation boundary.


## Same-image signed admission and six-ceiling broker handoff (2026-11-20)

This draft now has two **trusted-host-only** admission paths:

- `native/src/host_fs_signed_admission.c` verifies the signed artifact
  ceiling using the exact artifact byte array supplied by its host caller.
  It constructs the six-layer broker with its **artifact layer taken only
  from the verified descriptor**, never an unchecked caller field. The
  other five independent ceilings still come from the trusted host.
  The caller must guarantee the byte array is the executable image;
  this C API alone cannot prove that relationship.
- `TenantVmGuestLoader.cpp` has a new pre-dlopen gate which receives
  the **same in-memory byte vector** subsequently copied into the sealed
  private guest image. `TenantVmSignedFsLoader.cpp` uses that gate to
  Ed25519-verify the descriptor and exact SHA-256 identity *before*
  mapping executable code. A helper can construct a six-layer broker
  from a successfully loaded and signature-verified host object,
  rejecting a missing/unloaded module or invalid host tenant context.
  The resulting artifact layer is copied from the verifier output;
  runtime, TenantVM, actor, delegation, and host limits are independent.

**Security limitations:** the verified guest result is ordinary mutable
**host-owned** C++ memory, **not** a guest-mintable capability or an
unforgeable authorization token against a compromised host process. The
host loader must keep the module-owner lifetime tied to its FS broker
and outstanding async worker tickets. No active TenantVM host constructor
yet binds that lifecycle, and a caller must not re-open or rehash another
pathname to choose privileges. Dynamic linker dependencies also require
separate closure/audit and instruction-level/syscall confinement for
hostile native guest code. The current runtime still admits only ABI v4
and gives v4 guests **no filesystem host callbacks**; v5 remains a
candidate with no scheduler-authenticated dispatch.

**Executable evidence:** the exact new C signed-admission source blob
passed GCC/Clang O1/O2 ASan+UBSan local compilation and four positive/
negative signed-verifier executions with an **in-memory mock** broker
constructor. The exact signed C++ loader wrapper and header passed
GCC/Clang++ O1/O2 ASan+UBSan with a **mock** pre-dlopen loader and broker
constructor. Those test doubles prove signature propagation and failure
handling, not real OS filesystem isolation or a real mapped guest.
The checked-in real `host_fs_signed_admission_test.c` executes against
the Linux broker, and `tests/tenant-vm/signed_fs_loader_test.cpp`
builds and loads a v4 C shared library and exercises a real Linux
broker, but **neither checked-in integration test has yet executed**
on source-owned CI. A C fixture is not compiled Atto source.

As of this checkpoint, source-owned Actions failed without recording
test steps. All compiler `.atto` filesystem security statuses remain
`NOT_EXECUTED`; no PR is ready to merge. The next integration must
install scheduler-authenticated v5 callbacks, signed image admission
as a mandatory gate, actor-bound async tickets and real `.atto`
stdlib lowering, then produce exact-head positive and negative
compiled-guest evidence.


## Audit: carrier-safe submission and GitHub runner admission (2026-11-20)

A follow-up audit found a **separate carrier parking gap** in
`atto_fs_async_submit_read/write`: both operations still used
`pthread_mutex_lock(&service->lock)` on the submit path, even after
poll/cancel had been made nonparking. Submission now probes the service
mutex with `pthread_mutex_trylock`, checks for a free, non-exhausted
ticket **before** allocating or copying an up-to-1 MiB request, and
try-locks again to admit the snapshotted job. A contended mutex or full
queue returns `-EAGAIN` with ticket zero; the second capacity check
inside the lock closes the preflight race. This avoids sleeping on a
worker/other caller's mutex and prevents repeated payload allocation
when the bounded queue is already full. A submit can still execute
bounded memory allocation and copying on the caller; stricter realtime
profiles require further scheduler budgeting/quotas.

**Deterministic mock-backed regression:**
`native/tests/host_fs_async_contention_test.c` intentionally includes
the production async C module in one test-only translation unit so it
can hold the private queue mutex on the same thread. The test proves
submit/poll/cancel return `-EAGAIN` without deadlocking, then fills a
capacity-1 queue with a blocked real worker and verifies that a second
write submit fails without calling the mocked broker. This is
**mock-broker host testing**, not proof of Linux filesystem isolation or
compiled Atto guest execution.

Local exact-source tests passed on GCC 14.2.0 and Clang 17.0.0, at
-O1/-O2 with `-Wall -Wextra -Wconversion -Werror -pedantic`,
`-pthread` and AddressSanitizer+UndefinedBehaviorSanitizer.
The existing async mock regression passed 4/4 newly compiled variants.
The new contention regression passed 4/4 compiler/optimization
variants plus 120 additional invocations (30 per combination), with
exact Git blob verification:
- `native/src/host_fs_async.c`: `0c2b2bf4c5246844473d5146a278a634457bf6fb`
- `native/tests/host_fs_async_contention_test.c`:
  `7fcfc32ee0ccc14c56a5e915a60b04c3fabeb37f`

**Token-free private Actions fallback probe:** Two approved private
carrier orgs received **only the required exact source blob** via
GitHub Git Data `create_blob`/`create_tree`/`create_commit`, with
a temporary `ci/**` branch and their existing local checkout
workflow. No cross-org checkout, token, credential, or source push.
- `agent-pontifex/atto-ci`: run `44681945053`,
  mirrored blob `9631a901af2e75bf7648047152e49fc46fd1ed16`
  (note: source blob checks are separately recorded in the conformance
  evidence), Linux and macOS each failed before any steps.
- `athlet-o/atto-ci`: run `44682513534`, mirrored blob
  `0c2b2bf4c5246844473d5146a278a634457bf6fb`,
  Linux and macOS each failed before any steps.

The other six approved organizations' most recent observed runs were
also zero-step. These runs **do not establish active GHA minutes or
current-head test failure**. Only a minimal one-blob probe was sent:
a full source mirror was not created without a working carrier.
All compiled `.atto` std.fs execution and ABI-v5 runtime dispatch
remain `NOT_EXECUTED`; the signed loaded C fixture and real-broker
integration still need admitted stepful CTest.
