// C++ host-native boundary oracle. NOT compiled Atto guest execution.
// Identical test body used in Forge and LLVM; no endianness / UB host fallback.
#if __has_include("atto/byte_buffer_v1.hpp")
#include "atto/byte_buffer_v1.hpp"
#else
#include "../src/main/cpp/oreslang/runtime/CheckedByteAccess.hpp"
#endif
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>

using atto::byte_buffer_v1::Code;
using atto::byte_buffer_v1::Endian;
using atto::byte_buffer_v1::read;
using atto::byte_buffer_v1::write;

int main() {
  std::size_t checks = 0;
  for (auto endian : {Endian::little, Endian::big}) {
    std::array<std::uint8_t, 8> b{};
    assert(write(b.data(), b.size(), 0, 4, 0x12345678, endian) == Code::ok);
    ++checks;
    assert(read(b.data(), b.size(), 0, 4, endian).value == 0x12345678);
    ++checks;
    assert(b[4] == 0 && b[5] == 0 && b[6] == 0 && b[7] == 0);
    ++checks;
    if (endian == Endian::little) {
      assert(b[0] == 0x78 && b[1] == 0x56 && b[2] == 0x34 && b[3] == 0x12);
    } else {
      assert(b[0] == 0x12 && b[1] == 0x34 && b[2] == 0x56 && b[3] == 0x78);
    }
    ++checks;
    const auto before = b;
    for (std::int64_t off : std::array<std::int64_t, 4>{-1, 5, std::numeric_limits<std::int64_t>::max(),
                            std::numeric_limits<std::int64_t>::min()}) {
      assert(write(b.data(), b.size(), off, 4, 42, endian) == Code::bounds);
      assert(read(b.data(), b.size(), off, 4, endian).code == Code::bounds);
      assert(b == before);
      checks += 3;
    }
    for (std::int64_t value : std::array<std::int64_t, 3>{-1, INT64_C(4294967296),
                               std::numeric_limits<std::int64_t>::max()}) {
      assert(write(b.data(), b.size(), 0, 4, value, endian) == Code::value);
      assert(b == before);
      checks += 2;
    }
    for (std::size_t width : {1u, 2u, 4u}) {
      for (std::int64_t off = 0; off <= static_cast<std::int64_t>(8 - width); ++off) {
        std::array<std::uint8_t, 8> x{};
        const std::int64_t value = (INT64_C(1) << (8 * width)) - 1;
        assert(write(x.data(), x.size(), off, width, value, endian) == Code::ok);
        const auto observed = read(x.data(), x.size(), off, width, endian);
        assert(observed.code == Code::ok && observed.value == static_cast<std::uint64_t>(value));
        checks += 2;
      }
    }
  }
  assert(read(nullptr, 0, 0, 1, Endian::little).code == Code::bounds);
  assert(read(nullptr, 8, 0, 1, Endian::little).code == Code::storage);
  assert(read(nullptr, 8, 0, 8, Endian::little).code == Code::value);
  checks += 3;
  std::uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
  std::array<std::uint8_t, 8> bytes{};
  for (int trial = 0; trial < 100000; ++trial) {
    state ^= state << 7;
    state ^= state >> 9;
    state ^= state << 8;
    const std::int64_t value = static_cast<std::int64_t>(state & UINT64_C(0xffffffff));
    const Endian order = (state & 1) ? Endian::little : Endian::big;
    assert(write(bytes.data(), bytes.size(), 1, 4, value, order) == Code::ok);
    const auto out = read(bytes.data(), bytes.size(), 1, 4, order);
    assert(out.code == Code::ok && out.value == static_cast<std::uint64_t>(value));
    checks += 2;
  }
  std::cout << "ATTO_NATIVE_BYTES_HOST_PASS checks=" << checks
            << " compiled_atto_guest=NO\n";
}
