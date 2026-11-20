#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace oreslang {

// Standalone, bounded actor DATA-only wire profile for
// Option<Result<int,string>>. Not a guest compiler or CONTROL lane.
// The runtime-owned supervisor ledger authenticates CONTROL separately.
class NativeActorAdtDataWireV1 final {
public:
    struct None { bool operator==(None) const { return true; } };
    struct SomeOk {
        std::int64_t value;
        bool operator==(const SomeOk& o) const { return value == o.value; }
    };
    struct SomeErr {
        std::string value;
        bool operator==(const SomeErr& o) const { return value == o.value; }
    };
    using Value = std::variant<None, SomeOk, SomeErr>;
    static constexpr std::size_t header_size = 7;
    static constexpr std::size_t max_error_bytes = 256;
    static constexpr std::size_t max_wire_bytes = header_size + 2 + max_error_bytes;

    static bool valid_utf8(std::string_view text) noexcept {
        std::size_t i = 0;
        const auto byte = [&](std::size_t pos) -> unsigned {
            return static_cast<unsigned char>(text[pos]);
        };
        const auto continuation = [](unsigned x) { return x >= 0x80 && x <= 0xbf; };
        while (i < text.size()) {
            const unsigned b = byte(i++);
            if (b <= 0x7f) continue;
            if (b >= 0xc2 && b <= 0xdf) {
                if (text.size() - i < 1 || !continuation(byte(i))) return false;
                ++i;
            } else if (b >= 0xe0 && b <= 0xef) {
                if (text.size() - i < 2) return false;
                const unsigned a = byte(i), c = byte(i+1);
                if (!(continuation(a) && continuation(c))) return false;
                if (b == 0xe0 && a < 0xa0) return false; // overlong
                if (b == 0xed && a > 0x9f) return false; // surrogate
                i += 2;
            } else if (b >= 0xf0 && b <= 0xf4) {
                if (text.size() - i < 3) return false;
                const unsigned a = byte(i), c = byte(i+1), d = byte(i+2);
                if (!(continuation(a) && continuation(c) && continuation(d))) return false;
                if (b == 0xf0 && a < 0x90) return false; // overlong
                if (b == 0xf4 && a > 0x8f) return false; // > U+10ffff
                i += 3;
            } else {
                return false;
            }
        }
        return true;
    }

    static std::vector<std::uint8_t> encode(const Value& value) {
        std::vector<std::uint8_t> output{'A', 'T', 'A', 'D', 1, 1, 0};
        if (std::holds_alternative<None>(value)) return output;
        if (const auto* ok = std::get_if<SomeOk>(&value)) {
            output[6] = 1;
            const auto n = static_cast<std::uint64_t>(ok->value);
            for (unsigned i = 0; i < 8; ++i)
                output.push_back(static_cast<std::uint8_t>(n >> (i * 8)));
            return output;
        }
        const auto& error = std::get<SomeErr>(value).value;
        if (error.size() > max_error_bytes || !valid_utf8(error))
            throw std::invalid_argument("invalid or unbounded ADT Err UTF-8");
        output[6] = 2;
        output.push_back(static_cast<std::uint8_t>(error.size()));
        output.push_back(static_cast<std::uint8_t>(error.size() >> 8));
        output.insert(output.end(), error.begin(), error.end());
        return output;
    }

    static Value decode(const std::vector<std::uint8_t>& wire) {
        if (wire.size() < header_size || wire.size() > max_wire_bytes ||
            wire[0] != 'A' || wire[1] != 'T' || wire[2] != 'A' ||
            wire[3] != 'D' || wire[4] != 1 || wire[5] != 1)
            throw std::invalid_argument("invalid ADT magic, version, family or size");
        switch (wire[6]) {
            case 0:
                if (wire.size() != header_size)
                    throw std::invalid_argument("None contains data");
                return None{};
            case 1: {
                if (wire.size() != header_size + 8)
                    throw std::invalid_argument("Ok payload length");
                std::uint64_t bits = 0;
                for (unsigned i = 0; i < 8; ++i)
                    bits |= std::uint64_t(wire[header_size+i]) << (i*8);
                // The wire is the exact two's-complement i64 bit pattern.
                const auto value = bits <= INT64_MAX
                        ? static_cast<std::int64_t>(bits)
                        : -1 - static_cast<std::int64_t>(~bits);
                return SomeOk{value};
            }
            case 2: {
                if (wire.size() < header_size + 2)
                    throw std::invalid_argument("Err length missing");
                const std::size_t n = std::size_t(wire[7]) | (std::size_t(wire[8]) << 8);
                if (n > max_error_bytes || wire.size() != header_size + 2 + n)
                    throw std::invalid_argument("Err length invalid");
                const std::string_view view(
                    reinterpret_cast<const char*>(wire.data() + header_size + 2), n);
                if (!valid_utf8(view)) throw std::invalid_argument("invalid Err UTF-8");
                return SomeErr{std::string(view)};
            }
            default:
                // In-band DATA must never mint a supervisor CONTROL notice.
                throw std::invalid_argument("unknown ADT variant / forbidden CONTROL tag");
        }
    }
};
} // namespace oreslang
