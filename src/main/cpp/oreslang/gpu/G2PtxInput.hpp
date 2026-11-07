#pragma once
#include <cstddef>
#include <istream>
#include <stdexcept>
#include <string>

namespace oreslang::gpu::g2 {

// Host-side input preflight only. A matching PTX entry substring is not a
// cryptographic authorization for submitting arbitrary device code.
inline std::string read_bounded_ptx(std::istream& input) {
    constexpr std::size_t max_ptx_bytes = 512 * 1024;
    std::string ptx;
    char chunk[4096];
    while (input) {
        input.read(chunk, sizeof(chunk));
        const auto received = static_cast<std::size_t>(input.gcount());
        if (received > max_ptx_bytes - ptx.size())
            throw std::invalid_argument("GPU-G2-004: PTX exceeds bounded source size");
        ptx.append(chunk, received);
    }
    if (input.bad() || (input.fail() && !input.eof()) || ptx.empty() ||
        ptx.find('\0') != std::string::npos)
        throw std::invalid_argument("GPU-G2-004: invalid or empty PTX input");
    if (ptx.find(".entry saxpy") == std::string::npos)
        throw std::invalid_argument("GPU-G2-004: expected SAXPY entry missing");
    return ptx;
}

} // namespace oreslang::gpu::g2
