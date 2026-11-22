#include "oreslang/runtime/NativeActorAdtDataWireV1.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using Wire = oreslang::NativeActorAdtDataWireV1;
using Bytes = std::vector<std::uint8_t>;

static Bytes hex(const std::string& text) {
    if (text.size()%2) throw std::logic_error("odd hex");
    Bytes result;
    const auto nibble = [](char c) -> unsigned {
        if (c>='0' && c<='9') return static_cast<unsigned>(c-'0');
        if (c>='a' && c<='f') return static_cast<unsigned>(c-'a'+10);
        throw std::logic_error("bad hex fixture");
    };
    for (std::size_t i=0; i<text.size(); i+=2)
        result.push_back(static_cast<std::uint8_t>((nibble(text[i]) << 4) | nibble(text[i+1])));
    return result;
}
static void rejects(const Bytes& bytes) {
    try { (void) Wire::decode(bytes); }
    catch (const std::invalid_argument&) { return; }
    throw std::logic_error("accepted malformed DATA or forged CONTROL");
}
static void exact(const Wire::Value& value, const char* wire) {
    const auto encoded = Wire::encode(value);
    if (encoded != hex(wire) || !(Wire::decode(encoded) == value))
        throw std::logic_error("native ADT wire vector mismatch");
}
int main() {
    exact(Wire::None{}, "41544144010100");
    exact(Wire::SomeOk{42}, "415441440101012a00000000000000");
    exact(Wire::SomeOk{-1}, "41544144010101ffffffffffffffff");
    exact(Wire::SomeOk{INT64_MIN}, "415441440101010000000000000080");
    exact(Wire::SomeErr{""}, "415441440101020000");
    exact(Wire::SomeErr{u8"é"}, "415441440101020200c3a9");
    std::mt19937_64 rng(20261116);
    for (int i = 0; i < 256; ++i) {
        const Wire::Value value = Wire::SomeOk{static_cast<std::int64_t>(rng())};
        if (!(Wire::decode(Wire::encode(value)) == value))
            throw std::logic_error("random signed i64 payload drift");
    }
    for (int i = 0; i <= 256; i += 2) {
        const Wire::Value value = Wire::SomeErr{std::string(i, 'x')};
        if (!(Wire::decode(Wire::encode(value)) == value))
            throw std::logic_error("bounded Err length drift");
    }
    const auto good = hex("415441440101020200c3a9");
    for (std::size_t i=0; i<good.size(); ++i)
        rejects(Bytes(good.begin(), good.begin()+static_cast<std::ptrdiff_t>(i)));
    for (std::size_t i=0; i<7; ++i) {
        auto broken = good;
        broken[i] = i == 6 ? 127 : (broken[i] ^ 1);
        rejects(broken);
    }
    rejects(hex("41544144010100ff"));
    rejects(hex("4154414401010100000000000000"));
    rejects(hex("415441440101020300c3a9"));
    rejects(hex("415441440101020200c328"));
    auto oversized = Bytes(266, 0);
    std::copy(good.begin(), good.begin()+7, oversized.begin());
    rejects(oversized);
    const auto bad_err = Wire::SomeErr{std::string("\xed\xa0\x80",3)};
    try { (void)Wire::encode(bad_err); throw std::logic_error("surrogate admitted"); }
    catch (const std::invalid_argument&) { }
    try { (void)Wire::encode(Wire::SomeErr{std::string(257,'x')});
          throw std::logic_error("oversized Err admitted"); }
    catch (const std::invalid_argument&) { }
    std::puts("LLVM_ADT_DATA_WIRE_V1_PASS|profiles=6|control_authority=none|guest_execution=no");
}
