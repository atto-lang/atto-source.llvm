#pragma once

#include "oreslang/runtime/OresIsolateVM.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oreslang::runtime {

// HOST-ONLY prototype. This is NOT a native-code security boundary or guest
// source lowering. Never admit untrusted guest code in TrustedInProcess mode.
// The process sandbox is deliberately rejected until an OS worker exists.
// OresVM is the host's isolate registry and admission boundary. It owns no
// user-code scheduler and exposes only checked offset-based memory operations;
// no native heap address can escape through this API.
class OresVM final {

public:
    class IsolateHandle final {
        friend class OresVM;
        std::weak_ptr<IsolateVM> state_;
        std::uint64_t generation_ = 0;
        explicit IsolateHandle(const std::shared_ptr<IsolateVM>& state)
            : state_(state), generation_(state->generation) {}
    public:
        IsolateHandle() = default;
    };

    // Canonical name: opaque per-tenant-generation host capability.
    using TenantVMHandle = IsolateHandle;

    class Allocation final {
        friend class OresVM;
        std::weak_ptr<IsolateVM> state_;
        std::uint64_t generation_ = 0;
        std::uint32_t offset_ = 0;
        std::uint32_t size_ = 0;
        Allocation(const std::shared_ptr<IsolateVM>& state,
                   std::uint32_t offset, std::uint32_t size)
            : state_(state), generation_(state->generation),
              offset_(offset), size_(size) {}
    public:
        Allocation() = default;
        std::uint32_t offset() const noexcept { return offset_; }
        std::uint32_t size() const noexcept { return size_; }
    };

    class ActorTicket final {
        friend class OresVM;
        std::weak_ptr<IsolateVM> state_;
        std::uint64_t generation_ = 0;
        std::uint64_t id_ = 0;
        std::uint64_t actor_generation_ = 0;
        ActorTicket(const std::shared_ptr<IsolateVM>& state, std::uint64_t id,
                    std::uint64_t actor_generation)
            : state_(state), generation_(state->generation), id_(id),
              actor_generation_(actor_generation) {}
    public:
        ActorTicket() = default;
    };

    class ResourceTicket final {
        friend class OresVM;
        std::weak_ptr<IsolateVM> state_;
        std::uint64_t generation_ = 0;
        std::uint64_t resource_id_ = 0;
        ResourceTicket(const std::shared_ptr<IsolateVM>& state, std::uint64_t id)
            : state_(state), generation_(state->generation), resource_id_(id) {}
    public:
        ResourceTicket() = default;
    };

    explicit OresVM(OresVmLimits limits = {}) : limits_(limits) {
        if (limits_.max_tenants == 0 || limits_.max_admitted_heap_bytes == 0 ||
            limits_.max_inflight_guest_calls == 0)
            throw std::invalid_argument("OresVM requires positive aggregate quotas");
    }
    OresVM(const OresVM&) = delete;
    OresVM& operator=(const OresVM&) = delete;

    // Compatibility-only spelling: this still admits TRUSTED in-process
    // tenant state only. Untrusted code remains denied by create_isolate.
    TenantVMHandle create_tenant_vm(TenantVMConfig config) {
        return create_isolate(std::move(config));
    }

    IsolateHandle create_isolate(IsolateConfig config) {
        // Tenant identifiers are host-owned but can originate from customer
        // metadata. Bound their uncharged allocation and reject control bytes
        // before the ID reaches maps, diagnostics, or observability.
        // Shared tenant-isolate identity grammar, identical to Java, .NET,
        // Cranelift and Wasmtime: ASCII [A-Za-z0-9_.-]{1,128}, excluding
        // "." and "..". Never let a future path/namespace broker gain
        // authority from an identifier admitted only by the native host.
        const bool unsafe_tenant_id = config.tenant_id.empty() ||
            config.tenant_id.size() > 128 ||
            config.tenant_id == "." || config.tenant_id == ".." ||
            !std::all_of(config.tenant_id.begin(), config.tenant_id.end(),
                [](unsigned char c) {
                    return (c >= 'A' && c <= 'Z') ||
                           (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') ||
                           c == '_' || c == '.' || c == '-';
                });
        if (unsafe_tenant_id || config.heap_limit_bytes == 0 ||
            config.max_actors == 0 || config.max_inflight_guest_calls == 0 ||
            config.max_resource_pins == 0)
            throw std::invalid_argument("isolate requires a bounded ID and positive quotas");
        if (config.mode != IsolateMode::TrustedInProcess)
            throw std::logic_error("sandboxed-process isolation is not implemented");
        std::lock_guard<std::mutex> lock(mutex_);
        if (isolates_.count(config.tenant_id))
            throw std::logic_error("tenant already has an isolate");
        if (isolates_.size() >= limits_.max_tenants)
            throw std::logic_error("OresVM tenant quota exhausted");
        // Charge the entire declared logical heap limit at admission, not
        // only current committed vector bytes. Prevent tenant overcommit.
        if (config.heap_limit_bytes >
            limits_.max_admitted_heap_bytes - admitted_heap_bytes_)
            throw std::bad_alloc();
        if (next_generation_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("isolate generations exhausted");
        auto state = std::shared_ptr<IsolateVM>(new IsolateVM(std::move(config), next_generation_++));
        isolates_.emplace(state->config.tenant_id, state);
        admitted_heap_bytes_ += state->config.heap_limit_bytes;
        return IsolateHandle(state);
    }

    void start(const IsolateHandle& handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto state = checked(handle);
        if (state->phase != IsolatePhase::Created)
            throw std::logic_error("isolate is not newly created");
        state->phase = IsolatePhase::Running;
    }

    Allocation allocate(const IsolateHandle& handle, std::uint32_t bytes,
                        std::uint32_t alignment = 1) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto state = checked(handle);
        if (state->phase != IsolatePhase::Running)
            throw std::logic_error("isolate does not admit allocations");
        if (bytes == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0)
            throw std::invalid_argument("nonzero bytes and power-of-two alignment required");
        const auto limit = static_cast<std::size_t>(state->config.heap_limit_bytes);
        const auto used = state->heap.size();
        const auto pad = (alignment - (used % alignment)) % alignment;
        if (used > limit || pad > limit - used || bytes > limit - used - pad)
            throw std::bad_alloc();
        const auto offset = used + pad;
        state->heap.resize(offset + bytes, 0);
        return Allocation(state, static_cast<std::uint32_t>(offset), bytes);
    }

    void write_byte(const IsolateHandle& handle, const Allocation& ref,
                    std::uint32_t index, std::uint8_t value) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto state = checked(handle);
        check_ref(state, ref);
        if (state->phase != IsolatePhase::Running &&
            state->phase != IsolatePhase::Quiescing)
            throw std::logic_error("isolate memory is not accessible");
        if (index >= ref.size_)
            throw std::out_of_range("allocation index out of bounds");
        state->heap[static_cast<std::size_t>(ref.offset_) + index] = value;
    }

    std::uint8_t read_byte(const IsolateHandle& handle, const Allocation& ref,
                           std::uint32_t index) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto state = checked(handle);
        check_ref(state, ref);
        if (state->phase != IsolatePhase::Running &&
            state->phase != IsolatePhase::Quiescing)
            throw std::logic_error("isolate memory is not accessible");
        if (index >= ref.size_)
            throw std::out_of_range("allocation index out of bounds");
        return state->heap[static_cast<std::size_t>(ref.offset_) + index];
    }

    ActorTicket register_actor(const IsolateHandle& handle, std::uint64_t actor_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto state = checked(handle);
        if (state->phase != IsolatePhase::Running || actor_id == 0)
            throw std::logic_error("actor admission closed or actor ID invalid");
        if (state->actors.count(actor_id))
            throw std::logic_error("duplicate actor ID in isolate");
        if (state->actors.size() >= state->config.max_actors)
            throw std::logic_error("actor quota exhausted");
        if (state->next_actor_generation == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("actor generations exhausted");
        const auto incarnation = state->next_actor_generation++;
        state->actors.emplace(actor_id, incarnation);
        return ActorTicket(state, actor_id, incarnation);
    }

    void retire_actor(const IsolateHandle& handle, const ActorTicket& actor) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto state = checked(handle);
        if (state->phase != IsolatePhase::Running &&
            state->phase != IsolatePhase::Quiescing)
            throw std::logic_error("actor retirement in invalid phase");
        const auto it = state->actors.find(actor.id_);
        if (actor.generation_ != state->generation ||
            actor.state_.lock() != state || it == state->actors.end() ||
            it->second != actor.actor_generation_)
            throw std::logic_error("stale or foreign actor ticket");
        for (const auto& pin : state->resource_pins)
            if (pin.second.actor_id == actor.id_ &&
                pin.second.actor_generation == actor.actor_generation_)
                throw std::logic_error("actor has outstanding native resource pins");
        state->actors.erase(it);
    }

    // A ticket must be released only after the underlying I/O registration,
    // callback, continuation or mailbox loan has PHYSICALLY retired. This
    // substrate cannot prove external retirement; native adapters must do so.
    ResourceTicket pin_actor_resource(const IsolateHandle& handle,
                                      const ActorTicket& actor,
                                      IsolateResourceKind kind) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto state = checked(handle);
        const auto it = state->actors.find(actor.id_);
        if (state->phase != IsolatePhase::Running ||
            actor.state_.lock() != state || actor.generation_ != state->generation ||
            it == state->actors.end() || it->second != actor.actor_generation_)
            throw std::logic_error("resource pin admission denied or actor stale");
        if (state->resource_pins.size() >= state->config.max_resource_pins)
            throw std::logic_error("tenant resource pin quota exhausted");
        if (state->next_resource_id == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("resource pin generations exhausted");
        const auto id = state->next_resource_id++;
        state->resource_pins.emplace(id, IsolateResourceRecord{
            actor.id_, actor.actor_generation_, kind});
        return ResourceTicket(state, id);
    }

    void retire_actor_resource(const IsolateHandle& handle,
                               const ActorTicket& actor,
                               const ResourceTicket& ticket) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto state = checked(handle);
        const auto actor_it = state->actors.find(actor.id_);
        const auto pin_it = state->resource_pins.find(ticket.resource_id_);
        if ((state->phase != IsolatePhase::Running &&
             state->phase != IsolatePhase::Quiescing) ||
            actor.state_.lock() != state || actor.generation_ != state->generation ||
            actor_it == state->actors.end() || actor_it->second != actor.actor_generation_ ||
            ticket.state_.lock() != state || ticket.generation_ != state->generation ||
            pin_it == state->resource_pins.end() ||
            pin_it->second.actor_id != actor.id_ ||
            pin_it->second.actor_generation != actor.actor_generation_)
            throw std::logic_error("resource retirement denied or stale ticket");
        state->resource_pins.erase(pin_it);
    }

    // Invoke precompiled native guest code with this isolate's pinned state.
    // The call's stack-owned context must not escape to asynchronous callbacks.
    // This is NOT a sandbox: arbitrary native code can still escape isolation.
    struct GuestOutcome {
        std::int32_t status = ORES_GUEST_FAILED;
        std::int64_t value = 0;
    };

    GuestOutcome invoke_guest_result(const IsolateHandle& handle, OresGuestEntry guest,
                                    std::int64_t input) {
        if (!guest) throw std::invalid_argument("guest entry is null");
        std::shared_ptr<IsolateVM> state;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state = checked(handle);
            if (state->phase != IsolatePhase::Running)
                throw std::logic_error("guest admission is closed");
            // Includes invocations waiting on guest_turn_mutex. Bound both
            // per-tenant queues and the aggregate VM workload before pinning.
            if (state->active_guest_calls >= state->config.max_inflight_guest_calls ||
                pinned_guest_calls_ >= limits_.max_inflight_guest_calls)
                throw std::logic_error("OresVM guest invocation budget exhausted");
            ++state->active_guest_calls;
            ++pinned_guest_calls_;
        }
        struct GuestPin final {
            OresVM* vm;
            std::shared_ptr<IsolateVM> state;
            ~GuestPin() noexcept {
                std::lock_guard<std::mutex> lock(vm->mutex_);
                --state->active_guest_calls;
                --vm->pinned_guest_calls_;
            }
        } pin{this, state};
        // Pin queued turns, and recheck admission after waiting for the tenant.
        // A shutdown closes admission even for already-waiting calls.
        static thread_local bool guest_active = false;
        if (guest_active) throw std::logic_error("nested guest entry forbidden");
        std::unique_lock<std::mutex> tenant_turn(state->guest_turn_mutex);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state->phase != IsolatePhase::Running)
                throw std::logic_error("guest invocation cancelled during shutdown");
        }
        guest_active = true;
        struct GuestThreadGuard {
            bool& active;
            ~GuestThreadGuard() noexcept { active = false; }
        } thread_guard{guest_active};
        GuestCall call{this, state, state->generation};
        OresIsolateContext ctx{ORES_ISOLATE_ABI_VERSION, ORES_ISOLATE_I64_SLOTS,
                              state->generation, &call, &guest_load_i64, &guest_store_i64};
        std::int64_t value = 0;
        std::int32_t status = ORES_GUEST_FAILED;
        try {
            status = guest(&ctx, input, &value);
        } catch (...) {
            // A native guest exception may occur AFTER a partial write to
            // tenant-owned globals. Never reuse this incarnation: quarantine
            // it, retain existing resource pins, and let the host explicitly
            // drain/retire the tenant. This is only trusted-in-process safety,
            // NOT containment against arbitrary native memory corruption.
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (state->phase == IsolatePhase::Running)
                    state->phase = IsolatePhase::Quiescing;
            }
            throw;
        }
        // A returned status is a typed guest result (e.g. checked overflow),
        // not a C++ trap. Preserve such outcomes without retiring the tenant.
        if (status != ORES_GUEST_OK) return {status, 0};
        return {ORES_GUEST_OK, value};
    }

    std::int64_t invoke_guest(const IsolateHandle& handle, OresGuestEntry guest,
                              std::int64_t input) {
        const auto outcome = invoke_guest_result(handle, guest, input);
        if (outcome.status != ORES_GUEST_OK)
            throw std::runtime_error("guest failed (status=" +
                std::to_string(outcome.status) + ")");
        return outcome.value;
    }

    void begin_shutdown(const IsolateHandle& handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto state = checked(handle);
        if (state->phase != IsolatePhase::Running &&
            state->phase != IsolatePhase::Created)
            throw std::logic_error("shutdown already started");
        state->phase = IsolatePhase::Quiescing;
    }

    void complete_shutdown(const IsolateHandle& handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto state = checked(handle);
        if (state->phase != IsolatePhase::Quiescing || !state->actors.empty() ||
            state->active_guest_calls != 0 || !state->resource_pins.empty())
            throw std::logic_error("actor or guest invocation still active");
        state->phase = IsolatePhase::Stopped;
    }

    void destroy(const IsolateHandle& handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto state = checked(handle);
        if (state->phase != IsolatePhase::Stopped)
            throw std::logic_error("isolate must stop before destruction");
        // Best-effort zeroing, not a guarantee of secure secret erasure.
        std::fill(state->heap.begin(), state->heap.end(), 0);
        state->guest_globals.fill(0);
        isolates_.erase(state->config.tenant_id);
        admitted_heap_bytes_ -= state->config.heap_limit_bytes;
    }

    IsolateSnapshot snapshot(const IsolateHandle& handle) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto state = checked(handle);
        return {state->config.tenant_id, state->generation, state->phase,
                static_cast<std::uint32_t>(state->heap.size()),
                state->actors.size(), state->active_guest_calls,
                state->resource_pins.size()};
    }

private:
    struct GuestCall final {
        OresVM* vm;
        std::shared_ptr<IsolateVM> state;
        std::uint64_t generation;
    };

    static int guest_load_i64(void* raw, std::uint32_t slot, std::int64_t* out) noexcept {
        if (!raw || !out || slot >= ORES_ISOLATE_I64_SLOTS) return 0;
        auto* call = static_cast<GuestCall*>(raw);
        std::lock_guard<std::mutex> lock(call->vm->mutex_);
        const auto& state = call->state;
        if (call->generation != state->generation ||
            (state->phase != IsolatePhase::Running &&
             state->phase != IsolatePhase::Quiescing) ||
            state->active_guest_calls == 0) return 0;
        *out = state->guest_globals[slot];
        return 1;
    }

    static int guest_store_i64(void* raw, std::uint32_t slot, std::int64_t value) noexcept {
        if (!raw || slot >= ORES_ISOLATE_I64_SLOTS) return 0;
        auto* call = static_cast<GuestCall*>(raw);
        std::lock_guard<std::mutex> lock(call->vm->mutex_);
        const auto& state = call->state;
        if (call->generation != state->generation ||
            (state->phase != IsolatePhase::Running &&
             state->phase != IsolatePhase::Quiescing) ||
            state->active_guest_calls == 0) return 0;
        state->guest_globals[slot] = value;
        return 1;
    }

    std::shared_ptr<IsolateVM> checked(const IsolateHandle& handle) const {
        auto state = handle.state_.lock();
        if (!state || handle.generation_ != state->generation)
            throw std::logic_error("expired isolate handle");
        const auto it = isolates_.find(state->config.tenant_id);
        if (it == isolates_.end() || it->second != state)
            throw std::logic_error("foreign or destroyed isolate handle");
        return state;
    }

    static void check_ref(const std::shared_ptr<IsolateVM>& state, const Allocation& ref) {
        if (ref.state_.lock() != state || ref.generation_ != state->generation ||
            ref.size_ == 0 ||
            static_cast<std::uint64_t>(ref.offset_) + ref.size_ > state->heap.size())
            throw std::logic_error("stale or foreign allocation");
    }

    mutable std::mutex mutex_;
    const OresVmLimits limits_;
    std::uint64_t admitted_heap_bytes_ = 0;
    std::size_t pinned_guest_calls_ = 0;
    std::unordered_map<std::string, std::shared_ptr<IsolateVM>> isolates_;
    std::uint64_t next_generation_ = 1;
};

} // namespace oreslang::runtime
