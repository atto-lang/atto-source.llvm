# Sealed native AOT: native runtime link-closure boundary

The opt-in LLVM `sealed-scalar-v1` and `sealed-class-fields-v1`
profiles now reject linker-discovered native runtime dependencies **before**
Clang or output publication. The existing string, record, array, actor and
Task implementations are **not deleted or disabled**: ordinary AOT mode
continues to link them.

## Why the existing IR auditor is not enough

`audit_no_gc_ir.rs` screens *guest LLVM IR* for known tracing GC
intrinsics and collector symbols. It does not prove the transitive
allocation, destruction, dynamic library, or FFI behavior of the linked
runtime and process. Each native library must be admitted separately
with recorded source closure and cancellation/retirement proof.

## Still required for full guest AOT + zero tracing GC

- Closed-world symbol graph for imports, specialization, virtual method
  dispatch, reflection, module initialization, exception cleanup, await,
  cancellation, hot reload, actor supervisor restart and dynamic plug-ins.
- Compilation of *actual* `.atto` application functions and callbacks,
  rather than a Java interpreter Native Image or Wasmtime precompile of
  an externally supplied Wasm module.
- Actor/tenant-owned arenas with compressed 32-bit offset-based guest
  references (plus generation/region identity), bounds and borrow gates,
  teardown on **every** exit path, and no tracing collection.
- C/C++/JNI/PInvoke/FFI allocation/link audits including the transitively
  reachable native and managed runtime libraries. Epsilon GC has zero
  collections but is **not** deterministic reclamation.
- Real native binary inspection, long-lived repeat-allocation/restart soak,
  independently observed GC events, exact-head compiler-backed positive
  and negative mutation tests, and reviewer approval.

`scripts/test-sealed-aot-link-closure.sh` uses mock compiler, auditor and
Clang binaries to check ten **source-free** negative cases and an ordinary
mode compatibility control. It establishes only wrapper behavior; it
does **not** establish actual guest compilation, linked zero-GC execution,
or readiness to merge.
