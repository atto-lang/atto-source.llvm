# Native immutable record collections

LLVM admits lexical `Array<PathSegment>` and other records whose fields are final
int, bool or String values. `add(new PathSegment(...))`, `get`, and replacement
with `set` retain the element's nominal type. Imported classes use the same
qualified identity as their constructors. Wrong classes, integers masquerading
as record handles, mutable classes and nested owning fields reject before IR.

Records are owned once by a bounded synchronous invocation region. Arrays own
their container storage and retain typed references to that region's immutable
records. Handles encode a non-recycling invocation generation and slot index;
schema checks prevent interpreting one class as another. Lookup is indexed,
not a graph scan. Record slots use contiguous AoS storage; growth may move the
backing allocation without changing logical identities. No address escapes.

An array `get` bound to a named class local creates a readonly view. The compiler
materializes its immutable fields in a bounded stack frame for the existing
borrowed-class ABI. This preserves all admitted observations: replacing an array
slot or growing storage cannot change an older view. Views cannot be moved,
written, or upgraded to mutable class/projected loans. Mutable aliasing and class
identity comparisons remain unadmitted until their own representation/proofs
exist. This is not an implementation of mutable objects by copying them.

The region holds at most 65,536 records with at most 16 scalar/String fields.
It scrubs storage and frees it at root exit, including failed invocations. Root
generation exhaustion never wraps or recycles. Allocation failure, invalid or
foreign handles, bounds and schema violations poison the invocation, producing
a nonzero host exit rather than a forged value succeeding. This ABI does not
yet expose recoverable guest Result errors. String bytes have the existing
bounded synchronous String-region lifetime. Neither region uses tracing GC.

Straight-line array effects can precede later local declarations. They are
lowered to compiler-owned discarded effect slots in source order; `set`'s void
host ABI never becomes a void scalar store. No extra channel or future is used.

Record-array class fields now support constructor transfer and borrowed calls;
immutable leaf returns use the [record-return ABI](NATIVE_RECORD_RETURNS.md).
Array-containing classes now have a separate [owning result ABI](NATIVE_OWNING_AGGREGATE_RETURNS.md).
Still required for the full router/Spin server: standalone array parameters and
returns, mutable objects/nested owners, iteration and actor/continuation-owned lifetimes. All actors, await,
exceptions and unsupported targets continue to reject record-region lowering.
Do not use a server-main invocation arena as an unlimited request heap. The full
[Spin execution gate](SPIN_NATIVE_EXECUTION_GATES.md) remains unmet.
