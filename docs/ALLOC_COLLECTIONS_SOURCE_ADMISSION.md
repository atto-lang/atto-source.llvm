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

The compiler-owned LLVM source registry now **registers** these three
`.atto` modules, and the exact-byte Rust generator pins their Git blob IDs.
The generated C++17 includes and registry probe test unit identity, canonical
`atto.alloc/*` plus legacy `alloc/*` spelling, and allocator-provider
requirements. This is SOURCE / REGISTRY evidence only.

The important missing gate is still **actual native guest execution**.
The current native parser, class constructor and ownership/type lowering for
`Array<int>`, typed `Option`/`Result`, and container methods have not
been demonstrated with these new modules. A successful source embedding or
C++ registry probe is never proof that the guest can import and execute them.

Before promoting this draft:

1. Add an exact-head native `oreslang-llvmc` compiler run importing
   `atto.alloc/bitset`, `atto.alloc/linked_list` and `atto.alloc/map`.
2. Resolve actual compiler diagnostics without host-language emulation or a
   more permissive unsigned allocator fallback.
3. Link compiler-emitted IR to the explicit native Array allocation provider;
   execute values in guest programs and compare with the deterministic
   behavioral vectors. Prove failure is clean, with no IR on rejection.
4. Run native sanitizer, freestanding-provider rejection, and ownership
   move/drop tests on the same immutable commit.
5. Mark zero-step runner jobs and host/source-only results `NOT_EXECUTED`;
   never claim native/JIT/AOT parity from those results.

The source implementations are independently owned by this compiler and
need not match Java's text or internal algorithm.
