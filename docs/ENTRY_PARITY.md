# Native Entry<K,V> parity gate

The Java/Graal frontend defines `Entry<K,V>` as a compiler-owned nominal two-slot
value constructed only with `Entry.of(key, value)`. It is not a user class or a
structural `{key,value}` record.

The LLVM source compiler does **not** yet implement that contract. Until it has
a real nominal representation and matching ownership/runtime semantics, native
source must fail closed instead of flattening Entry to i64, treating it as a
user class, or accepting a lookalike declaration.

## Native substrate now present

The runtime/type substrate may exist before guest syntax is admitted. In
particular, the native tree now has:

- a factory-only, immutable `NativeEntry<K,V>` carrier with readonly key/value
  projections and no host `operator==`;
- deterministic key-then-value child visitation for graph walkers;
- a fixed two-i64 ABI record for the first scalar product lowering target;
- `Entry`-specific Send/Sync/Share composition from exactly two component
  types, matching the Java/Graal immutable-product rule.
- field-sensitive native drop planning for `entry.key` and `entry.value`,
  with reverse-initialization-order exact-once teardown and double-drop
  rejection.

Those pieces are **not** permission to accept `Entry` source yet. They exist so
the parser, ownership checker, LLVM lowerer and actor transport can converge on
one nominal contract instead of independently inventing representations.

## Required parity before admission

Native source support must still land as one coherent slice covering:

- exactly two typed slots, `key` and `value`, with immutable projections;
- factory-only construction through direct `Entry.of(K,V)`;
- no first-class extraction of `Entry.of`;
- recursive Copy/Send/Share admission from both component types;
- whole-owner affine moves and projection-level move safety;
- nominal distinction from records, structs, tuples and user classes;
- guest value equality without changing host/container identity semantics;
- actor/message/freeze/copy graph traversal through both components;
- deterministic native destruction for move-only component payloads;
- stable LLVM ABI/layout tests on supported targets;
- malformed-source and shadowing negatives that emit no IR.

Until those are implemented, `Entry` is reserved as a compiler-owned builtin
identity and `Entry<...>` in native parameters, results, or locals is rejected
with an explicit parity diagnostic.

This is a fail-closed compatibility gate, not a claim of LLVM Entry support.
