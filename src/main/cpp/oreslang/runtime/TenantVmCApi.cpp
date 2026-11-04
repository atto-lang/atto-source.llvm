#include "oreslang/runtime/TenantVmCApi.h"
#include "oreslang/runtime/TenantVm.hpp"
#include "oreslang/runtime/TenantVmGuestLoader.hpp"

#include <cstring>
#include <new>

using namespace oreslang::runtime::vm;

struct OresVmCHost { TenantVmHost host; explicit OresVmCHost(HostLimits l) : host(l) {} };
struct OresVmCVm { VmHandle handle; };
struct OresVmCActor { VmActor actor; };
struct OresVmCGuest { LoadedGuest guest; };

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
    // No C++ exception may cross into a P/Invoke caller.
    try {
        VmSpec spec;
        spec.tenant_id = tenant_id ? tenant_id : "";
        spec.module = module;
        spec.limits.turn_deadline_ms = turn_deadline_ms;
        VmOpenResult result = h->host.open(std::move(spec));
        *status = static_cast<int32_t>(result.status);
        if (result.status != Status::Ok) return nullptr;
        return new OresVmCVm{std::move(result.handle)};
    } catch (...) {
        *status = static_cast<int32_t>(Status::HostClosed);
        return nullptr;
    }
}

OresVmCGuest* ores_vm_c_guest_load(const char* path, const char* sha256) {
    if (!path || !sha256) return nullptr;
    try {
        LoadedGuest guest = load_guest_library(path, sha256);
        if (!guest.module) return nullptr;
        return new OresVmCGuest{std::move(guest)};
    } catch (...) {
        return nullptr;
    }
}
void ores_vm_c_guest_free(OresVmCGuest* g) { delete g; }

OresVmCVm* ores_vm_c_open_guest(OresVmCHost* h, const char* tenant_id, const OresVmCGuest* g,
                                uint32_t turn_deadline_ms, int32_t* status) {
    try {
        VmSpec spec;
        spec.tenant_id = tenant_id ? tenant_id : "";
        spec.module = g ? g->guest.module : nullptr;
        spec.module_owner = g ? g->guest.owner : nullptr;
        spec.limits.turn_deadline_ms = turn_deadline_ms;
        VmOpenResult result = h->host.open(std::move(spec));
        *status = static_cast<int32_t>(result.status);
        if (result.status != Status::Ok) return nullptr;
        return new OresVmCVm{std::move(result.handle)};
    } catch (...) {
        *status = static_cast<int32_t>(Status::HostClosed);
        return nullptr;
    }
}
void ores_vm_c_vm_free(OresVmCVm* v) { delete v; }

OresVmCActor* ores_vm_c_spawn(OresVmCHost* h, OresVmCVm* v, uint32_t kind, int32_t* status) {
    try {
        VmSpawnResult result = h->host.spawn_actor(v->handle, kind);
        *status = static_cast<int32_t>(result.status);
        if (result.status != Status::Ok) return nullptr;
        return new OresVmCActor{std::move(result.actor)};
    } catch (...) {
        *status = static_cast<int32_t>(Status::HeapBudget);
        return nullptr;
    }
}
void ores_vm_c_actor_free(OresVmCActor* a) { delete a; }

int32_t ores_vm_c_call(OresVmCHost* h, OresVmCActor* a, const uint8_t* input, uint32_t input_length,
                       uint8_t* output, uint32_t output_capacity, uint32_t* output_length) {
    if ((!input && input_length) || !output_length) return static_cast<int32_t>(Status::InvalidArgument);
    try {
        TurnResult result = h->host.call(a->actor, std::vector<std::uint8_t>(input, input + input_length)).get();
        if (result.status == Status::Ok) {
            if (result.payload.size() > output_capacity) return static_cast<int32_t>(Status::ReplyTooLarge);
            if (!result.payload.empty()) {
                if (!output) return static_cast<int32_t>(Status::InvalidArgument);
                std::memcpy(output, result.payload.data(), result.payload.size());
            }
            *output_length = static_cast<uint32_t>(result.payload.size());
        }
        return static_cast<int32_t>(result.status);
    } catch (...) {
        return static_cast<int32_t>(Status::Busy);
    }
}

int32_t ores_vm_c_kill(OresVmCHost* h, OresVmCVm* v) { return static_cast<int32_t>(h->host.kill(v->handle)); }
int32_t ores_vm_c_wait_closed(OresVmCHost* h, OresVmCVm* v) {
    return static_cast<int32_t>(h->host.wait_closed(v->handle).reason);
}
const char* ores_vm_c_status_name(int32_t status) { return to_string(static_cast<Status>(status)); }
const char* ores_vm_c_reason_name(int32_t reason) { return to_string(static_cast<FailReason>(reason)); }

} // extern "C"
