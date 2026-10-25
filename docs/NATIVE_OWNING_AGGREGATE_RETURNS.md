# Owning aggregate return ABI

Native functions can return classes that own int, String or immutable-record
arrays. This includes `return new PathPattern(template, segments)`, returning a
named local owner, guarded/lazy return paths, forwarding calls, and named or
conditional caller bindings. The returned class remains a unique owner; it can
be moved or exclusively borrowed, unlike a readonly immutable leaf view.

The compiler appends two hidden arguments after source parameters and borrowed
region metadata: caller-owned result storage and the caller's array region.
The physical return is a compiler-only success word; source calls retain their
nominal class result type and explicit-argument arity. No callee stack address
escapes. Scalar fields are captured while live; array fields transfer into the
caller region before callee cleanup. Forwarding can pass the final destination
directly through the intermediate call.

`try_transfer_many` validates both bounded regions, unique source owners,
zeroed destination places and aggregate quotas before modifying anything.
It moves backing storage without allocating/copying it, preserves the live
token identity, clears source owner places and retires source metadata. The
old region rejects the transferred token. The caller eventually scrubs/drops
the storage. Quota failure leaves every array owner and both regions unchanged;
compiler failure shadows retain cleanup authority until commit succeeds.

The move/borrow checker rejects borrowed-owner returns, borrowed fields used
as owned constructor inputs, duplicate arrays, moved results and deferred reuse.
By-value owning parameters and mutable objects inside record arrays remain
unsupported. Direct return constructors require explicit array arguments;
classes with default array fields can be constructed in a local then returned.
Constructor input arrays must be unique named lexical owners. Recoverable guest
errors and cross-frame exception cleanup are not implemented: unhandled native
resource errors still end the invocation/process through the existing terminal
resource-failure ABI after local cleanup.

Transfers require serialized ownership; they introduce no mutex, channel, IPC,
HTTP forwarding, tracing GC or reference counting. String/immutable-record data
still has the existing bounded synchronous root lifetime. Actor, suspension,
exception-frame and external-library owning-return lifetimes remain rejected.
This is a compiler prerequisite for the real router, not a claim that Spin runs.
General router iteration/control flow, typed HTTP mailboxes and source await
remain required.
