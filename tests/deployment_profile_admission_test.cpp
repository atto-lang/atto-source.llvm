#include "oreslang/runtime/DeploymentProfile.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

static void check(bool ok) {
    if (!ok) throw std::runtime_error("deployment profile safety contract failed");
}

int main() {
    using oreslang::DeploymentProfile;
    struct Case { const char* name; DeploymentProfile::Kind kind; const char* output; };
    const std::array<Case, 5> cases{{
        {"ebpf-xdp", DeploymentProfile::Kind::EbpfXdp, "verified BPF ELF/BTF"},
        {"ebpf-tc", DeploymentProfile::Kind::EbpfTc, "verified BPF ELF/BTF"},
        {"carrier-linux", DeploymentProfile::Kind::CarrierLinux, "qualified native AOT ELF"},
        {"fpga-hls", DeploymentProfile::Kind::FpgaHls, "synthesized RTL and timing evidence"},
        {"asic-rtl", DeploymentProfile::Kind::AsicRtl, "formally checked RTL and signoff evidence"}
    }};
    static_assert(!std::is_default_constructible<DeploymentProfile>::value,
                  "deployment capabilities cannot be default constructed");
    static_assert(!std::is_assignable<DeploymentProfile&, DeploymentProfile>::value,
                  "profile identity cannot be overwritten");
    for (const auto& item : cases) {
        const auto profile = DeploymentProfile::parse(item.name);
        check(profile.kind() == item.kind);
        check(profile.name() == item.name);
        check(profile.required_output() == item.output);
        bool denied = false;
        try {
            profile.require_certified_emitter();
        } catch (const std::logic_error& e) {
            const std::string diagnostic = e.what();
            denied = diagnostic.find(item.name) != std::string::npos
                  && diagnostic.find(item.output) != std::string::npos
                  && diagnostic.find("refusing host fallback") != std::string::npos;
        }
        check(denied);
    }

    for (std::string_view invalid : {
            "", "host", "linux", "carrier", "bpf", "ebpf",
            "ebpf-xdp ", "EBPF-XDP", "ebpf-xdp,host", "ebpf-xdp\n--target=host",
            "fpga-hls ", "asic-rtl-v2", "../asic-rtl", "x86_64-unknown-linux-gnu"}) {
        bool denied = false;
        try {
            (void)DeploymentProfile::parse(invalid);
        } catch (const std::invalid_argument&) {
            denied = true;
        }
        check(denied);
    }

    // A view with an embedded NUL must never be treated as a valid prefix.
    const std::string with_nul("ebpf-xdp\0--target=host", 22);
    bool denied = false;
    try {
        (void)DeploymentProfile::parse(std::string_view(with_nul));
    } catch (const std::invalid_argument&) {
        denied = true;
    }
    check(denied);
    std::cout << "PASS: all deployment profiles deny emission; aliases and malformed names rejected\n";
}
