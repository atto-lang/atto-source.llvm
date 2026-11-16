# M2b candidate: compact 32-bit actor heaps for the native LLVM host

**Status: draft implementation and locally tested native allocator; compiled .atto guest
and true i386 tenant-VM integration NOT YET EXECUTED.** This PR is stacked on
the typed Pointer<int> M2a compiler [#15](https://github.com/atto-lang/atto-source.llvm/pull/15).
Do not claim five-backend parity or whole-process GC elimination.

## Implemented native runtime path

- Replaces the existing tenant-VM actor heap's **guest-writeable intrusive free-list**
  with a trusted sidecar of compact 4 KiB slab descriptors outside the VM cage.
- Each descriptor stores 256 live bits plus class/count information; `sizeof(Page) <= 64`.
  Sidecar allocation is performed once before actor admission. No `new`, malloc or
  vector reallocation occurs in the actor allocation/free hot path.
- A request selects 16-, 32-, 64-, 128-, 256-, 512-, 1024-, 2048- or 4096-byte slabs,
  or an explicitly tracked multi-page large run. Partial first/last pages and
  unaligned heap bases are checked, including unsigned 32-bit range limits.
- Allocation fails with 0, without modifying live allocations, on invalid alignment,
  exhaustion, or too-large requests. Release checks the live allocation bitmap,
  class, exact block boundary and actor region. Free scrub zeros the released block.
  STOP and supervised actor restart retain the existing cage zeroization and clear
  the trusted sidecar. Actor-sibling heaps have separate metadata.
- Direct four-byte Offset32 references are unchanged, including on 64-bit hosts.
  Host cage pointers remain native width; the existing tenant VM still uses its
  64-bit, multi-GiB virtual guard cage. The new standalone slab allocator is
  independently compilable for i386 but **the full tenant VM is not i386-capable
  merely because the allocator compiles with -m32**.

## Important safety and ABI boundaries

This is the **existing ABI v4 raw Offset32**, not a new secure generational Ref32.
Although the allocator blocks double-frees of already-free slots and ignores
guest-poisoned free-list bytes, a stale offset can refer to a newly allocated
object at the same address. It is not cryptographically authenticated or
actor/tenant-global. The compiler must prevent stale references and raw
cross-actor transport. A future versioned typed generational handle ABI is
required for dynamic stale-reference rejection. Same-class request lengths
can be indistinguishable at release; this prototype tracks the size class,
not each source-level requested byte count.

The host allocator is not a security boundary against deliberately malicious
native guest machine code. A genuine tenant sandbox requires an enforced
separate runtime/process boundary or verified executable restrictions.

The current first-fit page scan is **bounded by the configured number of
pages but not constant-time**. It is not yet certified for hard real-time
latency. Per-actor slab pages can fragment and leave memory unavailable
despite adequate total free bytes. Tight class-level allocation profiling,
RSS per actor and optional dedicated class pools remain release gates.

## Verification

Local x86-64 C++17 Clang optimized model: **1,285 checks passed**.
Local x86-64 Clang ASan+UBSan: **1,285 checks passed**.
This includes 1,000 allocate/release cycles with an instrumented global
operator-new counter, guaranteeing zero host new calls after heap initialization
*for that tested loop*. Existing native tenant-VM regression code was modified
to use this allocator but has **not yet been executed** at its new PR head.

The source-owned CI workflow requires (not merely checks a macro):
1. Real x86-64 optimized host allocator execution.
2. ASan/UBSan native allocator execution.
3. Real 32-bit ELF `g++ -m32` build/execution with installed multilib headers.
4. Existing native tenant VM host integration regressions.
5. Two separate 10-million-reference RSS samples.

Locally i386 compilation failed because multilib C++ headers are unavailable;
status **NOT_EXECUTED**, not PASS. Source-organization GitHub Actions historically
failed before runner admission; only nonempty successful runner steps qualify.
The benchmark `tests/benchmark_ref_width.cpp` is standalone and does **not**
demonstrate an end-to-end Atto process RSS saving.

Locally observed native process RSS for 10 million references:
`u32: 40,768 KiB`, `u64: 79,832 KiB` in separate sample runs (not an
allocator or actor process comparison; process sampling and platform matter).

## Next release gates

- Execute exact branch/tree SHA native `tenant_vm_test` and actual compiled
  `.atto` guest actor allocations using the updated host API. Prove stale
  offsets reject at the ownership-checker layer and negative syntax cannot emit
  guest IR or binaries.
- Verify real host 32-bit tests and target i686/ARM32 compiled guests, distinct
  from the existing 64-bit-only tenant-VM cage. Cross-compile and run ARM32 in
  QEMU if toolchain support exists.
- Track actual **per-actor** RSS, sidecar bytes, mailbox overhead and fragmentation
  under 1/100/10,000/100,000 actor loads before asserting 20% process reduction.
- Decide and implement a versioned generational guest reference ABI with explicit
  lifetime, ownership, checked decode, STOP/restart incarnation fences and no raw
  actor-to-actor transport. Do not introduce handle-table overhead into all
  transient offsets without measurements.
- Coordinate shared semantics with atto-common-source#13 and
  atto-platform-conformance#25 before Java/GraalVM and .NET NativeAOT parity.

## Compile standalone checks

```sh
g++ -std=c++17 -O2 -Wall -Wextra -Werror -pedantic \
    -Isrc/main/cpp tests/compact_actor_heap32_test.cpp -o /tmp/heap-test
/tmp/heap-test
clang++ -std=c++17 -O1 -g -Wall -Wextra -Werror -pedantic \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Isrc/main/cpp tests/compact_actor_heap32_test.cpp -o /tmp/heap-test-san
ASAN_OPTIONS=detect_leaks=1 /tmp/heap-test-san
```
