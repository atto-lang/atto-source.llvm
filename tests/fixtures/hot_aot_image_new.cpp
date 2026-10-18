#include "hot_aot_image_abi.hpp"

namespace {
std::int64_t process(std::int64_t value) { return value + 20; }
const OresTestHotAotImageV1 image = {
    1U, sizeof(OresTestHotAotImageV1),
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
    "1111111111111111111111111111111111111111111111111111111111111111",
    "2222222222222222222222222222222222222222222222222222222222222222",
    "3333333333333333333333333333333333333333333333333333333333333333",
    &process
};
}
extern "C" const OresTestHotAotImageV1* ores_test_hot_aot_image_v1() {
    return &image;
}
