# Oreslang generic-scalar conformance v1

This corpus is a **source-level shared contract** for the intersection of Java/GraalVM and native LLVM generic free functions. Java serves as the established semantic reference; LLVM's current implementation is an explicit **int64-only** slice and is not general monomorphization.

The tests cover explicit and inferred scalar slots, ordered dependent defaults, named overrides, bare `_` inference holes, and fail-closed admission for unknown/duplicate/forward/nested generic parameters. Each case has one immutable `.ores` file. Backend-specific diagnostic substrings are recorded separately: sharing a rejection category does not imply identical error text.

Statuses: `required` means the backend is obligated to satisfy the case, **not** that it has been independently run. `pending` means the backend's language semantics differ or its implementation is not certified. The native LLVM `bool` rejection is `pending` on Java because bool is a legal Java generic specialization.

A conformance run must prove full source bytes, actual compilation, and executable result for `native_exit` cases. A diagnostic passes only on a non-crashing failure **before any IR** (LLVM) or failed `--check` admission (GraalVM), with a matching backend-specific diagnostic. Compilation acceptance alone is not runtime parity.

The suite is validated and executed using `verify.py`. Run `python3 verify.py --validate-only`, or supply `--backend llvm --compiler /path/to/oreslang-llvmc --clang /path/to/clang`. Graal execution support is separately gated until the reference binary's exit-code contract is confirmed.

Extend this corpus in the common-source repository first, then update Java and LLVM harnesses with exact Git hashes. Do not claim parity for method/class/actor/non-scalar generic constructs until both backends admit and execute them.

## Expanded stage-one regression inventory

Six new canonical cases, promoted from the LLVM #401 test matrix, cover long dependent-default chains, positional-plus-named type parameters, explicit inference holes with independent defaults, uninferrable dependent holes, duplicate generic declarations, and self-dependent defaults. Identical cases already present in v1 are deliberately not duplicated. LLVM execution/diagnostics are required; new Java/Graal cases remain pending until independently executed.

## Type-safety requirement for portable positive fixtures

Positive cross-backend fixtures must be well-typed *before specialization* as well as afterward. In particular, an unconstrained generic type parameter is not known to support `+`: Java rejects arithmetic on `A` and `B`, while the current LLVM scalar-only specialization may accept that usage by monomorphizing to `int`. The default-chain, named-override, and positional/named fixtures therefore use `A`-typed identity returns (`return x`) to prove type binding, defaults, and the executable exit code without making an unproven numeric-trait assumption. Numeric generic arithmetic belongs in a separately constrained contract once a shared bound/trait is specified.

## JVM and GraalVM Native Image evidence

The six new regression cases were independently executed using identical Java source and fixture bytes with the actual JDK 25 launcher ([private JVM Actions run #38813072525](https://github.com/embedded-alerts-test/web-ui-e2e/actions/runs/38813072525)) and a compiled GraalVM Native Image AOT Oreslang launcher ([private run #38812706253](https://github.com/embedded-alerts-test/web-ui-e2e/actions/runs/38812706253)). Both reported all 13 previously required cases passing; the six newly added cases also passed as observed pending tests. This evidence justifies promoting those six to required, using the actual diagnostic substrings observed for Java rejection cases. The expanded 20-case corpus passed real native LLVM IR→Clang→guest tests ([private run #38812430633](https://github.com/embedded-alerts-test/web-ui-e2e/actions/runs/38812430633)).

The `nonscalar-native-rejected` case remains **required for LLVM and pending for Java** because Java intentionally accepts that specialization; promoting it would be an incorrect language claim. Required `graal` cases must pass *both* real JVM guest execution and GraalVM Native Image AOT guest execution. Graal admission-only mode in `verify.py` is insufficient to claim that parity; use `scripts/verify_generic_guest_parity.py` or an equally strict executor and compare actual exit codes.
