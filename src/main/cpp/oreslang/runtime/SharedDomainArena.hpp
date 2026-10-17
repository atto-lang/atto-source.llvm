#pragma once
// Trusted host substrate; guest never receives native pointers or these C++ tokens.
// SHARED actors keep their own ActorArena; this is an explicitly shared region.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace oreslang::runtime {
class SharedDomainArena final {
public:
    static constexpr std::size_t Slots = 16;
    static constexpr std::size_t SlotBytes = 64;
    static constexpr std::size_t Members = 16;
    static constexpr std::size_t MaxLifetimePins = 64;
    class Lease;
    class Control;
    class LifetimePin;

    // Domain CONTROL is a one-shot opaque capability. Holding a reference to
    // the arena is not enough to mutate membership or retire the domain.
    class Control final {
        friend class SharedDomainArena;
        SharedDomainArena* arena_ = nullptr;
        std::uint64_t domain_ = 0;
        Control(SharedDomainArena& arena, std::uint64_t domain)
            : arena_(&arena), domain_(domain) {}
    public:
        Control(const Control&) = delete;
        Control& operator=(const Control&) = delete;
        Control(Control&&) = delete;
        Control& operator=(Control&&) = delete;
    };

    // Lifetime-only pins model continuation / FFI / I/O owners that can
    // outlive an actor critical section. They carry no slot or byte access and
    // may be released from a completion carrier different from the acquirer.
    class LifetimePin final {
        friend class SharedDomainArena;
        SharedDomainArena* arena_ = nullptr;
        explicit LifetimePin(SharedDomainArena& arena) : arena_(&arena) {}
        void reset() noexcept {
            auto* arena = std::exchange(arena_, nullptr);
            if (arena) arena->release_lifetime_pin();
        }
    public:
        LifetimePin(const LifetimePin&) = delete;
        LifetimePin& operator=(const LifetimePin&) = delete;
        LifetimePin(LifetimePin&& other) noexcept
            : arena_(std::exchange(other.arena_, nullptr)) {}
        LifetimePin& operator=(LifetimePin&& other) noexcept {
            if (this != &other) {
                reset();
                arena_ = std::exchange(other.arena_, nullptr);
            }
            return *this;
        }
        ~LifetimePin() noexcept { reset(); }
        void release() noexcept { reset(); }
        bool live() const noexcept { return arena_ != nullptr; }
    };
    // Grants and handles are capabilities, not serializable coordinates.
    // Raw aggregate fields previously let trusted-but-unprivileged host code
    // fabricate another member's predictable (domain,generation,index) token.
    class Grant final {
        friend class SharedDomainArena;
        friend class Lease;
        std::uint64_t domain_ = 0, generation_ = 0;
        std::size_t member_ = 0;
        Grant(std::uint64_t domain, std::uint64_t generation, std::size_t member)
            : domain_(domain), generation_(generation), member_(member) {}
    public:
        Grant(const Grant&) = default;
        Grant& operator=(const Grant&) = default;
        std::uint64_t domain_id() const noexcept { return domain_; }
    };
    class Handle final {
        friend class SharedDomainArena;
        friend class Lease;
        std::uint64_t domain_ = 0, generation_ = 0;
        std::size_t slot_ = 0;
        Handle(std::uint64_t domain, std::uint64_t generation, std::size_t slot)
            : domain_(domain), generation_(generation), slot_(slot) {}
    public:
        // An invalid zero handle is useful for delayed assignment, but callers
        // cannot manufacture a live handle because the authority fields remain private.
        Handle() = default;
        Handle(const Handle&) = default;
        Handle& operator=(const Handle&) = default;
    };

    class Lease final {
        friend class SharedDomainArena;
        SharedDomainArena* arena_;
        // A std::shared_mutex must be unlocked by the same thread that locked it.
        std::thread::id owner_thread_;
        std::shared_lock<std::shared_mutex> read_;
        std::unique_lock<std::shared_mutex> write_;
        bool exclusive_;
        Lease(SharedDomainArena& arena, std::uint64_t actor, std::uint64_t incarnation, Grant grant, bool exclusive)
            : arena_(&arena), owner_thread_(std::this_thread::get_id()), exclusive_(exclusive) {
            if (!actor || !incarnation) throw std::logic_error("shared region actor incarnation required");
            // Actor carrier threads must not park behind another actor's
            // critical lease. Admission is fail-fast; the scheduler may
            // resubmit a turn after the owner releases its lease.
            if (exclusive_) {
                write_ = std::unique_lock<std::shared_mutex>(arena.mutex_, std::try_to_lock);
                if (!write_.owns_lock()) throw std::logic_error("shared region busy");
            } else {
                read_ = std::shared_lock<std::shared_mutex>(arena.mutex_, std::try_to_lock);
                if (!read_.owns_lock()) throw std::logic_error("shared region busy");
            }
            arena.validate_grant(grant, actor, incarnation, exclusive_);
        }
        void require_live(bool write = false) const {
            if (!arena_ || owner_thread_ != std::this_thread::get_id() ||
                arena_->closing_.load(std::memory_order_acquire) ||
                (write && !exclusive_))
                throw std::logic_error("shared region lease revoked or insufficient rights");
        }
        auto& slot(Handle h) const {
            require_live();
            if (h.domain_ != arena_->identity_ || h.slot_ >= Slots)
                throw std::logic_error("foreign shared region handle");
            auto& s = arena_->slots_[h.slot_];
            if (!s.live || s.generation != h.generation_)
                throw std::logic_error("stale shared region handle");
            return s;
        }
    public:
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&&) = delete; // moved-from lease must never outlive its lock
        Lease& operator=(Lease&&) = delete;
        ~Lease() noexcept {
            // Destructing on a foreign carrier would unlock a shared_mutex
            // from the wrong thread (undefined behavior), so fail closed.
            if (owner_thread_ != std::this_thread::get_id()) std::terminate();
        }
        Handle allocate(std::size_t bytes) {
            require_live(true);
            if (bytes == 0 || bytes > SlotBytes) throw std::out_of_range("shared region slot bytes");
            for (std::size_t i = 0; i < Slots; ++i) {
                auto& s = arena_->slots_[i];
                if (!s.live && s.generation != 0) {
                    s.live = true;
                    s.length = bytes;
                    s.data.fill(std::byte{0});
                    return Handle(arena_->identity_, s.generation, i);
                }
            }
            throw std::out_of_range("shared region slot quota");
        }
        void read(Handle h, void* out, std::size_t length) const {
            auto& s = slot(h);
            if (length != s.length || (length && !out)) throw std::out_of_range("read bounds");
            if (length) std::memcpy(out, s.data.data(), length);
        }
        void write(Handle h, const void* data, std::size_t length) {
            require_live(true);
            auto& s = slot(h);
            if (length != s.length || (length && !data)) throw std::out_of_range("write bounds");
            if (length) std::memcpy(s.data.data(), data, length);
        }
        void drop(Handle h) {
            require_live(true);
            auto& s = slot(h);
            // Security-sensitive drop must not be optimized away as a dead store.
            // The final destructor uses the same volatile scrub primitive.
            SharedDomainArena::scrub(s.data);
            s.length = 0;
            s.live = false;
            s.generation = s.generation == std::numeric_limits<std::uint64_t>::max()
                ? 0 : s.generation + 1;
        }
    };

    SharedDomainArena() : identity_(allocate_identity()) {}
    Control take_control() {
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) throw std::logic_error("shared region busy");
        if (closing_.load(std::memory_order_acquire) || control_issued_)
            throw std::logic_error("shared region control unavailable");
        control_issued_ = true;
        return Control(*this, identity_);
    }
    SharedDomainArena(const SharedDomainArena&) = delete;
    SharedDomainArena& operator=(const SharedDomainArena&) = delete;
    ~SharedDomainArena() noexcept {
        // Destruction requires completed CONTROL retirement and no live
        // actor lease. Never scrub storage beneath a concurrent carrier.
        std::unique_lock<std::shared_mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock() || !retired_ || lifetime_pins_.load(std::memory_order_acquire) != 0) std::terminate();
        for (auto& s : slots_) scrub(s.data);
    }

    Grant grant(const Control& control, std::uint64_t actor, std::uint64_t incarnation, bool allow_read, bool allow_write) {
        // Like actor lease admission, membership grants may not park a CONTROL
        // carrier behind a guest-held critical lease.
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) throw std::logic_error("shared region busy");
        validate_control(control);
        if (closing_.load(std::memory_order_acquire) || !actor || !incarnation || (!allow_read && !allow_write))
            throw std::logic_error("invalid shared region grant");
        for (std::size_t i = 0; i < Members; ++i) {
            auto& m = members_[i];
            if (!m.active && m.generation != 0) {
                m.active = true;
                m.actor = actor;
                m.incarnation = incarnation;
                m.read = allow_read || allow_write;
                m.write = allow_write;
                return Grant(identity_, m.generation, i);
            }
        }
        throw std::out_of_range("shared region membership quota");
    }
    // CONTROL never waits on a guest-held reader/writer lease. The caller may
    // retry after actor lease quiescence; no state is mutated on contention.
    bool try_revoke(const Control& control, Grant grant) {
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) return false;
        validate_control(control);
        validate_grant(grant, 0, 0, false);
        auto& m = members_[grant.member_];
        m.active = false;
        m.actor = 0;
        m.incarnation = 0;
        m.read = m.write = false;
        m.generation = m.generation == std::numeric_limits<std::uint64_t>::max()
            ? 0 : m.generation + 1;
        return true;
    }
    Lease read(std::uint64_t actor, std::uint64_t incarnation, Grant grant) {
        return Lease(*this, actor, incarnation, grant, false);
    }
    Lease write(std::uint64_t actor, std::uint64_t incarnation, Grant grant) {
        return Lease(*this, actor, incarnation, grant, true);
    }

    LifetimePin pin(std::uint64_t actor, std::uint64_t incarnation, Grant grant) {
        std::shared_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) throw std::logic_error("shared region busy");
        validate_grant(grant, actor, incarnation, false);
        auto current = lifetime_pins_.load(std::memory_order_relaxed);
        for (;;) {
            if (current >= MaxLifetimePins)
                throw std::out_of_range("shared region lifetime pin quota");
            if (lifetime_pins_.compare_exchange_weak(
                    current, current + 1, std::memory_order_acq_rel,
                    std::memory_order_relaxed))
                return LifetimePin(*this);
        }
    }

    // Supervisor CONTROL path only: never block a carrier waiting for loans.
    // Returning false means existing readers/writers have not yet quiesced.
    bool try_retire(const Control& control) {
        validate_control(control);
        closing_.store(true, std::memory_order_release);
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) return false;
        if (retired_) return true;
        if (lifetime_pins_.load(std::memory_order_acquire) != 0) return false;
        for (auto& slot : slots_) {
            scrub(slot.data);
            slot.live = false;
            slot.length = 0;
            slot.generation = slot.generation == std::numeric_limits<std::uint64_t>::max()
                ? 0 : slot.generation + 1;
        }
        for (auto& member : members_) {
            member.active = false;
            member.actor = 0;
            member.incarnation = 0;
            member.read = member.write = false;
            member.generation = member.generation == std::numeric_limits<std::uint64_t>::max()
                ? 0 : member.generation + 1;
        }
        retired_ = true;
        return true;
    }
private:
    struct Slot { std::array<std::byte, SlotBytes> data{}; std::uint64_t generation = 1; std::size_t length = 0; bool live = false; };
    struct Member { std::uint64_t actor = 0, incarnation = 0, generation = 1; bool active = false, read = false, write = false; };
    inline static std::atomic<std::uint64_t> next_identity_{1};
    // Saturate rather than wrapping the identity counter: a wrapped value
    // could eventually reauthorize stale cross-domain grants or handles.
    static std::uint64_t allocate_identity() {
        auto current = next_identity_.load(std::memory_order_relaxed);
        for (;;) {
            if (current == 0 || current == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("shared region identity exhausted");
            if (next_identity_.compare_exchange_weak(
                    current, current + 1, std::memory_order_relaxed,
                    std::memory_order_relaxed))
                return current;
        }
    }
    const std::uint64_t identity_;
    std::shared_mutex mutex_;
    std::atomic<bool> closing_{false};
    std::atomic<std::size_t> lifetime_pins_{0};
    bool control_issued_ = false;
    bool retired_ = false;
    std::array<Slot, Slots> slots_{};
    std::array<Member, Members> members_{};
    template<std::size_t N> static void scrub(std::array<std::byte, N>& data) noexcept {
        volatile unsigned char* p = reinterpret_cast<volatile unsigned char*>(data.data());
        for (std::size_t i = 0; i < N; ++i) p[i] = 0;
    }
    void validate_control(const Control& control) const {
        if (!control_issued_ || control.arena_ != this ||
            control.domain_ != identity_)
            throw std::logic_error("foreign or stale shared region control");
    }
    void release_lifetime_pin() noexcept {
        auto current = lifetime_pins_.load(std::memory_order_acquire);
        for (;;) {
            if (current == 0) std::terminate();
            if (lifetime_pins_.compare_exchange_weak(
                    current, current - 1, std::memory_order_acq_rel,
                    std::memory_order_acquire))
                return;
        }
    }
    void validate_grant(Grant grant, std::uint64_t actor, std::uint64_t incarnation, bool writer) const {
        if (closing_.load(std::memory_order_acquire) ||
            grant.domain_ != identity_ || grant.member_ >= Members)
            throw std::logic_error("foreign or retired shared region grant");
        const auto& m = members_[grant.member_];
        if (!m.active || m.generation != grant.generation_ || !m.actor ||
            !m.read || (actor != 0 && (m.actor != actor || m.incarnation != incarnation)) || (writer && !m.write))
            throw std::logic_error("stale or insufficient shared region grant");
    }
};
} // namespace oreslang::runtime
