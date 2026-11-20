#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>

namespace oreslang::runtime {

// HOST-ONLY fixed-budget reference primitive for a candidate .atto immutable
// container root. Not an implementation of the guest critical syntax, and
// not a security boundary. Scheduler callers must retry Busy outside a turn.
// A root is a frozen array of scalars in a distinct shared region, never an
// actor-private ref32 word. No allocator, I/O, callback or wait inside commit.
template<std::size_t Slots = 32, std::size_t Width = 8>
class CriticalImmutableSnapshotRoot final {
    static_assert(Slots >= 2 && Slots <= 65536);
    static_assert(Width > 0 && Width <= 1024);

    enum class State : std::uint8_t { Free, Building, Ready, Published, Retired };
    struct Node {
        std::atomic<State> state{State::Free};
        std::uint64_t generation = 0;
        std::size_t readers = 0;
        std::array<std::int64_t, Width> data{};
    };

public:
    enum class Result { Ok, Busy, Conflict, Capacity, Stale, Exhausted };
    class Lease {
        friend class CriticalImmutableSnapshotRoot;
        std::size_t slot_ = Slots;
        std::uint64_t generation_ = 0;
        std::uint64_t revision_ = 0;
        const CriticalImmutableSnapshotRoot* owner_ = nullptr;
        Lease(const CriticalImmutableSnapshotRoot* owner, std::size_t slot,
              std::uint64_t generation, std::uint64_t revision) noexcept
            : slot_(slot), generation_(generation), revision_(revision), owner_(owner) {}
    public:
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&&) = delete;
        Lease& operator=(Lease&&) = delete;
        // Release is explicit: destructors must not block an actor carrier or
        // terminate the whole process when a misbehaving actor leaks a lease.
        std::uint64_t revision() const noexcept { return revision_; }
    };
    class Candidate {
        friend class CriticalImmutableSnapshotRoot;
        std::size_t slot_ = Slots;
        std::uint64_t generation_ = 0;
        const CriticalImmutableSnapshotRoot* owner_ = nullptr;
        Candidate(const CriticalImmutableSnapshotRoot* owner, std::size_t slot,
                  std::uint64_t generation) noexcept
            : slot_(slot), generation_(generation), owner_(owner) {}
    public:
        Candidate() = default;
        Candidate(const Candidate&) = delete;
        Candidate& operator=(const Candidate&) = delete;
        Candidate(Candidate&&) = delete;
        Candidate& operator=(Candidate&&) = delete;
        // Discard is explicit; abandoned candidates retain bounded capacity
        // until actor/supervisor cleanup rather than crashing the OS process.
    };

    CriticalImmutableSnapshotRoot() {
        nodes_[0].generation = 1;
        nodes_[0].state = State::Published;
    }
    CriticalImmutableSnapshotRoot(const CriticalImmutableSnapshotRoot&) = delete;
    CriticalImmutableSnapshotRoot& operator=(const CriticalImmutableSnapshotRoot&) = delete;

    Result try_snapshot(Lease& lease) noexcept {
        if (lease.owner_) return Result::Stale;
        std::unique_lock<std::mutex> lock(mu_, std::try_to_lock);
        if (!lock.owns_lock()) return Result::Busy;
        Node& node = nodes_[root_];
        if (node.readers == std::numeric_limits<std::size_t>::max()) return Result::Exhausted;
        ++node.readers;
        lease.owner_ = this;
        lease.slot_ = root_;
        lease.generation_ = node.generation;
        lease.revision_ = revision_;
        return Result::Ok;
    }

    // A caller must hold its Lease while reading. No storage can be reused
    // until the last lease of an old root is released after publication.
    Result read(const Lease& lease, std::size_t index, std::int64_t& output) const noexcept {
        if (index >= Width || lease.owner_ != this || lease.slot_ >= Slots)
            return Result::Stale;
        const Node& node = nodes_[lease.slot_];
        // The caller owns a live lease: this node cannot be recycled, so its
        // generation and frozen payload are stable. Do not read mutable
        // reader-count/state here: a writer can retire the root concurrently.
        if (node.generation != lease.generation_) return Result::Stale;
        output = node.data[index];
        return Result::Ok;
    }

    Result try_stage(const Lease& basis, std::size_t index, std::int64_t value,
                     Candidate& output) noexcept {
        if (output.owner_ || basis.owner_ != this || basis.slot_ >= Slots || index >= Width)
            return Result::Stale;
        std::unique_lock<std::mutex> lock(mu_, std::try_to_lock);
        if (!lock.owns_lock()) return Result::Busy;
        const Node& base = nodes_[basis.slot_];
        if (base.generation != basis.generation_ || base.readers == 0 ||
            (base.state != State::Published && base.state != State::Retired))
            return Result::Stale;
        for (std::size_t slot = 0; slot < Slots; ++slot) {
            Node& next = nodes_[slot];
            if (next.state.load(std::memory_order_relaxed) != State::Free) continue;
            if (next.generation == std::numeric_limits<std::uint64_t>::max())
                return Result::Exhausted;
            ++next.generation;
            next.state.store(State::Building, std::memory_order_relaxed);
            lock.unlock();
            // Copy + transform outside critical; basis stays pinned by caller.
            next.data = base.data;
            next.data[index] = value;
            // Release publishes constructed immutable values; a subsequent
            // publisher acquires this state while holding the short gate.
            // No scheduler/carrier blocking lock reacquisition after staging.
            next.state.store(State::Ready, std::memory_order_release);
            output.owner_ = this;
            output.slot_ = slot;
            output.generation_ = next.generation;
            return Result::Ok;
        }
        return Result::Capacity;
    }

    Result try_publish(const Lease& basis, Candidate& candidate) noexcept {
        if (basis.owner_ != this || candidate.owner_ != this ||
            basis.slot_ >= Slots || candidate.slot_ >= Slots) return Result::Stale;
        std::unique_lock<std::mutex> lock(mu_, std::try_to_lock);
        if (!lock.owns_lock()) return Result::Busy;
        Node& next = nodes_[candidate.slot_];
        if (next.generation != candidate.generation_ ||
            next.state.load(std::memory_order_acquire) != State::Ready)
            return Result::Stale;
        if (revision_ != basis.revision_ || root_ != basis.slot_)
            return Result::Conflict;
        if (revision_ == std::numeric_limits<std::uint64_t>::max())
            return Result::Exhausted;
        Node& previous = nodes_[root_];
        previous.state = State::Retired;
        if (previous.readers == 0) reclaim(previous);
        next.state = State::Published;
        root_ = candidate.slot_;
        ++revision_;
        candidate.owner_ = nullptr;
        candidate.slot_ = Slots;
        return Result::Ok;
    }

    Result try_discard(Candidate& candidate) noexcept {
        if (candidate.owner_ != this || candidate.slot_ >= Slots)
            return Result::Stale;
        std::unique_lock<std::mutex> lock(mu_, std::try_to_lock);
        if (!lock.owns_lock()) return Result::Busy;
        Node& node = nodes_[candidate.slot_];
        if (node.generation != candidate.generation_ ||
            node.state.load(std::memory_order_acquire) != State::Ready)
            return Result::Stale;
        reclaim(node);
        candidate.owner_ = nullptr;
        candidate.slot_ = Slots;
        return Result::Ok;
    }

    Result try_release(Lease& lease) noexcept {
        if (lease.owner_ != this || lease.slot_ >= Slots) return Result::Stale;
        std::unique_lock<std::mutex> lock(mu_, std::try_to_lock);
        if (!lock.owns_lock()) return Result::Busy;
        Node& node = nodes_[lease.slot_];
        if (node.generation != lease.generation_ || node.readers == 0 ||
            (node.state.load(std::memory_order_relaxed) != State::Published &&
             node.state.load(std::memory_order_relaxed) != State::Retired))
            return Result::Stale;
        --node.readers;
        if (node.state.load(std::memory_order_relaxed) == State::Retired &&
            node.readers == 0) reclaim(node);
        lease.owner_ = nullptr;
        lease.slot_ = Slots;
        return Result::Ok;
    }

    std::uint64_t revision() const noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        return revision_;
    }
    std::size_t retired_pinned() const noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        std::size_t count = 0;
        for (const auto& node : nodes_)
            if (node.state.load(std::memory_order_relaxed) == State::Retired && node.readers != 0) ++count;
        return count;
    }

private:
    static void reclaim(Node& node) noexcept {
        // Declassify old immutable scalar bytes before publishing free state.
        node.data.fill(0);
        node.state = State::Free;
    }
    mutable std::mutex mu_;
    std::array<Node, Slots> nodes_{};
    std::size_t root_ = 0;
    std::uint64_t revision_ = 0;
};

}  // namespace oreslang::runtime
