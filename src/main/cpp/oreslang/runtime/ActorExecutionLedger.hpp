#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace oreslang::runtime {

// Actor-owned fixed-capacity logical-turn ledger. This object must live in an
// actor-controlled region and may be mutated ONLY under the existing actor
// scheduler's exclusive execution lease. Cross-thread wake producers publish
// generation-checked *notices* to scheduler CONTROL; the actor consumes them
// after acquiring the lease. The scheduler's ready bit is not execution authority.
// No allocation, mutex, guest callable or heap pointer in event records.
template<std::size_t Capacity>
class ActorExecutionLedger final {
    static_assert(Capacity > 0, "ledger capacity must be positive");
public:
    enum class Kind : std::uint8_t { Message, Timer, Control };
    enum class Phase : std::uint8_t { Empty, Pending, Ready, Running, Suspended };
    enum class Status : std::uint8_t {
        Ok, Full, Stale, Busy, Blocked, Idle, NotReady, AlreadyReady,
        Unauthorized, Closed, Overflow
    };
    struct EventId {
        std::uint64_t actor_id = 0;
        std::uint64_t actor_generation = 0;
        std::uint64_t sequence = 0;
    };
    struct Snapshot {
        std::size_t live = 0;
        std::size_t suspended = 0;
        std::size_t ready = 0;
        std::size_t running = 0;
        bool closed = false;
    };

    explicit ActorExecutionLedger(std::uint64_t actor_id,
                                  std::uint64_t actor_generation) noexcept
        : actor_id_(actor_id), actor_generation_(actor_generation) {}
    ActorExecutionLedger(const ActorExecutionLedger&) = delete;
    ActorExecutionLedger& operator=(const ActorExecutionLedger&) = delete;

    // A mailbox envelope is READY on admission, a timer is PENDING until its
    // scheduler due token arrives, and supervisory CONTROL is READY.
    Status admit(Kind kind, EventId& id) noexcept {
        if (closed_) return Status::Closed;
        if (!actor_id_ || !actor_generation_) return Status::Unauthorized;
        if (next_sequence_ == UINT64_MAX) return Status::Overflow;
        for (auto& slot : slots_) {
            if (slot.phase != Phase::Empty) continue;
            slot.sequence = next_sequence_++;
            slot.kind = kind;
            slot.phase = kind == Kind::Timer ? Phase::Pending : Phase::Ready;
            id = {actor_id_, actor_generation_, slot.sequence};
            ++live_;
            return Status::Ok;
        }
        return Status::Full;
    }

    // Completion only changes eligibility. It NEVER invokes source code.
    // A duplicated/late notice cannot ready a canceled or newer event.
    Status notify_ready(EventId id) noexcept {
        auto* slot = find(id);
        if (!slot) return Status::Stale;
        if (closed_) return Status::Closed;
        if (slot->phase == Phase::Ready) return Status::AlreadyReady;
        if (slot->phase != Phase::Pending && slot->phase != Phase::Suspended)
            return Status::NotReady;
        slot->phase = Phase::Ready;
        return Status::Ok;
    }

    // CONTROL takes precedence, then earliest outstanding logical turn.
    // A suspended older turn blocks later mail/timers by default; a dormant
    // timer (before expiry) doesn't block unrelated mail.
    Status select(EventId& output) const noexcept {
        if (active_sequence_) return Status::Busy;
        const Slot* first_control = nullptr;
        const Slot* first_normal = nullptr;
        for (const auto& slot : slots_) {
            if (slot.phase == Phase::Empty) continue;
            if (slot.kind == Kind::Control && slot.phase == Phase::Ready) {
                if (!first_control || slot.sequence < first_control->sequence)
                    first_control = &slot;
            } else if (slot.kind != Kind::Control &&
                       (slot.kind != Kind::Timer || slot.phase != Phase::Pending)) {
                if (!first_normal || slot.sequence < first_normal->sequence)
                    first_normal = &slot;
            }
        }
        const Slot* pick = first_control ? first_control : first_normal;
        if (!pick) return Status::Idle;
        if (pick->phase != Phase::Ready) return Status::Blocked;
        output = {actor_id_, actor_generation_, pick->sequence};
        return Status::Ok;
    }

    // "authenticated" must come from scheduler's real exclusive actor claim;
    // never accept this boolean from guest source/FFI. Selection is rechecked.
    Status claim(EventId id, bool authenticated) noexcept {
        if (!authenticated) return Status::Unauthorized;
        if (closed_) return Status::Closed;
        auto* slot = find(id);
        if (!slot) return Status::Stale;
        EventId selected{};
        const auto status = select(selected);
        if (status != Status::Ok) return status;
        if (selected.sequence != id.sequence) return Status::Blocked;
        slot->phase = Phase::Running;
        active_sequence_ = id.sequence;
        return Status::Ok;
    }

    // An actor's pending await carries the SAME logical-turn identity.
    // The physical carrier is now free; later normal mail still cannot pass.
    Status suspend(EventId id) noexcept {
        auto* slot = find(id);
        if (!slot || slot->phase != Phase::Running ||
            active_sequence_ != id.sequence) return Status::Stale;
        slot->phase = Phase::Suspended;
        active_sequence_ = 0;
        return Status::Ok;
    }

    // A physical-return receipt: one completed carrier segment is essential
    // before actor arena reclamation. Does not run arbitrary destructors.
    Status finish(EventId id) noexcept {
        auto* slot = find(id);
        if (!slot || slot->phase != Phase::Running ||
            active_sequence_ != id.sequence) return Status::Stale;
        slot->phase = Phase::Empty;
        active_sequence_ = 0;
        --live_;
        return Status::Ok;
    }

    Status cancel(EventId id) noexcept {
        auto* slot = find(id);
        if (!slot) return Status::Stale;
        if (slot->phase == Phase::Running) return Status::Busy;
        slot->phase = Phase::Empty;
        --live_;
        return Status::Ok;
    }

    // Shutdown revokes all non-running work and fences late wake notices.
    // An in-flight guest turn remains counted until finish() is called.
    std::size_t stop() noexcept {
        closed_ = true;
        std::size_t revoked = 0;
        for (auto& slot : slots_) {
            if (slot.phase == Phase::Empty || slot.phase == Phase::Running) continue;
            slot.phase = Phase::Empty;
            --live_;
            ++revoked;
        }
        return revoked;
    }

    bool physically_retired() const noexcept { return closed_ && live_ == 0; }
    Snapshot snapshot() const noexcept {
        Snapshot result{};
        result.live = live_;
        result.closed = closed_;
        for (const auto& slot : slots_) {
            if (slot.phase == Phase::Suspended) ++result.suspended;
            if (slot.phase == Phase::Ready) ++result.ready;
            if (slot.phase == Phase::Running) ++result.running;
        }
        return result;
    }
private:
    struct Slot {
        std::uint64_t sequence = 0;
        Kind kind = Kind::Message;
        Phase phase = Phase::Empty;
    };
    static bool equal(EventId id, std::uint64_t actor_id,
                      std::uint64_t gen) noexcept {
        return id.sequence && id.actor_id == actor_id &&
               id.actor_generation == gen;
    }
    Slot* find(EventId id) noexcept {
        if (!equal(id, actor_id_, actor_generation_)) return nullptr;
        for (auto& slot : slots_)
            if (slot.phase != Phase::Empty && slot.sequence == id.sequence) return &slot;
        return nullptr;
    }
    const std::uint64_t actor_id_;
    const std::uint64_t actor_generation_;
    std::array<Slot, Capacity> slots_{};
    std::uint64_t next_sequence_ = 1;
    std::uint64_t active_sequence_ = 0;
    std::size_t live_ = 0;
    bool closed_ = false;
};
} // namespace oreslang::runtime
