#pragma once
#include "oreslang/runtime/TenantVmGuestAbi.h"
#ifdef __cplusplus
extern "C" {
#endif
extern const OresVmGuestModule ores_vm_lowered_module;
enum { OP_ADD, OP_STATE_OFFSET, OP_PEEK, OP_PEEK_FAR, OP_POKE, OP_TRAP, OP_SPIN, OP_RECURSE, OP_INDIRECT };
#ifdef __cplusplus
}
#endif
