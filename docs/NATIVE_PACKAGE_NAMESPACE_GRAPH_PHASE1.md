# LLVM package-local namespace graph — phase 1

**Status: C++17 host-only checked identity model, not guest compilation or LLVM codegen.**

Native source-linking currently rejects namespaces and type-only identities.
This patch intentionally **does not bypass that gate**. The C++ graph under
frontend/PackageNamespaceGraph.hpp accepts a verified inventory of source
units, package-instance ownership, and pinned dependency aliases, and builds
namespaces keyed by (PackageInstanceId, NamespaceName), not by a global
spelling or folder.

It records only public scalar function names/arity/source provenance;
private functions are not exported, duplicate same-package names/arity reject,
same-spelling namespace names in other packages stay separate. It refuses
unsupported native nominal/class/actor namespace types until typed ABI
lowering exists. Existing define module scopes remain independent.

The host-only regression executes five identity/isolation/rejection cases,
including ten sources in foo with a=3, b=4, c=3, and an app namespace selection
from a pinned foo. The test does **not** parse import ns, compile .ores guest
sources, validate a real manifest/lockfile, emit LLVM IR, or run the 43 fixture.

Required next vertical slice: package-format source inventory + pinned aliases;
parse exact import ns a from 'pkg:foo'; with a typed AST selector; compile all
verified namespace files with safe canonical identity; check exports/visibility
and callable types; lower qualified cross-unit call to LLVM; execute actual
guest fixture returning 43; test misbinding, cycles, symlink escapes, stale ABI.

Shared source-language design: oreslang-common-source PR #189.
Java parser/source-graph counterpart: oreslang-source.java PR #2255.
