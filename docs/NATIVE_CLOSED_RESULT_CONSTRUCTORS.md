# Native closed Result constructor lowering — first executable guest slice

## Exact source admitted (synchronous, no owners, no arguments)

```oreslang
pub fnc success(): Result<void, ArrayError> { return Ok(); }
pub fnc limit(): Result<void, ArrayError> { return Err(ArrayError.Limit); }
pub fnc oom(): Result<void, ArrayError> { return Err(ArrayError.NoMemory); }
```

These functions are a *separate native type/AST category* from the existing
i64 expression compiler. They are not scalar status-code functions.
The compiler emits an LLVM named struct
`%ores.result.void.array_error = type { i8, i8 }` and returns the aggregate
by value. The logical invariant is:

| Variant | variant byte | error byte | Semantics |
|---|---|---|---|
| Ok() | 0 | 0 | success, **no** synthetic integer payload |
| Err(ArrayError.Limit) | 1 | 1 | recoverable quota/capacity limit |
| Err(ArrayError.NoMemory) | 1 | 2 | recoverable allocation failure |

The `error byte` is meaningful only for Err; Ok's byte is canonically zero.
This is a closed nominal LLVM *in-memory guest type*. The 3-byte wire format
from the host contract is separate and may not be fed directly into this type.
The raw array native status ABI also remains separate.

## Safety admission

- No user Result or Option class may redefine builtin type identity.
- Constructors may not have parameters, locals, arbitrary expressions, calls,
  side effects, deferred cleanup, exceptions, actor state, await, or match
  until those stages have complete borrow and CFG correctness proofs.
- Result-returning `main` is rejected; no pretending that the 2-byte
  aggregate is a process exit code.
- Duplicate names, collisions with scalar functions/actors/classes, and
  reserved native ABI names reject *before IR emission*.
- Only `Ok()`, `Err(ArrayError.Limit)`, and
  `Err(ArrayError.NoMemory)` construct values.
- LLVM Wasm ABI admission remains disabled until target verification.
- Source parameters, locals, generic `Result<T,E>`, `match`, and
  `Array<int>.try_add` remain explicitly unsupported by LLVM.

## Validation

CTest `native_result_closed_constructor_execution` invokes the **real
source compiler**, checks nominal LLVM IR, links an LLVM-written typed caller
with Clang, and executes it. The LLVM caller checks both discriminant and
payload for all three constructors. Negative fixtures reject invalid
constructors, unsupported type conversions, name collisions, and unhandled
Result storage with *no LLVM IR output*.

**Crucial ABI distinction:** a C `struct { uint8_t variant; uint8_t error; }`
return signature cannot automatically call LLVM aggregate-return functions.
Such a C wrapper must have its own explicit and verified FFI ABI (e.g. an
out-parameter, architecture-specific calling convention, and schema envelope).
The test purposely exercises a caller compiled with the identical LLVM
function type, rather than incorrectly claiming C ABI interoperability.

Before merging: run pinned native Release/Debug compiler CTests and sanitizers,
then JVM/GraalVM differential cases. CI failing before executing steps does
not count as source-level verification.

## Next gates

1. Add typed `Result<void,ArrayError>` parameters, locals and exclusive
   match arms with real exhaustive case checking, without degrading to i64.
2. Implement borrow/ownership, move and every-exit cleanup of the tagged
   value. Reject aliases and cross-actor transport until serialized nominals
   are authenticated.
3. Connect `Array<int>.try_add(int)` to the tagged Result, preserving owner
   state on Limit and OOM, then retire the numeric guest API in draft #333.
