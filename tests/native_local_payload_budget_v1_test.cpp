#include "oreslang/runtime/LocalPayloadBudgetV1.hpp"
#include <cassert>
#include <string>
#include <vector>
#include <map>
#include <tuple>
#include <optional>
#include <array>
#include <variant>
using namespace oreslang::runtime;

static_assert(!LocalUnboundedPayloadV1<std::tuple<int, int>>::value);
static_assert(!LocalUnboundedPayloadV1<std::variant<int, double>>::value);
static_assert(LocalUnboundedPayloadV1<std::tuple<int, std::string>>::value);
static_assert(LocalUnboundedPayloadV1<std::variant<int, std::string>>::value);
static_assert(LocalUnboundedPayloadV1<std::vector<int>>::value);
static_assert(LocalUnboundedPayloadV1<std::optional<std::string>>::value);
static_assert(!LocalUnboundedPayloadV1<std::array<int, 4>>::value);

int main() {
    const std::string emoji = "\xf0\x9f\x98\x80";
    assert(local_valid_utf8_scalars_v1(emoji));
    assert(local_measured_payload_bytes_v1(emoji) == 4);
    assert(local_measured_payload_bytes_v1(std::string(8192,'a')) == 8192);
    const std::string invalid[] = {
        std::string("\xc0\xaf",2), std::string("\xed\xa0\x80",3),
        std::string("\xf4\x90\x80\x80",4), std::string("\xf0\x9f\x98",3),
        std::string("\x80",1), std::string("\xe2\x28\xa1",3),
    };
    for (const auto& value : invalid) {
        assert(!local_valid_utf8_scalars_v1(value));
        assert(local_measured_payload_bytes_v1(value) > 8192);
    }
    assert(local_measured_payload_bytes_v1(7) == sizeof(int));
    // In a v1 shallow-typed host key, heap-owning STL containers are
    // not safely size-accounted; deny instead of billing only sizeof(T).
    assert(local_measured_payload_bytes_v1(
        std::vector<std::string>{std::string(10000, 'z')}) > 8192);
    assert(local_measured_payload_bytes_v1(
        std::map<int, std::string>{{1, "x"}}) > 8192);
    assert(local_measured_payload_bytes_v1(
        std::optional<std::string>{"x"}) > 8192);
    assert(local_measured_payload_bytes_v1(
        std::tuple<int, std::string>{7, "x"}) > 8192);
    assert(local_measured_payload_bytes_v1(
        std::array<std::string, 2>{"x", "y"}) > 8192);
    assert(local_measured_payload_bytes_v1(
        std::variant<int, std::string>{std::string("x")}) > 8192);
    // Truly fixed-size arithmetic composites can be conservatively charged.
    assert(local_measured_payload_bytes_v1(
        std::array<int, 4>{1, 2, 3, 4}) == sizeof(std::array<int, 4>));

}
