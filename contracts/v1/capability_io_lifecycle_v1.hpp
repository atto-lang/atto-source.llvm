#pragma once
// HOST-ONLY reference lifecycle gate; not source-visible guest I/O authority.
// Caller must authenticate actor identity and BOTH requested and selected
// permissions in a trusted transport adapter before reserve().
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <vector>
namespace oreslang::capability_io {
class HostLifecycleGate final {
public:
    struct Ticket { std::size_t slot = 0; std::uint64_t nonce = 0; };
    explicit HostLifecycleGate(std::size_t capacity, std::size_t max_revocations = 1024)
        : slots_(bounded_count(capacity)), revoked_(bounded_count(max_revocations)) {}
    Ticket reserve(bool authenticated, bool requested_allowed,
                   bool selected_allowed, std::uint64_t actor,
                   std::uint64_t incarnation, std::uint64_t selected_binding) {
        std::lock_guard<std::mutex> hold(mu_);
        if (closed_ || !authenticated || !requested_allowed || !selected_allowed ||
            actor == 0 || incarnation == 0 || selected_binding == 0 ||
            is_revoked(actor, incarnation)) return {};
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].phase != Phase::free) continue;
            if (next_nonce_ == std::numeric_limits<std::uint64_t>::max()) {
                closed_ = true; return {};
            }
            const auto nonce = ++next_nonce_;
            slots_[i] = Slot{actor, incarnation, selected_binding, nonce, Phase::reserved};
            ++in_flight_;
            return Ticket{i, nonce};
        }
        return {};
    }
    bool dispatch(Ticket t, std::uint64_t actor, std::uint64_t incarnation,
                  std::uint64_t actual_binding) {
        std::lock_guard<std::mutex> hold(mu_);
        Slot* s = owned(t, actor, incarnation);
        if (!s || s->phase != Phase::reserved || actual_binding != s->selected ||
            is_revoked(actor, incarnation) || closed_) return false;
        s->phase = Phase::dispatched;
        return true;
    }
    bool cancel(Ticket t, std::uint64_t actor, std::uint64_t incarnation) {
        std::lock_guard<std::mutex> hold(mu_);
        Slot* s = owned(t, actor, incarnation);
        if (!s || (s->phase != Phase::reserved && s->phase != Phase::dispatched)) return false;
        s->phase = Phase::cancel_requested;
        return true;
    }
    bool complete(Ticket t, std::uint64_t actor, std::uint64_t incarnation) {
        std::lock_guard<std::mutex> hold(mu_);
        Slot* s = owned(t, actor, incarnation);
        if (!s || s->phase == Phase::settled || s->phase == Phase::free) return false;
        s->phase = Phase::settled;
        return true;
    }
    // The trusted adapter, NOT guest code, supplies physical_quiescence after
    // real descriptor/transport retirement; Future settlement is insufficient.
    bool retire(Ticket t, std::uint64_t actor, std::uint64_t incarnation,
                bool physical_quiescence) {
        std::lock_guard<std::mutex> hold(mu_);
        Slot* s = owned(t, actor, incarnation);
        if (!s || !physical_quiescence ||
            (s->phase != Phase::settled && s->phase != Phase::cancel_requested)) return false;
        *s = Slot{};
        --in_flight_;
        return true;
    }
    // Close admission and dispatch authority without pretending that pending
    // transport resources have been physically retired.
    bool shutdown() {
        std::lock_guard<std::mutex> hold(mu_);
        if (closed_) return false;
        close_all_locked();
        return true;
    }
    bool revoke_actor(std::uint64_t actor, std::uint64_t incarnation) {
        std::lock_guard<std::mutex> hold(mu_);
        if (!actor || !incarnation || is_revoked(actor, incarnation)) return false;
        if (revocation_count_ >= revoked_.size()) {
            close_all_locked(); // Fail closed without abandoning physical credits.
        } else {
            revoked_[revocation_count_++] = {actor, incarnation};
        }
        for (Slot& s : slots_) {
            if (s.actor == actor && s.incarnation == incarnation &&
                (s.phase == Phase::reserved || s.phase == Phase::dispatched))
                s.phase = Phase::cancel_requested;
        }
        return true;
    }
    std::size_t in_flight() const {
        std::lock_guard<std::mutex> hold(mu_);
        return in_flight_;
    }
private:
    void close_all_locked() {
        closed_ = true;
        for (Slot& s : slots_) {
            if (s.phase == Phase::reserved || s.phase == Phase::dispatched)
                s.phase = Phase::cancel_requested;
        }
    }
    // Reject hostile oversized requests BEFORE allocating either vector.
    static std::size_t bounded_count(std::size_t n) {
        if (n == 0 || n > 65536)
            throw std::invalid_argument("bounded lifecycle capacity required");
        return n;
    }
    enum class Phase : std::uint8_t { free, reserved, dispatched, cancel_requested, settled };
    struct Slot {
        std::uint64_t actor = 0, incarnation = 0, selected = 0, nonce = 0;
        Phase phase = Phase::free;
    };
    struct Revoked { std::uint64_t actor = 0, incarnation = 0; };
    Slot* owned(Ticket t, std::uint64_t actor, std::uint64_t incarnation) {
        if (t.nonce == 0 || t.slot >= slots_.size()) return nullptr;
        Slot& s = slots_[t.slot];
        return s.nonce == t.nonce && s.actor == actor &&
            s.incarnation == incarnation ? &s : nullptr;
    }
    bool is_revoked(std::uint64_t actor, std::uint64_t incarnation) const {
        for (std::size_t i = 0; i < revocation_count_; ++i)
            if (revoked_[i].actor == actor && revoked_[i].incarnation == incarnation)
                return true;
        return false;
    }
    mutable std::mutex mu_;
    std::vector<Slot> slots_;
    std::vector<Revoked> revoked_;
    std::size_t in_flight_ = 0, revocation_count_ = 0;
    std::uint64_t next_nonce_ = 0;
    bool closed_ = false;
};
} // namespace oreslang::capability_io
