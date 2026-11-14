#pragma once
// Native LLVM backend source descriptor validation. Host-only metadata floor:
// no compiler probe insertion, no JIT/ORC, and no runtime OTel span allocation.
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace oreslang::telemetry {

enum class SourceKind { Routine, Site };
struct SourceId {
  SourceKind kind = SourceKind::Routine;
  std::array<std::uint8_t, 16> bytes{};
  bool operator==(const SourceId &other) const noexcept {
    return kind == other.kind && bytes == other.bytes;
  }
};
struct StaticSite {
  SourceId routine_id;
  SourceId site_id;
  std::uint32_t descriptor_index = 0;
};

inline int lower_hex(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + c - 'a';
  return -1;
}

// Parse UUID-like source IDs only, never a W3C runtime trace_id or span_id.
// Out is unchanged on malformed input.
inline bool parse_source_id(std::string_view value, SourceKind kind,
                            SourceId &out) noexcept {
  if (kind != SourceKind::Routine && kind != SourceKind::Site) return false;
  const std::string_view prefix = kind == SourceKind::Routine ? "rid_" : "sid_";
  if (value.size() != 36 || value.substr(0, 4) != prefix) return false;
  SourceId candidate{};
  candidate.kind = kind;
  bool nonzero = false;
  for (std::size_t i = 0; i < 16; ++i) {
    const int hi = lower_hex(value[4 + i * 2]);
    const int lo = lower_hex(value[5 + i * 2]);
    if (hi < 0 || lo < 0) return false;
    candidate.bytes[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    nonzero = nonzero || candidate.bytes[i] != 0;
  }
  if (!nonzero) return false;
  out = candidate;
  return true;
}

// Cold-path admission of a bounded, already-resolved static descriptor table.
// Called before publication to native runtime; caller owns the array lifetime.
// This is intentionally independent of LLVM IR, Cranelift and Truffle ASTs.
inline bool validate_static_sites(const StaticSite *sites,
                                  std::size_t count) noexcept {
  if (count > 4096 || (sites == nullptr && count != 0)) return false;
  const SourceId zero{};
  for (std::size_t i = 0; i < count; ++i) {
    if (sites[i].routine_id.bytes == zero.bytes ||
        sites[i].site_id.bytes == zero.bytes ||
        sites[i].routine_id.kind != SourceKind::Routine ||
        sites[i].site_id.kind != SourceKind::Site) return false;
    for (std::size_t j = 0; j < i; ++j) {
      if (sites[i].site_id == sites[j].site_id ||
          sites[i].descriptor_index == sites[j].descriptor_index) return false;
    }
  }
  return true;
}

}  // namespace oreslang::telemetry
