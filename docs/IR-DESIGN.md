# HIR and MIR design sketch (non-normative)

> **Status: illustrative proposal.** No code in this document defines supported current Oreslang syntax or a stable on-disk IR.

## Data model and identity

Every serialized module should declare `ir_family` (`ores-hir` or `ores-mir`), a **schema version**, producer compiler/version, feature flags, target-independent data model, and reproducible source-content digests. Symbols, blocks and source spans need deterministic IDs within their explicit scope. Paths must be normalized without leaking absolute developer-machine paths.

A portable envelope should be **typed, validated, bounded** in size/depth and reject unknown *required* features. Ignore unknown **optional metadata** only when explicitly safe. No unchecked polymorphic JSON payloads may become executable operations.

### Illustrative HIR envelope

```json
{
  "ir_family": "ores-hir",
  "schema_version": "0.0.0-proposal",
  "module_id": "example",
  "required_features": ["fnc.v1", "checked-int.v1"],
  "functions": [
    {
      "symbol": "example::main",
      "params": [],
      "result_type": "int",
      "body": { "kind": "return", "value": { "kind": "int_literal", "value": "42" } }
    }
  ]
}
```

The above is a discussion aid. Even `int` bit-width, overflow behavior and representation of `42` must be formally specified before a real schema is accepted.

### Illustrative MIR flow

```text
fn example::main() -> int
  block entry:
    %n: int = constant 42
    return %n
```

No instruction here has a defined wire opcode or semantics yet. We should decide between SSA and explicit-place forms using borrow/async-lowering evidence, not preference alone.

## Required canonicalization

- String normalization, identifier/symbol resolution, module namespaces and encoding.
- Fixed deterministic ordering for maps, sets, blocks and generated IDs; canonical serialization independent of producer language.
- Explicit rules for integer sizes/overflow, floating point NaN and signed-zero handling (as applicable), tuples/unions, layouts and recursive types.
- Versioned generic-instantiation identity and cross-module visibility.
- Macro provenance: lexical source, expansion stack and originating syntax.
- Canonical diagnostics: error category, stable ID, span and optional structured detail. Backend-specific wording is not a conformance signal.

## Ownership / borrowing model

HIR carries high-level move/borrow restrictions. MIR lowers them into typed places/regions and obligations. Questions needing decisions:

1. What is the precise set of freely copyable types? Recursive types or heap owners must not silently gain copy semantics.
2. How do `Ref<T>` and `WeakRef<T>` represent actor-local cyclic structures with deterministic or managed cleanup?
3. How are cross-actor sends validated for isolated memory, shared-memory concurrency, and capability rights?
4. Are borrowing and lifetime inferences erased after proof, or do some runtime ownership fences persist?
5. What happens to captures and borrowed values across `await`, `select`, stream suspension, cancellation or hot-load generations?
6. How do dynamic dispatch, interface implementations and generic specialization preserve ownership proofs?

Two independent checkers must continue rejecting the same invalid programs until a common checker can be trusted.

## Async / actor lowerings

- Compile async routines to an explicit frame/state machine with typed suspension states and cleanup.
- Avoid a backend-specific requirement for physical self-pointers. LLVM may pin a frame, while managed Graal code may use stable logical handles.
- Represent actor messages as typed transfers/copies/borrows with explicit validity constraints.
- Preserve mailbox linearizability and supervisor/restart semantics in an executable conformance trace.
- Require compiler-inserted scheduling checks for loops and higher-order operations, including collection callbacks; no extra source keyword.
- Express deadlines and priorities as capabilities/contracts, **not** unconditional guarantees on every runtime.

## Verification passes (future)

| Stage | Fail-closed checks |
| --- | --- |
| HIR import | Supported schema/feature versions, valid digests, IDs, full required fields |
| HIR semantic verification | Well-typed calls, resolved symbols, effect and capability constraints |
| HIR → MIR lowering | Reproducibility, deterministic evaluation order, complete control flow |
| MIR verifier | Dominance and type consistency, move/drop paths, borrow regions, suspension safety |
| Security verifier | Trust/actor boundaries, allowed FFI, mandatory safepoints and I/O cancellation |
| Backend admission | Declared target features, ABI version, AOT/JIT mode and resource requirements |

Never accept malformed HIR/MIR by silently dropping unknown instructions, stripping effects or guessing types.

## TypeSpec and JSON Schema

TypeSpec can define the envelope, feature negotiation, diagnostics and report contracts; committed JSON Schema 2020-12 can strictly validate serialized fixtures. Schemas must be **generated or checked for drift**, with golden serialized fixtures and fuzz/negative cases. More advanced invariants (dominance, region containment, loan graph, actor transfer and effects) require a semantic verifier, not merely JSON Schema. See [conformance plan](CONFORMANCE.md).

## Open technical choices

- Choice of IR implementation language and build/distribution packaging.
- Library API vs canonical wire transport; endian/format and stable hash scheme.
- SSA-only vs SSA plus explicit mutable places.
- Exception/error, deferred cleanup and cancellation representations.
- How to encode runtime layout only after backend selection.
- Whether frontends ever converge; this is **not** a prerequisite for common HIR → MIR.
