# LiveView RFC6455 bounded C++ host profile (DRAFT)

This standalone, fixed-storage, allocation-free `LiveViewWireProfile.hpp`
implements masked client-frame admission and first-party LiveView TEXT profile.
It rejects unmasked frames, RSV/reserved opcodes, binary frames, control fragmentation,
invalid UTF-8/close codes, nonminimal length declarations, oversized frames,
orphan continuations, nested fragmented messages and excessive fragment counts.
Errors are typed with RFC6455 close codes; a future native socket actor must
actually transmit them.

Decoder instances own bounded arrays (about 128 KiB), one per connection.
The host must preallocate them within a controlled budget; this source does NOT
prove hard-real-time execution, scheduler bounded latency, or zero-copy transport.
Sinks receive an ephemeral borrowed view and must never execute guest code on
an I/O reactor. The adapter must own socket rights, deadlines, CONTROL capacity,
closed-generation fencing, cancellation, backpressure and actor teardown.

Same 41-vector fixture as Phoenix PR #14 and platform-conformance PR #278,
Git blob `931f58c1acd40e4c7e1b0b061f1711a0afe3c9b8`.
`tests/liveview_wire_profile_test.cpp` authors 41x4=164 deterministic
host decode observations; run `bash scripts/test-liveview-wire-profile.sh`
with GCC/Clang and ASan/UBSan. It is a standalone host profile, **NOT** a
compiled Oreslang guest, websocket listener, ISO actor, native AOT artifact,
nor independently verified parity with other backends. Underlying runtime
and public guest API must be reviewed before promotion.

The exact-head authorized private CI may be blocked before runner assignment;
zero steps is NOT_EXECUTED. No private source was mirrored to another org.
