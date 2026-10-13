# LLVM native async I/O and keyword hardening — 2026-10-13

Base: `8607da45`. Local branch: `hardening/async-io-keyword-boundaries-20261013`.

## Implemented

- Reserve `with`, `owns`, `owner`, exact `critical-read` and `critical-write`
  alongside the already reserved `critical`/`using`; unsupported forms cannot
  silently become normal scalar names. Fourteen source rejection cases emit
  no IR; a native prefix/subtraction fixture executes with exit status 8.
- Reconcile the six-file canonical host Read/Write loan slice from existing
  draft `b52cbaef6a267f5ae5184c11c758770fa58b8845` (PR #257). A read cannot
  mutate under broad effects or serial actor privilege. Actual writes need
  write mode and write metadata. Missing/forged modes fail closed.
- Reject every suspension within a synchronous state-loan callback, even if
  continuation metadata claims generation fencing; reject `block` effects
  even under a permissive outer upper bound. State generations are unchanged
  after denied admission. Stale/wrong owner and exception retirement gates
  remain intact.
- Accept only valid async completion outcomes; all invalid 8-bit outcome and
  state-mode values are tested. Preserve single-winner completion/cancel/
  timeout CAS behavior and reject zero/stale/wrong operation tickets.
- Extend unsupported I/O tests to methods, source imports, first-class aliases,
  await and async declarations: 24 cases reject without partial IR.

## Verification and limits

64 selected Release CTests passed on LLVM 20, including the standalone typed
scalar state runtime. Full build is blocked by existing macOS `<threads.h>`
requirements in actor arena/heap tests; WASM ownership linkage requires absent
`wasm-ld`. Neither gate is counted as passed. Sanitizer receipts and the complete
org/backend audit live in `audit-reports/2026-10-13-native-async` at the org root.

This does not implement native fs/net/http I/O or typed async continuation
lowering. Source critical actor-state lowering is still rejected. The legacy
`using` effect signature in main is a prototype, not canonical scoped using;
source `with`/using reconciliation remains a release gate. `owns`/`owner`
semantics are not enabled merely by reservation; real by-value ownership and
`rt` borrow/take checks remain the existing language contract.

Test-only tracing in `SourceActorBootstrap.c` is guarded by
`ORESLANG_SOURCE_ACTOR_TESTING`. `ParkingSignal` parks a dedicated idle reactor
using nonblocking eventfd; that is not an async guest-I/O implementation.
`NativeActorMailbox` and trusted-main actor stop still contain legacy scheduler
waits. They must not be reused as async guest I/O or advertised as proof of
carrier-free source continuations.
