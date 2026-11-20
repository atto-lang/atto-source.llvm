# Native tree shaking v2

Status: design and acceptance contract for the LLVM/native compiler.

## Why v2

The native compiler already has an opt-in function-only `--tree-shake` pass.
An older semantic-tree-shaking draft (#698) explored actor/class/runtime-feature
reachability, but that branch is now hundreds of commits behind current
`main`. Current AST and analysis surfaces have expanded materially since that
draft, including conditional/logical expression children, generic call metadata,
actor actions, class ownership rules, static-safety analysis, and bounded call
scanner infrastructure.

Do not revive #698 by mechanically rebasing its old reachability walker. Carry
forward its invariants and tests, but rebuild semantic reachability on current
`main` using shared compiler analysis infrastructure.

## Non-negotiable safety invariants

1. **Admission before deletion.** Parse, type/ownership-check, target-admit, and
   validate the complete source program before pruning. Dead code must not hide
   invalid target syntax, unsupported ownership, security violations, or
   capability errors.

2. **One canonical AST edge scanner.** Tree shaking must consume the same
   current AST traversal used by compiler analysis rather than maintaining an
   independent recursive walker. New AST child fields must fail tests unless
   the canonical scanner sees them. Phase 0 may reuse the existing realtime
   scanner, but Phase 1 must extract the traversal into a policy-neutral
   semantic dependency scanner so tree shaking is not coupled to hard-RT
   effect policy.

3. **Typed callable identity.** Replace parallel string encodings such as
   analysis `name/arity` and codegen `name$arityN` with one typed
   `CallableId{name, arity, ...}` inside compiler analysis. String encoding is
   an ABI/report boundary only. Generic specialization, methods, callbacks,
   closures, traits, modules, and dynamic call surfaces must not be guessed.
   Unsupported call identity fails closed.

4. **No unmodeled structural owners.** Until explicitly modeled, tree shaking
   rejects programs containing actors, classes with initializer/runtime
   ownership, host-driven continuations, cooperative loops, FFI exports,
   reflection/dynamic registration, hot-load/plugin surfaces, serializer
   registries, or other declarations that create hidden roots.

5. **Same-Program deletion authority.** Reachability is computed from the exact
   in-memory `Program` being pruned. A serialized manifest is evidence/report
   output, never independent authority to delete declarations from another
   compiler state.

6. **Target roots are explicit.** Host entrypoints, Wasm exports, Cortex-M
   bootstrap/vector roots, actor ABI entrypoints, continuation init/resume/
   cancel functions, future GPU kernels, and other target-visible symbols are
   roots supplied by the target contract rather than inferred from naming.

7. **Build kind is explicit.** Reachability receives an explicit
   executable/closed-world versus library/open-world build kind. Do not infer
   ABI policy merely from whether explicit roots were supplied. Library/plugin/
   hot-load builds retain externally reachable API surfaces unless a
   closed-world link contract proves otherwise. Exact executable roots may be
   more aggressive only under an explicit closed-world contract.

8. **Runtime features are a separate graph.** Semantic declarations determine
   retained source. Backend implementation features determine runtime imports,
   sections, helper libraries, and resources. Do not mix backend-only feature
   nodes into the backend-neutral semantic manifest.

9. **Feature pruning is consumed by codegen.** A runtime feature is considered
   prunable only when codegen/linking explicitly takes the retained feature set
   and independently checks it against the pruned program.

10. **Provenance is mandatory for production deletion.** Every retained root has
    a root reason; every retained non-root has at least one incoming reasoned
    edge. No provenance edge may point outside the retained closure.

11. **Bounded analysis.** AST traversal and function/semantic graph
    construction have explicit AST-node, AST-depth, graph-node, graph-edge,
    and graph-depth budgets. Prefer an iterative worklist for untrusted deep
    syntax. Exceeding any budget rejects tree shaking; it never silently
    truncates the graph or risks recursive scanner stack exhaustion.

12. **Final artifact evidence matters.** Source/IR symbol removal alone is not
    enough. Release qualification records final symbols, sections, imported
    libraries/resources, behavior, and size deltas.

13. **Semantic and artifact reachability are distinct.** The shared
    `ores-reachability/1` receipt is emitted after frontend semantic
    normalization and before backend-specific code generation, optimizer DCE,
    Native Image analysis, linker GC, Wasm optimization, or synthesis. A
    separate target report may describe backend helpers, imports, sections,
    libraries, resources, kernels, RTL, gates, and bytes.

14. **Transform ordering is explicit.** Source is fully parsed and semantically
    admitted first. Semantics-producing macro/annotation expansion,
    trait/composition flattening, lexical import resolution, and other
    frontend-defined declaration synthesis happen before semantic reachability.
    Only canonical frontend specialization backed by shared compile-time
    constants may affect the backend-neutral graph. Backend-specific optimizer
    decisions happen after the semantic receipt and cannot change its retained
    semantic contract.

15. **Multi-file/module identity is lexical and stable.** When LLVM gains
    linked imports/modules, declaration IDs include canonical code-unit/module
    ownership rather than file-system accident or short names. Type-only
    namespaces/contracts participate in semantic admission and type dependency
    checking but do not become runtime roots merely because they exist.

16. **Target admission is a verifier, not discarded codegen.** Phase 0 may use
    full IR emission as the existing fail-closed preflight, but v2 should
    extract a pure `validate_for_target(Program, Target)`/lowering-admission
    boundary. Tree shaking must not require generating and discarding a full
    target module before generating the real one. The verifier and emitter must
    share the same admission rules so dead target-invalid code still rejects.

## Rollout

### Phase 0 — harden the shipped function-only pass

Keep current `--tree-shake` opt-in. It may prune only ordinary functions in a
program with no actors, classes, cooperative loops, or scalar continuation
functions. Reuse the shared budgeted call scanner. Ternary/logical/guard/defer
children and every current expression child must participate in liveness.

This phase is allowed to reduce function-local runtime requirements only as an
incidental consequence of deleting a whole function; it must not independently
claim runtime-subsystem tree shaking.

### Phase 1 — graph extraction, no new deletion

Extract a policy-neutral semantic dependency scanner from the current shared
analysis traversal. Introduce typed `CallableId` and declaration IDs shared by
reachability, ownership/static analysis, and codegen selection. Introduce an
explicit build-kind/root-policy input and a pure target-admission verifier.
Build and validate roots, nodes, edges, reasons, and complete retained/removed
classification, but keep deletion behavior equal to Phase 0.

Compile-time-only declarations such as structural contracts/conformance
evidence remain part of full-source admission but are not runtime-pruning
targets unless lowering later gives them runtime artifacts.

Required initial reason classes:

- ROOT_ENTRY
- ROOT_EXPLICIT
- ROOT_PUBLIC_API
- ROOT_TARGET_ABI
- CONTINUATION_ABI
- DIRECT_CALL
- ACTOR_SPAWN
- ACTOR_HANDLER
- CLASS_REFERENCE
- CLASS_INITIALIZER
- EXPORTED_OVERLOAD_ABI

Expose an internal `explain_reachability(symbol)` query. Keep
`ores-reachability/1` backward compatible unless a separately reviewed schema
version adds provenance.

### Phase 2 — semantic deletion by declaration category

Enable deletion one owner category at a time, each behind executed positive and
negative fixtures:

1. functions, preserving entire public overload families in open-world mode;
2. classes and class initializer dependencies;
3. actors, handler dependencies, spawn edges, and target actor ABI roots;
4. both continuation representations;
5. future declaration kinds only after their hidden edges are specified.

A category remains fail-closed until its complete edge contract is tested.

### Phase 3 — backend runtime-feature graph

Derive runtime features from the already-pruned Program. Start only with
external/runtime imports that current codegen actually emits conditionally.

For each feature require:

- one fixture where it is needed and remains present;
- one fixture where the only user is dead and the feature disappears;
- codegen expected-vs-derived feature equality;
- final IR/import absence checks;
- linked execution equivalence when linkable.

Do not create speculative channel/network/TLS/etc. nodes before native lowering
actually has those imports.

### Phase 4 — final artifact verification

For canonical tiny programs record:

- semantic retained/removed declarations;
- retention reasons;
- runtime features;
- LLVM symbols/imports;
- linked symbols/sections/libraries/resources;
- final bytes;
- process behavior.

Tiny-program size is a regression signal, not a universal threshold: linker
padding can make very small artifacts equal in file size.

### Phase 5 — cross-backend parity

For the shared supported subset, Java/GraalVM and LLVM consume the same source
and compare the backend-neutral `ores-reachability/1` roots,
retained/removed sets, and compile-time constants **before backend-specific
optimization**. Backend runtime-feature/artifact graphs remain target-specific.

Java parity must include its real lexical module/import ownership, annotation/
macro expansion, proof-carrying named calls, generic specialization admission,
and guest-AOT closed/open-world build mode. LLVM must not claim parity by
comparing only same-file scalar call graphs.

## Required adversarial fixtures

Before semantic tree shaking becomes default, cover at least:

- calls only in ternary true/false arms;
- logical short-circuit operands;
- if/else statement conditions and both arms;
- guarded returns;
- defer/caught-throw cleanup;
- named and generic calls;
- overload families and exported ABI stability;
- recursion and graph-budget overflow;
- branch-local constructors and constructor arguments;
- class field initializers;
- class-owned arrays;
- actor field initializers;
- actor handler -> helper calls;
- actor spawn/send/stop/ready-await paths;
- Wasm actor exports vs native actor imports;
- both continuation forms;
- module-qualified symbols;
- malformed/unresolved dependencies;
- target-invalid code that is otherwise dead;
- forged/incomplete reachability/provenance data.

Every newly added Expression/Statement/Declaration kind must add a traversal
fixture or an explicit fail-closed admission rule.

## Default-enable gate

Do not make tree shaking the release default until all of the following hold:

- exact-head compiler build and focused tree-shake suite execute successfully;
- full native/LLVM regression suite executes successfully;
- sanitizers cover runtime-feature fixtures where applicable;
- randomized graph/provenance tests pass;
- current-main AST completeness/adversarial fixtures pass;
- final linked behavior is equivalent for shaken/unshaken controls;
- Java/LLVM semantic parity is green for identical-source shared-subset fixtures at the semantic pre-codegen boundary;
- no unmodeled dynamic/FFI/reflection/hot-load surface is admitted silently;
- release artifact inspection demonstrates actual subsystem removal;
- CI evidence is tied to the exact Git tree and contains executed steps.

Until then, `--tree-shake` remains opt-in and unsupported surfaces reject
rather than falling back to unsound partial pruning.
