# OwnershipSourceLoweringV1 — resolved source to ownership CFG

**Status:** first real typed-Program adapter, limited to the currently admitted
ActorRegionV2 class-owner source slice. It is not yet a general source CFG
builder or full-language ownership checker.

## Pipeline

The normal compiler driver parses, resolves and validates the source Program
before the explicit ActorRegionV2 AOT backend is entered. That backend now
calls `OwnershipSourceLoweringV1::lower_guarded_owned_class_function` with
references to the canonical Function and ClassDecl inside that Program.
The adapter rejects noncanonical references, extra program roots, imports,
actors, generators, sum/type declarations, contracts and other unmodelled
roots instead of deriving a partial CFG.

It maps the actual lexical class allocations in `Function.locals` to ordered
owner identities, maps direct owner.field expressions in the guard/early return
and normal return to owner-use operations, and builds the explicit three-block
CFG: entry/acquisitions/guard-use, early-return use, normal-return use.
OwnershipDataflowV1 validates the graph and produces the cleanup list for both
source return paths. ActorRegionV2 checks that result against its existing
backend cleanup contract and uses the source-derived order for LLVM emission.

## Admitted source slice

- The exact canonical source Program has one class and one function.
- `main/0` has 1–8 fresh, immutable, nonescaping instances of that class.
- The class has 1–16 public, final, immutable literal integer fields.
- One pure equality guard selects an early field return; the normal path has
  one direct field return.
- Every field receiver names one of those live owners and every member exists
  in the declared class layout.
- No ordinary statements, branch-local allocations, method calls, moves,
  borrows, exceptions, defer/recover, await/cancel, actors, FFI or dynamic
  effects are admitted.

## Tests and proof limits

`native_ownership_source_lowering_v1` constructs resolved Program AST
instances and checks one positive two-exit source lowering and five rejection
cases (foreign owner, unknown field, unmodeled statement, hidden program root
and mutable/escaping owner). The dataflow unit suite is also expanded to
8 positive / 21 negative cases, including historical acquisition order across
move/drop/reallocation.

These tests are authored and registered. They are **not passing evidence**
until a native CMake/CTest job executes on this exact head with nonzero steps.
They do not prove arbitrary source Statements are converted into CFG blocks,
because this first adapter rejects all statements. It also does not admit
branch-local objects, nested scopes, source-level rt take/borrow, exception,
suspension, cancellation, actor scheduler authority or cross-backend parity.

No existing frontend, runtime or code-generation implementation was removed.
The strict zero-GC linker gate remains closed pending transitive runtime/FFI
certification.
