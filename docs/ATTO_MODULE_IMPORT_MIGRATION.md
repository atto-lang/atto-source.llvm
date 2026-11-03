# Native `.atto` module resolution — fail-closed migration admission

This draft adds explicit `.atto` module imports to the native LLVM compiler
without removing the existing legacy `.ores` import form. New language
sources, fixtures and examples should use `.atto`.

For relative `./lib`, `../lib` and project-root `+/lib` imports:

- For a canonical `.atto` importer, extensionless `lib` resolves
  **only** `lib.atto`, including when a same-basename `lib.ores` exists.
  If `lib.atto` is missing the import fails; there is no silent downgrade.
- Legacy `.ores` importers retain an explicit migration fallback to
  `lib.ores` when a canonical `.atto` target is absent.
- Inspect directory entries rather than only resolving target existence:
  a dangling `.atto` symlink must be selected and rejected by canonical-file
  admission, never silently bypassed in favor of a legacy sibling.
- If neither exists, fail at the ordinary canonical source resolution.
- Explicit `lib.atto` and explicit `lib.ores` are accepted independently.
- All other suffixes fail closed before any foreign-source import.

The stacked canonical-source milestone additionally admits
`.atto.pkg.toml` as the preferred package manifest, with
`.ores.pkg.toml` as a migration fallback at the same directory. It rejects
nested package boundaries defined by **either** manifest spelling and
accepts only the literal `[project] root = "."` subset until the native
full TOML parser is integrated. Arbitrary root overrides continue to fail
closed. Source UTF-8, physical identity, symlink/regular-file admission and
module-runtime semantics remain unchanged.

## Tests and evidence

`tests/project_root_import_test.cpp` adds root-resolution, legacy shadowing,
nested-package and symlink-escape cases. Existing CTest name:
`native_project_root_import_security`.

`tests/native_module_imports.py` retains four actual-compiler `.atto`
module graph cases (extensionless modern, explicit modern, explicit legacy,
and project-root modern), plus the existing negative fail-before-IR cases.
The canonical-over-legacy precedence, explicit case-insensitive `.ATTO`,
and dangling-entry shadow cases are asserted by the native C++ resolver test;
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

## Cross-backend canonicalization follow-up (2026-11-03)

Java canonical `.atto` contract: [Java PR #2486](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/2486).
Native cache package and guest gate: [LRU PR #9](https://github.com/lru-cache-atto/lru-cache.atto/pull/9).
Portable conformance: [platform issue #320](https://github.com/ores-truffle-oreslang/oreslang-platform-conformance/issues/320).

New source files and package fixtures must be `.atto`, not `.ores`.
Explicit legacy paths are retained only for migration. The native LLVM
source/import implementation must be compiled and tested on the exact
PR head with real executed CTest and `oreslang-llvmc` guest receipts before
parity is promoted. Java source-org CI and cross-org mirrors cannot substitute
for that evidence, and existing source transfer holds remain in force.
