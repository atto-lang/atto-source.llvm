#pragma once
#include <cstdint>

// TEST-ONLY C ABI fixture. A matching descriptor is NOT a signature, an
// authenticated artifact digest, a safety sandbox, or a production loader.
extern "C" {
struct OresTestHotAotImageV1 {
    std::uint32_t abi_version;
    std::uint32_t struct_bytes;
    const char* code_sha256;
    const char* state_sha256;
    const char* message_sha256;
    const char* capability_sha256;
    std::int64_t (*apply)(std::int64_t);
};
using OresTestHotAotImageGetter = const OresTestHotAotImageV1* (*)();
}
