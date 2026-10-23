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
- Alias targets are exactly `int` or `bool`. Trait/interface signatures admit 
  `int` and `bool` value parameters, and `int`, `bool` or `void` results;
  arity-disambiguated method declarations, source-order method metadata and
  same-kind intra-unit `extends` are retained.
- Declarations are **not values**: the AST stores `TypeOnlyDecl::Kind` identity
  for alias/trait/interface and emits no constructor, global, actor or IR
  representation. Separate class, actor and module-contract type identity
  collisions are rejected before native lowering.
- Missing inheritance parents, incompatible kinds, cycles, duplicate names
  and duplicate (name,arity) slots are rejected with stable `TYPE_*` diagnostics.
- Source-level test matrix: `tests/native_type_only_declarations.py`; the
  `native_type_only_declaration_boundaries` CTest runs it through
  `oreslang-llvmc` and checks for IR-only positives and fail-closed negatives.

## Explicitly unsupported (must fail, not silently lower)

- Using the alias in executable native function/field signatures, or materializing
  trait/interface instances. Type-only metadata is not yet a resolved native
  signature ABI; merely storing `type Id = int` **does not enable** `fnc f(Id x)`.
- Generic aliases/traits/interfaces, structural fields, default implementations,
  implicit trait evidence/dictionaries and cross-unit import/link resolution.
- Arbitrary nominal, owned, borrowed, floating, string or generic members,
  trait/interface transport, dynamic dispatch or typeclass instance selection.
- `define model` general computational graphs, model constructor values,
  digests, composition and execution. The frontend explicitly emits
  `MODEL_BACKEND_UNSUPPORTED` until shared-model, checked operation-provenance,
  static-configuration and ownership/codegen parity land under LLVM issue #621.
- LLVM native `import` still fails closed until a validated cross-unit linker
  exists; public visibility does **not** imply a working linker.

## Merge and verification gates

1. Build exact commit on an approved same-repository native Actions runner and
   run `ctest -R native_type_only_declaration_boundaries --output-on-failure`.
2. Run the complete LLVM CTest regression suite and the existing ownership /
   hard-RT / scalar compiler source matrices. Actual job steps and compiler output
   are required; an Actions job showing zero steps is not compiler evidence.
3. Compare accepted/rejected declaration corpus, nominal-kind compatibility,
   errors, and format/parse identity against Java/GraalVM and oreslang-format.
4. Extend alias resolution and the shared linker **only** with explicit
   source-level ownership/ABI tests; never make imported type-only declarations
   executable by treating them as ordinary classes or callables.
5. Independently approve any private-source cross-organization CI carrier
   through protected source repository policy before exporting a tree.
