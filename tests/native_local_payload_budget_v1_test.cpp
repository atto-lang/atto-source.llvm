#include "oreslang/runtime/LocalPayloadBudgetV1.hpp"
#include <cassert>
#include <string>
using namespace oreslang::runtime;
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
}
