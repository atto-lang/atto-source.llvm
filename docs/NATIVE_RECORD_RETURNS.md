# Native immutable record returns

Functions and methods can return leaf classes with final int, bool and String
fields. This includes `PathPattern.segment_at(int) -> PathSegment`, constructors
in return expressions, forwarding calls, lazy conditional values, and local
immutable records. Imported return types retain their qualified nominal identity.
Integers and structurally similar but differently named classes cannot stand in
for the declared return type. The OS entrypoint still returns int or void.

The ABI returns a schema-checked, generation-checked root record handle. Returning
a local record copies its fields before stack cleanup; returning an array element
retains its immutable root value even after the callee destroys its array
container. No pointer into a callee frame or closed array region escapes. Call
results bound to a named class local materialize readonly fields for the existing
borrowed-method ABI. Owned local returns participate in ownership/move checks;
incoming class loans cannot be returned through this owned-value ABI.
Lazy return arms have distinct ownership CFG paths, including their moves and
deferred cleanup checks. A deferred call cannot reuse the returned owner.

Record snapshots allocate bounded native storage and are recorded as allocating
effects. The region uses deterministic destruction and no tracing GC. It remains
a synchronous invocation region, limited to 65,536 records with up to 16 fields;
it is not a lifetime policy for an indefinitely running server.

`scripts/compile-aot.sh` now links the record and array runtimes when their
declarations occur in generated IR, in addition to the String runtime. Rust
CTest coverage builds both scalar-only and record/String/array programs with
this public driver, deletes the entry source, then executes with an empty PATH
and environment. The executable does not invoke Java, a compiler or source code.
The driver rejects output aliases of the source/compiler/Clang by file identity;
Rust tests check direct paths, symlinks and hard links without touching real tools.

Array-containing classes now use a separate [owning return ABI](NATIVE_OWNING_AGGREGATE_RETURNS.md)
that transfers storage rather than snapshotting array tokens. Mutable object
identity within record arrays, nested reference owners, by-value owning
parameters, returned loan lifetimes, actors, suspension and exception-frame
record lifetimes remain unimplemented and rejected. General router loops are
the next parser/lowering boundary in the actual routing source; the full Spin
server execution gate remains unmet.
