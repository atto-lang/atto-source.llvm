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
