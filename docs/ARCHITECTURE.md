# Proposed shared HIR → MIR architecture

Status: design proposal, 2026-10-08. The repository boundary is intentional; the detailed IR and integration choices remain open.

## Why a separate repository

A shared semantic core can reduce duplicated language rules without forcing LLVM and Java/Graal into one implementation or release cycle. It also creates an explicit place to review semantic changes before they diverge across backends.

This repository should own the common compiler contract and, when implemented, its transformations. Backend repositories should pin compatible common-source versions. A common repository is not sufficient on its own: adapters and real execution tests must demonstrate that both implementations honor the contract.

## Independent implementations first

The Java/Graal and LLVM repositories retain their own lexer/parser, AST, type checker, ownership/borrow checker, semantic analysis, code generation, runtime and scheduler. Shared source is a later migration, not a prerequisite for improving either implementation.

Their common authority should be the language specification and executable conformance contracts. This repository records the intended future shared semantic core. Initially, independent frontends can emit a common HIR contract without sharing their parsing or borrow-checking code. Moving those implementations here requires a separate, evidence-backed decision.

## Stage responsibilities

| Stage | Proposed responsibilities | Required boundary |
| --- | --- | --- |
| Source/frontend | Parsing, imports, name resolution, type checking and language diagnostics | Frontend placement and sharing are open decisions |
| Typed HIR | Preserve structured language constructs, resolved symbols/types, ownership qualifiers, callable effects and source spans | No backend-specific object model or machine pointers |
| HIR → MIR | Make evaluation order, control flow, cleanup, ownership transfers and runtime operations explicit | Preserve observable semantics and retain diagnostic provenance |
| MIR | Represent executable semantics with explicit blocks, values, calls and effects | Verify before backend consumption; declare target assumptions |
| Backend adapter | Legalize supported operations, select layouts and map abstract runtime operations to backend facilities | Reject unsupported requirements rather than silently changing semantics |
| Backend/runtime | Execute or generate code; implement allocation, scheduling, exceptions, I/O and platform services | Independently tested implementation of the common runtime contract |

SSA versus another MIR value model is not yet selected. If SSA is adopted, the verifier must check dominance, block arguments or phi nodes, and definition/use consistency. Do not make every proposed language feature depend on finalizing all optimization machinery.

## HIR contract

A proposed HIR unit should include:

- Stable module, declaration, symbol and type identities.
- Structured expressions and statements with deterministic evaluation order.
- Resolved generic information, with specialization strategy explicitly recorded.
- Ownership and borrowing modes, mutability, callable effects and capability requirements.
- Source spans and expansion provenance for diagnostics.
- Import signatures and semantic dependency information for incremental compilation.

Type validation and ownership validation need a documented division of responsibility. The MIR verifier is a mandatory defense against invalid transformations, not a substitute for source-level semantic analysis.

## MIR contract

MIR should make implicit obligations inspectable:

- Control-flow blocks, explicit value definitions, branches, calls and returns.
- Normal and exceptional completion, panic/cancellation distinctions where required, and cleanup on every relevant exit.
- Moves, borrows, drops and resource lifetime obligations, including exceptional paths.
- Abstract allocation and runtime calls with declared side effects and capability requirements.
- Defined numeric, string, collection and aliasing semantics.
- Explicit suspension/resumption boundaries and live state when async/actors enter the supported slice.
- Source mappings that survive lowering.

Do not erase ownership or capability facts merely because one backend can tolerate their absence. Do not assume LLVM integer widths, overflow behavior, pointer layout or a JVM object model are universal language semantics.

Target-independent transformations operate before target legalization. Any transformation depending on word size, layout, atomics or runtime capabilities must declare those assumptions and be restricted to compatible targets.

## Backend boundaries

### LLVM

The LLVM adapter lowers verified MIR into LLVM IR and native runtime calls. It owns LLVM-specific representation choices, target data layout, calling conventions, object emission and platform integration.

An abstract MIR operation must not be mapped to LLVM undefined behavior where the Oreslang language contract requires a defined result or diagnostic.

### Java/Graal

“Graal lowering” describes an integration boundary, not an already chosen API. Candidate approaches include:

- A Truffle execution representation constructed from common MIR, with Graal optimizing that implementation.
- A Java/JVM representation generated from MIR.
- A dedicated Graal compiler integration, if justified by an explicit prototype.

These are different engineering paths. The preferred guest-AOT experiment from the proposal is MIR → generated JVM classfiles → Native Image, so program-specific Oreslang methods are compiled rather than only an interpreter. Keep Truffle available for interactive execution and optional JIT specialization. Prove this path through a design decision and working vertical slice; do not assume arbitrary MIR can simply be handed to Graal or that sharing MIR automatically provides guest AOT compilation.

### Runtime contracts

The common layer may define abstract operations for allocation, resource release, actor/channel communication, suspension, capabilities and exceptions. It should not embed either backend's runtime implementation.

GC and native memory management need not use the same algorithms. Scheduling need not use the same implementation. Observable ownership, lifetime, isolation, cancellation and progress guarantees must still be specified and tested. Unsupported guarantees must produce an explicit unsupported result or diagnostic.

A common async/actor ABI should begin as a **semantic protocol**: entrypoints, message ownership, suspension, wake epochs, cancellation, execution leases and code generations. It does not promise identical physical object layouts, in-process pointers, or a binary ABI shared between a JVM and a native process. Cross-process interoperability would need a separately versioned wire contract.

## Versioning and distribution

Before external consumption, define:

- A versioned IR schema and a capability/feature matrix.
- Required versus optional fields, unknown-operation rejection, and compatibility rules.
- Compiler/common-core version and source provenance in generated artifacts.
- Cache keys including semantic version, compiler options, target assumptions and dependency signatures.
- A reproducible transport/package boundary.

The implementation language and distribution mechanism remain open. Compare an in-process library, an out-of-process compiler service, and serialized IR artifacts using build reproducibility, startup cost, integration complexity and debugging requirements. Do not prescribe FFI or a monorepo before measuring a prototype.

If serialized IR crosses a trust boundary, apply size/depth/resource limits and validate it before compilation. Unsupported or malformed input must not enter code generation.

## Verification strategy

Use independent source-to-backend executions as well as IR-level checks:

1. HIR and MIR verifier tests, including malformed and unsupported inputs.
2. Transformation tests preserving evaluation order, effects and cleanup.
3. Shared source fixtures with explicit expected admission and behavior.
4. Differential LLVM and Java/Graal tests on the same supported semantic subset.
5. Optimization-on/off comparisons and targeted negative cases.
6. Backend-specific runtime, platform, JIT/AOT and concurrency tests where applicable.

A negative admission case passes only for a recognized language rejection. A crash, timeout, unavailable runner, missing toolchain or broken launcher is a test/infrastructure failure. Pending features remain pending. Matching compile acceptance alone does not prove runtime equivalence.

Schema languages such as TypeSpec can describe interchange shapes, but cannot by themselves specify evaluation order, ownership, concurrency semantics or prove compiler correctness.
