#pragma once

#include "oreslang/runtime/OresIsolateVM.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
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

    // Opaque host-issued object ticket: slot and generation are never guest
    // pointers. A stale ticket cannot authorize a reused physical slot.
    class ObjectHandle final {
        friend class OresVM;
        std::weak_ptr<IsolateVM> state_;
        std::uint64_t tenant_generation_ = 0;
        std::size_t slot_ = 0;
        std::uint64_t object_generation_ = 0;
        ObjectHandle(const std::shared_ptr<IsolateVM>& state,
                     std::size_t slot, std::uint64_t generation)
            : state_(state), tenant_generation_(state->generation),
              slot_(slot), object_generation_(generation) {}
    public:
        ObjectHandle() = default;
    };

    class ActorTurnTicket final {
        friend class OresVM;
        std::weak_ptr<IsolateVM> state_;
        std::uint64_t tenant_generation_ = 0;
        std::uint64_t actor_id_ = 0;
        std::uint64_t actor_generation_ = 0;
        std::uint64_t turn_serial_ = 0;
        std::thread::id carrier_{};
        std::uint64_t carrier_incarnation_ = 0;
        bool cleanup_only_ = false;
        ActorTurnTicket(const std::shared_ptr<IsolateVM>& state,
                        std::uint64_t actor_id, std::uint64_t actor_generation,
                        std::uint64_t serial, std::thread::id carrier,
                        std::uint64_t carrier_incarnation, bool cleanup_only)
            : state_(state), tenant_generation_(state->generation),
              actor_id_(actor_id), actor_generation_(actor_generation),
              turn_serial_(serial), carrier_(carrier),
              carrier_incarnation_(carrier_incarnation),
              cleanup_only_(cleanup_only) {}
    public:
        ActorTurnTicket() = default;
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
            config.max_resource_pins == 0 || config.max_object_slots == 0 ||
            config.max_object_slots > 65536)
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

    // Host-only bounded memory access. This does NOT expose a raw heap base
    // or install compiler-enforced checks into native guest machine code.
    // The broker must authenticate actors and bind compiler-generated object
    // operations to the current actor turn before exposing a guest ABI.
    ObjectHandle allocate_object(const IsolateHandle& handle,
                                 std::uint32_t bytes, std::uint32_t alignment = 8) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto state = checked(handle);
        if (state->phase != IsolatePhase::Running)
            throw std::logic_error("tenant object admission closed");
        return allocate_object_locked(state, bytes, alignment, 0, 0);
    }

    std::uint8_t read_object_byte(const IsolateHandle& handle,
                                  const ObjectHandle& object,
                                  std::uint32_t index) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto state = checked(handle);
        if (state->phase != IsolatePhase::Running &&
            state->phase != IsolatePhase::Quiescing)
            throw std::logic_error("tenant object access closed");
        const auto& slot = checked_object(state, object);
        if (index >= slot.size)
            throw std::out_of_range("object read exceeds allocated object");
        return state->heap[static_cast<std::size_t>(slot.offset) + index];
    }

    void write_object_byte(const IsolateHandle& handle,
                           const ObjectHandle& object,
                           std::uint32_t index, std::uint8_t value) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto state = checked(handle);
        if (state->phase != IsolatePhase::Running &&
            state->phase != IsolatePhase::Quiescing)
            throw std::logic_error("tenant object access closed");
        const auto& slot = checked_object(state, object);
        if (index >= slot.size)
            throw std::out_of_range("object write exceeds allocated object");
        state->heap[static_cast<std::size_t>(slot.offset) + index] = value;
    }

    // Retirement and physical slot reuse are serialized with all accesses by
    // the host lock. Retired bytes must be zero before a new token is issued.
    void retire_object(const IsolateHandle& handle, const ObjectHandle& object) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto state = checked(handle);
        if (state->phase != IsolatePhase::Running &&
            state->phase != IsolatePhase::Quiescing)
            throw std::logic_error("tenant object retirement closed");
        auto& slot = checked_object(state, object);
        scrub_object(state, slot);
        // A UINT64_MAX descriptor remains retired forever; no ABA wrap.
    }

    // These are authenticated trusted-host *turn* tickets, not native
    // machine-code sandbox or user-creatable ActorRef values. A ticket is
    // admitted on one carrier pthread and cannot be used from another.
    ActorTurnTicket enter_actor_turn(const IsolateHandle& handle,
                                     const ActorTicket& actor) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto state = checked(handle);
        if (state->phase != IsolatePhase::Running)
            throw std::logic_error("actor physical turn admission closed");
        return admit_actor_turn_locked(state, actor, false);
    }

    // Quiescing seals new work but the supervisor must still be able to
    // reclaim actor-owned bytes. This dedicated host-only path admits ONLY
    // retirement/cleanup, not allocations or newly scheduled guest work.
    ActorTurnTicket enter_actor_cleanup_turn(const IsolateHandle& handle,
                                             const ActorTicket& actor) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto state = checked(handle);
        if (state->phase != IsolatePhase::Quiescing)
            throw std::logic_error("cleanup-only turn requires quiescing");
        return admit_actor_turn_locked(state, actor, true);
    }

    void leave_actor_turn(const IsolateHandle& handle, const ActorTicket& actor,
                          const ActorTurnTicket& turn) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto state = checked(handle);
        require_live_turn(state, actor, turn);
        state->actor_turns.erase(actor.id_);
        // Release physical-carrier admission only after the registered turn
        // has been revoked. All other tenant/root-VM turns on this pthread
        // remain denied until that happens.
        active_carrier_tenant_ = nullptr;
        active_carrier_turn_serial_ = 0;
    }

    // All actor-owned object operations use the exact live actor and current
    // physical turn. Host-only legacy object APIs deliberately cannot read
    // or retire actor-owned objects. No guest gets a native heap address.
    ObjectHandle allocate_actor_object(const IsolateHandle& handle,
                                      const ActorTicket& actor,
                                      const ActorTurnTicket& turn,
                                      std::uint32_t bytes,
                                      std::uint32_t alignment = 8) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto state = checked(handle);
        if (state->phase != IsolatePhase::Running)
            throw std::logic_error("actor object allocation admission closed");
        require_live_turn(state, actor, turn);
        return allocate_object_locked(state, bytes, alignment,
                                      actor.id_, actor.actor_generation_);
    }

    std::uint8_t read_actor_object_byte(const IsolateHandle& handle,
                                       const ActorTicket& actor,
                                       const ActorTurnTicket& turn,
                                       const ObjectHandle& object,
                                       std::uint32_t index) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto state = checked(handle);
        if (state->phase != IsolatePhase::Running &&
            state->phase != IsolatePhase::Quiescing)
            throw std::logic_error("actor object access closed");
        require_live_turn(state, actor, turn);
        if (turn.cleanup_only_)
            throw std::logic_error("cleanup-only turn cannot read actor objects");
        const auto& slot = checked_object(
            state, object, actor.id_, actor.actor_generation_);
        if (index >= slot.size)
            throw std::out_of_range("actor object read out of bounds");
        return state->heap[static_cast<std::size_t>(slot.offset) + index];
    }

    void write_actor_object_byte(const IsolateHandle& handle,
                                 const ActorTicket& actor,
                                 const ActorTurnTicket& turn,
                                 const ObjectHandle& object,
                                 std::uint32_t index, std::uint8_t value) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto state = checked(handle);
        if (state->phase != IsolatePhase::Running &&
            state->phase != IsolatePhase::Quiescing)
            throw std::logic_error("actor object access closed");
        require_live_turn(state, actor, turn);
        if (turn.cleanup_only_)
            throw std::logic_error("cleanup-only turn cannot write actor objects");
        const auto& slot = checked_object(
            state, object, actor.id_, actor.actor_generation_);
        if (index >= slot.size)
            throw std::out_of_range("actor object write out of bounds");
        state->heap[static_cast<std::size_t>(slot.offset) + index] = value;
    }

    void retire_actor_object(const IsolateHandle& handle,
                             const ActorTicket& actor,
                             const ActorTurnTicket& turn,
                             const ObjectHandle& object) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto state = checked(handle);
        if (state->phase != IsolatePhase::Running &&
            state->phase != IsolatePhase::Quiescing)
            throw std::logic_error("actor object retirement closed");
        require_live_turn(state, actor, turn);
        auto& slot = checked_object(
            state, object, actor.id_, actor.actor_generation_);
        scrub_object(state, slot);
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
        if (state->actor_turns.count(actor.id_))
            throw std::logic_error("actor has an outstanding physical turn");
        for (const auto& object : state->object_slots)
            if (object.live && object.owner_actor_id == actor.id_ &&
                object.owner_actor_generation == actor.actor_generation_)
                throw std::logic_error("actor has live owned object allocations");
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
            ~GuestThreadGuard() noexcept {
                // Revoke on every exit, including exceptions and error returns.
                OresVM::active_guest_call_ = nullptr;
                OresVM::active_guest_cookie_ = 0;
                active = false;
            }
        } thread_guard{guest_active};
        GuestCall call{this, state, state->generation};
        // The guest receives a single-use opaque cookie, not a stack address.
        // Only callbacks on this thread with this ticket may resolve the call.
        const auto cookie = issue_guest_cookie();
        active_guest_call_ = &call;
        active_guest_cookie_ = cookie;
        OresIsolateContext ctx{ORES_ISOLATE_ABI_VERSION, ORES_ISOLATE_I64_SLOTS,
                              state->generation, reinterpret_cast<void*>(cookie),
                              &guest_load_i64, &guest_store_i64};
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
            state->active_guest_calls != 0 || !state->resource_pins.empty() ||
            !state->actor_turns.empty() ||
            std::any_of(state->object_slots.begin(), state->object_slots.end(),
                        [](const IsolateObjectSlot& slot) { return slot.live; }))
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
                state->resource_pins.size(),
                static_cast<std::size_t>(std::count_if(
                    state->object_slots.begin(), state->object_slots.end(),
                    [](const IsolateObjectSlot& slot) { return slot.live; }))};
    }

private:
    struct GuestCall final {
        OresVM* vm;
        std::shared_ptr<IsolateVM> state;
        std::uint64_t generation;
    };

    // Host-only callback fencing, not a sandbox for arbitrary native code.
    // The integer/void* cookie representation is native-host-specific and
    // must NEVER be dereferenced, treated as an address, or serialized.
    // Exhaustion fails closed rather than allowing any cookie to be reused.
    inline static std::atomic<std::uintptr_t> next_guest_cookie_{1};
    inline static thread_local GuestCall* active_guest_call_ = nullptr;
    inline static thread_local std::uintptr_t active_guest_cookie_ = 0;

    static std::uintptr_t issue_guest_cookie() {
        auto next = next_guest_cookie_.load(std::memory_order_relaxed);
        for (;;) {
            if (next == std::numeric_limits<std::uintptr_t>::max())
                throw std::overflow_error("guest invocation cookies exhausted");
            if (next_guest_cookie_.compare_exchange_weak(
                    next, next + 1, std::memory_order_relaxed,
                    std::memory_order_relaxed))
                return next;
        }
    }

    static int guest_load_i64(void* raw, std::uint32_t slot, std::int64_t* out) noexcept {
        if (!raw || !out || slot >= ORES_ISOLATE_I64_SLOTS) return 0;
        // Check before any dereference: a stale token cannot alias a new
        // invocation that happens to reuse the former stack address.
        if (!active_guest_call_ ||
            raw != reinterpret_cast<void*>(active_guest_cookie_)) return 0;
        auto* call = active_guest_call_;
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
        // Check before any dereference: a stale token cannot alias a new
        // invocation that happens to reuse the former stack address.
        if (!active_guest_call_ ||
            raw != reinterpret_cast<void*>(active_guest_cookie_)) return 0;
        auto* call = active_guest_call_;
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

    // All helper functions require mutex_ to be held by their caller.
    // Unlike std::thread::id or pthread_t, this lifetime value never
    // repeats when the OS recycles a carrier-thread identity.
    inline static std::atomic<std::uint64_t> next_carrier_incarnation_{1};
    // Process-wide (across all OresVM objects) thread-local physical turn
    // admission, not per-TenantVM bookkeeping alone. Without this fence a
    // callback on one pthread could enter a second tenant while retaining
    // its first actor's active capabilities. The pool label is not an
    // isolation boundary; fail closed on nested cross-tenant turns.
    inline static thread_local const IsolateVM* active_carrier_tenant_ = nullptr;
    inline static thread_local std::uint64_t active_carrier_turn_serial_ = 0;
    static std::uint64_t issue_carrier_incarnation() {
        auto next = next_carrier_incarnation_.load(std::memory_order_relaxed);
        for (;;) {
            if (next == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("carrier lifetime serial exhausted");
            if (next_carrier_incarnation_.compare_exchange_weak(
                    next, next + 1, std::memory_order_relaxed,
                    std::memory_order_relaxed))
                return next;
        }
    }
    static std::uint64_t current_carrier_incarnation() {
        static thread_local const std::uint64_t incarnation =
            issue_carrier_incarnation();
        return incarnation;
    }

    static ActorTurnTicket admit_actor_turn_locked(
        const std::shared_ptr<IsolateVM>& state, const ActorTicket& actor,
        bool cleanup_only) {
        require_live_actor(state, actor);
        if (active_carrier_tenant_ != nullptr || active_carrier_turn_serial_ != 0)
            throw std::logic_error("carrier already owns an actor turn");
        if (state->actor_turns.count(actor.id_))
            throw std::logic_error("actor already owns a physical turn");
        const auto carrier = std::this_thread::get_id();
        const auto carrier_incarnation = current_carrier_incarnation();
        for (const auto& active : state->actor_turns)
            if (active.second.carrier == carrier)
                throw std::logic_error("reentrant actor carrier turn denied");
        if (state->next_actor_turn_serial ==
            std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("actor turn serial exhausted");
        const auto serial = state->next_actor_turn_serial++;
        // Construct the ticket first; then publish it into the tenant's
        // turn ledger. The host mutex protects every ledger mutation, while
        // this thread-local fence disallows entering any other tenant turn.
        ActorTurnTicket ticket(state, actor.id_, actor.actor_generation_,
                               serial, carrier, carrier_incarnation, cleanup_only);
        state->actor_turns.emplace(actor.id_, IsolateActorTurnRecord{
            actor.actor_generation_, serial, carrier, carrier_incarnation});
        active_carrier_tenant_ = state.get();
        active_carrier_turn_serial_ = serial;
        return ticket;
    }

    // No actor ID/name is accepted as authority without a host-issued
    // incarnation ticket AND a physical carrier-bound turn ticket.
    static void require_live_actor(const std::shared_ptr<IsolateVM>& state,
                                   const ActorTicket& actor) {
        const auto it = state->actors.find(actor.id_);
        if (actor.id_ == 0 || actor.state_.lock() != state ||
            actor.generation_ != state->generation ||
            it == state->actors.end() ||
            it->second != actor.actor_generation_)
            throw std::logic_error("foreign or retired actor incarnation");
    }

    static void require_live_turn(const std::shared_ptr<IsolateVM>& state,
                                  const ActorTicket& actor,
                                  const ActorTurnTicket& turn) {
        require_live_actor(state, actor);
        if (turn.state_.lock() != state ||
            turn.tenant_generation_ != state->generation ||
            turn.actor_id_ != actor.id_ ||
            turn.actor_generation_ != actor.actor_generation_ ||
            turn.turn_serial_ == 0 ||
            turn.carrier_ != std::this_thread::get_id() ||
            turn.carrier_incarnation_ != current_carrier_incarnation() ||
            active_carrier_tenant_ != state.get() ||
            active_carrier_turn_serial_ != turn.turn_serial_)
            throw std::logic_error("foreign, stale or migrated actor turn");
        const auto it = state->actor_turns.find(actor.id_);
        if (it == state->actor_turns.end() ||
            it->second.incarnation != actor.actor_generation_ ||
            it->second.serial != turn.turn_serial_ ||
            it->second.carrier != turn.carrier_ ||
            it->second.carrier_incarnation != turn.carrier_incarnation_)
            throw std::logic_error("actor physical turn no longer active");
    }

    static ObjectHandle allocate_object_locked(
        const std::shared_ptr<IsolateVM>& state,
        std::uint32_t bytes, std::uint32_t alignment,
        std::uint64_t owner_id, std::uint64_t owner_generation) {
        if (bytes == 0 || alignment == 0 ||
            (alignment & (alignment - 1)) != 0 ||
            alignment > state->config.heap_limit_bytes)
            throw std::invalid_argument("object bytes and alignment invalid");
        // A retired descriptor is scrubbed before its generation is ever
        // incremented. Reuse is checked for size/alignment compatibility.
        for (std::size_t i = 0; i < state->object_slots.size(); ++i) {
            auto& slot = state->object_slots[i];
            if (slot.live ||
                slot.generation == std::numeric_limits<std::uint64_t>::max() ||
                slot.capacity < bytes || slot.offset % alignment != 0)
                continue;
            slot.size = bytes;
            slot.owner_actor_id = owner_id;
            slot.owner_actor_generation = owner_generation;
            slot.live = true;
            return ObjectHandle(state, i, slot.generation);
        }
        if (state->object_slots.size() >= state->config.max_object_slots)
            throw std::bad_alloc();
        const auto limit = static_cast<std::size_t>(state->config.heap_limit_bytes);
        const auto used = state->heap.size();
        const auto padding = (alignment - (used % alignment)) % alignment;
        if (used > limit || padding > limit - used ||
            bytes > limit - used - padding)
            throw std::bad_alloc();
        const auto offset = used + padding;
        state->object_slots.push_back(IsolateObjectSlot{
            static_cast<std::uint32_t>(offset), bytes, bytes, 1, true,
            owner_id, owner_generation});
        try {
            state->heap.resize(offset + bytes, 0);
        } catch (...) {
            state->object_slots.pop_back();
            throw;
        }
        return ObjectHandle(state, state->object_slots.size() - 1, 1);
    }

    static void scrub_object(const std::shared_ptr<IsolateVM>& state,
                             IsolateObjectSlot& slot) {
        // The host lock serializes every read/write/retire/reuse. The pointer
        // is never exposed; stale handles cannot alias a new object.
        std::fill(state->heap.begin() + slot.offset,
                  state->heap.begin() + static_cast<std::size_t>(slot.offset) +
                      slot.capacity, std::uint8_t{0});
        slot.live = false;
        slot.size = 0;
        slot.owner_actor_id = 0;
        slot.owner_actor_generation = 0;
        if (slot.generation != std::numeric_limits<std::uint64_t>::max())
            ++slot.generation;
    }

    static IsolateObjectSlot& checked_object(
        const std::shared_ptr<IsolateVM>& state, const ObjectHandle& object,
        std::uint64_t owner_id = 0,
        std::uint64_t owner_generation = 0) {
        if (object.state_.lock() != state ||
            object.tenant_generation_ != state->generation ||
            object.slot_ >= state->object_slots.size())
            throw std::logic_error("foreign or stale tenant object ticket");
        auto& slot = state->object_slots[object.slot_];
        if (!slot.live || slot.generation != object.object_generation_ ||
            slot.owner_actor_id != owner_id ||
            slot.owner_actor_generation != owner_generation ||
            slot.size == 0 || slot.size > slot.capacity ||
            static_cast<std::uint64_t>(slot.offset) + slot.capacity >
                state->heap.size())
            throw std::logic_error("retired or invalid tenant object");
        return slot;
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
