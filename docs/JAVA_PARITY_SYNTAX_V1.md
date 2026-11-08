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
  printed just before the panic vanished. `ores_string_panic_flush` now drains
  it first.
- `expect` requires a **string literal** message and prints only that message;
  Java also appends `: <payload>`. Exit status is `abort()` (134), not Java's
  panic status. Both differences only affect the failure path.
- `import fnc` rejects private or unknown names, a name that collides with a
  local function or another import, and `as` on a multi-name list. Import
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
and runs five `.atto` guests under `tests/fixtures/atto_parity_syntax/` with the
real `compile-aot.sh` pipeline and checks exact stdout and exit status. It also
runs nine fail-closed compiler rejections with exact diagnostics.

Against the shared 49-case corpus, LLVM moves from 9 runtime matches and 0
classified rejections to 13 runtime matches and 6 classified rejections
(`recursion`, `import-cycles`, `order-of-operations`, `named-parameters` now
print `PARITY_OK`). These are exploratory matches, not promoted parity.
