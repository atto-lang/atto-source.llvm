# AttoLang freestanding Core gate (Cortex-M3)

This is the integration candidate for canonical `atto.core` and `atto.alloc`
imports on the **current LLVM main tree**, independent of the older stacked
[#1390](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1390)
and [#1450](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1450)
branches. Reconcile rather than blindly merge overlapping registries.

## Canonical source and compatibility

- `atto.core/numeric` and `atto.core/numeric.atto` share the same compiler-owned
  source and unit identity as `ores.core/numeric[.ores]`.
- `atto.alloc/int_buffer[.atto]` and `atto.alloc/int_buffer_bundle[.atto]`
  are canonical aliases for existing `alloc/*` modules. A transitive import
  retains its allocator-provider requirement; zero-provider Cortex-M rejects it.
- `atto.std/*` is reserved but deliberately **not implemented** on LLVM.
  Unknown and traversal-looking compiler-owned imports fail before IR emission.
- The existing `--library-profile=core|std` flags and exported
  `ores_core_entry`/`ores_core_overflow_probe` symbols are compatibility ABI,
  not an endorsement of `ores.*` as the public namespace.

## Exact-source executable test

`.github/workflows/core-baremetal-qemu.yml` checks out the PR head and verifies
its SHA and Git tree. The Rust gate now compiles
`tests/fixtures/core_baremetal/app.atto` importing `atto.core/numeric`, checks
LLVM IR and ARM object symbols, links an ELF with **no libc, compiler-rt,
allocator, RTOS, or actor runtime**, and boots it on QEMU Cortex-M3. Its
independent negative controls reject `atto.std`, unavailable `atto.alloc`,
unknown `atto.core` imports, traversal syntax, and ambient filesystem imports.
The linked-image auditor also rejects deliberately hosted objects and checks
that checked arithmetic traps through the platform hook.

Run on an appropriately provisioned host:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --target oreslang-llvmc --parallel 2
cargo test --locked --manifest-path tools/core-baremetal-gate/Cargo.toml
cargo run --locked --release --manifest-path tools/core-baremetal-gate/Cargo.toml -- --compiler build/oreslang-llvmc
```

**Evidence policy:** source changes and model-only checks are not guest proof.
Require exact-head run identity, nonzero executed steps, successful compiler,
linker and QEMU process statuses, and an independently reviewable receipt before
marking this profile certified. A job with `steps=[]` or `steps=null` is
`NOT_EXECUTED`, never a pass.

**Limitations:** only the existing scalar and checked-arithmetic subset is
covered. This does not prove full AttoLang Core, `atto.alloc`, `atto.std`,
static actors, or backend parity. Existing semihosting is platform test
plumbing in `startup.c`, not an AttoLang guest dependency.
