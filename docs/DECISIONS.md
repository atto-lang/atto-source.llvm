# Architectural decisions and open questions

## Decided direction (2026-10-08)

1. **Keep compilers separate now.** The Java/Graal and C++/LLVM implementations retain their own parser, semantic/type checker, borrow checker, codegen and runtime.
2. **Use this repository for future HIR → MIR work.** New common-source code is not being adopted or built yet.
3. **Share semantics rather than internals first.** Independent compiler implementations must meet one evolving Oreslang language/conformance contract.
4. **Trust and isolation are orthogonal.** Shared/isolated is independent of trusted/untrusted; compilation mode is a third dimension.
5. **Trusted AOT is a target.** Full guest-code AOT for trusted routines and actors is required in LLVM and targeted in Graal. JIT optimization may be optional; an AOT-only program must not depend on guest JIT. Untrusted guest code may use JIT in both backends and need not be AOT.
6. **Specialize safely.** Native-only privileged hardware/realtime features and Java-specific host interop may diverge behind declared capabilities without splitting core Oreslang semantics.

## Performance and memory targets (2026-10-10)

7. **LLVM Oreslang execution is collector-free by design.** The native runtime must never require a tracing GC; native heap allocations are allowed but require ownership-correct, deterministic reclamation. Tracing-managed foreign components are outside any certified zero-GC execution profile. Ownership cycles are handled using actor/arena ownership and non-owning generational links, not implicit tracing.
8. **Java/Graal remains ownership-first, with minimal guest garbage.** The host JVM/Natively compiled Java runtime may still GC; zero-allocation steady-state hot loops and explicit resource lifetime control are targets to measure, not automatically established guarantees.
9. **Rust-class safe-source memory/concurrency safety and C++-class speed are acceptance targets for both backends.** Complete borrow/drop checking across suspension, actors, cancellation and FFI is a safety prerequisite. Prove performance with equivalent C++/Rust baselines, never by backend choice alone. See [memory and performance contract](MEMORY_SAFETY_AND_PERFORMANCE.md).

## Explicitly not decided

- A single parser/front end versus two HIR producers.
- A single borrow checker versus two implementations audited to one contract.
- Host implementation language of the future middle-end (C++, Rust, Java, other).
- Wire format (JSON prototype vs compact canonical binary), ABI and plugin packaging.
- Precise instruction set, SSA/place split, optimization passes and API stability.
- Location of shared macros, plugins and userland standard libraries.
- Whether the common repository later becomes a monorepo for all Oreslang sources (not presumed).

## Rejected shortcuts

- Renaming LLVM IR or a Graal graph `MIR` and declaring them shared.
- Silently interpreting guest code when a strict **guest AOT** build is requested.
- Equating a GraalVM Native Image of a generic interpreter with guest-code AOT.
- Assigning trust solely from shared/isolated actor identity.
- Treating successfully parsed HIR/MIR as safe without semantic verification.
- Merging source repositories or deprecating existing compilers before parity and a rollback plan.
- Moving bug-prone compiler analysis from either repo here merely to reduce duplication.

## Decision process

Each normative change requires: problem statement, concrete Oreslang examples, cross-backend semantic consequences, security/ownership impact, versioning rule, negative tests, migration steps, rollback and links to implementation PRs. A design PR does not by itself authorize production adoption.

See [Architecture](ARCHITECTURE.md), [IR design](IR-DESIGN.md) and [Roadmap](ROADMAP.md).
