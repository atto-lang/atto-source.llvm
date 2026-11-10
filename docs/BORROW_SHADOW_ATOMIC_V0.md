# LLVM CFG borrow observer — fail-closed publication V0

Stacked on LLVM native borrow shadow PR #1634. This patch preserves the existing in-pass `OwnershipDataflowV1::analyze` verifier; the normal non-observed path has no shadow heap allocation and no additional CFG traversal.

The opt-in observer stages at most **65,536** accepted operation events. Only after all reachable CFG operations, joins, exit plans, reachability and acyclic checks succeed are these events published. Overflow or later-invalid CFG blocks/joins reject without any published events. Callback failure aborts returning the analysis result. External callback effects are not rollback-able and must not be treated as proof.

The helper still covers the restricted typed ownership CFG. It does not authenticate a Java/Rust source-MIR adapter, Copy/Send witnesses, exceptional/cancellation cleanup, pinned actor continuations or native .atto guest codegen. Verify the exact header and test blobs on Clang Release/NDEBUG and GCC Debug private runners, then run full CTest, sanitizers and compiler guests before release certification.
