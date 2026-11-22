# Core four libraries (v1): range, cell, contracts, num

These are backend-owned AttoLang source files under core/. They do not depend
on byte-identical source files in another compiler repository.

## API and semantics

- core/range: half-open and inclusive contains/empty/overlaps, inclusive clamp
  returning Result on inverted bounds, and overflow-guarded forward/backward stepping.
- core/cell: admission-only borrow-state transitions. 0=idle, -1=exclusive,
  1..MAX=shared count; saturation rejects additional shared borrows.
  Not a generic Cell<T> or RefCell<T>; not authorization to access memory.
- core/contracts: requires, ensures and invariant are always-on runtime checks
  which fail by rt fail. They may not disappear in JIT, release or AOT.
- core/num: checked int64 add/sub/mul/div/rem/neg/abs; saturating
  add/sub/mul/neg/abs; wrapping add/sub/mul/neg. Signed overflow and division
  failure must not depend on host-language overflow behavior.

The intended imports are:
    import module range from "core/range";
    import module cell from "core/cell";
    import module contracts from "core/contracts";
    import module num from "core/num";

A file on disk is not proof that the compiler's import resolver supports it.
The current native compiler may need registry or package-discovery updates.

## Verification commands

Source/model checks (NON-CERTIFYING):
    node --test tests/core-four-v1-source.test.mjs

Native compiled guest (requires actual built compiler and native toolchain):
    export ATTO_CORE_FOUR_LANE=llvm-native
    export ATTO_CORE_FOUR_COMPILE_ARGV_JSON='["bash","scripts/compile-aot.sh","{source}","{artifact}"]'
    export ATTO_CORE_FOUR_RUN_ARGV_JSON='["{artifact}"]'
    node scripts/run-core-four-guest.mjs

LLVM sample requires build/oreslang-llvmc, build/oreslang-no-gc-ir-audit and
Clang. For other backends supply their real compile/run argv, not a script
that evaluates the fixture in another language.

The checked-in smoke guest must compile and exit 42.
The negative fixture must compile and execute, then fail with the exact
core-four-v1: precondition failed witness. A different error is not a pass.
Even successful local execution is NON-CERTIFYING until independently tied
to the current source tree, actual runner steps and compiler build.

## Ownership work still required

The core/cell integer transitions alone DO NOT establish safety. Before
advertising Cell<T> or RefCell<T>, the frontend/runtime must implement:
- owner-bound generation-scoped access grants, with actor and domain identity;
- borrow! shared reads and exclusive mutable borrows, checked atomically;
- no alias after take!, no escaped mutable borrows and no cross-actor mutation;
- no live mutable borrow across await or cancellation without pinned safety;
- deterministic cleanup on throw, raise, panic and cancelled callbacks;
- compile-negative tests for escaped borrows, conflicting mutable borrows,
  use-after-take, stale owner generation and foreign-actor access.

Do not conflate runtime policy function tests with real compiler borrow tests.
An unauthorized cross-org private source export is prohibited. Any Actions job
with zero actual steps is NOT_EXECUTED, not a test failure or success.
