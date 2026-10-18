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
