# Freestanding actor kernel prototype (Cortex-M)

This slice advances [issue #612](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/612) without redefining the hosted actor runtime.

## Current contract

- `FreestandingActorKernel.c/.h` use only `<stdint.h>`. The caller supplies **zero-initialized** `OresKernel` storage. There are no allocator, libc, pthread, syscall, POSIX, GC, or native runtime dependencies.
- A **single trusted cooperative executor** drives one event per `ores_kernel_tick`. It is not concurrent, interrupt-safe, or currently preemptive. No anonymous worker threads are created.
- At most **8 actor slots**, **16 pending i64 messages per slot**, and **4 actor-local u64 fields**. These are prototype capacity constants, not yet profile-dependent budgets.
- Per-slot monotonic generation refs reject stale sends, stops and snapshots; generation wrap permanently exhausts a slot. Reinitializing CONTROL advances generations; reset while a turn executes is rejected.
- Calls to spawn/send/stop/snapshot from a running handler are rejected. Actor handlers cannot asynchronously await or call actor-graph operations yet.
- Stop prevents new messages but drains admitted ones before retirement; double-stop and stale references fail closed. On handler failure, that actor's mailbox and private fields are cleared, the slot is retired, and a fault counter is incremented with saturation.
- Snapshot only exports processed count and last scalar result. The callback field pointer is not exported by the API, but isolation is **logical**, not MPU-enforced: a native C handler can still form or retain raw pointers. This is not yet an adversarial untrusted actor sandbox.

## Run narrow host and ARM checks

```sh
cc -std=c11 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined \
  -fno-sanitize-recover=all -Isrc/main/cpp \
  src/main/cpp/oreslang/runtime/FreestandingActorKernel.c \
  tests/freestanding_actor_kernel_test.c -o /tmp/ores-freestanding && /tmp/ores-freestanding

clang --target=thumbv7m-none-eabi -mcpu=cortex-m3 -mthumb \
  -std=c11 -ffreestanding -fno-builtin -Wall -Wextra -Werror -pedantic -Os \
  -Isrc/main/cpp -c src/main/cpp/oreslang/runtime/FreestandingActorKernel.c \
  -o /tmp/ores-freestanding-arm.o
readelf -Ws /tmp/ores-freestanding-arm.o
```

The checked-in CMake target `freestanding_actor_kernel_bounded_host` is host-only and forces assertions on even in Release builds. Tests cover actor-private state, round-robin mailboxes, capacity, stop/drain, stale refs, reset, handler fault, and reentrancy. The ARM object was independently checked to contain **no named undefined symbols**, but an object is not a bootable image.

## What is deliberately NOT proven

1. No source-defined Oreslang actor is yet linked or executed through this kernel. Existing hosted `SourceActorBootstrap.c` still uses `pthread`, `calloc`, and `free`; it must not be used to claim freestanding parity.
2. No QEMU boot/positive/negative case is yet present for the actor runtime. The previously verified no-libc QEMU test in LLVM PR #580 is **scalar guest code only**.
3. No linker-map RAM/stack/flash accounting, arena allocator provenance, MPU enforcement, hard real-time guarantees, class fields, actor group operations, supervisor restart, or hot-reload integration.

## Required follow-up

Build a freestanding `SourceActorBootstrap` adapter that binds compiler-emitted constructors and message handlers to owned static arenas; preserve the strict `core / alloc / actor / std` layering. Add Cortex-M reset/vector and RAM initialization, link the actual source-compiled program without libc, execute positive/overflow/stale-ref/fault cases under QEMU, and prove the final ELF's undefined-symbol and memory budgets. Keep this PR draft until then.
