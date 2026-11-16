# Boolean punctuation operators: native parity target (2026-11-12)

Canonical boolean infix syntax is `&&` for short-circuit AND, `||` for short-circuit OR, and `^^` for eager left-to-right XOR. All operands/results must be `bool`; no truthiness, implicit conversions, or integer bitwise overloading. Precedence from tightest is `&& > ^^ > ||`, each left associative.

The previous proposed lowercase `and`/`or`/`xor` keyword operators are superseded. In expressions, bare `|` or `&` never mean boolean OR/AND (Java only admits `,` and bare `|` as condition-only aliases in `if`/`while` heads). Single `^` remains rejected; `^^` is now **only** boolean XOR. Keep `|>` and unrelated type/borrow grammar separate.

Native lexer already recognizes adjacent `^^` as one token and LLVM's typed `LogicalXor` expression has existing lowering; the parser now admits it with Java's precedence and bool-only typing (Java #3009, `LogicalPunctuationOperatorTest` / `ReservedOperatorTest`). `%` and single `^` keep their reserved diagnostics (`Integer.rem`/`Integer.mod`, `Integer.bitXor`). `tests/native_logical_punctuation_operators.sh` (ctest `native_logical_punctuation_operators`) compiles the byte-identical Java contract fixture `contracts/logical-punctuation-v1/scalar-parity.atto` and runs it at O0/O2.

Still differs from Java: the condition-only aliases (`if a, b then`, `if a | b then`) are not yet accepted by the native parser (spec ch.16 lists them as an open question).

Parity gates: truth tables and precedence, non-boolean negatives, short-circuit AND/OR versus eager XOR (with observable effects), proper source ordering, lexical adjacency and comment/string boundaries, and independent Java/LLVM/Forge/.NET/Wasm/Rust/JS execution. Host-only models and zero-step CI do not count as guest parity.
