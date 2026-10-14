# Native `std/error` core substrate (host-only)

This slice mirrors the **99** nominal error classes defined in Java
`stdlib/error.ores` and `contracts/v1/error-catalog.json` on draft
[Java PR #811](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/811).
It provides `CoreErrorDescriptor`, trusted ancestry traversal, and
`NativeCoreError` with an owned, UTF-8-normalized, 1024-byte-bounded diagnostic.

## Security properties

- The canonical descriptor is a *static runtime table entry*. A structurally
  equal caller-created descriptor is rejected by pointer identity. Name/code
  lookups are for metadata, never by themselves guest type authentication.
- Constructing abstract family classes rejects admission. A concrete
  `Error` root (code 0) is allowed and does not automatically capture a trace.
- The nominal class tree supports reflexive, transitive ancestry. The direct
  RetryAdvisory/ActorApiError interface flags are metadata; they do not grant
  capabilities or trigger retries. Other nominal interfaces are not lowered.
- The native value owns its diagnostic string; it holds no guest object,
  function, mutable heap pointer, or native exception object. This is *not*
  permission to send or serialize the value across an actor boundary.
- `Throw`, `Raise`, `Panic`, cancellation and fatal host effects remain
  control policies, not guest-recoverable errors just because the catalog exists.

## Tests and boundaries

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --target oreslang-native-core-error-test
ctest --test-dir build -R 'native_core_error_' --output-on-failure
```

`native_core_error_catalog_manifest_sync` verifies the generated header rows
against the local v1 JSON manifest. Keep the two repositories' manifests
synchronized in the integration matrix before marking cross-backend parity.

**Not implemented:** Oreslang `import class Error` lowering in LLVM;
allocation of guest Error instances; nominal class payload layout in LLVM IR;
typed catch/throw/raise/panic, cause/suppressed graphs, stacks, protobuf/
MessagePack source APIs, actor transport and supervisor dispatch. No LLVM
source-execution parity is claimed by this host-only addition.
