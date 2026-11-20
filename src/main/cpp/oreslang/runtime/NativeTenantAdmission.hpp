#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace oreslang::runtime {

// Trusted-host admission, NOT an OS sandbox, guest source lowering, or a
// hard-real-time hot-path primitive. Each actor has a distinct fixed-size
// byte region, and no raw region pointers are returned to another actor.
// Allocate/prefault/lock memory as appropriate BEFORE certified RT admission;
// the mutex and cleanup scrub here are deliberately outside RT turns.
class NativeTenantAdmission final {
    enum class Phase : std::uint8_t { stopped, running, quiescing };
    // Retire confidential actor bytes even when a host is destroyed without
    // a successful finish_stop. Volatile stores are observable side effects;
    // unlike memset, an optimizer must not discard this final wipe as dead.
    // This bounded cleanup is OFF the realtime actor execution path.
    static void secure_erase(void* memory, std::size_t bytes) noexcept {
        volatile std::uint8_t* cursor = static_cast<volatile std::uint8_t*>(memory);
        for (std::size_t i = 0; i < bytes; ++i) cursor[i] = 0;
    }
    struct Slot final {
        std::mutex mutex;
        std::unique_ptr<std::uint8_t[]> arena;
        std::unique_ptr<bool[]> active_actors;
        Phase phase = Phase::stopped;
        std::uint64_t generation = 0;
        std::size_t active_turns = 0;
        bool host_closed = false;
        std::size_t arena_size = 0;

        // State may be released only after the last Turn pin has dropped.
        // This destructor also runs for partially constructed State entries
        // when allocation of a later tenant arena throws.
        ~Slot() noexcept {
            if (arena) secure_erase(arena.get(), arena_size);
        }
    };
    struct State final {
        const std::size_t slots;
        const std::size_t actors;
        const std::size_t actor_bytes;
        std::mutex admission_gate;  // trusted-host ingress vs global close
        std::atomic<bool> closing{false};
        std::unique_ptr<Slot[]> entries;
        State(std::size_t slot_count, std::size_t actors_per_tenant,
              std::size_t region_bytes)
            : slots(slot_count), actors(actors_per_tenant),
              actor_bytes(region_bytes), entries(new Slot[slot_count]) {
            for (std::size_t i = 0; i < slots; ++i) {
                entries[i].arena_size = actors * actor_bytes;
                entries[i].arena = std::unique_ptr<std::uint8_t[]>(
                    new std::uint8_t[actors * actor_bytes]{});
                entries[i].active_actors =
                    std::unique_ptr<bool[]>(new bool[actors]{});
            }
        }
    };

public:
    // Experimental trusted-host quota, not a per-tenant RSS or OS cgroup limit.
    // Keep construction bounded before allocating any tenant backing memory.
    static constexpr std::size_t kMaxTenants = 4096;
    static constexpr std::size_t kMaxActorSlotsPerTenant = 4096;
    static constexpr std::size_t kMaxArenaBytes = 64u * 1024u * 1024u;

    struct Handle final {
        std::size_t slot = 0;
        std::uint64_t generation = 0;
        // A same-numbered generation in another NativeOresVM must never
        // admit this handle. Weak provenance avoids a registry lifetime leak.
        // This is an identity fence, never an authentication credential.
        Handle() = default;
        // Two-field construction is retained for legacy negative fixtures,
        // but it produces an unscoped handle that can never be admitted.
        Handle(std::size_t slot_value, std::uint64_t generation_value) noexcept
            : slot(slot_value), generation(generation_value) {}
    private:
        friend class NativeTenantAdmission;
        Handle(std::size_t slot_value, std::uint64_t generation_value,
               const std::shared_ptr<State>& owner) noexcept
            : slot(slot_value), generation(generation_value),
              issued_slot_(slot_value), issued_generation_(generation_value),
              registry_(owner) {}
        // Private issued values prevent a copied token from being retargeted
        // by rewriting its public diagnostic slot/generation fields.
        std::size_t issued_slot_ = std::numeric_limits<std::size_t>::max();
        std::uint64_t issued_generation_ = 0;
        std::weak_ptr<const State> registry_;
    };

    class Turn final {
    public:
        Turn(const Turn&) = delete;
        Turn& operator=(const Turn&) = delete;
        Turn(Turn&& rhs) noexcept
            : state_(std::move(rhs.state_)), slot_(rhs.slot_),
              generation_(rhs.generation_), actor_(rhs.actor_) {}
        Turn& operator=(Turn&& rhs) noexcept {
            if (this != &rhs) {
                release();
                state_ = std::move(rhs.state_);
                slot_ = rhs.slot_;
                generation_ = rhs.generation_;
                actor_ = rhs.actor_;
            }
            return *this;
        }
        ~Turn() { release(); }

        bool write(std::size_t offset, const void* data, std::size_t length) {
            return transfer(offset, const_cast<void*>(data), length, true);
        }
        bool read(std::size_t offset, void* destination,
                  std::size_t length) const {
            return transfer(offset, destination, length, false);
        }
        std::size_t actor_index() const noexcept { return actor_; }

    private:
        friend class NativeTenantAdmission;
        Turn(std::shared_ptr<State> state, std::size_t slot,
             std::uint64_t generation, std::size_t actor)
            : state_(std::move(state)), slot_(slot),
              generation_(generation), actor_(actor) {}

        bool transfer(std::size_t offset, void* buffer, std::size_t length,
                      bool writing) const {
            if (!state_ || (length != 0 && buffer == nullptr) ||
                offset > state_->actor_bytes ||
                length > state_->actor_bytes - offset) return false;
            Slot& slot = state_->entries[slot_];
            std::lock_guard<std::mutex> guard(slot.mutex);
            // Quiescing permits already-admitted turns to finish; STOP does
            // not revoke their memory while the physical execution is active.
            // Destruction of the owning host revokes even pinned turn access.
            if (state_->closing.load(std::memory_order_acquire) ||
                slot.host_closed || slot.generation != generation_ ||
                slot.phase == Phase::stopped ||
                !slot.active_actors[actor_]) return false;
            std::uint8_t* where =
                slot.arena.get() + actor_ * state_->actor_bytes + offset;
            if (length != 0) {
                if (writing) std::memcpy(where, buffer, length);
                else std::memcpy(buffer, where, length);
            }
            return true;
        }

        void release() noexcept {
            if (!state_) return;
            Slot& slot = state_->entries[slot_];
            {
                std::lock_guard<std::mutex> guard(slot.mutex);
                if (slot.generation == generation_ && slot.active_actors[actor_]) {
                    slot.active_actors[actor_] = false;
                    --slot.active_turns;
                }
            }
            state_.reset();
        }

        std::shared_ptr<State> state_;
        std::size_t slot_;
        std::uint64_t generation_;
        std::size_t actor_;
    };

    NativeTenantAdmission(std::size_t max_tenants,
                          std::size_t actors_per_tenant,
                          std::size_t actor_region_bytes) {
        if (max_tenants == 0 || max_tenants > kMaxTenants ||
            actors_per_tenant == 0 ||
            actors_per_tenant > kMaxActorSlotsPerTenant ||
            actor_region_bytes == 0 ||
            actors_per_tenant >
                std::numeric_limits<std::size_t>::max() / actor_region_bytes ||
            max_tenants > std::numeric_limits<std::size_t>::max() /
                (actors_per_tenant * actor_region_bytes) ||
            max_tenants * (actors_per_tenant * actor_region_bytes) >
                kMaxArenaBytes)
            throw std::invalid_argument("unbounded native tenant admission");
        state_ = std::make_shared<State>(max_tenants, actors_per_tenant,
                                         actor_region_bytes);
    }
    NativeTenantAdmission(const NativeTenantAdmission&) = delete;
    NativeTenantAdmission& operator=(const NativeTenantAdmission&) = delete;

    // Trusted off-carrier, nonblocking admission shutdown. Safe to call
    // concurrently with other public methods while the registry object lives.
    // Admission and close serialize on the ingress gate, before any slot
    // locks. After the closing flag is published, no open/enter can admit.
    // Physical turns retain backing memory but may no longer read/write.
    // This does NOT join or forcibly preempt uncooperative guest callbacks.
    void close() noexcept {
        {
            std::lock_guard<std::mutex> admission(state_->admission_gate);
            state_->closing.store(true, std::memory_order_release);
        }
        for (std::size_t i = 0; i < state_->slots; ++i) {
            Slot& slot = state_->entries[i];
            std::lock_guard<std::mutex> guard(slot.mutex);
            slot.host_closed = true;
            if (slot.phase == Phase::running) slot.phase = Phase::quiescing;
        }
    }

    // Object destruction still needs an external producer/API lifetime fence.
    // Call close(), join every producer/actor callback, THEN destroy this VM.
    // Existing pins keep State allocated until their final RAII release.
    ~NativeTenantAdmission() noexcept { close(); }

    // A handle is a trusted-host identity/fencing token, NOT authentication.
    // The ingress broker must authenticate and authorize tenant identity.
    // Matching slot+generation is insufficient across separate registries.
    std::optional<Handle> open() {
        std::lock_guard<std::mutex> admission(state_->admission_gate);
        if (state_->closing.load(std::memory_order_acquire))
            return std::nullopt;
        for (std::size_t i = 0; i < state_->slots; ++i) {
            Slot& slot = state_->entries[i];
            std::lock_guard<std::mutex> guard(slot.mutex);
            if (state_->closing.load(std::memory_order_acquire) ||
                slot.host_closed || slot.phase != Phase::stopped ||
                slot.generation == std::numeric_limits<std::uint64_t>::max())
                continue;
            ++slot.generation;
            slot.phase = Phase::running;
            return Handle{i, slot.generation, state_};
        }
        return std::nullopt;
    }

    std::optional<Turn> enter(Handle handle, std::size_t actor_index) {
        std::lock_guard<std::mutex> admission(state_->admission_gate);
        if (!owns(handle) || handle.slot >= state_->slots ||
            actor_index >= state_->actors || handle.generation == 0)
            return std::nullopt;
        Slot& slot = state_->entries[handle.slot];
        std::lock_guard<std::mutex> guard(slot.mutex);
        if (state_->closing.load(std::memory_order_acquire) ||
            slot.host_closed || slot.phase != Phase::running ||
            slot.generation != handle.generation ||
            slot.active_actors[actor_index]) return std::nullopt;
        slot.active_actors[actor_index] = true;
        ++slot.active_turns;
        return Turn(state_, handle.slot, handle.generation, actor_index);
    }

    bool request_stop(Handle handle) {
        if (!owns(handle) || handle.slot >= state_->slots ||
            handle.generation == 0) return false;
        Slot& slot = state_->entries[handle.slot];
        std::lock_guard<std::mutex> guard(slot.mutex);
        if (state_->closing.load(std::memory_order_acquire) ||
            slot.host_closed || slot.generation != handle.generation ||
            slot.phase != Phase::running) return false;
        slot.phase = Phase::quiescing;
        return true;
    }

    // Trusted off-carrier supervisor operation; scrub is not RT bounded.
    // Never recycle a tenant while an actor turn holds a physical lease.
    bool finish_stop(Handle handle) {
        if (!owns(handle) || handle.slot >= state_->slots ||
            handle.generation == 0) return false;
        Slot& slot = state_->entries[handle.slot];
        std::lock_guard<std::mutex> guard(slot.mutex);
        if (state_->closing.load(std::memory_order_acquire) ||
            slot.host_closed || slot.generation != handle.generation ||
            slot.phase != Phase::quiescing || slot.active_turns != 0)
            return false;
        secure_erase(slot.arena.get(), slot.arena_size);
        slot.phase = Phase::stopped;
        return true;
    }

private:
    bool owns(const Handle& handle) const noexcept {
        const auto lifetime = handle.registry_.lock();
        return lifetime && lifetime.get() == state_.get() &&
            handle.generation != 0 &&
            handle.slot == handle.issued_slot_ &&
            handle.generation == handle.issued_generation_;
    }
    std::shared_ptr<State> state_;
};

} // namespace oreslang::runtime
