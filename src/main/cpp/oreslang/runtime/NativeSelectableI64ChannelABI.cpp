#include "oreslang/runtime/NativeSelectableI64ChannelABI.h"

#include "oreslang/runtime/NativeSelectableI64Channel.hpp"

#include <cstdlib>
#include <new>

namespace {

using Channel = oreslang::runtime::NativeSelectableI64Channel<1, 64>;

struct OresChannelI64 final {
    Channel value;
};

[[noreturn]] void invalid_channel() noexcept {
    std::abort();
}

OresChannelI64* checked(void* channel) noexcept {
    if (channel == nullptr) invalid_channel();
    return static_cast<OresChannelI64*>(channel);
}

}  // namespace

extern "C" void* oreslang_channel_i64_new_1(void) {
    auto* channel = new (std::nothrow) OresChannelI64();
    if (channel == nullptr) std::abort();
    return channel;
}

extern "C" int oreslang_channel_i64_try_write(
        void* channel, std::int64_t value) {
    return checked(channel)->value.try_write(value) ? 1 : 0;
}

extern "C" int oreslang_channel_i64_try_read(
        void* channel, std::int64_t* out_value) {
    if (out_value == nullptr) invalid_channel();
    const auto result = checked(channel)->value.try_read_result();
    switch (result.status) {
        case Channel::ReadStatus::WouldBlock:
            return 0;
        case Channel::ReadStatus::Value:
            *out_value = result.value;
            return 1;
        case Channel::ReadStatus::EndOfStream:
            return 2;
    }
    invalid_channel();
}

extern "C" void oreslang_channel_i64_drop(void* channel) {
    delete checked(channel);
}
