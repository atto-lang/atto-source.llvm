# Legacy actor-field regression migration audit

Status: **release blocker** (2026-10-12).

The new actor rule forbids ordinary class instance data in actors. Only
`state { key: Type = initializer; }` supplies actor-owned data, and state
access uses `self.handle_state(|state| -> {...})` for read-only
loans and `self.handle_state(critical |state| -> {...})` for
exclusive writes. Prototype `using state.read/write` aliases
remain only in the restricted native implementation. User-defined actor methods
remain allowed.

The following preexisting source fixtures were inspected on the LLVM
draft branch and **still use legacy `const`/`val`/`let` actor instance
fields**. Their full original CTest behavior is **not** covered by the
targeted 45-test funded using-capability gate:

| Intended existing coverage | Confirmed affected fixture |
|---|---|
| Owned actor heap fields | `actor-owned-fields.ores` |
| Duplicate-field rejection | `actor-duplicate-field.ores` |
| Immutable-field write rejection | `actor-immutable-write.ores` |
| Local/field shadow rejection | `actor-field-shadow.ores` |
| Unknown field rejection | `actor-unknown-field.ores` |
| Unknown state write rejection | `actor-unknown-write.ores` |
| Unsupported field type rejection | `actor-unsupported-field.ores` |
| Native source actor send | `source-actor-send.ores` |
| Five-worker actor lifecycle | `source-actor-five-workers.ores` |
| Spawn/stop actor field handling | `source-actor-spawn-stop.ores` |
| Tagged integer DATA dispatch | `source-tagged-int-payload-dispatch.ores` |
| Boolean receive match | `message-receive-bool-match.ores` |
| Three-actor bidirectional native pipeline | `source-actor-pipeline-bidirectional.ores` |
| Generated actor ABI collision | `actor-abi-actor-collision.ores` |

**Migration criteria:**

1. Positive fixtures must be rewritten to declare state, use legal
   scoped capability loans, and **still exercise their original runtime
   behavior**, not simply turn into compile-only checks.
2. Negative fixtures must fail for the originally intended invariant
   (e.g. duplicate key, shadowing or ABI collision), not for an unrelated
   early legacy-field syntax rejection.
3. General ordinary actor method and legacy boolean-match state access
   require real ownership-aware lowering before these historical fixtures
   can be fully migrated. The currently executable LLVM `handle_state` subset
   covers only exhaustive **tagged receive arms** and fixed `int` keys.
4. Keep Java/GraalVM fail-closed until equivalent guest `handle_state`
   capability semantics exist; do not rewrite source tests to weaken that
   safety boundary.
5. Re-run full relevant repository CMake/CTest, Java Maven/JUnit,
   Native Image and sanitizer suites on exact source trees **before**
   removing the draft restriction or merging.

This inventory covers 14 inspected files, **not necessarily all**
legacy actor-field usages in the repository. It is not a claim that every
listed test fails the same way or that these are all the outstanding gaps.

Focused new-capability evidence:
https://github.com/zed-pkg-test/oreslang-private-pr-validation-20261012/actions/runs/38314809579
(57 selected native CTests) and
https://github.com/zed-pkg-test/oreslang-private-pr-validation-20261012/actions/runs/38314867754
(27 focused JDK25 compiler checks).
