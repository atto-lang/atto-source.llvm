#include "oreslang/runtime/DbWalFrameV1.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <vector>

using oreslang::runtime::DbWalFrameV1;

static void u16(std::vector<std::uint8_t>& f, std::size_t p, std::uint32_t x) {
    f[p] = static_cast<std::uint8_t>(x);
    f[p + 1] = static_cast<std::uint8_t>(x >> 8);
}
static void u32(std::vector<std::uint8_t>& f, std::size_t p, std::uint32_t x) {
    u16(f, p, x); u16(f, p + 2, x >> 16);
}
static void checksum(std::vector<std::uint8_t>& f) {
    std::uint32_t a = 1, b = 0;
    for (std::size_t i = 0; i < f.size() - 4; ++i) {
        a = (a + f[i]) % 65521U;
        b = (b + a) % 65521U;
    }
    u16(f, f.size() - 4, a); u16(f, f.size() - 2, b);
}
static std::vector<std::uint8_t> make_frame(
        std::uint32_t tenant, std::uint32_t generation,
        std::uint32_t sequence, std::uint32_t op,
        std::uint32_t key_size, std::uint32_t value_size,
        std::uint8_t kind = 1) {
    std::vector<std::uint8_t> frame(26 + key_size + value_size + 4, 0x5a);
    frame[0] = 79; frame[1] = 68; frame[2] = 66; frame[3] = 49;
    frame[4] = 1; frame[5] = kind;
    u32(frame, 6, tenant); u32(frame, 10, generation);
    u32(frame, 14, sequence); u32(frame, 18, op);
    u16(frame, 22, key_size); u16(frame, 24, value_size);
    checksum(frame);
    return frame;
}
template <typename F> static void rejects(F f) {
    bool denied = false;
    try { f(); } catch (const std::invalid_argument&) { denied = true; }
    assert(denied);
}

int main() {
    auto frame = make_frame(7, 4, 2, 99, 1, 0);
    DbWalFrameV1::require_matching(frame.data(), frame.size(), 7, 4, 99);
    auto big = make_frame(7, 4, 2147483647U, 2147483647U, 64, 1024);
    DbWalFrameV1::require_matching(big.data(), big.size(), 7, 4, 2147483647U);
    auto del = make_frame(1, 1, 1, 1, 1, 0, 2);
    DbWalFrameV1::require_matching(del.data(), del.size(), 1, 1, 1);
    rejects([&] { DbWalFrameV1::require_matching(frame.data(), frame.size(), 8, 4, 99); });
    rejects([&] { DbWalFrameV1::require_matching(frame.data(), frame.size(), 7, 5, 99); });
    rejects([&] { DbWalFrameV1::require_matching(frame.data(), frame.size(), 7, 4, 100); });
    for (std::size_t i = 0; i < frame.size(); ++i) {
        auto corrupt = frame;
        corrupt[i] ^= 1;
        rejects([&] {
            DbWalFrameV1::require_matching(corrupt.data(), corrupt.size(), 7, 4, 99);
        });
    }
    auto zero = frame; u32(zero, 14, 0); checksum(zero);
    rejects([&] { DbWalFrameV1::require_matching(zero.data(), zero.size(), 7, 4, 99); });
    auto illegal_delete = make_frame(7, 4, 2, 99, 1, 3, 2);
    rejects([&] {
        DbWalFrameV1::require_matching(illegal_delete.data(), illegal_delete.size(), 7, 4, 99);
    });
    return 0;
}
