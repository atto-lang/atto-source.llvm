# Native LLVM type-only declarations, constrained v1

This is a **compiler-only frontend admission slice**, not full language/backend parity with
Java/GraalVM. It is tracked alongside Java [PR #1947](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/1947).
The native executable subset remains intentionally bounded.

## Supported surface

At the source root or inside the existing flat `define module ... as ... end` wrapper:

```oreslang
declare type LocalId = int;
declare pub type PublicId = bool;

declare trait Tagged as
  fnc tag(int value) => int;
end

declare pub interface Endpoint as
  fnc request(): int;
end
```

- `declare pub type`, `declare pub trait`, `declare pub interface` are canonical. 
- Compatibility: `pub declare ...`, `define type/trait/interface`,
  `define pub ...` and `pub define ...` (within modules) are accepted.
- `declare` is contextual: normal functions may still be named `declare`.
- Alias targets are `int`, `bool` or another type-only scalar alias, including
  forward references between alias definitions; all alias edges are validated
  even when the alias is unused. Cycles, unknown targets, trait/interface
  targets and chains deeper than 128 are rejected before native IR.
  Alias names are resolved to `ScalarType::I64` or `ScalarType::Bool` for
  native function parameters/returns, explicitly typed scalar locals, and
  already-resolved trait/interface scalar method signatures. `int` aliases
  are allowed as native scalar-generic defaults and arguments; `bool`
  aliases must not bypass the int-only native generic gate.
  Trait/interface signatures admit 
  `int` and `bool` value parameters, and `int`, `bool` or `void` results;
  arity-disambiguated method declarations, source-order method metadata and
  same-kind intra-unit `extends` are retained.
- Constrained aliases now support descriptor-v1 metadata such as
  `declare type Code = int with min 0 and max 5;`. Only direct `int`
  aliases may carry v1 constraints. `min` / `max` are canonicalized to
  inclusive signed-i64 predicates, including exact i64 endpoints. The
  frontend computes the same `ores.type-constraint.v1` canonical string and
  SHA-256 fingerprint as Java/GraalVM. `--constraint-descriptors` is a
  metadata-only compiler mode that emits the descriptor identity, fingerprint,
  and canonical form without enabling value construction.
- Constrained aliases remain **fail closed as values**. Parameters, returns,
  locals, branch locals, extension receivers, and aliases that transitively
  target a constrained alias reject with
  `CONSTRAINT_VALIDATION_BRIDGE_REQUIRED` before LLVM IR. Descriptor metadata
  is not validation provenance.
- Declarations are **not values**: the AST stores `TypeOnlyDecl::Kind` identity
  for alias/trait/interface and emits no constructor, global, actor or IR
  representation. Separate class, actor and module-contract type identity
  collisions are rejected before native lowering.
- Missing inheritance parents, incompatible kinds, cycles, duplicate names,
  duplicate formal parameter names and duplicate (name,arity) slots are rejected
  with stable `TYPE_*` diagnostics. Admission is bounded by 1,024 type-only
  declarations per unit, 128 inheritance edges of depth, 32 direct parents,
  128 method signatures per interface/trait, and 32 parameters per method.
- Source-level test matrix: `tests/native_type_only_declarations.py`; the
  `native_type_only_declaration_boundaries` CTest runs it through
  `oreslang-llvmc` and checks for IR-only positives and fail-closed negatives.

## Explicitly unsupported (must fail, not silently lower)

- A bare `declare type Id;` or `define type Id;` is invalid. Both source
  introducers require a complete `= target;` definition. An alias must be
  declared before use in native executable signatures or locals; a *complete*
  alias definition may refer to another alias defined later in the same unit,
  with all targets checked before LLVM IR.
  Alias-based class fields and materializing trait/interface instances remain
  unsupported.
- Generic aliases/traits/interfaces, structural fields, default implementations,
  implicit trait evidence/dictionaries and cross-unit import/link resolution.
- Arbitrary nominal, owned, borrowed, floating, string or generic members,
  trait/interface transport, dynamic dispatch or typeclass instance selection.
- `define mtd name on Alias(...): Result { ... }` lowers to a
  compiler-owned static function `$mtd$Alias$name` with the named receiver
  inserted as argument zero. Dispatch is selected from lexical alias identity
  before aliases erase to the i64/bool transport ABI. Raw primitives and
  unrelated aliases do not inherit the method merely because they share the
  same scalar representation. Root/module declarations, forward declarations,
  explicit-arity slots and named arguments are covered by source tests plus a
  compiler -> LLVM IR -> Clang -> native-process gate. Those tests remain
  authored rather than executed while hosted Actions creates zero jobs.
- `define model` general computational graphs, model constructor values,
  digests, composition and execution. The frontend explicitly emits
  `MODEL_BACKEND_UNSUPPORTED` until shared-model, checked operation-provenance,
  static-configuration and ownership/codegen parity land under LLVM issue #621.
- LLVM native `import` still fails closed until a validated cross-unit linker
  exists; public visibility does **not** imply a working linker.

## Merge and verification gates

1. Build exact commit on an approved same-repository native Actions runner and
   run `ctest -R 'native_(type_only_declaration_boundaries|type_constraint_descriptor_v1)'`
   with `--output-on-failure`.
2. Run the complete LLVM CTest regression suite and the existing ownership /
   hard-RT / scalar compiler source matrices. Actual job steps and compiler output
   are required; an Actions job showing zero steps is not compiler evidence.
3. Compare accepted/rejected declaration corpus, nominal-kind compatibility,
   errors, and format/parse identity against Java/GraalVM and oreslang-format.
4. Extend the shared linker, nominal type/protocol semantics, and general
   source-level alias use **only** with explicit ownership/ABI tests; never
   make imported type-only declarations executable by treating them as
   ordinary classes or callables.
5. Independently approve any private-source cross-organization CI carrier
   through protected source repository policy before exporting a tree.

## Equivalent completed type aliases

Both forms compile to the **same** type-only alias metadata and native scalar ABI:

```oreslang
declare type Code = int;
// Equivalent source spelling:
// define type Code = int;
```

Do not write both lines as active definitions in the same module: that would
redeclare `Code`. The language has **no opaque forward type promise** in this
syntax, and neither spelling creates an instantiable runtime value. Native
source type checking resolves declared aliases to the existing i64/bool ABI.
The alias spelling is compile-time metadata, not a newtype/refinement proof.

## Executable scalar alias example

```oreslang
declare type UserId = int;
declare pub type PublicId = UserId;

fnc identity(PublicId value): PublicId {
  return value;
}

pub fnc main(): int {
  val id: UserId = 7;
  return identity(id);
}
```

Native source type checking uses the underlying i64/bool ABI while maintaining
compile-time alias source identity; this example does not create a heap owner,
dictionary or alias object. It does **not** establish that aliases provide newtype
nominality: neither `define type` nor `declare type` implies refinement or
newtype validation without a separately checked constrained-type contract.
