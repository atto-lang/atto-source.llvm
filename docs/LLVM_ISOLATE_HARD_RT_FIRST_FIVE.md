# LLVM Java-parity, tenant IsolateVM, zero-GC and hard-RT foundation (first five tasks)

**Status: review candidate — host-only implementation; NO compiled .ores guest parity or hard-RT certification.**

Source baseline: LLVM `main` 31b2fff390adac994d270d6fcbf2f65beb980d2c (2026-10-31 audit). Recheck when restacking. This patch does **not** modify the existing `NativeOresVM` four-domain scheduler. Each existing domain is a pool of trusted host callbacks, not a VM isolate or process boundary.

## Tasks addressed

1. Reconcile Java-led parity and executable status; inherit `oreslang-platform-conformance#209` (49-case corpus). Do not presume Java reference behavior is implemented when Java itself fails its fixtures.
2. Define tenant/actor distinction: tenant generation + authenticated ingress (not supplied by this host registry); per-actor logical slots and private byte regions. Never label an ActorRef or tenant handle a security credential.
3. Stage fixed-capacity native tenant lifecycle in `NativeTenantAdmission.hpp`: Running -> Quiescing -> Stopped, generation-fenced open/enter/STOP, one simultaneous turn per actor, independent actors may overlap; a retired slot can be reopened only after quiescence.
4. Stage preallocated, zero-initialized, actor-private tenant region and scrub-on-recycle, with bounded offset accesses and retained RAII turn pins. No tracing GC, and no raw pointer transfers in this interface. This is **not** source-lowered actor-owned storage; integration with `OwnedHeap`, `NativeOresVM`, compiler-generated Oreslang state, async I/O pins and supervisor failure notices remains open.
5. Add `StrictHardRealtimeProfile.hpp` fail-closed predicate gate: no blanket hard-RT claim without named OS/hardware, compiler + linked ELF closure, zero collector, static guest AOT, fixed arenas/mailboxes/control slots, independently evidenced WCET, CPU/OS scheduling and interference bounds. Reject impossible response-time sum without integer overflow.

## Explicit limits and release holds

- Tenant IsolateVM **host model**, not protected OS process, not a hostile tenant sandbox, and not integrated into `NativeOresVM` carrier callbacks. The host alone can mint handles; a genuine broker must authenticate tenant and capability at every ingress. User-supplied tenant identifiers or handles are not authorization.
- `std::mutex` locks in admission/transfer and memory scrub after quiescence are **not hard real-time safe**; they cannot run as certified RT turns. The proposed strict gate is an admission schema, not a timing/verification authority. RT integration needs RT-specific nonblocking admission and independently verified bounds.
- Actor regions are fixed-size opaque byte buffers. Compiler proof of borrowing, zero-copy messages, typed drops, per-actor heap alignment, actor state read/write locks, async continuation pins, and ownership through cancellation is pending.
- A shared_ptr pin retains registry backing if a turn survives registry wrapper destruction. This safety fallback cannot replace explicit physical actor quiescence before tenant teardown.
- `wasm` should use Wasmtime Store/Instance per tenant (WASM #173), with process sandbox for hostile tenants. `.net` should use static Native AOT guest functions and custom tenant runtime (NET #243); CLR AssemblyLoadContext is not a security boundary. Java GraalVM isolates likewise need policy/broker and threat-model verification.
- Protected-source CI is preferred. Do **not** use cross-org private source mirrors without independent per-destination authorization, protected source/carrier repos, immutable SHA, full tree/blob/mode check and cleanup; existing security incident holds remain open.

## Host-only test command

~~~sh
for compiler in g++ clang++; do
  for opt in -O0 -O2; do
    "$compiler" -std=c++17 "$opt" -Wall -Wextra -Wpedantic -Werror -UNDEBUG -pthread \
      -I src/main/cpp tests/native_tenant_isolate_rt_host_test.cpp -o /tmp/ores-tenant-test
    /tmp/ores-tenant-test
  done
done
~~~

Separate ASan/UBSan run is in the accompanying workflow. A zero-step Actions run is **NOT_EXECUTED**, not a passing test. This host test must never be recorded as compiler-source, full-AOT guest, tenant security isolation, or hard-RT evidence.

## Next implementation gates

- Invoke the tenant admission layer from authenticated `NativeOresVM` spawn/dispatch, without loosening existing supervisor CONTROL priority, actor fairness, domain/resource lifetime pins or physical carrier budgets.
- Emit per-actor state/heap and typed ownership from the real LLVM parser/MIR, then run source-derived positive and negative `.ores` fixtures with strict native AOT and ASan/UBSan.
- Prove actor ZERO-GC on the linked native artifact, bounded per-turn allocation/cleanup and WCET with named RTOS/kernel/CPU configuration; qualify hard RT only after nonzero-step exact-head evidence.
- Extend 49-case conformance ledger with detailed Java/LLVM/NET/WASM results and explicit `unsupported` vs `not_executed` statuses, without treating host-only tests as guest parity.

## Follow-up hardening: admission ceilings and concurrency regression (2026-11-01)

- The first host constructor used overflow-safe multiplication but accepted any
  representable allocation size. A trusted-host misconfiguration could request
  excessive native memory before the VM even started. Cap aggregate private
  actor backing to 64 MiB, tenant slots to 4096 and actor slots per tenant to
  4096 in this experimental prototype. Invalid configurations throw before
  any allocation. This is an internal **prototype default**, not a production
  tenant RSS cap, target hard-RT arena reservation, or external cgroup quota.
- New `tests/native_tenant_isolate_race_test.cpp`: deny zero/overflow/oversize
  capacity, reject foreign/stale handles and OOB/null reads, reenter while
  active, prove move-assignment pin transfer, 300 generational retire/reopen
  iterations, simultaneous unique opens, 12 same-actor contenders, 200
  enter-vs-STOP races, and memory scrub before reactivation.
- New `tests/native_tenant_rt_denial_matrix_test.cpp`: independently negate all
  13 safety evidence predicates and all three resource admission counters,
  and exercise near-deadline, zero, period mismatch, and unsigned overflow
  cases. Its synthetic all-true positive case checks **only** the function's
  arithmetic and boolean gate, never validates real physical evidence.
- Source-local workflow now includes three C++ suites, GCC+Clang O0/O2 and
  GCC ASan/UBSan repeats. The old original host fixture is retained.
- Local execution in the working container: GCC 14 and Clang 17, O0/O2 for
  the two new suites: 8/8 combinations pass. GCC ASan+UBSan race test passed.
  The exact GitHub PR head and new workflow run remain NOT_EXECUTED until a
  nonzero-step runner receipt is inspected. This local proof is limited to
  host-only source, not the full repo compiler or linked native guest.

**Unchanged release holds:** no direct integration with `NativeOresVM`, no
actual `.ores` actor lowering, no independent scheduler/WCET artifacts, and
no claim of secure hostile-tenant or hard-real-time execution. Avoid private
source transfer while independent security approvals are outstanding.

## Follow-up: registry-scoped handle provenance and shutdown revocation

The original v0 `Handle{slot, generation}` could collide with a live tenant
in another host registry using the same slot and generation. This was not
authentication, but it was a real host-side identity confusion bug.

- Each admitted handle now carries a private weak reference to its issuing
  registry state and private immutable-at-issuance copies of slot/generation.
  `enter`, `request_stop` and `finish_stop` reject foreign, missing and
  retargeted provenance before accessing a tenant slot.
- Public two-field constructor remains for negative/legacy host fixtures;
  it does **not** mint authority and is deliberately rejected. Callers must
  obtain a real handle from the trusted host `open` method.
- Host wrapper destruction sets each slot's `host_closed` flag under its
  mutex, revoking new read/write operations by previously admitted turns.
  Turn pins keep the backing state alive for RAII release, avoiding a UAF.
  Registry destructor is a trusted off-carrier operation. Concurrent calls
  to the registry object during its destruction are not supported; external
  lifetime coordination remains mandatory.
- Added `tests/native_tenant_registry_provenance_test.cpp` covering matched
  slot+generation across two registries, same-registry slot retargeting to a
  live sibling tenant, generation upgrade to a real restarted tenant, bare
  forged handles, cross-registry STOP/finish rejection, post-owner-destruction
  retained-turn read/write rejection, private memory independence and 16
  concurrently executing registries with cross-registry negative probes.

**Local verification:** GCC 14 and Clang 17 at `-O0` and `-O2` each passed
the new source-exact host suite (4/4 combinations). GCC and Clang
`-fsanitize=undefined` passed five runs each (10/10); GCC combined
AddressSanitizer+UndefinedBehaviorSanitizer passed one more run. This
evidence is isolated host-side C++ only, not exact-head CI or compiled
`.ores` source. The new header and suite blobs were independently matched
against the local `git hash-object` outputs.

**Boundary remains:** copying somebody else's valid handle is possible in
trusted host code, so this is not a secret bearer token or tenant
authentication. A trusted policy broker must check tenant membership and
permissions on every ingress. Independent OS process boundaries remain
necessary for malicious native tenants; RT qualification still requires
separate timing and native artifact evidence.

## Follow-up: deterministic multi-VM state-machine regression

The new `tests/native_tenant_state_machine_test.cpp` executes 30,000
deterministically seeded transitions over four independent VM registries with
four tenant slots and three actors each. Its separately maintained lifecycle
model asserts actual host outcomes for open/enter, per-actor physical turn
admission, read/write content isolation, request_stop, quiescent finish_stop,
fresh-generation zeroing, stale/foreign handle rejection and public token
retargeting. The test includes minimum coverage thresholds, so a degenerate
operation schedule cannot pass vacuously.

Local source-exact proof: GCC 14 / Clang 17 O0/O2 compiled and ran all
four configurations; GCC ASan+UBSan and Clang UBSan also passed (one and
three executions, respectively). The fixture's Git object SHA was checked
against the locally executed bytes. This is non-certifying HOST-ONLY
reference-model evidence: it does not execute compiled `.ores` guest source,
does not prove a real VM scheduler/supervisor integration, and does not
qualify LLVM hard real time.

Next executable promotion requires separately authorized source-org runner
admission (at least one step), then actual source-to-native actor guest
tests, CONTROL/DATA priority and mailbox/failure retirement integration.
