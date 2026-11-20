# Native String arrays

The LLVM compiler admits lexical `val Array<String> paths = new Array<String>();`
and `let mut Array<string> paths = new Array<string>();` in synchronous native
programs. Both spellings preserve the String element type through `add`, `get`,
`set`, function arguments and comparisons. Indices and `size` remain integers.
The same `val Array<int>` declaration form is admitted for integer arrays.

Array owners use the existing bounded, checked native array region, with reverse
owner cleanup on supported return and resource-failure edges. String elements
are immutable handles into the invocation's bounded String region. Overwriting
an array slot does not invalidate a String retained in another slot. The array
backing storage and the String region are deterministically reclaimed; neither
uses tracing GC. String bytes remain live until the synchronous root region
closes, rather than being reclaimed individually when a slot is overwritten.

The compiler retains the element type separately from the physical i64 ABI.
Integer values cannot enter String arrays, a String element cannot be returned
as an integer, and a String array cannot be transferred into an integer-array
field. Array owners remain non-Copy and cannot escape through scalar values.
There is no guest-accessible raw pointer or unchecked String-handle conversion.

This supplies String collections needed by Spin's path and header processing,
but does not yet implement object arrays, String-array class fields, array
parameters/returns, or actor/async storage lifetimes. String use across actors,
await and unsupported targets continues to reject. The full Spin integration
gate remains in [SPIN_NATIVE_EXECUTION_GATES.md](SPIN_NATIVE_EXECUTION_GATES.md).
