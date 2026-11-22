# Native actor send admission: tagged ABI v1 (host-only foundation)

**Scope:** native pthread actor bootstrap, C11, scalar int/bool messages. This is **not** compiler-generated Atto `Result<Ack, SendError>` or cross-backend guest parity.

`atto_actor_send_result_v1(ref, scalar)` makes one linearizable attempt to enqueue on the **same real runtime** as `atto_actor_send`. It returns an 8-byte, two-u32 structure (`tag`, `reason`):

| Outcome | tag | reason | Meaning |
| --- | ---: | ---: | --- |
| Admitted | 0 | 0 | Enqueued; not processed or durable |
| Rejected, unavailable | 1 | 1 | Invalid slot, stale generation, closed/retired actor |
| Rejected, full | 1 | 2 | Mailbox full; no enqueue; scalar retained for retry |

Compile-time C ABI size/alignment assertions guard layout. The result is a trusted **local host ABI**, not network wire bytes, a message-completion receipt or a capability. An unexpected internal raw status aborts; it cannot be misrepresented as an admission success. The native actor mailbox remains the sole authority.

## Regression and evidence

`tests/native_actor_send_result_v1.c` blocks the first real actor callback, fills its bounded 128-slot queue, checks two rejected full sends, then releases and drains exactly 129 accepted messages. Invalid/forged-generation and post-stop references reject. The fixture uses real C worker callbacks, **not a mock**. The new scoped CI runs GCC, Clang, and Clang ASan/UBSan; `ci/test_native_actors.sh` also runs the focused test.

**Current compiler boundary:** the existing frontend types `ActorRef.send` as `void` and the LLVM native emitter traps on nonzero raw send status. Both are deliberately unchanged to preserve historical runnable source guests. The new C entrypoint is not called by compiled guest code. Host-only evidence cannot promote any `.atto` guest Result contract to PASS.

**Negative source guard:** `tests/actors/fail/205-typed-send-result-pending.atto` uses the canonical `match result over` + `on Ok(ack)` / `on Err(err)` spelling but requires `E_TYPE` and no native output, because the current actor `send()` is `void`. `ci/test_native_actors.sh` discovers this fixture through its fail-case loop. Once true native guest `Result` lowering lands, this fixture must be replaced by an executed positive guest (and separate full/closed/stale negative scenarios), not deleted silently or converted into a false green test.

## Follow-up to achieve actual semantic parity

1. Align `Ack` and `SendError` with `atto-spec/spec/05-actors-and-state.md` and typed actor conformance PR #33; keep admission distinct from processed/durable acknowledgments and `get_done()`.
2. Introduce resolved `Result<Ack,SendError>` (or separately ratified precise type) into the real production typed AST, checks and native owned-sum IR; reject unhandled legacy cases or explicitly version a compatibility profile. Never synthesize `Ok` on failure or silently drop a failed send.
3. Prove single evaluation; reject wrong payloads, forged/cross-tenant capabilities, transfer of an active loan, double take and wrong-generation handles. On a failed `take!` send, roll back ownership exactly once; no double-free or mailbox success.
4. Run byte-identical positive/negative `.atto` programs through LLVM-native and wasm32 compilers on independently pinned exact heads; compare typed result traces, messages, backpressure, DATA/CONTROL separation, cancellation and physical retirement, with native sanitizer gates and checked artifact absence on compiler rejection.

**Overlaps:** Atto LLVM #48 is a host-only density experiment with a distinct ABI, and #50 is a separate cross-process ACK model. Atto conformance #33/#40 own portable actor send/reply targets. Legacy LLVM #1165 is a stacked, host-only buffer-delivery path and is not this scalar ABI. Recheck overlaps and independently review before merge.
