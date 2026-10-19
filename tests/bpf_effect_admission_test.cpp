#include "oreslang/runtime/BpfEffectAdmission.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void check(bool ok, const char* label) {
    if (!ok) throw std::runtime_error(label);
}
} // namespace

int main() {
    unsigned cases = 0;
    for (unsigned packet = 0; packet < 2; ++packet)
        for (unsigned ack = 0; ack < 2; ++ack)
            for (unsigned closure = 0; closure < 2; ++closure) {
                ++cases;
                const bool expected = closure != 0 ? ack == 0 : packet == ack;
                bool accepted = false;
                std::string diagnostic;
                try {
                    oreslang::enforce_bpf_xdp_effect_admission(
                        packet != 0, ack != 0, closure != 0);
                    accepted = true;
                } catch (const std::runtime_error& error) {
                    diagnostic = error.what();
                }
                check(accepted == expected, "XDP effect gate accepted wrong capability matrix");
                if (!expected) {
                    check(!diagnostic.empty(), "missing fail-closed diagnostic");
                    if (closure)
                        check(diagnostic.find("closure cannot grant") != std::string::npos,
                              "effect declaration unexpectedly treated as authority");
                    else if (packet)
                        check(diagnostic.find("--bpf-grants=bpf.packet.read") != std::string::npos,
                              "missing packet grant diagnostic");
                    else
                        check(diagnostic.find("does not use") != std::string::npos,
                              "unused packet grant diagnostic");
                }
            }
    check(cases == 8, "incomplete XDP effect admission matrix");
    // Wire-format contract consumed by oreslang-target-profiles. The two
    // serialized declarations differ ONLY in compiler-inferred effect.
    // Neither may include self-authorized grants or backend-specific IR.
    const std::string constant =
        R"({"schemaVersion":1,"entry":"ores_xdp","target":"ebpf","backend":"llvm","grants":[],"nodes":[{"id":"ores_xdp","targets":["ebpf"],"requires":[],"imports":[]}]})";
    const std::string packet =
        R"({"schemaVersion":1,"entry":"ores_xdp","target":"ebpf","backend":"llvm","grants":[],"nodes":[{"id":"ores_xdp","targets":["ebpf"],"requires":["bpf.packet.read"],"imports":[]}]})";
    check(oreslang::emit_bpf_xdp_ungranted_closure(false) == constant + "\n",
          "XDP constant-only closure wire shape changed");
    check(oreslang::emit_bpf_xdp_ungranted_closure(true) == packet + "\n",
          "XDP packet-read closure wire shape changed");
    check(oreslang::emit_bpf_xdp_ungranted_closure(false).find("bpf.packet.read") ==
              std::string::npos,
          "constant-only closure incorrectly requires packet read");

    std::cout << "XDP host effect acknowledgement: 8/8 cases PASS; "
                 "closure wire contracts: 3/3 PASS; "
                 "not eBPF verifier or source policy proof\n";
}
