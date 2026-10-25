# LLVM HasFuture compiler resolution contract (stage 1)

The native LLVM compiler now stores an explicit `NativeAwaitForm` on its
`Function` AST, rather than relying only on a receiver name to decide whether
an `await` uses a native actor continuation.

The deterministic, statically typed resolver `NativeHasFutureContract.hpp`
admits only three known native source shapes:

| Source syntax | Static witness | Native ABI | Result |
| --- | --- | --- | --- |
| `await worker;` | Live `ActorRef<Worker>` implementing `HasFuture` | SourceActorReadyControl | void |
| `await worker.get_ready();` | Same ActorRef readiness method | SourceActorReadyControl | void |
| `const Future<void> ready = worker.get_ready(); await ready;` | Non-Copy token verified to originate from the same live actor | SourceActorReadyControl | void |

Unknown, scalar, class, `Future<int>`, unlive/stale ActorRefs, forged and consumed
Future tokens, implicit `get_done()` or unverified nominal trait names remain
unsupported. A `Future<void>` producer must also match the exact actor
identity being awaited, not merely assert a boolean provenance flag. The validator performs identity and provenance checks before
lowering; it does not trust the spelling of `HasFuture`.

The resolver is a **closed-world compiler contract**, not full arbitrary
user-defined `HasFuture` interface implementation lookup, not new scheduler
runtime code, and not typed `Future<int>` frames. Those require proven
trait/coherence resolution, ownership-aware suspended frames and separate
runtime dispatch authority before any new case may be admitted.

The standalone `native_has_future_contract_test.cpp` exhaustively checks
960 source-shape combinations (9 valid, 951 invalid) in C++17. It can execute
without LLVM dev libraries. Its successful local result is not a substitute
for exact-head end-to-end LLVM source compilation, native AOT, sanitizer,
and Java/GraalVM parity tests, which remain blocked until a runner is admitted.
