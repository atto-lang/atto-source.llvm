#include "native-error-slot-v1.h"

#include <cstddef>
#include <cstdint>
#include <iostream>

static_assert(sizeof(OresNativeErrorSlotV1) == 16);
static_assert(offsetof(OresNativeErrorSlotV1, code) == 0);
static_assert(offsetof(OresNativeErrorSlotV1, effect) == 4);
static_assert(offsetof(OresNativeErrorSlotV1, flags) == 5);
static_assert(offsetof(OresNativeErrorSlotV1, reserved) == 6);
static_assert(offsetof(OresNativeErrorSlotV1, payload) == 8);

int main() {
    OresNativeErrorSlotV1 slot{};
    if (slot.code != ORES_ERROR_NONE_V1 ||
        slot.effect != ORES_EFFECT_NONE_V1 ||
        slot.flags != 0 ||
        slot.reserved != 0 ||
        slot.payload != 0) {
        return 1;
    }

    slot.code = ORES_ERROR_ARITHMETIC_OVERFLOW_V1;
    slot.effect = ORES_EFFECT_THROW_V1;
    if (slot.code != 65537u || slot.effect != 1u) {
        return 2;
    }

    slot.effect = ORES_EFFECT_RAISE_V1;
    if (slot.effect != 2u) {
        return 3;
    }

    slot.effect = ORES_EFFECT_PANIC_V1;
    if (slot.effect != 3u) {
        return 4;
    }

    std::cout << "PASS NativeErrorSlotV1 size=16 overflow=65537 throw=1 raise=2 panic=3\n";
    return 0;
}
