# Pointer<int> M2a — canonical frontend / typed Program HIR integration

**Current scope:** opt-in host-only `--experimental-pointer-int-aot`,
single synchronous `pub fnc main(): int`, one nonescaping stack-promoted
`Pointer<int>` owner, compile-time 1..16 slots and static indexes.
This is **not** general heap-backed `Pointer<T>`, dynamic dispatch, or
actor/network transferable capabilities.

## What changed from M1

1. **One canonical lexical analyzer.** `Parser.cpp::Lexer` is the sole
   scanner for scalar and opt-in pointer programs. Only the pointer-mode
   tokenization gate admits nominal `Pointer`; normal compilation remains
   fail closed. Quoted strings and templates cannot spoof source tokens.
2. **Shared typed AST/HIR.** `PointerIntHIR.hpp` defines the nominal
   plan, explicit owner, allocation extent, `write_at`, immutable scalar
   binding, `close`, read expressions and return. The core `Program`
   carries the typed HIR as `pointer_int_plan`, never an exposed i64.
3. **Shared ownership checker.** The existing
   `validate_i64_ownership(Program&)` entrypoint dispatches to
   `PointerIntOwnership.hpp`, which replays source-ordered effects,
   independently checks initializer values, initialized-element masks,
   bounds, lifetime, immutable scalar value semantics and exactly-one-close.
   Admission is revoked before each revalidation attempt.
4. **Verified LLVM backend.** `emit_verified_llvm_ir(Program&)` dispatches
   to `PointerIntCodegen.hpp` **only** after the HIR has passed ownership
   checks. It independently reruns the read-only verifier before lowering.
   No parser-to-LLVM shortcut or alternate source lexer remains.
5. **Adversarial compiler-AST proof.** A source-owned C++ test constructs
   and then mutates a previously admitted HIR, verifying the backend's
   independent semantic checks will reject stale validation; tests continue
   to require six compiled, executed native guests with ASan/UBSan and
   zero-IR rejected operations.

## Supported example

```atto
pub fnc main(): int {
  let mut Pointer<int> cells = Pointer.alloc<int>(4);
  cells.write_at(0, 42);
  cells.write_at(1, 100);
  val int first = cells.read_at(0);
  val int second = cells.read_at(1);
  cells.close();
  return first + second;
}
```

```sh
./build/oreslang-llvmc --target=host --experimental-pointer-int-aot example.atto > example.ll
clang-18 -O2 -x ir example.ll -o example
./example
```

Exit status is `142`. This **does not yet** implement the proposed
`Pointer.alloc<int>(4)?` / `Result<int, PointerError>` API. Those
require effect typing and a fallible runtime allocation ABI.

## Security and release boundaries

- The canonical frontend now owns tokenization, the full Program structure
  and the same validation entrypoint, but the M2a pointer body grammar is
  still **deliberately specialized**, not accepted by ordinary general
  function/actor bodies. This is the first integration stage, **not** full
  general-pointer HIR/MIR parity.
- No raw `&` or unary `*` pointer operators, user-supplied capability
  integers, duplicate owners, pointer copies/borrows, FFI, recursion,
  dynamic indexes, import aliases, or pointer suspension are admitted.
- Real runtime generation counters, heap-backed regions, dynamic
  initialization bitmaps, `Result` errors, async drop/cancellation, actor
  transfer, and the Rust/Wasm/Java/.NET/Forge backends are **not implemented
  or certified by this PR**.
- Do not merge or promote this branch until exact-head native compiler,
  compiled-guest, sanitizer, ownership unit and no-IR evidence are green.

Next milestone M2b: typed heap region ABI + dynamic generation-checked
capabilities, then apply the same ownership admission to nonescaping
ordinary functions and effect-aware error paths.
