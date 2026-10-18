#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace oreslang::runtime {

/**
 * HOST-ONLY checkpoint wire frame, identical to the Java/Python v1 contract.
 * This framing library supplies NO hashing algorithm or code authorization:
 * a trusted host must inject a real SHA-256 implementation. The unkeyed hash
 * detects corruption, NOT adversarial fabrication or artifact authenticity.
 * Input state bytes must already be canonical and pointer/handle-free.
 */
class ActorCheckpointEnvelopeV1 final {
public:
    using Hash = std::array<std::uint8_t, 32>;
    using ActorId = std::array<std::uint8_t, 16>;
    using Digest = Hash;
    using Sha256 = Hash (*)(const std::uint8_t*, std::size_t);
    static constexpr std::size_t MAX_STATE_BYTES = 1U << 20;
    static constexpr std::size_t HEADER_BYTES = 168;
    static constexpr std::size_t HASH_BYTES = 32;

    struct Header {
        ActorId actor_id;
        std::uint64_t incarnation;
        Digest code_digest;
        Digest state_schema_digest;
        Digest message_abi_digest;
        Digest capability_digest;
    };
    struct Snapshot {
        Header header;
        std::vector<std::uint8_t> state_bytes;
    };

private:
    static void require(bool good, const char* message) {
        if (!good) throw std::invalid_argument(message);
    }
    static void put32(std::vector<std::uint8_t>& out, std::uint32_t n) {
        for (int i = 3; i >= 0; --i)
            out.push_back(static_cast<std::uint8_t>(n >> (8 * i)));
    }
    static void put64(std::vector<std::uint8_t>& out, std::uint64_t n) {
        for (int i = 7; i >= 0; --i)
            out.push_back(static_cast<std::uint8_t>(n >> (8 * i)));
    }
    static std::uint32_t get32(const std::vector<std::uint8_t>& v, std::size_t start) {
        std::uint32_t n = 0;
        for (std::size_t i = 0; i < 4; ++i) n = (n << 8) | v[start + i];
        return n;
    }
    static std::uint64_t get64(const std::vector<std::uint8_t>& v, std::size_t start) {
        std::uint64_t n = 0;
        for (std::size_t i = 0; i < 8; ++i) n = (n << 8) | v[start + i];
        return n;
    }
    template<std::size_t N>
    static void append(std::vector<std::uint8_t>& v,
                       const std::array<std::uint8_t, N>& a) {
        v.insert(v.end(), a.begin(), a.end());
    }
    template<std::size_t N>
    static std::array<std::uint8_t, N> extract(
            const std::vector<std::uint8_t>& v, std::size_t start) {
        std::array<std::uint8_t, N> a{};
        for (std::size_t i = 0; i < N; ++i) a[i] = v[start + i];
        return a;
    }
    template<std::size_t N>
    static bool equal(const std::array<std::uint8_t, N>& a,
                      const std::array<std::uint8_t, N>& b) noexcept {
        unsigned difference = 0;
        for (std::size_t i = 0; i < N; ++i) difference |= a[i] ^ b[i];
        return difference == 0;
    }
    static void validate_header(const Header& h) {
        // Java ByteBuffer.getLong is signed; reject native-only epochs with
        // the high bit set so both backends admit exactly the same wire space.
        require(h.incarnation != 0
                && h.incarnation <= static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()),
                "actor checkpoint incarnation must fit positive int64");
    }

public:
    static std::vector<std::uint8_t> encode(const Snapshot& snapshot, Sha256 sha256) {
        require(sha256 != nullptr, "SHA-256 provider required");
        validate_header(snapshot.header);
        require(snapshot.state_bytes.size() <= MAX_STATE_BYTES,
                "actor state exceeds checkpoint byte limit");
        const auto& h = snapshot.header;
        std::vector<std::uint8_t> out;
        out.reserve(HEADER_BYTES + snapshot.state_bytes.size() + HASH_BYTES);
        for (std::uint8_t byte : std::array<std::uint8_t, 8>{
                'O', 'R', 'E', 'S', 'C', 'P', '0', '1'}) out.push_back(byte);
        put32(out, 1);
        append(out, h.actor_id);
        put64(out, h.incarnation);
        append(out, h.code_digest);
        append(out, h.state_schema_digest);
        append(out, h.message_abi_digest);
        append(out, h.capability_digest);
        put32(out, static_cast<std::uint32_t>(snapshot.state_bytes.size()));
        require(out.size() == HEADER_BYTES, "checkpoint header layout drift");
        out.insert(out.end(), snapshot.state_bytes.begin(), snapshot.state_bytes.end());
        const auto checksum = sha256(out.data(), out.size());
        append(out, checksum);
        return out;
    }

    static Snapshot decode(const std::vector<std::uint8_t>& frame,
                           const Header& expected, Sha256 sha256) {
        require(sha256 != nullptr, "SHA-256 provider required");
        validate_header(expected);
        require(frame.size() >= HEADER_BYTES + HASH_BYTES
                && frame.size() <= HEADER_BYTES + MAX_STATE_BYTES + HASH_BYTES,
                "invalid actor checkpoint frame size");
        const std::array<std::uint8_t, 8> magic{
            'O', 'R', 'E', 'S', 'C', 'P', '0', '1'};
        for (std::size_t i = 0; i < magic.size(); ++i)
            require(frame[i] == magic[i], "checkpoint magic mismatch");
        require(get32(frame, 8) == 1, "checkpoint version mismatch");
        Header observed{
            extract<16>(frame, 12),
            get64(frame, 28),
            extract<32>(frame, 36),
            extract<32>(frame, 68),
            extract<32>(frame, 100),
            extract<32>(frame, 132)};
        validate_header(observed);
        const std::uint32_t size = get32(frame, 164);
        require(size <= MAX_STATE_BYTES
                && static_cast<std::size_t>(size) + HEADER_BYTES + HASH_BYTES
                    == frame.size(), "checkpoint state length mismatch");
        require(equal(observed.actor_id, expected.actor_id)
                && observed.incarnation == expected.incarnation
                && equal(observed.code_digest, expected.code_digest)
                && equal(observed.state_schema_digest, expected.state_schema_digest)
                && equal(observed.message_abi_digest, expected.message_abi_digest)
                && equal(observed.capability_digest, expected.capability_digest),
                "checkpoint foreign actor, epoch, code or ABI");
        const auto computed = sha256(frame.data(), HEADER_BYTES + size);
        const auto checksum = extract<32>(frame, HEADER_BYTES + size);
        require(equal(checksum, computed), "checkpoint checksum mismatch");
        return Snapshot{observed,
                        std::vector<std::uint8_t>(frame.begin() + HEADER_BYTES,
                            frame.begin() + HEADER_BYTES + size)};
    }
};

} // namespace oreslang::runtime
