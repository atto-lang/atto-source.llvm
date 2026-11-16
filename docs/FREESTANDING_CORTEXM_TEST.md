# Cortex-M3 freestanding scalar Atto guest (test lane)

**Status: candidate / NOT_EXECUTED on the source branch as of 2026-11-16**. This test is stacked on [atto-source.llvm PR #11](https://github.com/atto-lang/atto-source.llvm/pull/11) and its scalar typed Atto → LLVM IR compiler. The source branch is not an independent merged compiler. See atto-spec #42, atto-common-source #23 and atto-target-profiles #5 for the proposed library layering contract.

## Goal

Produce an **actual .atto-derived** bare-metal ARM Thumb Cortex-M3 image; link it with `-nostdlib` and no libc/host allocator/OS startup; boot it under QEMU `lm3s6965evb`; confirm the return value from the actual compiled Atto `main`; and reject host/undefined source before emitting any IR/image.

The compiler itself is a Rust **hosted tool**; only the *compiled guest* is required to be freestanding.

## Run

Requires Rust 1.85.1, host cargo package cache for the offline compiler, Clang 18 with ARM target + lld, `readelf`, `timeout`, and `qemu-system-arm`:

```sh
bash ci/test_freestanding_cortexm.sh
```

`.github/workflows/cortexm-freestanding.yml` checks out the exact PR head, records the Git tree, installs the above prerequisites, and calls the script. Do **not** count a zero-step runner-admission failure as an executed test.

## Tests

- `pass-42.atto`: checked addition through a real Atto function call, linked to Cortex-M machine code, booted in QEMU; expected test-runner status **0** only when `main()` returns **42**.
- `fail-41.atto`: a distinct valid Atto program returning **41** must produce QEMU status **1**; this is the fail-negative control against a fake/always-success startup.
- `deny-stdio.atto`: host stdio source must produce an error and **no LLVM IR or ELF**.
- `deny-unbound.atto`: unresolved identifier must produce an error and **no LLVM IR or ELF**.
- `deny-alloc.atto`, `deny-process.atto`, `deny-std-import.atto`: reject dynamic allocation, OS process operations, and explicit hosted standard-library imports before emitting IR/ELF.
- Every final ELF is checked as ARM, with vector/startup symbols, no dynamic loader, and **no named undefined symbols**.

`cortexm_boot.c` and `cortexm_flash.ld` are **test-only platform code**, not `atto.core`, `atto.alloc`, a new language runtime or production startup. The harness communicates the result back to QEMU using ARM semihosting `SYS_EXIT_EXTENDED` and a breakpoint. This instrumented exit path is not a source-level OS or standard-library dependency of the Atto guest. Production firmware must provide its own device startup and termination policy.

**ARM EHABI hardening:** Clang 18 emits an `R_ARM_NONE` personality reference to `__aeabi_unwind_cpp_pr0` in an otherwise-unused `.ARM.exidx` section even when source unwinding is disabled. The test now uses LLVM objcopy to remove only these exception-table sections from relocatable inputs, then uses `--strip-unneeded` to discard the dead symbol. Crucially, actual guest object undefined symbols and final ELF undefined symbols must still be empty, no replacement unwinder function is provided, and lld has `--no-undefined`. This is a test-only no-unwind target policy; if a future guest requires real unwinding, it must fail rather than silently use an OS library.

**Compiler admission:** `atto-llvm input.atto -o out.ll --emit-llvm --freestanding-core` explicitly selects the bounded freestanding scalar profile. The normal Linux native executable path remains hosted. Freestanding admission runs on compiler tokens and the typed program and rejects imports, host I/O, actor/executor dependencies, heap-backed types, and unsupported guest runtime needs with stable diagnostic families. No automatically injected `std` or allocator is permitted in this profile. The current profile does **not** yet support importing arbitrary `core` modules.

## Scope / limitations

This proves, **only after an exact-head stepful QEMU pass**, a bounded scalar `core` execution profile: integer functions, checked arithmetic, no heap and no OS. It does **not** establish source import graph enforcement, dynamic allocation, `Option/Result` native lowering, actor execution, arbitrary `core` modules, hard-real-time bounds, full std isolation, or bare-metal support for .NET/Java/Forge/Wasm. Separate compiler import/effect admission and binary link audits are required for those claims.

**Current evidence:** a local independent minimal LLVM IR ARM linker smoke was performed without libc, with no final undefined symbols. That IR was not produced by `atto-llvm`, and QEMU is unavailable locally, so it is **not** a passing Atto guest or cross-backend proof. The source-owned workflow must execute to upgrade status.
