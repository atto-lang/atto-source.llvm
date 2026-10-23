# Oreslang LLVM backend

Native companion to [oreslang-source.java](https://github.com/ores-truffle-oreslang/oreslang-source.java).
Tracks [LLVM port issue #1](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/1).

**Parity commitment: LLVM and Java/GraalVM are first-class Oreslang backends with the same language and observable runtime semantics.** Full feature parity is a standing requirement for the compiler, type system, standard library, actors, supervision, ownership/borrowing, scheduling, async/await, I/O and error handling—not an optional stretch goal. Features added to either backend must be tracked against the other backend and covered by shared conformance tests. LLVM must not be treated as a permanently restricted dialect of Oreslang.

**Current verification status (October 2026):** LLVM has a tested but incomplete native source-to-IR/AOT implementation. **Full end-to-end parity has not yet been verified.** The inventory below documents implementation gaps to close, not exceptions to the parity requirement. Unsupported source fails closed rather than silently compiling with different semantics.

Static select, channels, reusable `SelectPlan`, and mutable `DynamicSelectPlan`
are **not implemented** in the LLVM compiler. The CI regression fixtures
reject them without producing IR. See [the native select parity contract](docs/SELECT_PLAN_PARITY.md).

## Data-oriented design and programming priorities

**Prioritize data-oriented design and data-oriented programming** throughout the
LLVM compiler and native runtime. Design data representations around access
patterns, locality and efficient processing, while preserving Oreslang semantics,
ownership safety, full AOT compilation and zero tracing GC.

Choose and evaluate layouts explicitly:

- **AoS — Array of Structures:** keep fields together when work consumes whole records.
- **SoA — Structure of Arrays:** keep each field contiguous when work scans selected fields or benefits from SIMD.
- **AoSoA — Array of Structures of Arrays:** group field arrays into blocks suited to cache locality and vectorized processing.
- **SoAoS — Structure of Arrays of Structures:** keep related fields together in records within separate arrays when access patterns favor those groups.

Apply these choices to compiler tables, actor state, mailboxes, scheduling and I/O
metadata. Favor compact contiguous storage, batching and fewer allocations or
pointer indirections. Select layouts using representative benchmarks and profiles,
including cache behavior, throughput and tail latency; no single layout is best
for every workload.

## Implemented and verified native functionality (October 2026)

- The two-file `Foo` example now compiles natively with class imports,
  fieldless instance methods, typed `val` declarations, string returns and
  concatenation, and nonblocking console output. See
  [native class/string parity and remaining limits](docs/NATIVE_CLASS_STRING_PARITY.md).
- Parse a relative module graph with one or more `fnc`/`routine` declarations returning `int`, with optional
  `pub` and one optional `define module M [as] ... end` wrapper.
- Typed `int` and `bool` parameters, scalar fields and loans; checked bool operators
  and lazy ternary expressions. See [native bool types](docs/NATIVE_BOOL_TYPES.md).
- `return` expressions, integer literals with underscores,
  unary `-`, parentheses, `+`, `-`, `*`, parameter references and same-module function calls.
- Forward declarations enable calls before definitions and recursion (not bounded recursion).
- Scalar instance methods lower with implicit read/exclusive receiver loans,
  lexical private access and qualified public class identity. See
  [native class methods and remaining limits](docs/NATIVE_CLASS_METHODS.md).
- Relative module selections are statically linked across files, preserving private
  helpers, overload slots and whole-program ownership checking. See
  [native module linking and limits](docs/NATIVE_MODULE_LINKING.md).
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
calling conventions are still **provisional**. Their semantics must be
reconciled with the shared Oreslang specification and Java/GraalVM behavior,
then verified by cross-backend differential tests before those features
are marked parity-complete.
The native slice does not support `-9223372036854775808` as one literal.
Wasm32 remains governed by its distinct embedder/WASI entrypoint ABI.

## Build and test

Requires LLVM development libraries 14+, CMake 3.20+, C++17, Rust and ideally Clang.
The remaining legacy test harnesses also require Python3 during migration.

Use Rust for new and replacement test runners, benchmarks, build utilities and
CI tooling. Keep shell scripts as thin entrypoints. Port existing Python tooling
without losing its checks; invoking Python from Rust does not count as a port.

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

**Mandatory parity work remaining:** actors, async/await, channels, traits,
general heap-allocated classes, package/std imports, tuples, recursive/cyclic owned objects,
memory management, scheduler semantics and compiler-integrated native actor
memory domains. Currently unsupported constructs **fail closed**.
The stack-only class subset and native RAII primitives above are verified
building blocks, not a substitute for the remaining guest-language support.

## Parity development and release policy

- **One language contract:** match Java/GraalVM for parsing, types, ADTs, modules,
  errors, effects, ownership and all observable program behavior. Backend-specific
  implementation choices are allowed; semantic deviations are not.
- **One concurrency contract:** actor isolation, supervision, mailbox delivery,
  channel/select behavior, cancellation, continuations and scheduling guarantees
  must hold on LLVM as on Java/GraalVM.
- **Continuous gap closure:** every feature requires a cross-backend capability
  entry, associated regression fixtures, and explicit implementation/verification
  status. Java-only or LLVM-only implementations remain open parity work.
- **Evidence-driven parity claims:** promote a feature to *verified parity* only
  after equivalent positive, negative and differential execution tests pass on
  pinned revisions. Parser acceptance, rejection tests, or host-only C++ tests
  do not establish source-level runtime parity.
- **Release gate:** general-purpose LLVM distributions must not be labeled fully
  parity-complete until the shared conformance suite and required safety/runtime
  gates pass. Constrained embedded targets may document platform-specific
  capability limits without weakening the full host backend parity target.

The detailed status matrix and open obligations live in the
[source parity contract](docs/SOURCE_PARITY.md).

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
make Spin compile: public module object/method surfaces, channels and async HTTP
still require guest lowering and native runtime integration. See [the full-AOT target](docs/FULL_AOT.md).

## Native HTTP actor integration

An experimental same-process HTTP adapter now dispatches every valid request
through the native bounded mailbox to an LLVM-compiled scalar Oreslang actor.
It uses kqueue/epoll, actor-owned heaps, generation-fenced exchange ownership,
keep-alive and a sustained benchmark. It does **not** compile the Spin framework
or its async/OTel modules. See [build, ownership and limits](examples/native-http/README.md).

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

## Exact-tree host concurrency analyzer test gate

The race analyzer is host-only and **does not yet instrument LLVM guest loads
and stores**. Its logical participant IDs must identify distinct concurrent
task/message lifetimes, not a shared actor reference, and recursive lock
ownership publishes a happens-before edge only on the final unlock. Invalid
control-character event identities are rejected. Native `fork/finish/join`
supports fresh logical task lifetimes, rejects unfinished/repeated joins and
preserves sibling concurrency. It still depends on complete, trustworthy
compiler/runtime lifecycle instrumentation and does **not** prove guest
race-freedom. An authorized private runner can validate the exact source tree
using:

```sh
EXPECTED_SOURCE_TREE_SHA=<40-character-source-tree-sha> bash scripts/ci-verify-concurrency-p0.sh
```

The test compiles the analyzer with ASan/UBSan and strict warnings, runs the
adversarial C++ host tests and emits an evidence receipt only on success.
A mirror must contain the exact private source Git tree, with its SHA verified
before tests; this script neither performs cross-organization checkout nor
bypasses access controls. Host tests alone cannot certify a complete guest
memory race checker.

To run the entire repository test suite alongside the targeted exact-tree gate,
set `RUN_FULL_SUITE=1` when invoking `scripts/ci-verify-concurrency-p0.sh`.
This requires a working authorized runner; scripts and setup alone do not
constitute executed test evidence.


## Tree shaking

The native compiler now has an opt-in `--tree-shake` direct-call reachability
slice. It runs after parsing/semantic admission and before LLVM IR emission.

Current roots are `main` and `pub` functions. The pass follows direct
same-module calls transitively and removes unreachable private functions, which
also prevents function-local runtime requirements from being discovered by
later codegen when their owning function is removed.

This is deliberately narrower than the cross-backend target: classes, actor
types, dynamic dispatch, reflection/FFI, hot-load roots, stdlib/module pruning,
and the versioned reachability report still need follow-up support. Rootless
strict tree shaking fails closed rather than silently producing an empty
artifact.

The executed CTest gate compiles both shaken and unshaken IR through Clang and
requires identical process behavior while asserting the dead private symbol is
absent from the shaken IR.
