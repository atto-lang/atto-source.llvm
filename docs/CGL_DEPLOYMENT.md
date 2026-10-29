# Carrier Grade Linux (CGL) / PREEMPT_RT: native pilot

Status: **draft compatibility pilot**, NOT a CGL-certified, PREEMPT_RT-qualified, or hard-real-time actor runtime.

## Scope of this PR

- Target Linux x86-64 and AArch64, starting with a reproducible host-native ELF64 scalar subset.
- Build the actual oreslang-llvmc compiler, compile tests/fixtures/forward-call-main.ores through scripts/compile-aot.sh, inspect resulting ELF architecture, and execute the native process (expected exit 42).
- Collect a non-privileged JSON report of Linux, effective sched_getaffinity CPU count, monotonic clock availability and optional PREEMPT_RT sysfs flag.
- Optionally require a positively verified /sys/kernel/realtime=1 before proceeding. The absence of this flag is UNKNOWN, not proof that the kernel is not realtime.
- Run the same tests on the *actual* carrier-grade target; ordinary Ubuntu Actions verifies only generic Linux compatibility.
- No source export to a different GitHub organization: CI stays in the source repository's trust boundary.

## Build / validation

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
    cmake --build build --target oreslang-llvmc --parallel 2
    python3 -m unittest discover -s tests -p test_cgl_preflight.py -v
    bash scripts/cgl-native-smoke.sh
    python3 scripts/cgl-preflight.py --require-preempt-rt

On a target CGL host, also record the exact Git tree/commit, distribution image, kernel config, libc/interpreter, CPU/SMT topology, cgroup effective cpuset, security profile and boot flags. Verify actual ELF loader + glibc/libstdc++ ABI dependencies with platform tooling. Successful ELF admission is not a dependency/ABI check.

## Separate qualification stages

1. **Compatibility**: host executable builds, transfers, launches and returns correct exit status on the CGL image for x86-64 and AArch64; no privileged CPU policy required.
2. **Soft real-time**: source actor lowering into the native runtime, bounded producer-stall-resistant ingress, lifetime-safe async completion/cancellation, priority/backpressure, actual affinity and policy admission, monitored deadline misses, and p99/p99.9/max measurement with IRQ/network/load interference.
3. **Strict realtime / carrier grade**: admission for dedicated cpuset/SMT/IRQ housekeeping, privileged SCHED_FIFO or SCHED_DEADLINE, pre-faulted locked bounded memory, no blocking/GC/JIT/unbounded work on RT paths, bounded WCET/jitter evidence, independent supervisory stop path, recovery/failover/upgrade testing and >24h soak.

## Explicit non-claims

- Existing LLVM compiler remains an experimental source subset; this pilot does **not** compile Oreslang actor source or a telecom service.
- Logical affinity alone cannot prove CPU exclusivity.
- /sys/kernel/realtime=1 is a necessary evidence item for an explicitly PREEMPT_RT-configured target, never sufficient for real-time qualification.
- No guarantee of hard deadlines, zero GC, network dataplane readiness, or vendor CGL certification.
- CGL vendor specifics and production licensing/support remain deployment concerns.

Blocking implementation references: LLVM issues #15 (privileged OS admission), #20 (producer progress), draft LLVM PR #444 (typed advisory realtime metadata), and Java issue #1061 (soft-RT parity).

## Native C++ host admission gate

A native, read-only C++ host probe is now exposed at src/main/cpp/oreslang/runtime/CglHostAdmission.hpp. It inspects the caller's effective CPU mask, monotonic-clock availability, scheduler policy and PREEMPT_RT sysfs state. The CTest native_cgl_linux_host_admission exercises both live probing and negative policy/flag parsing cases. It **does not** request scheduler privileges, change CPU pinning, or get called by compiled Oreslang actors yet.

## Audit and security gates (October 2026)

The hardened host preflight now requires a regular executable ELF64 little-endian
image, supported OS ABI, bounded and in-file program headers, at least one
PT_LOAD segment, host-matching e_machine and an existing PT_INTERP loader for
dynamic PIE. Symlinked files are rejected. This deliberately excludes
ET_DYN without PT_INTERP (including possible static PIE builds) from the
first pilot; extend only with dedicated source/ABI tests. No ELF-only check
proves that glibc versions, shared libraries or runtime syscalls are compatible.

PREEMPT_RT sysfs values are tri-state: only exact 1/0 count as known values;
missing or malformed values are unknown. A required RT proof fails closed.
That setting is not proof of deadline latency, privileged scheduling
admission, CPU exclusivity, or carrier-grade certification.

The negative test matrix covers foreign ELF architecture, malformed headers,
program-header bounds, missing load segments and loader, wrong permissions,
symlinked executables, 128 reproducible malformed payloads, missing binaries
and unknown PREEMPT_RT state. The LLVM native C++ admission test also avoids
assert-dependent side effects, allowing it to fail even with NDEBUG enabled.

Private source must not be mirrored just because another organization has
available Actions minutes. The protected source Java allowlist
.github/private-ci-approved-repositories.txt is currently empty, and Java
security issue #962 prohibits exporting source until independent security
approval and complete provenance checks are in force. The suggested
sagitta-stack/sagitta-infra destination is awaiting approval in Java draft
PR #1326. No private-source Git blobs, trees, or branches were published to
another repository by this CGL audit.

Local validation is limited to the self-contained preflight/host probe.
Actual compiler execution on the exact PR Git tree, a real MontaVista/CGL
image, PREEMPT_RT deadlines, and >24h telecom fault-soak remain UNVERIFIED
until the named jobs record executed test steps on approved runners.

## Executable-mapping and stack admission (2026-10-29)

The ELF preflight rejects load segments carrying both PF_W and PF_X and any
PT_GNU_STACK segment with PF_X set. This is a narrow fail-closed *file-level*
security admission rule; it is **not** an enforcement of final process mappings,
JIT mappings, dynamic loader behavior, dependency flags, memory permissions,
or CGL real-time guarantees. The negative matrix includes writable/executable
PT_LOAD and executable-stack fixtures as well as admitted RX/normal-stack
controls. Inspect loaded shared libraries and actual `/proc/<pid>/maps` on
the target image separately before production admission.

The linked GitHub CI jobs must execute steps on the PR's exact head and record
source SHA/tree provenance. Zero-step GitHub failures remain runner admission
failures, not compiler test outcomes. Private-source mirroring remains blocked
until the source policy explicitly approves an immutable private destination
repository ID through independent security review.
