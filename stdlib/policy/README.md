# Source-only library candidates (not registered)

Files under `stdlib/policy/<layer>/` are backend-owned AttoLang library
candidates that the native LLVM frontend does not lower yet (classes with
constructor bodies, `Option`/`Result` returns, `%`/`/` on int, and similar).
They are **not** in `stdlib/LIBRARIES.txt`, so `import module x from
"atto.<layer>/x"` does not reach them, and they never shadow the registered,
executable units in `stdlib/<layer>/`.

Their source-contract tests run against these paths. Native guest scripts that
compile them are target gates: they are expected to fail closed until the
missing lowering lands, at which point a unit moves back to `stdlib/<layer>/`
and gains a `LIBRARIES.txt` line plus a `tests/atto_libraries` guest.
