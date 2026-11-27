#pragma once
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>

namespace oreslang::spawn_reservation {
// Host-only quota and logical receive gate. Physical carrier admission and
// proof of allocator/FFI/task quiescence are separate dispatcher obligations.
class Reservation final {
    struct Identity {};
public:
    struct Charge {
        std::int64_t heap, stacks, mailbox_bytes, messages;
    };
    enum class Phase { initializing, ready, retiring };
    class Permit {
        friend class Reservation;
        std::shared_ptr<const Identity> owner_;
        std::uint64_t generation_;
        Permit(std::shared_ptr<const Identity> owner, std::uint64_t generation)
            : owner_(owner), generation_(generation) {}
    };
    class InitTicket {
        friend class Reservation;
        Permit permit_;
        explicit InitTicket(Permit permit) : permit_(permit) {}
    };
    class ReceiveTicket {
        friend class Reservation;
        Permit permit_;
        std::uint64_t sequence_;
        ReceiveTicket(Permit permit, std::uint64_t sequence) : permit_(permit), sequence_(sequence) {}
    };
    explicit Reservation(Charge limit) : limit_(limit) { validate(limit); }
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;
    Permit reserve(Charge charge, std::int64_t max_live_receives) {
        validate(charge);
        if (max_live_receives <= 0) throw std::invalid_argument("E_SPAWN_POLICY");
        std::lock_guard<std::mutex> lock(mutex_);
        if (charge.heap > limit_.heap - used_.heap || charge.stacks > limit_.stacks - used_.stacks ||
            charge.mailbox_bytes > limit_.mailbox_bytes - used_.mailbox_bytes ||
            charge.messages > limit_.messages - used_.messages)
            throw std::logic_error("E_SPAWN_QUOTA");
        if (generation_ == std::numeric_limits<std::uint64_t>::max())
            throw std::logic_error("E_SPAWN_GENERATION_EXHAUSTED");
        const auto next = generation_ + 1;
        // Allocation may fail before charging; no throwing work after insertion.
        active_.emplace(next, Entry{charge, max_live_receives, Phase::initializing, false, false, 0, {}});
        generation_ = next;
        used_.heap += charge.heap; used_.stacks += charge.stacks;
        used_.mailbox_bytes += charge.mailbox_bytes; used_.messages += charge.messages;
        return Permit(identity_, next);
    }
    InitTicket begin_init(const Permit& p) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& e = require(p);
        if (e.phase != Phase::initializing || e.init_started) throw std::logic_error("E_SPAWN_INIT_AUTHORITY");
        e.init_started = true; e.init_in_flight = true;
        return InitTicket(p);
    }
    bool complete_init(const InitTicket& ticket, bool success) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& e = require(ticket.permit_);
        if (!e.init_in_flight) throw std::logic_error("E_SPAWN_INIT_AUTHORITY");
        e.init_in_flight = false;
        if (!success || e.phase == Phase::retiring) { e.phase = Phase::retiring; return false; }
        e.phase = Phase::ready; return true;
    }
    std::optional<ReceiveTicket> try_enter_receive(const Permit& p) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& e = require(p);
        if (e.phase != Phase::ready || e.receives.size() >= static_cast<std::uint64_t>(e.max_live_receives)) return std::nullopt;
        if (e.sequence == std::numeric_limits<std::uint64_t>::max()) throw std::logic_error("E_SPAWN_LEASE_LIMIT");
        const auto next = e.sequence + 1;
        e.receives.insert(next);
        e.sequence = next;
        return ReceiveTicket(p, next);
    }
    // Copy/replay of a receipt cannot decrement a different live receive.
    bool leave_receive(const ReceiveTicket& ticket) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& e = require(ticket.permit_);
        return e.receives.erase(ticket.sequence_) == 1;
    }
    void retire(const Permit& p) {
        std::lock_guard<std::mutex> lock(mutex_);
        require(p).phase = Phase::retiring;
    }
    // Only call after external physical quiescence has been established.
    void release_after_quiescence(const Permit& p) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto& e = require(p);
        if (e.phase != Phase::retiring) throw std::logic_error("E_SPAWN_NOT_RETIRED");
        if (e.init_in_flight || !e.receives.empty()) throw std::logic_error("E_SPAWN_IN_FLIGHT");
        const auto c = e.charge;
        active_.erase(p.generation_);
        used_.heap -= c.heap; used_.stacks -= c.stacks;
        used_.mailbox_bytes -= c.mailbox_bytes; used_.messages -= c.messages;
    }
    Charge usage() const { std::lock_guard<std::mutex> lock(mutex_); return used_; }
private:
    struct Entry {
        Charge charge;
        std::int64_t max_live_receives;
        Phase phase;
        bool init_started, init_in_flight;
        std::uint64_t sequence;
        std::set<std::uint64_t> receives;
    };
    static void validate(Charge c) {
        if (c.heap <= 0 || c.stacks <= 0 || c.mailbox_bytes <= 0 || c.messages <= 0)
            throw std::invalid_argument("E_SPAWN_QUOTA");
    }
    Entry& require(const Permit& p) {
        if (p.owner_ != identity_) throw std::logic_error("E_SPAWN_STALE_PERMIT");
        auto i = active_.find(p.generation_);
        if (i == active_.end()) throw std::logic_error("E_SPAWN_STALE_PERMIT");
        return i->second;
    }
    const Charge limit_;
    const std::shared_ptr<const Identity> identity_ = std::make_shared<const Identity>();
    mutable std::mutex mutex_;
    Charge used_{0,0,0,0};
    std::uint64_t generation_ = 0;
    std::map<std::uint64_t, Entry> active_;
};
} // namespace oreslang::spawn_reservation
