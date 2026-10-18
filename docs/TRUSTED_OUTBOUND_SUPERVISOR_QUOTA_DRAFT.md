# Draft: native trusted outbound quota foundation

Tracking [LLVM #589](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/589), [common #69](https://github.com/ores-truffle-oreslang/oreslang-common-source/issues/69). See `docs/TWO_DOMAIN_EXECUTION_SECURITY.md`.

This draft adds a bounded, integer, host-only token-bucket helper and a multithreaded CTest fixture. There is no guest compiler lowering, supervisor-route hook or message enforcement in this PR. Do not claim this as runtime security or UNTRUSTED execution parity. Actual arbitrary untrusted native guest source must remain rejected in the in-process native OresVM backend.

## Remaining integration gates

1. Identify authenticated supervisor/child/tenant IDs from native execution context. Implement protected/stable lineage metadata across restarts and descendent groups.
2. Hierarchical atomic multi-bucket (count, bytes), outstanding mailbox credits and exactly-once pending reservation settlement. The local helper alone is insufficient; do not mutate standalone scopes separately without rollback/locking.
3. Charge every supervisor-bound data delivery before copying or enqueueing. Seal sender and content; close bypasses in ports, mailboxes, guest bridge and FFI. Validate actual bytes and reject size overflow.
4. Separate bounded, runtime-authored crash/cancel lifecycle lane with no guest forging. Coalesce crash storms.
5. Add sanitizer and real protected-guest-to-supervisor tests; preserve no tracing-GC and no unbounded hot-path allocation. Avoid CONTROL round trips for every send.
6. Document exact test command, commit SHA and actual isolated runtime evidence before ready-for-review.

The bounded policy parameters here are an intentionally conservative prototype limit, **not** validated throughput defaults. Tests prove only helper arithmetic/race behavior.

## Addition: untrusted HTTP and TCP egress budgets

`TrustedUntrustedNetworkQuota.hpp` is a second, *host-only*, fixed-storage quota prototype: 10 HTTP requests in flight, 10 request starts/second (burst 2), and 10 simultaneously connecting/open TCP sockets. Distinct RAII leases have once-only release. New native CTest `untrusted_network_quota_host_only` is registered in CMake. It does not hook `NonblockingTcpLinux`, NativeOresVM guest operations, or real secure egress. **No arbitrary LLVM UNTRUSTED code may be admitted to in-process execution as a result of this prototype.**

Integrate only behind a protected trusted egress bridge with authenticated lineage/tenant policy, DNS and redirect/SSRF permission enforcement, physical socket accounting including HTTP pools, connection-start rate limits, stable quotas across restart, and cancellation/teardown fences. Both pending source and cross-backend CI tests must execute before claiming enforcement.

## Native audit: actual kernel-boundary hooks still pending

`NonblockingTcpLinux.hpp` provides Linux socket and epoll primitives, not a protected UNTRUSTED guest network broker. `NativeOresVM`'s scheduler domain called UNTRUSTED does not isolate arbitrary native machine code. Keep untrusted network source execution denied until a protected process/Wasm bridge exists.

This branch now also contains `TrustedHierarchicalNetworkGate.hpp` (child lineage + supervisor + tenant) with atomic three-scope HTTP/TCP admission and all-or-nothing **unpublished** rollback. Its scopes and leases must remain in protected host memory. Native CTest `trusted_hierarchical_network_gate_host_only` is registered but not yet evidenced as executed. Real integration must bind authenticated routing ID, charge before DNS/connect/HTTP submission, settle on actual socket closure, share the total socket budget with HTTP client pools, enforce DNS/SSRF permissions, and preserve tenant/lineage identity across actor restart/hot code reload. No new tracing GC should be added to LLVM runtime hot paths.

C++'s lease owner must outlive every outstanding lease; destroying a quota with live leases fails closed. Test teardown and cancellation/stop barriers before making this production code.

## Security-audit correction: no delayed token refund

The pre-dispatch reserve/abort interface originally returned rate tokens to the current token bucket. That let a caller hold reservations across refill epochs and cancel later to create a burst beyond the configured maximum. **Now a reservation published to the trusted broker permanently spends its HTTP/TCP start token, whether or not dispatch succeeds.** Cancellation, timeout and close only return the in-flight HTTP or connecting/open TCP credit. A partial child/supervisor/tenant reservation rejected *inside the host admission lock before publication* may reverse its provisional tokens. The helper name `rollbackUnpublished` (native `rollback_unpublished`) is deliberately limited to this use.

Revoking/closing a policy denies *new pre-dispatch commits*, but an already committed host I/O operation still needs explicit cancel-and-drain or physical socket close. Native gates enforce scope/lifetime teardown assertions; they do **not** replace a broker shutdown fence. Java pre-constructs quota admission envelopes before consuming tokens to avoid stranding quotas on allocation failure. None of these changes grants untrusted NETWORK capability or constitutes demonstrated guest-to-broker enforcement. CI executions and sanitizer evidence remain pending.

## 2026-10-17 hardening: atomic scope revocation and direct egress denial

A pre-dispatch **three-scope** lease now checks child, supervisor and tenant revocation status while holding the same trusted gate lock as supervisor Scope.close, before any scope commits. The previous sequential commit could partially mutate child/supervisor leases and throw (Java) or abort the native process (LLVM) when a later scope had already been revoked. Deterministic all-three-role rejection and Java/native concurrent revoke-vs-commit regressions were added.

The real Java NativeIo outbound HTTP and raw TCP call sites also now explicitly reject a direct UNTRUSTED actor turn through TrustedNetworkIngressGuard. The four guarded entry points are HTTP request submission, TCP connect, TCP read and TCP write; ordinary socket close remains available for cleanup. This is defense in depth and **does not create a protected guest->broker network path**. The real actor-turn JUnit case is added but not executed by the source-only carrier; the carrier verifies exact source hash and performs a host-only simulated identity check. Protected isolate and host-network cancellation semantics remain pending.

## October 18, 2026 cross-backend audit and release gates

The trusted-only host quota prototypes now have source-exact loopback validation of
asynchronous Java HTTP and TCP operations. The Java TCP adapter reserves capacity
before opening a channel; completed, cancelled or failed dials must close the OS
channel before releasing the active socket slot. Host TCP handle tracking is
retained if an OS close failure leaves the Socket open. The Java HTTP adapter
isolates caller-future cancellation from host transport cancellation and tracks
its own outstanding exchanges. All HTTP/TCP test helpers are HOST-ONLY; they do
**not** grant network access to UNTRUSTED actors or establish protected isolation.

The shared contract now has **28** adversarial cases and the conformance registry
has **22**. All backend conformance remains **pending**: compiling and executing
host helper tests is not the same as executing an authenticated guest request.

**Blocking requirements before promotion:**

- The runtime must authenticate actual actor identity and durable child/supervisor/
  tenant lineage at the trusted bridge. No guest-provided sender, quota scope,
  timestamp or permission decision can be authoritative.
- Egress authorization must pin the resolved destination/address before DNS,
  redirects, proxy dispatch and connect, and reject private/metadata SSRF
  escalation across all transports and HTTP/2 multiplexing.
- Supervisor revocation and cancellation must close real host sockets, drain
  active I/O and prevent credit reuse before underlying transport termination.
  A terminated Java CompletableFuture alone is not proof of OS socket closure.
- Native LLVM UNTRUSTED execution must retain default-deny until a real
  containment boundary exists; in-process private arenas are not a sandbox.
- Run the actual protected guest and full original runtime tests; source-org
  Actions currently fail before jobs execute. Keep every associated PR draft
  until both CI and the appropriate protected tests pass.

## October 18 — actor-bound broker prototype and outstanding proof

Java now has an experimental static actor-turn ingress backed by trusted-only
registration, exact runtime actor identity, at most 16 unconsumed result
tickets and no guest-facing Java network callbacks. It can currently target
only a supervisor-selected literal loopback endpoint. A private-carrier
simulated-identity harness and real localhost HTTP/TCP operations have passed,
but the authored full `spawnUntrusted` runtime integration test has not run.
The shared threat contract has **31** proposed vectors, while the network
conformance registry has **25**, with every protected backend still pending.

The Java prototype does not enable Oreslang guest NETWORK or provide
production SSRF-safe arbitrary egress. Java/Graal and LLVM protected guest
execution, host transport quiescence, stable actor lineage, supervisor mailbox
quota admission and original source CI remain hard blockers before merge.
