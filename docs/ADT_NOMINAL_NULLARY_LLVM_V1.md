# LLVM nominal enum v1 (opt-in, closed nullary source)

This compiler patch adds the explicit command-line switch
`--experimental-nominal-enum-v1` to `oreslang-llvmc`. The ordinary
frontend retains its fail-closed nominal `define enum` admission.

The restricted `.atto` input accepts exactly one local `define enum`,
2–32 unique nullary cases, optional positive unique signed-i32 case
tags, a `pub fnc main(): int`, one immutable locally constructed
`val Enum value = Enum.Case`, and a closed exhaustive
`match value over on Enum.Case -> { return N; } ... end`.
No user input can supply a foreign raw tag; enum symbols remain nominal
at source level. Wrong owner, duplicate case/tag/arm, missing case,
payload, numeric forged constructor, and unimplemented source forms
reject before LLVM IR generation.

The compiler-only lowering transforms the *semantically checked* source
into the existing verified scalar native frontend's internal
`val int` + typed equality/conditional syntax. The real
`oreslang-llvmc` compiler and LLVM IR/backend then compile that
code. The generated integer tags are private compiler representation,
**not** guest-visible integer values or external enum ABI. The
original disk source is identity-pinned and UTF-8 checked before
translation. This opt-in lane explicitly refuses non-host targets,
realtime, allocator/heap experiments, imports, actor/isolate and
cross-module use. It is not a generic ADT AST/layout implementation.

Verification tiers:
1. `native_nominal_enum_v1_source_admission` CTest checks source
   owner/variant, exhausted matches and 15 negative admission cases
   (host-only).
2. Native compiler-backed guest test compiles the canonical exact-source
   `tests/fixtures/adt-nominal-nullary-v1.atto`, emits actual LLVM IR,
   and executes a Clang linked binary with expected exit status 20 at
   both O0 and O2. Compiler-side negatives must produce no LLVM IR.
3. ASan/UBSan, generic payload case ownership, borrowed values,
   actor mail, import identity, generation fences and all 12 canonical
   nominal ADT conformance fixtures remain additional **unproven**
   milestones. Do not certify any such property from v1 tests.

This source shape matches the .NET experimental nullary enum
subset, not the full intended AttoLang `define enum` language.
