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
