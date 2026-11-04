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
- No source symlinks, multiply-linked files, or imports through nested
  package markers in the project inventory.
- Per-linker identity registry; idempotent same-file imports and duplicate
  object alias rejection, without cross-project or process-global state.
- Opened device/inode/size/mtime/ctime SourceIdentity is bound to the logical
  ID per build; repeated imports must not silently replace source contents.
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
- CTest `native_atto_host_logical_unit_identity` is host-only. The local
  byte-identical Git blobs were compiled with GCC 14.2 and Clang 17 under
  `-Wall -Wextra -Wpedantic -Werror`, and passed. Clang 17 ASan+UBSan also
  passed. These are local host executions, not source-org CI or compiler guest.
- `native_atto_logical_unit_linked_guest_gate` requires real
  `oreslang-llvmc` -> LLVM IR -> Clang O0 and O2 binaries, exit status 42,
  and four fail-closed source-input cases. This CTest is AUTHORED, NOT_EXECUTED.
  The Python script passed syntax compilation only; no native LLVM compiler
  was available locally.
- GitHub Actions source-owned workflow now includes both tests and repaired
  native CTest command quoting. Runner admission and exact-head CI remain
  independent evidence gates; null/empty steps are NOT_EXECUTED.

## Follow-ups
1. Verify exact SHA/tree in a source-authorized stepful runner.
2. Apply this identity consistently to compiler AST modules, diagnostics,
   dependency caches, output mapping and macro import bindings.
3. Add approved external/import-root inventory and explicit macro authority
   policy; reject unsupported compiler macro imports in the meantime.
4. Harmonize .atto extensionless legacy fallback policy across Java,
   LLVM, Wasm, Cranelift and .NET before broad parity promotion.

## Follow-up hardening (2026-11-04)

- The first canonical or legacy package marker is now read (bounded 64 KiB,
  UTF-8) and validated before granting logical-root ownership even when the
  guest imports only `./relative` files. An invalid preferred canonical marker
  fails closed; a valid legacy sibling cannot silently override it.
- The real guest harness adds canonical relative-import cases for invalid,
  oversized, and symlinked `.atto.pkg.toml` files. Rejection requires nonzero
  exit and no emitted IR/object; the positive path still requires LLVM IR,
  Clang O0/O2, and executable exit code 42.
- The native guest CI gate now verifies the presence of all five required
  registered CTest names before execution, to prevent a zero-selected-tests
  false green.
- Local Python harness syntax compilation succeeded; substituting `/bin/true`
  for both compiler and linker causes the guest harness to fail as intended.
  That is **harness-adversarial evidence only**, not guest compilation.
- Source-owned Actions [run 41777567554](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/actions/runs/41777567554)
  reports `steps=null` for both admission and guest jobs. No source-owned
  execution receipt exists for the updated compiler code.
- The private alternate repository `gha-indie-worker/gha-indie-worker-docs`
  demonstrated a stepful **source-free** GCC/Clang probe
  [run 41777357635](https://github.com/gha-indie-worker/gha-indie-worker-docs/actions/runs/41777357635).
  This is not source transfer authorization or proof of remaining minutes.

### Private-source transfer hold

Java security issue #962 and conformance issue #315 require independent
approval of the immutable source head/tree, exact destination numeric repo ID,
protected-source policy, workflow and retention, plus unresolved exposure-gate
closure **before** cross-org transfer. No private LLVM source has been mirrored;
this draft must stay source-owned until approval is independently recorded.
