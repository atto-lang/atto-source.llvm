# Native nested class ownership

A fixed declaration-only class field can own another class, including its
Array<int>, Array<String>, or immutable-record array fields. Construction
consumes a unique, complete local child owner. Implicit class copies, duplicate
constructor arguments, borrowed children, partial owners and type-forged values
are rejected before IR output.

The checked layout expands descendants into contiguous i64 storage. Each source
class and subobject retains its nominal type, field path and declaring-class
visibility. A projection loan addresses that inline storage and freezes the
whole parent for the synchronous call; disjoint sibling loans are not inferred.
An ancestor's visibility or mutability never grants a child's private access or
strengthens a read-only edge. A final primitive slot cannot be borrowed mutably.
A val child permits mutable referent access; final without mut permits reads.

Every descendant array remains a unique owner in its frame's region. Class
construction and whole-object moves zero the previous array places and retain
initialized failure-cleanup authority. Owning results transfer all descendant
array places into the caller region before source cleanup. Multiple children,
nonzero subobject offsets and further nested children use the same protocol.
No child stack pointer, reference count or tracing collector is retained.
Array region transfer is not an actor/domain resource handoff API.

Immutable nested scalar/String records can also be materialized in the bounded
root record arena and used in record arrays. Mutable record identities and
array-owning record elements are still rejected by collection typing. Constructor
and borrowed-scalar expression temporaries allocate once at function entry,
including constructors reached repeatedly in loops.

The current layout allows at most eight class levels and sixteen expanded leaf
fields, checked regardless of declaration order. Nested fieldless identities,
mutable child scalar slots, default nested initializers and explicit constructors
for classes containing nested fields remain unsupported. This slice uses fixed
implicit constructors; it does not reinterpret scalar constructor parameters as
class ownership parameters. Borrowed subobject results and arbitrary by-value
class function parameters still require lifetime-aware ABI work.

The fixture nested-owned-capture.ores models routing PathCapture owning
RequestPath, including its String array, callee return and whole-object move.
Rust tests compare native output/status with Java, exercise imported modules and
multiple child regions, reject invalid ownership/privacy/permission/layout cases,
and run the public AOT binary after entry-source removal. Sanitized tests mark
generated LLVM definitions for ASan and verify emitted guest-memory check
calls; passing the linker flag alone is insufficient for imported LLVM IR. The fixed-field
declaration without an initializer has a separate native-only test because the
pinned Java parser does not yet admit that syntax. Sanitizers cover the
executed generated native code.

This is synchronous compiler support, not complete Spin parity. Typed HTTP
mailboxes, await frames, linear HTTP resources and actor/request-region lifetimes
remain required. Root String/record arenas are bounded invocation storage, not
the final long-running server allocation policy. Future HttpExchange capabilities
must never be classified as ordinary immutable record data.
