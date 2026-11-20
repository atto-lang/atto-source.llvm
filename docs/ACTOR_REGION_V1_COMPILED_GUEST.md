# ActorRegionV1 restricted compiler-backed guest AOT

This is an **additive**, explicitly selected first application-specific
source-to-native guest slice stacked on ActorRegionV1 PR #1599.

Command:
`oreslang-llvmc --actor-region-v1-guest --target=host main.atto`

Unlike the ordinary stack-local class lowering, this opt-in mode consumes the
production parser's **validated** Program AST and emits LLVM function IR that
calls the *real* ActorRegionV1 C allocator. It admits only:
- one class with 1..16 public, immutable, literal integer fields;
- one `main/0` that constructs exactly one fresh class owner;
- one direct scalar field return from that owner, with **no** early returns,
  branching, methods, mutations, calls, ownership escapes or async operations.

The generated native function performs actor-region open, authenticated turn
enter, bounded allocation, field initialization through C ABI, checked field
read, explicit drop, turn leave, retirement and checked final destruction.
LLVM IR is verified before stdout publication. Source values never become raw
native pointers. Fixed-size turn/ref sidecars live in a trusted native stack
frame, contain original domain/index/generation data and cannot be forged by
the admitted guest source. No interpreter/JIT fallback is allowed.

## Exact guest execution gate

```sh
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build --no-tests=error --output-on-failure \
  -R '^native_actor_region_v1_(compiled_guest_aot|offsets_generation_drop)$'
```

The compiled-guest CTest checks two actual `.atto` sources on `-O0` and
`-O2`, native binary execution, signed exit value, empty output, binary
SHA-256, no tracing GC intrinsics in generated guest IR, and eight negative
source admissions. Each positive links *exact* `ActorArena.c` +
`ActorRegionV1.c`, not mock routines.

## Important proof limits and security obligations

- The first root uses fixed trusted actor identity (1,1) and a single
  synchronous turn, not a real source actor spawn or supervisor. It does
  **not** implement broad class lowering, independent actor stacks or
  scheduler-issued authority.
- Cleanup is guaranteed on **the sole admitted ordinary return**. Early
  return, exception, borrow escapes, cancellation, await, concurrent handlers,
  generics, private source imports and FFI are **explicitly rejected**.
- Runtime misuse remains fail-stop and unsuitable for untrusted actors until
  an isolate-aware controlled fault boundary is implemented.
- A real, complete zero-tracing-GC release needs a transitive native symbol
  and allocation audit, host/scheduler/FFI proof, heap quiescence and restart
  tests. The existing GC IR screen is not a whole-process certification.
- The sealed link-profile PR #1590 **must not** implicitly whitelist this
  runtime until the linked closure is independently certified.
- `steps=null` GitHub CI is NOT_EXECUTED. Source-only commits do not establish
  passing native guest tests. Reviewer approval remains mandatory.

No existing compiler, actor-memory or runtime code has been deleted.
