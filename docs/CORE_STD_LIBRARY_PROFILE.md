# Oreslang library profiles: ores.core and ores.std

The native compiler recognizes exactly two public library profiles:

- `core` corresponds to `ores.core`: freestanding/no-OS execution.
- `std` corresponds to `ores.std`: hosted/platform execution.

There are no public `alloc`, `actor`, or `otp` library profiles. Allocation,
actor execution, supervision, and clocks are implementation facilities beneath
one of the two public layers.

## Current native admission

The current LLVM backend is deliberately fail-closed:

| target | default | admitted explicit profile |
| --- | --- | --- |
| `host` | `std` | `std` |
| `wasm32-unknown-unknown` | `core` | `core` |
| `thumbv7m-none-eabi` | `core` | `core` |

The compiler rejects `std` for a freestanding target and rejects a claimed
`core` execution on the ordinary host target until host-side effect/capability
closure can prove that combination.

Module resolution under `core` admits only the closed, compiler-owned registry
(currently `ores.core/numeric`, plus the allocator-provider-gated `alloc/*`
probes). Relative/project imports, `ores.std/*`, and other host imports fail
closed for freestanding targets. Native `ores.std/*` package resolution is not
implemented yet.

## Bare-metal execution evidence (no libc, no OS)

`tools/core-baremetal-gate` (Rust) is the executable contract for "`ores.core`
links and runs without an OS". CI runs it in
`.github/workflows/core-baremetal-qemu.yml`. It:

1. Compiles `tests/fixtures/core_baremetal/app.ores` (imports
   `ores.core/numeric`) with `--target=thumbv7m-none-eabi
   --library-profile=core`, and requires the IR to declare no external
   functions other than LLVM intrinsics, and to contain no external globals,
   module-level assembly or inline assembly (an `svc` would bypass every
   symbol audit). Compiling without `--library-profile` must give the same
   module, so the target cannot silently default away from `core`.
2. Compiles the IR to an ARM object and requires every undefined symbol to be
   (including weak `w`/`v` references)   in the **freestanding support ABI**: `memcpy`/`memmove`/`memset`/`memcmp`
   and the ARM RTABI memory and 64-bit arithmetic helpers (`__aeabi_*`,
   `__mulodi4`). The list lives in `tools/core-baremetal-gate/src/audit.rs`.
   libc, allocator, thread, unwinder, TLS and Oreslang runtime symbols
   (`ores_actor_*`, `oreslang_rt_*`, ...) are rejected by name. Today the
   guest requests none of the helpers.
3. Links the guest with a ~150-line platform startup (`startup.c`: vector
   table, reset, `.data`/`.bss`, fault handler) using `ld.lld` and a linker
   script with no heap. No libc, compiler-rt, RTOS, or actor runtime is on the
   link line. The image must resolve every symbol, contain no allocator
   definition, and have no `INTERP`/`DYNAMIC`/`TLS` segments or
   constructor/TLS sections.
4. Boots the image on QEMU `lm3s6965evb` (Cortex-M3). The harness checks
   six input/expected pairs that cover every branch of the guest (both sides
   of the `&&`/`||` window, both clamp bounds) and requires
   `ORES_CORE_BAREMETAL_PASS vectors=6 value=42`. The guest uses the whole
   scalar subset the Cortex-M core path admits today: int/bool values,
   checked arithmetic, negation, comparisons, `&&`/`||`, `if`, direct calls
   and an `ores.core` import.
5. Boots a second image whose harness feeds `INT64_MAX` to a checked
   multiply, and requires the Oreslang overflow trap to land in the
   platform's `HardFault_Handler`. The fault must follow the probe's start
   line and happen exactly once, so a fault from a broken reset path or vector
   table cannot pass. The passing image must print no trap, failure or
   unexpected-exception line. Failure policy belongs to the platform,
   not to an OS abort.
6. Requires the compiler to reject: an `ores.std` import, a transitive
   allocator requirement with no granted provider, an ambient project import,
   and `--library-profile=std` on the freestanding target.
7. Proves the audit is not vacuous: a deliberately hosted object
   (`forbidden_probe.c`: `malloc`, `printf`, `pthread_create`,
   `ores_actor_spawn`) must be flagged by the symbol audit and must fail the
   no-libc link.

Run locally (needs clang with the ARM backend, `ld.lld`, `llvm-nm`,
`llvm-readelf`, `qemu-system-arm`):

```sh
cmake --build build --target oreslang-llvmc
cargo run --manifest-path tools/core-baremetal-gate/Cargo.toml -- \
  --compiler build/oreslang-llvmc
# override tool paths with --clang/--ld-lld/--llvm-nm/--llvm-readelf/--qemu
```

**Boundary.** This proves one scalar `ores.core` program, plus one compiler-owned
core module, links and executes freestanding on an emulated Cortex-M3. QEMU
does not check timing, memory protection or real silicon. Semihosting
is harness plumbing in `startup.c` only; the guest object never calls it.
Wider `ores.core` surface (slices, `Option`/`Result`, bounded actors) needs
the same gate before it can claim freestanding status. The earlier
Zephyr/QEMU run (`docs/CORTEXM_QEMU_EVIDENCE.md`) remains RTOS evidence, not
no-OS evidence.

The Cortex-M core path still rejects typed locals, loops, `Option`/`Result`,
arrays and structs. It also rejects a bool-returning call used as an `if`
condition, and reports that with a misleading "rejects main, effects or
unbounded functions" diagnostic. Each of those needs compiler work before
this gate can cover it.
