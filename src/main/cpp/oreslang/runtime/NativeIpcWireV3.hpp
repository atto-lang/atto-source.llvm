#pragma once

#include "oreslang/runtime/NativeIpcReceiptTable.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::runtime {

class NativeIpcWireV3 final {
public:
    using Byte = unsigned char;
    using Bytes = std::vector<Byte>;
    using ConnectionBinding = std::array<Byte, 16>;

    static constexpr std::uint32_t magic = 0x4f524553u;
    static constexpr Byte version = 3;
    static constexpr Byte type_data = 1;
    static constexpr Byte type_receipt = 2;
    static constexpr Byte receipt_remote_accepted = 1;
    static constexpr Byte receipt_handler_committed = 2;
    static constexpr Byte receipt_handler_failed = 3;
    static constexpr std::size_t mac_bytes = 32;
    static constexpr std::size_t max_payload_bytes = 8u * 1024u * 1024u;
    static constexpr std::size_t max_data_frame_bytes = max_payload_bytes + 1024u;
    static constexpr std::size_t max_receipt_frame_bytes = 1024u;

    struct DecodedData {
        IpcReceiptEnvelope envelope;
        std::uint64_t physical_generation;
        Bytes payload;
    };

    struct DecodedReceipt {
        IpcReceiptEnvelope envelope;
        std::uint64_t physical_generation;
        IpcReceiptStage stage;
    };

    class InboundReplayGate final {
    public:
        void accept(const IpcReceiptEnvelope& envelope) {
            validate_ipc_receipt_envelope(envelope);
            std::lock_guard<std::mutex> guard(mutex_);
            if (envelope.message_sequence <= last_message_sequence_ ||
                envelope.frame_sequence <= last_frame_sequence_)
                throw std::invalid_argument(
                    "duplicate/reordered authenticated IPC DATA sequence");
            last_message_sequence_ = envelope.message_sequence;
            last_frame_sequence_ = envelope.frame_sequence;
        }

        std::pair<std::uint64_t, std::uint64_t> last() const {
            std::lock_guard<std::mutex> guard(mutex_);
            return {last_message_sequence_, last_frame_sequence_};
        }

    private:
        mutable std::mutex mutex_;
        std::uint64_t last_message_sequence_ = 0;
        std::uint64_t last_frame_sequence_ = 0;
    };

    static Bytes encode_data_frame(
            const IpcReceiptEnvelope& envelope,
            const Bytes& payload,
            std::uint64_t physical_generation,
            const ConnectionBinding& binding,
            const Bytes& key) {
        validate_common(envelope, physical_generation, binding, key);
        if (payload.size() > max_payload_bytes)
            throw std::invalid_argument("IPC payload too large");
        if (sha256_hex(payload) != envelope.payload_sha256)
            throw std::invalid_argument("IPC payload digest mismatch");

        Writer out;
        out.reserve(payload.size() + 256u);
        write_header(out, type_data, physical_generation, binding);
        write_envelope(out, envelope);
        out.u32(static_cast<std::uint32_t>(payload.size()));
        out.raw(payload.data(), payload.size());
        return append_mac(std::move(out.bytes), key, max_data_frame_bytes);
    }

    static Bytes encode_receipt_frame(
            const IpcReceiptEnvelope& envelope,
            std::uint64_t physical_generation,
            IpcReceiptStage stage,
            const ConnectionBinding& binding,
            const Bytes& key) {
        validate_common(envelope, physical_generation, binding, key);
        const Byte code = receipt_code(stage);
        Writer out;
        out.reserve(256u);
        write_header(out, type_receipt, physical_generation, binding);
        write_envelope(out, envelope);
        out.u8(code);
        return append_mac(std::move(out.bytes), key, max_receipt_frame_bytes);
    }

    static DecodedData decode_data_frame(
            const Bytes& frame,
            const IpcReceiptEnvelope& expected_epoch,
            std::uint64_t expected_physical_generation,
            InboundReplayGate& replay_gate,
            const ConnectionBinding& expected_binding,
            const Bytes& key) {
        validate_common(
            expected_epoch, expected_physical_generation, expected_binding, key);
        const std::size_t body_size =
            verify_mac_and_body_size(frame, key, max_data_frame_bytes);
        Reader in(frame.data(), body_size);
        const auto physical_generation =
            read_header(in, type_data, expected_binding);
        auto envelope = read_envelope(in);
        const auto payload_size = static_cast<std::size_t>(in.u32());
        if (payload_size > max_payload_bytes || payload_size > in.remaining())
            throw std::invalid_argument("invalid bounded IPC payload length");
        Bytes payload = in.bytes(payload_size);
        if (in.remaining() != 0)
            throw std::invalid_argument("trailing IPC DATA frame");
        if (sha256_hex(payload) != envelope.payload_sha256)
            throw std::invalid_argument("authenticated IPC payload digest mismatch");
        if (physical_generation != expected_physical_generation ||
            !same_ipc_transport_epoch(expected_epoch, envelope))
            throw std::invalid_argument(
                "stale IPC DATA transport epoch/generation");
        replay_gate.accept(envelope);
        return {std::move(envelope), physical_generation, std::move(payload)};
    }

    static DecodedReceipt decode_receipt_frame(
            const Bytes& frame,
            const ConnectionBinding& expected_binding,
            const Bytes& key) {
        validate_binding(expected_binding);
        validate_key(key);
        const std::size_t body_size =
            verify_mac_and_body_size(frame, key, max_receipt_frame_bytes);
        Reader in(frame.data(), body_size);
        const auto physical_generation =
            read_header(in, type_receipt, expected_binding);
        auto envelope = read_envelope(in);
        const auto stage = receipt_stage(in.u8());
        if (in.remaining() != 0)
            throw std::invalid_argument("invalid IPC receipt frame length");
        return {std::move(envelope), physical_generation, stage};
    }

    static std::string sha256_hex(const Bytes& bytes) {
        const auto hash = sha256(bytes.data(), bytes.size());
        static constexpr char digits[] = "0123456789abcdef";
        std::string out;
        out.resize(hash.size() * 2u);
        for (std::size_t i = 0; i < hash.size(); ++i) {
            out[i * 2u] = digits[(hash[i] >> 4u) & 0x0fu];
            out[i * 2u + 1u] = digits[hash[i] & 0x0fu];
        }
        return out;
    }

private:
    using Digest = std::array<Byte, 32>;

    template<std::size_t Size>
    class SensitiveArray final {
    public:
        std::array<Byte, Size> bytes{};
        SensitiveArray() = default;
        SensitiveArray(const SensitiveArray&) = delete;
        SensitiveArray& operator=(const SensitiveArray&) = delete;
        ~SensitiveArray() {
            OPENSSL_cleanse(bytes.data(), bytes.size());
        }
    };

    class Writer final {
    public:
        Bytes bytes;

        void reserve(std::size_t n) {
            if (n > max_data_frame_bytes)
                throw std::invalid_argument("IPC frame reserve exceeds bound");
            bytes.reserve(n);
        }

        void u8(Byte value) { append(&value, 1u); }

        void u32(std::uint32_t value) {
            std::array<Byte, 4> raw{};
            for (int i = 3; i >= 0; --i) {
                raw[static_cast<std::size_t>(3 - i)] =
                    static_cast<Byte>((value >> (i * 8)) & 0xffu);
            }
            append(raw.data(), raw.size());
        }

        void u64(std::uint64_t value) {
            std::array<Byte, 8> raw{};
            for (int i = 7; i >= 0; --i) {
                raw[static_cast<std::size_t>(7 - i)] =
                    static_cast<Byte>((value >> (i * 8)) & 0xffu);
            }
            append(raw.data(), raw.size());
        }

        void token(const std::string& value) {
            if (!ipc_receipt_token(value))
                throw std::invalid_argument("invalid IPC token");
            u8(static_cast<Byte>(value.size()));
            raw(reinterpret_cast<const Byte*>(value.data()), value.size());
        }

        void raw(const Byte* data, std::size_t size) { append(data, size); }

    private:
        void append(const Byte* data, std::size_t size) {
            if (size > max_data_frame_bytes - bytes.size())
                throw std::invalid_argument("IPC frame exceeds byte budget");
            if (size != 0u)
                bytes.insert(bytes.end(), data, data + size);
        }
    };

    class Reader final {
    public:
        Reader(const Byte* data, std::size_t size)
            : current_(data), end_(data + size) { }

        std::size_t remaining() const {
            return static_cast<std::size_t>(end_ - current_);
        }

        Byte u8() {
            require(1u);
            return *current_++;
        }

        std::uint32_t u32() {
            require(4u);
            std::uint32_t value = 0;
            for (int i = 0; i < 4; ++i)
                value = (value << 8u) | *current_++;
            return value;
        }

        std::uint64_t u64() {
            require(8u);
            std::uint64_t value = 0;
            for (int i = 0; i < 8; ++i)
                value = (value << 8u) | *current_++;
            return value;
        }

        std::string token() {
            const std::size_t size = u8();
            if (size == 0u || size > 96u)
                throw std::invalid_argument("invalid IPC token length");
            require(size);
            std::string value(
                reinterpret_cast<const char*>(current_), size);
            current_ += size;
            if (!ipc_receipt_token(value))
                throw std::invalid_argument("noncanonical IPC token");
            return value;
        }

        Bytes bytes(std::size_t size) {
            require(size);
            Bytes result(current_, current_ + size);
            current_ += size;
            return result;
        }

    private:
        void require(std::size_t size) const {
            if (size > remaining())
                throw std::invalid_argument("truncated IPC frame");
        }

        const Byte* current_;
        const Byte* end_;
    };

    struct MdCtxDeleter {
        void operator()(EVP_MD_CTX* value) const noexcept {
            EVP_MD_CTX_free(value);
        }
    };

    static Digest sha256(const Byte* data, std::size_t size) {
        std::unique_ptr<EVP_MD_CTX, MdCtxDeleter> context(EVP_MD_CTX_new());
        if (!context)
            throw std::runtime_error("SHA-256 context allocation failed");
        Digest out{};
        unsigned int length = 0;
        if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
            (size != 0u &&
             EVP_DigestUpdate(context.get(), data, size) != 1) ||
            EVP_DigestFinal_ex(context.get(), out.data(), &length) != 1 ||
            length != static_cast<unsigned int>(out.size()))
            throw std::runtime_error("SHA-256 unavailable");
        return out;
    }

    static Digest sha256_two(
            const Byte* first, std::size_t first_size,
            const Byte* second, std::size_t second_size) {
        std::unique_ptr<EVP_MD_CTX, MdCtxDeleter> context(EVP_MD_CTX_new());
        if (!context)
            throw std::runtime_error("SHA-256 context allocation failed");
        Digest out{};
        unsigned int length = 0;
        if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
            (first_size != 0u &&
             EVP_DigestUpdate(context.get(), first, first_size) != 1) ||
            (second_size != 0u &&
             EVP_DigestUpdate(context.get(), second, second_size) != 1) ||
            EVP_DigestFinal_ex(context.get(), out.data(), &length) != 1 ||
            length != static_cast<unsigned int>(out.size()))
            throw std::runtime_error("SHA-256 unavailable");
        return out;
    }

    static Digest hmac_sha256(
            const Byte* body, std::size_t body_size, const Bytes& key) {
        validate_key(key);
        SensitiveArray<64> key_block;
        if (key.size() > key_block.bytes.size()) {
            SensitiveArray<32> reduced;
            reduced.bytes = sha256(key.data(), key.size());
            std::copy(
                reduced.bytes.begin(), reduced.bytes.end(),
                key_block.bytes.begin());
        } else {
            std::copy(key.begin(), key.end(), key_block.bytes.begin());
        }
        SensitiveArray<64> inner_pad;
        SensitiveArray<64> outer_pad;
        for (std::size_t i = 0; i < key_block.bytes.size(); ++i) {
            inner_pad.bytes[i] =
                static_cast<Byte>(key_block.bytes[i] ^ 0x36u);
            outer_pad.bytes[i] =
                static_cast<Byte>(key_block.bytes[i] ^ 0x5cu);
        }
        SensitiveArray<32> inner;
        inner.bytes = sha256_two(
            inner_pad.bytes.data(), inner_pad.bytes.size(), body, body_size);
        return sha256_two(
            outer_pad.bytes.data(), outer_pad.bytes.size(),
            inner.bytes.data(), inner.bytes.size());
    }

    static void validate_key(const Bytes& key) {
        if (key.size() < 32u || key.size() > 128u)
            throw std::invalid_argument("IPC MAC key must be 32..128 bytes");
    }

    static void validate_binding(const ConnectionBinding& binding) {
        bool nonzero = false;
        for (Byte value : binding)
            nonzero = nonzero || value != 0u;
        if (!nonzero)
            throw std::invalid_argument(
                "IPC connection binding must not be all zero");
    }

    static void validate_common(
            const IpcReceiptEnvelope& envelope,
            std::uint64_t physical_generation,
            const ConnectionBinding& binding,
            const Bytes& key) {
        validate_ipc_receipt_envelope(envelope);
        if (!ipc_positive_java_long(physical_generation))
            throw std::invalid_argument(
                "physical generation must be positive Java long");
        validate_binding(binding);
        validate_key(key);
    }

    static void write_header(
            Writer& out, Byte type, std::uint64_t physical_generation,
            const ConnectionBinding& binding) {
        if (!ipc_positive_java_long(physical_generation))
            throw std::invalid_argument(
                "physical generation must be positive Java long");
        validate_binding(binding);
        out.u32(magic);
        out.u8(version);
        out.u8(type);
        out.u64(physical_generation);
        out.raw(binding.data(), binding.size());
    }

    static void write_envelope(
            Writer& out, const IpcReceiptEnvelope& envelope) {
        validate_ipc_receipt_envelope(envelope);
        out.token(envelope.domain);
        out.token(envelope.sender);
        out.token(envelope.destination);
        out.token(envelope.session);
        out.token(envelope.payload_sha256);
        out.u64(envelope.sender_incarnation);
        out.u64(envelope.destination_incarnation);
        out.u64(envelope.route_epoch);
        out.u64(envelope.capability_generation);
        out.u64(envelope.message_sequence);
        out.u64(envelope.frame_sequence);
    }

    static std::uint64_t read_header(
            Reader& in, Byte expected_type,
            const ConnectionBinding& expected_binding) {
        if (in.u32() != magic || in.u8() != version ||
            in.u8() != expected_type)
            throw std::invalid_argument("invalid IPC frame header");
        const auto generation = in.u64();
        if (!ipc_positive_java_long(generation))
            throw std::invalid_argument(
                "invalid IPC physical transport generation");
        ConnectionBinding actual{};
        for (auto& value : actual)
            value = in.u8();
        if (CRYPTO_memcmp(
                actual.data(), expected_binding.data(), actual.size()) != 0)
            throw std::invalid_argument("stale IPC connection binding");
        return generation;
    }

    static IpcReceiptEnvelope read_envelope(Reader& in) {
        IpcReceiptEnvelope envelope;
        envelope.domain = in.token();
        envelope.sender = in.token();
        envelope.destination = in.token();
        envelope.session = in.token();
        envelope.payload_sha256 = in.token();
        envelope.sender_incarnation = in.u64();
        envelope.destination_incarnation = in.u64();
        envelope.route_epoch = in.u64();
        envelope.capability_generation = in.u64();
        envelope.message_sequence = in.u64();
        envelope.frame_sequence = in.u64();
        validate_ipc_receipt_envelope(envelope);
        return envelope;
    }

    static Byte receipt_code(IpcReceiptStage stage) {
        switch (stage) {
            case IpcReceiptStage::remote_accepted:
                return receipt_remote_accepted;
            case IpcReceiptStage::handler_committed:
                return receipt_handler_committed;
            case IpcReceiptStage::handler_failed:
                return receipt_handler_failed;
            default:
                throw std::invalid_argument(
                    "local-only/invalid peer receipt stage");
        }
    }

    static IpcReceiptStage receipt_stage(Byte code) {
        switch (code) {
            case receipt_remote_accepted:
                return IpcReceiptStage::remote_accepted;
            case receipt_handler_committed:
                return IpcReceiptStage::handler_committed;
            case receipt_handler_failed:
                return IpcReceiptStage::handler_failed;
            default:
                throw std::invalid_argument(
                    "peer asserted invalid IPC receipt code");
        }
    }

    static Bytes append_mac(
            Bytes body, const Bytes& key, std::size_t maximum) {
        if (body.size() > maximum - mac_bytes)
            throw std::invalid_argument("IPC authenticated frame too large");
        const auto tag = hmac_sha256(body.data(), body.size(), key);
        body.insert(body.end(), tag.begin(), tag.end());
        return body;
    }

    static std::size_t verify_mac_and_body_size(
            const Bytes& frame, const Bytes& key, std::size_t maximum) {
        validate_key(key);
        if (frame.size() < mac_bytes || frame.size() > maximum)
            throw std::invalid_argument(
                "invalid IPC authenticated frame length");
        const std::size_t body_size = frame.size() - mac_bytes;
        const auto expected =
            hmac_sha256(frame.data(), body_size, key);
        if (CRYPTO_memcmp(
                expected.data(), frame.data() + body_size, mac_bytes) != 0)
            throw std::invalid_argument("IPC frame authentication failed");
        return body_size;
    }
};

}  // namespace oreslang::runtime
