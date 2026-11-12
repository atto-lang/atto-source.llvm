# Boolean punctuation operators: native parity target (2026-11-12)

Canonical boolean infix syntax is `&&` for short-circuit AND, `||` for short-circuit OR, and `^^` for eager left-to-right XOR. All operands/results must be `bool`; no truthiness, implicit conversions, or integer bitwise overloading. Precedence from tightest is `&& > ^^ > ||`, each left associative.

The previous proposed lowercase `and`/`or`/`xor` keyword operators are superseded. Bare `|` or `&` must never mean boolean OR/AND. Single `^` remains rejected; `^^` is now **only** boolean XOR. Keep `|>` and unrelated type/borrow grammar separate.

Native lexer already recognizes adjacent `^^` as one token and LLVM's typed `LogicalXor` expression has existing lowering. This change admits that token through the parser and adds real-guest O0/O2 source test cases. Whether all targets pass remains **NOT EXECUTED** until an admitted exact-head compiler and native run produces receipts.

Parity gates: truth tables and precedence, non-boolean negatives, short-circuit AND/OR versus eager XOR (with observable effects), proper source ordering, lexical adjacency and comment/string boundaries, and independent Java/LLVM/Forge/.NET/Wasm/Rust/JS execution. Host-only models and zero-step CI do not count as guest parity.
