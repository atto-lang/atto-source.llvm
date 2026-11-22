# AttoLang LLVM alloc collections: source-owned candidate

This branch introduces **native-backend-owned .atto source** under
`stdlib/alloc/{bitset,linked_list,map}.atto`.

## Implementations

- `bitset`: `BitSet` uses owned 63-bit nonnegative words with arithmetic masks
  and exact boundaries, avoiding any assumption about native bit-shift lowering.
- `linked_list`: `IntLinkedList` stores a doubly-linked index graph in owned
  `Array<int>` values and recycles zeroed removed slots.
- `map`: `IntMap` uses an independent, bounded hash-bucket chaining design,
  including negative keys, replacement, deletion, slot reuse, and full-capacity
  rejection without mutation.

The current integer-specialized contract includes:
`new_bitset`, `len`, `get`, `try_set`, `clear_all`,
`count_ones`, `first_set`, `try_union_with`,
`try_intersect_with`; `new_linked_list`, `push_front`,
`push_back`, `pop_front`, `pop_back`, `front`, `back`,
`contains`, `remove_first`, `clear`, `to_array`;
and `new_map`, `capacity`, `get`, `get_or`,
`contains_key`, `try_insert`, `remove`, `clear`.

## Admission and unsupported boundaries

This is **SOURCE ONLY**, not native guest parity. The LLVM compiler currently
embeds a closed source registry of `numeric`, `int_buffer`, and
`int_buffer_bundle`; it does **not** install or link these three new
modules. Do not extend the generated registry or grant allocator provider
admission just to turn a red native compiler into a green test.

Before promoting this draft:

1. Extend native class/method/typechecking, owned `Array<int>`, `Option`,
   `Result`, checked arithmetic and module-linked constructor support where
   presently unavailable; refuse unknown constructs with diagnostics.
2. Register the new files only with exact Git blob SHA verification through
   `tools/atto-library-embed`, generate compiler-owned includes, retain
   canonical/legacy unit identity, and enforce allocator-provider closure.
3. Execute native compiler + linked guest fixtures on the immutable PR SHA,
   including negative import/path, bounds, capacity and ownership fixtures.
4. Run sanitizer, freestanding-provider rejection, and AOT evidence gates.
5. Treat pending, zero-step and host-model-only results as `NOT_EXECUTED`.

No Java source is imported or shared with this repo at runtime, and these
source files are independently maintained here.
