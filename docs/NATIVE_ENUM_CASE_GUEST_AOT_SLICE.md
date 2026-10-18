# Experimental native enum-case guest-code AOT: first vertical slice

**Status: draft, opt-in, not production-qualified.** This branch introduces
a real Oreslang source-to-LLVM IR lowering path in `oreslang-llvmc` behind
`--experimental-enum-aot`. It does **not** turn on nominal enums in the
normal native frontend, Java/Graal runtime, Native Image, or any other backend.

This is guest code AOT (actual compiled Oreslang functions with native call
sites), **not** a Native Image enclosing an Oreslang interpreter. The path is
limited to a local root nullary enum and a same-name overload family whose
functions each return an integer literal. `main(): int` may define immutable,
explicitly typed nullary enum cases and return a sum of direct calls.

## First executable test

```ores
define enum Foo as A, B end
fnc classify(Foo.A value): int { return 10; }
fnc classify(Foo.B value): int { return 20; }
fnc classify(Foo value): int { return 0; }

fnc main(): int {
    const Foo.A specific = Foo.A;
    const Foo widened = Foo.A;
    return classify(specific) + classify(widened);
}
```

When built and qualified, the first call directly targets the `Foo.A`
`eos2` symbol and the second directly targets the whole-`Foo` fallback
symbol. Both pass the same 32-bit source tag value zero-extended into the
first slice's native i64 argument ABI. The desired native process exit status
is **10**. No dynamic tag-switch overload resolver is emitted.

## Exact compiler-source qualification commands

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --target oreslang-llvmc --parallel 2

./build/oreslang-llvmc --experimental-enum-aot \
  tests/fixtures/enum-aot-basic.ores > /tmp/ores-enum-aot.ll
clang -x ir -O0 /tmp/ores-enum-aot.ll -o /tmp/ores-enum-guest
/tmp/ores-enum-guest
# Expected exit code: 10

python3 tests/native_enum_guest_aot.py \
  --compiler "$(realpath build/oreslang-llvmc)" \
  --clang "$(command -v clang)" \
  --fixture tests/fixtures/enum-aot-basic.ores
```

The source `.github/workflows/ci.yml` has an exact-PR-head checkout
assertion and runs the Python guest-AOT matrix after the real LLVM compiler
build. The smoke checks binary execution, distinct exact `eos2` direct
calls, AOT retagging stability, no default-mode enum admission, and 21
negative syntax/provenance/ambiguity/effect/target cases. Real LLVM module
verification is performed in `emit_verified_enum_aot_ir`.

## Fail-closed scope and security

- One local nullary enum; no imports, payloads, record values, generics,
  actors, suspension, virtual methods, matching or dynamic dispatch.
- One synchronous overload family with one enum argument per function.
  Functions currently have literal integer returns only.
- Main accepts immutable, explicitly typed enum locals preceding a sum of
  direct one-argument calls. No unknown/complex expressions or effects.
- The source parser computes exact declarations and static argument
  refinements with `EnumCaseOverloadPolicy::selectStatic`.
- The LLVM emitter **independently revalidates** family admissibility,
  enum tags and static target selection, even if the internal C++ plan
  aggregate is mutated. No guest value supplies an overload name or slot.
- Numeric wire tags are value representation, not overload identities:
  A statically widened `Foo` remains whole-`Foo` even for an `A` value.
- Host-only; Wasm/Cortex-M and mixed experimental modes are rejected.
- Default native parsing still rejects enum source before this opt-in
  experimental slice is requested.

## Evidence and non-evidence

The independently authored, **source-free** `sagitta-stack/sagitta-infra`
model CI compiled and executed synthetic native LLVM IR with Clang and
validated exact `eos2` link behavior for randomized wire tags:
https://github.com/sagitta-stack/sagitta-infra/actions/runs/39139056290 .
This contains **no private Oreslang repository source** and cannot establish
a successful compilation of this PR's private C++ compiler code.

Source-repository Actions continue to report failed jobs **without runners
or executed steps** (e.g., `runner_id=null`, `steps=[]`). Therefore
this branch is **not yet certified** for CMake/Clang source compilation,
LLVM module verifier execution, binary execution, sanitizer conformance,
JDK25, GraalVM Native Image, or general Oreslang enum parity.

The Java source owning organization has an empty reviewed private-carrier
allowlist. Cross-org Git tree/source-byte mirroring is forbidden until a
separately approved private/private destination and authenticated security
preflight explicitly authorize that export. Successful source-free funded
CI is not an authorization to mirror private compiler source.

## Next vertical expansions

1. Certify this exact source head by real CMake+Clang execution on a
   source-authorized runner; fix any compile/runtime regressions found.
2. Reconcile canonical parser/HIR with general nominal enum AST and
   TypeChecker/OwnershipChecker provenance so the opt-in parser can be
   retired instead of duplicated indefinitely.
3. Support bounded simple expression bodies and match narrowing without
   inspecting runtime tags for overload resolution.
4. Carry exact `eos2` symbol IDs across imported units with ABI digests.
5. Implement exact-signature virtual root slots, payload ownership, actor
   messages and full frontend/backend AOT parity.
