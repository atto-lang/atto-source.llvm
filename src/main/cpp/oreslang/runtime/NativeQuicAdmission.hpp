#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace oreslang::native_quic {

// Preflight descriptor only: the boolean is NOT an authorization token.
// Untrusted code can forge this struct. A future provider MUST consume a
// separate, unforgeable, source-generation-bound policy capability instead.
// string_view fields are borrowed and MUST NOT outlive this synchronous check.
struct PinnedEndpoint {
    std::array<std::uint8_t, 16> ip{};
    std::uint8_t ip_length = 0; // 4 (IPv4) or 16 (IPv6)
    std::uint16_t port = 0;
    std::string_view canonical_sni{};
    std::string_view alpn{};
    std::uint64_t egress_generation = 0;
    bool authorized_dns_set = false;
};

enum class Admission {
    StructurallyValidNotAuthorized,
    DeniedEgress,
    InvalidIp,
    InvalidPort,
    InvalidSni,
    InvalidAlpn,
    StaleAuthority
};

constexpr bool valid_dns_sni(std::string_view host) noexcept {
    if (host.empty() || host.size() > 253 || host.front() == '.' ||
        host.back() == '.') return false;
    std::size_t label_size = 0;
    char previous = '.';
    for (char ch : host) {
        const bool alpha = ch >= 'a' && ch <= 'z';
        const bool digit = ch >= '0' && ch <= '9';
        if (ch == '.') {
            if (label_size == 0 || label_size > 63 || previous == '-') return false;
            label_size = 0;
        } else {
            if (!alpha && !digit && ch != '-') return false;
            if (label_size == 0 && ch == '-') return false;
            ++label_size;
            if (label_size > 63) return false;
        }
        previous = ch;
    }
    return label_size > 0 && previous != '-';
}

constexpr Admission validate_http3_peer(const PinnedEndpoint& endpoint) noexcept {
    if (!endpoint.authorized_dns_set) return Admission::DeniedEgress;
    if (endpoint.egress_generation == 0) return Admission::StaleAuthority;
    if (endpoint.port == 0) return Admission::InvalidPort;
    if (endpoint.ip_length != 4 && endpoint.ip_length != 16) return Admission::InvalidIp;
    bool any_byte = false;
    for (std::size_t i = 0; i < endpoint.ip_length; ++i) {
        any_byte = any_byte || endpoint.ip[i] != 0;
    }
    if (!any_byte) return Admission::InvalidIp;
    if (endpoint.ip_length == 4) {
        // Canonical v4: unused bytes must be zero, and multicast/broadcast
        // ranges may not be minted into a remote QUIC peer descriptor.
        for (std::size_t i = 4; i < endpoint.ip.size(); ++i) {
            if (endpoint.ip[i] != 0) return Admission::InvalidIp;
        }
        if (endpoint.ip[0] == 0 || endpoint.ip[0] >= 224)
            return Admission::InvalidIp;
    } else if (endpoint.ip[0] == 0xff) {
        return Admission::InvalidIp;
    }
    if (!valid_dns_sni(endpoint.canonical_sni)) return Admission::InvalidSni;
    if (endpoint.alpn != "h3") return Admission::InvalidAlpn;
    // A successful preflight CANNOT serve as permission to connect.
    return Admission::StructurallyValidNotAuthorized;
}

// Not a QUIC provider or a claim of working guest std/net/quic. A native
// QUIC/TLS engine, UDP quotas, AOT linkage and retirement receipts must land
// before this may become true. No TCP/h2 fallback is permitted.
constexpr bool guest_quic_available() noexcept { return false; }

} // namespace oreslang::native_quic
