# LLVM backend — controlled dynamism, full AOT, zero guest-GC

**Proposed directive, not implementation evidence.** Read [organization policy PR #38](https://github.com/ores-truffle-oreslang/.github/pull/38), [canonical specification amendment PR #18](https://github.com/ores-truffle-oreslang/attolang-language-spec.md/pull/18) and root [AGENTS.md](../AGENTS.md). Preserve existing Rust-for-tooling/no-Python, `.atto` guest-source-first, security/permission and review contracts.

## Admission requirements

- Declare **code profile:** interpreted/JIT, hybrid, host-only AOT, sealed full *guest* AOT or governed AOT-precompiled module loading. Do not conflate host binary AOT with guest machine-code AOT.
- Declare **memory profile:** managed host GC, no guest tracing GC, or proven process zero-GC. Native/off-heap actor heaps do not automatically imply process zero-GC.
- In sealed full guest AOT, **reject before executable IR**: runtime `eval`/source compilation, dynamic class or prototype/method mutation, unrestricted reflection/unknown proxy targets, runtime emitted guest code, and hidden interpreter/JIT fallback.
- Preserve virtual dispatch, inheritance, typeclasses, generics, hygienic macros, typed closures, task state machines and **compile-time** proxy wrappers through finite reachability, generated descriptors and explicit effects/ownership. Instance proxy forwarding must not escalate private access; static methods need a separate class-level facade.
- Check every `rt take`/`rt copy`/`rt share`/`rt borrow` use, closure environment, actor heap and suspended frame for lifetime, generation, quota, OOM and exactly-once teardown. No unrestricted shared cycles; release memory during long-lived actor operation. No closure/method transport across channels/pub-sub.
- Existing language rules are unchanged: no implicit conversions or operator overloading, prohibited `%`/`^`/`^^` operators, and no implicit nested `Option`/`Future` flattening. Experimental existing behavior is not certified by this policy.

## Backend-specific boundary

Produce real LLVM IR and native machine-code guest receipts at O0/O2; run ASan/UBSan and allocator/actor lifetime checks when executable. Native actor arena reclamation must be tested during long-lived actor execution and at shutdown, not inferred from host reference models.

## Required test evidence

| Positive case | Negative/stress case |
| --- | --- |
| AOT-linked known-class proxy with typed forwarding, static facade, virtual method reachability | Unknown runtime class, private escalation, missing dispatch metadata, `eval` or runtime codegen |
| Owned moved closure/task frame, `await`, actor message and scoped region lifetime | Escaping loan after suspension, strong cycle without approved region, function/closure sent via channel |
| Long-lived actor churn, cancellation, I/O completion and upgrade | Quota/OOM, concurrent termination, stale generation, double drop and dangling handle |
| Generated guest artifact plus true guest execution | Parser-only, host-only model, interpreter fallback, `steps=[]`/`steps=null` |

An evidence receipt must name exact backend source commit/tree, canonical `.atto` fixture blob, compiler revision, generated target artifact digest, nonzero executed steps and the GC/allocator measurement method. Unsupported cases fail closed. This document changes no compiler or runtime behavior by itself.
