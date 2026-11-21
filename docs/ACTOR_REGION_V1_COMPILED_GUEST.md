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

## Companion hardening review (stacked draft)

The compiler admission path must reject all concealed AST edges even when a
node's top-level `kind` looks like an integer literal or field projection:
conditional equality compatibility operands, named/generic arguments, sum
metadata and nonleaf receiver subtrees are not ignorable. Constructor metadata,
declared effects, actor awaits, hidden C exports and owner aliases also
remain forbidden. This gate is limited to the original class source and
normal return; it must **not** be widened by a future parser feature.

LLVM 18 and later use opaque pointers; do not call the removed
`Type::getInt8PtrTy` there. The native region reference/turn sidecars
must still retain their C ABI alignment and size assertions. Run the same
compiler-backed tests on each supported LLVM major version to prove this,
rather than treating successful C syntax checks as proof.

The native guest regression verifies **executed LLVM call instructions**
for precisely one allocation, read and destruction. Merely finding symbol
names in `declare` instructions is not enough. Positive runs record hashes
of the source, compiler binary, generated IR and linked executable. Valid
negative source cases demand a specific `actor-region-v1 guest AOT rejects`
diagnostic instead of accepting unrelated frontend errors.

**Still not a production untrusted-guest boundary.** Compiler-created
stack slots are controlled by an admitted typed source *only within this
restricted profile*, but a hostile native process can corrupt its own stack.
Do not present native sidecar storage as an OS hardware security boundary.
The fixed synthetic actor identity (1,1) is a testing root, not scheduler
issuance or tenant isolation. Do not run untrusted native guest binaries
inside the AttoLang OS process; use an appropriately isolated worker and
authenticated IPC, quotas and supervisor crash recovery before deployment.
The strict sealed-AOT native runtime linker allowlist remains unchanged.

The current branch is source/test design until a trusted exact-head workflow
records nonzero steps, tested Git-tree identity, both Clang optimization modes
and native process outputs; a source-free checker cannot establish guest
execution or sanitizer cleanliness.
