# LLVM native Atto core slice, iter, cmp

The compiler-owned import registry maps `atto.core/{slice,iter,cmp}` to
the immutable embedded source in `core/{slice,iter,cmp}.atto`.
The `core/*` shorthand and an explicit `.atto` suffix resolve to the same (never `ores.*`)
unit identity. Included `Generated*Atto.inc` raw-string snapshots have
Git-blob SHA-1 comments verified byte-for-byte by
`tests/check_atto_core_modules.py`.

The **compiler-admitted first tier** is intentionally scalar-only:
checked ranges/index metadata, pure cursor positions with explicit -1
exhaustion, deterministic comparisons, min/max and bound checks.
It allocates no memory, but does **not** create a lifetime-safe
`Slice<T>` or a generic lazy iterator.

Full `Array<int>` and higher-order callback APIs are preserved as
experimental `.atto` source under `core/experimental/`, not
advertised as compiled guest functionality. They must wait for checked
Array/Option/borrow/callback MIR and AOT execution. The native source
graph positive tests in `tests/native_module_imports.py` and
`tests/core_library_sources_test.cpp` only count as proven after exact
PR-head compiler and guest execution, not merely passing source
snapshot checks.
