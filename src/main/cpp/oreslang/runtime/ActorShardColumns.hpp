#pragma once
#include "oreslang/runtime/DopLayout.hpp"
#include "oreslang/runtime/NativeActorScheduler.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace oreslang::runtime {

/**
 * Trusted host admission ledger, reused at the VM scope and at each actor
 * scope. Reserving is atomic: concurrent shard creation cannot exceed either
 * limit. This intentionally owns no actor pointers and grants no data access.
 * Both ledgers must outlive every shard charged to them.
 */
class ActorShardBudget final {
public:
    /** An unbound budget is permitted only for VM-wide accounting. */
    explicit ActorShardBudget(std::uint64_t maximum_bytes)
        : maximum_(maximum_bytes) {
        if (maximum_bytes == 0 || maximum_bytes > kMaxBytes) {
            throw std::invalid_argument("shard budget must fit the 63-bit byte counter");
        }
    }
    /** Bind per-actor budgets at spawn, before dispatch and shard creation. */
    ActorShardBudget(std::uint64_t maximum_bytes,
            OresNativeActorScheduler* scheduler, OresNativeActorRef owner)
        : ActorShardBudget(maximum_bytes) {
        if (!scheduler || !owner.scheduler_domain || !owner.generation) {
            throw std::invalid_argument("actor shard budget requires live actor identity");
        }
        scheduler_ = scheduler;
        owner_ = owner;
    }
    ActorShardBudget(const ActorShardBudget&) = delete;
    ActorShardBudget& operator=(const ActorShardBudget&) = delete;
    ~ActorShardBudget() {
        // A host that destroys a quota ledger before its actor-owned shards
        // would otherwise leave dangling quota references. Fail closed at
        // lifetime teardown instead of silently dropping outstanding bytes.
        if (used_bytes() != 0) std::abort();
    }

    bool matches_owner(OresNativeActorScheduler* scheduler,
            OresNativeActorRef owner) const noexcept {
        return scheduler_ != nullptr && scheduler_ == scheduler
            && owner_.scheduler_domain == owner.scheduler_domain
            && owner_.actor_slot == owner.actor_slot
            && owner_.generation == owner.generation;
    }
    bool vm_wide() const noexcept { return scheduler_ == nullptr; }

    bool retired() const noexcept {
        std::lock_guard<std::mutex> hold(state_lock_);
        return (state_ & kRetiredBit) != 0;
    }

    /**
     * Retirement and reservation use the same ledger mutex. For paired
     * actor/VM admissions, both ledgers are locked together so retirement,
     * capacity checks and both byte increments have one linearization point.
     */
    void mark_retired_after_done() noexcept {
        std::lock_guard<std::mutex> hold(state_lock_);
        state_ |= kRetiredBit;
    }

    bool try_reserve(std::uint64_t bytes) noexcept {
        std::lock_guard<std::mutex> hold(state_lock_);
        if (!can_reserve_locked(bytes)) return false;
        state_ += bytes;
        return true;
    }

    void release(std::uint64_t bytes) noexcept {
        std::lock_guard<std::mutex> hold(state_lock_);
        release_locked(bytes);
    }

    /**
     * Atomically reserve the same byte count against two distinct ledgers.
     * No observer using these ledger APIs can see a committed charge in only
     * one ledger, and actor retirement cannot interleave between the checks.
     */
    static bool try_reserve_pair(
            ActorShardBudget& actor, ActorShardBudget& vm,
            std::uint64_t bytes) noexcept {
        if (&actor == &vm || bytes == 0) return false;
        std::scoped_lock hold(actor.state_lock_, vm.state_lock_);
        if (!actor.can_reserve_locked(bytes) || !vm.can_reserve_locked(bytes)) {
            return false;
        }
        actor.state_ += bytes;
        vm.state_ += bytes;
        return true;
    }

    /** Refund a previously committed pair reservation exactly once. */
    static void release_pair(
            ActorShardBudget& actor, ActorShardBudget& vm,
            std::uint64_t bytes) noexcept {
        if (&actor == &vm || bytes == 0) std::abort();
        std::scoped_lock hold(actor.state_lock_, vm.state_lock_);
        actor.release_locked(bytes);
        vm.release_locked(bytes);
    }

    std::uint64_t used_bytes() const noexcept {
        std::lock_guard<std::mutex> hold(state_lock_);
        return state_ & kMaxBytes;
    }
    std::uint64_t maximum_bytes() const noexcept { return maximum_; }

private:
    bool can_reserve_locked(std::uint64_t bytes) const noexcept {
        if (bytes == 0 || bytes > maximum_ || (state_ & kRetiredBit) != 0) {
            return false;
        }
        const std::uint64_t used = state_ & kMaxBytes;
        return used <= maximum_ - bytes;
    }

    void release_locked(std::uint64_t bytes) noexcept {
        if (!bytes || bytes > maximum_ || (state_ & kMaxBytes) < bytes) {
            std::abort();
        }
        // Unsigned subtraction changes only the low counter because underflow
        // was rejected; the permanent retirement tombstone remains set.
        state_ -= bytes;
    }

    static constexpr std::uint64_t kRetiredBit = UINT64_C(1) << 63;
    static constexpr std::uint64_t kMaxBytes = kRetiredBit - 1;
    std::uint64_t maximum_;
    OresNativeActorScheduler* scheduler_ = nullptr;
    OresNativeActorRef owner_ = {};
    mutable std::mutex state_lock_;
    // High bit: permanent retirement tombstone. Low 63 bits: charged bytes.
    std::uint64_t state_ = 0;
};

/**
 * Trusted host retirement barrier. The scheduler MUST remain alive throughout
 * this call. Stop the actor first; waiting from an actor carrier is rejected by
 * NativeActorScheduler. Host must destroy outstanding shard/snapshot objects
 * before destroying the actor/VM budget ledgers.
 */
inline bool retire_actor_shard_budget_after_done(
        OresNativeActorScheduler* scheduler, OresNativeActorRef owner,
        ActorShardBudget& actor_budget) {
    if (!scheduler || !actor_budget.matches_owner(scheduler, owner))
        return false;
    if (actor_budget.retired()) return true;
    if (!ores_native_actor_scheduler_wait_done(scheduler, owner))
        return false;
    actor_budget.mark_retired_after_done();
    return true;
}

/**
 * Detached but owner-fenced snapshot with explicit, accounted native storage.
 * Reads/copy are allowed only in a live physical turn of the original actor,
 * including if it later migrates pthread carriers. Destruction may happen
 * outside an actor turn, but both budget ledgers must outlive this object.
 */
class ActorColumnSnapshot final {
public:
    ActorColumnSnapshot(OresNativeActorScheduler* scheduler, OresNativeActorRef owner,
            ActorShardBudget& actor_budget, ActorShardBudget& vm_budget, std::size_t length)
        : scheduler_(scheduler), owner_(owner),
          actor_budget_(actor_budget), vm_budget_(vm_budget) {
        if (!scheduler_ || length == 0
                || length > (16U * 1024U * 1024U) / sizeof(double)
                || !actor_budget_.matches_owner(scheduler_, owner_)
                || !vm_budget_.vm_wide()) {
            throw std::invalid_argument("invalid native snapshot owner, size or budget");
        }
        reserved_bytes_ = oreslang::dop::checked_mul(length, sizeof(double));
        if (!ActorShardBudget::try_reserve_pair(
                actor_budget_, vm_budget_, reserved_bytes_)) {
            throw std::length_error("actor/VM snapshot quota exhausted");
        }
        try {
            values_.resize(length);
        } catch (...) {
            ActorShardBudget::release_pair(
                actor_budget_, vm_budget_, reserved_bytes_);
            throw;
        }
    }

    ActorColumnSnapshot(const ActorColumnSnapshot&) = delete;
    ActorColumnSnapshot& operator=(const ActorColumnSnapshot&) = delete;
    ActorColumnSnapshot(ActorColumnSnapshot&&) = delete;
    ActorColumnSnapshot& operator=(ActorColumnSnapshot&&) = delete;

    ~ActorColumnSnapshot() { retire_unchecked(); }

    double read(std::size_t i) const {
        require_reader();
        std::lock_guard<std::mutex> hold(lock_);
        require_open();
        return values_.at(i);
    }
    std::size_t length() const {
        require_reader();
        std::lock_guard<std::mutex> hold(lock_);
        require_open();
        return values_.size();
    }
    void copy_into(double* output, std::size_t available) const {
        require_reader();
        std::lock_guard<std::mutex> hold(lock_);
        require_open();
        if (!output || available < values_.size()) {
            throw std::out_of_range("invalid snapshot destination span");
        }
        std::copy(values_.begin(), values_.end(), output);
    }
    void close() {
        require_close_authority();
        retire_unchecked();
    }
private:
    void retire_unchecked() noexcept {
        std::lock_guard<std::mutex> hold(lock_);
        if (revoked_) return;
        revoked_ = true;
        {
            std::vector<double> released;
            released.swap(values_);
            volatile double* raw = released.data();
            for (std::size_t i = 0; i < released.size(); ++i) raw[i] = 0.0;
            std::atomic_signal_fence(std::memory_order_seq_cst);
        } // vector allocation is actually freed before refunding quota
        ActorShardBudget::release_pair(
            actor_budget_, vm_budget_, reserved_bytes_);
    }
    friend class ActorShardColumns; // Only the creating shard can populate a snapshot.
    void write_from_owner(std::size_t i, double value) {
        require_owner_turn();
        std::lock_guard<std::mutex> hold(lock_);
        require_open();
        values_.at(i) = value;
    }
    void require_owner_turn() const {
        if (actor_budget_.retired() || !scheduler_
                || !ores_native_actor_scheduler_is_current_actor(scheduler_, owner_)) {
            throw std::logic_error("native snapshot retired or foreign owner turn");
        }
    }
    void require_reader() const {
        if (actor_budget_.retired()) {
            throw std::logic_error("native snapshot owner retired");
        }
        require_close_authority();
    }
    void require_close_authority() const {
        if (!scheduler_) throw std::logic_error("native snapshot scheduler unavailable");
        if (ores_native_actor_scheduler_is_current_actor(scheduler_, owner_)) return;
        if (ores_native_actor_scheduler_is_current_carrier(scheduler_)) {
            throw std::logic_error("foreign actor carrier cannot access native snapshot");
        }
        // No scheduler callback is active on this thread: trusted host access.
    }
    void require_open() const {
        if (revoked_) throw std::logic_error("native snapshot revoked");
    }
    OresNativeActorScheduler* scheduler_;
    OresNativeActorRef owner_;
    ActorShardBudget& actor_budget_;
    ActorShardBudget& vm_budget_;
    std::uint64_t reserved_bytes_ = 0;
    std::vector<double> values_;
    mutable std::mutex lock_;
    bool revoked_ = false;
};

/**
 * Trusted native host actor-owned scalar ECS shard (no tracing GC).
 *
 * A single AoS/SoA/AoSoA layout owns one contiguous allocation. Reads and
 * writes require the EXACT actor generation executing on this scheduler.
 * The shard follows actor identity across pthread migration and never exports
 * raw pointers. This does not establish LLVM source-level InvertedArray or
 * permission to share an actor's arena with another Task/actor.
 */
class ActorShardColumns final {
public:
    using Layout = oreslang::dop::Layout;

    ActorShardColumns(OresNativeActorScheduler* scheduler, OresNativeActorRef owner,
            ActorShardBudget& actor_budget, ActorShardBudget& vm_budget,
            Layout layout, std::size_t rows, std::uint32_t fields,
            std::uint32_t lanes = 1)
        : scheduler_(scheduler), owner_(owner),
          actor_budget_(actor_budget), vm_budget_(vm_budget),
          plan_(layout, rows, fields, sizeof(double), alignof(double), lanes) {
        require_owner();
        if (rows == 0 || plan_.byte_length() > 16U * 1024U * 1024U
            || plan_.byte_length() > std::numeric_limits<std::size_t>::max()) {
            throw std::invalid_argument("invalid/oversized actor shard");
        }
        if (&actor_budget_ == &vm_budget_ || !actor_budget_.matches_owner(scheduler_, owner_)
                || !vm_budget_.vm_wide()) {
            throw std::invalid_argument("shard budget owner, generation or VM scope mismatch");
        }
        reserved_bytes_ = plan_.byte_length();
        if (!ActorShardBudget::try_reserve_pair(
                actor_budget_, vm_budget_, reserved_bytes_)) {
            throw std::length_error("actor/VM shard memory budget exceeded");
        }
        try {
            data_.resize(static_cast<std::size_t>(reserved_bytes_) / sizeof(double));
        } catch (...) {
            ActorShardBudget::release_pair(
                actor_budget_, vm_budget_, reserved_bytes_);
            throw;
        }
    }

    ~ActorShardColumns() {
        {
            std::vector<double> released;
            released.swap(data_);
            volatile double* bytes = released.data();
            for (std::size_t i = 0; i < released.size(); ++i) bytes[i] = 0.0;
            std::atomic_signal_fence(std::memory_order_seq_cst);
        } // free native allocation before permitting another actor reservation
        ActorShardBudget::release_pair(
            actor_budget_, vm_budget_, reserved_bytes_);
    }

    ActorShardColumns(const ActorShardColumns&) = delete;
    ActorShardColumns& operator=(const ActorShardColumns&) = delete;
    ActorShardColumns(ActorShardColumns&&) = delete;
    ActorShardColumns& operator=(ActorShardColumns&&) = delete;

    std::size_t rows() const noexcept { return static_cast<std::size_t>(plan_.rows()); }
    std::uint32_t fields() const noexcept { return plan_.fields(); }
    Layout layout() const noexcept { return plan_.layout(); }
    std::uint32_t lanes() const noexcept { return plan_.lanes(); }

    double read(std::size_t row, std::uint32_t field) const {
        require_owner();
        return data_.at(index(row, field));
    }
    void write(std::size_t row, std::uint32_t field, double value) {
        require_owner();
        data_.at(index(row, field)) = value;
    }

    void integrate(std::uint32_t position, std::uint32_t velocity, double dt) {
        require_owner();
        if (position == velocity || position >= fields() || velocity >= fields()
                || !std::isfinite(dt)) {
            throw std::invalid_argument("requires distinct valid fields and finite dt");
        }
        for (std::size_t row = 0; row < rows(); ++row) {
            const auto p = index(row, position);
            data_[p] += data_[index(row, velocity)] * dt;
        }
    }

    /** Copy into previously allocated host memory with no new allocation. */
    void copy_column_into(std::uint32_t field, double* output,
            std::size_t available) const {
        require_owner();
        if (field >= fields() || !output || available < rows()) {
            throw std::out_of_range("invalid destination column span");
        }
        for (std::size_t i = 0; i < rows(); ++i) {
            output[i] = data_[index(i, field)];
        }
    }

    /** Budget-reserved snapshot; caller owns and eventually destroys lease. */
    std::unique_ptr<ActorColumnSnapshot> snapshot_column_leased(
            std::uint32_t field) const {
        require_owner();
        if (field >= fields()) throw std::out_of_range("invalid shard field");
        auto result = std::make_unique<ActorColumnSnapshot>(
            scheduler_, owner_, actor_budget_, vm_budget_, rows());
        for (std::size_t i = 0; i < rows(); ++i) {
            result->write_from_owner(i, data_[index(i, field)]);
        }
        return result;
    }

    /**
     * Legacy unmetered detached copy is intentionally fail-closed. Keeping the
     * signature catches old host callers without permitting an allocation
     * whose lifetime escapes the actor/VM ledgers.
     */
    [[deprecated("use snapshot_column_leased or copy_column_into")]]
    std::vector<double> snapshot_column(std::uint32_t field) const {
        require_owner();
        if (field >= fields()) throw std::out_of_range("invalid shard field");
        throw std::logic_error(
            "unmetered detached snapshots are disabled; use a leased snapshot");
    }

private:
    void require_owner() const {
        if (actor_budget_.retired() || !scheduler_
                || !ores_native_actor_scheduler_is_current_actor(scheduler_, owner_)) {
            throw std::logic_error("actor shard retired or foreign owner turn");
        }
    }
    std::size_t index(std::size_t row, std::uint32_t field) const {
        return static_cast<std::size_t>(plan_.byte_offset(row, field) / sizeof(double));
    }

    OresNativeActorScheduler* scheduler_; // host must drain scheduler before shard teardown
    OresNativeActorRef owner_;
    ActorShardBudget& actor_budget_;
    ActorShardBudget& vm_budget_;
    oreslang::dop::Plan plan_;
    std::uint64_t reserved_bytes_ = 0;
    std::vector<double> data_;
};

} // namespace oreslang::runtime
