# AttoLang logical source-unit identities — LLVM M0

**Status: source-implemented candidate, NOT guest or Java macro parity.**

Reference: Java issue [#2492](https://github.com/ores-truffle-oreslang/oreslang-source.java/issues/2492) and Java draft [#2485](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/2485).

## Admission contract

- A trusted native loader maps a declared project-root file to
  `project/<root-relative-file>.atto` (or explicit legacy `.ores`).
  Host paths, drive letters and filesystem ancestors are not compiler
  source-unit identifiers.
- The nearest regular, non-symlinked `.atto.pkg.toml` or
  `.ores.pkg.toml` marks the ownership root for this M0 identity
  adapter. A manifest-less entry owns its containing directory only.
- The build-local registry rejects sources outside that root, symlink
  components available in the original path, hardlinked files,
  duplicate logical IDs and reused file identities. No ambient or
  externally configured roots are authorized by this slice.
- Source unit IDs are bounded to 4,096 UTF-8 bytes and reject dot,
  traversal, empty path segments, malformed UTF-8, control characters,
  colon and selected Unicode format/bidi/noncharacters. This is
  intentionally fail-closed: the full Java Unicode general-category
  oracle has **not** been independently ported or certified here.
- An import already seen in the graph must retain descriptor identity
  on every known backedge. Existing bounded source reads remain
  responsible for before/after descriptor metadata checks.
- Compiler-owned core library module IDs remain compiler-internal.
  They are not filesystem paths and cannot grant macro ownership.

## Scope and compatibility

This patch threads the **verified identity registry** through the
existing native `ModuleLoader`, but does not change the ABI's
`__ores_module_<index>` symbol naming or make imported macros executable.
The linked compiler remains a restricted `int`/class/function subset.

The mainline import resolver currently rejects extensionless paths when
both `.atto` and `.ores` exist. LLVM draft #1336 and Java draft #2486
propose different canonical migration behavior. Merge integration must
resolve the import policy before claiming full cross-toolchain
equivalence. The identity adapter does not relax these checks.

The adapter uses a canonical filesystem path plus descriptor metadata
for admission. Some entry and `+/` resolution paths canonicalize before
the adapter sees original spellings. Directory-FD/handle-pinned
resolution, complete Unicode-property parity, and fully adversarial
rapid swap-and-restore TOCTOU protection remain unverified. Do not
describe this as a hard security boundary against a hostile writer
with concurrent access to the project tree.

## Tests and evidence levels

- `native_logical_source_unit_identity` is a C++17 standalone CTest
  for ordinary project files, external denial, relocatability,
  malformed IDs, hardlinks, symlinks and replacement.
- `tests/native_module_imports.py` adds real compiler-negative cases
  for project-root escape and out-of-closure hardlinks, while preserving
  its existing positive real LLVM IR → Clang → exit-42 module guests.
- **Authored tests are not executed tests.** A positive result requires
  an exact-head, nonempty-step receipt for GCC/Clang host tests, the
  production native compiler, and executable native guest fixtures.
- This patch does not demonstrate imported
  `define pub macro literal(Const<int>)` compilation. Such source must
  reject without IR until checked typed macro provenance and erasure
  actually exist on LLVM. The Java linked macro positive is a separate
  parity milestone.
- No cross-org private source transfer was performed or authorized.
