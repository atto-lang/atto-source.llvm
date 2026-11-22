# Static enum/case overloads — full guest AOT acceptance contract

**Scope:** future Oreslang **guest-code** AOT compilation. This file is a design and
release gate, not a claim that the Java or LLVM production backends currently
compile executable enum/case overloads. `EnumVariantOverloadPolicy` (Java) and
`EnumVariantOverloadPolicy.hpp` (LLVM) are **host-side** static proof oracles.

## Required compilation pipeline

1. **Typed source identity:** parse a closed nominal enum declaration and case
   refinement type `Foo.A` without conflating it with the expression `Foo.A`.
   Qualified symbol IDs come only from the resolved declaration registry;
   aliases preserve identity. Entire enum eligibility is checked at declaration.
2. **Arity-first family collection:** partition by owner, module/instance/static
   domain, name and caller-visible expanded argument arity. Source `fnc` and
   `routine` share a free-callable family; constructors remain arity-only.
   Every same-arity pair must be disjoint or strictly nested by certified enum
   cases. Crossed overlaps must fail before generated artifacts are published.
3. **Static call binding:** call syntax, named/spread expansion, generic
   substitution and approved match narrowing yield static argument types.
   Choose one exact target; check the whole selected signature, visibility,
   ownership, effects, borrow lifetime and capture authority. Neither runtime
   case tags nor expected result types rank candidates.
4. **HIR / typed IR:** preserve an immutable target-declaration identity and a
   canonical signature token, independent of AST node equality or source order.
   The type checker, ownership checker, optimizer, inliner, code generator and
   debug metadata must refer to that SAME selection. A cloned AST node must
   carry or revalidate its binding; never scan runtime name+arity fallbacks.
5. **AOT code generation:** emit direct symbol relocations for module functions
   and static class functions. Reject unresolved, duplicate, foreign, stale,
   structurally forged and unproven callable targets. Runtime reflection, JIT,
   host method lookup and dynamic overload resolution are NOT prerequisites.
6. **Virtual methods:** an instance selector is a *virtual slot identity*, not
   necessarily a concrete function body. Dynamic receiver overrides must
   preserve the exact selected slot. A final/sealed receiver may be devirtualized
   only after a separate closed-world proof. Current AOT **direct body** linker
   oracles intentionally reject instance-domain signatures; they cannot be
   misinterpreted as a complete vtable linker.
7. **Link closure:** a closed-world image must map EVERY admitted definition to
   exactly one compiled implementation and reject all missing/extra symbols.
   Body digests must be computed from actual trusted compilation outputs, not
   supplied by guest code. The current oracles only validate **canonical
   SHA-256 formatting**, not that a body was actually compiled.
8. **Incremental build and hot reload:** a schema change (including enum case
   availability/payload) must invalidate dependent call sites and AOT objects.
   Family hashes are cache hints, never authority; verify full source snapshot,
   authoritative nominal descriptor and object/link provenance. In-memory
   image identities must not leak as nondeterministic binary ABI fields.
9. **Static/runtime safety:** preserve once-only argument evaluation, resource
   moves/drops and actor message isolation. Untrusted actor/message envelope
   tags may validate an already selected contract but never select a new code
   path from an overload family.
10. **Executable qualification:** compile and run the SAME positive/negative
    Oreslang guest fixtures on LLVM native AOT and Java/GraalVM Native Image
    paths, distinguishing Native Image-of-interpreter from native *guest-code*
    lowering. Include direct fnc/routine, static, virtual, imports, aliases,
    overload-set-as-value, AOT tree shaking, debug/release, sanitizers,
    runtime/no-JIT flags and deterministic binaries. Fail release admission
    until real guest execution and exact-tree provenance are available.

## Current staged components

- Java PR #1242: bounded, prepared static compiler family, opaque image-owned
  direct call relocations, complete symbol-to-canonical-digest proof and
  virtual-instance direct-call rejection.
- LLVM PR #577: host-only C++17 matching static relation and symbol encoder,
  sealed relocation values, closed AOT image proof, CTest wiring, and
  virtual-instance direct-call rejection.
- Both: identical source-free `contracts/v1/enum-variant-aot/test_static_aot_model.py`
  Git blob (SHA-1 `990a67d0180b3435320c0f5ea57e314b7c376574`), which tests
  3,240 signature pairs, crossing ambiguity, static fallback, the ABI token,
  complete exports and virtual-vs-direct handling. It is an **independent
  reference model**, NOT evidence that the compiler or guest executables passed.
- Current Java and LLVM guest source frontends are not permitted to activate
  this overload mode based on these host-only proofs.

## CI and private source protection

GitHub source organization Actions have reported zero-step jobs; a failed
workflow with `runner_id=0` has **no compiled evidence**. Source-private exact
head CI requires nonzero runner steps and a verified commit/tree + artifact
digest chain. Do not mirror private source into another organization without
a separately reviewed, approved private destination, pinned repository IDs,
authenticated private/private preflight and explicit source-owner policy.
The Java source repository's destination allowlist was empty when audited
on 2026-10-17. Running the source-free model in another environment is not a
substitute for exact-source JDK25, Maven, native-image, LLVM/CTest and guest
codegen verification.
