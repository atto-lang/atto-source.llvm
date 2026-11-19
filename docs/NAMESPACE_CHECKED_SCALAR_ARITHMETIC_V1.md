# Native flat-namespace checked scalar arithmetic (stacked draft)

This incremental LLVM backend slice is stacked on #1756 -> #1720. It
accepts exactly one pure, zero-argument `main(): int` in a flat namespace
when its result consists only of integer literals, unary negation,
addition, subtraction or multiplication. Existing checked signed-i64 LLVM
intrinsics remain mandatory; overflow does **not** wrap. The gate caps
nested literal expressions at depth 64.

The compiler must **reject** namespace variables, function calls,
additional functions, actors, declared actors, runtime-only
`external mtd receive`, imports, source effects and any other runtime
shape before IR emission. This change grants no manifest/linking,
`critical-read`/`critical-write`, actor state, mailbox, Task/await or
Java guest actor execution.

CTest `native_use_namespace_scalar_aot_and_actor_codegen_gate`
builds the actual Atto compiler, checks arithmetic IR for signed
overflow intrinsics, links and executes O0 and O2 native guests (exit 42),
and rechecks all unsupported actor source units. These tests are
**authored but not verified executed** at this commit; do not promote the
stacked PR or claim Java/LLVM actor parity without exact-head, stepful
CI and independent review. Private source transfer to any alternate
CI organization requires separate authorization.
