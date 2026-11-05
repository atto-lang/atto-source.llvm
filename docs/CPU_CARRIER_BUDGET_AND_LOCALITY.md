# Native OresVM CPU budget and bounded actor locality

Status: **host-side C scheduler slice, not source-language actor/Task lowering**.

## Opt-in CPU-budgeted VM constructor

`ores_native_vm_open_cpu_budgeted(config)` snapshots available online
processors plus readable Linux cgroup v2/v1 quota and cpuset constraints,
then divides the CPU budget across the VM's four independent scheduler
domains. Explicit nonzero `carrier_workers` allocations are preserved;
unconfigured domains receive at least one worker. The original
`ores_native_vm_open` still defaults to **5 workers per domain** for ABI
and existing test-contract compatibility. Carriers remain lazy-created
only when work is accepted.

A process quota under four cores cannot be strictly enforced by four
separate one-carrier minimum pools. This is a heuristic pool *size*, not
an execution permit, CPU pinning, or a hard limit on runnable time.
There is no guest access to this host API, and the UNTRUSTED pool is not
a sandbox or isolation boundary.

## Same-carrier actor reuse

`NativeActorScheduler.c` gives the pthread which just ran actor A
one bounded opportunity to execute A's next ready DATA message without
going back through the ready queue. CONTROL and WAKE queues are always
checked first. After two consecutive DATA turns on that carrier, the head
of the FIFO DATA ready queue gets priority. The actor remains a
generation-fenced logical identity; no heap ownership is attached to a
pthread. This is a cache-warmth hint, not CPU affinity and not NUMA
topology awareness. Hard carrier-to-CPU pinning is a separate, Linux-only
bootstrap option; see [native scheduler latency](NATIVE_SCHEDULER_LATENCY.md). Other pthreads can run another turn when A's lane is inactive,
but two may never enter A simultaneously.

## Evidence and future parity

New strict-warning tests:
- `native_vm_cgroup_cpu_budgeted_carrier_admission`
- `native_actor_carrier_locality_control_priority`

The older four-domain, generation and supervisor CTests must stay green
on exact source. Remaining work includes an independent scheduler-domain
nonparking CPU permit broker, NUMA/hierarchical work stealing, actual
actor-heap placement, scoped ECS zero-copy permits, LLVM guest execution
parity, cpuset hot reload and hardware performance counters.
