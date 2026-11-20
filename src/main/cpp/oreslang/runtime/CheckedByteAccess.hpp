#pragma once
// Trusted native compiler/runtime primitive only. Not an Atto guest Buffer
// and not a tenant authority/allocator. Caller must supply a validated,
// exclusive live storage span and enforce actor ownership and quotas.
#include <cstddef>
#include <cstdint>

namespace atto::byte_buffer_v1 {
enum class Code { ok, bounds, value, storage };
enum class Endian { little, big };
struct ReadResult {
  Code code;
  std::uint64_t value;
};

inline Code validate(const std::uint8_t* ptr, std::size_t length,
                     std::int64_t offset, std::size_t width) noexcept {
  if (width != 1 && width != 2 && width != 4) return Code::value;
  if (offset < 0 || width > length ||
      static_cast<std::uint64_t>(offset) > static_cast<std::uint64_t>(length - width))
    return Code::bounds;
  if (ptr == nullptr) return Code::storage;
  return Code::ok;
}

inline ReadResult read(const std::uint8_t* ptr, std::size_t length,
                       std::int64_t offset, std::size_t width, Endian endian) noexcept {
  const auto code = validate(ptr, length, offset, width);
  if (code != Code::ok) return {code, 0};
  if (endian != Endian::little && endian != Endian::big) return {Code::value, 0};
  const auto at = static_cast<std::size_t>(offset);
  std::uint64_t result = 0;
  for (std::size_t i = 0; i < width; ++i) {
    const auto shift = 8 * (endian == Endian::little ? i : width - i - 1);
    result |= std::uint64_t{ptr[at + i]} << shift;
  }
  return {Code::ok, result};
}

inline Code write(std::uint8_t* ptr, std::size_t length, std::int64_t offset,
                  std::size_t width, std::int64_t value, Endian endian) noexcept {
  const auto code = validate(ptr, length, offset, width);
  if (code != Code::ok) return code;
  if (endian != Endian::little && endian != Endian::big) return Code::value;
  const auto max = (std::uint64_t{1} << (8 * width)) - 1;
  if (value < 0 || static_cast<std::uint64_t>(value) > max) return Code::value;
  const auto at = static_cast<std::size_t>(offset);
  const auto bits = static_cast<std::uint64_t>(value);
  // After complete validation all stores are non-throwing and non-allocating.
  for (std::size_t i = 0; i < width; ++i) {
    const auto shift = 8 * (endian == Endian::little ? i : width - i - 1);
    ptr[at + i] = static_cast<std::uint8_t>((bits >> shift) & 255);
  }
  return Code::ok;
}
}  // namespace atto::byte_buffer_v1
