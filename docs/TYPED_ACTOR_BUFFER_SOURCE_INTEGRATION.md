# Typed actor-buffer send integration — draft staged implementation

## Current evidence (2026-10-29)

This branch is stacked on **#1161** (`feat/trusted-actor-buffer-move-20261029`), not on `main`. It adds `TrustedActorBufferDelivery.hpp`, an exception-safe **HOST-ONLY** typed DATA callback layer over the tenant-confined pool, scheduler gate, and bounded mailbox. It does not implement a source `.ores` message or change the existing scalar actor runtime ABI.

`deliver_one(actor, handler)` enters exactly one scheduler-owned actor turn, obtains a moved buffer already published to the destination mailbox, presents a non-copyable and non-movable `Received` view to the host callback, and drops the accepted payload before releasing the turn, including on callback exceptions. A callback cannot return a direct reference to `Received`. The callback and actor identity are trusted host inputs, not guest FFI. The payload is owned by the recipient for the duration of the handler. Sender alias invalidation and queue-full rollback are enforced in the underlying pool/mailbox/gate.

`tests/trusted_actor_buffer_delivery_test.cpp` covers receiver payload integrity at offsets, full-queue rollback retaining sender authority, use-after-transfer rejection, guaranteed cleanup after handler exceptions, rejection of retire during an active turn, and STOP/retire drain of unread accepted messages. Additional coverage now checks 64 concurrent send-versus-STOP races, rejection of sender-side double drop after accepted publication, sender reclamation after a failed send, and prohibition on reading an explicitly released Received view. This test is registered as `native_trusted_actor_buffer_delivery`. The latest exact-Git-blob host suite ran **4/4 CTest**, GCC/Clang ASan+UBSan with leak detection and GCC TSan; these results do **not** constitute full LLVM repository CI or compiled-source evidence.

The source compiler still accepts only `on_message(int n): int` for host-compiled actors and emits `oreslang_source_actor_send(void*, i64)`. The existing C actor runtime `SourceActorBootstrap.c` delivers the 64-bit value to `OresSourceActorMessage`. This path is **not** suitable for native `Array<int>` owner handles. On the parent draft, `Parser.cpp` now rejects direct owned, borrowed, or unmoved `Array<T>` actor DATA arguments with no LLVM IR; the three new `.ores` negative test fixtures are **not yet executed** on a real `oreslang-llvmc`.

## Required source-to-runtime steps before enabling `.ores` DATA

1. **Typed frontend contract:** introduce a distinct `OwnedBuffer<int>` message type (or a formally equivalent, uniquely owned `Array<int>` layout) with `Sendable` validation, handler signature checking, and checked `rt take` of a named live owner. Use snake_case for source-level APIs. Reject borrows, cross-suspension escaped views, non-Sendable elements, nested/foreign raw pointers, and all failed paths before emission. Preserve existing scalar `on_message(int)` unchanged.
2. **Separate message ABI:** introduce an explicitly tagged bounded DATA envelope in the **real** `NativeActorScheduler`, not a pointer cast or buffer-ticket integer through `oreslang_source_actor_send(i64)`. Maintain a receiver-scheduler capability bound to the exact tenant, actor, generation, and type schema. Failure to reserve the destination queue must preserve the original owner; publication and ownership transfer must share one linearization barrier.
3. **Native source runtime glue:** connect compiler-generated handler entrypoints to the verified actor execution turn in `SourceActorBootstrap.c`, and only there enter the native `TrustedActorBufferDelivery` adapter. The old C ABI is C11 and this adapter is C++17: any bridge needs explicit C-compatible opaque handles, versioned functions and safe close/join semantics. Guest code must never construct scheduler tickets, read raw pointers, access the pool, or choose actor incarnations. No untrusted native tenant is permitted in `TrustedInProcess`.
4. **Lifetime and cancellation:** the existing source stop seals DATA and abandons queued DATA. Ensure the new typed envelopes are reclaimed exactly once during stop/cancel and that outstanding reader loans and I/O pins block actor or tenant destruction. A successful send acknowledges admission, not handler completion; `get_done()` requires physical drain.
5. **Positive source fixture:** add a real actor with `on_message(owned Array<int> values)`; an actor producer creates a buffer, writes 42, sends `rt take values`, and receives a result using the future/ack contract, without a raw pointer or implicit buffer copy. Compile `.ores` to LLVM IR, AOT link, execute the actor scheduler, prove zero payload copies on the accepted same-tenant path, and instrument exact buffer-owner generation changes. A compile-only or C++ host test is insufficient.
6. **Parity and isolation:** execute the same source fixtures on Java/GraalVM before moving to WASM/Forge/.NET, with serialized/copying fallback when tenant isolation domains differ. Keep #224 statuses pending until actual guest execution receipts exist.

## Verification gates

The exact-head source GitHub Actions workflow in this branch includes the new native host test alongside the other three tests. Source-org Actions previously failed with **zero executed steps**. Record both job steps and exact SHA before claiming any Actions pass. A separate authorized full LLVM compiler build with LLVM headers/libraries is required to run `tests/source_actor_e2e.py` and the new buffer negative fixtures. No cross-organization private repo mirroring was performed.

**Not merged / not production-ready:** host-only typed delivery is not compiler-enabled zero-copy source messaging, and a same-process tenant logical fence is not OS process isolation.

## Host adapter verification receipt (latest branch revision)

The exact new source Git blob hashes are `TrustedActorBufferDelivery.hpp` **393621f5b4ade4793b12199de28b7a995f92115d** and `trusted_actor_buffer_delivery_test.cpp` **50fd602af4f6937074d5e98f0989f2c8cd5544a0**. Both matched local `git hash-object`. GCC and Clang C++17 `-Wall -Wextra -Wpedantic -Werror -UNDEBUG -fsanitize=address,undefined` with leak detection passed, and GCC ThreadSanitizer passed; the isolated 4-target CMake/CTest project passed 4/4. This is host-only evidence, not source `.ores` execution, exact-head repository build, or Actions certification.

## 2026-10-29 audit: mailbox terminal ownership and runner security

The earlier `TrustedActorBufferMailbox::close()` caught **every** pool-drop exception and discarded the associated queue handle. If a genuinely unexpected ownership mismatch occurred, cleanup would be silently incomplete. Terminal queue cleanup now checks that every queued handle still belongs to the receiver and has no outstanding read loans before publishing close. Borrowed unread DATA prevents queue closure without losing any slot. An unexpected failed drop is fatal unless the pool was already retired (and thus logically revoked the bytes). `close()` returns a success boolean and its destructor fails closed on a nonquiescent queue; the scheduler gate propagates a refused mailbox close rather than proceeding with retirement. **Only queued DATA** is checked by mailbox closure: a previously delivered actor-owned buffer belongs to the actor lifecycle gate, so its loan must not indefinitely block closure of an otherwise empty mailbox.

The final changed exact Git blobs were confirmed by local `git hash-object`:
- `TrustedActorBufferPool.hpp`: `aa15615d6b3eab092889e1645b6d834de26deb2c`
- `TrustedActorBufferMailbox.hpp`: `133ebdf4200f3f33cc37cac1428c346e45446dcc`
- `TrustedActorBufferSchedulerGate.hpp`: `e5d6a548442fe6856163ee064a9421aa8c620b97`
- `tests/trusted_actor_buffer_mailbox_test.cpp`: `08ade147f78d8ec1fee1b08d3e5359037c6e6bb0`

**Actually executed on the final native blobs:** fresh isolated CMake/CTest **4/4 passed**, GCC and Clang C++17 with `-Wall -Wextra -Wpedantic -Werror -UNDEBUG` + ASan/UBSan + leak detection **4/4 each passed**, GCC ThreadSanitizer **4/4 passed**. The first combined test command exhausted its overall command timeout while compiling its third TSan test; the two remaining TSan tests were compiled and executed separately and passed. No failures or races were reported in the completed executions. This is host-only evidence, **not** full LLVM-repository or compiled `.ores` test evidence.

### Private runner mirror decision

Source-branch `.github/private-ci-approved-repositories.txt` contains **zero approved private-source destination repositories** and requires security-owner/maintainer review on a protected branch. The related `.github/private-ci-carrier-contracts.json` pins specific carrier repository/workflow/test-plan blobs for older, unrelated PRs, not this integration head. This is a mandatory source-export gate even when the user authorizes a token-free cross-org mirror. Do **not** transfer the 1,315 source Git blobs to another org or treat a manually prepared temporary branch as authorized until the exact private destination is approved.

Two candidate private carriers were checked read-only: `akrion-sim/akrion-sim-e2e` run `40812378019` and `opto-sync-test/go-java-validation` run `40919664446`. Their inspected failure jobs executed **zero steps**, so neither is verified to have available Actions execution capacity. No private source was exported to either organization. Exact per-blob/tree SHA mirroring can be performed only **after** an approved destination has been provisioned and a source-free canary confirms actual executed runner steps; it must preserve original source tree and blobs, use no cross-org secret/token checkout, and leave both the mirror and source PRs unmerged until full exact-head verification.

Remaining blockers: source-level typed owned-buffer actor send/receive compiler lowering and actual scheduler envelope, real `.ores` execution, verified source CI/approved private runner, and Java/LLVM/other-backend conformance. Keep this PR **DRAFT**.

## Turn affinity and typed admission — later draft hardening

`TrustedActorBufferSchedulerGate::Turn` is now bound to the **physical C++ thread that admitted the turn**. Gate data operations reject a turn presented by a different thread even if the actor ID and incarnation match. An active turn destroyed on another thread terminates rather than silently clearing another carrier's execution authority. Host continuations must mint a new turn when resumed on another carrier; moving a `Turn` to a new thread is not an authorization transfer. This is a *host-side check*, not yet proof that `NativeActorScheduler` cryptographically or structurally authorizes the ticket: the scheduler-to-gate binding is still missing.

The new `TrustedActorBufferDelivery::try_send` returns `Gate::SendStatus`:

| Result | Ownership after call |
| --- | --- |
| `Accepted` | Moves to the recipient; the sender's generation-bound alias becomes stale |
| `MailboxFull` | Remains with sender; no buffer generation change |
| `RecipientStopped` | Remains with sender; no buffer generation change |
| `UnknownRecipient` | Remains with sender; no buffer generation change |

After a **live** receiver is found, the mailbox validates that the sender owns an unborrowed, current-generation buffer **before** reporting `MailboxFull` or `Closed`. An invalid owner or stale/borrowed handle throws, rather than impersonating transient backpressure. All accepted sends commit under the scheduler gate lock, serialized with recipient retirement. These result codes are for trusted C++ callers only; they are **not yet** an Oreslang `Result<T,E>` or a real scheduler-native DATA envelope.

Updated native tests assert wrong-carrier turn rejection; stale/borrowed-vs-full distinction; sender buffer usability after all expected rejection statuses; and exactly one accepted-versus-stopped outcome in repeated concurrent send-vs-STOP races. Existing compiled guest `Array<T>` actor sends must still fail closed.

**Verification status for this later revision:** GitHub source files and test fixtures are committed; the new revision has **not been executed locally as an exact-head build**. Prior 4/4 CTest and sanitizer evidence applies only to the earlier audited blobs, not automatically to these changes. Exact-head GitHub Actions are currently failing before executing job steps. Keep this PR **DRAFT** pending executed CTest/GCC/Clang sanitizers, real compiler-negative fixtures, and source-to-native positive `.ores` execution.

## Exact-commit verifier and SHA checkout hardening

The new `tests/verify_trusted_actor_buffers.sh` verifies that the source tree is clean, prints `git rev-parse HEAD`, checks the eight required C++ source/test blob SHA-1s against `git rev-parse HEAD:<path>`, and then runs all four native host tests under GCC ASan+UBSan, Clang ASan+UBSan and GCC TSan. It terminates on the **first** failure; the final `HOST_ONLY_EXACT_HEAD` receipt is printed only after twelve test invocations succeed. This script is a human/approved-runner reproducibility tool, not an assertion that those twelve tests have run on the latest changes.

The script's downloaded Git blob hash was `89ee689f898b42ec7cba29069eab43456176ed9c`, matching the local `git hash-object`. `bash -n` passed. In isolated synthetic-repository dry runs, a dirty checkout returned exit 2 and a checkout missing the expected tracked source files returned nonzero. **The current branch's C++ files were not compiled in this verification step.**

The `trusted-actor-buffer-exact-head.yml` workflow now pins checkout to `github.event.pull_request.head.sha || github.sha` (both ASan matrix jobs and TSan job) rather than trusting the default `pull_request` synthetic merge ref. Each job independently asserts the checkout HEAD, clean state and all eight tracked source/test blob hashes *before* compilation. The checkout action remains commit-pinned with `persist-credentials: false` and `contents: read`.

The remaining native scheduler still carries scalar `uint64_t` DATA only, and the compiler still rejects `Array<T>` actor sends before LLVM emission. Neither the typed host delivery status path nor these verifier changes prove `.ores` zero-copy execution. The source organization's GitHub Actions jobs still fail with **zero executed steps**; no private-tree cross-organization mirror was attempted because `.github/private-ci-approved-repositories.txt` has no authorized destination entries.

## 2026-10-29 sender authority regression

The trusted scheduler gate now validates transferable sender ownership before returning a missing or stopped recipient status. The test suite additionally covers invalid senders with missing/stopped targets. The updated C++ tests are not yet executed at the latest commit.

The exact verifier script blob `7480d571a1bd85b2e725efd9b95b7bcb026056d9` passed a local Bash syntax check and three negative admission cases: wrong expected HEAD (exit 2), dirty checkout (exit 2), and missing source file (nonzero). These results do not establish native source execution.
