#include "oreslang/runtime/CoreJson.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using oreslang::CoreJson;
using oreslang::CoreJsonNumber;
using oreslang::CoreJsonValue;

static bool rejects(const std::string& source) {
    try {
        (void)CoreJson::parse(source);
        return false;
    } catch (const std::runtime_error&) {
        return true;
    }
}

int main() {
    const auto value = CoreJson::parse(
        R"({"name":"Ada","n":123456789012345678901234567890,"a":[true,null,"\uD83D\uDE80"]})");
    const auto encoded = CoreJson::encode(value);
    assert(encoded ==
           u8R"({"name":"Ada","n":123456789012345678901234567890,"a":[true,null,"🚀"]})");

    assert(rejects(R"({"x":1,"x":2})"));
    assert(rejects("01"));
    assert(rejects("1."));
    assert(rejects("1e"));
    assert(rejects(R"("\uD800")"));
    assert(rejects(std::string("\"\xC0\x80\"", 4)));

    assert(CoreJson::exact_i64(CoreJson::parse("-9223372036854775808")) ==
           std::numeric_limits<std::int64_t>::min());
    bool overflow = false;
    try {
        (void)CoreJson::exact_i64(CoreJson::parse("9223372036854775808"));
    } catch (const std::runtime_error&) {
        overflow = true;
    }
    assert(overflow);
    assert(rejects(std::string(CoreJson::max_number_chars + 1, '9')));

    CoreJsonValue::Object duplicate;
    duplicate.emplace_back("x", CoreJsonValue(CoreJsonNumber{"1"}));
    duplicate.emplace_back("x", CoreJsonValue(CoreJsonNumber{"2"}));
    bool duplicate_encode = false;
    try {
        (void)CoreJson::encode(CoreJsonValue(std::move(duplicate)));
    } catch (const std::runtime_error&) {
        duplicate_encode = true;
    }
    assert(duplicate_encode);

    CoreJsonValue invalid_utf8(std::string("\xF4\x90\x80\x80", 4));
    bool invalid_utf8_encode = false;
    try {
        (void)CoreJson::encode(invalid_utf8);
    } catch (const std::runtime_error&) {
        invalid_utf8_encode = true;
    }
    assert(invalid_utf8_encode);

    std::string deep;
    for (std::size_t i = 0; i < CoreJson::max_depth + 1; ++i) deep.push_back('[');
    deep += '0';
    for (std::size_t i = 0; i < CoreJson::max_depth + 1; ++i) deep.push_back(']');
    assert(rejects(deep));

    std::cout << "NATIVE_CORE_JSON_STRICT_SUBSTRATE_PASSED\n";
}
