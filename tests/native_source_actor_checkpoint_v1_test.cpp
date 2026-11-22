#include "oreslang/runtime/SourceActorCheckpointCodecV1.hpp"

#include <openssl/evp.h>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using Codec = oreslang::runtime::SourceActorCheckpointCodecV1;

static void check(bool ok) {
    if (!ok) throw std::runtime_error("source actor checkpoint parity failed");
}
template<class F> static bool rejects(F&& f) {
    try { f(); } catch (const std::invalid_argument&) { return true; }
    return false;
}
static std::string sha256(const std::vector<std::uint8_t>& bytes) {
    std::array<unsigned char, 32> out{};
    unsigned int written = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), out.data(), &written,
                   EVP_sha256(), nullptr) != 1 || written != out.size())
        throw std::runtime_error("SHA-256 failed");
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (auto byte : out) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 15]);
    }
    return result;
}

int main() {
    const std::vector<Codec::Field> fields{
        {"count", Codec::Kind::Int},
        {"ready", Codec::Kind::Bool},
        {"name", Codec::Kind::String}};
    const std::vector<Codec::Value> values{
        std::int64_t{42}, true, std::string{"worker-\xCF\x80"}};
    const auto golden = Codec::encode(fields, values);
    check(golden.size() == 57);
    check(sha256(golden)
        == "eab52d5bea5c2b873c925edf9f4d1905cded85daac3ca0f6b920b97da22d5b40");
    check(Codec::decode(fields, golden) == values);

    for (std::int64_t value : {
            std::numeric_limits<std::int64_t>::min(),
            std::int64_t{-1}, std::int64_t{0}, std::int64_t{1},
            std::numeric_limits<std::int64_t>::max()}) {
        std::vector<Codec::Value> current{
            value, value >= 0, std::string{"state"}};
        check(Codec::decode(fields, Codec::encode(fields, current)) == current);
    }

    for (std::size_t cut = 0; cut < golden.size(); ++cut) {
        std::vector<std::uint8_t> truncated(golden.begin(), golden.begin() + cut);
        check(rejects([&] { (void) Codec::decode(fields, truncated); }));
    }
    auto trailing = golden;
    trailing.push_back(0);
    check(rejects([&] { (void) Codec::decode(fields, trailing); }));

    auto bad_bool = golden;
    bad_bool[36] = 2;
    check(rejects([&] { (void) Codec::decode(fields, bad_bool); }));

    auto bad_utf8 = golden;
    bad_utf8[48] = 0xff;
    check(rejects([&] { (void) Codec::decode(fields, bad_utf8); }));

    check(rejects([&] {
        (void) Codec::encode(fields,
            {std::string{"not-int"}, true, std::string{"worker"}});
    }));
    return 0;
}
