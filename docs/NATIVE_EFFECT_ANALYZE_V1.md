# Native LLVM `--analyze` v1 (restricted compiler evidence)

Use `oreslang-llvmc --analyze source.atto` (legacy `.ores` accepted).
The compiler parses, resolves and attempts verified LLVM IR lowering for the
native guest subset, then discards the IR and reports compiler-owned
exception-effect observations. It does **not** run the guest, write the source,
or generate an artifact. Reuses `realtime_detail::direct_facts` rather than a
new lexical scanner.

- Exit 0: native source admitted and bounded AST call-effect observation
  finished. Warnings about optional exceptional annotations are advisory.
- Exit 1: compiler parsing, checking or native-lowering failure.
- Exit 2: unsupported/inconclusive effect closure (unresolved target, recursion,
  ambiguous overload identity, incomplete runtime/effect evidence, budgets,
  incompatible mode/invalid options).

The emitted `ANALYZE ADVISORY` receipt is not a purity certificate.
`throws`, `raises`, and `panics` are optional may-effects. This v1 only
reports actual LLVM native AST exceptional-effect bits when the native compiler
can admit the source. The broader syntax, types, effect propagation,
actor/supervisor unwinding and linked guest execution remain separately gated.
Java/GraalVM currently has `--check` (semantic admission), **not** this
`--analyze` protocol. Do not claim cross-backend parity from a v1 LLVM PASS.

Advisory source hints for full AttoLang syntax remain in
`atto test --effects [--effects-strict] --file source.atto` (Rust host scanner);
that is not a substitute for compiler-owned semantic analysis.

The CTest `native_effect_analyze_compiler_cli` exercises the actual
`oreslang-llvmc` process on a valid scalar `.atto`, a compiler-admitted
caught-`throw` `.atto`, invalid syntax, duplicate mode, incompatible mode,
and source immutability. No full sanitizer, Linux/x86, Java or runtime evidence
is supplied by the source PR alone.
