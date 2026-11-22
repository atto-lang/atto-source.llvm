# Pointer<int> LLVM native AOT M1 — restricted executable slice

**Experimental / opt-in only.** Build `oreslang-llvmc` and pass
`--target=host --experimental-pointer-int-aot <source.atto>`. On success it
emits actual, verified LLVM IR for Clang's **native ahead-of-time** linker.
Without the flag, the ordinary compiler still rejects `Pointer<T>` before IR.

## One accepted program

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

The compiled native process returns exit code **142**, rather than interpreting
the source. The checked compiler IR includes an actual LLVM `alloca` of
`[4 x i64]`, typed `getelementptr`, `store` and `load`. The close operation
emits volatile zeroing stores for each element prior to return.

This profile performs **proven stack promotion**, not general-purpose heap
allocation: `Pointer.alloc<int>(N)` admits only compile-time `1 <= N <= 16`,
one owned lexical region, no aliasing, one synchronous zero-argument
`pub fnc main(): int`, static nonnegative literal indices, no parameters,
no other calls, no generics other than `int`, no nested functions, no actor
operations, no FFI, no imports, no suspension, no exceptions or recovery.

Immutable `val int` bindings may read initialized elements and feed
subsequent stores or return arithmetic. `+`, `-` and `*` values are verified
for signed overflow **before** LLVM emission. A read must follow a successful
write of the same index; all access is inside allocated extent. The owner is
not an integer value and cannot be copied, passed, returned or reassigned.
`close()` must occur once before the final scalar return; no read/write is
possible after it. There is no runtime alias or generation table in this
first slice because no second handle can exist.

**Not yet supported:** a fallible `Result<Pointer<T>, E>` API or `?`
propagation, dynamic indices, dynamic allocations, arbitrary `Pointer<T>`
specialization, pointer fields, slices, escaped pointers, actor handoff,
cross-tenant transport, foreign memory, callbacks, threads, async, shared
loans and arbitrary mutable borrows. Keep all of these fail closed.
The standalone restricted parser is deliberately separate from ordinary
full-language parsing; integrate its typed plan into HIR/MIR before enabling
the feature by default.

## Release proof

The native test command on a real LLVM18 runner is:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm
cmake --build build --target oreslang-llvmc -j2
python3 tests/pointer_int_native_e2e.py --compiler build/oreslang-llvmc --clang /usr/bin/clang-18
python3 tests/guard_native_e2e.py --compiler build/oreslang-llvmc --clang /usr/bin/clang-18
```

The new tests compile and execute real native guests both normally and under
ASan/UBSan and assert every invalid source produces **zero LLVM IR bytes**.
Run on a verified exact-Git-tree mirror if source Actions cannot start; attach
the full source SHA/tree, compiler SHA256 and executable SHA256 to the PR.

**Do not claim complete `Pointer<T>` native AOT or five-backend parity
until the general typed HIR/MIR, generation handles, cleanup and ownership
transfer have their own executed positive guest proofs.**
