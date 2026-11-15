# AttoLang Native LLVM backend

> **Security integration (draft):** [AttoLang LLVM wasm32 actor → Volucro authenticated Wasmi Store](docs/ATTO_VOLUCRO_WASM_ACTOR_ISOLATION_V1.md) supplies a real source-to-Wasm-to-Volucro smoke harness, exact-head provenance checks, and documented security gaps. Not production native tenant isolation.


**AttoLang** is the current public language name (formerly **Oreslang**).
This compiler/runtime remains the independent native LLVM implementation; existing tool names, `.ores` fixtures, module identifiers and compiled ABI remain unchanged.
This branding change does not establish new syntax, source-extension support,
execution parity, AOT capabilities or zero-GC guarantees. New source SHOULD
use `.atto` only after this particular backend has passed a real compile/run
admission test; `.ores` remains supported wherever already implemented.
Do not rename repositories, dependencies, commands or fixed interfaces merely
to make their names match the public brand.


Native companion to [oreslang-source.java](https://github.com/ores-truffle-oreslang/oreslang-source.java).
Tracks [LLVM port issue #1](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/1).

Please keep parity with oreslang-source.java in terms of features and capability. The same guest code that runs on graalvm should compile and execute with gcc/llvm.
Do not use python for anything, use Rust instead of python to support the language.

Notes on the compiler and runtime:

* Strive for zero gc. The borrow checker should accomplish this, and having separate heaps/arenas per actor helps reduce any remaining gc overhead.
* Borrow checking uses reserved keywords `rt copy, rt share, rt take, rt borrow`
* Go for full AOT. Some JIT elements are ok, as long as they can be explicitly controlled/governed by user. But we need native binaries.
* Strive for hard real time not just soft-real time by minimizing JIT (or having JIT switched off for certain paths) and moving towards full AOT.
* Use data-oriented design and data oriented programming - be mindful of using AoS, SoA, AoSoA and SoAoS where possible.
* Use 32 bit pointers instead of 64 bit pointers for allocation/heaps. Having separate heaps per actor helps with this. 32 bit pointers helps us save up to 50% on memory/RAM. The 32 bit pointers should be implemented so that it stays 32 bit even on Arm-64 machines, etc etc.
* OresLang must be built around excellent tree-shaking so we can get final binary executables as small as possible and reduce startup overhead etc.


## OresVM root host and tenant IsolateVM architecture

**OresVM** is the root supervisor and tenant admission runtime; each **IsolateVM** is a tenant-owned execution domain containing many Oreslang actors. The OresIsolateContext is a temporary guest-call ABI view, not a separate VM. This distinction and the relevant V8 isolate/context, pointer-compression and security lessons are documented in [OresVM vs IsolateVM](docs/ORESVM_VS_ISOLATEVM.md).

The host uses a concrete, privately owned IsolateVM C++ class. There is also a narrow opt-in synchronous scalar .ores-to-LLVM isolate entry, but general guest source lowering, the actor scheduler and OS-process sandboxing remain outstanding. See [OresVM tenant-isolate substrate status](docs/ORESVM_TENANT_ISOLATES.md).

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
- Scalar-payload `Option`/`Result` values use distinct checked types and
  generation/schema/tag validation. Real routing capture fixtures run natively;
  class payloads and the full Spin server remain work in progress. See
  [native sums and lifetime boundaries](docs/NATIVE_SCALAR_SUMS.md).
- Ordered scalar/sum locals and immutable record views can follow calls and
  control flow. Mutable scalar slots preserve lexical scope, loan checks and
  deferred cleanup lifetimes. See [ordered locals](docs/NATIVE_ORDERED_LOCALS.md).
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

The `int` to `i64` mapping and other non-entrypoint calling conventions
remain **provisional**, but default signed i64 add/subtract/multiply and unary
negation now lower through LLVM checked-overflow intrinsics and trap on overflow;
they no longer intentionally wrap. Native executable regression cases cover
runtime and constant-source paths at O1/O2, including the signed minimum
boundary. This is still a target trap ABI rather than the final typed Oreslang
`ArithmeticOverflow` error contract, and it does not establish Math/BigInt/
Decimal or floating-point parity. Cross-backend differential tests remain
required before numeric parity is marked complete.
Wasm32 remains governed by its distinct embedder/WASI entrypoint ABI and must
pin a compiler revision carrying the same checked arithmetic semantics.

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
[source parity contract](docs/SOURCE_PARITY.md). The first strict source-level
Java/GraalVM ↔ LLVM execution gate is documented in
[Java/LLVM source-execution qualification](docs/JAVA_LLVM_SOURCE_EXECUTION_GATE.md).
LLVM-only native CTests are important regression evidence, but never count as
completed Java differential execution without a pinned, executed Java run.

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

## AttoLang `.atto` source admission (integer subset)

The LLVM CLI accepts canonical `.atto` source paths as well as legacy `.ores`
paths; the file extension does not change the current parser or semantics.
A native executable regression fixture lives at
`tests/fixtures/numerics-atto-main.atto` and is registered as
`numerics_atto_integer_guest` when Clang is available. It exercises the
existing integer-only subset (forward function call and signed i64 addition)
and must return exit code 42.

This is source-extension admission only. LLVM currently does **not** lower
floating-point values, floating division, contiguous numerical arrays, or
tensor kernels. The numerical-library fixtures using `float`, `List<float>`,
dot products, reductions and matrix multiplication remain unsupported here
and must fail closed rather than silently map to i64. See the existing
`native_float_division_unavailable` contract. Java/LLVM numerical parity is
not claimed by this fixture.

## Repository ownership and migration (2026-11-08)

**Current home:** `ores-truffle-oreslang/oreslang-source.llvm` (legacy Oreslang organization). **Language name:** AttoLang; historical `oreslang-*` CLI/package identities and existing `.ores` files remain compatibility interfaces until independently migrated and tested.

- **What belongs in this repository:** Independent LLVM IR/native AOT compiler backend, target ABI, per-actor memory/runtime integration and native proof.
- **What belongs elsewhere:** Java/CIL backend implementation, cloud hosting/billing, or inferred guest parity from host-only tests. Shared portable AttoLang language semantics and core/std should be authored in canonical `.atto` modules and reconciled with `oreslang-common-source`; indispensable host compiler, scheduler and ABI primitives remain backend-specific here.
- **Organization boundary:** language/compiler/tooling/conformance work belongs to [`atto-lang`](https://github.com/atto-lang); commercial deployment, identity, billing, hosted runtime and multi-tenant cloud operations belong to separately governed [`atto-co`](https://github.com/atto-co). The existence of similarly named `atto-*` repositories is **not** proof of a Git repository transfer or tested feature parity.
- **Proposed first migration:** transfer this *existing repository identity* to `atto-lang/oreslang-source.llvm` retaining its name and history, **only after** individual source/target owner and independent security approvals. No direct transfer to `atto-co` is authorized in the current migration wave. A future product carve-out requires separate approval.

**Maintainer cutover instructions:** coordinate under [migration issue #26](https://github.com/ores-truffle-oreslang/.github/issues/26) and the private [migration runbook draft #25](https://github.com/ores-truffle-oreslang/.github/pull/25); check stable repository numeric ID, exact head SHA/tree, branch protection/rulesets, reviewers, GitHub Apps, secrets, deploys, dependencies, releases, packages, Pages and remote URLs. Transfer using an approved GitHub ownership operation rather than mirroring private source; validate the unchanged ID/tree and rerun exact-head CI at destination. **`steps=[]` or `steps=null` means `NOT_EXECUTED`; host-only tests do not establish native AttoLang guest parity.** Do not combine transfer with forced `.ores`→`.atto` syntax changes, repo renames, CI policy relaxation or AOT/zero-GC claims.
