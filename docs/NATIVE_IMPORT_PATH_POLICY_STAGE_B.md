# Native import path policy substrate (stage B, partial)

This branch adds a small dependency-free C++17 `oreslang::imports` policy
component and a host-only CTest. It is **not invoked by the native Oreslang
parser, module loader, code generator, or linker**. Thus neither standalone
compilation success nor these tests demonstrate executable multi-file imports.

The component provides:
- Namespace classification: `@/` root, `pkg:` alias, explicit `./` /
  `../`, bare dot-prefixed and ordinary names, absolute paths, and
  `java:` (classified only; native host interop is unsupported).
- Strict alias syntax and a 4,096-byte import path limit.
- Lexically bounded rooted-path joins with component-aware ancestry,
  forward/backslash normalization, and rejection of `..` traversal
  outside a declared root.
- Ordered, de-duplicated search roots: project `source.roots`,
  `source.import_paths`, colon-separated `ORES_PATHS`, then
  legacy `ORESLANG_PATH` with caller-selected OS separator.
- Omitted `source.roots` defaults to `["."]`; explicit `[]` does
  not silently make the project root importable.

Local independent host-only verification (2026-10-16, GNU g++ 14.2.0):
```sh
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
    -Isrc/main/cpp tests/native_import_path_policy_test.cpp \
    -o native_import_path_policy_test
./native_import_path_policy_test

g++ -std=c++17 -O1 -g -fsanitize=address,undefined \
    -fno-omit-frame-pointer -Wall -Wextra -Wpedantic -Werror \
    -Isrc/main/cpp tests/native_import_path_policy_test.cpp \
    -o native_import_path_policy_sanitizer_test
ASAN_OPTIONS=detect_leaks=1 ./native_import_path_policy_sanitizer_test
```

Both independent host-only binary invocations passed with zero reported
sanitizer errors. The committed header and test file blob identities were
compared to the exact locally tested bytes.

## Required integration work

1. Add `.ores.pkg.toml` and zed-pkg TOML parsers, dependency aliases,
   manifest discovery boundaries and physical source identities. This
   header intentionally does not parse TOML or read installed artifacts.
2. Integrate the policy with the native import grammar and cycle-safe
   multi-file symbol/link graph tracked by
   [LLVM #341](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/341).
3. Validate actual `.ores` imports using compiler-generated LLVM IR,
   compiled/linked execution, import order permutations, nested packages,
   negative diagnostics and provenance fixtures. Keep existing unsupported
   import admissions until those tests execute.
4. Run full repository CMake/CTest under a trusted runner with LLVM libraries.
   Direct local host-only g++ tests are not a substitute for full CI.

See [LLVM #465](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/465)
and the five-backend
[conformance umbrella #39](https://github.com/ores-truffle-oreslang/oreslang-platform-conformance/issues/39).

**Security:** lexical path normalization does not confine a symlink or
protect against filesystem mutation races. No Java source, tokens, or private
cross-organization artifacts are involved in this change.
