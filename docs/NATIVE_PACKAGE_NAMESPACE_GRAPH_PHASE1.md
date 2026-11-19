# LLVM package-local namespace graph — phase 1

**Status: C++17 host-only checked identity model, not guest compilation or LLVM codegen.**

Native source-linking currently rejects namespaces and type-only identities.
This patch intentionally **does not bypass that gate**. The C++ graph under
frontend/PackageNamespaceGraph.hpp accepts a verified inventory of source
units, package-instance ownership, and pinned dependency aliases, and builds
namespaces keyed by (PackageInstanceId, NamespaceName), not by a global
spelling or folder.

It records only public scalar function names/arity/source provenance, as well as exact checked i64/bool parameter and return types;
private functions are not exported, duplicate same-package names/arity reject,
same-spelling namespace names in other packages stay separate. It refuses
unsupported native nominal/class/actor namespace types until typed ABI
lowering exists. Existing define module scopes remain independent.

The host-only regression authors identity/isolation, scalar-ABI and malformed-AST rejection cases,
including ten sources in foo with a=3, b=4, c=3, and an app namespace selection
from a pinned foo. The new `resolve_package_namespace_scalar_call` lookup uses the checked import edge
plus exact function name/value arity/parameter types to return one physical provider source ID
and its scalar result type. An import may specify a distinct lexical `as` alias;
the import table is keyed by (importing source, **local alias**), not by a
globally shared namespace name. Thus two pinned dependency instances exporting
the same namespace may coexist without merging providers. Duplicate and
reserved local aliases fail closed. In addition, `+/` resolves only
against the already verified importing source's package-instance identity,
never against a global namespace or ambient directory; an absent namespace in
that package must fail, without falling back to a similarly named external
package. The graph does not infer verified ownership from paths. It rejects
missing bindings, wrong arity/type, missing exports and cross-package fallbacks
without disk I/O or guest-side reflection.

Separately, `tests/native_module_imports.py` now includes an **executable three-file module**
guest returning 43: two imported scalar provider functions must each appear as
unique IR definitions and native call sites. The test then removes the source files
before executing the AOT binary. This proves an existing *module* codegen primitive
only when the compiler-backed workflow actually executes.

The native parser now **recognizes** the syntax
`import namespace billing as payments from "+/";` and the analogous quoted
pinned alias selector, capturing a distinct `NamespaceImportSyntax` record.
It validates basic lexical selector bounds and local alias collisions.
**It always rejects nonempty namespace imports before emitting IR.**
The native `ModuleLoader` and host namespace graph independently reject
unverified embedded namespace-import AST records, so future parser changes
cannot silently reuse module-import source authority. Compiler-negative
fixtures assert no LLVM output for unverified imports, traversal selectors,
empty selectors and conflicting aliases. Parser recognition is not package
authentication or executable namespace support.

The separate **module-import** guest fixture does not verify a v2 namespace
manifest/lockfile, connect the host namespace resolver to the compiler, or
lower any actual namespace guest call. No native namespace execution result
is claimed. The existing v1 project-root importer also now refuses symlinked
package manifest markers, including same-directory aliases; it is **not**
a substitute for authenticated v2 package admission.

Required next vertical slice: strict v2 package-format source inventory,
immutable source digests and pinned aliases; bind the parsed namespace AST to
that inventory; compile all verified namespace sources under physical owner
identity; check exports/visibility and callable types; lower qualified
cross-unit scalar calls to LLVM; execute an actual namespace guest fixture
returning 43; test misbinding, init lifecycle, cycles, symlink escapes and
stale ABI.

Shared source-language design: oreslang-common-source PR #189.
Java parser/source-graph counterpart: oreslang-source.java PR #2255.


## Restricted v2 manifest/source digest admission (host only)

`frontend/VerifiedNamespaceManifestV2.hpp` introduces a conservative,
self-contained, C++17 inventory verifier. It accepts **immutable bytes supplied
by a separately checked physical-file loader**, a real SHA-256 callback, and
two independently pinned lowercase SHA-256 commitments (manifest and complete
source inventory). It refuses an incomplete or extra file, changed source
bytes, noncanonical paths, duplicate package membership, malformed or
duplicate manifest keys, excess file/manifest byte budgets, and unsupported
TOML constructs. The source digest uses the same sorted
`relative/path.ores + "\\n" + SHA256(file_bytes) + "\\n"` framing as
the Java manifest admission contract. Its return value is immutable after
verification. The focus test uses OpenSSL SHA-256 rather than a synthetic
source digest.

**The admitted TOML subset is intentionally narrow:** exactly
`schema_version = "2"`, `[project] root = "."`, and one or more
`[packages.NAME] files = [...] ` declarations, including multiline arrays.
It rejects external `namespace_dependencies`, alternate roots and unknown
sections/keys until the **full** v2 parser, package lock/source-pin authority
and physical source identity graph are integrated. A caller may not mint the
expected hashes from arbitrary untrusted input and call that authentication.
This host verifier does **not** open files, establish filesystem authority,
validate runtime `.ores` semantics, resolve package aliases, or generate LLVM
IR. The native executable loader still rejects namespace imports.


## Descriptor-to-verified namespace-owner bridge (host-only)

`NamespacePhysicalSourceV2.hpp` now compares the last source path entry
against the already opened descriptor's inode/metadata **after** reading,
preventing a same-byte regular-file replacement during that capture from
silently passing the checked boundary. A deterministic test opens the old
inode, renames an identical-byte replacement onto its pathname, and proves
the old pathname identity is rejected. POSIX path checking cannot provide an
atomic filesystem freeze; no stage may re-open file paths after the verified
snapshot is published.

`VerifiedNamespaceGraphV2.hpp` now takes one or more authenticated,
immutable `VerifiedNamespaceSourceSnapshotV2` values and a **trusted host
source parser callback**. It generates package and physical source-unit IDs
from the pinned manifest/source SHA-256 commitments plus package key and
manifest-member relative path. The callback receives the **captured bytes**,
not the mutable source path. It binds lexical `import namespace ... as ...
from "+/"` AST edges to their verified owner's package instance and
passes the resulting checked graph to `PackageNamespaceGraph.hpp`.
Duplicate source identities, unlisted sources, unverified module imports,
source-count overflow, duplicate lexical aliases and all external package
alias imports fail closed. Those external dependencies require a separately
verified lock/pin authority before they can be admitted.

The focused `native_verified_namespace_graph_v2_test.cpp` currently uses a
fixture host callback constructing scalar ASTs from fixed text labels. This
**does not** demonstrate the production compiler parser, real Oreslang
source semantics, actual LLVM lowering or a three-file guest returning 43.
The CLI's existing parser/loader gate for executable namespace imports is
unchanged and remains fail-closed. GCC, Clang and sanitized compilation of
the new host graph test must be recorded from executing, exact-head CI or a
complete local private checkout before treating that bridge as qualified.


## Authenticated production-parser host bridge (preparatory, non-executable)

`parse_verified_namespace_source_unresolved` is a separately named,
host-only parser entry point that accepts already pinned/captured source
bytes and preserves namespace declarations and `import namespace` syntax
as **unresolved AST**. It does not run ordinary executable program
validation or LLVM lowering. The existing `parse_unresolved_program` /
`parse_program` entry points still reject namespace runtime imports and
namespace function declarations. Moreover, `resolve_and_validate_program`
now independently rejects such host-only ASTs, preventing use of the generic
executable validator as an accidental admission bypass.

`link_verified_native_namespace_snapshots_v2` composes that real parser
with the already authenticated snapshot-to-owner graph: source text comes
from frozen verified byte buffers rather than a post-admission file read.
The host graph still accepts `+/` only, rejects external package aliases
without a separately verified lockfile/pin graph, and does **not** emit
LLVM IR or run guest code.

Two new CMake/CTest targets are authored:
`native_verified_namespace_parser_v2_host_only` exercises actual
`Parser.cpp` parsing and executable rejection; and
`native_verified_namespace_real_parser_v2_host_only` creates a local v2
manifest/source fixture, admits it using OpenSSL-backed test SHA-256 pins,
parses only the captured bytes, and resolves its qualified scalar target.
The fixture's self-generated hashes establish **test consistency only**:
production digest authority MUST come from independently approved package
pins, not hashes minted from the same untrusted tree.

These new tests must compile and execute in a real, pinned LLVM toolchain
before they count as evidence. Neither permits the CLI to compile namespaces,
and neither is the still-pending three-file LLVM guest exit-43 conformance.


## Authenticated external namespace aliases (host-only lock slice)

`VerifiedNamespaceDependencyLockV2.hpp` now accepts a bounded, deliberately
restricted TOML **lockfile** with independently supplied lowercase SHA-256
commitment, checked against a set of *already authenticated physical package
instance IDs*. This is not a general-purpose TOML reader or proof that a
caller-supplied hash is authoritative. The host must obtain the expected lock
digest from a separately authorized package root and use a real SHA-256
implementation. Supported syntax, one or more entries:

```toml
schema_version = "1"
[[dependency]]
importer = "v2:<64-lowercase-hex-manifest>:<64-lowercase-hex-sources>:app"
alias = "checkout"
target = "v2:<64-lowercase-hex-manifest>:<64-lowercase-hex-sources>:checkout"
```

Each `[[dependency]]` row must contain exactly `importer`, `alias`,
`target`. Unknown keys, unresolved owner instances, self-links, duplicate
alias bindings, unsupported TOML, oversized lockfile and SHA mismatches fail
closed. The result has a private constructor and read-only alias bindings;
it cannot be mutated after verification. The host-only
`link_verified_namespace_snapshots_v2` accepts this optional sealed lock,
**re-checks every importer and target against the actual physical snapshots
of the current invocation**, and passes only those pins to
`link_package_namespaces`. Without an approved lock, external aliases
continue to reject; local `+/` continues to resolve only within the
importer's own physical package.

A standalone OpenSSL-backed lock verifier suite has locally executed 16/16
cases under GCC, Clang, and ASan/UBSan with matching Git blob IDs; the graph
integration fixture is authored but **not yet executed** on an exact-head
native LLVM runner. This does **not** integrate an authoritative host lock
store or enable guest namespace execution. Production compiler/codegen
still rejects imports until typed, verified cross-file lowering exists.
