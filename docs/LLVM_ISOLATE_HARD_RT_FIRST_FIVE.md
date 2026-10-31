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
