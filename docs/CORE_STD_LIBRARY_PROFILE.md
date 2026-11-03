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

This profile gate is separate from module resolution. The native module loader
still supports its existing relative/project-root application imports and does
not yet implement canonical compiler-owned `ores.core/*` or `ores.std/*`
package resolution. Unsupported package/std/host imports continue to fail
closed.

Cortex-M remains the strongest current executable `ores.core` evidence:
Oreslang source is lowered to a `thumbv7m-none-eabi` LLVM module and compiled
with Clang `-ffreestanding` to an ARM ELF object. The platform supplies startup,
linker, device, and other board-specific pieces.
