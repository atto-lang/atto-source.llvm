# Owned collection fields and borrowed calls

Synchronous native classes can consume named lexical `Array<int>`,
`Array<String>` and immutable-record arrays through declaration-only `val`
fields. For example, `val Array<PathSegment> segments;` adds an implicit
constructor argument. `new PathPattern(template, segments)` moves that array
into the class. `rt take segments` is also accepted. Using the old binding,
supplying it twice, passing a borrowed field, or changing its element type
rejects before LLVM IR emission.

The constructor disarms the source owner and transfers failure-cleanup
responsibility to the class field. Storage remains in the caller's bounded
native array region; destruction occurs exactly once. There is no tracing GC.

Borrowed class calls carry a compiler-generated region pointer after all
source arguments. Methods, nested reborrows, and reordered named arguments
retain the actual owner's provenance rather than looking up handles in the
callee's region. This pointer is ABI metadata, unavailable to source code.
Shared borrows permit reads; exclusive borrows permit array mutation. A callee
does not destroy a borrowed region. Tests include two simultaneously live,
distinct caller/callee regions to detect incorrect provenance.

Owned class returns now use the [owning aggregate ABI](NATIVE_OWNING_AGGREGATE_RETURNS.md).
This does not admit standalone array returns, by-value owning parameters,
moving fields out through a borrow, mutable record-array elements, or suspended
actor lifetimes. In particular, returning an array handle and then closing its
callee region would be invalid. Those boundaries remain rejected until their
ownership/result ABI exists. The full Spin server remains unsupported.

The Rust class/string runner checks common-source programs against Java,
one explicitly native-only named-argument program, five sanitizer executions
per native program, and rejection cases. Java currently lacks named value-call
arguments; that native-only result is not reported as cross-backend parity.
