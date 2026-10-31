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

The host-only regression authors identity/isolation, scalar-ABI and malformed-AST rejection cases,
including ten sources in foo with a=3, b=4, c=3, and an app namespace selection
from a pinned foo. The test does **not** parse import ns, compile .ores guest
sources, validate a real manifest/lockfile, emit LLVM IR, or execute the guest fixture returning 43.

Required next vertical slice: package-format source inventory + pinned aliases;
parse exact import ns a from 'pkg:foo'; with a typed AST selector; compile all
verified namespace files with safe canonical identity; check exports/visibility
and callable types; lower qualified cross-unit call to LLVM; execute actual
guest fixture returning 43; test misbinding, cycles, symlink escapes, stale ABI.

Shared source-language design: oreslang-common-source PR #189.
Java parser/source-graph counterpart: oreslang-source.java PR #2255.


## Stacked executable overlay (#1224)

Phase 1 remains **call-free by default**. The graph API now exposes an explicit
`PackageNamespaceLinkOptions{true}` mode for the pinned executable linker.
That option admits only structurally scalar, flat Call nodes; it does not make
the host graph responsible for call target resolution.

The pinned executable loader supplies the missing authority:

- a closed host-authenticated source/package inventory and exact package alias pins;
- exact `(PackageInstanceId, NamespaceName)` selection with no global/path fallback;
- lexical namespace-binding shadow rejection;
- same-namespace provider-to-provider **acyclic** call resolution;
- no implicit cross-namespace or cross-package provider calls;
- no private target access;
- exact arity before whole-program type checking;
- deterministic internal symbol assignment from sorted source identities;
- provider implementations forced to LLVM internal linkage after graph authorization;
- final ordinary scalar type/ownership validation after all call names are rewritten.

The current provider-call slice is intentionally positional and int-returning.
Call-free public bool exports remain part of the scalar graph, but bool-returning
cross-file provider calls are deferred until the executable linker has an
explicit pre-validation typed-call signature table rather than relying on the
parser's unresolved default expression type.

The executable regression changes namespace `a` into a three-file DAG:
`aggregate/1 -> double/1 -> add/2`, while a different package instance also
declares namespace `a` with a colliding `add/2`. The intended native result
remains exit 43. Current source-org Actions still fail before runner steps, so
this checked-in executable path is **not execution-qualified** yet.
