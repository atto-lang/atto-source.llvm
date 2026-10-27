# LLVM QUIC and HTTP/3 native host admission (not implemented transport)

This draft introduces a standalone C++17 tested host-only descriptor: authorized complete DNS set, literal IPv4/IPv6 pin, generation, canonical TLS SNI and ALPN `h3`. It explicitly rejects unauthorized or malformed descriptors. It is **not a QUIC UDP/TLS engine, guest lowering or HTTP/3/QPACK implementation** and does not advertise support.

The next runtime slice is an AOT-linked, audited QUIC v1 TLS 1.3 engine fed by nonblocking UDP, with per-session/per-stream quotas and native generation-tagged retirement receipts. The guest-visible `core/net/quic` and `std/net/quic` API must use opaque handles and true nonblocking await, with `std/http3` remaining a separate source-layer implementation. No HTTP/2 downgrade; no 0-RTT or path migration until proven policy-safe. Gate source guest support on exact-head interoperable QUIC/HTTP3 tests and LLVM AOT build.

Test this standalone host preflight with `bash scripts/test-native-quic-admission.sh`. It proves **only descriptor-validation negatives**, not QUIC network traffic.
