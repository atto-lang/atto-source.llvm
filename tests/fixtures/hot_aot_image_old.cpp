#include "hot_aot_image_abi.hpp"

namespace {
std::int64_t process(std::int64_t value) { return value + 10; }
const OresTestHotAotImageV1 image = {
    1U, sizeof(OresTestHotAotImageV1),
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    "1111111111111111111111111111111111111111111111111111111111111111",
    "2222222222222222222222222222222222222222222222222222222222222222",
    "3333333333333333333333333333333333333333333333333333333333333333",
    &process
};
}
extern "C" const OresTestHotAotImageV1* ores_test_hot_aot_image_v1() {
    return &image;
}

namespace {
void (*test_unload_hook)() = nullptr;
__attribute__((destructor)) void on_unload() {
    if (test_unload_hook) test_unload_hook();
}
}
extern "C" void ores_test_hot_aot_set_unload_hook(void (*fn)()) {
    test_unload_hook = fn;
}
