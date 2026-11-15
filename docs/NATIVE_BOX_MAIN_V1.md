# Experimental native Box<int> main-compiler slice

This is an **opt-in, restricted AOT feature** in the real LLVM C++17 compiler.
It is not general `Box<T>` support and does not activate actor heap
relocation, shared references, `Send`/`Sync`, `Arc` or `Weak`.

## Admitted guest source

```atto
pub fnc main(): int {
    val Box<int> value = Box<int>(42);
    return value.get();
}
```

Compile using `--experimental-owned-int` on the hosted native 64-bit
target. There can be exactly one Box local in synchronous zero-argument
`main`. Construction takes a signed literal int and `get()` reads a copy
of the inner scalar. Additional plain scalar locals and guarded returns
may consume the copied int, but the Box owner cannot itself be copied,
moved, borrowed across a call, returned or sent to an actor.

The compiler mints `native_box_i64` and `native_box_read` metadata.
The independent native scalar type verifier treats a Box owner as
a **noncopyable category**, not a plain `int`, and checks that only
the authenticated `get()` projection produces an int. The ownership
checker independently denies raw owner reads, unsupported `rt take`
and escaping loans. The emitter independently checks that every Box
read has the correct compiler-owned flag.

## Runtime ownership and cleanup

The existing `NativeOwnedI64Region.c` runtime creates an opaque,
monotonically unique region handle, with checked allocation, read,
drop and region-close operations. It uses native allocation without
a tracing garbage collector. Generation/identity and resource lifetime
are enforced by the runtime; guest source does not receive its pointer.

The existing LLVM owned-region lowering emits a live flag and
conditional drop/region-close on every supported return path.
The tests execute native binaries and check both an early return
and the fallthrough path for successful deterministic teardown.
Invalid runtime handle operations fail closed.

This standalone main-owned region is **not** the 32-bit
actor-owned private arena planned for production AttoLang actors.
It is not safe to transfer its handles between actor heaps or use
it from concurrent receiver methods.

## Explicitly rejected

- `Box<bool>`, general `Box<T>` and multiple live Box locals
- raw owner copying, taking a Box as an int, or taking `box.get()`
  as an ownership move
- `rt borrow box.get()` and unsupported write/read methods
- unsupported Wasm32, Cortex-M and isolated VM targets
- user-defined type/actor/alias definitions impersonating
  `Atomic`, `Box`, `Arc`, or `Weak`
- granting `Send` or `Sync` to unimplemented Atomic/Box handles
  through a permissive nominal marker callback

## Verification and next milestones

Run `tests/atto_box_main_v1.sh <compiler>`; set
`ATTO_BOX_SANITIZE=1` to compile and execute native guest binaries
with AddressSanitizer and UndefinedBehaviorSanitizer.
The source CI workflow uses LLVM 18.

Independent exact-source private carrier branch:
`honeypot-r-us-test/hnpt-lambdas@audit/atto-box-main-integration-20261114`.
Its manifest verifies every selected source blob SHA before compilation;
no token or secret is used to check out the private source organization.
This is focused C++17 main-compiler testing, not the entire source tree.

**Merge blockers:** full affine Box moves and borrow-lifetime inference,
arbitrary `T` destruction, function exits and unwind/cancellation cleanup,
scheduler-authenticated actor-domain allocation, parallel receiver
isolation, target/backend parity, and full native CI. Keep this feature
experimental and this PR draft until those are proven.
