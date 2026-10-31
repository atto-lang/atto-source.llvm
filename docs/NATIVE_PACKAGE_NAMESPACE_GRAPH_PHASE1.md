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
