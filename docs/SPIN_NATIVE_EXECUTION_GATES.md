# Compile the real Spin server on LLVM

The native HTTP adapter in `examples/native-http` is executable AOT machine code
with in-process mailboxes and nonblocking sockets. It is not a compiled Spin
`Application`: the adapter still owns transport framing and its fixed routing.
Successful adapter curls must not be reported as Spin execution parity.

The current Spin source at `oreslang-spin` revision
`856422ade7e48aebfd8653f27f2624d590b8d8b7` imports
`src/spin.ores` and the Oreslang routing library. Compiling `src/spin.ores` still
fails at `FrameworkResponse`: `actor requires at least one scalar field`.
Adding a dummy field would merely expose later unsupported constructs.

The execution path requires these compiler/runtime changes, in dependency order:

1. General class/reference-owned fields and mutable/nested `Array<Binding>`
   storage, with deterministic owner/drop behavior. Lexical immutable
   `Array<PathSegment>` now executes with typed region-owned records; object-array
   fields now support constructor ownership transfer and borrowed-class access
   for synchronous calls (see `NATIVE_OWNED_COLLECTION_FIELDS.md`). Owning class
   returns now transfer arrays through caller-owned result storage and regions
   (see `NATIVE_OWNING_AGGREGATE_RETURNS.md`). Standalone array returns,
   by-value parameters and mutable elements still need lowering. Lexical synchronous
   `Array<String>` and its owned class fields now execute natively; standalone
   array parameter and return ownership still need lowering. Declaration-only int/bool/String records
   such as PathSegment now construct and copy natively. Native stack records and local
   scalar/String arrays alone cannot represent the router.
   Immutable leaf class returns such as `segment_at() -> PathSegment` now use
   checked root handles (see `NATIVE_RECORD_RETURNS.md`). Deeper mutable/nested
   reference identity and actor-lifetime result ownership still need lowering.
2. Typed `Result`/`Option` values, collection iteration and the remaining control
   flow used by the actual router. String content equality is now available.
3. Source actor mailbox signatures and typed HTTP exchange capabilities. Every
   valid request and framework-generated reply must execute in an actor turn.
   Isolated transfers must invalidate the previous owner; no actor IPC or HTTP
   forwarding may substitute for same-process mailbox delivery.
4. Source Future/await lowering into pinned continuations, including cancellation,
   request completion, teardown and physical retirement. Host NIO completion and
   cleanup receipts are prerequisites, not implementations of source await.
5. Actor/continuation-owned String and aggregate regions. The CLI String root
   lifetime cannot silently become storage for an indefinitely running server.
6. Native lowering of the logging/OTel modules and the REST demo's remaining
   standard-library operations. Unsupported filesystem operations must reject;
   no blocking worker fallback may masquerade as kernel NIO.

The acceptance gate is the same checked Spin server source compiled to a native
executable, run with Java and source/compiler files unavailable, serving the
shared/isolated, parameter, middleware, error and lifecycle cases in Spin's
integration suite. Follow it with the REST demo's ten curls. The adapter's fixed
health workload and the String routing unit programs are useful intermediate
tests but do not satisfy this gate. Keep Spin's LLVM execution status unsupported
until this positive execution test passes.
