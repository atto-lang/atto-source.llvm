#include "oreslang/runtime/NativeOresVM.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#if defined(__linux__)
#include <dirent.h>
#include <sys/types.h>
#endif

static atomic_int received[ORES_NATIVE_VM_DOMAIN_COUNT];
static void require(int yes) { if (!yes) abort(); }

#if defined(__linux__)
typedef struct {
    pid_t ids[128];
    size_t count;
} TidSet;

static int compare_tid(const void* left, const void* right) {
    const pid_t a = *(const pid_t*)left;
    const pid_t b = *(const pid_t*)right;
    return (a > b) - (a < b);
}

static TidSet snapshot_tids(void) {
    TidSet set = {{0}, 0};
    DIR* tasks = opendir("/proc/self/task");
    require(tasks != NULL);
    struct dirent* entry;
    while ((entry = readdir(tasks)) != NULL) {
        char* end = NULL;
        const long value = strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || *end != '\0' || value <= 0) continue;
        require(set.count < sizeof(set.ids) / sizeof(set.ids[0]));
        set.ids[set.count++] = (pid_t)value;
    }
    require(closedir(tasks) == 0);
    qsort(set.ids, set.count, sizeof(set.ids[0]), compare_tid);
    return set;
}

static void require_same_tids(TidSet before, TidSet after) {
    require(before.count == after.count);
    for (size_t i = 0; i < before.count; ++i)
        require(before.ids[i] == after.ids[i]);
}
#endif

static void deliver(void* ctx, size_t actor, uint64_t value) {
    int domain = *(int*)ctx;
    require(actor == 0 && value == 42);
    atomic_fetch_add_explicit(&received[domain], 1, memory_order_relaxed);
}

int main(void) {
    OresNativeVMConfig cfg = {0};
    int ids[ORES_NATIVE_VM_DOMAIN_COUNT];
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        ids[i] = i;
        cfg.domains[i].actor_slots = 1;
        cfg.domains[i].mailbox_capacity = 4;
        cfg.domains[i].carrier_workers = 3;
        cfg.domains[i].on_message = deliver;
        cfg.domains[i].context = &ids[i];
    }

    /* Normal throughput open/spawn starts no workers until runnable work. */
    OresNativeVM* lazy = ores_native_vm_open(&cfg);
    require(lazy != NULL);
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        enum OresNativeVMDomain domain = (enum OresNativeVMDomain)i;
        require(ores_native_vm_started_carriers(lazy, domain) == 0);
        require(ores_native_vm_spawn(lazy, domain, 0).scheduler_domain != 0);
        require(ores_native_vm_started_carriers(lazy, domain) == 0);
    }
    require(!ores_native_vm_prepare_realtime(lazy));
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        require(ores_native_vm_started_carriers(lazy, (enum OresNativeVMDomain)i) == 0);
    require(ores_native_vm_close(lazy));

    /* RT open performs potentially blocking pthread creation only at host
     * bootstrap; thereafter all per-domain physical capacity is provisioned. */
    OresNativeVM* vm = ores_native_vm_open_realtime(&cfg);
    require(vm != NULL);
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        enum OresNativeVMDomain domain = (enum OresNativeVMDomain)i;
        require(ores_native_vm_workers(vm, domain) == 3);
        require(ores_native_vm_started_carriers(vm, domain) == 3);
    }
    require(!ores_native_vm_prepare_realtime(vm)); /* cannot bootstrap twice */

#if defined(__linux__)
    const TidSet admitted_threads = snapshot_tids();
#endif

    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        enum OresNativeVMDomain domain = (enum OresNativeVMDomain)i;
        OresNativeActorRef actor = ores_native_vm_spawn(vm, domain, 0);
        require(actor.scheduler_domain != 0);
        require(ores_native_vm_try_send(vm, domain, actor, 42) == ORES_NATIVE_SEND_ACCEPTED);
        require(ores_native_vm_started_carriers(vm, domain) == 3);
    }

    /* Wait only in the external test host. The RT runtime itself must neither
     * create nor retire pthreads while these already-admitted turns execute. */
    int complete = 0;
    for (int attempt = 0; attempt < 5000 && !complete; ++attempt) {
        complete = 1;
        for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
            if (atomic_load_explicit(&received[i], memory_order_relaxed) != 1) {
                complete = 0;
                break;
            }
        }
        if (!complete) {
            const struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
    }
    require(complete);

#if defined(__linux__)
    require_same_tids(admitted_threads, snapshot_tids());
#endif

    /* External host fence and teardown. Already admitted work must drain. */
    require(ores_native_vm_close(vm));
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        require(atomic_load_explicit(&received[i], memory_order_relaxed) == 1);
    return 0;
}
