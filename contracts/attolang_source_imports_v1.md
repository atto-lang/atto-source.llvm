# AttoLang native source imports — v1 compatibility and security

This is a compiler admission contract for the LLVM native frontend. The
preferred source extension is **`.atto`**. Existing `.ores` sources remain
supported explicitly during the migration. This does not rename historical
repository or native ABI symbols.

## Resolution rules

- For a relative import `./module` or `../module`, a missing extension
  inherits the importing source's extension. A `main.atto` resolves
  `./module` as `module.atto`, never silently as `module.ores`.
- An importer named `main.ores` continues to resolve extensionless imports
  as `.ores`; explicit `./module.atto` is allowed.
- Project-root `+/` imports use the same deterministic extension rule.
  Either `.atto.pkg.toml` or legacy `.ores.pkg.toml` may mark a package root.
  **Both** markers in the same directory are rejected as ambiguous.
- The *nearest* package marker is authoritative. Crossing a nested package
  marker of either generation, traversing outside the package, or escaping
  its canonical physical root through a symlink is rejected.
- Only `.atto` and explicit legacy `.ores` source suffixes are admitted.
  No fallback based on file existence or search order is permitted.
- The package manifest still accepts only the narrow source-reviewed
  `schema_version = "1"` TOML subset. This is not a general TOML parser.

## Executable verification surfaces

- `tests/project_root_import_test.cpp` exercises the header-only root
  resolver, extension selector, nearest and mixed manifest authority,
  nested-package rejection, symlink traversal, and no implicit fallback.
- `tests/native_module_imports.py` exercises compiled `.atto` relative,
  project-root, explicitly mixed, and same-stem import graphs. It also
  contains negative cases for missing modern imports, dual manifests,
  and unsupported source extensions. Java differential execution is
  deliberately not implied for the new `.atto` cases.
- The exact-head `attolang-native-module-imports` Actions job builds
  `oreslang-llvmc`, executes the registered resolver CTest, compiles,
  links, and runs the import graph matrix, and records binary and tree hashes.

## Evidence boundary — November 3, 2026

A stand-alone native C++ audit extracted the **exact GitHub blob**
`0b87bf968c8b5d977f930dc8c15eba4e482bc0b5` of
`ProjectRootImport.hpp` and verified it using `git hash-object`.
Ten project-root security/precedence scenarios plus direct shared-extension
assertions passed locally under GCC 14.2 and Clang 17 in optimized and
ASan/UBSan configurations. GCC's sanitizer build required demoting
`-Wmaybe-uninitialized` from error to warning for a libstdc++ `<regex>`
diagnostic; Clang's sanitizer build used normal `-Werror`.

These are **header-level and local audit assertions**. The full native
`tests/project_root_import_test.cpp` and the new end-to-end compiler-backed
`.atto` module graph must still run on an authorized LLVM-capable runner
before marking guest parity proved. No private source mirror may be used to
bypass platform or transfer approval. A GitHub Actions job with zero executed
steps proves only runner admission failure, not test failure or success.

## Deferred work

Complete the remaining AttoLang frontend, standard-library, toolchain, and
cross-backend file-extension/package-renaming migrations independently. Do
not equate passing import resolution with compiled sealed `state` or
`critical handle_state` semantics, AOT actor ownership, or zero GC.
