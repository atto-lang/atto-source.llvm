# Inverted native source-to-binary evidence hardening

The restricted `native_inverted_production_guest.sh` lane is a real
`.atto`→production compiler→verified MIR→LLVM IR→Clang→native process
*candidate*. The following checks are necessary but not sufficient for
compiler parity, memory isolation, or a no-GC artifact:

- The compiler and Clang executable paths are resolved and SHA-256
  fingerprinted before running guests. The source-built no-GC auditor is
  fingerprinted separately. All three tools are rechecked around
  positive/negative cases. Command names alone are not toolchain identity.
- The exact three canonical cross-backend `.atto` Git blobs are checked
  before and after the suite, including the immutable-column swap case.
- The LLVM IR bytes approved by the independent auditor are bound by a
  SHA-256 digest to the bytes remaining after Clang compilation, for O0
  and O2 positive and bounds-signal cases.
- The linked guest executable must have ELF/Mach-O magic and remain
  byte-stable after real process execution. A shell script that simply
  exits 42 is not a native guest.
- The bounds checks must be observed as actual SIGILL/SIGTRAP signals,
  not shell exit codes that imitate crash numbers.

**Residual trust assumptions:** these hashes prove only observed
stability, not compiler provenance, signature/trust chain, binary
machine-code equivalence to source MIR, linked allocator/FFI closure,
OS-level process isolation, or freedom from malicious concurrent
same-UID mutations that restore bytes between checks. The caller must
qualify the build tree, compiler provenance, source revision, Clang
toolchain, reviewer approval, and exact-head GitHub CI run independently.

Never classify jobs with `steps=[]` or `steps=null` as executed native
tests. No source-free smoke result is guest parity; no source code should
be mirrored into a different organization without security approval.
