# Oreslang LLVM backend

Native companion to [oreslang-source.java](https://github.com/ores-truffle-oreslang/oreslang-source.java).
Tracks [LLVM port issue #1](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/1).

**Status: small experimental source-to-LLVM compiler, not Java language/runtime parity.**

Static select, channels, reusable `SelectPlan`, and mutable `DynamicSelectPlan`
are **not implemented** in the LLVM compiler. The CI regression fixtures
reject them without producing IR. See [the native select parity contract](docs/SELECT_PLAN_PARITY.md).

## Implemented slice (October 2026)

- Parse one file with one or more `fnc`/`routine` declarations returning `int`, with optional
  `pub` and one optional `define module M [as] ... end` wrapper.
- Typed `int` parameters, `return` expressions, integer literals with underscores,
  unary `-`, parentheses, `+`, `-`, `*`, parameter references and same-module function calls.
- Forward declarations enable calls before definitions and recursion (not bounded recursion).
- Reject undeclared names, duplicate function/parameter names, mismatched call arities,
  overflowed positive i64 literals, malformed syntax, and unsupported language features.
- Lower each function to i64 LLVM IR, verify the entire LLVM module before printing IR.
- CI covers positive/negative source-to-IR contracts and Clang compilation/execution of
  standalone `main` functions, when Clang is present.
- A narrow nonescaping **stack-only class** form: `define class Box as` with
  `let int` fields initialized by literals; `let Box b = new Box()` inside
  a function; reading `b.field` in an integer expression. Compiles to LLVM
  stack `alloca` storage, with no tracing GC or native malloc. Returning class objects or unsupported moves/shares fails closed; admitted
  call-scoped borrows are checked by the current ownership CFG.
- `OwnedHeap.hpp` provides move-only native RAII owners, runtime borrow leases,
  accounting and deterministic destruction. `EpochArena.hpp` provides
  actor-carrier-local, bounded typed regions with reverse-order drop and
  quiescence fences. Both have CTest and sanitizers; general `.ores` objects
  are **not yet lowered into these allocators**.

Oreslang `int` still lowers to `i64` for guest functions. For a
zero-argument `main()` (unqualified or `module.main`), native compilation
emits a separate `i32 @main()` C/OS wrapper that calls the Oreslang
entrypoint. An unqualified guest main gets an internal generated symbol;
a module-qualified main retains its own exported guest symbol. Falling off a
`main(): void` body returns status `0`. An integer `main()` must return
a code within `0..255`; values outside that range return failure status `1`
rather than silently truncating to an unrelated shell status. Native source
`std.process.exit()` and exception effects remain unsupported and fail closed.

The `int` to `i64` mapping, wraparound arithmetic and other non-entrypoint
calling conventions are still **provisional** and do not imply Java parity.
The native slice does not support `-9223372036854775808` as one literal.
Wasm32 remains governed by its distinct embedder/WASI entrypoint ABI.

## Build and test

Requires LLVM development libraries 14+, CMake 3.20+, C++17, Python3 and ideally Clang.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
./build/oreslang-llvmc tests/fixtures/forward-call-main.ores > /tmp/guest.ll
clang -x ir /tmp/guest.ll -o /tmp/guest
/tmp/guest
echo $? # 42
```

The compiler CLI prints LLVM IR; the AOT driver below links it into a host executable.
Direct object output and a versioned ABI remain unimplemented.

Unsupported actors, async/await, channels, traits, general heap-allocated classes,
imports, tuples, recursive/cyclic owned objects, tracing GC, HFT scheduling
and compiler-integrated native actor memory domains **fail closed**.
The narrow stack-only class subset and native RAII runtime primitives above
are the first tested steps toward an ownership-first backend.

Native I/O API naming: blocking I/O operations will require the `_blocking` suffix; async I/O may use `_async`. This is a documented target contract, **not yet an implemented compiler check**.

See [source parity contract](docs/SOURCE_PARITY.md), [LLVM roadmap](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/1),
and the distinct [Java cyclic graph work](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/474).

## Standalone AOT executable

After building the compiler, compile its supported **host** subset without a JIT:

```sh
CLANG=clang scripts/compile-aot.sh tests/fixtures/forward-call-main.ores /tmp/ores-app
/tmp/ores-app
```

This driver emits verified IR, invokes Clang with `-O2`, and atomically publishes
an executable. It needs the compiler and Clang only at build time; the output
requires neither Java nor an Ores interpreter. A failed compile/link preserves
any existing output. No unsupported source falls back to interpretation.
This is not a static-linking or cross-compilation guarantee, and it does not
make Spin compile: imports, actors, channels and async HTTP still require guest
lowering and native runtime integration. See [the full-AOT target](docs/FULL_AOT.md).

## Experimental Cortex-M3 native AOT (scalar subset)

`oreslang-llvmc --target=thumbv7m-none-eabi` now admits a deliberately narrow
freestanding `int` function subset, emits ARM-targeted LLVM IR and rejects
actors, owners, suspension, non-scalar locals, guest `main` and unsupported
runtime features. Clang 18 cross-compiles the IR into 32-bit ARM ELF objects.
This **does not** constitute general embedded actor/runtime support.

**Verified firmware smoke:** the exact compiler source change
`ba8552bc9ca8cc4e9edc3b7f665ed2a1ed9a5b39`, combined with
`oreslang-platform.embedded@5705a25b0dc5ee9ddb1a7c15ebe747142d51975c`,
produced Zephyr 4.3.0 firmware that ran on QEMU Cortex-M3 and logged
`ORESLANG_CORTEXM_AOT_PASS value=42`.
[GitHub Actions evidence](https://github.com/canonical-cloud/canonical-docs/actions/runs/38646752678).
See [target constraints](docs/CORTEXM_SCALAR_IR.md).


## Experimental Cortex-M3 source actor bootstrap

The compiler's `--target=thumbv7m-none-eabi` target now includes a
separately bounded scalar-actor slice, in addition to pure `int` functions.
It admits source declarations and a restricted `main` with `spawn`, literal
`send`, and `stop`, exposing `oreslang_cortexm_bootstrap` and
compiler-generated actor init/handler ABI functions. The embedded platform
provides the static adapter; this is **not** full actor lowering, inter-actor
handler sends, suspension, or standard OresVM on Cortex-M.
See [admission limits](docs/CORTEXM_SCALAR_IR.md).

See [value-boundary audit and admission tests](docs/VALUE_BOUNDARIES.md) for
aggregate, call, string, conditional and async feature limits.
