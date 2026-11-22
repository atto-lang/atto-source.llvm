# LLVM native AOT: bounded local Pointer<int> read leases (candidate)

## Purpose and scope

This opt-in feature adds a distinct BorrowedLocal typed authority state to the existing
Pointer<int> native AOT profile. Previously Pointer.alloc<int>(N) modeled an owned region.
rt ref value / ref!value instead creates a compiler-minted non-owning lexical read
lease. rt unref ptr / unref!ptr consumes the handle, NEVER the original local.
The canonical LLVM lexer, shared ownership verifier and real LLVM IR emitter
are used for both variants; a separate source scanner is forbidden.

The deliberately small admitted subset is one synchronous top-level
pub fnc main(): int, an initialized let value: int = 42, one
val Pointer<int> ptr = rt ref value, one val answer = rt deref ptr
(or deref! ptr), exactly one statement-level unref, and return answer.
Contiguous ref!value / deref!ptr / unref!ptr forms also parse.
The verifier rejects any additional control flow, call, pointer copy, alias
write, early exit, conversion, invalid bang spacing or extra declaration.

## Two exact cross-compiler user programs

```atto
pub fnc main(): int {
  let value: int = 42;
  val Pointer<int> ptr = rt ref value;
  val answer = rt deref ptr;
  rt unref ptr;
  return answer;
}
```

```atto
pub fnc main(): int {
  let value: int = 42;
  val Pointer<int> ptr = rt ref value;
  val answer = deref! ptr;
  unref! ptr;
  return answer;
}
```

## Test and safety acceptance

The Rust native compiler driver tests four positive Atto guests (the exact
original pair, all-bang aliases, and local value 13) and 21 rejection cases.
The test driver invokes the real compiler to emit verified LLVM IR, links it
with Clang18, and executes both optimized and ASan/UBSan native guests.
All invalid sources must reject before producing any LLVM IR or binary.
The C++ HIR test also checks five mutations to the borrowed-local authority.

Build the compiler through the canonical source CMake target and use
--target=host --experimental-pointer-int-aot to opt into the profile.
Without the flag, any Pointer<T> source remains unsupported.

**Important:** Authored source and tests are not a runtime result. This
branch must remain draft until exact-head compiler/guest tests actually run
on an allocated runner, review gates pass, and the older Pointer.alloc
regressions remain green. This is NOT general Pointer<T> support, not
actors/TenantVM, not exception/suspension cleanup, and not full 11-backend
parity. The canonical 36-case target remains atto-platform-conformance #30.
