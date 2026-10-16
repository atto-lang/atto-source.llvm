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
- ASCII control-byte rejection for classified imports, rooted path suffixes,
  configured filesystem roots, and ambient path lists (including embedded NUL).
  Rooted-path suffixes are also bounded at 4,096 bytes.
- Lexically bounded rooted-path joins with component-aware ancestry,
  forward/backslash normalization, and rejection of `..` traversal
  outside a declared root. The exported `beneath()` ancestry helper
  normalizes **both** arguments before the component comparison, so callers
  cannot bypass it by passing unnormalized `..` components.
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

Additional exact-byte tests (2026-10-16): GCC 14.2.0 and Clang 17 both
compiled and ran the hardened host test with `-Wall -Wextra -Wpedantic -Werror`.
AddressSanitizer and UndefinedBehaviorSanitizer also passed, and isolated
CMake/CTest passed **1/1**. The regression suite checks all ASCII control bytes,
embedded NUL, normalized ancestry bypasses, trailing separators, root
prefix attacks and **729 deterministic enumerated rooted paths**.

Before hardening, the exact prior header accepted an unnormalized `/../` ancestry
escape and embedded NUL bytes in rooted and manifest paths. Those inputs now
reject as regression cases. The hardened exact tested file Git blob SHAs are:

- `ImportPathPolicy.hpp`: `d55b4cb57f3b030d6011b787ee1552d13f5fb93d`
- `native_import_path_policy_test.cpp`: `e2ea2c9c0a8de4c2e51f6ddd68ec52dd9161fc36`

All these tests exercise a **host-only policy helper**, not LLVM `.ores`
import linking, runtime initialization or filesystem confinement.

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
protect against filesystem mutation races. Cross-org GitHub Actions canary
[run #38848228875](https://github.com/ores-dummy-org-oreslang-stack/ores-dummy-org-oreslang-stack-lambdas/actions/runs/38848228875)
was rerun successfully on an authorized private destination with a **synthetic
fixture only**, proving runner execution and policy-fixture behavior, **not**
this C++ code or compiler tests. The Java source-export approval PR #1062 and
destination PR #5 remain unmerged, so **no private Java or LLVM source** was
mirrored into another organization.


## Stage-B manifest discovery and filesystem resolution (2026-10-16)

Added ManifestSourceResolver.hpp and the native_manifest_resolver_host_only CTest.
They operate against real on-disk temporary project manifests, zed manifests and
Oreslang fixtures, but are **not yet called by oreslang-llvmc or its parser**.

Implemented in this standalone host library:

- Nearest .ores.pkg.toml wins over an inner legacy compiler config, including
  a distinct root for a nested package.
- Canonical package discovery boundaries prevent manifest-less installed
  dependencies from inheriting consumer project-root authority.
- Parse a bounded, **restricted TOML subset** of schema_version, [package],
  [project], [source], [entrypoints], [dependencies] and zed [install].dir;
  duplicate/unknown project fields, malformed syntax, oversized manifests
  and unsupported TOML constructs fail closed. This is **NOT a complete
  TOML 1.0 parser**: multiline values, dotted/quoted keys, literal strings
  and several valid TOML forms remain unsupported. Production compatibility
  with Java/Graal tomlj has NOT been demonstrated.
- Source roots and import paths with explicit empty roots=[]; declarative
  package-alias resolution into zed-pkg org/name roots; optional installed
  zed metadata identity validation; importer-relative and ordered bare
  resolution; canonical physical source identity and .ores/.java lookup.
- Package entrypoints cannot lexically escape the project root.
  Installed metadata comparison does not prove lockfile provenance.
- Ambient ORES_PATHS and ORESLANG_PATH values must be passed by future
  compiler integration; this helper does not automatically read process env.

Executed local host-only test evidence from exact committed source blobs:

- ManifestSourceResolver.hpp: 2fe8d39ab77900fc136633a8add7f53981cdbb92
- tests/native_manifest_resolver_test.cpp: b088b24e082ea57f3f35f6b48947c5dac60ad782
- GCC 14.2 and Clang 17 strict C++17 builds: PASS
- GCC ASan + UBSan with leak detection: PASS
- Isolated CMake/CTest of both policy and manifest resolver: 2/2 PASS

Promotion gates: use a complete reviewed TOML implementation or reconcile
the restricted syntax contract explicitly with the language; integrate import
AST/lexer with these APIs; add a cycle-safe multi-file linker and startup;
and execute real Oreslang source -> LLVM IR -> Clang binaries under CI.
Unsupported/foreign imports must remain rejected until supported.
Returning a .java path does not demonstrate native Java source islands.

**This is a tested host-only Stage-B component, not completed native backend
Stage-B/Stage-C import or runtime parity.**

## Follow-up security audit (2026-10-16, additional hardening)

Found and corrected **unverified-by-full-compiler** admission gaps:

- Explicit source files outside a caller-provided discovery boundary must
  fail instead of silently selecting the boundary's root manifest.
- The native source resolver must fail closed on explicit and implicit Java
  source imports, `java:` host imports and foreign `.c` files. A symlink
  with an `.ores` filename pointing to a `.java` target is also rejected.
  Returning a foreign path must not be mistaken for compiler parity.
- Accepted descriptive TOML project/package metadata now requires actual
  quoted basic-string values; malformed known fields are not silently ignored.
- The source identity registry binds canonical `.ores` files to the
  project's physical root, manifest and discovery boundary. A conflicting
  second registration fails before a future linker can use that graph.

Regression cases for all four areas were added to the host-only manifest
resolver test. A **separate isolated C++17 identity-logic reproducer** with
these source-context checks ran successfully under strict GCC, strict Clang,
and GCC AddressSanitizer/UndefinedBehaviorSanitizer (3 executions).
These runs test the extracted identity-check logic, **not the entire updated
GitHub header, manifest resolver, LLVM compiler, or CTest**. Do not re-use
the earlier 2/2 CTest evidence as proof of this newer branch head.

The pre-approved destination's source-free CI job also executed five steps
successfully on the latest retry of
[private workflow run #38931969271](https://github.com/ores-dummy-org-oreslang-stack/ores-dummy-org-oreslang-stack-lambdas/actions/runs/38931969271).
The compiler import-regression job remains **skipped**. The repository's
private-source transfer approval PRs remain unmerged; no private Oreslang
source was copied across organizations.


## Physical-path containment follow-up (2026-10-16)

LLVM PR #466 now additionally rejects physical symlink-target escapes for:
- rooted `@/`, `pkg:` imports, and bare names under configured search roots;
- existing package entrypoints, manifests and installed-package metadata;
- the zed installation directory and installed package directory.

The containment check is applied **after** discovering and canonicalizing the
actual `.ores` file. A symlink to another physical file *inside* the allowed
root remains supported; a symlink to a valid `.ores` file *outside* the root
is rejected instead of silently accepted. Regression cases were added for
escaped file and directory symlinks and for in-root aliases.

Local isolated C++17 reproduction of the physical-root predicate and source
lookup: GCC 14.2 strict warnings PASS; Clang 17 strict warnings PASS; GCC
ASan + UBSan with leak detection PASS (3 executions). This independently
constructed reproducer **does not compile the latest repository headers**.
New exact-head full-repository CTest, LLVM codegen, linked .ores import
execution and sanitizer CI remain **UNVERIFIED**. Latest source-workflow
jobs examined had zero executed steps.

This policy relies on path canonicalization, not descriptor-relative `openat`
or immutable snapshots: hostile concurrent symlink/path replacement is still
a TOCTOU risk. Treat filesystem trees and manifests as trusted/mutation-
restricted until descriptor-based loading is implemented and tested.

**Private source isolation:** Existing Java origin approval PR #1062 and
destination PR #5 remain open. An approval for Java source transfer would not
automatically authorize LLVM source publication. Do not publish LLVM private
source to an alternate organization without a separately reviewed LLVM-source
origin policy and destination allowlist.
