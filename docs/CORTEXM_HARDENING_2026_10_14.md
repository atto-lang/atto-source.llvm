# LLVM Cortex-M admission audit — 2026-10-14

**Verification: PASS.**
[Private exact-source cross-organization CI run](https://github.com/canonical-cloud/canonical-docs/actions/runs/38653462399)
built Oreslang compiler, ran its Cortex-M positive/negative admission suite,
then cross-compiled an actual `.ores` source into ARM ELF, linked Zephyr 4.3.0
firmware and booted `qemu_cortex_m3`. Console evidence:

```text
Cortex-M scalar ABI, ARM ELF, recursion and bounded-admission tests PASS
*** Booting Zephyr OS build v4.3.0 ***
ORESLANG_CORTEXM_AOT_PASS value=42 signed64=PASS
```

The `thumbv7m-none-eabi` compiler target now imposes maximum source size
(128 KiB), function count (64), parameters/call arguments (8), expression
nodes (4096), expression AST depth (32), and static call-chain depth (16).
It rejects direct and mutual recursion and overloaded function names before
generating IR. The compiler retains its limited scalar-only target surface.
The compiler regression suite uses explicit runtime assertions unaffected by
Python optimization. The build produced ARM ELF objects with LLVM 18/Clang 18.

Compiler source audited: `366dd593dd4d15311bd38494784dd3bbeb3ab60f`
(includes the audited main.cpp and corrected test suite).
No formal WCET/stack proof or guest actor support is claimed.

The private mirror uses exact-source blobs from source branches in
`canonical-cloud/canonical-docs` branch `ci/ores-cortexm-aot-20261014`.
Do not treat this as a source-repository Actions green check or proof of
backend parity outside the admitted scalar Cortex-M profile.
