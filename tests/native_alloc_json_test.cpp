#include "oreslang/runtime/AllocJson.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using oreslang::AllocJson;
using oreslang::AllocJsonNumber;
using oreslang::AllocJsonValue;

static bool rejects(const std::string& source) {
    try {
        (void)AllocJson::parse(source);
        return false;
    } catch (const std::runtime_error&) {
        return true;
    }
}

int main() {
    const auto value = AllocJson::parse(
        R"({"name":"Ada","n":123456789012345678901234567890,"a":[true,null,"\uD83D\uDE80"]})");
    const auto encoded = AllocJson::encode(value);
    assert(encoded ==
           u8R"({"name":"Ada","n":123456789012345678901234567890,"a":[true,null,"🚀"]})");

    assert(rejects(R"({"x":1,"x":2})"));
    assert(rejects("01"));
    assert(rejects("1."));
    assert(rejects("1e"));
    assert(rejects(R"("\uD800")"));
    assert(rejects(std::string("\"\xC0\x80\"", 4)));

    assert(AllocJson::exact_i64(AllocJson::parse("-9223372036854775808")) ==
           std::numeric_limits<std::int64_t>::min());
    assert(AllocJson::exact_i64(AllocJson::parse("1.0")) == 1);
    assert(AllocJson::exact_i64(AllocJson::parse("1e0")) == 1);
    assert(AllocJson::exact_i64(AllocJson::parse("100e-2")) == 1);
    assert(AllocJson::exact_i64(AllocJson::parse("-1200.00e-2")) == -12);

    for (const char* non_integral : {"1.5", "1e-1", "0.001e2"}) {
        bool rejected_non_integral = false;
        try {
            (void)AllocJson::exact_i64(AllocJson::parse(non_integral));
        } catch (const std::runtime_error&) {
            rejected_non_integral = true;
        }
        assert(rejected_non_integral);
    }

    AllocJsonValue forged_number(AllocJsonNumber{"1e999999999999999999999999"});
    bool forged_overflow = false;
    try {
        (void)AllocJson::exact_i64(forged_number);
    } catch (const std::runtime_error&) {
        forged_overflow = true;
    }
    assert(forged_overflow);

    for (const char* bad_number_token : {" 1", "1 ", "\t1"}) {
        AllocJsonValue bad_number(AllocJsonNumber{bad_number_token});
        bool rejected_bad_number = false;
        try {
            (void)AllocJson::encode(bad_number);
        } catch (const std::runtime_error&) {
            rejected_bad_number = true;
        }
        assert(rejected_bad_number);
    }
    bool overflow = false;
    try {
        (void)AllocJson::exact_i64(AllocJson::parse("9223372036854775808"));
    } catch (const std::runtime_error&) {
        overflow = true;
    }
    assert(overflow);
    assert(rejects(std::string(AllocJson::max_number_chars + 1, '9')));

    AllocJsonValue::Object duplicate;
    duplicate.emplace_back("x", AllocJsonValue(AllocJsonNumber{"1"}));
    duplicate.emplace_back("x", AllocJsonValue(AllocJsonNumber{"2"}));
    bool duplicate_encode = false;
    try {
        (void)AllocJson::encode(AllocJsonValue(std::move(duplicate)));
    } catch (const std::runtime_error&) {
        duplicate_encode = true;
    }
    assert(duplicate_encode);

    AllocJsonValue invalid_utf8(std::string("\xF4\x90\x80\x80", 4));
    bool invalid_utf8_encode = false;
    try {
        (void)AllocJson::encode(invalid_utf8);
    } catch (const std::runtime_error&) {
        invalid_utf8_encode = true;
    }
    assert(invalid_utf8_encode);

    std::string deep;
    for (std::size_t i = 0; i < AllocJson::max_depth + 1; ++i) deep.push_back('[');
    deep += '0';
    for (std::size_t i = 0; i < AllocJson::max_depth + 1; ++i) deep.push_back(']');
    assert(rejects(deep));

    std::cout << "NATIVE_ALLOC_JSON_STRICT_SUBSTRATE_PASSED\n";
}
