# RFC: Pointer<T>-aware separation logic and verified machine code

Status: Draft / design only. No claims of implemented verification.

## Scope
Bridge relational parametricity, ownership-derived separation logic, dependent/refinement propositions and instruction-level translation validation. The first target is a restricted LLVM/native subset, with independently checked proof artifacts.

## Atto pointer syntax (normative design constraint)
Atto uses the `Pointer` class and `Pointer<T>` for pointer-typed values. Do not introduce C/Rust pointer or reference sigils (`&`, `*`) into Atto syntax. Ownership operations use `take!`, `borrow!`, `share!` and their established variants. Pointer access must use the existing/approved `Pointer<T>` methods; examples below are illustrative API proposals, not implemented interfaces.

```atto
fnc exchange<T>(left: Pointer<T>, right: Pointer<T>)
    where disjoint(left, right)
{
    let first = take!(left.read());
    let second = take!(right.read());
    left.write(take!(second));
    right.write(take!(first));
}
```

The example is *not* yet well-typed by specification: moving from a pointed-to location requires initialization tracking, exclusive access, a safe replacement operation, and a defined `Pointer<T>` access API. A later revision must either use a validated swap primitive or specify the full protocol before acceptance.

## Semantics and obligations
1. Track allocation identity, provenance, bounds, alignment, initialization, lifetimes, permissions, and region ownership for each `Pointer<T>`.
2. `disjoint(a,b)` must mean nonoverlapping accessed byte ranges for the relevant operation, not merely distinct pointer objects or unequal addresses. Zero-sized ranges, subviews, shared backing storage and integer-to-pointer conversions need explicit rules.
3. Integrate `take!`/`borrow!`/`share!` with separation assertions. Exclusive write permission must be demonstrated; shared permissions cannot silently authorize mutation.
4. Generic relational parametricity is conditional on effects, representation exposure, unsafe capabilities, termination and observable behavior. It is not automatic for arbitrary `Pointer<T>` operations.
5. Express pre/postconditions and frame rules in a Verification IR (VIR) lowered from typed HIR/MIR. Record proof provenance and unsupported features.
6. Produce proof obligations for each lowering/optimization boundary, including ABI, registers, stack, memory, atomics and instruction semantics.
7. Start with per-function translation validation of a constrained native target. LLVM output alone is not a proof. Machine-level validation must use a specified ISA model and trusted independent checker.
8. Solver outcomes: proven, refuted, unknown, assumed. `atto verify --strict` fails on unknown, assumed, missing certificates or unsupported transformations. Never report partial verification as end-to-end verified.
9. Actor ownership and message passing require separate concurrency and memory-order proofs; ordinary separation reasoning alone does not establish scheduler correctness.

## Implementation phases
- P0: specify `Pointer<T>` ownership/alias semantics and add positive/negative pointer fixtures.
- P0: VIR schema for regions, capabilities, refinements, proof obligations and trust metadata.
- P1: automated separation and refinement discharge via SMT, plus independent certificate checking.
- P1: relational parametricity obligations for constrained generic functions.
- P1: validate one small native executable against an ISA semantics model.
- P2: extend to actor concurrency and multiple backends, retaining backend-owned implementations and shared semantic conformance.

## Acceptance criteria
- No `&` or `*` pointer syntax in Atto examples, parser changes or formatter rules.
- Reject aliasing writes, use-after-take, out-of-bounds and uninitialized pointer reads.
- Demonstrate frame preservation and representation-independent generic behavior under explicit assumptions.
- Negative tests must fail verification; timeouts and unknowns must fail strict mode.
- Verify exact source, IR, binary and proof artifact hashes; report target triple and ISA model.
- Proof checker independent of synthesizer; no untrusted assumptions silently promoted to theorems.
- Tests must execute actual generated machine code where supported; documentation-only changes do not claim execution parity.

## Relationship to existing RFCs
Extends the Atto Verified Core draft RFCs (LLVM #894, Rust #557, Java #1799). This proposal focuses specifically on `Pointer<T>` memory semantics and machine-code proof preservation, without imposing identical library implementations across backends.
