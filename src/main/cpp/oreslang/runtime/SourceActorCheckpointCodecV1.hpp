#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace oreslang::runtime {

/**
 * Native parity for Java's compiler-private ORESAS01 source-actor scalar
 * state payload. This is a serializer only; ActorCheckpointEnvelopeV1 owns
 * the outer identity/ABI receipts and SHA-256 corruption trailer.
 */
class SourceActorCheckpointCodecV1 final {
public:
    enum class Kind : std::uint8_t { Int = 1, Bool = 2, String = 3 };
    struct Field {
        std::string name;
        Kind kind;
    };
    using Value = std::variant<std::int64_t, bool, std::string>;
    static constexpr std::size_t MAX_BYTES = 1U << 20;

private:
    static void require(bool ok, const char* message) {
        if (!ok) throw std::invalid_argument(message);
    }

    static void u16(std::vector<std::uint8_t>& out, std::size_t value) {
        require(value <= 0xffff, "source actor field name too long");
        out.push_back(static_cast<std::uint8_t>(value >> 8));
        out.push_back(static_cast<std::uint8_t>(value));
    }

    static void u32(std::vector<std::uint8_t>& out, std::size_t value) {
        require(value <= std::numeric_limits<std::uint32_t>::max(),
                "source actor string too long");
        for (int shift = 24; shift >= 0; shift -= 8)
            out.push_back(static_cast<std::uint8_t>(value >> shift));
    }

    static void i64(std::vector<std::uint8_t>& out, std::int64_t value) {
        std::uint64_t bits = static_cast<std::uint64_t>(value);
        for (int shift = 56; shift >= 0; shift -= 8)
            out.push_back(static_cast<std::uint8_t>(bits >> shift));
    }

    static std::uint16_t read16(
            const std::vector<std::uint8_t>& in, std::size_t& pos) {
        require(pos + 2 <= in.size(), "truncated source actor u16");
        std::uint16_t value =
                (static_cast<std::uint16_t>(in[pos]) << 8) | in[pos + 1];
        pos += 2;
        return value;
    }

    static std::uint32_t read32(
            const std::vector<std::uint8_t>& in, std::size_t& pos) {
        require(pos + 4 <= in.size(), "truncated source actor u32");
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) value = (value << 8) | in[pos++];
        return value;
    }

    static std::int64_t read64(
            const std::vector<std::uint8_t>& in, std::size_t& pos) {
        require(pos + 8 <= in.size(), "truncated source actor int64");
        std::uint64_t bits = 0;
        for (int i = 0; i < 8; ++i) bits = (bits << 8) | in[pos++];
        return static_cast<std::int64_t>(bits);
    }

    static bool valid_utf8(const std::string& text) noexcept {
        const auto* p = reinterpret_cast<const unsigned char*>(text.data());
        std::size_t i = 0;
        while (i < text.size()) {
            unsigned char c = p[i++];
            if (c <= 0x7f) continue;
            unsigned need = 0;
            std::uint32_t code = 0;
            if (c >= 0xc2 && c <= 0xdf) {
                need = 1; code = c & 0x1f;
            } else if (c >= 0xe0 && c <= 0xef) {
                need = 2; code = c & 0x0f;
            } else if (c >= 0xf0 && c <= 0xf4) {
                need = 3; code = c & 0x07;
            } else return false;
            if (i + need > text.size()) return false;
            for (unsigned n = 0; n < need; ++n) {
                unsigned char tail = p[i++];
                if ((tail & 0xc0) != 0x80) return false;
                code = (code << 6) | (tail & 0x3f);
            }
            if ((need == 2 && code < 0x800)
                    || (need == 3 && code < 0x10000)
                    || (code >= 0xd800 && code <= 0xdfff)
                    || code > 0x10ffff)
                return false;
        }
        return true;
    }

public:
    static std::vector<std::uint8_t> encode(
            const std::vector<Field>& fields,
            const std::vector<Value>& values) {
        require(fields.size() == values.size(),
                "source actor field/value count mismatch");
        std::vector<std::uint8_t> out{
            'O','R','E','S','A','S','0','1'};
        u32(out, fields.size());
        for (std::size_t i = 0; i < fields.size(); ++i) {
            const auto& field = fields[i];
            require(!field.name.empty(), "blank source actor field name");
            require(valid_utf8(field.name), "source actor field name is not UTF-8");
            u16(out, field.name.size());
            out.insert(out.end(), field.name.begin(), field.name.end());
            out.push_back(static_cast<std::uint8_t>(field.kind));
            switch (field.kind) {
                case Kind::Int:
                    require(std::holds_alternative<std::int64_t>(values[i]),
                            "source actor int value type mismatch");
                    i64(out, std::get<std::int64_t>(values[i]));
                    break;
                case Kind::Bool:
                    require(std::holds_alternative<bool>(values[i]),
                            "source actor bool value type mismatch");
                    out.push_back(std::get<bool>(values[i]) ? 1 : 0);
                    break;
                case Kind::String: {
                    require(std::holds_alternative<std::string>(values[i]),
                            "source actor string value type mismatch");
                    const auto& text = std::get<std::string>(values[i]);
                    require(valid_utf8(text), "source actor string is not UTF-8");
                    u32(out, text.size());
                    out.insert(out.end(), text.begin(), text.end());
                    break;
                }
                default:
                    throw std::invalid_argument("unsupported source actor scalar kind");
            }
            require(out.size() <= MAX_BYTES,
                    "source actor checkpoint exceeds 1 MiB");
        }
        return out;
    }

    static std::vector<Value> decode(
            const std::vector<Field>& fields,
            const std::vector<std::uint8_t>& bytes) {
        require(bytes.size() >= 12 && bytes.size() <= MAX_BYTES,
                "invalid source actor checkpoint size");
        const std::uint8_t magic[8] = {'O','R','E','S','A','S','0','1'};
        for (std::size_t i = 0; i < 8; ++i)
            require(bytes[i] == magic[i], "source actor checkpoint magic mismatch");
        std::size_t pos = 8;
        require(read32(bytes, pos) == fields.size(),
                "source actor field count mismatch");
        std::vector<Value> values;
        values.reserve(fields.size());
        for (const auto& field : fields) {
            const std::size_t name_size = read16(bytes, pos);
            require(pos + name_size <= bytes.size(),
                    "truncated source actor field name");
            std::string name(
                    reinterpret_cast<const char*>(bytes.data() + pos),
                    name_size);
            pos += name_size;
            require(valid_utf8(name), "source actor field name is not UTF-8");
            require(name == field.name,
                    "source actor field order/name mismatch");
            require(pos < bytes.size(), "missing source actor type tag");
            const auto tag = static_cast<Kind>(bytes[pos++]);
            require(tag == field.kind, "source actor type tag mismatch");
            switch (field.kind) {
                case Kind::Int:
                    values.emplace_back(read64(bytes, pos));
                    break;
                case Kind::Bool: {
                    require(pos < bytes.size(), "truncated source actor bool");
                    const auto bit = bytes[pos++];
                    require(bit <= 1, "noncanonical source actor bool");
                    values.emplace_back(bit == 1);
                    break;
                }
                case Kind::String: {
                    const std::size_t length = read32(bytes, pos);
                    require(pos + length <= bytes.size(),
                            "truncated source actor string");
                    std::string text(
                            reinterpret_cast<const char*>(bytes.data() + pos),
                            length);
                    pos += length;
                    require(valid_utf8(text), "source actor string is not UTF-8");
                    values.emplace_back(std::move(text));
                    break;
                }
                default:
                    throw std::invalid_argument("unsupported source actor scalar kind");
            }
        }
        require(pos == bytes.size(),
                "trailing bytes in source actor checkpoint");
        return values;
    }
};

} // namespace oreslang::runtime
