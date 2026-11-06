# Java-parity syntax for the native compiler (v1)

The shared cross-compiler corpus (`oreslang-compilers-parity-testing`) writes
programs in the Java reference's canonical syntax. Four gaps stopped every
multi-file check from compiling on LLVM, even though the programs used no
feature beyond the native scalar subset. This change closes them, following
the Java reference's existing rules.

| Form | Java reference | Native compiler before | Now |
| --- | --- | --- | --- |
| `if cond then … fi` | canonical; `;` optional; `do` deprecated | only `if cond; then` / `if cond; do` | `;` optional, `then` or deprecated `do` |
| `stdio.stdout.write(x)` / `println(x)` | exactly one argument, rendered; `write` adds no newline | rejected | lowered onto the existing console node |
| printing `int` / `bool` | rendered as decimal / `true`/`false` | String only | `ores_string_from_i64` / `ores_string_from_bool` |
| `Option/Result.expect("msg")` | value, or panic `msg: <payload>` | rejected | value, or `msg` on stderr then `abort()` |
| `import fnc a, b from "./f";`, `import fnc a as b from …` | named function import | rejected (`import module` only) | binds each name to the target file's `pub fnc` |

## Behaviour details and limits

- **Panic output is no longer lost.** Native console output is asynchronous (a
  writer thread drains a queue, per the no-blocking-I/O rule). A failed
  `unwrap`/`expect` previously aborted before the queue drained, so text
  printed just before the panic vanished. `ores_string_panic` now drains it
  first, then writes the `expect` message to stderr. The drain has one 1 s
  budget, so a stdout nobody reads cannot stall a panic for 5 s per line.
  The console region is thread-local, so only output queued by the panicking
  thread is drained (the host native string slice runs on the main thread).
- **Guest names no longer hijack libc.** A host guest function named like a
  libc symbol the native runtime imports (`write`, `read`, `poll`, `malloc`, ...)
  was emitted under that name and replaced libc for the runtime at link time (a
  guest `write` made the console writer thread call it and trap). Such
  functions, and any `_`-prefixed one, are now emitted as `__ores_guest.<name>`.
  Calls resolve by source name, so behaviour is unchanged. The list lives in
  `HostRuntimeLibcSymbols.hpp`; CTest `native_runtime_libc_symbol_guard`
  recompiles the runtimes and fails if they import a libc symbol not on it.
  Wasm/Cortex-M exports and all other host symbols are unchanged.
- `expect` requires a **string literal** message and prints only that message;
  Java also appends `: <payload>`. Exit status is `abort()` (134), not Java's
  panic status. Both differences only affect the failure path.
- `import fnc` also accepts Java's `import fnc {a, b} from …`. It rejects
  private or unknown names, a name that collides with a local function, another
  function import or a module import (in either order), empty braces, `as` on a
  multi-name list, and parameters or locals that shadow an imported function. Import
  cycles between files work because a file's functions are registered before
  its own imports are followed.
- Arity diagnostics now say which error occurred, keeping the old prefix:
  `unknown function or wrong call arity 'f': unknown function` or
  `…: call arity mismatch`.
- Native stdout must be a pipe, socket or terminal (strict non-blocking I/O).
  Redirecting a compiled program's stdout to a regular file still makes it
  exit 1 without output; that is unchanged and intentional.

## Evidence

`native_atto_parity_syntax` (CTest, `tests/atto_parity_syntax.cmake`) compiles
and runs seven `.atto` guests under `tests/fixtures/atto_parity_syntax/` with the
real `compile-aot.sh` pipeline and checks exact stdout and exit status. One
guest defines its own `write` and must still panic cleanly; panic cases also
check stderr. It runs seventeen fail-closed compiler rejections (including freestanding
`import fnc` of relative, ungranted `alloc/*` and `ores.std` sources) with exact
diagnostics. `native_string_region_lifetime_and_budgets` covers the pre-panic
drain, exact `INT64_MIN`/bool text, and the bounded drain.

Against the shared 49-case corpus, LLVM moves from 9 runtime matches and 0
classified rejections to 13 runtime matches and 6 classified rejections
(`recursion`, `import-cycles`, `order-of-operations`, `named-parameters` now
print `PARITY_OK`). These are exploratory matches, not promoted parity.
