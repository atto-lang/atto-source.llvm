# Native actor scheduler shared-payload bridge (host-only integration)

## What is implemented

The native actor scheduler already has a bounded, generation-checked DATA mailbox whose payload is an opaque u64. New host class NativeActorSharedPayloadBridge binds a target actor incarnation to preallocated immutable-buffer descriptor slots. The **existing** ores_native_actor_scheduler_try_send is the only delivery/admission route, and the C scheduler's separate CONTROL lane retains priority.

- A source actor must hold an **authenticated currently executing DATA turn** with a native scheduler-minted DATA-mail sequence; CONTROL/WAKE/host callbacks and forged/external actor references are rejected.
- The bridge issues a preallocated slot ticket (32-bit generation, 32-bit nonzero slot index). No payload bytes or C++ addresses appear in actor DATA messages.
- A trusted native scheduler API returns the exact target DATA-mail sequence on successful admission. The bridge binds (ticket, actor incarnation, admitted sequence) under the same handoff and rejects a ticket presented in a different DATA turn, even when its index/generation are valid. Failed admissions return sequence 0.
- A descriptor holds immutable backing once; copies are reference-counted without copying 1 MiB of bytes. Both the registry and target callback hold independent lifetime references.
- Byte credits reserve the entire backing allocation per queued ticket. All admission failures return the ticket slot and drop its reference; the native scheduler is the authority for bounded admission.
- The target actor's authenticated physical callback consumes a ticket exactly once; stale/replayed/wrong-slot and malformed tickets fail closed.
- STOP_SEALED may abandon DATA tickets inside the C mailbox. After **scheduler terminal DONE**, close_after_done drops all remaining descriptor references. The caller must fence the bridge lifetime until DONE.
- This test is host integration, not source-level compiler lowering, zero kernel copy, cross-backend parity, or hard-real-time WCET evidence.

## Integration test and limitations

The CTest target native_actor_shared_payload_real_scheduler compiles the real NativeActorScheduler.c with a C++ test. One native actor publishes a 1 MiB allocation into the bridge from its authenticated callback. A second actor receives 100 legitimate descriptor tickets, checks the same immutable backing address and octets, and rejects replays. The source deliberately injects a forged first DATA envelope containing the *valid-looking first descriptor ticket* before publishing it, making mail #1 carry an exact ticket for mail #2. Only sequence-bound redemption rejects this cross-mail substitution (one denied cross-mail plus 100 denied post-delivery replays). An independently reserved supervisor CONTROL envelope must run first. A second run uses one worker and STOP_SEALED to abandon all 101 accepted DATA messages (100 retained descriptors), proving terminal cleanup/refcount release. The test also rejects external senders.

Outstanding security and language gates:

1. Compiled AttoLang source-to-MIR/LLVM lowering, typed immutable Bytes, and compile-time rt share / rt take authorization, with negative alias tests.
2. Separate handle/capability namespaces: untrusted guest code must NEVER manufacture this host ticket or enter this bridge through a raw u64 scalar route. Do not enable guest access before a typed, authenticated source lowering and mailbox envelope are in place.
3. Cross-domain, cross-isolate, and cross-node messages require safe serialization or authorized copying: no raw pointers.
4. Shared-domain and tenant aggregate accounting; memory pressure and deletion on cancellation/restart; bounded I/O pins until nonblocking completion.
5. Source-org exact-head executable CI and review. External private-carrier mirroring must separately satisfy source authorization and approved destination/review conditions; a zero-step Actions job is NOT_EXECUTED.

## Build evidence

No whole-tree compiler or native guest-AOT receipt is asserted for this branch. Do not mark the CTest as passed until an actual compiler invocation and stepful test logs are inspected.
