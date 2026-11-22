# Freestanding actor kernel prototype (Cortex-M)

This slice advances [issue #612](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/612) without redefining the hosted actor runtime.

## Current contract

- `FreestandingActorKernel.c/.h` use only `<stdint.h>`. The caller supplies **zero-initialized** `OresKernel` storage. There are no allocator, libc, pthread, syscall, POSIX, GC, or native runtime dependencies.
- A **single trusted cooperative executor** drives one event per `ores_kernel_tick`. It is not concurrent, interrupt-safe, or currently preemptive. No anonymous worker threads are created.
- At most **8 actor slots**, **16 pending i64 messages per slot**, and **4 actor-local u64 fields**. These are prototype capacity constants, not yet profile-dependent budgets.
- Per-slot monotonic generations **and a local owner pointer** reject stale and cross-kernel refs for send/stop/snapshot, including two kernels with the same slot and generation. Generation wrap permanently exhausts a slot. Reinitializing CONTROL advances generations; reset while a turn executes is rejected. The owner pointer is a process-local C identity check, **not an unforgeable permission token, guest ActorRef, or cross-process wire encoding**.
- Calls to spawn/send/stop/snapshot from a running handler are rejected. Actor handlers cannot asynchronously await or call actor-graph operations yet.
- Stop prevents new messages but drains admitted ones before retirement; double-stop and stale references fail closed. On handler failure or corrupt queue metadata (out-of-bounds head, length, invalid handler or closing state), that actor's mailbox and private fields are cleared, the slot is retired, and a fault counter is incremented with saturation. The processed counter also saturates instead of wrapping.
- Retirement uses volatile scalar writes for actor-owned fields and mailbox payloads, retaining explicit zeroization under optimizations. This does **not** prove that stale CPU registers, cached copies, DMA, arbitrary C aliases or external hardware buffers were scrubbed.
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

The checked-in CMake targets `freestanding_actor_kernel_bounded_host` and `freestanding_actor_kernel_model_100k` are host-only and force assertions on even in Release builds. The first exercises actor-private state, round-robin mailboxes, capacity, stop/drain, stale/foreign refs, reset, malformed queue metadata, counter saturation, handler faults, and reentrancy. The second compares 100,000 deterministically generated actor operations with an independent bounded-mailbox model, including stale refs and full post-operation state invariants. The ARM object was independently checked to contain **no named undefined symbols**, but an object is not a bootable image.

## What is deliberately NOT proven

1. No source-defined Oreslang actor is yet linked or executed through this kernel. Existing hosted `SourceActorBootstrap.c` still uses `pthread`, `calloc`, and `free`; it must not be used to claim freestanding parity.
2. Cortex-M QEMU now executes **C-defined** actor handlers from this kernel, but **no compiler-generated Oreslang actor** has been linked to or run through the freestanding runtime. LLVM PR #580 independently verifies source-compiled scalar guest code.
3. The linker checks the Cortex-M memory map and reserves 4 KiB of stack, but detailed worst-case stack/RAM accounting, arena provenance, MPU enforcement, hard real-time guarantees, class fields, actor groups, supervisor restart, and hot reload remain unverified.

## Required follow-up

Build a freestanding `SourceActorBootstrap` adapter that binds compiler-emitted constructors and message handlers to owned static arenas, preserving strict `core / alloc / actor / std` layering. Repeat the now-passing Cortex-M QEMU positive/negative tests using **source-compiled Oreslang actors**, then establish provenance-enforced actor state, MPU/interrupt constraints and explicit worst-case RAM/stack budgets. Keep this PR draft until then.

## Standalone Cortex-M C-kernel firmware integration evidence (2026-10-18)

The repository now includes `tests/freestanding_actor_kernel_firmware.c`.
It exercises **two C-handled actor instances**, mailbox FIFO and overflow,
CONTROL stop/drain, read-only state inspection, foreign/stale references,
a handler failure, and damaged mailbox metadata. A deliberately failing
`NEGATIVE_GUEST` variant must return **exactly 99**; the positive variant
must return **exactly zero**. It checks zeroed BSS and explicitly initialized
`.data`, which become meaningful after a real MCU reset/vector startup.

The private exact-source carrier [run #39146202030](https://github.com/claritas-viz/claritas-sync/actions/runs/39146202030)
compiled and executed both host variants under ASan+UBSan. It also compiled the
actor firmware for Cortex-M3 and combined it with the kernel in a relocatable
ARM object using `-nostdlib -nodefaultlibs --no-undefined` with **no remaining
named unresolved symbols**. Five mirrored file Git blob hashes matched the
source commit `db7badfe84bc7fca71b8fa4d7ecd6d06be658a84`.
[Evidence artifact](https://github.com/claritas-viz/claritas-sync/actions/runs/39146202030/artifacts/12142903139)
is temporary and private.

**QEMU milestone (2026-10-18):** the checked-in startup assembly,
linker script, and `tests/freestanding_actor_kernel_qemu.py` now build ELF32
no-libc binaries for `lm3s6965evb`, initialize BSS and `.data` at reset,
validate the vector/Thumb entry and memory segments, and boot both C-kernel
variants. [Private exact-source run #39148351438](https://github.com/claritas-viz/claritas-sync/actions/runs/39148351438)
returned **0** for the positive firmware and **1** for the deliberately
failing firmware, both with the expected semihosting output. The test also
passed 2/2 host CTests, 100,000 model operations, ASan/UBSan and checks
of eight exact source Git blob hashes. [Evidence artifact](https://github.com/claritas-viz/claritas-sync/actions/runs/39148351438/artifacts/12144655481)
is private and temporary.

The CMake test `freestanding_actor_kernel_cortexm_qemu_positive_negative`
is registered when clang, readelf and QEMU are available. CI requiring Cortex-M
runtime verification should set `-DORE_REQUIRE_CORTEXM_QEMU=ON` to **fail
configuration** rather than quietly skip the gate if a tool is missing.

The C actor handler is hand-authored, **not compiler-generated Oreslang actor
code**. No claim of source-actor semantics, Oreslang actor ABI integration,
hardware memory protection, multi-core synchronization or safe raw-pointer
capabilities follows from these checks.
