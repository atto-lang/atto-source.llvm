#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#if defined(ORESLANG_NIO_TEST_HOOKS)
#include <thread>
#endif

namespace oreslang::runtime {

// A bounded, HOST-ONLY ownership ledger for supervisor-issued logical FD
// identities. This is not an FD identity oracle, a guest ABI, or a cross-process
// broker. Only a trusted issuer may bind an identity to an OS descriptor.
// Every hot-path transition uses try_lock and makes no OS calls/allocations.
class NativeFdAuthority final {
public:
    static constexpr std::size_t capacity = 128;
    enum Right : unsigned { readiness = 1u, transfer = 2u };
    enum class Phase : std::uint8_t {
        vacant, owned, watching, terminal, transferring,
        revoking_watch, revoking_terminal, reclaim_pending, revoked
    };
    class Capability final {
        friend class NativeFdAuthority;
        std::uint64_t authority_ = 0;
        std::uint64_t generation_ = 0;
        std::uint16_t slot_ = 0;
        Capability(std::uint64_t authority, std::uint64_t generation,
                   std::uint16_t slot) noexcept
            : authority_(authority), generation_(generation), slot_(slot) {}
    public:
        Capability() noexcept = default;
    };
    class Handoff final {
        friend class NativeFdAuthority;
        std::uint64_t authority_ = 0;
        std::uint64_t generation_ = 0;
        std::uint64_t sequence_ = 0;
        std::uint64_t destination_ = 0;
        std::uint16_t slot_ = 0;
        Handoff(std::uint64_t authority, std::uint64_t generation,
                std::uint64_t sequence, std::uint64_t destination,
                std::uint16_t slot) noexcept
            : authority_(authority), generation_(generation),
              sequence_(sequence), destination_(destination), slot_(slot) {}
    public:
        Handoff() noexcept = default;
    };

    NativeFdAuthority() : authority_(take_namespace()) {}
    NativeFdAuthority(const NativeFdAuthority&) = delete;
    NativeFdAuthority& operator=(const NativeFdAuthority&) = delete;

#if defined(ORESLANG_NIO_TEST_HOOKS)
    // Test-only deterministic mutex contention. Never compiled into normal
    // Oreslang targets and never invoked on a production actor/CONTROL lane.
    void test_hold_gate_until(std::atomic<bool>& acquired,
                              const std::atomic<bool>& release) {
        std::unique_lock<std::mutex> hold(mu_);
        acquired.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire))
            std::this_thread::yield();
    }
    void test_deny_next_consume(unsigned count = 1) noexcept {
        test_consume_denials_.store(count, std::memory_order_release);
    }
#endif

    // A revoked logical slot remains unavailable until confirm_reclaimed().
    // The issuer/broker must have authenticated `owner` and the actual FD
    // identity before minting. A numeric FD supplied by guest code is never
    // sufficient authority to invoke this trusted-host constructor path.
    std::optional<Capability> mint(std::uint64_t owner, unsigned rights) noexcept {
        if (!owner || !(rights & readiness) || (rights & ~(readiness | transfer)))
            return std::nullopt;
        std::unique_lock<std::mutex> g(mu_, std::try_to_lock);
        if (!g.owns_lock()) return std::nullopt;
        for (std::size_t i = 0; i < capacity; ++i) {
            auto& e = slots_[i];
            if (e.phase != Phase::vacant && e.phase != Phase::revoked) continue;
            const auto generation = take_generation();
            if (!generation) return std::nullopt;
            e = Entry{Phase::owned, generation, owner, rights, 0, 0};
            return Capability{authority_, generation,
                              static_cast<std::uint16_t>(i)};
        }
        return std::nullopt;
    }
    bool begin_watch(Capability cap, std::uint64_t caller) noexcept {
        std::unique_lock<std::mutex> g(mu_, std::try_to_lock);
        if (!g.owns_lock()) return false;
        auto* e = owned_entry(cap, caller);
        if (!e || !(e->rights & readiness)) return false;
        e->phase = Phase::watching;
        return true;
    }
    // A host event is terminal only after the epoll layer has finalized its
    // watch ticket. Repeated/foreign reports must not mint extra completions.
    bool publish_terminal(Capability cap, std::uint64_t caller) noexcept {
        std::unique_lock<std::mutex> g(mu_, std::try_to_lock);
        if (!g.owns_lock()) return false;
        auto* e = lookup(cap);
        if (!e || e->owner != caller) return false;
        if (e->phase == Phase::watching) e->phase = Phase::terminal;
        else if (e->phase == Phase::revoking_watch)
            e->phase = Phase::revoking_terminal;
        else return false;
        return true;
    }
    bool consume_terminal(Capability cap, std::uint64_t caller) noexcept {
#if defined(ORESLANG_NIO_TEST_HOOKS)
        auto remaining = test_consume_denials_.load(std::memory_order_relaxed);
        while (remaining && !test_consume_denials_.compare_exchange_weak(
                remaining, remaining - 1, std::memory_order_relaxed))
            ;
        if (remaining) return false; // reproducible transient failure
#endif
        std::unique_lock<std::mutex> g(mu_, std::try_to_lock);
        if (!g.owns_lock()) return false;
        auto* e = lookup(cap);
        if (!e || e->owner != caller) return false;
        if (e->phase == Phase::terminal) e->phase = Phase::owned;
        else if (e->phase == Phase::revoking_terminal)
            e->phase = Phase::reclaim_pending;
        else return false;
        return true;
    }
    // Transfer is a two-phase HOST CONTROL operation. `acknowledge` must be
    // called by a trusted broker after destination identity/FD handoff checks;
    // a raw guest ticket is not an acknowledgment. A watch cannot transfer.
    std::optional<Handoff> begin_transfer(
        Capability cap, std::uint64_t caller, std::uint64_t destination) noexcept {
        if (!destination || caller == destination) return std::nullopt;
        std::unique_lock<std::mutex> g(mu_, std::try_to_lock);
        if (!g.owns_lock()) return std::nullopt;
        auto* e = owned_entry(cap, caller);
        if (!e || !(e->rights & transfer)) return std::nullopt;
        if (next_sequence_ == UINT64_MAX) return std::nullopt;
        e->phase = Phase::transferring;
        e->destination = destination;
        e->sequence = next_sequence_++;
        return Handoff{authority_, cap.generation_, e->sequence,
                       destination, cap.slot_};
    }
    std::optional<Capability> acknowledge(
        Handoff handoff, std::uint64_t trusted_destination) noexcept {
        std::unique_lock<std::mutex> g(mu_, std::try_to_lock);
        if (!g.owns_lock() || !trusted_destination ||
                handoff.authority_ != authority_ || handoff.slot_ >= capacity)
            return std::nullopt;
        auto& e = slots_[handoff.slot_];
        if (e.phase != Phase::transferring ||
                e.generation != handoff.generation_ ||
                e.sequence != handoff.sequence_ ||
                e.destination != trusted_destination ||
                handoff.destination_ != trusted_destination) return std::nullopt;
        // Re-key on transfer: the previous owner's copied host handle is now
        // invalid even if it is presented with forged future owner metadata.
        const auto generation = take_generation();
        if (!generation) return std::nullopt;
        e.generation = generation;
        e.owner = trusted_destination;
        e.destination = 0;
        e.sequence = 0;
        e.phase = Phase::owned;
        return Capability{authority_, generation, handoff.slot_};
    }
    bool revoke(Capability cap, std::uint64_t caller) noexcept {
        std::unique_lock<std::mutex> g(mu_, std::try_to_lock);
        if (!g.owns_lock()) return false;
        auto* e = lookup(cap);
        if (!e || e->owner != caller) return false;
        if (e->phase == Phase::owned || e->phase == Phase::transferring)
            e->phase = Phase::reclaim_pending;
        else if (e->phase == Phase::watching) e->phase = Phase::revoking_watch;
        else if (e->phase == Phase::terminal)
            e->phase = Phase::revoking_terminal;
        else return false;
        // Revoke readiness/transfer rights immediately. Keep identity only
        // for terminal draining and trusted-host reclamation authentication.
        e->rights = 0;
        e->destination = 0;
        e->sequence = 0;
        return true;
    }
    // Trusted host CONTROL-plane cleanup receipt, never a guest/actor call.
    // Revocation removes operating rights immediately but the logical slot
    // remains unusable until the broker has fenced all transport aliases,
    // retired terminal delivery and completed descriptor reclamation. An
    // interrupted handoff or lost destination ACK is NOT cleanup evidence.
    bool confirm_reclaimed(Capability cap, std::uint64_t trusted_owner) noexcept {
        std::unique_lock<std::mutex> g(mu_, std::try_to_lock);
        if (!g.owns_lock()) return false;
        auto* e = lookup(cap);
        if (!e || !trusted_owner || e->owner != trusted_owner ||
                e->phase != Phase::reclaim_pending) return false;
        // The tombstone retains the generation but no former owner authority.
        e->phase = Phase::revoked;
        e->owner = 0;
        e->rights = 0;
        e->sequence = 0;
        e->destination = 0;
        return true;
    }
    std::optional<Phase> phase(Capability cap) noexcept {
        std::unique_lock<std::mutex> g(mu_, std::try_to_lock);
        if (!g.owns_lock()) return std::nullopt;
        auto* e = lookup(cap);
        return e ? std::optional<Phase>(e->phase) : std::nullopt;
    }
private:
    struct Entry {
        Phase phase = Phase::vacant;
        std::uint64_t generation = 0;
        std::uint64_t owner = 0;
        unsigned rights = 0;
        std::uint64_t sequence = 0;
        std::uint64_t destination = 0;
    };
    Entry* lookup(Capability cap) noexcept {
        if (cap.authority_ != authority_ || !cap.generation_ ||
                cap.slot_ >= capacity) return nullptr;
        auto& e = slots_[cap.slot_];
        return e.generation == cap.generation_ &&
               e.phase != Phase::vacant ? &e : nullptr;
    }
    Entry* owned_entry(Capability cap, std::uint64_t caller) noexcept {
        auto* e = lookup(cap);
        return e && e->owner == caller && e->phase == Phase::owned ? e : nullptr;
    }
    std::uint64_t take_generation() noexcept {
        if (!next_generation_ || next_generation_ == UINT64_MAX) return 0;
        return next_generation_++;
    }
    static std::uint64_t take_namespace() {
        auto id = next_namespace_.load(std::memory_order_relaxed);
        while (id != 0 && id != UINT64_MAX) {
            if (next_namespace_.compare_exchange_weak(id, id + 1,
                    std::memory_order_relaxed)) return id;
        }
        throw std::runtime_error("NIO FD authority namespaces exhausted");
    }
    inline static std::atomic<std::uint64_t> next_namespace_{1};
    const std::uint64_t authority_;
    std::mutex mu_{};
    std::array<Entry, capacity> slots_{};
    std::uint64_t next_generation_ = 1;
    std::uint64_t next_sequence_ = 1;
#if defined(ORESLANG_NIO_TEST_HOOKS)
    std::atomic<unsigned> test_consume_denials_{0};
#endif
};
} // namespace oreslang::runtime
