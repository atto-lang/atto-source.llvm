# AttoLang native source imports — v1 compatibility and security

This is a compiler admission contract for the LLVM native frontend. The
preferred source extension is **`.atto`**. Existing `.ores` sources remain
supported explicitly during the migration. This does not rename historical
repository or native ABI symbols.

## Resolution rules

- Relative `./module` / `../module` and project-root `+/module` imports
  share the same closed suffix resolver. If exactly one sibling exists,
  it chooses `module.atto` or legacy `module.ores`, irrespective of the
  importing file's extension. **If both exist, resolution fails as ambiguous.**
  This matches the subsequently merged canonical source-admission policy.
- Explicit `./module.atto` and `./module.ores` (or corresponding `+/`
  paths) always disambiguate source selection. Source names ending in other
  extensions are rejected.
- The package-root marker may be `.atto.pkg.toml` or legacy
  `.ores.pkg.toml`. Both markers in the **same** directory are rejected as
  conflicting authority.
- The nearest package marker is authoritative. Relative imports from a
  package may not cross its physical root or a nested package marker of
  either generation. Project-root imports likewise reject traversal,
  nested-package crossings and symlink escapes.
- Only source files with admitted suffixes are opened. Dangling symlinks,
  hardlink twins and same-stem source aliases still count as conflicting
  entries; the compiler must not silently choose one.
- The package manifest still accepts only the narrow source-reviewed
  `schema_version = "1"` TOML subset. This is not a general TOML parser.

## Executable verification surfaces

- `tests/project_root_import_test.cpp` exercises the header-only root
  resolver, ambiguity denial, nearest and mixed manifest authority,
  nested-package rejection, symlink traversal and explicit version selection.
- `tests/native_module_imports.py` exercises compiled `.atto` relative,
  project-root, explicitly mixed, and same-stem import graphs. It also
  contains negative cases for ambiguous source twins, dual manifests,
  nested package crossings and unsupported source extensions. Java differential execution is
  deliberately not implied for the new `.atto` cases.
- The exact-head `attolang-native-module-imports` Actions job builds
  `oreslang-llvmc`, executes the registered resolver CTest, compiles,
  links, and runs the import graph matrix, and records binary and tree hashes.

## Evidence boundary — November 3, 2026

The exact current resolver header Git blob
`f078ba2cd95ce43199c0fe7bbdfec7c4f16f0477` was reconstructed locally,
verified with `git hash-object`, and compiled and executed in an independent
13-case native package/import security harness. GCC optimized and
ASan/UBSan and Clang optimized and ASan/UBSan all passed (4/4 executable
configurations; 52 case executions). GCC's sanitizer build needed a narrow
`-Wno-error=maybe-uninitialized` exception for a libstdc++ `<regex>`
warning; Clang used ordinary `-Werror`.

The full native `tests/project_root_import_test.cpp` and compiler-backed
`tests/native_module_imports.py` graph are still **NOT_EXECUTED** at the
current exact PR head. `llvm-config` and development headers are unavailable
in the local environment. The source-org exact-head workflow failed at
runner admission with `steps=0` in all nine jobs; this is neither a
compiled-guest pass nor a compiler failure. An alternate private carrier
requires independent authorization before a source transfer, and earlier
blocked transfer operations were not bypassed.

This resolves the post-merge discrepancy between the old importer-extension
precedence tests/documentation and the newly merged fail-closed twin rejection
policy. It does not establish sealed actor state, zero-GC or full AOT parity.

## Deferred work

Complete the remaining AttoLang frontend, standard-library, toolchain, and
cross-backend file-extension/package-renaming migrations independently. Do
not equate passing import resolution with compiled sealed `state` or
`critical handle_state` semantics, AOT actor ownership, or zero GC.
