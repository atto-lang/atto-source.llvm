# Native two-level ready indexing (stacked draft)

This is a narrow, separately reviewable follow-on to the native carrier locality PR. It replaces per-carrier O(logical actor count) **linear actor-lane scans** with two preallocated scheduler-owned bit indexes for ready CONTROL and DATA. Each index has one bit per eligible actor, one summary bit per nonempty 64-bit word, and is updated only under the scheduler mutex.

* CONTROL selection always precedes DATA selection. Active actors have no ready bits, even when another DATA or CONTROL envelope is admitted, so one actor never runs twice concurrently.
* A worker probes at most 32 indexed DATA candidates for previous-carrier reuse; after eight preferred DATA turns, the next turn uses the global indexed cursor. Any runnable DATA eventually receives a global opportunity.
* Dispatch has no heap allocations, worker-thread creation or guest-owned index state. All existing actor generation checks, FIFO mailboxes, execution claims, retirement, stop/drain, and four-domain ownership remain unchanged.
* Logical actor capacity increases from 1,024 to 65,536, with memory still bounded by the configured per-actor fixed ring capacity. Construction fails gracefully on allocation failure. It does **not** guarantee a production workload of 65,536 actors will fit in memory, nor claim real-time certification or NUMA binding.
* At 65,536 actors there are 1,024 primary words and only 16 summary words. The index query's outer scan is bounded by 16 summary words; each candidate lookup selects a set bit in a 64-bit word. Neither path loops across all actor slots.

**Required merge gates:** exact source C11 GCC and Clang; the stress regression with 4,096 actors and 16,384 ordered turns; sparse last-slot lookup at actor 65,535; CONTROL priority, FIFO, exclusivity, stop/drain and sanitizers. Cross-org CI must verify Git blob hashes of all mirrored private sources before building. The private mirror remains a draft and must not be merged.
