#pragma once
// Experimental XDP target: this is a local compiler effect acknowledgement,
// NOT a trusted build-policy authorization or a Linux verifier result.
#include <stdexcept>
#include <string>

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
// Compiler-inferred effect declaration ONLY. No compiler flag, source AST,
// or IR lowering path is permitted to smuggle a build grant into this graph.
// Keeping this serializer LLVM-free lets native tests pin its wire contract.
inline std::string emit_bpf_xdp_ungranted_closure(bool packet_reads) {
    return std::string(
        "{\"schemaVersion\":1,\"entry\":\"ores_xdp\","
        "\"target\":\"ebpf\",\"backend\":\"llvm\",\"grants\":[],"
        "\"nodes\":[{\"id\":\"ores_xdp\",\"targets\":[\"ebpf\"],"
        "\"requires\":") +
        (packet_reads ? "[\"bpf.packet.read\"]" : "[]") +
        ",\"imports\":[]}]}\n";
}

} // namespace oreslang
