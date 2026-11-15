# AttoLang trusted native-pointer supervisor bridge — host-only hardening

Status: **draft, not guest-executable, not production admission certified**.
Stacked security work: LLVM pointer-region revocation PR #1818 and broker
integration PR #1825. The public AttoLang pointer syntax and effects remain
**unsupported**; no native guest IR/AOT lowering has been enabled.

## Verified design boundaries

1. Native objects and actor-owned values are not raw host pointers.
   Actor-relative compressed32 words remain checked offsets. No `.atto`
   source expression may fabricate a host memory address.
2. `NativeRegionRuntimeAuthority` is private to native supervisor integration.
   Each `NativeRegionRegistry` is permanently bound to **one** broker/runtime
   for its lifetime, so a second conflicting trust table cannot reopen a
   retired actor incarnation. No source user receives the broker.
3. `NativeActorSupervisor` is a HOST adapter over the real
   `NativeActorScheduler`'s exact scheduler-issued reference and
   `is_current_actor` claim. Only a live matching actor callback may request
   a grant. An external thread, a foreign actor carrier, a forged generation,
   or an untrusted actor is denied.
4. The trusted host supervisor first `admit_spawned(ref, trust)`, then
   `register_mapped_region(ref, region_id, allocation_id, base, bytes, mode)`.
   Host actor turns can only call `issue_current_region(ref, region_id,
   allocation_id)` — **no** raw pointer/address/length/mode is accepted at
   that callsite. Duplicate IDs and overlapping virtual RAM ranges reject.
   MMIO and invalid modes reject until a separate device grant exists.
   The supervisor is trusted C++ and remains within the TCB; no untrusted
   native machine-code sandbox is claimed.
5. `stop_and_revoke(ref)` seals the exact actor's USER mailbox first,
   fences the broker incarnation, invalidates cached native windows and
   purges registered mapping metadata only after the fence returns.
   `revoke_terminal(ref)` is a separate idempotent path for the exact
   scheduler-verified terminal reference, e.g. after panic/raise escalation.
   Neither is callable from an actor scheduler carrier, which could deadlock
   waiting for its own FFI callback.
6. Synchronous native FFI raw-address scopes must quiesce before backing
   storage is freed. A same-thread callback retirement defers and throws:
   it is NOT terminal success. The broker stays `Retiring` until a retry
   completes and then transitions to `Retired`. Incomplete teardown
   fails-stop at broker destruction rather than silently freeing backing
   memory. Async DMA or foreign callbacks need a separate completion-pin
   protocol before production release.
7. Actor incarnation and mapped-region tables are bounded. Terminal epochs
   stay tombstoned until whole registry shutdown; do not drop tombstones
   while stale grants could be replayed. Grants may not enter message,
   channel, generic erasure, serde or foreign guest-untrusted domains.

## Exact sequence for native host integration

- Construct scheduler, then its one supervising broker/registry.
- Spawn actor on scheduler and obtain its exact `OresNativeActorRef`.
- Independently authenticate the actor's code-image trust policy before
  invoking `admit_spawned`; a source `trusted` flag or a caller-supplied
  enum is not authorization.
- The trusted supervisor registers owned backing-memory windows. Keep the
  allocation mapped until the exact actor's native pin quiescence has been
  established.
- Only the current live actor callback may request a registered region.
- On STOP, permission loss, cancellation, panic, raise, hot-reload restart or
  supervisor retirement: seal DATA admission, call `stop_and_revoke` or
  `revoke_terminal`, retry deferred FFI fences, wait for scheduler DONE
  from an external host thread, and *then* release/scrub backing allocations.
- Globally retire the scheduler and drain before `try_destroy`. The bridge
  must not outlive its scheduler or be destroyed while native grants remain.
- **Not yet wired:** automatic callbacks for every scheduler termination mode;
  audited trust/code-domain authentication; asynchronous FFI and DMA pin
  accounting; allocation-catalog physical alias detection across mappings
  or separate registries; source-level checked unsafe effects and compiler
  lowering. Do not treat this adapter as a complete guest isolation system.

## Runnable host-only profile (not a guest compiler/AOT test)

From the source repository at the **exact intended PR head**:

```sh
cmake -S . -B build/native-pointer-host \
  -DATTO_NATIVE_POINTER_HOST_ONLY=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/native-pointer-host --parallel
ctest --test-dir build/native-pointer-host --output-on-failure
```

The standalone profile compiles C++ region/FFI tests and a real native
`NativeActorScheduler.c` integration fixture. It also invokes compile-
negative authorization tests on UNIX. Separately run the updated GitHub
workflow's GCC release, Clang ASan+UBSan and GCC TSan matrix, and the
`oreslang-llvmc` negative `.atto` corpus in the normal compiler build.

## Exact-head local evidence receipt (approved checkout only)

Use the checked-in verifier at the **exact PR head** to avoid a misleading
build of an older commit or a dirty tree. The script refuses a wrong SHA,
missing required tools, absent tests, zero/too-few CTest cases, and an in-tree
artifact directory. It runs the standalone host tests in three separate builds:
GCC release, Clang ASan+UBSan and GCC TSan.

```sh
sha="$(git rev-parse HEAD)"
bash scripts/native-pointer-exact-head-verify.sh "$sha"
```

The verifier writes an external receipt directory with each stage's actual
exit status and logs, immutable commit/tree identity, compiler versions,
log SHA-256 digests and native binary SHA-256 digests. It explicitly reports
`atto_guest_aot=NOT_EXECUTED` and
`cross_backend_parity=NOT_CERTIFIED` even when all **host-only** tests pass.
Compiler `.atto` fixtures still require separate true compiler execution.

Run only on an authorized local/private checkout. Never place private source
or logs in an unapproved public repository or cross-organization carrier.

**Evidence gate:** source-only checks and GitHub Actions jobs with zero
executed steps are `NOT_EXECUTED`; do not promote/merge as verified.
Cross-organization private Git mirroring requires reviewed protected-source
authority and independently approved destination policy. A source-free runner
canary is not permission to copy private source.
