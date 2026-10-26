#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque capacity-1 signed-i64 channel capability used by generated LLVM IR.
// Status values for try_read: 0 = would block, 1 = value committed, 2 = EOF.
void* oreslang_channel_i64_new_1(void);
int oreslang_channel_i64_try_write(void* channel, std::int64_t value);
int oreslang_channel_i64_try_read(void* channel, std::int64_t* out_value);
void oreslang_channel_i64_drop(void* channel);

#ifdef __cplusplus
}
#endif
