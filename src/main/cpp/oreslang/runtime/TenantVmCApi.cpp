#include "oreslang/runtime/TenantVmCApi.h"
#include "oreslang/runtime/TenantVm.hpp"

#include <cstring>
#include <new>

using namespace oreslang::runtime::vm;

struct OresVmCHost { TenantVmHost host; explicit OresVmCHost(HostLimits l) : host(l) {} };
struct OresVmCVm { VmHandle handle; };
struct OresVmCActor { VmActor actor; };

extern "C" {

OresVmCHost* ores_vm_c_host_new(uint32_t carriers, uint32_t max_vms) {
    HostLimits limits;
    limits.carriers = carriers;
    limits.max_vms = max_vms;
    try { return new OresVmCHost(limits); } catch (...) { return nullptr; }
}
void ores_vm_c_host_free(OresVmCHost* h) { delete h; }

OresVmCVm* ores_vm_c_open(OresVmCHost* h, const char* tenant_id, const OresVmGuestModule* module,
                          uint32_t turn_deadline_ms, int32_t* status) {
    VmSpec spec;
    spec.tenant_id = tenant_id ? tenant_id : "";
    spec.module = module;
    spec.limits.turn_deadline_ms = turn_deadline_ms;
    VmOpenResult result = h->host.open(std::move(spec));
    *status = static_cast<int32_t>(result.status);
    if (result.status != Status::Ok) return nullptr;
    return new (std::nothrow) OresVmCVm{std::move(result.handle)};
}
void ores_vm_c_vm_free(OresVmCVm* v) { delete v; }

OresVmCActor* ores_vm_c_spawn(OresVmCHost* h, OresVmCVm* v, uint32_t kind, int32_t* status) {
    VmSpawnResult result = h->host.spawn_actor(v->handle, kind);
    *status = static_cast<int32_t>(result.status);
    if (result.status != Status::Ok) return nullptr;
    return new (std::nothrow) OresVmCActor{std::move(result.actor)};
}
void ores_vm_c_actor_free(OresVmCActor* a) { delete a; }

int32_t ores_vm_c_call(OresVmCHost* h, OresVmCActor* a, const uint8_t* input, uint32_t input_length,
                       uint8_t* output, uint32_t output_capacity, uint32_t* output_length) {
    TurnResult result = h->host.call(a->actor, std::vector<std::uint8_t>(input, input + input_length)).get();
    if (result.status == Status::Ok) {
        if (result.payload.size() > output_capacity) return static_cast<int32_t>(Status::ReplyTooLarge);
        if (!result.payload.empty()) std::memcpy(output, result.payload.data(), result.payload.size());
        *output_length = static_cast<uint32_t>(result.payload.size());
    }
    return static_cast<int32_t>(result.status);
}

int32_t ores_vm_c_kill(OresVmCHost* h, OresVmCVm* v) { return static_cast<int32_t>(h->host.kill(v->handle)); }
int32_t ores_vm_c_wait_closed(OresVmCHost* h, OresVmCVm* v) {
    return static_cast<int32_t>(h->host.wait_closed(v->handle).reason);
}
const char* ores_vm_c_status_name(int32_t status) { return to_string(static_cast<Status>(status)); }
const char* ores_vm_c_reason_name(int32_t reason) { return to_string(static_cast<FailReason>(reason)); }

} // extern "C"
