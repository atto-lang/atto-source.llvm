#pragma once
#include "oreslang/runtime/DopLayout.hpp"
#include "oreslang/runtime/NativeActorScheduler.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace oreslang::runtime {

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
            Layout layout, std::size_t rows, std::uint32_t fields,
            std::uint32_t lanes = 1)
        : scheduler_(scheduler), owner_(owner),
          plan_(layout, rows, fields, sizeof(double), alignof(double), lanes) {
        require_owner();
        if (rows == 0 || plan_.byte_length() > 16U * 1024U * 1024U
            || plan_.byte_length() > std::numeric_limits<std::size_t>::max()) {
            throw std::invalid_argument("invalid/oversized actor shard");
        }
        data_.resize(static_cast<std::size_t>(plan_.byte_length()) / sizeof(double));
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
    oreslang::dop::Plan plan_;
    std::vector<double> data_;
};

} // namespace oreslang::runtime
