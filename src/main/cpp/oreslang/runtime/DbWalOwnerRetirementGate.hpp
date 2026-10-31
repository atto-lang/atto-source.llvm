#pragma once
// Host-only native WAL actor retirement fence. This is not a guest capability,
// file handle, durable receipt, ACK or substitute for AsyncIoActorLifetimePin.
// The caller must authenticate the actor identity and own the physical pin.
#include <cstdint>
#include <mutex>
#include <stdexcept>

namespace oreslang::runtime {

class DbWalOwnerRetirementGate final {
public:
    enum class Phase { open, draining, closing, closed };

    DbWalOwnerRetirementGate(std::uint64_t actor_id, std::uint32_t generation)
        : actor_id_(actor_id), generation_(generation) {
        if (actor_id == 0 || generation == 0)
            throw std::invalid_argument("DB WAL owner identity required");
    }
    DbWalOwnerRetirementGate(const DbWalOwnerRetirementGate&) = delete;
    DbWalOwnerRetirementGate& operator=(const DbWalOwnerRetirementGate&) = delete;

    bool admit(std::uint64_t actor_id, std::uint32_t generation) {
        std::lock_guard<std::mutex> guard(mu_);
        if (!matches(actor_id, generation) || phase_ != Phase::open || active_)
            return false;
        active_ = true;
        return true;
    }

    // A guest's Future completion MUST NOT call this method. It belongs to
    // the physical I/O operation retirement path after native callbacks exit.
    void physical_write_retired(std::uint64_t actor_id, std::uint32_t generation) {
        std::lock_guard<std::mutex> guard(mu_);
        if (!matches(actor_id, generation) || !active_)
            throw std::logic_error("DB WAL stale or double physical retirement");
        active_ = false;
    }

    // Always seals new admission, even while the original worker is live.
    // Subsequent owner retries are admitted once the worker has retired.
    bool request_drain(std::uint64_t actor_id, std::uint32_t generation) {
        std::lock_guard<std::mutex> guard(mu_);
        if (!matches(actor_id, generation)) return false;
        if (phase_ == Phase::open) phase_ = Phase::draining;
        return phase_ == Phase::draining && !active_;
    }

    bool begin_close(std::uint64_t actor_id, std::uint32_t generation) {
        std::lock_guard<std::mutex> guard(mu_);
        if (!matches(actor_id, generation) ||
                phase_ != Phase::draining || active_) return false;
        phase_ = Phase::closing;
        return true;
    }

    // After OS close AND release of the final generation-matched physical pin.
    void physical_close_retired(std::uint64_t actor_id, std::uint32_t generation) {
        std::lock_guard<std::mutex> guard(mu_);
        if (!matches(actor_id, generation) || phase_ != Phase::closing || active_)
            throw std::logic_error("DB WAL close without physical retirement");
        phase_ = Phase::closed;
    }

    bool owns(std::uint64_t actor_id, std::uint32_t generation) const noexcept {
        return matches(actor_id, generation);
    }

    Phase phase() const {
        std::lock_guard<std::mutex> guard(mu_);
        return phase_;
    }
    bool active() const {
        std::lock_guard<std::mutex> guard(mu_);
        return active_;
    }
private:
    bool matches(std::uint64_t actor_id, std::uint32_t generation) const noexcept {
        return actor_id == actor_id_ && generation == generation_;
    }
    const std::uint64_t actor_id_;
    const std::uint32_t generation_;
    mutable std::mutex mu_;
    Phase phase_ = Phase::open;
    bool active_ = false;
};

} // namespace oreslang::runtime
