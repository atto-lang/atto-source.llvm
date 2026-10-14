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
    std::cout << "Symbol-key v1 host-only conformance: OK\n";
}
