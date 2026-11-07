# M3 host-local read revocation boundary — source review notes

This branch is a **review-only, host-native hardening companion** to LLVM PR #1389. It intentionally does not introduce a second scoped-local store, change the guest ABI, or assert compiler-backed parity.

## Failure mode

`LogicalLocalScopeV1::get_copy` pins a cell and copies it outside the scope mutex. That is necessary because host copy constructors can reenter the store and must never deadlock while holding its mutex. Before this patch, a reentrant copy constructor could close the logical scope yet `get_copy` returned the already-copied value as if the scope were still open. The parent-to-child explicit `copy_to_child` path could therefore consume a snapshot despite parent revocation during the copy.

## Narrow repair

- Copy into `std::optional<T>` **outside** the mutex with the source cell pinned.
- Reacquire the mutex and require `open_` immediately after copying. The post-copy open check is the read's linearization point. If an adversarial host copy closes the scope, return `std::nullopt`.
- Preserve existing atomic quota, replacement, erase, and destructor-outside-lock behavior; do not change physical TLS semantics.
- Update the preexisting reentrant-copy test expectation to fail closed after close-during-copy.
- Add four C++ host regression groups, including a 24-iteration handshaked close/read interleaving and a child-copy cancellation denial.
- Update the existing pinned, exact-head GCC/Clang optimized and ASan/UBSan/TSan workflow. It will compile and execute all three host-local test programs if the repository admits runners.

## Evidence limits and obligations

**These are host C++ constructs, not compiler-minted AttoLang local values.** Custom host aggregates with embedded `shared_ptr` are intentionally used to exercise reentrancy; they are shallow-accounted and not guest-admissible. A subsequent close between the final check and the caller observing the returned value is an ordinary concurrent linearization race, not proof of a live guest authority. The trusted scheduler gate must still revalidate actor/task identity, generation, tenant/isolate membership and STOP/cancellation status. The legacy RequestLocal adapters lacking original-mail provenance remain non-guest.

Local compiler-backed execution has **not** been verified in this change. The source organization's GitHub Actions have recently reported `steps=null` / no executed jobs; classify these as runner-admission failures, not native compiler failures. Exact head and tree receipts, stepful native host tests, the two compiled `.atto` TaskLocal positives, 14 compiler negatives, and independent review remain required before merging or claiming parity.

This PR deliberately targets the current #1389 development branch rather than `main`, to keep the semantic diff small and avoid racing the existing #1389/#1391 integration. Port or merge only after conflict-aware revalidation of the new target head.
