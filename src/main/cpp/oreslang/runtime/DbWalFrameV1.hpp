#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace oreslang::runtime {

// Pure host-only ODB1 v1 structural/checksum validation. This is neither
// tenant authentication nor a durable commit receipt. Validate the immutable
// detached frame before authoritative shard admission/physical dispatch.
class DbWalFrameV1 final {
public:
    static void require_matching(const std::uint8_t* frame, std::size_t size,
            std::uint32_t tenant, std::uint32_t generation,
            std::uint32_t operation_id) {
        if (!frame || size < 31 || size > 1118 || tenant == 0
                || generation == 0 || operation_id == 0
                || tenant > 0x7fffffffU || generation > 0x7fffffffU
                || operation_id > 0x7fffffffU)
            throw std::invalid_argument("WAL frame/caller bounds invalid");
        if (frame[0] != 79 || frame[1] != 68 || frame[2] != 66
                || frame[3] != 49 || frame[4] != 1
                || (frame[5] != 1 && frame[5] != 2))
            throw std::invalid_argument("WAL magic, version or kind");
        if ((frame[9] & 128U) || (frame[13] & 128U)
                || (frame[17] & 128U) || (frame[21] & 128U))
            throw std::invalid_argument("WAL signed ID overflow");
        const auto actual_tenant = u32(frame, 6);
        const auto actual_generation = u32(frame, 10);
        const auto sequence = u32(frame, 14);
        const auto actual_operation = u32(frame, 18);
        const auto key_size = u16(frame, 22);
        const auto value_size = u16(frame, 24);
        if (actual_tenant == 0 || actual_generation == 0 || sequence == 0
                || actual_operation == 0 || key_size == 0 || key_size > 64
                || value_size > 1024 || (frame[5] == 2 && value_size != 0)
                || size != 26U + key_size + value_size + 4U)
            throw std::invalid_argument("WAL header/length mismatch");
        std::uint32_t a = 1, b = 0;
        for (std::size_t i = 0; i < size - 4; ++i) {
            a = (a + frame[i]) % 65521U;
            b = (b + a) % 65521U;
        }
        if (u16(frame, size - 4) != a || u16(frame, size - 2) != b)
            throw std::invalid_argument("WAL checksum mismatch");
        if (actual_tenant != tenant || actual_generation != generation
                || actual_operation != operation_id)
            throw std::invalid_argument("WAL frame does not match owner grant");
    }

private:
    static std::uint32_t u16(const std::uint8_t* p, std::size_t at) noexcept {
        return std::uint32_t(p[at]) | (std::uint32_t(p[at + 1]) << 8);
    }
    static std::uint32_t u32(const std::uint8_t* p, std::size_t at) noexcept {
        return u16(p, at) | (u16(p, at + 2) << 16);
    }
};
} // namespace oreslang::runtime
