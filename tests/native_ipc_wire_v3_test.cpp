#include "oreslang/runtime/NativeIpcWireV3.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace oreslang::runtime;

static std::vector<unsigned char> hex(const std::string& text) {
    if (text.size() % 2u != 0u)
        throw std::invalid_argument("odd hex length");
    auto digit = [](char ch) -> unsigned char {
        if (ch >= '0' && ch <= '9')
            return static_cast<unsigned char>(ch - '0');
        if (ch >= 'a' && ch <= 'f')
            return static_cast<unsigned char>(10 + ch - 'a');
        throw std::invalid_argument("invalid hex");
    };
    std::vector<unsigned char> out;
    out.reserve(text.size() / 2u);
    for (std::size_t i = 0; i < text.size(); i += 2u)
        out.push_back(static_cast<unsigned char>(
            (digit(text[i]) << 4u) | digit(text[i + 1u])));
    return out;
}

template<class F>
static void rejects(F&& callback) {
    try {
        callback();
    } catch (const std::invalid_argument&) {
        return;
    }
    assert(false && "protocol-v3 invalid input accepted");
}

static IpcReceiptEnvelope golden_envelope() {
    IpcReceiptEnvelope envelope;
    envelope.domain = "tenant";
    envelope.sender = "sender";
    envelope.destination = "worker";
    envelope.session = "v3golden";
    envelope.payload_sha256 =
        "0005f12a6ca8277ba63364ad9a8a21e43bedea6f209373e27b662fcb7a93b11a";
    envelope.sender_incarnation = 1;
    envelope.destination_incarnation = 2;
    envelope.route_epoch = 3;
    envelope.capability_generation = 4;
    envelope.message_sequence = 5;
    envelope.frame_sequence = 6;
    return envelope;
}

static NativeIpcWireV3::Bytes golden_key() {
    NativeIpcWireV3::Bytes key(32u);
    for (std::size_t i = 0; i < key.size(); ++i)
        key[i] = static_cast<unsigned char>(0x20u + i);
    return key;
}

static NativeIpcWireV3::ConnectionBinding golden_binding() {
    NativeIpcWireV3::ConnectionBinding binding{};
    for (std::size_t i = 0; i < binding.size(); ++i)
        binding[i] = static_cast<unsigned char>(i + 1u);
    return binding;
}

int main() {
    const auto key = golden_key();
    const auto binding = golden_binding();
    const auto envelope = golden_envelope();
    const NativeIpcWireV3::Bytes payload{
        'h','e','l','l','o','-','v','3'};

    const auto expected_data = hex(
        "4f524553030100000000000000070102030405060708090a0b0c0d0e0f10"
        "0674656e616e740673656e64657206776f726b6572087633676f6c64656e40"
        "3030303566313261366361383237376261363333363461643961386132316534"
        "3362656465613666323039333733653237623636326663623761393362313161"
        "0000000000000001000000000000000200000000000000030000000000000004"
        "000000000000000500000000000000060000000868656c6c6f2d7633"
        "f155c8abe25e56699de661eeb6df46bbc65e026f4a3d42cd449553adcac7ab60");
    const auto expected_receipt = hex(
        "4f524553030200000000000000070102030405060708090a0b0c0d0e0f10"
        "0674656e616e740673656e64657206776f726b6572087633676f6c64656e40"
        "3030303566313261366361383237376261363333363461643961386132316534"
        "3362656465613666323039333733653237623636326663623761393362313161"
        "0000000000000001000000000000000200000000000000030000000000000004"
        "0000000000000005000000000000000601"
        "c928561e7dd8ed11e10c09aaa07012ab052b1d88d5c5c6362bd8f3161553a840");
    const auto valid_mac_invalid_receipt = hex(
        "4f524553030200000000000000070102030405060708090a0b0c0d0e0f10"
        "0674656e616e740673656e64657206776f726b6572087633676f6c64656e40"
        "3030303566313261366361383237376261363333363461643961386132316534"
        "3362656465613666323039333733653237623636326663623761393362313161"
        "0000000000000001000000000000000200000000000000030000000000000004"
        "0000000000000005000000000000000604"
        "158c14bef448a43530fa4bde51315abe967d9ddb4e99de1cbfac1f7dc7f68388");

    const auto encoded_data = NativeIpcWireV3::encode_data_frame(
        envelope, payload, 7, binding, key);
    assert(encoded_data == expected_data);
    const auto encoded_receipt = NativeIpcWireV3::encode_receipt_frame(
        envelope, 7, IpcReceiptStage::remote_accepted, binding, key);
    assert(encoded_receipt == expected_receipt);

    // Java permits HMAC keys up to 128 bytes. Exercise the RFC HMAC key
    // reduction path (> SHA-256's 64-byte block) against an independent vector.
    NativeIpcWireV3::Bytes long_key(128u);
    for (std::size_t i = 0; i < long_key.size(); ++i)
        long_key[i] = static_cast<unsigned char>(i);
    const auto long_key_data = NativeIpcWireV3::encode_data_frame(
        envelope, payload, 7, binding, long_key);
    assert(long_key_data.size() == expected_data.size());
    assert(std::equal(
        long_key_data.end() - static_cast<std::ptrdiff_t>(NativeIpcWireV3::mac_bytes),
        long_key_data.end(),
        hex("11c9a96a57a3d1d5fabcd891eb745d1c248a38904e592445fd50fee13ce9bad4").begin()));

    NativeIpcWireV3::InboundReplayGate replay;
    const auto decoded_data = NativeIpcWireV3::decode_data_frame(
        expected_data, envelope, 7, replay, binding, key);
    assert(decoded_data.envelope == envelope);
    assert(decoded_data.physical_generation == 7);
    assert(decoded_data.payload == payload);
    assert(replay.last() ==
        (std::pair<std::uint64_t, std::uint64_t>{5, 6}));

    const auto decoded_receipt = NativeIpcWireV3::decode_receipt_frame(
        expected_receipt, binding, key);
    assert(decoded_receipt.envelope == envelope);
    assert(decoded_receipt.physical_generation == 7);
    assert(decoded_receipt.stage == IpcReceiptStage::remote_accepted);

    rejects([&] {
        NativeIpcWireV3::encode_receipt_frame(
            envelope, 7, IpcReceiptStage::outcome_unknown, binding, key);
    });
    rejects([&] {
        NativeIpcWireV3::encode_receipt_frame(
            envelope, 7, IpcReceiptStage::rejected_before_remote, binding, key);
    });
    rejects([&] {
        auto zero_binding = binding;
        zero_binding.fill(0);
        NativeIpcWireV3::encode_data_frame(
            envelope, payload, 7, zero_binding, key);
    });
    rejects([&] {
        NativeIpcWireV3::encode_data_frame(
            envelope, payload,
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max()) + 1u,
            binding, key);
    });

    {
        auto tampered = expected_data;
        tampered.back() ^= 0x01u;
        NativeIpcWireV3::InboundReplayGate gate;
        rejects([&] {
            NativeIpcWireV3::decode_data_frame(
                tampered, envelope, 7, gate, binding, key);
        });
    }
    {
        auto stale_binding = binding;
        stale_binding[0] ^= 0x40u;
        NativeIpcWireV3::InboundReplayGate gate;
        rejects([&] {
            NativeIpcWireV3::decode_data_frame(
                expected_data, envelope, 7, gate, stale_binding, key);
        });
    }
    {
        NativeIpcWireV3::InboundReplayGate gate;
        (void)NativeIpcWireV3::decode_data_frame(
            expected_data, envelope, 7, gate, binding, key);
        rejects([&] {
            NativeIpcWireV3::decode_data_frame(
                expected_data, envelope, 7, gate, binding, key);
        });
    }
    {
        auto wrong_epoch = envelope;
        ++wrong_epoch.route_epoch;
        NativeIpcWireV3::InboundReplayGate gate;
        rejects([&] {
            NativeIpcWireV3::decode_data_frame(
                expected_data, wrong_epoch, 7, gate, binding, key);
        });
    }
    {
        NativeIpcWireV3::InboundReplayGate gate;
        rejects([&] {
            NativeIpcWireV3::decode_data_frame(
                expected_data, envelope, 8, gate, binding, key);
        });
    }
    {
        // Authentication alone never grants authority to an unknown receipt
        // code. This vector has a valid HMAC for wire code 4.
        rejects([&] {
            NativeIpcWireV3::decode_receipt_frame(
                valid_mac_invalid_receipt, binding, key);
        });
    }

    assert(NativeIpcWireV3::sha256_hex(payload) == envelope.payload_sha256);
    std::cout << "native-ipc-wire-v3 PASS java-golden parity\n";
}
