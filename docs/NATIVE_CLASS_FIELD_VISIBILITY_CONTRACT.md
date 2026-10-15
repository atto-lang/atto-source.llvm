# Native class field visibility — bounded LLVM subset

The currently supported native class subset has no compiled instance methods.
Every field projection from a free function is external to the class body and
therefore requires an explicit `pub` modifier.

The native parser retains the `pub` flag for each class field, separately
from fixed binding (the `final` bit). Ownership admission propagates the
public-slot set through class locals, class parameters and supported class moves.
It rejects any private field read, write, destructive take or owned
`Array<int>` field access before LLVM IR emission. Merely borrowing an owner
or holding an exclusive mutable owner does not confer private visibility.

Negative CTest fixtures cover scalar reads, writes and takes, and both
`Array<int>.size` and mutation through a private class field. An executable
fixture covers public final-field read returning `42`.

This changes only the current mainline visibility path. Full native scalar
constructor lowering from the old draft #294 is not integrated here.
The branch remains draft until exact-head CMake/CTest, AddressSanitizer,
UndefinedBehaviorSanitizer and Java/LLVM parity runs have actually executed.

## Bulk array-commit privacy gate

The source-reachable `rt array_commit_filled` transaction must verify that all
four array fields are public before mutating any. Even a unique mutable owner
cannot use this intrinsic to access private fields from a free function.
The private-field fixture must reject before LLVM IR emission.
