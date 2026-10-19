#include "oreslang/runtime/NativeCpuBudget.h"
#include "oreslang/runtime/NativeOresVM.h"
#if defined(NDEBUG)
#undef NDEBUG // Tests must not silently disappear in optimized CMake builds
#endif
#include <assert.h>
#include <stdio.h>

static void on_mail(void* ctx, size_t slot, uint64_t message) {
    (void)ctx; (void)slot; (void)message;
}
static OresNativeVMConfig configuration(void) {
    OresNativeVMConfig config = {0};
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        config.domains[i].actor_slots = 2;
        config.domains[i].mailbox_capacity = 2;
        config.domains[i].on_message = on_mail;
    }
    return config;
}

int main(void) {
    assert(ores_native_cpu_max("150000 100000\n") == 2);
    assert(ores_native_cpu_max("50000 100000\n") == 1);
    assert(ores_native_cpu_max("max 100000\n") == 0);
    assert(ores_native_cpu_max("-1 100000") == 0);
    assert(ores_native_cpu_max("100000 0") == 0);
    assert(ores_native_cpu_quota("250000\n", "100000\n") == 3);
    assert(ores_native_cpu_set("0-2,5,8-9\n") == 6);
    assert(ores_native_cpu_set("3") == 1);
    assert(ores_native_cpu_set("0-3,2-5") == 0);
    assert(ores_native_cpu_set("1,,3") == 0);
    assert(ores_native_cpu_set("0-1,1-2") == 0);

    for (size_t budget = 1; budget <= 128; ++budget) {
        size_t requested[4] = {0, 0, 0, 0}, result[4] = {0};
        ores_native_cpu_distribute(budget, requested, result);
        size_t total = 0;
        for (int i = 0; i < 4; ++i) {
            assert(result[i] >= 1);
            total += result[i];
        }
        assert(total == (budget < 4 ? 4 : budget));
    }

    OresNativeVMConfig config = configuration();
    OresNativeVM* legacy = ores_native_vm_open(&config);
    assert(legacy != NULL);
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        assert(ores_native_vm_workers(legacy, (enum OresNativeVMDomain)i) == 5);
        assert(ores_native_vm_started_carriers(legacy, (enum OresNativeVMDomain)i) == 0);
    }
    assert(ores_native_vm_close(legacy));

    size_t capacity = ores_native_vm_effective_cpu_budget();
    assert(capacity >= 1);
    size_t effective = capacity > 512 ? 512 : capacity;
    size_t requested[4] = {0}, result[4] = {0};
    ores_native_cpu_distribute(effective, requested, result);
    OresNativeVM* budgeted = ores_native_vm_open_cpu_budgeted(&config);
    assert(budgeted != NULL);
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        assert(ores_native_vm_workers(budgeted, (enum OresNativeVMDomain)i) == result[i]);
        assert(ores_native_vm_started_carriers(budgeted, (enum OresNativeVMDomain)i) == 0);
    }
    assert(ores_native_vm_close(budgeted));

    config.domains[ORES_NATIVE_VM_UNTRUSTED].carrier_workers = 2;
    requested[ORES_NATIVE_VM_UNTRUSTED] = 2;
    ores_native_cpu_distribute(effective, requested, result);
    budgeted = ores_native_vm_open_cpu_budgeted(&config);
    assert(budgeted != NULL);
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        assert(ores_native_vm_workers(budgeted, (enum OresNativeVMDomain)i) == result[i]);
    assert(ores_native_vm_close(budgeted));

    puts("native CPU-budgeted VM carrier admission: OK");
    return 0;
}
