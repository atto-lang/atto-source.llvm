#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace oreslang::native_quic {

// Trusted-host descriptor only; NEVER a guest-constructible authorization.
// The ingress policy must authorize the complete DNS answer set before
// populating authorized_dns_set, and connect to the literal IP bytes only.
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
    ValidHostOnlyDescriptor,
    DeniedEgress,
    InvalidIp,
    InvalidPort,
    InvalidSni,
    InvalidAlpn,
    StaleAuthority
};

constexpr bool valid_dns_sni(std::string_view host) noexcept {
    if (host.empty() || host.size() > 253 || host.front() == '.' ||
        host.back() == '.' || host.front() == '-' || host.back() == '-') {
        return false;
    }
    bool prev_dot = false;
    for (char ch : host) {
        const bool legal = (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '.';
        if (!legal || (ch == '.' && prev_dot)) return false;
        prev_dot = ch == '.';
    }
    return true;
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
    if (!valid_dns_sni(endpoint.canonical_sni)) return Admission::InvalidSni;
    if (endpoint.alpn != "h3") return Admission::InvalidAlpn;
    return Admission::ValidHostOnlyDescriptor;
}

// Not a QUIC provider or a claim of working guest std/net/quic. A native
// QUIC/TLS engine, UDP quotas, AOT linkage and retirement receipts must land
// before this may become true. No TCP/h2 fallback is permitted.
constexpr bool guest_quic_available() noexcept { return false; }

} // namespace oreslang::native_quic
