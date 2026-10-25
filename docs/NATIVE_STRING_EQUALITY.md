# Native String equality

The LLVM backend lowers `==`, `!=`, `eq`, and `neq` on matching String operands
to content comparison. Two different literal or concatenation handles containing
the same bytes compare equal. Length participates in comparison; embedded NUL
bytes are ordinary content. The compiler retains validated operand types through
parameters and function calls so opaque i64 handles cannot become integer
identity comparisons by accident.

Comparison allocates no storage. Both operands must belong to the active native
String region; an invalid handle poisons the invocation and causes region-end
failure. Empty strings need no payload dereference. Ordered String comparisons,
String/integer comparisons and nominal object equality remain rejected before IR.

The Rust native class/String harness executes nine shared programs, including
method/path routing primitives, concatenation equality, Unicode and embedded NUL,
in release and ASan/UBSan modes. The same source can be compared with Java using
its `--java-command` option. These are frontend/value parity tests; they do not
establish compiled Spin or source HTTP/await support.

String regions still have synchronous root-invocation lifetime. Moving String
storage across actor resumptions requires explicit actor/continuation ownership
integration; this change does not enable that transfer.
