# AttoNATS native LLVM `String.split_literal` initial ownership slice

This commit introduces the **restricted lexical, synchronous LLVM host**
`String.split_literal` lowering. The result lives in a unique native
`Array<String>` owner, not an i64 alias or a host-implemented NATS matcher.

Supported source:
```atto
val String subject = "orders.created.eu";
val parts = subject.split_literal(".");
// Or: val Array<String> parts = subject.split_literal(".");
```

The delimiter must be a nonempty, at-most-64-byte literal; the source must
have a checked String type. The result is capped at **1024** token
entries, preserves interior/trailing empty tokens, handles embedded NUL,
and uses length-delimited byte search (not C string termination).
A native String slice borrows stable source bytes inside the same thread-local
invocation region. The owning Array handle is created and registered in the
LLVM array-failure owner shadows **before** any slice is appended. Its
cleanup is deterministic on normal and failure paths, and the lexical
scope never transfers the handle across a suspend/actor/thread boundary.

The independent native bridge is linked **only** when generated IR calls
`ores_string_split_literal_fill`, retaining the standalone String and
Array runtimes for unrelated programs. Resource failure is routed to the
current native array resource failure/cleanup path. This v1 does not
provide a guest-catching `Result` error object, an open-ended
`Array<String>` function-return ABI, a hard real-time proof, or an actor
mailbox-transfer capability.

This is deliberately a narrow stage toward compiling the unchanged
AttoNATS `src/core/subjects.atto` validation functions. Native actor
`state{}`, async Bytes/TCP and actual end-to-end NATS protocol remain
separate gates. The current LLVM source organization's Actions has
experienced zero-step failures; authoring tests or source-only passes
does not constitute native AOT execution evidence. Keep draft until
the compiler/ABI tests and sanitizers actually run on exact head.
