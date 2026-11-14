# Synchronous typed array iteration

The host LLVM backend supports `for (const element of array) { ... }` (or
`val` in place of `const`) for a named local array or direct class array field.
Elements are `int`, `String`, or immutable leaf records. The routing regression
uses the real `PathPattern.copy()` and `concat()` loop bodies, including owning
aggregate returns and distinct borrowed source regions.

The compiler snapshots the handle and length once and takes a shared source
loan for the complete loop. Mutation, exclusive borrowing and ownership transfer
of that source are rejected. A class-field loan conservatively covers the entire
class owner. Nested reads are allowed. Each back edge must preserve the incoming
outer owners and loans; consuming an outer owner cannot become valid merely
because the loop might execute zero times.

Element slots and typed scalar/readonly record locals are allocated once in the
function entry. Lifetime start/end occur on reached scope edges, including every
iteration and nested branch. Scoped names cannot escape or shadow live names.
Readonly record materialization preserves nominal type and field visibility;
mutable record identity cannot be turned into an immutable snapshot. Implicit
record alias assignment remains rejected by the existing ownership rules.

Supported bodies contain calls, field writes, scoped values, nested iteration,
and `if (...) { ... } else { ... } fi`. This is synchronous host lowering:
actors, await, exception frames, return/break/continue inside the loop, C-style
loops, and strict realtime certification remain unsupported. The parser caps
structured nesting at 16 and each arm at 16 actions. Existing bounded array and
root String/record region limits still apply. This does not provide long-running
server request-region reclamation or prove Spin server parity.

The Rust parity driver checks common source against Java, runs native binaries
repeatedly with ASan/UBSan, requires unsafe programs to fail before IR output,
and runs the public AOT output with the entry source removed and an empty PATH.
