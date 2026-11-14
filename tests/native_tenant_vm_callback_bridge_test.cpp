// Host-only NativeOresVM callback / tenant lease integration. This is NOT
// compiled .ores guest code, an OS isolation boundary, or a hard-RT proof.
#include "oreslang/runtime/NativeOresVM.h"
#include "oreslang/runtime/NativeTenantAdmission.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <thread>

using oreslang::runtime::NativeTenantAdmission;
using namespace std::chrono_literals;

#define CHECK(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    std::abort(); \
} } while (false)

template<class Pred>
static bool until(Pred predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

struct Harness final {
    NativeTenantAdmission tenants{2, 1, 32};
    std::optional<NativeTenantAdmission::Handle> first;
    std::optional<NativeTenantAdmission::Handle> peer;
    OresNativeVM* vm = nullptr;
    std::atomic<unsigned> errors{0};
    std::atomic<bool> first_active{false};
    std::atomic<bool> release_first{false};
    std::atomic<bool> first_done{false};
    std::atomic<bool> peer_done{false};
    std::atomic<bool> control_done{false};
    std::atomic<bool> second_active{false};
    std::atomic<bool> release_second{false};
    std::atomic<bool> second_revoked{false};
    std::atomic<bool> second_done{false};
};
struct DomainContext final {
    Harness* harness;
    OresNativeVMDomain domain;
};
static void fault(Harness& h) noexcept {
    h.errors.fetch_add(1, std::memory_order_relaxed);
}
static void deliver(void* raw, size_t actor, uint64_t payload) {
    auto& ctx = *static_cast<DomainContext*>(raw);
    Harness& h = *ctx.harness;
    if (ctx.domain != ORES_NATIVE_VM_MAIN || actor > 1) {
        fault(h); return;
    }
    OresNativeExecutionContext execution{};
    if (!ores_native_vm_current_execution_context(h.vm, ctx.domain, &execution) ||
        execution.id.runtime_domain == 0 ||
        execution.id.execution == 0 ||
        execution.allocation.kind != ORES_ALLOCATION_DOMAIN_ACTOR ||
        execution.allocation.owner != actor) {
        fault(h); return;
    }
    auto& issued = actor == 0 ? h.first : h.peer;
    auto lease = h.tenants.enter(*issued, 0);
    if (!lease) { fault(h); return; }

    if (actor == 0 && payload == 10) {
        const std::uint64_t secret = 0x8918273645546372ULL;
        if (!lease->write(0, &secret, sizeof(secret))) fault(h);
        h.first_active.store(true, std::memory_order_release);
        if (!until([&] { return h.release_first.load(std::memory_order_acquire); }))
            fault(h);
        std::uint64_t observed = 0;
        // Tenant-specific quiescing allows the already-admitted turn to finish.
        if (!lease->read(0, &observed, sizeof(observed)) ||
            observed != secret) fault(h);
        lease.reset();
        h.first_done.store(true, std::memory_order_release);
    } else if (actor == 1 && payload == 20) {
        std::uint64_t observed = 999;
        const std::uint64_t marker = 0x2020ULL;
        if (!lease->read(0, &observed, sizeof(observed)) || observed != 0 ||
            !lease->write(0, &marker, sizeof(marker))) fault(h);
        lease.reset();
        h.peer_done.store(true, std::memory_order_release);
    } else if (actor == 0 && payload == 40) {
        const std::uint64_t secret = 0x4040ULL;
        if (!lease->write(0, &secret, sizeof(secret))) fault(h);
        h.second_active.store(true, std::memory_order_release);
        if (!until([&] { return h.release_second.load(std::memory_order_acquire); }))
            fault(h);
        std::uint64_t observed = 0;
        // Global close revokes even a pinned execution lease, unlike STOP.
        if (lease->read(0, &observed, sizeof(observed)) ||
            lease->write(0, &secret, sizeof(secret))) fault(h);
        else h.second_revoked.store(true, std::memory_order_release);
        lease.reset();
        h.second_done.store(true, std::memory_order_release);
    } else {
        fault(h);
    }
}
static void control(void* raw, size_t actor, uint64_t payload) {
    auto& ctx = *static_cast<DomainContext*>(raw);
    Harness& h = *ctx.harness;
    if (ctx.domain != ORES_NATIVE_VM_MAIN || actor != 1 || payload != 30) {
        fault(h); return;
    }
    auto lease = h.tenants.enter(*h.peer, 0);
    if (!lease) { fault(h); return; }
    std::uint64_t observed = 0;
    if (!lease->read(0, &observed, sizeof(observed)) || observed != 0x2020ULL)
        fault(h);
    lease.reset();
    h.control_done.store(true, std::memory_order_release);
}

int main() {
    Harness h;
    h.first = h.tenants.open();
    h.peer = h.tenants.open();
    CHECK(h.first && h.peer && h.first->slot != h.peer->slot);
    OresNativeVMConfig config{};
    DomainContext context[ORES_NATIVE_VM_DOMAIN_COUNT]{};
    for (int d = 0; d < ORES_NATIVE_VM_DOMAIN_COUNT; ++d) {
        context[d] = DomainContext{&h, static_cast<OresNativeVMDomain>(d)};
        config.domains[d].actor_slots = d == ORES_NATIVE_VM_MAIN ? 2 : 1;
        config.domains[d].mailbox_capacity = 4;
        config.domains[d].carrier_workers = d == ORES_NATIVE_VM_MAIN ? 2 : 1;
        config.domains[d].on_message = deliver;
        config.domains[d].on_control = control;
        config.domains[d].context = &context[d];
    }
    h.vm = ores_native_vm_open(&config);
    CHECK(h.vm);
    CHECK(!ores_native_vm_has_hard_security_boundary(h.vm, ORES_NATIVE_VM_UNTRUSTED));
    const auto actor_a = ores_native_vm_spawn(h.vm, ORES_NATIVE_VM_MAIN, 0);
    const auto actor_b = ores_native_vm_spawn(h.vm, ORES_NATIVE_VM_MAIN, 1);
    CHECK(actor_a.scheduler_domain && actor_b.scheduler_domain);
    CHECK(ores_native_vm_try_send(h.vm, ORES_NATIVE_VM_UNTRUSTED, actor_b, 20)
          == ORES_NATIVE_SEND_INVALID_REF);
    CHECK(ores_native_vm_try_send(h.vm, ORES_NATIVE_VM_MAIN, actor_a, 10)
          == ORES_NATIVE_SEND_ACCEPTED);
    CHECK(until([&] { return h.first_active.load(std::memory_order_acquire); }));
    const auto stale = *h.first;
    CHECK(h.tenants.request_stop(stale));
    CHECK(!h.tenants.finish_stop(stale));
    CHECK(!h.tenants.enter(stale, 0));
    CHECK(!h.tenants.open());
    // Real second scheduler carrier must continue serving another tenant.
    CHECK(ores_native_vm_try_send(h.vm, ORES_NATIVE_VM_MAIN, actor_b, 20)
          == ORES_NATIVE_SEND_ACCEPTED);
    CHECK(until([&] { return h.peer_done.load(std::memory_order_acquire); }));
    CHECK(ores_native_vm_publish_control(h.vm, ORES_NATIVE_VM_MAIN, actor_b, 30));
    CHECK(until([&] { return h.control_done.load(std::memory_order_acquire); }));
    CHECK(!h.first_done.load(std::memory_order_acquire));
    CHECK(h.errors.load(std::memory_order_acquire) == 0);

    h.release_first.store(true, std::memory_order_release);
    CHECK(until([&] { return h.first_done.load(std::memory_order_acquire); }));
    CHECK(h.tenants.finish_stop(stale));
    CHECK(!h.tenants.enter(stale, 0));
    h.first = h.tenants.open();
    CHECK(h.first && h.first->slot == stale.slot &&
          h.first->generation > stale.generation);
    {
        auto fresh = h.tenants.enter(*h.first, 0);
        CHECK(fresh);
        std::uint64_t observed = 1;
        CHECK(fresh->read(0, &observed, sizeof(observed)) && observed == 0);
    }
    CHECK(ores_native_vm_try_send(h.vm, ORES_NATIVE_VM_MAIN, actor_a, 40)
          == ORES_NATIVE_SEND_ACCEPTED);
    CHECK(until([&] { return h.second_active.load(std::memory_order_acquire); }));
    h.tenants.close();
    CHECK(!h.tenants.open() && !h.tenants.enter(*h.peer, 0));
    h.release_second.store(true, std::memory_order_release);
    CHECK(until([&] { return h.second_done.load(std::memory_order_acquire); }));
    CHECK(h.second_revoked.load(std::memory_order_acquire));
    CHECK(h.errors.load(std::memory_order_acquire) == 0);

    // Producer and callback lifetimes are fenced by native VM closure before
    // the tenant registry wrapper (which Harness owns) is destroyed.
    ores_native_vm_retire(h.vm);
    CHECK(ores_native_vm_close(h.vm));
    CHECK(h.errors.load(std::memory_order_acquire) == 0);
    std::puts("NATIVE_TENANT_VM_CALLBACK_HOST_PASS");
}
