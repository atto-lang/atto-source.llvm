# Actor state scope hardening audit — October 15, 2026

Audited the synchronous scalar `with`/`using` implementation introduced by
`3dd9034bb799c92831d4e21e86022785dcecab22`. All findings below were reproduced
against that implementation before fixing them. This audit does not establish
general actor-language, asynchronous, or reference/container-state parity.

| Finding | Reproduction | Hardening |
| --- | --- | --- |
| P1: no-op admission bypass | A read callback returning `7` returned normally when called with a null heap and fabricated turn IDs. Pure scalar methods also skipped all turn checks. | Every generated native canonical state method now calls `ores_actor_i64_heap_validate_turn` before its body. It checks the existing active domain/epoch and retirement state, performs no field read, and releases the short metadata lock before guest code. |
| P1: ambiguous generated ABI | `A_B.C` and `A.B_C` both generated `oreslang_actor_A_B_C`; LLVM silently renamed the second. Actor initializers could collide with another actor's method in either declaration order. | Reject duplicate generated actor symbols across the entire compilation unit, including initializer/method collisions, before emitting IR. |
| P1: unchecked Cortex-M methods | A canonical actor's extra method calling `main()` bypassed the target's bounded-handler checks, which inspected only `on_message`. | Reject canonical scoped-state/additional-method lowering on Cortex-M in both CLI admission and backend target admission until that target has the required ABI and complete method proofs. Existing bounded legacy Cortex-M actors remain supported. |
| P2: split callback arrow | `using state.read(\|s\| - > s.x)` compiled in LLVM but JVM rejected it. Comments/newlines could also split the arrow. | Require contiguous `->` source offsets. Add matching JVM/LLVM negative cases for spaces, comments, and newlines. |
| P1: compiler stack exhaustion | A 200,117-byte callback with 100,000 nested groups and a 400,113-byte callback summing 100,000 projections both terminated the compiler with SIGSEGV, below the 8 MiB file limit. | Bound scoped callbacks to 128 recursive parser frames, 2,048 consumed tokens, and expression depth 64 before recursive effect validation. Scope-aware parser frames unwind through RAII. These are callback limits, not a claim of complete resource bounds for every native grammar form. |

## Regression coverage

`tests/with_using_hardening.py` checks duplicate ABI rejection without partial
IR, distinct legal ABI names, native/Wasm positive admission, and Cortex-M
rejection of both canonical handlers and unverified extra methods.

The compiled entry-fence harness executes three methods (no-op read, no-op
write, and a constant scalar method) under six conditions each: valid, null,
inactive, stale epoch, retired, and foreign domain. The 15 invalid calls must
terminate with SIGABRT specifically; SIGSEGV, normal return, or other failures
do not count as successful rejection. The valid calls must return their
expected values and reclaim the heap. IR assertions ensure entry validation
does not add an implicit field read.

Large-group, unary-chain, long-sum, and just-over-depth callbacks must fail with
specific budget diagnostics and no partial IR. Just-under/exact-boundary cases
remain accepted. In sanitizer mode, the test also compiles the actual production
Parser and OwnershipChecker sources with ASan/UBSan and executes the bounded
input tests through their C++ API; it does not substitute parser/runtime shims.

The JVM comparison matrix now contains 55 cases, including the three arrow
regressions. Its reference remains the exact production JVM parser/state/effect
gate sources at `42dd7ceaadf43a4b467dd1d83f065dc50c84c0cd`. Resource-limit tests
are native-specific and are not presented as JVM resource-behavior parity.

## Retained trust boundaries

The new native entry fence validates scheduler-issued turn tokens; it does not
mint admission, authenticate arbitrary native callers, or replace the scheduler's
single-flight execution guarantee. A holder of trusted raw native heap APIs is
outside the guest-language capability boundary. No lock is held across guest
execution, no read-to-write upgrade is introduced, and no live state reference
may cross a suspension. Wasm retains its separate private-instance lifecycle
guard rather than importing the native heap fence.

General reference/container state, arbitrary callback control flow, multiple
composable loans, and asynchronous methods remain unsupported. Unsupported
Cortex-M forms fail explicitly instead of inheriting the host profile.

## Verification

- Full frozen-build LLVM CTest suite: **403/403 passed**.
- Exact JVM parser/state/effect comparison: **55/55 cases matched**.
- Native parity runner under ASan/UBSan: 14 executed actor cases, 32 generated
  state-update histories, and four separate actor turns/carriers passed.
- Both new adversarial CTests passed, including 18 compiled method entry
  outcomes, ABI collisions, target admission, malformed arrows and callback
  resource boundaries.
- Sanitized adversarial execution also compiled the production C++ Parser and
  OwnershipChecker under ASan/UBSan and passed the large-input and boundary
  checks without a compiler crash.

An earlier full-suite run overlapped a compiler relink and one test could not
find the executable. It is not counted as a verified run. The final 403-test
run used a completed, unchanged compiler build. No GitHub Actions budget,
remote source transfer, PR update or merge was needed.
