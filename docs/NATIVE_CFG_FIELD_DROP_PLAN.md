# CFG-aware native per-field exact-once destruction substrate

This is an intentionally **native-host-only** ownership proof increment. The
public Oreslang-to-LLVM compiler currently admits 1–16 `int` class fields;
those fields are trivially destructible, and `llvm.lifetime.end` is not a
destructor. **This change does not enable** class-valued nontrivial fields,
source-level partial-field `rt take`, arbitrary constructor failures,
exception unwinding, actor transfers, cancellation or `await`.

`OwnershipDropPlan.hpp` consumes the existing `OwnershipCfg.hpp` fixed-point
analysis to derive reverse-declaration-order cleanup at every reachable CFG
exit. An uncertain joined state such as `Owned|Uninitialized` produces a
runtime-guarded drop, not an unconditional destructor. Native `DropSlot`
holds one concrete per-field ownership flag, an opaque native resource
pointer, and a trusted `noexcept` destructor callback.

Execution preflights **all** flags against the exit's abstract state before
running *any* callback. If valid, each live owner is marked dropped **before**
its callback executes, and moved, uninitialized or already-dropped fields
receive no callback. A second unconditional cleanup fails closed. Outstanding
borrows and unknown/mismatched field layouts fail admission. Source-ordered
field identities keep reverse cleanup correct independent of string sorting.

Regression `tests/ownership_drop_plan_test.cpp` exercises:

- A diamond where one predecessor initializes a field and the other does not.
- A diamond where one predecessor moves or drops a field.
- Exact-once reverse-order native callbacks on both actual paths.
- Repeated cleanup, invalid flags, live-loan and duplicate-layout rejection.
- Preflight rejection before any destructive callback.

The test builds against the **exact existing** `OwnershipCfg.hpp` source blob
`031489aa0acda7d9959687a16eb607fbfb72b4da`; the new header and test
blobs are respectively `676f6e714bfe4772e14d4ab12a12d9f2c5a00396` and
`1ccd44b09d60631fcc066763821aaca1495b43fe`.
Locally, C++17 `-O2 -DNDEBUG -Wall -Wextra -Wpedantic -Werror`
and Clang `-fsanitize=address,undefined` executions both printed
`NATIVE_CFG_EXACT_ONCE_FIELD_DROP_OK`.

**Next compiler integration gates:** allocate concrete per-field initialized
flags when lowering nontrivial source class fields; emit guarded destructor
calls on each normal/early-return edge; prove safe cleanup on partially failed
initialization; integrate typed exceptional and cancellation edges only after
those source-to-native proofs are executable. Keep LLVM's existing rejection
of class-field moves and experimental owned-region transfers in place until
this is integrated. Track [#96](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/96).
