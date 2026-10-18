#pragma once

#include "oreslang/runtime/ActorHeapIdentity.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <tuple>
#include <stdexcept>
#include <utility>

namespace oreslang::runtime {

class HungryCarrier;

/**
 * Actor-generation-wide physical retirement cohort. A trusted supervisor
 * creates exactly one live cohort for an ActorHeapIdentity and fixes the
 * number of physical execution lanes before those lanes are admitted.
 *
 * Each HungryCarrier registers exactly once and acknowledges join exactly once.
 * A proof exists only after every expected lane has registered+joined and no
 * lane reported unclean frame cancellation. The weak global registry rejects
 * a second live cohort for the same actor generation; heaps/proofs retain the
 * cohort so stale generations cannot be silently reused while capabilities live.
 */
class ActorHeapRetirementCohort final {
    friend class HungryCarrier;
    using Key = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>;

    ActorHeapIdentity owner_;
    std::uint64_t cohort_id_;
    std::size_t expected_lanes_;
    std::atomic<std::size_t> registered_lanes_{0};
    std::atomic<std::size_t> joined_lanes_{0};
    std::atomic<bool> cleanup_failed_{false};

    ActorHeapRetirementCohort(ActorHeapIdentity owner, std::uint64_t id,
                              std::size_t expected) noexcept
        : owner_(owner), cohort_id_(id), expected_lanes_(expected) {}

    static std::mutex& registry_mutex();
    static std::map<Key, std::weak_ptr<ActorHeapRetirementCohort>>& registry();
    static std::atomic<std::uint64_t>& next_id();
    static std::uint64_t mint_id();

    bool register_lane() noexcept {
        auto current = registered_lanes_.load(std::memory_order_acquire);
        for (;;) {
            if (current >= expected_lanes_) return false;
            if (registered_lanes_.compare_exchange_weak(
                    current, current + 1,
                    std::memory_order_acq_rel, std::memory_order_acquire))
                return true;
        }
    }

    void acknowledge_lane(bool cleanup_ok) noexcept {
        if (!cleanup_ok)
            cleanup_failed_.store(true, std::memory_order_release);
        const auto before = joined_lanes_.fetch_add(1, std::memory_order_acq_rel);
        const auto registered = registered_lanes_.load(std::memory_order_acquire);
        if (before >= expected_lanes_ || before >= registered)
            cleanup_failed_.store(true, std::memory_order_release);
    }

public:
    static std::shared_ptr<ActorHeapRetirementCohort> create(
        ActorHeapIdentity owner, std::size_t expected_lanes);

    ActorHeapIdentity owner() const noexcept { return owner_; }
    std::uint64_t cohort_id() const noexcept { return cohort_id_; }
    std::size_t expected_lanes() const noexcept { return expected_lanes_; }
    std::size_t registered_lanes() const noexcept {
        return registered_lanes_.load(std::memory_order_acquire);
    }
    std::size_t joined_lanes() const noexcept {
        return joined_lanes_.load(std::memory_order_acquire);
    }
    bool cleanup_failed() const noexcept {
        return cleanup_failed_.load(std::memory_order_acquire);
    }
    bool ready_for_retirement() const noexcept {
        return !cleanup_failed()
            && registered_lanes() == expected_lanes_
            && joined_lanes() == expected_lanes_;
    }
    // Bound heap destruction uses the same full-quorum rule as explicit
    // retirement. Partial startup requires a separate supervisor abort protocol;
    // silently shrinking the cohort here could race a late lane registration.
    bool safe_for_bound_heap_destruction() const noexcept {
        return ready_for_retirement();
    }
};

inline std::mutex& ActorHeapRetirementCohort::registry_mutex() {
    static std::mutex mutex;
    return mutex;
}
inline std::map<ActorHeapRetirementCohort::Key,
                std::weak_ptr<ActorHeapRetirementCohort>>&
ActorHeapRetirementCohort::registry() {
    static std::map<Key, std::weak_ptr<ActorHeapRetirementCohort>> values;
    return values;
}
inline std::atomic<std::uint64_t>& ActorHeapRetirementCohort::next_id() {
    static std::atomic<std::uint64_t> value{1};
    return value;
}
inline std::uint64_t ActorHeapRetirementCohort::mint_id() {
    auto candidate = next_id().load(std::memory_order_relaxed);
    for (;;) {
        if (candidate == 0)
            throw std::overflow_error("actor retirement cohort identity exhausted");
        if (next_id().compare_exchange_weak(candidate, candidate + 1,
                std::memory_order_relaxed, std::memory_order_relaxed))
            return candidate;
    }
}
inline std::shared_ptr<ActorHeapRetirementCohort>
ActorHeapRetirementCohort::create(
        ActorHeapIdentity owner, std::size_t expected_lanes) {
    if (!owner.runtime || !owner.actor || !owner.epoch ||
            expected_lanes == 0 || expected_lanes > 64)
        throw std::invalid_argument("invalid actor retirement cohort");
    const Key key{owner.runtime, owner.actor, owner.epoch};
    std::lock_guard<std::mutex> lock{registry_mutex()};
    auto& values = registry();
    if (const auto found = values.find(key); found != values.end()) {
        if (found->second.lock())
            throw std::logic_error("actor generation already has a live retirement cohort");
        values.erase(found);
    }
    auto cohort = std::shared_ptr<ActorHeapRetirementCohort>(
        new ActorHeapRetirementCohort(owner, mint_id(), expected_lanes));
    values.emplace(key, cohort);
    return cohort;
}

/**
 * Minted only after every lane in one ActorHeapRetirementCohort has joined,
 * closed ingress, drained every outstanding producer call, and either no
 * logical frame was abandoned or its carrier-thread cancellation/drop callback
 * explicitly acknowledged cleanup. Missing/failed frame cleanup blocks proof
 * issuance permanently for that actor retirement cohort.
 *
 * A proof does NOT cover unrelated FFI borrows: those must be tracked with
 * ActorHeapBorrowGate below and drained separately.
 */
class CarrierRetirementProof final {
    friend class HungryCarrier;
    std::shared_ptr<ActorHeapRetirementCohort> cohort_;
    explicit CarrierRetirementProof(
            std::shared_ptr<ActorHeapRetirementCohort> cohort) noexcept
        : cohort_(std::move(cohort)) {}
public:
    ActorHeapIdentity owner() const noexcept { return cohort_->owner(); }
    std::uint64_t cohort_id() const noexcept { return cohort_->cohort_id(); }
    std::size_t lanes() const noexcept { return cohort_->expected_lanes(); }
};

/**
 * Bounded admission guard for actor-heap borrows, async frames and FFI leases.
 * Callers MUST obtain a lease before publishing any native heap reference
 * outside its owning actor turn. Closing the gate bars new borrows; retirement
 * is allowed only after every prior borrow was released.
 *
 * This is an admission protocol, not a memory-protection mechanism against
 * malicious native code. Gate storage must outlive all acquired leases.
 */
class ActorHeapBorrowGate final {
    static constexpr std::uint64_t closing_bit = std::uint64_t{1} << 63;
    static constexpr std::uint64_t mutation_bit = std::uint64_t{1} << 62;
    static constexpr std::uint64_t counter_mask = mutation_bit - 1;
    ActorHeapIdentity owner_;
    std::atomic<std::uint64_t> state_{0};

public:
    explicit ActorHeapBorrowGate(ActorHeapIdentity owner) : owner_(owner) {}
    ActorHeapBorrowGate(const ActorHeapBorrowGate&) = delete;
    ActorHeapBorrowGate& operator=(const ActorHeapBorrowGate&) = delete;
    // A standalone gate may be stack-owned, not only graph-heap-owned.
    // No external Lease/Mutation may outlive the gate's storage; otherwise
    // their RAII release would dereference a destroyed atomic. Terminate
    // rather than silently permit a use-after-free.
    ~ActorHeapBorrowGate() noexcept {
        if (state_.load(std::memory_order_acquire) &
                (mutation_bit | counter_mask)) std::terminate();
    }

    class Lease final {
        friend class ActorHeapBorrowGate;
        ActorHeapBorrowGate* gate_ = nullptr;
        explicit Lease(ActorHeapBorrowGate* gate) noexcept : gate_(gate) {}
    public:
        Lease() noexcept = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept : gate_(std::exchange(other.gate_, nullptr)) {}
        Lease& operator=(Lease&& other) noexcept {
            if (this != &other) {
                reset();
                gate_ = std::exchange(other.gate_, nullptr);
            }
            return *this;
        }
        ~Lease() { reset(); }
        explicit operator bool() const noexcept { return gate_ != nullptr; }
        void reset() noexcept {
            if (auto* gate = std::exchange(gate_, nullptr))
                gate->state_.fetch_sub(1, std::memory_order_release);
        }
    };

    // A mutation token excludes every external loan and prevents new loans
    // for the duration of the operation. Merely observing zero loans before
    // mutating would be racy with a concurrent acquire().
    class Mutation final {
        friend class ActorHeapBorrowGate;
        ActorHeapBorrowGate* gate_ = nullptr;
        explicit Mutation(ActorHeapBorrowGate* gate) noexcept : gate_(gate) {}
    public:
        Mutation() noexcept = default;
        Mutation(const Mutation&) = delete;
        Mutation& operator=(const Mutation&) = delete;
        Mutation(Mutation&& other) noexcept
            : gate_(std::exchange(other.gate_, nullptr)) {}
        Mutation& operator=(Mutation&& other) noexcept {
            if (this != &other) {
                reset();
                gate_ = std::exchange(other.gate_, nullptr);
            }
            return *this;
        }
        ~Mutation() noexcept { reset(); }
        explicit operator bool() const noexcept { return gate_ != nullptr; }
        void reset() noexcept {
            if (auto* gate = std::exchange(gate_, nullptr))
                gate->state_.fetch_and(~mutation_bit, std::memory_order_release);
        }
    };

    std::optional<Mutation> begin_mutation(ActorHeapIdentity actor) {
        if (actor != owner_) throw std::logic_error("foreign heap mutation admission");
        auto current = state_.load(std::memory_order_acquire);
        for (;;) {
            if (current != 0) return std::nullopt;
            if (state_.compare_exchange_weak(current, mutation_bit,
                    std::memory_order_acq_rel, std::memory_order_acquire))
                return Mutation{this};
        }
    }

    std::optional<Lease> acquire(ActorHeapIdentity actor) {
        if (actor != owner_) throw std::logic_error("foreign heap borrow admission");
        auto current = state_.load(std::memory_order_acquire);
        for (;;) {
            if (current & (closing_bit | mutation_bit)) return std::nullopt;
            if ((current & counter_mask) == counter_mask)
                throw std::overflow_error("actor heap borrower count overflow");
            if (state_.compare_exchange_weak(current, current + 1,
                    std::memory_order_acq_rel, std::memory_order_acquire))
                return Lease{this};
        }
    }

    // Seal is monotonic and cannot lose a racing borrower. The caller retries
    // quiescence only after outstanding leases/FFI callbacks have drained.
    bool seal_and_quiesced(ActorHeapIdentity actor) {
        if (actor != owner_) throw std::logic_error("foreign actor heap retirement");
        const auto previous = state_.fetch_or(closing_bit, std::memory_order_acq_rel);
        return (previous & (mutation_bit | counter_mask)) == 0;
    }
    bool quiesced() const noexcept {
        return state_.load(std::memory_order_acquire) == closing_bit;
    }
    bool sealed() const noexcept {
        return (state_.load(std::memory_order_acquire) & closing_bit) != 0;
    }
};

} // namespace oreslang::runtime
