# AttoLang four local-storage domains: native LLVM implementation boundary

Status: native host substrate + tests; **not** source-level `.atto` compilation or cross-backend parity.

| API | Identity | Lifetime | Implementation |
| --- | --- | --- | --- |
| `ThreadLocal<T>` | physical OS thread | thread exit | `PhysicalThreadLocalsV1` in `LocalContextDomainsV1.hpp` |
| `ActorLocal<T>` | tenant + isolate + actor slot + actor generation | actor FINALIZED | `ActorLocalScopeV1` in `LocalContextDomainsV1.hpp` |
| `TaskLocal<T>` | actor identity + Task ID + Task generation | Task retired | `TaskLocalScopeV1` in `LocalContextDomainsV1.hpp` |
| `RequestLocal<T>` | admitted HTTP request + request generation | request terminal and pins drained | pre-existing `RequestContextLocalV1.hpp` |

These are **typed library facilities**, not new `threadlocal`, `actorlocal`, `tasklocal` or `requestlocal` grammar keywords. The common contract is in [conformance PR #331](https://github.com/ores-truffle-oreslang/oreslang-platform-conformance/pull/331).

## Implemented native rules

- Task and actor cells are independent of worker TLS, bounded to **32 keys each** and keyed by a host-minted unique typed identity.
- A **scheduler-authenticated** execution stamp is *required at call sites*. The current C++ `ExecutionStampV1` is only a data record: **it is not yet a cryptographic or runtime-issued capability**. Never surface its constructor or `open` to guest source.
- Actor reads/writes require an exact tenant/isolate/actor incarnation. The actor's single logical turn or legal `critical` section must be authenticated **outside this container**. The scope mutex is not an alternative to actor-state arbitration.
- Task reads/writes require exact Task incarnation; the logical scope may be handed to a continuation on a different OS carrier without copying TLS. Two Tasks on the same actor are isolated.
- A child Task inherits **nothing by default**. `copy_to_child` transfers a snapshot for one explicitly selected `Copy` key. The compiler must additionally establish `Send`, ownership and correct structured-concurrency lineage before making this guest-visible.
- Scope closure atomically fences new slot access and detaches existing cells; destructors and copy constructors execute outside its mutex. In-flight reads may complete with a previously admitted, pinned copy. No borrowed reference is returned.
- Physical TLS is intentionally **not** an execution authority, and its slots are destroyed at OS-thread exit. A Task/carrier migration does not migrate TLS.
- The existing RequestLocal candidate is **request-specific** and independently cancel-fenced. It must not be reconstructed from an actor singleton or a physical thread variable. The current native RequestLocal API is separate from generic TaskLocal/ActorLocal key typing.

## Blocking integration before guest promotion

1. Bind lifecycle creation and close to actual `ActorCell`, Task continuation and request admission/retirement, with **runtime-issued nonforgeable per-turn context authority**; plain host stamps alone are not authoritative.
2. Bind Task cancellation/revocation to TaskLocal close, actor stop/finalization to ActorLocal close, and verified HTTP ingress/response completion to RequestLocal close, after physical work and pinned descendants drain.
3. Add compiler-owned static key descriptors, full `T` type identities and AOT-safe lookup; reject forged key IDs, arbitrary pointers, borrows spanning suspension, non-`Send` transfers and mutable cross-actor aliases. C++ traits alone cannot validate guest types.
4. Enforce byte quotas and allocation-arena ownership in addition to current **slot-count** limits; a slot can currently hold an unbounded host allocation.
5. Add valid `.atto` fixtures and negative compile tests. Run compiled guest execution across LLVM, Java/GraalVM, Wasm, .NET NativeAOT and Forge. Unsupported features must fail closed.
6. Verify exact-head native/JDK/NativeAOT/Wasm receipts with nonzero executed steps. **Zero-step CI is NOT_EXECUTED**; a host-only passing test cannot promote guest parity.

## Host test

From repository root, compile with GCC/Clang `-std=c++17 -pthread -Isrc/main/cpp` and run `tests/native_context_domains_v1_test.cpp`. CTest also registers `native_local_context_domains_host_only`.

Checks include: physical TLS isolation and destructor timing; same actor/two distinct Tasks; Task values surviving physical-carrier migration; actor and task generation replay denial; explicit copy-to-child; 32-slot quotas; closed-scope access rejection; 200 concurrent close/write races; reentrant copy and erase safety. The existing `native_request_context_local_v1_test.cpp` separately exercises HTTP RequestLocal on a real native actor scheduler.

No guest `.atto` code was compiled by these host tests.
