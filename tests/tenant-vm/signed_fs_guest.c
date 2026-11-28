/* Host-only v4 test module to check signed-image loader admission.
 * This is C, NOT a compiled AttoLang .atto std.fs guest or ABI v5. */
#include "oreslang/runtime/TenantVmGuestAbi.h"
#include <stddef.h>
static int64_t turn(const OresVmHostApi* host, void* instance, uint8_t* cage,
                    uint64_t self, uint32_t state, uint32_t input,
                    uint32_t input_length, uint32_t output,
                    uint32_t output_capacity) {
    (void)host; (void)instance; (void)cage; (void)self; (void)state;
    (void)input; (void)input_length; (void)output; (void)output_capacity;
    return 0;
}
static const OresVmGuestKind kinds[] = {{turn, 16, 0, 64, 0, 0, 0}};
const OresVmGuestModule ores_vm_guest_module = {
    ORES_VM_GUEST_ABI_VERSION, 1, kinds
};
