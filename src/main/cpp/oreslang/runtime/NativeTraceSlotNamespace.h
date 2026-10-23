#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Reserve a non-reusable range of store slot IDs, or report exhaustion. */
int ores_native_trace_reserve_slot_namespace(uint32_t capacity, uint32_t* first_slot);
#ifdef __cplusplus
}
#endif
