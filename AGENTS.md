# AttoLang source-first implementation guidance

These instructions apply to this repository's code changes, examples, fixtures, and AI coding agents. They supplement, and never override, repository security rules or review requirements.

## Source placement

- New AttoLang guest source, standard-library algorithms, sample programs, and guest-language regression fixtures use **`.atto`**. The `.ores` suffix is for legacy compatibility or explicit migration tests only; do not create new `.ores` features.
- Implement portable guest-visible behavior in AttoLang wherever it is expressible. Add only the smallest necessary host/compiler primitive when the language cannot safely implement that behavior itself. Do not disguise host-language emulation as actual AttoLang guest execution.
- Keep functions and methods in `snake_case`. Preserve actor ownership, sealed state, capability boundaries, deterministic errors, and full-AOT compatibility.
- If a necessary host-language implementation remains, document why guest code is not yet possible, which capability it supplies, and how it can be replaced. Do not mass-rename host compiler/runtime files to `.atto`.

## Evidence and security

- Separate source-only fixtures, host-model tests, compiler parse/typecheck, executable guest tests, and native AOT evidence. A zero-step workflow, synthetic receipt, or host-only test means **NOT_EXECUTED** for guest parity.
- Record exact fixture/compiler Git blobs and source heads when making parity claims; do not mark another backend as passing without its own executed result.
- Do not copy private Git objects to another organization simply because its runner has capacity. Respect source-side authorization, independent security review, approved private destination, and protected-branch requirements.

## LLVM-specific responsibilities

- Use C++/LLVM IR for parsing, semantic checking, typed lowering, memory domains, machine-code generation, and security-critical native primitives—not for high-level standard-library logic that can live in `.atto`.
- Test `.atto` through the real `oreslang-llvmc` frontend; require actual generated-IR and native guest results, including O0/O2 and ASan/UBSan where applicable. An isolated C++ reference model is not compiler-backed parity.
- Preserve zero-GC actor-owned heaps, lifetime/borrow checks and deterministic rejection without executable IR for unsupported unsafe constructs.

## Full guest AOT and zero-GC controlled-dynamism directive (proposed policy)

**Review all new features against [organization policy PR #38](https://github.com/ores-truffle-oreslang/.github/pull/38) and [language-spec chapter PR #18](https://github.com/ores-truffle-oreslang/attolang-language-spec.md/pull/18), and the [local checklist](docs/AOT_ZERO_GC_DYNAMISM.md).** In sealed full-guest-AOT builds, no runtime eval/source compilation, new classes/methods, unbounded host reflection, monkey-patching or implicit interpreter/JIT fallback. Compile static proxy forwarding, virtual dispatch metadata, generics/macros and closure state machines ahead of time; reject unsupported shapes before IR/code emission.

Every allocation, closure capture, actor heap and suspended frame needs an owner, bounded accounting, release strategy and cancellation/restart/generation-fencing proof. Ban arbitrary strong-reference cycles without an approved lifetime/region plan; long-lived actors must reclaim retired allocations without waiting for actor termination. Channels/pub-sub carry Sendable data, never functions/closures, raw pointers or hidden host capabilities. Preserve explicit `rt take`/`rt copy`/`rt borrow`/`rt share`, no implicit conversions/operator overloading/`%`/single `^` (but `^^` is bool XOR only) and no implicit nested `Future`/`Option` flattening.

**Review gate:** PRs declare execution profile (JIT/hybrid, host AOT, sealed guest AOT, governed precompiled-module AOT) and memory claim (managed host, zero guest tracing-GC, process zero-GC); add positive and negative `.atto` compilation/execution cases, runtime cancellation/allocator stress, immutable source/fixture IDs and stepful evidence. `steps=[]` and `steps=null` remain **NOT_EXECUTED**. Do not claim cross-backend parity from this documentation.
