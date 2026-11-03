# In-process tenant VMs v1

One OS process hosts up to 1000 tenants, with one VM per tenant and that tenant's actors inside
it. On GraalVM a VM is a Graal isolate. On LLVM, Wasm and .NET it is what this document describes.

Status: working runtime and end-to-end evidence. `.atto` source does not reach it yet: guests are
hand-written C and WAT. No backend is certified as a production boundary.

## What a VM is

```
OS process
  TenantVmHost
    carrier pool: fixed OS threads shared by every VM, fair between tenants
    VM "acme":   cage, actors, generation
    VM "globex": cage, actors, generation
    ... up to 1000
```

| Graal isolate | LLVM tenant VM | Mechanism |
|---|---|---|
| Separate heap | Cage | 4 GiB window + 4 GiB guard, reserved per VM, committed on demand |
| Heap-base-relative access | Addressing rule | Guest accesses are `cage + u32 offset + displacement` and cannot name anything else |
| Trusted compiler | Wasm validator | A validated module cannot express an address outside its memory |
| Isolate teardown | Retirement | Interrupt its carriers, unmap the cage; nothing to unwind |
| One failure domain per isolate | Same | A trap ends every actor in that VM, and only that VM |

Actors inside a VM are separated by ownership and arenas, not by the VM boundary.

**Faults.** A guest fault is a signal on a carrier. The handler sees the carrier is in guest mode and
jumps back to the turn boundary. The VM fails, and the thread takes the next turn from any tenant.
Outside guest mode the same fault is a host bug, and stays fatal.

**Killing a VM.** Spinning guest code needs no compiler checks to be stopped. The host signals its
carriers; each abandons the turn. That is safe because the VM's memory is discarded, and guest code
holds no host locks. Host calls clear guest mode first, so a kill landing in one waits for it to
return. Interrupts name the turn they target, so a late one cannot hit another tenant.

**Hard rule for lowered code: no libc.** A kill landing inside `malloc` would leave the allocator
lock held for the whole process. Guest code may only call the host API.

**Fairness.** Each ready VM holds one place in the rotation however many actors it has, and
`max_parallel_turns` caps its carriers.

## The security boundary

The runtime does not enforce the addressing rule. `rule_breaking_read_is_contained_only_if_it_faults`
shows it: C guest code that breaks the rule reads host memory and nothing notices.

So whatever produces guest code is the boundary, exactly as Graal's compiler is for its isolates.
**Tenants must never supply native code.** The enforced path is:

```
.atto -> LLVM wasm32 -> validate -> wasm2c -> clang -O2 -> native guest in a cage
```

`wasm2c` in guard-page mode emits unchecked `base + u32 + u32` accesses, which is this cage layout,
so linear memory is the cage and there are no bounds checks to pay for. Firefox ships this technique
(RLBox) for library sandboxing.

Sharing a process has costs that come with this design, and a Graal isolate shares them:

- A bug in this runtime, the validator or wasm2c can reach every tenant in the process.
- A crash outside guest code ends every tenant.
- Speculative side channels between tenants are not addressed.

## Backends

| Backend | Guest code | Boundary | Status |
|---|---|---|---|
| GraalVM | Truffle | Graal isolate, one Engine per tenant | Replaces the shared pool in `TWO_DOMAIN_EXECUTION_SECURITY.md`; not changed yet |
| LLVM | Lowered Wasm | Cage + Wasm validation | Runs here, on the shared runtime |
| WASM | Wasm under Wasmtime | Store + Wasmtime | Spike passes. Builds on oreslang-source.wasm #173 |
| .NET | Lowered Wasm, via `TenantVmCApi.h` | Cage + Wasm validation | Spike passes, JIT and NativeAOT |

**Why .NET does not run tenants as CIL.** CoreCLR has no in-process boundary to build on: AppDomains
are gone and an AssemblyLoadContext is not a security boundary. It cannot kill a spinning managed
thread (`Thread.Abort` is unsupported), stack overflow ends the process, and NativeAOT cannot load
code at runtime, so per-tenant code would need the JIT. Running guests on the shared native core
avoids all of that, and tenants add no CLR GC load. Compiler-checked CIL remains possible for trusted
code. This reasoning rests on documented platform behaviour, not on a CIL experiment here.

**Wasmtime limits.** A `Store` runs on one thread at a time, so with one Store per tenant, that
tenant's actors do not run in parallel. Epochs are per Engine: killing one tenant needs a per-Store
flag checked in an epoch callback, which the spike does not implement.

## Evidence

Run on 2026-11-03: macOS 26 arm64 (Apple clang 21) and Ubuntu 24.04 arm64 in a VM (clang 18, gcc 13).

| Claim | Result | Where |
|---|---|---|
| 1000 VMs, 100,000 actors, one OS process | 37ms to create, 300,000 turns in 0.7-1.0s; RSS +98 MiB on Linux, +437 MiB on macOS (larger pages) | macOS, Linux |
| Faults end one VM, carrier reusable | Null offset, past-commit, farthest access, stack overflow, trap, spin; 3 rounds on 2 carriers | macOS, Linux |
| Parallel turns of a failing VM are stopped | 3 spinners interrupted by a sibling's trap | macOS, Linux |
| Kill spinning code without cooperation | Under 1ms | macOS, Linux |
| Noisy tenant cannot starve a quiet one | 2000 actors, about 15,800 queued turns, 2 carriers: quiet p50 4-7ms, set by turn length | macOS, Linux |
| Slots and memory reuse, scrubbed | 20,000 spawn/stop cycles with no growth | macOS, Linux |
| Concurrent open, fault, kill and shutdown | 1200 VMs, 8 threads | macOS, Linux |
| ASan+UBSan on host | Clean | macOS |
| TSan on host | Clean, but cannot cover pure-spin kills: TSan defers async signals to its next interceptor | macOS |
| Hostile Wasm stays in its cage | 45 attacks, including host pointer halves and maximum displacement: 39 faulted in its own cage, 6 hit its own memory, 0 left | Linux |
| Validator refuses malformed modules | Type confusion, out-of-range local | Linux |
| Wasmtime: 1000 Stores on 8 threads | 200,000 turns at 1.68M/s; hostile reads, faults and memory bomb contained | macOS, `spikes/wasmtime` |
| .NET hosts 1000 VMs | JIT and NativeAOT, while the CLR forced 700-1200 GCs and caught 1.4-2.4M null faults | Linux, `spikes/dotnet` |
| x86_64 | **Not run** | |
| Other OS kernels or hardware | **Not run** | |
| `.atto` source in a VM | **Not run** | |

Not tested on macOS: the lowered path (WABT was only installed on Linux). Not tested on Linux: Wasmtime.

## Running it

```sh
cmake -S tests/tenant-vm -B build/tv -DCMAKE_BUILD_TYPE=Debug
cmake --build build/tv -j8
ctest --test-dir build/tv --output-on-failure
# Install WABT to add the lowered test. Sanitizers: -DTENANT_VM_SANITIZE=ON or -DTENANT_VM_TSAN=ON
```

## Open gates

In priority order.

1. **`.atto` to VM.** Compile source through the wasm32 path, and generate the guest ABI: `turn`
   export, host API imports (`alloc`, `send`, `spawn`), shadow stack per actor, data segments
   applied once per VM. The adapter applies Wasm instantiation per turn, which is only correct
   for modules without data segments.
2. **Guest code loading.** Per-tenant lowered code via `dlopen`, with admission by signature, and no
   writable globals. Test guests are linked in statically.
3. **Durable per-actor heap.** The heap is bump-only and freed with the actor, with no free or
   suspension across `await`. Zero-GC for real programs depends on this and on borrow-checker lowering.
4. **Host call coverage.** Host calls must be proven free of faults reachable from guest input.
5. **Wasmtime and .NET into their own repos.** Targeted Wasmtime kill via epoch callback.
6. **Scale.** One lock guards the ready queue, so measure beyond 8 carriers.
7. **Hardening.** Side-channel review; optional hardware keys (x86 PKU) as a defense against bugs in
   the compiler path; fuzzing of the host API.
