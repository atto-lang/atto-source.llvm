# Native class ownership: bounded multi-field and guarded CFG slice

This current-main native compiler supports **1–16 ordered `int` fields per
stack-allocated class** and nonescaping shared/exclusive call-scoped loans.
It also supports limited return-only equality guards with deterministic
cleanup on every taken and fallthrough exit. These are deliberately bounded
features, **not** full Java/Graal class, Rust lifetime or actor async parity.

## Accepted source

```ores
define class Player as
  pub let int health = 100;
  pub let int stamina = 80;
end

fnc damage(borrow mut Player p): void {
  p.health = p.health - 10;
  return;
}

pub fnc main(): int {
  let Player p = new Player();
  damage(rt borrow mut p);
  if (p.health eq 90) { return p.stamina - 38; } fi
  return 0;
}
```

Each local `new Player()` is a native LLVM stack record with an ordered
`{i64, i64, ...}` layout, constant field initialization, field-indexed
`getelementptr` loads/stores, and exact `8 * number_of_fields` byte
lifetime markers. Ordinary source does not allocate a native GC/arena region.
`owned int` remains a **nonstandard LLVM-only opt-in experiment**;
`owned` is a valid ordinary identifier in public Oreslang syntax.

## Ownership and control-flow rules

- Classes are **non-`Copy`**. By-value class parameters/returns and
  `rt take` of class objects are rejected until real move/drop lowering
  exists. `borrow Player` permits reads; `borrow mut Player` permits
  reads and writes only while its call-scoped exclusive loan is active.
- **Whole-object exclusivity is conservative.** Even accesses to different
  fields of the same class cannot bypass a conflicting shared/mutable loan.
  Disjoint-field borrow splitting needs independent alias/provenance proofs.
- Class declarations require 1–16 distinct initialized `int` fields.
  Nested classes, pointer fields, unknown field names/types, extra fields,
  inheritance, constructors with arguments and escaping references reject
  with no IR.
- After locals have been initialized and ordinary statements executed, up
  to eight `if (i64 eq i64) { return i64; } fi` guards may precede a
  **mandatory terminal return**. Conditions must be pure expressions and
  class field reads: side-effectful calls, `rt take`, and borrows in the
  condition are rejected.
- An early `return rt take local_int` moves the trivially copyable scalar
  only in the **taken** branch's checker snapshot; it does not invalidate
  the distinct fallthrough branch. This is not general SSA/CFG ownership
  join analysis or support for moving non-`Copy` objects.
- Each return edge evaluates its result before reverse-order lexical
  lifetime endings. Experimental native owned-i64 regions have exactly one
  `drop` per live owner and one `close` per dynamic return path.
- All class locals are constructed **before** guards. Branch-local class
  allocations, arbitrary branches/loops, partial initialization, effects,
  `defer`, unwind/exception/panic cleanup, actor hops, `await`, `select`,
  suspension and cancellation remain fail-closed.

## Real verification

- [PR #122](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/122)
  merged with [123/123 full native CTests and sanitized class binaries](https://github.com/zed-pkg-test/java-app/actions/runs/38109426280),
  including 16 native field-layout executions.
- [PR #126](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/126)
  merged with [135/135 full native CTests](https://github.com/zed-pkg-test/java-app/actions/runs/38110049147):
  eight generated 1–8-guard native cleanup programs, ASan/UBSan class and
  region examples, and contemporaneous pinned-continuation actor tests.
- The exact `contracts/v1/semantics/fixtures/ownership.ores` negative
  mutation fixture rejects with an ownership diagnostic and emits no IR.
  [Pinned-head strict Java/LLVM comparison](https://github.com/zed-pkg-test/java-app/actions/runs/38105473772)
  passed 11/11 shared fixtures before these new native-only extensions;
  broad parity still needs additional shared executable fixtures.

The next major correctness gate is CFG/HIR ownership-state joins, partial
object initialization and **exactly-once non-`Copy` move/drop glue**, followed
by exception/cancellation unwind, actor-heap provenance and suspension
integration. Track this in [issue #96](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/96).
