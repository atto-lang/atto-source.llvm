# Registry identity + physical handoff integration (draft)

This branch is based on LLVM PR #869 head 3766e3106369ab6c6f0423f6ea8fdc0b5a4ea043 and carries forward the registry identity fix/test family from PR #860 head 213de7b99699f87fa82ca478cf97cc1e6a96641b. The older identity branch is 166 commits behind the handoff branch; no older runtime/CMake tree replaces the newer base.

## Behavior

Every turn, loan, callback and handoff ticket now authenticates the exact issuing registry lifetime before any activity pin is erased. Equal actor ID, incarnation, sequence and activity values cannot release another registry's pins, including a replacement constructed at the same address. The process-linked monotonic domain counter reserves zero/UINT64_MAX, fails permanently on exhaustion and cannot be reset through a production API.

The handoff pin from #869 remains part of try_reclaim's busy predicate. It must be acquired before a grant is exposed and released only after physical publication or cancellation reclamation. Existing logical lock-permit revocation, callback/loan/turn fences, failed-resource quarantine and resource-before-arena close order remain in place.

The counter must remain loaded while any issued ticket survives. Independently linked runtime copies and unload/reload with outstanding tickets require separate lifecycle/namespace work. This patch changes host Ticket layout: rebuild native consumers together. It does not authenticate arbitrary guest integers or grant source code access to host receipt APIs.

## Regression coverage

- Identity suite: all four activity roles, same-address replacement, copied/replayed/wrong-role tickets, separate translation units, 768 concurrent registry instances and 146,688 rejected foreign releases.
- Existing registry suite preserved; its 64 actual LogicalMutex dequeue/cancellation schedules additionally reject same-actor/incarnation foreign handoff tickets before physical acknowledgment.
- Constructor exhaustion retains both callback and handoff pins in each of the final eight admitted registries; 1,528 terminal-range construction attempts reject, followed by 1,024 permanently exhausted retries and zero-state denial.
- Retirement barrier suite preserves real native scalar heap and ActorGroup finalization checks, adding #860's foreign callback release and 32 retirement/last-callback race rounds.
- Production-mode negative compile rejects direct mutation of the private domain counter.

## Executed local evidence (2026-10-23)

GCC/G++ 13.3.0 compiled all four checked-in test programs with C++17/C11, strict warnings as errors and pthreads. All four executed successfully under -O2 -DNDEBUG and separately under -O1 -g -DNDEBUG -fsanitize=address,undefined -fno-omit-frame-pointer. Runtime settings: ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 and UBSAN_OPTIONS=halt_on_error=1. Leak detection is disabled because failure/quarantine fixtures intentionally retain ownership; no leak-freedom claim.

Two independent negative controls compiled but failed as required: removing registry-domain validation fails the identity suite; removing the handoff busy predicate fails the paused-dequeue registry suite. YAML structure and shell syntax of the same-repository GCC/Clang optimized/sanitized workflow were validated. All four suites are included in both workflow modes.

Tested blobs:
- ActorResourceRegistry.hpp: ff94d43bdb62d68225874f5884e5b055c83cf1e0
- actor_resource_registry_identity_test.cpp: 182c9c870ba43dedd7a4de5217ae2680a7282a86
- actor_resource_registry_identity_peer.cpp: 9b275eab8bb8bf38b9ec3452d5410d852e3b4de3
- actor_resource_registry_exhaustion_test.cpp: 272c091cc7aa1a33ffcb1a061c3c7c4a9c589eee
- actor_resource_registry_test.cpp: 06b096c75292e2fc16807bed22858dfe7c382ca2
- native_actor_retirement_barrier_test.cpp: 1e340a8e0238de53931f1119dfa5f4f9a417a8b8

## Remaining gates

Local tests compile the actual named runtime headers and scalar heap with their real dependencies. They do not constitute a complete repository build/CTest, Clang or TSan execution, ARM64/macOS/Windows evidence, GraalVM verification, or compiler-generated Oreslang guest execution. CMake/CTest and LLVM development tools were unavailable in this local environment. Full exact-head CI and independent review remain required.

Scheduler integration must still mint every handoff before waiter exposure, settle it only on real physical quiescence, enroll all native/FFI loans and gate actor DONE correctly. Source Mutex/RwLock lowering and cross-backend cancellation/unwind parity remain unverified. Keep draft and unmerged until the corresponding integration gates pass. No cross-organization private source mirror was used.
