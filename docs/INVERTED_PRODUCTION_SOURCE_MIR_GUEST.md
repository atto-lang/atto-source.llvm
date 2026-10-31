# Native LLVM production inverted-i64 source → verified MIR → checked IR (draft)

## Scope: one genuinely parsed source-to-MIR main

This milestone introduces a *separate, narrow production Program AST* slice, not
an expansion of the --experimental-inverted-i64 adapter. The real production
Lexer/Parser.cpp constructs InvertedSourceMain operations and stores them in
Program.inverted_main; resolve_and_validate_program independently validates
the source actions through InvertedProductionSourceMir.hpp, Layout,
LinearPlan and Module::lower. The normal oreslang-llvmc CLI then consumes the
same verified Module in InvertedMirLlvmI64.hpp, without an experimental flag.

The supported source **must** have exactly one inverted struct with exactly two
int/i64 columns and one top-level fnc main(): int. Fields are immutable by
default; an explicit let permits post-insertion mutation. Exactly one local
let owner must use InvertedArray<T>.with_capacity(constant 1..256), followed
by bounded literal-only rows.push(i64,i64), rows.set(index,field,i64),
rows.erase_swap(index), and a terminal return rows.get(index,field).
No general expressions, aliases, loops, calls, other functions, methods, actors,
network or escaped handles. An unsupported source does not emit guest IR.

Example source: tests/fixtures/inverted-i64/positive-production-erase-swap.ores

The guest storage is stack-local with two fixed i64 arrays plus a checked
local live-row marker, an owner-open flag and length. Read/write operations
are owner-guarded and length-guarded; MIR verifies mutability and operation
order independently of the parser. Erase-swap is admitted only for a row
within the statically tracked logical length, so an invalid constant erase
is a compile-time rejection. A runtime invalid indexed read or write traps.
A terminal read is followed by MIR close and volatile erasure of all three
bounded local lanes. The local row marker is *not* a freely movable,
generation-checked cross-owner handle: those semantics remain disabled.

No wasm32, Cortex-M, actor/task suspension, ownership mode, runtime-lifetime
or hard-real-time/profile admission is implied. This path does not call the
general-purpose emit_verified_llvm_ir; it is deliberately scoped to a separately
verified typed-MIR backend. Supporting general source expressions and
cross-function lifetimes requires full production HIR and ownership integration.

## Exact-head verification requirements

1. Configure/build full production oreslang-llvmc with actual LLVM SDK and
   the real production Parser.cpp, ModuleLoader.cpp and main.cpp.
2. Run native_inverted_struct_schema_ast and
   native_inverted_production_source_to_native, not just the adapter smoke.
   The latter executes three default-compiler .ores programs through Clang
   at -O2, asserts native process exit 42, requires the out-of-bounds guest
   to trap, and confirms six safety/unsupported-mode failures before IR.
3. Run inverted_production_source_mir_cpp17 on GCC and Clang, ASan/UBSan and
   the existing standalone source adapter/typed owner test suite.
4. Run the complete CTest suite to detect unintended parser/AST regressions.
5. Verify exact source SHA, source tree blob identities and genuine executed
   job steps. Jobs failing before their first step count as NOT_EXECUTED.

Commands in a permitted full LLVM SDK environment:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build -R 'native_inverted_(production_source_to_native|struct_schema_ast)|inverted_production_source_mir_cpp17' --output-on-failure
```

The standalone C++17 admission gate without LLVM-dev:

```sh
cmake -S tests/inverted-i64-standalone -B build/inverted-standalone -DCMAKE_CXX_COMPILER=g++ -DBUILD_TESTING=ON
cmake --build build/inverted-standalone --parallel 2
ctest --test-dir build/inverted-standalone -R inverted_production_source_mir_cpp17 --output-on-failure
```

## Evidence boundary

These are committed implementation and tests, **not passed execution proof**.
The source organization has returned zero-step Actions failures and this local
environment has not checked out the private full tree or installed LLVM
development packages. No cross-org mirroring occurred in this milestone.
Do not merge or promote five-backend parity, no-GC guarantee or hard-RT
guarantee on the basis of committed tests alone.

Before broader admission: support production scalar expression AST operands
and source-level ownership flow; implement movable generational row handles,
field-disjoint borrows, quiescent owner lifetime and task suspension rules;
then port the shared exact behavioral fixtures to Java, WASM, .NET and Forge.
