#pragma once
/* C entry points to TenantVmHost, for hosts in other languages (.NET P/Invoke). A host thread
 * here is any caller thread. Guest code only ever runs on the host's own carriers, so it
 * never runs on a thread owned by the calling language's runtime. */
#include "oreslang/runtime/TenantVmGuestAbi.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct OresVmCHost OresVmCHost;
typedef struct OresVmCVm OresVmCVm;
typedef struct OresVmCActor OresVmCActor;

OresVmCHost* ores_vm_c_host_new(uint32_t carriers, uint32_t max_vms);
void ores_vm_c_host_free(OresVmCHost*);
/* Status values are the integer value of oreslang::runtime::vm::Status; 0 is Ok. */
OresVmCVm* ores_vm_c_open(OresVmCHost*, const char* tenant_id, const OresVmGuestModule*,
                          uint32_t turn_deadline_ms, int32_t* status);
void ores_vm_c_vm_free(OresVmCVm*);
OresVmCActor* ores_vm_c_spawn(OresVmCHost*, OresVmCVm*, uint32_t kind, int32_t* status);
void ores_vm_c_actor_free(OresVmCActor*);
/* Blocks the caller until the turn ends. */
int32_t ores_vm_c_call(OresVmCHost*, OresVmCActor*, const uint8_t* input, uint32_t input_length,
                       uint8_t* output, uint32_t output_capacity, uint32_t* output_length);
int32_t ores_vm_c_kill(OresVmCHost*, OresVmCVm*);
/* Blocks until the VM is physically retired. Returns the FailReason value. */
int32_t ores_vm_c_wait_closed(OresVmCHost*, OresVmCVm*);
const char* ores_vm_c_status_name(int32_t status);
const char* ores_vm_c_reason_name(int32_t reason);
#ifdef __cplusplus
}
#endif
