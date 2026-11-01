# ODB1 WAL codec scalar parity: signed right shift by eight

This draft implements one compiler primitive required by the canonical pure
Oreslang `src/wal/frame.ores` encoder: `int >> 8`. In the Java/Graal
reference runtime, `int / int` produces float, so byte extraction must
not use integer-looking division or implicit float-to-int narrowing.

The native LLVM frontend recognizes **adjacent** `>>` exclusively while
parsing expressions, by peeking at a copy of the lexer; it does **not**
change the lexer tokens for nested generic `List<List<int>>` closers.
It admits only the exact literal 8 shift count, with standard arithmetic
precedence (+ and * before shift, comparisons after shift). Dynamic or other
counts, unsigned shifts and `<<` remain unsupported and fail closed.
The AST validator independently checks `ShiftRight8` is an i64 operation
with a literal-8 RHS; codegen rechecks the invariant and emits native LLVM
`ashr i64`, never float division or a host reference substitute.

The CTest `native_odb1_scalar_shift8_aot` executes the actual native
`oreslang-llvmc` frontend, Clang LLVM IR linker and OS process using eight
scalar cases (zero, 255, 256, 65535, addition precedence, chained shifts,
multiplication and a parenthesized count), plus five negative source
cases that must produce no LLVM IR. This script was locally POSIX
syntax-checked, **not executed against the actual LLVM compiler**, which
is unavailable in the current environment.

## Scope and remaining blockers

This is **only a signed-i64 scalar prerequisite**. The OresDB 15-case
unchanged `.ores` codec still needs native `List<int>`, `Result`,
`Frame` objects, module import/linking, dynamic loops and byte-level
stdout. No actor-owned WAL storage grants, fsync/dirsync, durable receipts,
generation-fenced commits, replay, or crash/restart have been proven.
WASM may use a verified LLVM frontend bridge, but it needs independently
pinned wasm32 artifacts, guest execution and Wasmtime isolation evidence;
a CTest pass on x86_64 is not Wasm parity.

No backend or DB release should be labeled working solely because these
source changes and tests exist. Require a **nonzero-step exact-head CI**,
real source-to-LLVM-to-Clang execution, independent review and verified
shared DB guest corpus before merging.
