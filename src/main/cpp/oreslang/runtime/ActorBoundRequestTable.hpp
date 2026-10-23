#pragma once
// Host-only bridge between runtime-authenticated actor incarnations and
// bounded transport request slots. NOT a guest-callable authority API.
#include "oreslang/runtime/RequestSlotTable.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

namespace oreslang::runtime {
template <std::size_t ActorCapacity, std::size_t Slots, std::size_t MaxBytes>
class ActorBoundRequestTable final {
    static_assert(ActorCapacity > 0, "must reserve at least one actor authority");
public:
    struct ActorIncarnation {
        std::uint64_t id = 0;
        std::uint64_t generation = 0;
    };
    using Table = RequestSlotTable<Slots, MaxBytes>;
    using Handle = typename Table::Handle;
    using Ingress = typename Table::Ingress;
    using ReadLease = typename Table::ReadLease;
private:
    struct Entry {
        ActorIncarnation actor;
        std::uint64_t owner_token = 0;
        bool active = false;
    };
    Table slots_;
    std::array<Entry, ActorCapacity> actors_{};
    std::uint64_t next_owner_token_ = 1; // never reused, including on restart
    mutable std::mutex mutex_;
    static bool equal(ActorIncarnation a, ActorIncarnation b) noexcept {
        return a.id == b.id && a.generation == b.generation;
    }
    std::uint64_t owner_locked(ActorIncarnation actor) const noexcept {
        if (actor.id == 0 || actor.generation == 0) return 0;
        for (const auto& entry : actors_)
            if (entry.active && equal(entry.actor, actor)) return entry.owner_token;
        return 0;
    }
public:
    ActorBoundRequestTable() = default;
    ActorBoundRequestTable(const ActorBoundRequestTable&) = delete;
    ActorBoundRequestTable& operator=(const ActorBoundRequestTable&) = delete;

    // Admission and the identity argument MUST be supplied by trusted runtime
    // CONTROL/execution context, not from message contents or guest integers.
    // A given actor ID cannot have two live incarnations in this host table.
    bool activate(ActorIncarnation actor) noexcept {
        if (actor.id == 0 || actor.generation == 0) return false;
        std::lock_guard<std::mutex> guard(mutex_);
        for (const auto& entry : actors_) {
            if (entry.active && entry.actor.id == actor.id) return false;
        }
        if (next_owner_token_ == 0) return false; // permanent exhaustion
        for (auto& entry : actors_) {
            if (!entry.active) {
                entry.actor = actor;
                entry.owner_token = next_owner_token_++;
                entry.active = true;
                return true;
            }
        }
        return false;
    }
    std::optional<Ingress> begin(ActorIncarnation actor) {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto owner = owner_locked(actor);
        if (!owner) return std::nullopt;
        return slots_.begin(owner);
    }
    std::optional<ReadLease> borrow(ActorIncarnation actor, Handle handle) {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto owner = owner_locked(actor);
        if (!owner) return std::nullopt;
        return slots_.borrow(handle, owner);
    }
    std::optional<Handle> transfer(
            ActorIncarnation from, ActorIncarnation to, Handle handle) {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto source = owner_locked(from);
        const auto destination = owner_locked(to);
        if (!source || !destination) return std::nullopt;
        return slots_.transfer(handle, source, destination);
    }
    bool close(ActorIncarnation actor, Handle handle) {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto owner = owner_locked(actor);
        return owner && slots_.close(handle, owner);
    }
    // Revoke under the same lock that authorizes begin/borrow/transfer/close.
    // Thus no stale token can begin a new request after retirement races.
    // Read loans keep their bytes pinned; aborted ingress releases on drop.
    std::optional<std::size_t> retire(ActorIncarnation actor) {
        std::lock_guard<std::mutex> guard(mutex_);
        for (auto& entry : actors_) {
            if (!entry.active || !equal(entry.actor, actor)) continue;
            const auto count = slots_.revoke_owner(entry.owner_token);
            entry.active = false;
            entry.owner_token = 0;
            return count;
        }
        return std::nullopt;
    }
    template <typename Visitor>
    bool with_borrow(ActorIncarnation actor, Handle handle, Visitor&& visitor) {
        static_assert(std::is_same_v<
                std::invoke_result_t<Visitor&, std::string_view>, void>,
                "borrow scope must not return a borrowed value");
        auto loan = borrow(actor, handle);
        if (!loan) return false;
        // Do not hold the authority mutex over user callback or I/O.
        std::forward<Visitor>(visitor)(loan->view());
        return true; // RAII retires lease on normal return or unwinding
    }
    std::size_t available() const noexcept { return slots_.available(); }
};
} // namespace oreslang::runtime
