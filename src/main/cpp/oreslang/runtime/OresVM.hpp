#pragma once

#include "oreslang/runtime/OresIsolateAbi.h"

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
enum class IsolateMode { TrustedInProcess, SandboxedProcess };
enum class IsolatePhase { Created, Running, Quiescing, Stopped };

struct IsolateConfig {
    std::string tenant_id;
    std::uint32_t heap_limit_bytes = 0;
    std::uint32_t max_actors = 0;
    IsolateMode mode = IsolateMode::TrustedInProcess;
};

struct IsolateSnapshot {
    std::string tenant_id;
    std::uint64_t generation = 0;
    IsolatePhase phase = IsolatePhase::Created;
    std::uint32_t used_bytes = 0;
    std::size_t active_actors = 0;
    std::size_t active_guest_calls = 0;
};

// OresVM is the host's isolate registry and admission boundary. It owns no
// user-code scheduler and exposes only checked offset-based memory operations;
// no native heap address can escape through this API.
class OresVM final {
    struct State {
        explicit State(IsolateConfig value, std::uint64_t epoch)
            : config(std::move(value)), generation(epoch) {}
        IsolateConfig config;
        const std::uint64_t generation;
        IsolatePhase phase = IsolatePhase::Created;
        std::vector<std::uint8_t> heap;  // bounded logical bump arena
        std::unordered_map<std::uint64_t, std::uint64_t> actors;
        std::uint64_t next_actor_generation = 1;
        std::size_t active_guest_calls = 0;
        std::mutex guest_turn_mutex;  // trusted, serialized non-reentrant guest turns
        std::array<std::int64_t, ORES_ISOLATE_I64_SLOTS> guest_globals{};
    };

public:
    class IsolateHandle final {
        friend class OresVM;
        std::weak_ptr<State> state_;
        std::uint64_t generation_ = 0;
        explicit IsolateHandle(const std::shared_ptr<State>& state)
            : state_(state), generation_(state->generation) {}
    public:
        IsolateHandle() = default;
    };

    class Allocation final {
        friend class OresVM;
        std::weak_ptr<State> state_;
        std::uint64_t generation_ = 0;
        std::uint32_t offset_ = 0;
        std::uint32_t size_ = 0;
        Allocation(const std::shared_ptr<State>& state,
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
        std::weak_ptr<State> state_;
        std::uint64_t generation_ = 0;
        std::uint64_t id_ = 0;
        std::uint64_t actor_generation_ = 0;
        ActorTicket(const std::shared_ptr<State>& state, std::uint64_t id,
                    std::uint64_t actor_generation)
            : state_(state), generation_(state->generation), id_(id),
              actor_generation_(actor_generation) {}
    public:
        ActorTicket() = default;
    };

    OresVM() = default;
    OresVM(const OresVM&) = delete;
    OresVM& operator=(const OresVM&) = delete;

    IsolateHandle create_isolate(IsolateConfig config) {
        if (config.tenant_id.empty() || config.heap_limit_bytes == 0 ||
            config.max_actors == 0)
            throw std::invalid_argument("isolate requires an ID and positive quotas");
        if (config.mode != IsolateMode::TrustedInProcess)
            throw std::logic_error("sandboxed-process isolation is not implemented");
        std::lock_guard<std::mutex> lock(mutex_);
        if (isolates_.count(config.tenant_id))
            throw std::logic_error("tenant already has an isolate");
        if (next_generation_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("isolate generations exhausted");
        auto state = std::make_shared<State>(std::move(config), next_generation_++);
        isolates_.emplace(state->config.tenant_id, state);
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
        state->actors.erase(it);
    }

    // Invoke precompiled native guest code with this isolate's pinned state.
    // The call's stack-owned context must not escape to asynchronous callbacks.
    // This is NOT a sandbox: arbitrary native code can still escape isolation.
    std::int64_t invoke_guest(const IsolateHandle& handle, OresGuestEntry guest,
                              std::int64_t input) {
        if (!guest) throw std::invalid_argument("guest entry is null");
        std::shared_ptr<State> state;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state = checked(handle);
            if (state->phase != IsolatePhase::Running)
                throw std::logic_error("guest admission is closed");
            if (state->active_guest_calls == std::numeric_limits<std::size_t>::max())
                throw std::overflow_error("too many pinned guest invocations");
            ++state->active_guest_calls;
        }
        struct GuestPin final {
            OresVM* vm;
            std::shared_ptr<State> state;
            ~GuestPin() noexcept {
                std::lock_guard<std::mutex> lock(vm->mutex_);
                --state->active_guest_calls;
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
        return guest(&ctx, input);
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
            state->active_guest_calls != 0)
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
    }

    IsolateSnapshot snapshot(const IsolateHandle& handle) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto state = checked(handle);
        return {state->config.tenant_id, state->generation, state->phase,
                static_cast<std::uint32_t>(state->heap.size()),
                state->actors.size(), state->active_guest_calls};
    }

private:
    struct GuestCall final {
        OresVM* vm;
        std::shared_ptr<State> state;
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

    std::shared_ptr<State> checked(const IsolateHandle& handle) const {
        auto state = handle.state_.lock();
        if (!state || handle.generation_ != state->generation)
            throw std::logic_error("expired isolate handle");
        const auto it = isolates_.find(state->config.tenant_id);
        if (it == isolates_.end() || it->second != state)
            throw std::logic_error("foreign or destroyed isolate handle");
        return state;
    }

    static void check_ref(const std::shared_ptr<State>& state, const Allocation& ref) {
        if (ref.state_.lock() != state || ref.generation_ != state->generation ||
            ref.size_ == 0 ||
            static_cast<std::uint64_t>(ref.offset_) + ref.size_ > state->heap.size())
            throw std::logic_error("stale or foreign allocation");
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<State>> isolates_;
    std::uint64_t next_generation_ = 1;
};

} // namespace oreslang::runtime
