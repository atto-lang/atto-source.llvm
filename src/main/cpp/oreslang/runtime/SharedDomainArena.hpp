#pragma once
// Trusted host substrate; guest never receives native pointers or these C++ tokens.
// SHARED actors keep their own ActorArena; this is an explicitly shared region.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>

namespace oreslang::runtime {
class SharedDomainArena final {
public:
    static constexpr std::size_t Slots = 16;
    static constexpr std::size_t SlotBytes = 64;
    static constexpr std::size_t Members = 16;
    struct Grant { std::uint64_t domain, generation; std::size_t member; };
    struct Handle { std::uint64_t domain, generation; std::size_t slot; };

    class Lease final {
        friend class SharedDomainArena;
        SharedDomainArena* arena_;
        std::shared_lock<std::shared_mutex> read_;
        std::unique_lock<std::shared_mutex> write_;
        bool exclusive_;
        Lease(SharedDomainArena& arena, Grant grant, bool exclusive)
            : arena_(&arena), exclusive_(exclusive) {
            if (exclusive_) write_ = std::unique_lock<std::shared_mutex>(arena.mutex_);
            else read_ = std::shared_lock<std::shared_mutex>(arena.mutex_);
            arena.validate_grant(grant, exclusive_);
        }
        void require_live(bool write = false) const {
            if (!arena_ || arena_->closing_.load(std::memory_order_acquire) ||
                (write && !exclusive_))
                throw std::logic_error("shared region lease revoked or insufficient rights");
        }
        auto& slot(Handle h) const {
            require_live();
            if (h.domain != arena_->identity_ || h.slot >= Slots)
                throw std::logic_error("foreign shared region handle");
            auto& s = arena_->slots_[h.slot];
            if (!s.live || s.generation != h.generation)
                throw std::logic_error("stale shared region handle");
            return s;
        }
    public:
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&&) = default;
        Lease& operator=(Lease&&) = delete;
        Handle allocate(std::size_t bytes) {
            require_live(true);
            if (bytes == 0 || bytes > SlotBytes) throw std::out_of_range("shared region slot bytes");
            for (std::size_t i = 0; i < Slots; ++i) {
                auto& s = arena_->slots_[i];
                if (!s.live && s.generation != 0) {
                    s.live = true;
                    s.length = bytes;
                    s.data.fill(std::byte{0});
                    return {arena_->identity_, s.generation, i};
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
            s.data.fill(std::byte{0});
            s.length = 0;
            s.live = false;
            s.generation = s.generation == std::numeric_limits<std::uint64_t>::max()
                ? 0 : s.generation + 1;
        }
    };

    SharedDomainArena()
        : identity_(next_identity_.fetch_add(1, std::memory_order_relaxed)) {
        if (!identity_ || identity_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("shared region identity exhausted");
    }
    SharedDomainArena(const SharedDomainArena&) = delete;
    SharedDomainArena& operator=(const SharedDomainArena&) = delete;
    ~SharedDomainArena() {
        // The supervisor must establish quiescence before object destruction.
        for (auto& s : slots_) scrub(s.data);
    }

    Grant grant(std::uint64_t actor, bool allow_read, bool allow_write) {
        std::unique_lock lock(mutex_);
        if (closing_.load(std::memory_order_acquire) || !actor || (!allow_read && !allow_write))
            throw std::logic_error("invalid shared region grant");
        for (std::size_t i = 0; i < Members; ++i) {
            auto& m = members_[i];
            if (!m.active && m.generation != 0) {
                m.active = true;
                m.actor = actor;
                m.read = allow_read || allow_write;
                m.write = allow_write;
                return {identity_, m.generation, i};
            }
        }
        throw std::out_of_range("shared region membership quota");
    }
    void revoke(Grant grant) {
        std::unique_lock lock(mutex_);
        validate_grant(grant, false);
        auto& m = members_[grant.member];
        m.active = false;
        m.actor = 0;
        m.read = m.write = false;
        m.generation = m.generation == std::numeric_limits<std::uint64_t>::max()
            ? 0 : m.generation + 1;
    }
    Lease read(Grant grant) { return Lease(*this, grant, false); }
    Lease write(Grant grant) { return Lease(*this, grant, true); }

    // Supervisor CONTROL path only: never block a carrier waiting for loans.
    // Returning false means existing readers/writers have not yet quiesced.
    bool try_retire() {
        closing_.store(true, std::memory_order_release);
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) return false;
        if (retired_) return true;
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
            member.read = member.write = false;
            member.generation = member.generation == std::numeric_limits<std::uint64_t>::max()
                ? 0 : member.generation + 1;
        }
        retired_ = true;
        return true;
    }
private:
    struct Slot { std::array<std::byte, SlotBytes> data{}; std::uint64_t generation = 1; std::size_t length = 0; bool live = false; };
    struct Member { std::uint64_t actor = 0, generation = 1; bool active = false, read = false, write = false; };
    inline static std::atomic<std::uint64_t> next_identity_{1};
    const std::uint64_t identity_;
    std::shared_mutex mutex_;
    std::atomic<bool> closing_{false};
    bool retired_ = false;
    std::array<Slot, Slots> slots_{};
    std::array<Member, Members> members_{};
    template<std::size_t N> static void scrub(std::array<std::byte, N>& data) noexcept {
        volatile unsigned char* p = reinterpret_cast<volatile unsigned char*>(data.data());
        for (std::size_t i = 0; i < N; ++i) p[i] = 0;
    }
    void validate_grant(Grant grant, bool writer) const {
        if (closing_.load(std::memory_order_acquire) ||
            grant.domain != identity_ || grant.member >= Members)
            throw std::logic_error("foreign or retired shared region grant");
        const auto& m = members_[grant.member];
        if (!m.active || m.generation != grant.generation || !m.actor ||
            !m.read || (writer && !m.write))
            throw std::logic_error("stale or insufficient shared region grant");
    }
};
} // namespace oreslang::runtime
