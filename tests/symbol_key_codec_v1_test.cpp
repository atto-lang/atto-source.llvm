#include "oreslang/runtime/SymbolKeyCodecV1.hpp"
#include <iostream>
#include <stdexcept>

using namespace oreslang::runtime::symbol_key_v1;

void require(bool condition) {
    if (!condition) throw std::runtime_error("Symbol-key v1 assertion failed");
}
template <typename F> void rejects(F&& f) {
    bool rejected = false;
    try { f(); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected);
}

template <typename F> void denies(F&& f) {
    bool denied = false;
    try { f(); } catch (const AdmissionDenied&) { denied = true; }
    require(denied);
}

int main() {
    const auto text = encode({Kind::Text, "foo"});
    const auto process = encode({Kind::Process, "foo"});
    require(text == std::vector<std::uint8_t>({'O', 'K', 1, 0, 0, 3, 'f', 'o', 'o'}));
    require(process == std::vector<std::uint8_t>({'O', 'K', 1, 1, 0, 3, 'f', 'o', 'o'}));
    require(decode(text) == Key{Kind::Text, "foo"});
    require(decode(process) == Key{Kind::Process, "foo"});
    require(decode(text) != decode(process));
    const std::string uuid = "550e8400-e29b-41d4-a716-446655440000";
    require(decode(encode({Kind::Stable, uuid})) == Key{Kind::Stable, uuid});
    require(decodeDistinct({encode({Kind::Text, "ready"}),
                           encode({Kind::Process, "ready"})}).size() == 2);
    rejects([&] { decodeDistinct({process, process}); });
    require(decodeForUntrusted(encode({Kind::Text, "#private"}))
            == Key{Kind::Text, "#private"});
    require(decodeForUntrusted(encode({Kind::Process, "ready"}))
            == Key{Kind::Process, "ready"});
    require(decodeForUntrusted(encode({Kind::Process, "error"}))
            == Key{Kind::Process, "error"});
    require(decodeForUntrusted(encode({Kind::Process, "Symbol.iterator"}))
            == Key{Kind::Process, "Symbol.iterator"});
    denies([&] { decodeForUntrusted(encode({Kind::Process, "tenant/private"})); });
    denies([&] { decodeForUntrusted(encode({Kind::Stable, uuid})); });
    require(decodeDistinctForUntrusted({
            encode({Kind::Text, "ready"}),
            encode({Kind::Process, "ready"})}).size() == 2);
    denies([&] { decodeDistinctForUntrusted({
            encode({Kind::Text, "ready"}),
            encode({Kind::Process, "tenant/private"})}); });
    rejects([&] { decodeDistinctForUntrusted({
            encode({Kind::Text, "x"}), encode({Kind::Text, "x"})}); });
    rejects([&] { encode({static_cast<Kind>(0xff), "arbitrary"}); });
    auto invalidUntrustedWire = process;
    invalidUntrustedWire[2] = 99;
    // Malformed wire is an argument error, NOT an authorization denial.
    rejects([&] { decodeForUntrusted(invalidUntrustedWire); });
    require(decodeForUntrusted(encode({Kind::Text, "tenant/private"}))
            == Key{Kind::Text, "tenant/private"});
    rejects([&] { encode({Kind::Process, ""}); });
    rejects([&] { encode({Kind::Process, "\x1b"}); });
    rejects([&] { encode({Kind::Process, "\xc3\xa9"}); }); // NFC not yet available
    rejects([&] { encode({Kind::Stable, "550E8400-e29b-41d4-a716-446655440000"}); });
    rejects([&] { encode({Kind::Text, "\xc0\x80"}); }); // overlong UTF-8
    rejects([&] { encode({Kind::Text, "\xed\xa0\x80"}); }); // surrogate UTF-8
    rejects([&] { encode({Kind::Text, std::string(16'385, 'x')}); });
    auto badVersion = process;
    badVersion[2] = 2;
    rejects([&] { decode(badVersion); });
    auto badLen = process;
    badLen[5] = 2;
    rejects([&] { decode(badLen); });
    auto badKind = process;
    badKind[3] = 7;
    rejects([&] { decode(badKind); });
    std::vector<std::vector<std::uint8_t>> tooMany;
    tooMany.assign(kMaxKeys + 1, text);
    rejects([&] { decodeDistinct(tooMany); });
    std::vector<std::vector<std::uint8_t>> oversized;
    for (int n = 0; n < 70; ++n) {
        oversized.push_back(encode({Kind::Text, std::string(16'000, 'x')
            + std::to_string(n)}));
    }
    rejects([&] { decodeDistinct(oversized); });
    // Exact aggregate boundary: 63 maximum-size text keys are admitted;
    // 64 would exceed the inclusive 1 MiB budget even though the entry
    // count is well below 4,096.
    std::vector<std::vector<std::uint8_t>> boundary;
    for (int i = 0; i < 64; ++i) {
        std::string name(16'384, 'x');
        const std::string serial = std::to_string(i);
        name.replace(name.size() - serial.size(), serial.size(), serial);
        boundary.push_back(encode({Kind::Text, name}));
    }
    auto allowedBoundary = std::vector<std::vector<std::uint8_t>>(
            boundary.begin(), boundary.end() - 1);
    require(decodeDistinct(allowedBoundary).size() == 63);
    rejects([&] { decodeDistinct(boundary); });
    require(encode({Kind::Process, std::string(256, 'x')}).size() == 262);
    rejects([&] { encode({Kind::Process, std::string(257, 'x')}); });
    // Ordinary Strings retain non-ASCII UTF-8 and embedded NUL identity:
    // these are not PROCESS Symbol names and must not be normalized.
    const std::string face = "\xf0\x9f\x98\x80";
    require(decode(encode({Kind::Text, face})) == Key{Kind::Text, face});
    require(decode(encode({Kind::Text, std::string("a\0b", 3)}))
            == Key{Kind::Text, std::string("a\0b", 3)});
    rejects([&] { encode({Kind::Text, "\xf4\x90\x80\x80"}); });
    rejects([&] { encode({Kind::Text, "\xe2\x82"}); });

    std::cout << "Symbol-key v1 host-only conformance: OK\n";
}
