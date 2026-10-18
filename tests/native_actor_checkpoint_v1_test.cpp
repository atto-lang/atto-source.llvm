#include "oreslang/runtime/ActorCheckpointEnvelopeV1.hpp"
#include <openssl/evp.h>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using Wire = oreslang::runtime::ActorCheckpointEnvelopeV1;
static Wire::Hash sha(const std::uint8_t* p, std::size_t n) {
    Wire::Hash hash{};
    unsigned int written = 0;
    if (EVP_Digest(p, n, hash.data(), &written, EVP_sha256(), nullptr) != 1
            || written != hash.size())
        throw std::runtime_error("digest failed");
    return hash;
}
static void check(bool b) { if (!b) throw std::runtime_error("checkpoint test failed"); }
template<class F> static bool rejects(F f) {
    try { f(); } catch (const std::invalid_argument&) { return true; }
    return false;
}
int main() {
    Wire::Header h{};
    h.actor_id = {0x12, 0x34, 0x56, 0x78, 0x12, 0x34, 0x43, 0x21,
                  0x81, 0x23, 0, 0, 0, 0, 0, 1};
    h.incarnation = 42;
    h.code_digest.fill(0xaa);
    h.state_schema_digest.fill(0xbb);
    h.message_abi_digest.fill(0xcc);
    h.capability_digest.fill(0xdd);
    auto original = Wire::encode({h, {0x0a, 0xff}}, sha);
    check(original.size() == 202);
    const auto digest = sha(original.data(), original.size());
    static constexpr char hex[] = "0123456789abcdef";
    std::string value;
    for (auto c : digest) { value += hex[c >> 4]; value += hex[c & 15]; }
    check(value == "c054268972740370c1be5ff20ceeda21edd17282a69692203acca15b4e6c1793");
    check(Wire::decode(original, h, sha).state_bytes
            == std::vector<std::uint8_t>({0x0a, 0xff}));
    for (std::size_t i = 0; i < original.size(); ++i) {
        auto damaged = original;
        damaged[i] ^= 1;
        check(rejects([&] { (void) Wire::decode(damaged, h, sha); }));
    }
    auto wrong = h;
    wrong.incarnation = 43;
    check(rejects([&] { (void) Wire::decode(original, wrong, sha); }));
    wrong = h;
    wrong.capability_digest[0] ^= 1;
    check(rejects([&] { (void) Wire::decode(original, wrong, sha); }));
    wrong = h;
    wrong.actor_id[0] ^= 1;
    check(rejects([&] { (void) Wire::decode(original, wrong, sha); }));
    wrong = h;
    wrong.code_digest[0] ^= 1;
    check(rejects([&] { (void) Wire::decode(original, wrong, sha); }));
    wrong = h;
    wrong.state_schema_digest[0] ^= 1;
    check(rejects([&] { (void) Wire::decode(original, wrong, sha); }));
    wrong = h;
    wrong.message_abi_digest[0] ^= 1;
    check(rejects([&] { (void) Wire::decode(original, wrong, sha); }));
    for (std::size_t bytes = 0; bytes < original.size(); ++bytes) {
        auto cut = std::vector<std::uint8_t>(original.begin(),
                                             original.begin() + bytes);
        check(rejects([&] { (void) Wire::decode(cut, h, sha); }));
    }
    auto trailing = original;
    trailing.push_back(0);
    check(rejects([&] { (void) Wire::decode(trailing, h, sha); }));
    auto zero_epoch = h;
    zero_epoch.incarnation = 0;
    check(rejects([&] { (void) Wire::encode({zero_epoch, {}}, sha); }));
    auto truncated = original;
    truncated.pop_back();
    check(rejects([&] { (void) Wire::decode(truncated, h, sha); }));
    check(rejects([&] { (void) Wire::decode(original, h, nullptr); }));
    std::mt19937 engine(0xC0DEC0DE);
    for (int i = 0; i != 1024; ++i) {
        std::vector<std::uint8_t> data(static_cast<std::size_t>(engine() % 512));
        for (auto& byte : data) byte = static_cast<std::uint8_t>(engine());
        auto wire = Wire::encode({h, data}, sha);
        check(Wire::decode(wire, h, sha).state_bytes == data);
    }
    std::vector<std::uint8_t> max_state(Wire::MAX_STATE_BYTES, 0x5a);
    auto largest = Wire::encode({h, max_state}, sha);
    check(Wire::decode(largest, h, sha).state_bytes == max_state);
    check(rejects([&] {
        (void) Wire::encode({h, std::vector<std::uint8_t>(Wire::MAX_STATE_BYTES + 1)},
                            sha);
    }));
}
