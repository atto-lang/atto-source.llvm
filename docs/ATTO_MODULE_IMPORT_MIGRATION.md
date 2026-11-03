# Native `.atto` module resolution — fail-closed migration admission

This draft adds explicit `.atto` module imports to the native LLVM compiler
without removing the existing legacy `.ores` import form. New language
sources, fixtures and examples should use `.atto`.

For relative `./lib`, `../lib` and project-root `+/lib` imports:

- If exactly one of `lib.atto` or legacy `lib.ores` exists, use that file.
- If both names exist, reject the extensionless import as ambiguous and require
  an explicit suffix. Migration must never silently change which source unit
  is selected merely because a canonical `.atto` twin appears.
- Inspect directory entries rather than only resolving target existence:
  dangling symlinks, hardlink aliases and symlink aliases still count as
  conflicting source names and therefore fail closed.
- If neither exists, fail at the ordinary canonical source resolution.
- Explicit `lib.atto` and explicit `lib.ores` are accepted independently.
- All other suffixes fail closed before any foreign-source import.
- Package-less legacy relative imports retain their historical behavior.
  Once the importing source is inside a directory governed by the nearest
  `.ores.pkg.toml`, relative `../` may traverse only within that same
  physical package. Escapes, nested-package crossings and symlink escapes are
  rejected before the imported source is read.

The change retains existing source-content UTF-8 checks, source identity
checks, regular-file-only admission, package nesting detection and root
symlink boundary checks. It does not automatically rename `.ores.pkg.toml`
or alter module runtime semantics.

## Tests and evidence

`tests/project_root_import_test.cpp` adds root-resolution, legacy shadowing,
nested-package and symlink-escape cases. Existing CTest name:
`native_project_root_import_security`.

`tests/native_module_imports.py` retains four actual-compiler `.atto`
module graph cases (extensionless modern, explicit modern, explicit legacy,
and project-root modern), plus the existing negative fail-before-IR cases.
The dual-suffix ambiguity rule, explicit case-insensitive `.ATTO`,
dangling-entry conflict, hardlink-alias and symlink-alias cases are asserted by
the native C++ resolver test;
they are host resolver evidence, not additional AOT guest execution. Existing
CTest name: `native_source_module_graph_execution`.

The pure suffix helper was also compiled/run locally in an isolated C++17
harness using g++ with `-Wall -Wextra -Wpedantic -Werror`. This is **only
component logic evidence**; it is **not** a build of the full repository
nor proof that `oreslang-llvmc` executed the new source fixtures.
Actual compiler/CTest and sanitizer receipts remain mandatory on the
immutable PR head before any merge or cross-backend parity claim.

## Private-source execution boundary

Security incidents [platform-conformance #22](https://github.com/ores-truffle-oreslang/oreslang-platform-conformance/issues/22)
and [#77](https://github.com/ores-truffle-oreslang/oreslang-platform-conformance/issues/77)
currently hold private-to-private source mirroring pending independent
source-visibility and exact source-snapshot approval. A private destination,
available Actions minutes and token-free GitHub-connector mirroring do
**not** independently satisfy the incident release gates. This PR was
developed in the source organization without cross-organization source copy.
