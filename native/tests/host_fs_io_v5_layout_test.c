#include "oreslang/runtime/TenantVmIoAbiV5.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
    /* New v5 layout is separate; it cannot be mistaken for v4 at module
     * admission or accidentally extend a live v4-sized callback table. */
    assert(ORES_VM_GUEST_ABI_VERSION == 4u);
    assert(ORES_VM_GUEST_IO_ABI_VERSION == 5u);
    assert(sizeof(OresVmHostApiV5) >= sizeof(OresVmHostApi) + 4u * sizeof(void *));
    OresVmHostApiV5 api = {0};
    api.v4.abi_version = ORES_VM_GUEST_IO_ABI_VERSION;
    assert(api.fs_submit_read == NULL && api.fs_submit_write == NULL);
    assert(api.fs_poll == NULL && api.fs_cancel == NULL);
    OresVmGuestModuleV5 module = {0};
    module.abi_version = ORES_VM_GUEST_IO_ABI_VERSION;
    assert(module.abi_version != ORES_VM_GUEST_ABI_VERSION);
    assert(ORES_VM_IO_PENDING < 0 && ORES_VM_IO_PERMISSION_DENIED < 0);
    puts("ATTO_FS_GUEST_IO_ABI_V5_LAYOUT_ONLY_TEST_PASS");
    return 0;
}
