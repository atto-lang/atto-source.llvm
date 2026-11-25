# Actor arenas and logical stacks: executed scope

Actor-owned heaps and stacks are a prerequisite for tenant-VM work, not a
consequence of putting an actor in a tenant VM. All actor kinds need their own
private allocation domain; explicitly shared regions need separate ownership or
reader/writer authority. An actor kind, quota counter, or mailbox alone is not
proof of independent storage.

## Native compiler-backed acceptance

This change builds on integration PR #1580, head
`ba1c6cc3a252f21df1c6a90195974b81d9cbd82e`, based on main
`0ee037ca3e5dd9ac6ac1e9cc3d40f19ee6326a9c`.

The Rust `actor-isolation` driver compiles the existing canonical `.atto`
non-tail-recursion fixture (Git blob
`459fcb445d75b9c3072599d3fdb75a5a28f51a3e`). It renames only the generated OS
entrypoint, then links the unchanged generated actor constructor, message
callback, and instruction constants with a trusted observation harness and the
real native scheduler/runtime. This is actual generated guest execution through
the actor callbacks; the test main substitutes for the ordinary guest main so
it can wait for completed delivery rather than racing STOP against admission.

The observer checks:

- Two simultaneously live actors have distinct domains and **nonoverlapping
  complete 64-KiB native frame regions**, not just different first addresses.
- Wrong-domain, stale-turn, and out-of-bounds frame access return no pointer.
  Each subsequent callback begins with scrubbed frame bytes.
- A 100000-depth non-tail request reaches the bounded **64 logical frame**
  limit and produces the actor-local quota receipt, before a native stack fault.
- A sibling returns 7 before that fault and 9 after physical retirement of the
  failed actor. A replacement has a fresh domain and returns 11. All three
  actors produce physical stop/release receipts.
- O0 and O2 executables run with a **512-KiB machine-stack limit**. A separate
  ASan/UBSan build instruments both generated guest IR and the native runtime,
  using the sanitizer's normal machine stack. Each execution has a 30-second
  deadline and is killed/reaped on timeout; CTest also bounds the outer driver.

Local environment: macOS arm64, LLVM frontend libraries 20.1.8, Apple Clang
21.0.0 for native linking and sanitizer execution. A separate LLVM 20 Clang
sanitizer attempt stalled inside ASan/dyld initialization before main; sampling
confirmed that location, and that process was terminated. It is **not** counted
as an executed guest or a passing sanitizer run.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLLVM_DIR="$(llvm-config --cmakedir)"
cmake --build build --parallel 4 --target oreslang-llvmc oreslang-rust-test-harness \
  oreslang-source-actor-frame-isolation-runtime-test \
  oreslang-source-actor-scalar-vm-runtime-test oreslang-actor-frame-arena-test
ctest --test-dir build --output-on-failure -R \
  'source_actor_compiled_isolation_low_stack|source_actor_compiled_native_lifecycle|source_actor_compiled_native_asan_ubsan|source_actor_frame_isolation_restart_and_denial|source_actor_scalar_vm_tail_quota_sibling|native_actor_frame_arena_owner_suspend_migrate'
```

## Runtime hardening

A reproduced frame-arena bug poisoned actor B when B attempted to pop a frame
capability from actor A. Domain/execution admission now precedes B's unwind
protocol, so foreign or old-execution capabilities are denied without poisoning
B. An authentic non-LIFO unwind still poisons its owner. This component's tests
are **native runtime tests**, distinct from the generated guest acceptance above.

The integration's tests also needed two compilation repairs (explicit C pointer
comparisons and a C++ frame token tested against the same arena specialization)
and precise negative diagnostics matching the newer mainline ABI-symbol guard.
No expected unsafe source is admitted and no no-IR rejection assertion is removed.

## Remaining implementation requirements

This is restricted scalar-frame evidence, **not complete actor isolation**. The
source runtime still uses serial physical callback admission. It does not yet
implement the concurrent-default actor contract, independently owned frames for
multiple live invocations, general heap objects/strings/arrays/closures, or
arbitrary actor helper/method and await/resume lowering. General guest allocation
coverage, 32-bit checked arena references with generation/domain checks, complete
borrow/drop/unwind inventories, resource quiescence and shared-region protocols
remain required. Native/FFI machine-stack faults are not safely recoverable here.

Java and .NET require their own real guest and allocation/frame evidence. No
result here certifies either compiler, the full Spin HTTP server, a hostile-code
sandbox, or a hosted Actions run. Tenant-VM work remains after the independent
actor-arena and frame requirements above.
