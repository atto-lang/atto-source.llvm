# LLVM canonical .atto logical source units — initial M0 bridge

This stacked change follows Java issue oreslang-source.java#2492 and LLVM #1336.
For manifest-backed .atto entries, the native linked module loader now
admits project-owned files under the canonical `project/<relative>.atto`
(or explicitly imported legacy `.ores`) source-unit identity. Host paths
are still used internally for descriptor-pinned reads and diagnostics; they
are never accepted as public logical IDs.

## Enforced now
- A declared project-root-relative identity, bounded to 4096 bytes.
- UTF-8 scalar validation and rejection of control/format spoofing, dot
  components, empty segments, absolute/drive-spelled IDs and root escapes.
- No source symlinks or multiply-linked files in the project inventory.
- Per-linker identity registry; idempotent same-file imports and duplicate
  object alias rejection, without cross-project or process-global state.
- Existing SourceInput pinned-descriptor reads and SourceIdentity checks stay
  authoritative; this source-ID layer does NOT replace them.

## Deliberately not claimed
- No public imported-macro linking, expansion or guest 42 parity is added.
- Rootless and legacy .ores entrypoints retain existing source admission.
- A distinct, verified external-root ordinal contract and compiler-generated
  provenance output are still missing.
- Filesystem metadata checks are not a handle-pinned atomic multi-file graph
  snapshot. Moving or swapping files during compilation remains a separate
  race-hardening gate.
- CTest `native_atto_host_logical_unit_identity` is host-only. Execute it
  with actual C++17 GCC/Clang and sanitizers; separately run real
  `oreslang-llvmc` module graph and Clang-linked native guests. No pass is
  inferred from source review or GitHub jobs with empty/null steps.

## Follow-ups
1. Verify exact SHA/tree in a source-authorized stepful runner.
2. Apply this identity consistently to compiler AST modules, diagnostics,
   dependency caches, output mapping and macro import bindings.
3. Add approved external/import-root inventory and explicit macro authority
   policy; reject unsupported compiler macro imports in the meantime.
4. Harmonize .atto extensionless legacy fallback policy across Java,
   LLVM, Wasm, Cranelift and .NET before broad parity promotion.
