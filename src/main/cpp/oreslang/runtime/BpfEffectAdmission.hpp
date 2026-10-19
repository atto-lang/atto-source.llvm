#pragma once
// Experimental XDP target: this is a local compiler effect acknowledgement,
// NOT a trusted build-policy authorization or a Linux verifier result.
#include <stdexcept>

namespace oreslang {

inline void enforce_bpf_xdp_effect_admission(
    bool packet_reads, bool explicit_packet_acknowledgement, bool closure_mode) {
    if (closure_mode) {
        // An inferred graph must never contain self-authorized capabilities.
        if (explicit_packet_acknowledgement)
            throw std::runtime_error("eBPF effect closure cannot grant packet authority");
        return;
    }
    if (packet_reads && !explicit_packet_acknowledgement)
        throw std::runtime_error(
            "eBPF XDP packet_u8 requires --bpf-grants=bpf.packet.read");
    if (!packet_reads && explicit_packet_acknowledgement)
        throw std::runtime_error(
            "eBPF XDP constant action does not use bpf.packet.read");
}
} // namespace oreslang
