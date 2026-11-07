# Core freestanding LLVM intrinsic admission hardening

The IR audit no longer ignores every `llvm.*` external declaration. Only
known exact and typed LLVM intrinsic families are exempt; unknown names
(including forged `llvm.ores_actor_send`), unsupported debug intrinsics,
missing type suffixes and quoted external names are reported as imports.
Undefined compiled-object symbols are also checked against the separate
exact 27-symbol freestanding SUPPORT_ABI allowlist. New Rust tests exercise
five adversarial groups.

This is defensive source-level admission. No compiler, linker or QEMU
execution may be claimed until GitHub supplies a real runner and immutable
head-bound receipts. New intrinsic families require explicit review before
being admitted.
