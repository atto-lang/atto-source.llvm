# ActorRegionV2: guarded-return cleanup for two real native guest owners

**Stage:** opt-in compiler-backed LLVM guest slice, not general runtime AOT,
zero process-wide GC proof, concurrent actor scheduling or supervisor-managed
life-cycle.

Command:

```sh
oreslang-llvmc --actor-region-v2-guest --target=host main.atto
```

Unlike v1, this experimental codegen recognizes **two** freshly allocated,
lexically owned instances of one immutable class. The production parser,
module resolver and ownership checker run before LLVM generation.
Every admitted class has 1–16 public, final, literal i64 fields. The main
function has a single pure equality guard with an early scalar field return
and one final scalar field return. It emits native allocations and writes for
both objects, a real checked field comparison, two branches and one shared
cleanup block containing two drops (reverse declaration order), turn leave,
retirement and a checked post-quiescence destruction before returning the
snapshot result. All references use the existing four-byte region-relative
offset with original trusted host sidecars; neither raw addresses nor actor
identities are guest values.

In particular, the false branch MUST NOT run the early return's effects
and an early return MUST NOT skip destruction of the second owner.
The shared block contains a PHI for the return snapshot and both drops.
This supports exactly one guard, not generalized exception/cancellation CFG
or nested ownership, and no borrowed reference may escape.

## Native evidence required

The registered `native_actor_region_v2_multiowner_guarded_cleanup` CTest
must compile two actual `.atto` programs (guard taken and not taken), check
exactly two generated native allocation and two destruction calls, audit
emitted LLVM IR, link to the REAL `ActorRegionV1.c` + `ActorArena.c`,
execute at `-O0` and `-O2`, and check exact status/stdout/stderr plus
binary SHA-256. Ten negative compiler/profile cases reject nonclosed
source or incompatible compiler modes with no output.

The CTest is a **planned validation until it actually executes**.
Require real exact-head workflow jobs with nonzero executed steps, clang
and LLVM diagnostics, native artefact hashes, and independent approval.
A mere emitted IR string or source-free native oracle does not establish
that AttoLang generated working executable code.

## Deliberately unsupported (fail closed)

- More than two owned allocations or more than one guard/early return.
- Mutations, constructor arguments/method calls, dynamic imports, closures,
  generics, loops, actor handlers, tasks, await, exceptions, cancellation,
  FFI and ownership transfer.
- Converting a compact location into full authorization; supervisor-issued
  actor identity and generation authority remain external.
- General actor heap closure, multi-tenant safety, whole-process no-GC
  linking, and cross-backend .NET/Java/Wasm/Forge parity.

The existing compiler, stack class path, V1 codegen, ActorRegionV1 native
allocator and C ABI are preserved. Certified strict sealed AOT linkage
remains independently gated by LLVM PR #1590.
