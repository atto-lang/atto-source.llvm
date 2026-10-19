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
        if (maximum_bytes == 0) {
            throw std::invalid_argument("empty shard budget");
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

    bool try_reserve(std::uint64_t bytes) noexcept {
        if (bytes == 0 || bytes > maximum_) return false;
        std::uint64_t used = used_.load(std::memory_order_relaxed);
        while (used <= maximum_ - bytes) {
            if (used_.compare_exchange_weak(used, used + bytes,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) return true;
        }
        return false;
    }
    void release(std::uint64_t bytes) noexcept {
        const std::uint64_t prior = used_.fetch_sub(bytes, std::memory_order_acq_rel);
        if (prior < bytes) std::abort(); // never silently hide an accounting bug
    }
    std::uint64_t used_bytes() const noexcept {
        return used_.load(std::memory_order_acquire);
    }
    std::uint64_t maximum_bytes() const noexcept { return maximum_; }
private:
    std::uint64_t maximum_;
    OresNativeActorScheduler* scheduler_ = nullptr;
    OresNativeActorRef owner_ = {};
    std::atomic<std::uint64_t> used_{0};
};

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
        if (!vm_budget_.try_reserve(reserved_bytes_)) {
            throw std::length_error("VM snapshot quota exhausted");
        }
        if (!actor_budget_.try_reserve(reserved_bytes_)) {
            vm_budget_.release(reserved_bytes_);
            throw std::length_error("actor snapshot quota exhausted");
        }
        try {
            values_.resize(length);
        } catch (...) {
            actor_budget_.release(reserved_bytes_);
            vm_budget_.release(reserved_bytes_);
            throw;
        }
    }

    ActorColumnSnapshot(const ActorColumnSnapshot&) = delete;
    ActorColumnSnapshot& operator=(const ActorColumnSnapshot&) = delete;
    ActorColumnSnapshot(ActorColumnSnapshot&&) = delete;
    ActorColumnSnapshot& operator=(ActorColumnSnapshot&&) = delete;

    ~ActorColumnSnapshot() { close(); }

    void write_from_owner(std::size_t i, double value) {
        require_owner();
        std::lock_guard<std::mutex> hold(lock_);
        require_open();
        values_.at(i) = value;
    }
    double read(std::size_t i) const {
        require_owner();
        std::lock_guard<std::mutex> hold(lock_);
        require_open();
        return values_.at(i);
    }
    std::size_t length() const {
        require_owner();
        std::lock_guard<std::mutex> hold(lock_);
        require_open();
        return values_.size();
    }
    void close() noexcept {
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
        actor_budget_.release(reserved_bytes_);
        vm_budget_.release(reserved_bytes_);
    }
private:
    void require_owner() const {
        if (!scheduler_ || !ores_native_actor_scheduler_is_current_actor(scheduler_, owner_)) {
            throw std::logic_error("native snapshot requires its owner actor turn");
        }
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
        if (!vm_budget_.try_reserve(reserved_bytes_)) {
            throw std::length_error("VM shard memory budget exceeded");
        }
        if (!actor_budget_.try_reserve(reserved_bytes_)) {
            vm_budget_.release(reserved_bytes_);
            throw std::length_error("actor shard memory budget exceeded");
        }
        try {
            data_.resize(static_cast<std::size_t>(reserved_bytes_) / sizeof(double));
        } catch (...) {
            actor_budget_.release(reserved_bytes_);
            vm_budget_.release(reserved_bytes_);
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
        actor_budget_.release(reserved_bytes_);
        vm_budget_.release(reserved_bytes_);
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

    /** Detached result copy: this does not expose a mutable actor heap view. */
    std::vector<double> snapshot_column(std::uint32_t field) const {
        require_owner();
        if (field >= fields()) throw std::out_of_range("invalid shard field");
        std::vector<double> result(rows());
        for (std::size_t i = 0; i < rows(); ++i) result[i] = data_[index(i, field)];
        return result;
    }

private:
    void require_owner() const {
        if (!scheduler_ || !ores_native_actor_scheduler_is_current_actor(scheduler_, owner_)) {
            throw std::logic_error("actor shard requires its live owning actor turn");
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
