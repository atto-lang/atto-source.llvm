#include "oreslang/runtime/NativeQuicAdmission.hpp"
#include <cassert>
#include <iostream>

using namespace oreslang::native_quic;

int main() {
    PinnedEndpoint valid{};
    valid.ip[0] = 127;
    valid.ip[3] = 1;
    valid.ip_length = 4;
    valid.port = 443;
    valid.canonical_sni = "origin.example";
    valid.alpn = "h3";
    valid.egress_generation = 4;
    valid.authorized_dns_set = true;
    assert(validate_http3_peer(valid) == Admission::StructurallyValidNotAuthorized);
    auto bad = valid;
    bad.authorized_dns_set = false;
    assert(validate_http3_peer(bad) == Admission::DeniedEgress);
    bad = valid;
    bad.egress_generation = 0;
    assert(validate_http3_peer(bad) == Admission::StaleAuthority);
    bad = valid;
    bad.port = 0;
    assert(validate_http3_peer(bad) == Admission::InvalidPort);
    bad = valid;
    bad.ip_length = 0;
    assert(validate_http3_peer(bad) == Admission::InvalidIp);
    bad = valid;
    bad.ip.fill(0);
    assert(validate_http3_peer(bad) == Admission::InvalidIp);
    bad = valid;
    bad.canonical_sni = "ORIGIN.EXAMPLE";
    assert(validate_http3_peer(bad) == Admission::InvalidSni);
    bad = valid;
    bad.canonical_sni = "origin..example";
    assert(validate_http3_peer(bad) == Admission::InvalidSni);
    bad = valid;
    bad.alpn = "h2";
    assert(validate_http3_peer(bad) == Admission::InvalidAlpn);
    bad = valid;
    bad.canonical_sni = "-bad.example";
    assert(validate_http3_peer(bad) == Admission::InvalidSni);
    bad = valid;
    bad.canonical_sni = "bad-.example";
    assert(validate_http3_peer(bad) == Admission::InvalidSni);
    bad = valid;
    bad.canonical_sni = "a................................................................";
    assert(validate_http3_peer(bad) == Admission::InvalidSni);
    bad = valid;
    bad.ip[4] = 9;
    assert(validate_http3_peer(bad) == Admission::InvalidIp);
    bad = valid;
    bad.ip[0] = 239;
    assert(validate_http3_peer(bad) == Admission::InvalidIp);
    bad = valid;
    bad.ip_length = 16;
    bad.ip.fill(0);
    bad.ip[0] = 0xff;
    bad.ip[15] = 1;
    assert(validate_http3_peer(bad) == Admission::InvalidIp);
    static_assert(!guest_quic_available(), "QUIC backend is not yet wired");
    std::cout << "QUIC host-only admission checks passed (no guest or wire parity)\n";
}
