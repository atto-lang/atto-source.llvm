#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <stdexcept>

namespace oreslang::actor_codec_policy_v1 {
// Host-only metadata policy. Not a decoder, permission or mailbox gate.
enum class Reject { UnknownCodec, InvalidMask, Denied, Unsupported, InvalidBinding, Mismatch };
struct PolicyError final : std::runtime_error {
    const Reject reason;
    explicit PolicyError(Reject r) : std::runtime_error("actor DATA codec policy reject"), reason(r) {}
};
enum class Codec : unsigned { MessagePack=1, Protobuf=2, Cbor=4, FlatBuffers=8 };
using Shape = std::array<std::uint8_t,32>;
inline std::optional<Codec> parse(std::string_view label) noexcept {
    if (label == "messagepack") return Codec::MessagePack;
    if (label == "protobuf") return Codec::Protobuf;
    if (label == "cbor") return Codec::Cbor;
    if (label == "flatbuffers") return Codec::FlatBuffers;
    return std::nullopt;
}
inline bool valid_schema(std::string_view schema) noexcept {
    if (schema.empty() || schema.size() > 256) return false;
    for (unsigned char c : schema) if (c < 33 || c > 126) return false;
    return true;
}
class Binding final {
    const Codec codec_;
    const std::uint32_t version_;
    const std::string schema_;
    const Shape shape_;
    Binding(Codec c, std::uint32_t v, std::string_view s, const Shape& h)
        : codec_(c), version_(v), schema_(s), shape_(h) {}
public:
    // Only trusted registration may call select; support mask is not proof.
    static Binding select(std::optional<std::string_view> requested, unsigned allowed,
                          unsigned supported, std::uint64_t version,
                          std::string_view schema, const Shape& shape) {
        if ((allowed | supported) & ~15u) throw PolicyError(Reject::InvalidMask);
        const auto codec = parse(requested.value_or("messagepack"));
        if (!codec) throw PolicyError(Reject::UnknownCodec);
        const auto bit = static_cast<unsigned>(*codec);
        if (!(allowed & bit)) throw PolicyError(Reject::Denied);
        if (!(supported & bit)) throw PolicyError(Reject::Unsupported);
        if (version == 0 || version > 0xffffffffULL || !valid_schema(schema))
            throw PolicyError(Reject::InvalidBinding);
        return Binding(*codec, static_cast<std::uint32_t>(version), schema, shape);
    }
    // Must be called after authenticated metadata admission; never dispatch on incoming label.
    void require_match(std::string_view label, std::uint64_t version,
                       std::string_view schema, const Shape& shape) const {
        if (parse(label) != codec_ || version != version_ || !valid_schema(schema)
            || schema != std::string_view(schema_) || shape != shape_)
            throw PolicyError(Reject::Mismatch);
    }
};
}
