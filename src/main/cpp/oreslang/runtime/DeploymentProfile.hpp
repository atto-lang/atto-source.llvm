#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace oreslang {

// A deployment profile is NOT a target triple and does not itself prove that
// executable code has been certified. Keep names pinned; never silently fall
// back to the generic host backend.
class DeploymentProfile final {
public:
    enum class Kind {
        EbpfXdp,
        EbpfTc,
        CarrierLinux,
        FpgaHls,
        AsicRtl
    };

    static DeploymentProfile parse(std::string_view name) {
        if (name == "ebpf-xdp") return DeploymentProfile(Kind::EbpfXdp);
        if (name == "ebpf-tc") return DeploymentProfile(Kind::EbpfTc);
        if (name == "carrier-linux") return DeploymentProfile(Kind::CarrierLinux);
        if (name == "fpga-hls") return DeploymentProfile(Kind::FpgaHls);
        if (name == "asic-rtl") return DeploymentProfile(Kind::AsicRtl);
        throw std::invalid_argument("unknown deployment profile: " + std::string(name));
    }

    std::string_view name() const noexcept {
        switch (kind_) {
            case Kind::EbpfXdp: return "ebpf-xdp";
            case Kind::EbpfTc: return "ebpf-tc";
            case Kind::CarrierLinux: return "carrier-linux";
            case Kind::FpgaHls: return "fpga-hls";
            case Kind::AsicRtl: return "asic-rtl";
        }
        return "invalid";
    }

    std::string_view required_output() const noexcept {
        switch (kind_) {
            case Kind::EbpfXdp:
            case Kind::EbpfTc: return "verified BPF ELF/BTF";
            case Kind::CarrierLinux: return "qualified native AOT ELF";
            case Kind::FpgaHls: return "synthesized RTL and timing evidence";
            case Kind::AsicRtl: return "formally checked RTL and signoff evidence";
        }
        return "invalid";
    }

    // This method is the sole current emitter-admission authority for the
    // deployment-profile CLI. Fail closed for every profile until *actual*
    // backend emission and validation are wired to a pinned evidence contract.
    // Never enable a profile by changing metadata alone.
    void require_certified_emitter() const {
        throw std::logic_error(
            "deployment profile '" + std::string(name()) +
            "' has no certified compiler emitter/validation pipeline (requires " +
            std::string(required_output()) + "); refusing host fallback");
    }

    Kind kind() const noexcept { return kind_; }

private:
    explicit DeploymentProfile(Kind kind) noexcept : kind_(kind) {}
    const Kind kind_;
};

} // namespace oreslang
