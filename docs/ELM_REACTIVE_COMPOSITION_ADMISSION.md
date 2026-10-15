# Elm composition and reactive interop: LLVM admission

**Draft gate, not native source implementation.** The canonical Oreslang names are snake_case: `flat_map`, `and_then`, `map_err`, `filter_map`, `on_success`.
See common-source PR #36 for the versioned contract and Java PR #222 for the existing draft Option/Result implementation.

The native source compiler has narrower `Result`, `Option`, callback, actor and suspension support than Java. It must reject unimplemented combinations before emitting IR, never erase `Err`, synthesize a `None`, or treat raw Channel as multicast Observable.

`tests/elm_composition_admission.py` checks one known-good native integer program and four **currently unsupported** real Oreslang sources. A rejection must return a diagnostic and zero IR. When native composition is implemented, replace each rejection with a positive typed source->IR->binary execution test plus negative shape, ownership, callback-effect and cancellation tests.

Native lowering later requires tagged payload ownership, exactly-once active-variant destruction, short-circuit callback counts, no borrow/actor escape, safe continuation lifetimes, bounded channel demand and distinct cancellation/complete/failure outcomes. Host-only C++ Option/Result emulations are not guest parity. No general `Monad<F<_>>` typeclass in this phase.
