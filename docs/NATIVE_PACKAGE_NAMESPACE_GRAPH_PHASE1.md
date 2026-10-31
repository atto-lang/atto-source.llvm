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

The current provider-call slice is intentionally positional and scalar
`int`/`bool` only. A closed pre-validation signature table seeds unresolved
provider expression types, validates positional argument types, and leaves
named/generic/richer ABI calls fail-closed before graph admission.

The executable regression changes namespace `a` into a three-file int DAG
`aggregate/1 -> double/1 -> add/2` and namespace `c` into a bool-returning
cross-file chain `response_delta/0 -> handler_ok/1 -> route_ok/1`, while a
different package instance also declares namespace `a` with a colliding
`add/2`. The intended native result remains exit 43. Current source-org
Actions still fail before runner steps, so this checked-in executable path is
**not execution-qualified** yet.


## Schema-v2 manifest and verified lock inventory

The pinned executable loader no longer requires callers to invent source/package
ownership by hand. `PackageManifestNamespaceInventory` adds a filesystem-backed,
fail-closed boundary for the proposed canonical `.ores.pkg.toml` schema v2.

The implemented manifest subset accepts the existing project/source/build fields
needed by native compilation and any number of explicit `[packages.<key>]`
groups. Package members are exact root-relative `.ores` files; there is no
globbing or recursive directory scan. Physical source identity is canonicalized,
hard-link/symlink aliases cannot grant multiple explicit package memberships,
configured `[project].root` must remain physically below the manifest directory,
and nested modern or legacy package markers terminate parent authority.

A single manifest may own multiple declared package groups. The immutable
`PackageInstanceId` therefore binds **both** an exact canonical manifest and an
exact `packages.<key>` group. Merely pinning a manifest does not authorize every
source listed elsewhere in the project.

The language-wide lockfile serialization is still unresolved. LLVM deliberately
does **not** invent a `.lock` filename or TOML/JSON wire format here. Instead,
the compiler consumes a serialization-neutral `VerifiedPackageLockSnapshot`
whose entries contain:

- opaque immutable `PackageInstanceId`;
- exact canonical manifest authority;
- exact `packages.<key>` selection;
- SHA-256 of the manifest bytes;
- exact manifest-relative source membership and SHA-256 for every selected file;
- root-package dependency alias -> exact PackageInstanceId pins.

The adapter reparses and re-hashes the **current manifest files inside the
materialization call** rather than trusting a caller-constructed parsed manifest
object. It compares those exact manifest bytes and every selected source body
against the lock SHA-256 values. A stale manifest, changed source, changed
membership list, unknown alias target, wrong package group, unreferenced
dependency manifest, duplicate physical manifest/group authority, or physical
source claimed by two locked package instances fails before namespace linking.
Manifest authority comparisons use physical filesystem identity, so hard-link
aliases cannot mint a second package authority. `pkg:` namespace resolution
never consults `ORES_PATHS`, `ORESLANG_PATH`, cwd, directory names, project
names, or namespace spelling as fallback authority.

Admission is bounded before source reads: at most 128 lock package instances,
127 root dependency aliases, 127 supplied dependency manifests, and 128 selected
source units total, with the existing 8 MiB aggregate source budget retained.

Nearest modern manifest discovery starts from the **physical entry source**.
A nearer legacy `.oreslangc.cfg.toml` is an independent boundary and blocks
fallback to an ancestor modern manifest; it is not silently skipped. Same-directory
modern authority wins by being checked first, but an invalid modern marker fails
closed rather than falling back.

The executable regression `native_locked_manifest_namespace_exit_43` builds a
real app/foo filesystem fixture. The foo manifest has ten selected files across
namespaces a/b/c, a separate colliding `packages.shadow` group, and an unlisted
ambient source. The verified lock selects only `packages.foo`. Materialization
must produce exactly app + ten foo sources, then compile the same int/bool
provider DAGs to LLVM IR and a native guest with expected exit 43.

This is still not an executed parity claim while source-org Actions jobs have
`steps=null`.
