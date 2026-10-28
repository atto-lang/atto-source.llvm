# LLVM QUIC/HTTP3: synchronous host preflight, not authorization

Status: **no compiled guest QUIC, no QUIC engine and no HTTP/3 wire support**.

`NativeQuicAdmission.hpp` is a C++17 host-only **structural check**. Its `authorized_dns_set` boolean is caller supplied and trivially forgeable; therefore `validate_http3_peer` returns `StructurallyValidNotAuthorized` even for an otherwise valid descriptor. **This enum outcome MUST NOT authorize opening a UDP socket**. The eventual native provider must obtain an opaque, single-use/generation-bound capability from an independently verified egress policy. Imported `std::string_view` fields are borrowed for synchronous preflight only and MUST NOT be retained across async waits.

The structural check now rejects oversized/invalid DNS labels, per-label leading/trailing hyphens, invalid IPv4 layout, IPv4 and IPv6 multicast, missing generations, nonexistent/unspecified peers and ALPN other than `h3`. These checks do not prove source policy admission, certificate identity, anti-amplification, native memory lifetime or QUIC compliance.

**Executed evidence:** Exact PR Git blobs `d740b5cada0d80a7524a374bf3520ac455efe9b8` (header) and `4d3eeebd361f48c9dd30d58116c52b393622067b` (negative tests) compiled using `clang++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -UNDEBUG` and ran locally. All asserted negative checks passed. Full LLVM compiler, runtime, external QUIC interop and CI workflows remain untested.

Next: select and pin an audited AOT native QUIC/TLS 1.3 provider, integrate bounded nonblocking UDP with native actor-lease and physical retirement semantics, bind genuine source-compiled `std/net/quic` to the sealed capability, and add independent QUIC/HTTP3 peer tests. Never claim h2/TCP is QUIC. Disable 0-RTT and migration until authorized.
